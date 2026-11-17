#include "platform.h"
#include "demo_data.h"
#include "model.h"

#define CHUNK_MIN 60
#define MIN_FW_MAJOR 4
#define MIN_FW_MINOR 32

static HrvWin *s_wins;
static uint16_t s_win_n, s_win_24h;
static HealthMinuteData *s_buf;
static AppWorkerResult s_launch = APP_WORKER_RESULT_NOT_RUNNING;
static uint16_t s_stat_min, s_stat_rec, s_stat_hr, s_stat_asleep;   // last replay, for the log
static uint32_t s_replay_from;
static bool s_launch_tried;
static BattTrack s_batt;

uint32_t plat_now_min(void) {
  return (uint32_t)(time(NULL) / 60);
}

static int16_t tz_offset_min(void) {
  const time_t now = time(NULL);
  const struct tm *lt = localtime(&now);
  int d = lt->tm_hour * 60 + lt->tm_min - (int)((now / 60) % 1440);
  if (d > 840) d -= 1440;
  if (d < -720) d += 1440;
  return (int16_t)d;
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
  memset(e, 0, sizeof(*e));

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
  e->max_hr = set->max_hr;
  e->tz_min = tz_offset_min();

  const int nights_bytes = persist_read_data(PK_NIGHTS, e->nights, sizeof(e->nights));
  e->night_count = nights_bytes > 0 ? (uint8_t)(nights_bytes / (int)sizeof(Night)) : 0;
  engine_rebase(e);

  const bool have_state =
      persist_read_data(PK_ENGINE, &e->st, sizeof(e->st)) == (int)sizeof(e->st) &&
      e->st.version == ENGINE_VERSION &&
      persist_read_data(PK_CURVE, e->curve, sizeof(e->curve)) == (int)sizeof(e->curve);
  if (!have_state) {
#ifdef ER_DEMO
    engine_reset(e, plat_now_min() - 5 * 1440);   // five synthetic nights: a full baseline
#else
    engine_reset(e, plat_now_min() - M_FIRST_LOOKBACK_MIN);
#endif
  }
  batt_update();
}

void plat_save_nights(const Engine *e) {
  if (e->night_count) {
    persist_write_data(PK_NIGHTS, e->nights, e->night_count * sizeof(Night));
  } else {
    persist_delete(PK_NIGHTS);
  }
}

