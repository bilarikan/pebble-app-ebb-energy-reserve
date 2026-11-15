// Background HRV sampler (PRD FR1).
//
// The firmware collects HRV only while a process holds an HRV sample period, so this worker
// owns the sampling schedule. It holds the sensor on in short bursts, turns a burst's
// beat-to-beat intervals into one RMSSD value, appends it to a ring in persistent storage,
// and always releases the sensor afterwards. Scoring happens in the app, not here: a worker
// has 10 KiB for code and data together.
//
// Only this worker sets sample periods. The app and its worker share one sensor subscription
// (it is keyed by app, not by process), so a second owner could cancel a burst in progress.
#include <pebble_worker.h>
#include "../../src/c/shared.h"

// ---- Sampling plan, indexed by Settings.intensity --------------------------------------
static const uint8_t NIGHT_INTERVAL_MIN[] = {20, 10, 5};
static const uint8_t DAY_INTERVAL_MIN[]   = {60, 30, 15};
static const uint8_t WINDOW_S[]           = {60, 60, 120};

// The firmware allows 20 s for the sensor to spin up before a good reading, and the driver
// then reports real beat-to-beat intervals, a few per algorithm result. The window is
// therefore timed from the first interval, not from the request. On the wrist the first
// ten or so intervals were still unusable (wild values, runs of repeats), hence the settle.
#define ABORT_S         45     // no interval at all by now: not worn, release the sensor
#define SETTLE_S        15     // discard intervals for this long after the first one
#define STILL_TICKS     3      // minutes without a step before a daytime burst may start
#define MAX_BACKOFF     4      // interval multiplier after bursts that saw no signal
#define PPI_MIN_MS      300    // 200 bpm
#define PPI_MAX_MS      2000   // 30 bpm
#define MAX_ARTIFACT_PCT 25    // reject the window above this share of bad intervals

typedef struct {
  bool     active;
  bool     asleep;
  bool     seen;          // an interval was delivered, good or bad
  uint8_t  window_s;
  uint8_t  off_wrist;     // "not on a wrist" reports from the sensor (interval of 0)
  time_t   start;
  time_t   first;         // when the first interval arrived
  uint16_t prev;          // previous accepted interval, 0 after an artifact
  uint16_t last_raw;      // previous interval as delivered, for spotting repeats
  uint16_t pairs;         // accepted successive differences
  uint16_t artifacts;     // intervals out of range or jumping more than 20%
  uint16_t repeats;       // intervals identical to the one before
  uint32_t sumsq;         // sum of squared successive differences, ms^2
  int32_t  steps_start;
} Burst;

static WorkerHdr s_hdr;
static Burst s_b;
static AppTimer *s_timer;
static int32_t s_prev_steps = -1;
static uint8_t s_still;
static uint8_t s_backoff = 1;
static HrvWin s_page[RING_PER_KEY];   // one ring key, reused for read-modify-write

// Raw intervals to the log, eight to a line, for checking the sensor's output by hand.
// Only in ER_DEBUG builds.
#ifdef ER_DEBUG
#define RAW_PER_LINE 8
static uint16_t s_raw[RAW_PER_LINE];
static uint8_t s_raw_n;

static void raw_flush(void) {
  if (!s_raw_n) return;
  for (uint8_t i = s_raw_n; i < RAW_PER_LINE; i++) s_raw[i] = 0;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "ppi %d %d %d %d %d %d %d %d", s_raw[0], s_raw[1], s_raw[2], s_raw[3],
          s_raw[4], s_raw[5], s_raw[6], s_raw[7]);
  s_raw_n = 0;
}

static void raw_add(uint16_t ppi) {
  s_raw[s_raw_n++] = ppi;
  if (s_raw_n == RAW_PER_LINE) raw_flush();
}
#else
static void raw_flush(void) {}
static void raw_add(uint16_t ppi) {}
#endif

static void save_hdr(void) {
  persist_write_data(PK_WHDR, &s_hdr, sizeof(s_hdr));
}

static uint32_t isqrt32(uint32_t v) {
  uint32_t r = 0, bit = 1u << 30;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else { r >>= 1; }
    bit >>= 2;
  }
  return r;
}

static void ring_append(HrvWin w) {
  const uint32_t key = PK_RING0 + s_hdr.head / RING_PER_KEY;
  memset(s_page, 0, sizeof(s_page));
  persist_read_data(key, s_page, sizeof(s_page));
  s_page[s_hdr.head % RING_PER_KEY] = w;
  persist_write_data(key, s_page, sizeof(s_page));
  s_hdr.head = (s_hdr.head + 1) % RING_CAP;
  if (s_hdr.count < RING_CAP) s_hdr.count++;
}

// Release the sensor. Both periods are cleared so nothing keeps the green LED running.
static void sensor_release(void) {
  health_service_set_hrv_sample_period(0);
  health_service_set_heart_rate_sample_period(0);
}

