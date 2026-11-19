// Ebb: a 0-100 energy reserve for the Pebble Time 2, computed on the watch.
// See README.md for the layout of the project and docs/SCORING.md for the model.
#include "app.h"
#include "model.h"
#include "platform.h"
#include "score.h"

#define STEPS_PER_SLICE 6   // hours of history handled before yielding to the event loop
#define WORKER_WAIT_MS  5000   // how long "Updating..." waits for the worker before showing what is stored

// The background worker does the scoring and this app shows the result. The app scores for
// itself only when no worker is running (not yet started, refused, or another app's worker
// holds the single slot) and in ER_DEMO builds.
static Engine s_eng;
static Settings s_set;
static AppTimer *s_timer;        // next slice of a run this app is doing itself
static AppTimer *s_wait;         // waiting for the worker to answer
static bool s_updating;
static bool s_local;             // the run in progress is this app's own
static bool s_backup_sent;
static bool s_viewed_once;       // the owner got past onboarding this launch
#ifdef ER_DEMO
static uint32_t s_first_view_min;   // fresh demo state: where "since you last looked" starts
#endif

Engine *app_engine(void) { return &s_eng; }
Settings *app_settings(void) { return &s_set; }
bool app_is_updating(void) { return s_updating; }

// ---- Formatting ----------------------------------------------------------------------------
void fmt_clock(char *buf, size_t size, uint32_t t_min) {
  const time_t t = (time_t)t_min * 60;
  strftime(buf, size, clock_is_24h_style() ? "%H:%M" : "%I:%M%p", localtime(&t));
  if (buf[0] == '0' && !clock_is_24h_style()) memmove(buf, buf + 1, strlen(buf));
}

// ln(RMSSD in ms) x100, integer only: log2 by repeated squaring, then scaled by ln 2.
static int ln_x100(uint32_t rmssd10) {
  if (rmssd10 < 10) return 0;
  int exp = 0;
  for (uint32_t x = rmssd10; x >= 2; x >>= 1) exp++;
  uint32_t m = (rmssd10 << 15) >> exp;   // mantissa in [1, 2), Q15
  int frac = 0;
  for (int i = 0; i < 8; i++) {
    m = (m * m) >> 15;
    frac <<= 1;
    if (m >= 65536u) { m >>= 1; frac |= 1; }
  }
  return (exp * 256 + frac) * 6931 / 25600 - 230;   // minus ln(10): the input is in 0.1 ms
}

void fmt_hrv(char *buf, size_t size, uint16_t rmssd10) {
  if (s_set.hrv_units == HRV_UNITS_LN) {
    const int ln = ln_x100(rmssd10);
    snprintf(buf, size, "ln %d.%02d", ln / 100, ln % 100);
  } else {
    snprintf(buf, size, "%d.%d ms", rmssd10 / 10, rmssd10 % 10);
  }
}

const char *band_name(int band) {
  static const char *const names[] = { "VERY LOW", "LOW", "MEDIUM", "HIGH" };
  return names[band < 0 ? 0 : (band > 3 ? 3 : band)];
}

// ---- Keeping the score current ----------------------------------------------------------------
static void refresh_ui(void) {
  ui_main_refresh();
  ui_detail_refresh();
}

// The phone keeps a copy of the baseline: send it once per launch and whenever it changes.
static void backup_if(bool changed) {
  if (!s_backup_sent || changed) {
    comm_backup_nights();
    s_backup_sent = true;
  }
}

#ifndef ER_DEMO
// Shows what the worker last stored.
static void show_stored(void) {
  Night before[NIGHTS_MAX];
  const uint8_t count_before = s_eng.night_count;
  memcpy(before, s_eng.nights, sizeof(before));
  score_load(&s_eng);
  s_updating = false;
#ifdef ER_DEBUG
  APP_LOG(APP_LOG_LEVEL_INFO, "shown: %d r%d tot %d %d %d %d %d", engine_score(&s_eng),
          (int)s_eng.st.reserve100, (int)s_eng.st.tot[0], (int)s_eng.st.tot[1], (int)s_eng.st.tot[2],
          (int)s_eng.st.tot[3], (int)s_eng.st.tot[4]);
#endif
  backup_if(count_before != s_eng.night_count || memcmp(before, s_eng.nights, sizeof(before)) != 0);
  refresh_ui();
}

static void wait_done(void *context) {
  s_wait = NULL;
  show_stored();   // a long catch-up answers later; show what there is meanwhile
}
#endif

static void local_finish(void) {
  score_end(&s_eng);
  s_local = false;
  s_updating = false;
#ifdef ER_DEMO
  if (s_first_view_min) {
    engine_mark_viewed(&s_eng);
    s_first_view_min = 0;
    score_store(&s_eng, false);
  }
#endif
  backup_if(s_eng.nights_dirty);
  refresh_ui();
}

