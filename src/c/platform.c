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

// ---- Replay --------------------------------------------------------------------------------
#ifndef ER_DEMO
typedef struct { uint32_t a, b; } Span;   // minutes [a, b)
#define MAX_SLEEP_SPANS 16
#define MAX_REST_SPANS  48

static Span s_sleep[MAX_SLEEP_SPANS], s_rest[MAX_REST_SPANS];
static uint8_t s_sleep_n, s_rest_n;

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

  // The newest minutes may not have reached the history yet: a missing minute counts as a
  // gap only once it is older than the lag, otherwise it is left for the next pass.
  uint32_t t = start;
  for (; t < first && t < end; t++) {
    if (t + M_RECENT_LAG_MIN >= now) return false;
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
  engine_set_windows(e, NULL, 0);
  free(s_wins);
  s_wins = NULL;
  free(s_buf);
  s_buf = NULL;
}