static void burst_finish(void) {
  if (!s_b.active) return;
  s_b.active = false;
  if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
  sensor_release();

  const time_t now = time(NULL);
  const uint32_t on_s = (uint32_t)(now - s_b.start);
  s_hdr.sensor_s_today = (uint16_t)MIN(65535u, s_hdr.sensor_s_today + on_s);
  if (s_hdr.bursts_today < 255) s_hdr.bursts_today++;
  s_hdr.last_pairs = (uint8_t)MIN(255, s_b.pairs);
  s_hdr.last_artifacts = (uint8_t)MIN(255, s_b.artifacts + s_b.repeats);

  // A window counts only if enough clean beat pairs arrived, few intervals were rejected,
  // and (awake) the wearer did not start walking part-way through.
  const uint16_t bad = s_b.artifacts + s_b.repeats;
  const uint16_t total = s_b.pairs + bad;
  bool valid = s_b.pairs >= s_b.window_s / 2 && bad * 100 <= total * MAX_ARTIFACT_PCT;
  if (valid && !s_b.asleep &&
      health_service_sum_today(HealthMetricStepCount) != s_b.steps_start) {
    valid = false;
  }

  uint32_t rmssd10 = 0;
  if (valid) {
    rmssd10 = isqrt32((s_b.sumsq / s_b.pairs) * 100u);
    if (rmssd10 > 65535u) rmssd10 = 65535u;
    valid = rmssd10 > 0;
  }

  if (valid) {
    HrvWin w = {
      .t = ((uint32_t)(now / 60) & WIN_T_MASK) |
           ((uint32_t)MIN(WIN_Q_MASK, (uint32_t)(s_b.pairs / 8)) << WIN_Q_SHIFT) |
           (s_b.asleep ? WIN_ASLEEP : 0),
      .rmssd10 = (uint16_t)rmssd10,
    };
    ring_append(w);
    s_hdr.last_ok_utc = (uint32_t)now;
    s_hdr.status |= WS_LAST_VALID;
    if (s_hdr.ok_today < 255) s_hdr.ok_today++;
  } else {
    s_hdr.status &= ~WS_LAST_VALID;
  }

  // No signal at all means the watch is probably off the wrist: space the next tries out.
  s_backoff = s_b.seen ? 1 : MIN(MAX_BACKOFF, s_backoff * 2);
  save_hdr();

  // The watch truncates long log lines, so this one is terse:
  // seconds on, seconds to first interval, off-wrist reports, pairs kept, intervals rejected.
  raw_flush();
  APP_LOG(APP_LOG_LEVEL_INFO, "burst end: on %d first %d offw %d ok %d rej %d rep %d rmssd %d.%d %s",
          (int)on_s, s_b.seen ? (int)(s_b.first - s_b.start) : -1, (int)s_b.off_wrist, (int)s_b.pairs,
          (int)s_b.artifacts, (int)s_b.repeats, (int)(rmssd10 / 10), (int)(rmssd10 % 10),
          valid ? "kept" : "dropped");

  AppWorkerMessage msg = { .data0 = (uint16_t)rmssd10, .data1 = s_b.pairs, .data2 = s_b.artifacts };
  app_worker_send_message(WMSG_WINDOW_DONE, &msg);
}

static void burst_timer(void *context) {
  s_timer = NULL;
  if (!s_b.active) return;
  if (!s_b.seen) {
    burst_finish();   // nothing by ABORT_S
    return;
  }
  const uint32_t elapsed = (uint32_t)(time(NULL) - s_b.first);
  const uint32_t total = SETTLE_S + s_b.window_s;
  if (elapsed >= total) {
    burst_finish();
    return;
  }
  s_timer = app_timer_register((total - elapsed) * 1000, burst_timer, NULL);
}

static void burst_start(bool asleep, uint8_t intensity, int32_t steps) {
  const time_t now = time(NULL);
  s_hdr.last_try_utc = (uint32_t)now;

  // A 1 s period keeps the green optical path on for the whole window, so successive
  // intervals come from neighbouring beats. Longer periods let the firmware duty-cycle the
  // sensor between readings, which breaks the successive-difference maths.
  if (!health_service_set_hrv_sample_period(1)) {
    s_hdr.status = (s_hdr.status & ~WS_HRV_OK) | WS_HRV_REFUSED;
    save_hdr();
    APP_LOG(APP_LOG_LEVEL_WARNING, "HRV sampling refused by the firmware");
    AppWorkerMessage none = { 0 };
    app_worker_send_message(WMSG_WINDOW_DONE, &none);   // so a waiting app stops waiting
    return;
  }
  s_hdr.status = (s_hdr.status & ~WS_HRV_REFUSED) | WS_HRV_OK;
  save_hdr();

  s_b = (Burst) {
    .active = true,
    .asleep = asleep,
    .window_s = WINDOW_S[intensity],
    .start = now,
    .steps_start = steps,
  };
  s_timer = app_timer_register(ABORT_S * 1000, burst_timer, NULL);
  APP_LOG(APP_LOG_LEVEL_INFO, "burst start: %s, %d s window", asleep ? "asleep" : "awake",
          (int)s_b.window_s);
}

