// App-side platform code: settings, the sampler's status, battery tracking and diagnostics.
// The scoring itself is in score.c, which the worker shares.
#include "platform.h"
#include "demo_data.h"
#include "model.h"
#include "score.h"

#define MIN_FW_MAJOR 4
#define MIN_FW_MINOR 32

static AppWorkerResult s_launch = APP_WORKER_RESULT_NOT_RUNNING;
static bool s_launch_tried;
static BattTrack s_batt;

uint32_t plat_now_min(void) {
  return score_now_min();
}

// ---- Storage -------------------------------------------------------------------------------
static void batt_update(void) {
  const BatteryChargeState b = battery_state_service_peek();
  const uint32_t now = (uint32_t)time(NULL);
  if (persist_read_data(PK_BATT, &s_batt, sizeof(s_batt)) != (int)sizeof(s_batt)) {
    memset(&s_batt, 0, sizeof(s_batt));
  }
  const bool restart = !s_batt.t_utc || b.is_plugged || b.charge_percent > s_batt.pct ||
                       now < s_batt.t_utc;
  if (restart) {
    s_batt.t_utc = now;
    s_batt.pct = b.charge_percent;
  } else if (now - s_batt.t_utc >= 6 * SECONDS_PER_HOUR) {
    // Charge only moves in coarse steps, so measure over six hours or more.
    const uint32_t span = now - s_batt.t_utc;
    s_batt.rate10 = (uint16_t)((uint32_t)(s_batt.pct - b.charge_percent) * 10u * SECONDS_PER_DAY / span);
    s_batt.span_h = (uint16_t)(span / SECONDS_PER_HOUR);
    s_batt.rate10_valid = 1;
    s_batt.t_utc = now;
    s_batt.pct = b.charge_percent;
  }
  persist_write_data(PK_BATT, &s_batt, sizeof(s_batt));
}

const BattTrack *plat_batt(void) {
  return &s_batt;
}

void plat_load(Engine *e, Settings *set) {
  if (persist_read_data(PK_SETTINGS, set, sizeof(*set)) != (int)sizeof(*set) ||
      set->version != SETTINGS_VERSION) {
    *set = (Settings) {
      .version = SETTINGS_VERSION,
      .intensity = INTENSITY_BALANCED,
      .hrv_units = HRV_UNITS_MS,
      .max_hr = M_DEFAULT_MAX_HR,
    };
    plat_save_settings(set);
  }
  score_load(e);
  batt_update();
}

void plat_save_settings(const Settings *set) {
  persist_write_data(PK_SETTINGS, set, sizeof(*set));
}

// ---- Sampler -------------------------------------------------------------------------------
static bool firmware_has_hrv(void) {
#ifdef ER_DEMO
  return true;
#else
  const WatchInfoVersion v = watch_info_get_firmware_version();
  return v.major > MIN_FW_MAJOR || (v.major == MIN_FW_MAJOR && v.minor >= MIN_FW_MINOR);
#endif
}

void plat_sampler_start(void) {
  if (!firmware_has_hrv()) return;
  s_launch = app_worker_launch();
  s_launch_tried = true;
  APP_LOG(APP_LOG_LEVEL_INFO, "sampler launch result %d, running %d", (int)s_launch,
          (int)app_worker_is_running());
}

bool plat_worker_hdr(WorkerHdr *out) {
  return persist_read_data(PK_WHDR, out, sizeof(*out)) == (int)sizeof(*out) &&
         out->version == WHDR_VERSION;
}

SamplerState plat_sampler_state(void) {
#ifdef ER_DEMO
  return SAMPLER_OK;
#else
  if (!firmware_has_hrv()) return SAMPLER_OLD_FIRMWARE;
  if (!app_worker_is_running()) {
    // Only one background worker runs at a time. If another app's worker holds the slot the
    // watch asks the owner before replacing it, and this app cannot stop it.
    const bool pending = !s_launch_tried || s_launch == APP_WORKER_RESULT_SUCCESS ||
                         s_launch == APP_WORKER_RESULT_ASKING_CONFIRMATION;
    return pending ? SAMPLER_STARTING : SAMPLER_NOT_RUNNING;
  }
  WorkerHdr h;
  if (plat_worker_hdr(&h) && (h.status & WS_HRV_REFUSED)) return SAMPLER_HRV_REFUSED;
  return SAMPLER_OK;
#endif
}

