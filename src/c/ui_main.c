// Main screen (PRD FR4, FR7): score, state label, reason line, 24-hour curve,
// last-updated time and a confidence flag. State is always written out in words;
// colour only repeats it.
#include "app.h"
#include "model.h"
#include "platform.h"

static Window *s_window;
static Layer *s_canvas;
static bool s_measuring;     // the owner asked for an HRV measurement and it has not finished

static GColor band_color(int band) {
  switch (band) {
    case 3: return GColorGreen;
    case 2: return GColorYellow;
    case 1: return GColorChromeYellow;
    default: return GColorSunsetOrange;
  }
}

static GColor band_fill(int band) {
  switch (band) {
    case 3: return GColorDarkGreen;
    case 2: return GColorLimerick;
    case 1: return GColorWindsorTan;
    default: return GColorDarkCandyAppleRed;
  }
}

static int round_points(int32_t v100) {
  return (int)((v100 + 50) / 100);
}

// "-8 activity, -3 stress, +5 rest" for what changed since the owner last looked.
static void build_reason(char *buf, size_t size) {
  const Engine *e = app_engine();
  static const char *const names[CAT_COUNT] = { "activity", "stress", "rest", "sleep", "off wrist" };
  static const int sign[CAT_COUNT] = { -1, -1, 1, 1, -1 };
  size_t len = 0;
  buf[0] = '\0';
  for (int c = 0; c < CAT_COUNT; c++) {
    const int pts = round_points(engine_since_view(e, c));
    if (pts == 0) continue;
    const int w = snprintf(buf + len, size - len, "%s%c%d %s", len ? ", " : "",
                           sign[c] < 0 ? '-' : '+', pts, names[c]);
    if (w < 0 || (size_t)w >= size - len) break;
    len += (size_t)w;
  }
  if (len == 0) {
    char clock[12];
    fmt_clock(clock, sizeof(clock), e->st.view_min + 1);
    snprintf(buf, size, "Steady since %s", clock);
  }
}

static void draw_curve(GContext *ctx, GRect f) {
  const Engine *e = app_engine();
  const int col_w = f.size.w / CURVE_SLOTS;
  const int x0 = f.origin.x + (f.size.w - col_w * CURVE_SLOTS) / 2;
  const int y_bot = f.origin.y + f.size.h - 1;
  const int h = f.size.h - 2;
  const uint32_t now_slot = e->st.last_min / CURVE_SLOT_MIN;

  // Filled columns, oldest on the left, now on the right.
  int prev_y = -1, prev_x = 0;
  for (int i = 0; i < CURVE_SLOTS; i++) {
    const uint8_t v = e->curve[(now_slot + 1 + i) % CURVE_SLOTS];
    const int x = x0 + i * col_w;
    if (v == CURVE_NONE) {
      // No data: a sparse dotted floor instead of a guessed value.
      graphics_context_set_stroke_color(ctx, GColorDarkGray);
      if (i % 2 == 0) graphics_draw_pixel(ctx, GPoint(x, y_bot - 2));
      prev_y = -1;
      continue;
    }
    const int y = y_bot - v * h / 100;
    graphics_context_set_fill_color(ctx, band_fill(engine_band(v)));
    graphics_fill_rect(ctx, GRect(x, y, col_w, y_bot - y + 1), 0, GCornerNone);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);
    if (prev_y >= 0) {
      graphics_draw_line(ctx, GPoint(prev_x, prev_y), GPoint(x + col_w - 1, y));
    } else {
      graphics_draw_line(ctx, GPoint(x, y), GPoint(x + col_w - 1, y));
    }
    prev_y = y;
    prev_x = x + col_w - 1;
  }

  // Band boundaries as dotted lines, and a tick every six hours.
  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  static const int levels[] = { M_BAND_LOW, M_BAND_MEDIUM, M_BAND_HIGH };
  for (unsigned k = 0; k < ARRAY_LENGTH(levels); k++) {
    const int y = y_bot - levels[k] * h / 100;
    for (int x = x0; x < x0 + col_w * CURVE_SLOTS; x += 6) {
      graphics_draw_pixel(ctx, GPoint(x, y));
    }
  }
  graphics_draw_line(ctx, GPoint(x0, y_bot + 1), GPoint(x0 + col_w * CURVE_SLOTS - 1, y_bot + 1));
  for (int i = 0; i <= CURVE_SLOTS; i += CURVE_SLOTS / 4) {
    const int x = MIN(x0 + i * col_w, x0 + col_w * CURVE_SLOTS - 1);
    graphics_draw_line(ctx, GPoint(x, y_bot + 1), GPoint(x, y_bot + 4));
  }
}