static void health_handler(HealthEventType event, void *context) {
  if (event != HealthEventHRVUpdate || !s_b.active) return;
  const uint16_t ppi = health_service_peek_hrv_ppi_ms();
  if (ppi == 0) {
    // The sensor reports "not on a wrist" as an interval of 0.
    if (s_b.off_wrist < 255) s_b.off_wrist++;
    return;
  }
  raw_add(ppi);
  const time_t now = time(NULL);
  if (!s_b.seen) {
    s_b.seen = true;
    s_b.first = now;
  }
  if (now - s_b.first < SETTLE_S) return;

  // The sensor's algorithm re-reports an interval when it has nothing new (seen on the wrist
  // as runs of identical values while the signal was poor). A repeat carries no information
  // about variability, and counting it as a clean pair would pull RMSSD towards zero.
  if (ppi == s_b.last_raw) {
    s_b.repeats++;
    return;
  }
  s_b.last_raw = ppi;

  if (ppi < PPI_MIN_MS || ppi > PPI_MAX_MS) {
    s_b.artifacts++;
    s_b.prev = 0;
    return;
  }
  if (s_b.prev) {
    const int32_t d = (int32_t)ppi - (int32_t)s_b.prev;
    // Reject a jump of more than 20% of the previous interval (missed or extra beat).
    if (ABS(d) * 5 > (int32_t)s_b.prev) {
      s_b.artifacts++;
    } else {
      s_b.sumsq += (uint32_t)(d * d);
      s_b.pairs++;
    }
  }
  s_b.prev = ppi;
}

static void tick_handler(struct tm *tm, TimeUnits changed) {
  const uint16_t day = (uint16_t)(tm->tm_year * 366 + tm->tm_yday);
  if (day != s_hdr.day) {
    s_hdr.sensor_s_yday = (uint16_t)(day == s_hdr.day + 1 ? s_hdr.sensor_s_today : 0);
    s_hdr.day = day;
    s_hdr.sensor_s_today = 0;
    s_hdr.bursts_today = 0;
    s_hdr.ok_today = 0;
    save_hdr();
  }
  if (s_b.active) return;

  const HealthActivityMask act = health_service_peek_current_activities();
  const bool asleep = (act & (HealthActivitySleep | HealthActivityRestfulSleep)) != 0;
  const int32_t steps = health_service_sum_today(HealthMetricStepCount);
  if (steps == s_prev_steps) {
    if (s_still < 255) s_still++;
  } else {
    s_still = 0;
  }
  s_prev_steps = steps;

  if (battery_state_service_peek().is_plugged) return;   // on the charger, not the wrist
  const bool moving = (act & (HealthActivityWalk | HealthActivityRun | HealthActivityOpenWorkout)) != 0;
  if (!asleep && (moving || s_still < STILL_TICKS)) return;

  Settings set = { .intensity = INTENSITY_BALANCED };
  persist_read_data(PK_SETTINGS, &set, sizeof(set));
  if (set.intensity > INTENSITY_HIGH) set.intensity = INTENSITY_BALANCED;

  const uint32_t now = (uint32_t)time(NULL);
  const uint32_t interval_s =
      (uint32_t)(asleep ? NIGHT_INTERVAL_MIN : DAY_INTERVAL_MIN)[set.intensity] * 60u * s_backoff;
  if (s_hdr.last_try_utc > now) s_hdr.last_try_utc = 0;   // clock moved backwards
  if (now - s_hdr.last_try_utc < interval_s) return;

  burst_start(asleep, set.intensity, steps);
}

static void app_message(uint16_t type, AppWorkerMessage *message) {
  if (type != WMSG_MEASURE_NOW || s_b.active) return;
  Settings set = { .intensity = INTENSITY_BALANCED };
  persist_read_data(PK_SETTINGS, &set, sizeof(set));
  if (set.intensity > INTENSITY_HIGH) set.intensity = INTENSITY_BALANCED;
  const HealthActivityMask act = health_service_peek_current_activities();
  burst_start((act & (HealthActivitySleep | HealthActivityRestfulSleep)) != 0, set.intensity,
              health_service_sum_today(HealthMetricStepCount));
}

int main(void) {
  if (persist_read_data(PK_WHDR, &s_hdr, sizeof(s_hdr)) != (int)sizeof(s_hdr) ||
      s_hdr.version != WHDR_VERSION || s_hdr.head >= RING_CAP || s_hdr.count > RING_CAP) {
    memset(&s_hdr, 0, sizeof(s_hdr));
    s_hdr.version = WHDR_VERSION;
  }
  // If the worker was killed mid-burst the periods may still be set: clear them first.
  sensor_release();

  const bool subscribed = health_service_events_subscribe(health_handler, NULL);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  app_worker_message_subscribe(app_message);
  APP_LOG(APP_LOG_LEVEL_INFO, "sampler started, health events %s, %d windows stored",
          subscribed ? "on" : "UNAVAILABLE", (int)s_hdr.count);
  worker_event_loop();

  burst_finish();
  sensor_release();
  tick_timer_service_unsubscribe();
  health_service_events_unsubscribe();
  return 0;
}