void plat_save(Engine *e) {
  persist_write_data(PK_ENGINE, &e->st, sizeof(e->st));
  persist_write_data(PK_CURVE, e->curve, sizeof(e->curve));
  plat_save_nights(e);

  const int score = engine_score(e);
  const ScoreRecord rec = {
    .version = SCORE_VERSION,
    .score = (uint8_t)score,
    .band = (uint8_t)engine_band(score),
    .flags = (uint8_t)(((e->st.flags & EF_STALE) ? SF_STALE : 0) |
                       ((e->st.flags & EF_LOW_CONF) ? SF_LOW_CONF : 0) |
                       (engine_calibrating(e) ? SF_CALIBRATING : 0)),
    .updated_utc = e->st.last_min * 60,
  };
  persist_write_data(PK_SCORE, &rec, sizeof(rec));
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

uint16_t plat_windows_24h(void) {
  return s_win_24h;
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

// ---- Replay --------------------------------------------------------------------------------
#ifndef ER_DEMO
typedef struct { uint32_t a, b; } Span;   // minutes [a, b)
#define MAX_SLEEP_SPANS 16
#define MAX_REST_SPANS  48

static Span s_sleep[MAX_SLEEP_SPANS], s_rest[MAX_REST_SPANS];
static uint8_t s_sleep_n, s_rest_n;

static bool activity_cb(HealthActivity activity, time_t start, time_t end, void *context);
static bool in_spans(const Span *spans, uint8_t n, uint32_t t);
#endif

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
    const bool rest = in_spans(s_rest, s_rest_n, t);
    if (rest) rf++;
    if (rest || in_spans(s_sleep, s_sleep_n, t)) sl++;
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
        flags = MF_VALID;
        const bool rest = in_spans(s_rest, s_rest_n, t);
        if (rest) flags |= MF_RESTFUL;
        if (rest || in_spans(s_sleep, s_sleep_n, t)) flags |= MF_ASLEEP;
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
    s_sleep_n = s_rest_n = 0;
    health_service_activities_iterate(HealthActivitySleep | HealthActivityRestfulSleep,
                                      (time_t)(now - DUMP_HOURS * 60) * 60, (time_t)now * 60,
                                      HealthIterationDirectionFuture, activity_cb, NULL);
    for (int i = 0; i < s_sleep_n; i++) {
      APP_LOG(APP_LOG_LEVEL_INFO, "D sleep %06d-%06d", hhmm(s_sleep[i].a), hhmm(s_sleep[i].b));
    }
    int rest_min = 0;
    for (int i = 0; i < s_rest_n; i++) rest_min += (int)(s_rest[i].b - s_rest[i].a);
    APP_LOG(APP_LOG_LEVEL_INFO, "D restful spans%d min%d", (int)s_rest_n, rest_min);
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

#ifndef ER_DEMO
static bool activity_cb(HealthActivity activity, time_t start, time_t end, void *context) {
  const Span span = { (uint32_t)(start / 60), (uint32_t)((end + 59) / 60) };
  if (activity == HealthActivitySleep) {
    if (s_sleep_n < MAX_SLEEP_SPANS) s_sleep[s_sleep_n++] = span;
  } else if (activity == HealthActivityRestfulSleep) {
    if (s_rest_n < MAX_REST_SPANS) s_rest[s_rest_n++] = span;
  }
  return true;
}

static bool in_spans(const Span *spans, uint8_t n, uint32_t t) {
  for (uint8_t i = 0; i < n; i++) {
    if (t >= spans[i].a && t < spans[i].b) return true;
  }
  return false;
}

static void load_ring(void) {
  s_wins = malloc(sizeof(HrvWin) * RING_CAP);
  s_win_n = 0;
  if (!s_wins) return;
  WorkerHdr h;
  if (!plat_worker_hdr(&h)) return;
  HrvWin page[RING_PER_KEY];
  for (int k = 0; k < RING_KEYS; k++) {
    const int bytes = persist_read_data(PK_RING0 + k, page, sizeof(page));
    for (int i = 0; i < bytes / (int)sizeof(HrvWin); i++) {
      if (page[i].rmssd10 && s_win_n < RING_CAP) s_wins[s_win_n++] = page[i];
    }
  }
}
#endif

void plat_replay_begin(Engine *e) {
  const uint32_t now = plat_now_min();
  e->tz_min = tz_offset_min();

  // The clock went backwards (time zone travel does not do this; a reset can).
  if (e->st.last_min > now) e->st.last_min = now;
  s_replay_from = e->st.last_min + 1;

#ifdef ER_DEMO
  const uint32_t from = e->st.last_min - 1440;   // a day earlier, for the 24 h window count
  const uint16_t cap = (uint16_t)((now - from) / 10 + 8);
  s_wins = malloc(sizeof(HrvWin) * cap);
  s_win_n = s_wins ? demo_windows(from, now, e->tz_min, s_wins, cap) : 0;
#else
  // Older than the replay limit: nothing reliable to say about that stretch.
  if (now - e->st.last_min > M_MAX_REPLAY_MIN) {
    engine_skip_to(e, now - M_MAX_REPLAY_MIN);
  }
  load_ring();
  s_sleep_n = s_rest_n = 0;
  // Start a day early so a sleep already under way at last_min is found.
  health_service_activities_iterate(HealthActivitySleep | HealthActivityRestfulSleep,
                                    (time_t)(e->st.last_min - 1440) * 60, (time_t)now * 60,
                                    HealthIterationDirectionFuture, activity_cb, NULL);
  s_buf = malloc(sizeof(HealthMinuteData) * CHUNK_MIN);
#endif

  s_stat_min = s_stat_rec = s_stat_hr = s_stat_asleep = 0;
  s_win_24h = 0;
  for (uint16_t i = 0; i < s_win_n; i++) {
    if ((s_wins[i].t & WIN_T_MASK) + 1440 >= now) s_win_24h++;
  }
  engine_set_windows(e, s_wins, s_win_n);
}

bool plat_replay_step(Engine *e) {
  const uint32_t now = plat_now_min();
  const uint32_t start = e->st.last_min + 1;
  if (start >= now) return false;
  const uint32_t end = MIN(start + CHUNK_MIN, now);   // exclusive

#ifdef ER_DEMO
  for (uint32_t t = start; t < end; t++) {
    EngMinute m;
    demo_minute(t, e->tz_min, &m);
    engine_minute(e, t, &m);
  }
  return end < now;
#else
  if (!s_buf) return false;
  time_t ts = (time_t)start * 60, te = (time_t)end * 60;
  const uint32_t n = health_service_get_minute_history(s_buf, CHUNK_MIN, &ts, &te);
  const uint32_t first = n ? (uint32_t)(ts / 60) : end;

  // Minutes before the first record returned are gaps for certain: the history is written in
  // order, so data after them means they will never arrive. (The call answers a range that
  // has no data with the next records it does have, which is how `first` can lie ahead.)
  // With nothing returned at all, the newest minutes may simply not have been written yet:
  // those count as gaps only once they are older than the lag.
  uint32_t t = start;
  for (; t < first && t < end; t++) {
    if (!n && t + M_RECENT_LAG_MIN >= now) return false;
    engine_minute(e, t, NULL);
  }
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t rt = first + i;
    if (rt < start) continue;
    if (rt >= end) break;
    const HealthMinuteData *r = &s_buf[i];
    EngMinute m = {
      .vmc = r->vmc,
      .steps = r->steps,
      .hr = r->heart_rate_bpm,
      .flags = (uint8_t)((r->is_invalid ? 0 : MF_VALID) |
                         (in_spans(s_sleep, s_sleep_n, rt) ? MF_ASLEEP : 0) |
                         (in_spans(s_rest, s_rest_n, rt) ? (MF_ASLEEP | MF_RESTFUL) : 0)),
    };
    engine_minute(e, rt, &m);
    s_stat_rec++;
    if (m.hr) s_stat_hr++;
    if (m.flags & MF_ASLEEP) s_stat_asleep++;
    t = rt + 1;
  }
  for (; t < end; t++) {
    if (t + M_RECENT_LAG_MIN >= now) return false;
    engine_minute(e, t, NULL);
  }
  return end < now;
#endif
}

void plat_replay_end(Engine *e) {
#ifndef ER_DEMO
  s_stat_min = (uint16_t)(e->st.last_min + 1 - s_replay_from);
  APP_LOG(APP_LOG_LEVEL_INFO, "replay: min %d rec %d hr %d asleep %d wins %d nights %d -> %d flags %d",
          (int)s_stat_min, (int)s_stat_rec, (int)s_stat_hr, (int)s_stat_asleep, (int)s_win_n,
          (int)e->night_count, engine_score(e), (int)e->st.flags);
#endif
  engine_set_windows(e, NULL, 0);
  free(s_wins);
  s_wins = NULL;
  free(s_buf);
  s_buf = NULL;
}