static void canvas_update(Layer *layer, GContext *ctx) {
  const Engine *e = app_engine();
  const GRect b = layer_get_bounds(layer);
  const int w = b.size.w;
  const int score = engine_score(e);
  const int band = engine_band(score);
  const bool stale = (e->st.flags & EF_STALE) != 0;
  const bool low_conf = (e->st.flags & EF_LOW_CONF) != 0;
  const bool calibrating = engine_calibrating(e);
  char buf[64];

  // Score
  snprintf(buf, sizeof(buf), "%d", score);
  graphics_context_set_text_color(ctx, stale ? GColorLightGray : band_color(band));
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_LECO_60_NUMBERS_AM_PM),
                     GRect(0, -6, w, 64), GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // State label
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, band_name(band), fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(0, 56, w, 28), GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // Reason line
  build_reason(buf, sizeof(buf));
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(4, 84, w - 8, 40), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);

  // 24-hour curve
  draw_curve(ctx, GRect(4, 128, w - 8, 54));

  // Under the curve: one line that says why, when the score is less than fully
  // trustworthy; otherwise the axis labels.
  const SamplerState sampler = plat_sampler_state();
  const char *notice = NULL;
  if (s_measuring) {
    notice = "Measuring HRV: keep still";
  } else if (stale) {
    notice = (e->st.flags & EF_AWAY_EST) ? "Not worn: estimating" : "Not worn: score frozen";
  } else if (sampler == SAMPLER_OLD_FIRMWARE) {
    notice = "HRV needs PebbleOS 4.32+";
  } else if (sampler == SAMPLER_NOT_RUNNING) {
    notice = "Sampler off: press Select";
  } else if (sampler == SAMPLER_HRV_REFUSED) {
    notice = "HRV refused: heart rate on?";
  } else if (calibrating) {
    snprintf(buf, sizeof(buf), "Calibrating: night %d of %d",
             MIN(e->base_nights + 1, M_MIN_BASE_NIGHTS), M_MIN_BASE_NIGHTS);
    notice = buf;
  } else if (low_conf) {
    notice = "Resumed after a gap";
  }
  const GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  if (notice) {
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, notice, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(0, 186, w, 18), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
  } else {
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, "24h ago", small, GRect(6, 186, 80, 16), GTextOverflowModeFill,
                       GTextAlignmentLeft, NULL);
    graphics_draw_text(ctx, "12h", small, GRect(w / 2 - 30, 186, 60, 16), GTextOverflowModeFill,
                       GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, "now", small, GRect(w - 86, 186, 80, 16), GTextOverflowModeFill,
                       GTextAlignmentRight, NULL);
  }

  // Footer: last updated on the left, confidence flag on the right
  char clock[12];
  char updated[32];
  fmt_clock(clock, sizeof(clock), e->st.last_min + 1);
  snprintf(updated, sizeof(updated), app_is_updating() ? "Updating..." : "Updated %s", clock);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, updated, small, GRect(6, 206, 110, 16), GTextOverflowModeFill,
                     GTextAlignmentLeft, NULL);

  const bool doubt = stale || low_conf || calibrating;
  const char *flag = stale ? "STALE" : doubt ? "LOW CONF" : "CONF OK";
  graphics_context_set_text_color(ctx, doubt ? GColorWhite : GColorLightGray);
  graphics_draw_text(ctx, flag, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(w - 96, 206, 90, 16), GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  ui_detail_push();
}

// Long press: ask the sampler for an HRV window now instead of waiting for its schedule.
void ui_main_measure_now(void) {
  if (s_measuring || plat_sampler_state() != SAMPLER_OK) return;
  AppWorkerMessage msg = { 0 };
  app_worker_send_message(WMSG_MEASURE_NOW, &msg);
  s_measuring = true;
  vibes_short_pulse();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void select_long_click(ClickRecognizerRef recognizer, void *context) {
  ui_main_measure_now();
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, select_long_click, NULL);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
  s_canvas = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void ui_main_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

void ui_main_measure_done(void) {
  if (!s_measuring) return;
  s_measuring = false;
  vibes_double_pulse();
  ui_main_refresh();
}

void ui_main_refresh(void) {
  if (s_canvas) layer_mark_dirty(s_canvas);
}