static void local_slice(void *context) {
  s_timer = NULL;
  for (int i = 0; i < STEPS_PER_SLICE; i++) {
    const bool more = score_step(&s_eng);
#ifdef ER_DEMO
    if (s_first_view_min && s_eng.st.last_min >= s_first_view_min) {
      engine_mark_viewed(&s_eng);
      s_first_view_min = 0;
    }
#endif
    if (!more) {
      local_finish();
      return;
    }
  }
  s_timer = app_timer_register(2, local_slice, NULL);
}

static void local_start(void) {
  if (!score_prepare()) {
    s_updating = false;
    return;
  }
  s_updating = true;
  s_local = true;
  score_begin(&s_eng);
  local_slice(NULL);
}

static void update_start(void) {
  if (s_updating) return;
#ifndef ER_DEMO
  if (app_worker_is_running()) {
    s_updating = true;
    AppWorkerMessage msg = { 0 };
    app_worker_send_message(WMSG_SCORE_NOW, &msg);
    s_wait = app_timer_register(WORKER_WAIT_MS, wait_done, NULL);
    return;
  }
#endif
  local_start();
}

// ---- Events --------------------------------------------------------------------------------
static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  update_start();
}

static void worker_message(uint16_t type, AppWorkerMessage *message) {
  if (type == WMSG_WINDOW_DONE) {
    ui_main_measure_done();
    update_start();
  }
#ifndef ER_DEMO
  if (type == WMSG_SCORED) {
    // data1: the worker's free memory during the run; data2: sleep sessions it was given,
    // 255 if it had to abandon the run.
    APP_LOG(APP_LOG_LEVEL_INFO, "worker scored %d, heap %d, sessions %d", (int)message->data0,
            (int)message->data1, (int)message->data2);
  }
  if (type == WMSG_SCORED && !s_local) {
    if (s_wait) {
      app_timer_cancel(s_wait);
      s_wait = NULL;
    }
    if (message->data2 == 255) {
      local_start();   // the worker could not do the run and stored nothing: do it here
    } else {
      show_stored();
    }
  }
#endif
}

void app_settings_changed(void) {
  s_eng.max_hr = s_set.max_hr;
  plat_save_settings(&s_set);
  refresh_ui();
}

void app_reset_baseline(void) {
  // The one case where the app writes a scorer's record: a deliberate reset from the phone.
  // The worker reads the nights afresh at the start of every run.
  engine_clear_baseline(&s_eng);
  s_eng.nights_dirty = false;      // the phone dropped its copy when it sent the reset
  persist_delete(PK_NIGHTS);
  refresh_ui();
}

void app_nights_restored(void) {
#ifndef ER_DEMO
  update_start();
#endif
  refresh_ui();
}

void app_onboarding_done(void) {
  s_viewed_once = true;
  plat_sampler_start();
  refresh_ui();
}

// ---- Lifecycle -----------------------------------------------------------------------------
#ifdef ER_DEBUG
static void debug_timer(void *context) {
  static int step = -1;
  if (step < 0) plat_log_diagnostics();
#ifndef ER_DEMO
  if (step < 0) step = 0;
  if (plat_debug_dump(&s_eng, step++)) {
    app_timer_register(250, debug_timer, NULL);
    return;
  }
  score_debug_flip_label(&s_eng);   // the next run rebuilds; its result should match
#endif
  ui_main_measure_now();
}
#endif

static void init(void) {
  const bool had_state = persist_exists(PK_ENGINE);
  plat_load(&s_eng, &s_set);
#ifdef ER_DEMO
  if (!had_state) s_first_view_min = plat_now_min() - 4 * 60;   // so the demo has a reason line
#endif

  const WatchInfoVersion fw = watch_info_get_firmware_version();
  // How old the stored score is on launch shows whether the worker has been keeping it up.
  APP_LOG(APP_LOG_LEVEL_INFO, "Ebb start: PebbleOS %d.%d.%d, %s state %d min old, %d nights",
          (int)fw.major, (int)fw.minor, (int)fw.patch, had_state ? "saved" : "fresh",
          (int)(plat_now_min() - s_eng.st.last_min), (int)s_eng.night_count);

  comm_init();
  app_worker_message_subscribe(worker_message);
  ui_main_push();
  if (persist_read_bool(PK_ONBOARDED)) {
    app_onboarding_done();
  } else {
    ui_onboard_push();
  }
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  update_start();
#ifdef ER_DEBUG
  // A little after launch, so a log session attached by the installer catches it.
  app_timer_register(12000, debug_timer, NULL);
#endif
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  app_worker_message_unsubscribe();
  if (s_timer) app_timer_cancel(s_timer);
  if (s_wait) app_timer_cancel(s_wait);
  if (s_local) score_end(&s_eng);
  // The next launch explains what changed since this one.
#ifdef ER_DEMO
  if (s_viewed_once && !s_first_view_min) engine_mark_viewed(&s_eng);
  score_store(&s_eng, true);
#else
  if (s_viewed_once) {
    const uint32_t shown = s_eng.st.last_min;
    persist_write_data(PK_VIEW, &shown, sizeof(shown));
  }
#endif
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
