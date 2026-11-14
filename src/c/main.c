// Ebb: a 0-100 energy reserve for the Pebble Time 2, computed on the watch.
// See README.md for the layout of the project and docs/SCORING.md for the model.
#include "app.h"
#include "model.h"
#include "platform.h"

#define STEPS_PER_SLICE 6   // hours of history handled before yielding to the event loop

static Engine s_eng;
static Settings s_set;
static AppTimer *s_timer;
static bool s_updating;
static bool s_backup_sent;
static bool s_viewed_once;       // the owner got past onboarding this launch
static uint32_t s_first_view_min;   // fresh install: where "since you last looked" starts

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

// ---- Catching up with the watch's history ----------------------------------------------------
static void refresh_ui(void) {
  ui_main_refresh();
  ui_detail_refresh();
}

static void replay_finish(void) {
  plat_replay_end(&s_eng);
  s_updating = false;
  if (s_first_view_min) {
    engine_mark_viewed(&s_eng);
    s_first_view_min = 0;
  }
  plat_save(&s_eng);
  if (!s_backup_sent || s_eng.nights_dirty) {
    comm_backup_nights();
    s_backup_sent = true;
  }
  refresh_ui();
}

static void replay_slice(void *context) {
  s_timer = NULL;
  for (int i = 0; i < STEPS_PER_SLICE; i++) {
    const bool more = plat_replay_step(&s_eng);
    if (s_first_view_min && s_eng.st.last_min >= s_first_view_min) {
      engine_mark_viewed(&s_eng);
      s_first_view_min = 0;
    }
    if (!more) {
      replay_finish();
      return;
    }
  }
  s_timer = app_timer_register(2, replay_slice, NULL);
}

static void replay_start(void) {
  if (s_updating) return;
  s_updating = true;
  plat_replay_begin(&s_eng);
  replay_slice(NULL);
}

// ---- Events --------------------------------------------------------------------------------
static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  replay_start();
}

static void worker_message(uint16_t type, AppWorkerMessage *message) {
  if (type == WMSG_WINDOW_DONE) replay_start();
}

void app_settings_changed(void) {
  s_eng.max_hr = s_set.max_hr;
  plat_save_settings(&s_set);
  refresh_ui();
}

void app_reset_baseline(void) {
  engine_clear_baseline(&s_eng);
  plat_save_nights(&s_eng);
  s_eng.nights_dirty = false;   // the phone dropped its copy when it sent the reset
  refresh_ui();
}

void app_nights_restored(void) {
  plat_save_nights(&s_eng);
  refresh_ui();
}

void app_onboarding_done(void) {
  s_viewed_once = true;
  plat_sampler_start();
  refresh_ui();
}

// ---- Lifecycle -----------------------------------------------------------------------------
static void init(void) {
  const bool had_state = persist_exists(PK_ENGINE);
  plat_load(&s_eng, &s_set);
  if (!had_state) {
#ifdef ER_DEMO
    s_first_view_min = plat_now_min() - 4 * 60;   // so the demo has a reason line to show
#else
    s_first_view_min = UINT32_MAX;                // marked when the first catch-up finishes
#endif
  }

  comm_init();
  app_worker_message_subscribe(worker_message);
  ui_main_push();
  if (persist_read_bool(PK_ONBOARDED)) {
    app_onboarding_done();
  } else {
    ui_onboard_push();
  }
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  replay_start();
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  app_worker_message_unsubscribe();
  if (s_timer) app_timer_cancel(s_timer);
  if (s_updating) plat_replay_end(&s_eng);
  // The next launch explains what changed since this one.
  if (s_viewed_once && !s_first_view_min) engine_mark_viewed(&s_eng);
  plat_save(&s_eng);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