// Valid HRV windows in the last 24 hours, counted from the worker's ring.
uint16_t plat_windows_24h(void) {
  uint16_t count = 0;
#ifndef ER_DEMO
  const uint32_t now = plat_now_min();
  HrvWin *page = malloc(sizeof(HrvWin) * RING_PER_KEY);
  if (!page) return 0;
  for (int k = 0; k < RING_KEYS; k++) {
    const int bytes = persist_read_data(PK_RING0 + k, page, sizeof(HrvWin) * RING_PER_KEY);
    for (int i = 0; i < bytes / (int)sizeof(HrvWin); i++) {
      if (page[i].rmssd10 && (page[i].t & WIN_T_MASK) + 1440 >= now) count++;
    }
  }
  free(page);
#endif
  return count;
}

// ---- Diagnostics ---------------------------------------------------------------------------
// One-off report of what the health service will give this app. Log only.
#ifndef ER_DEMO
static void probe_history(const char *label, time_t from, time_t to) {
  HealthMinuteData *buf = malloc(sizeof(HealthMinuteData) * 60);
  if (!buf) return;
  from -= from % 60;   // whole minutes, as the replay asks for
  to -= to % 60;
  time_t ts = from, te = to;
  const uint32_t n = health_service_get_minute_history(buf, 60, &ts, &te);
  int valid = 0, with_hr = 0, steps = 0, last_hr = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (!buf[i].is_invalid) valid++;
    if (buf[i].heart_rate_bpm) { with_hr++; last_hr = buf[i].heart_rate_bpm; }
    steps += buf[i].steps;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "probe %s: n %d at %+d s, valid %d, hr %d (last %d), steps %d",
          label, (int)n, (int)(ts - from), valid, with_hr, last_hr, steps);
  free(buf);
}
#endif

void plat_log_diagnostics(void) {
#ifndef ER_DEMO
  const time_t now = time(NULL);
  APP_LOG(APP_LOG_LEVEL_INFO, "access: steps today %d, HR 1h %d, sleep today %d (1=available)",
          (int)health_service_metric_accessible(HealthMetricStepCount, time_start_of_today(), now),
          (int)health_service_metric_accessible(HealthMetricHeartRateBPM, now - 3600, now),
          (int)health_service_metric_accessible(HealthMetricSleepSeconds, time_start_of_today(), now));
  APP_LOG(APP_LOG_LEVEL_INFO, "now: HR %d (raw %d), steps today %d, sleep today %d s, activities %d",
          (int)health_service_peek_current_value(HealthMetricHeartRateBPM),
          (int)health_service_peek_current_value(HealthMetricHeartRateRawBPM),
          (int)health_service_sum_today(HealthMetricStepCount),
          (int)health_service_sum_today(HealthMetricSleepSeconds),
          (int)health_service_peek_current_activities());
  probe_history("last 20 min", now - 20 * 60, now);
  probe_history("20-80 min ago", now - 80 * 60, now - 20 * 60);
  probe_history("3-4 h ago", now - 4 * 3600, now - 3 * 3600);
  probe_history("8-9 h ago", now - 9 * 3600, now - 8 * 3600);
  probe_history("24-25 h ago", now - 25 * 3600, now - 24 * 3600);
#endif
}

// ---- Debug dump ----------------------------------------------------------------------------
// ER_DEBUG only: writes what the score was built from to the log, a few lines per call so
// the log link keeps up. Reads only. Call with step 0, 1, 2... until it returns false.
#if defined(ER_DEBUG) && !defined(ER_DEMO)
#define DUMP_HOURS 34

static int hhmm(uint32_t t_min) {
  const time_t t = (time_t)t_min * 60;
  const struct tm *lt = localtime(&t);
  return lt->tm_mday * 10000 + lt->tm_hour * 100 + lt->tm_min;
}

static void dump_hour(uint32_t from_min) {
  HealthMinuteData *buf = malloc(sizeof(HealthMinuteData) * 60);
  if (!buf) return;
  time_t ts = (time_t)from_min * 60, te = ts + 3600;
  const uint32_t n = health_service_get_minute_history(buf, 60, &ts, &te);
  const uint32_t first = (uint32_t)(ts / 60);
  int valid = 0, sl = 0, rf = 0, steps = 0, hrn = 0, hrsum = 0, hrmin = 255, hrmax = 0, vmc = 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t t = first + i;
    if (t < from_min || t >= from_min + 60) continue;
    if (buf[i].is_invalid) continue;
    valid++;
    const int f = score_debug_flags(t);
    if (f & MF_RESTFUL) rf++;
    if (f & MF_ASLEEP) sl++;
    steps += buf[i].steps;
    vmc += buf[i].vmc;
    const int hr = buf[i].heart_rate_bpm;
    if (hr) {
      hrn++;
      hrsum += hr;
      if (hr < hrmin) hrmin = hr;
      if (hr > hrmax) hrmax = hr;
    }
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "H %06d v%d sl%d rf%d st%d vm%d hr n%d a%d lo%d hi%d", hhmm(from_min),
          valid, sl, rf, steps, vmc / 60, hrn, hrn ? hrsum / hrn : 0, hrn ? hrmin : 0, hrmax);
  free(buf);
}

// Half an hour of the per-minute record, six minutes to a line, each minute as hex:
// steps (2), heart rate (2), motion counts capped at 4095 (3), flags (1: valid, asleep, restful).
#define DUMP_MIN_HOURS 46
static void dump_minutes(uint32_t from_min) {
  HealthMinuteData *buf = malloc(sizeof(HealthMinuteData) * 30);
  if (!buf) return;
  time_t ts = (time_t)from_min * 60, te = ts + 30 * 60;
  const uint32_t n = health_service_get_minute_history(buf, 30, &ts, &te);
  const uint32_t first = (uint32_t)(ts / 60);
  char line[64];
  for (int row = 0; row < 5; row++) {
    int len = 0;
    for (int i = 0; i < 6; i++) {
      const uint32_t t = from_min + (uint32_t)(row * 6 + i);
      int steps = 0, hr = 0, vmc = 0, flags = 0;
      if (t >= first && t - first < n && !buf[t - first].is_invalid) {
        const HealthMinuteData *d = &buf[t - first];
        steps = d->steps;
        hr = d->heart_rate_bpm;
        vmc = d->vmc > 4095 ? 4095 : d->vmc;
        flags = MF_VALID | score_debug_flags(t);
      }
      len += snprintf(line + len, sizeof(line) - (size_t)len, "%02x%02x%03x%x", steps, hr, vmc, flags);
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "M %06d %d %s", hhmm(from_min + (uint32_t)(row * 6)),
            (int)(from_min + (uint32_t)(row * 6)), line);
  }
  free(buf);
}

bool plat_debug_dump(const Engine *e, int step) {
  static HrvWin *wins;
  static uint16_t win_n;
  const uint32_t now = plat_now_min();
  const EngineState *s = &e->st;
  char line[96];

  if (step == 0) {
    APP_LOG(APP_LOG_LEVEL_INFO, "D st r%d r0 %d fl%d slp%d ss%06d wk%06d last%06d hr%d", (int)s->reserve100,
            (int)s->r0_100, (int)s->flags, (int)s->sleep_min, hhmm(s->sleep_start_min),
            hhmm(s->wake_min), hhmm(s->last_min), (int)s->last_hr);
    APP_LOG(APP_LOG_LEVEL_INFO, "D tot a%d s%d r%d sl%d aw%d", (int)s->tot[0], (int)s->tot[1],
            (int)s->tot[2], (int)s->tot[3], (int)s->tot[4]);
    APP_LOG(APP_LOG_LEVEL_INFO, "D wtot a%d s%d r%d sl%d aw%d", (int)s->wake_tot[0],
            (int)s->wake_tot[1], (int)s->wake_tot[2], (int)s->wake_tot[3], (int)s->wake_tot[4]);
    APP_LOG(APP_LOG_LEVEL_INFO, "D base rmssd%d rhr%d n%d nights%d", (int)e->base_rmssd10,
            (int)e->base_rhr, (int)e->base_nights, (int)e->night_count);
    for (int i = 0; i < e->night_count; i++) {
      const Night *n = &e->nights[i];
      APP_LOG(APP_LOG_LEVEL_INFO, "D night%d day%d rmssd%d sleep%d rhr%d wins%d", i, (int)n->day,
              (int)n->rmssd10, (int)n->sleep_min, (int)n->rhr, (int)n->wins);
    }
    WorkerHdr h;
    if (plat_worker_hdr(&h)) {
      APP_LOG(APP_LOG_LEVEL_INFO, "D whdr st%d cnt%d b%d ok%d sens%d yd%d lp%d la%d", (int)h.status,
              (int)h.count, (int)h.bursts_today, (int)h.ok_today, (int)h.sensor_s_today,
              (int)h.sensor_s_yday, (int)h.last_pairs, (int)h.last_artifacts);
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "D batt pct%d rate%d valid%d span%d", (int)s_batt.pct,
            (int)s_batt.rate10, (int)s_batt.rate10_valid, (int)s_batt.span_h);
    return true;
  }

  if (step == 1) {   // sleep sessions as Pebble Health reports them
    const uint8_t n = score_debug_spans(now - DUMP_MIN_HOURS * 60, now);
    int rest_n = 0, rest_min = 0;
    for (int i = n - 1; i >= 0; i--) {   // stored newest first
      uint32_t a, b;
      bool restful;
      if (!score_debug_span((uint8_t)i, &a, &b, &restful)) continue;
      if (restful) {
        rest_n++;
        rest_min += (int)(b - a);
      } else {
        APP_LOG(APP_LOG_LEVEL_INFO, "D sleep %06d-%06d", hhmm(a), hhmm(b));
      }
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "D restful spans%d min%d", rest_n, rest_min);
    return true;
  }

  if (step == 2) {   // the score curve, oldest first
    const uint32_t slot_now = now / CURVE_SLOT_MIN;
    for (int row = 0; row < 4; row++) {
      int len = 0;
      for (int i = 0; i < 24; i++) {
        const uint32_t slot = slot_now - (CURVE_SLOTS - 1) + (uint32_t)(row * 24 + i);
        len += snprintf(line + len, sizeof(line) - (size_t)len, "%d ", (int)e->curve[slot % CURVE_SLOTS]);
      }
      APP_LOG(APP_LOG_LEVEL_INFO, "C %06d %s",
              hhmm((slot_now - (CURVE_SLOTS - 1) + (uint32_t)(row * 24)) * CURVE_SLOT_MIN), line);
    }
    return true;
  }

  if (step == 3) {   // HRV windows
    wins = malloc(sizeof(HrvWin) * RING_CAP);
    win_n = 0;
    if (!wins) return true;
    HrvWin page[RING_PER_KEY];
    for (int k = 0; k < RING_KEYS; k++) {
      const int bytes = persist_read_data(PK_RING0 + k, page, sizeof(page));
      for (int i = 0; i < bytes / (int)sizeof(HrvWin); i++) {
        if (page[i].rmssd10 && win_n < RING_CAP) wins[win_n++] = page[i];
      }
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "D wins %d", (int)win_n);
    return true;
  }

  const int win_steps = wins ? (win_n + 5) / 6 : 0;
  if (step < 4 + win_steps) {
    int len = 0;
    for (int i = (step - 4) * 6; i < (step - 3) * 6 && i < win_n; i++) {
      len += snprintf(line + len, sizeof(line) - (size_t)len, "%06d%c%d ",
                      hhmm(wins[i].t & WIN_T_MASK), (wins[i].t & WIN_ASLEEP) ? 's' : 'a',
                      (int)wins[i].rmssd10);
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "W %s", line);
    return true;
  }
  if (wins) { free(wins); wins = NULL; }

  const int hour = step - 4 - win_steps;
  if (hour < DUMP_HOURS) {
    const uint32_t base = now - now % 60 - (uint32_t)(DUMP_HOURS - 1 - hour) * 60;
    dump_hour(base - 60);
    return true;
  }
  const int half = hour - DUMP_HOURS;
  if (half < DUMP_MIN_HOURS * 2) {
    const uint32_t base = now - now % 30 - (uint32_t)(DUMP_MIN_HOURS * 2 - half) * 30;
    dump_minutes(base);
    return true;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "D end");
  return false;
}
#endif
