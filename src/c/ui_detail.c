// Breakdown view (PRD FR8): each model term behind the current score, last night's inputs,
// the personal baseline, and what the sampler is doing and costing.
#include "app.h"
#include "model.h"
#include "platform.h"

#define MAX_ROWS 20

typedef struct {
  uint8_t section;
  char title[24];
  char value[40];
} Row;

static const char *const SECTIONS[] = { "TODAY", "LAST NIGHT", "BASELINE", "SAMPLER", "ABOUT" };
#define NUM_SECTIONS ((int)ARRAY_LENGTH(SECTIONS))

static Window *s_window;
static MenuLayer *s_menu;
static Row *s_rows;
static uint8_t s_row_n;
static uint8_t s_section_start[NUM_SECTIONS + 1];

static Row *add_row(uint8_t section, const char *title) {
  if (s_row_n >= MAX_ROWS) return &s_rows[MAX_ROWS - 1];
  Row *r = &s_rows[s_row_n++];
  r->section = section;
  strncpy(r->title, title, sizeof(r->title) - 1);
  r->title[sizeof(r->title) - 1] = '\0';
  r->value[0] = '\0';
  return r;
}

static void fmt_points(char *buf, size_t size, int32_t v100, int sign) {
  const int tenths = (int)((v100 + 5) / 10);
  snprintf(buf, size, "%c%d.%d", sign < 0 ? '-' : '+', tenths / 10, tenths % 10);
}

static void build_rows(void) {
  const Engine *e = app_engine();
  const EngineState *s = &e->st;
  char a[16];
  Row *r;
  s_row_n = 0;

  // ---- Today: R = R0 - activity - stress + rest (+ naps)
  const int score = engine_score(e);
  r = add_row(0, "Reserve now");
  snprintf(r->value, sizeof(r->value), "%d - %s", score, band_name(engine_band(score)));

  r = add_row(0, "Morning charge");
  if (s->wake_min) {
    fmt_clock(a, sizeof(a), s->wake_min);
    snprintf(r->value, sizeof(r->value), "%d at %s", (s->r0_100 + 50) / 100, a);
  } else {
    snprintf(r->value, sizeof(r->value), "No full night yet");
  }
  r = add_row(0, "Activity");
  fmt_points(r->value, sizeof(r->value), engine_since_wake(e, CAT_ACT), -1);
  r = add_row(0, "Stress");
  fmt_points(r->value, sizeof(r->value), engine_since_wake(e, CAT_STRESS), -1);
  r = add_row(0, "Rest");
  fmt_points(r->value, sizeof(r->value), engine_since_wake(e, CAT_REST), 1);
  if (engine_since_wake(e, CAT_SLEEP) >= 50) {
    r = add_row(0, "Sleep since waking");
    fmt_points(r->value, sizeof(r->value), engine_since_wake(e, CAT_SLEEP), 1);
  }

  // ---- Last night
  const Night *n = engine_last_night(e);
  r = add_row(1, "Sleep");
  if (n) {
    snprintf(r->value, sizeof(r->value), "%dh %02dm", n->sleep_min / 60, n->sleep_min % 60);
  } else {
    snprintf(r->value, sizeof(r->value), "No night recorded");
  }
  r = add_row(1, "Overnight HRV");
  if (n && n->rmssd10) {
    fmt_hrv(a, sizeof(a), n->rmssd10);
    snprintf(r->value, sizeof(r->value), "%s, %d windows", a, n->wins);
  } else {
    snprintf(r->value, sizeof(r->value), n ? "Too few windows (%d)" : "-", n ? n->wins : 0);
  }
  r = add_row(1, "Resting heart rate");
  if (n && n->rhr) {
    snprintf(r->value, sizeof(r->value), "%d bpm", n->rhr);
  } else {
    snprintf(r->value, sizeof(r->value), "-");
  }

  // ---- Baseline
  r = add_row(2, "Usual overnight HRV");
  if (e->base_rmssd10) {
    fmt_hrv(r->value, sizeof(r->value), e->base_rmssd10);
  } else {
    snprintf(r->value, sizeof(r->value), "-");
  }
  r = add_row(2, "Usual resting HR");
  if (e->base_rhr) {
    snprintf(r->value, sizeof(r->value), "%d bpm", e->base_rhr);
  } else {
    snprintf(r->value, sizeof(r->value), "%d bpm (default)", M_DEFAULT_RHR);
  }
  r = add_row(2, "Nights with HRV");
  if (engine_calibrating(e)) {
    snprintf(r->value, sizeof(r->value), "%d, need %d", e->base_nights, M_MIN_BASE_NIGHTS);
  } else {
    snprintf(r->value, sizeof(r->value), "%d of last %d", e->base_nights, e->night_count);
  }

  // ---- Sampler
  WorkerHdr h;
  const bool have_hdr = plat_worker_hdr(&h);
  r = add_row(3, "Status");
  switch (plat_sampler_state()) {
    case SAMPLER_OK:           snprintf(r->value, sizeof(r->value), "Running"); break;
    case SAMPLER_STARTING:     snprintf(r->value, sizeof(r->value), "Starting"); break;
    case SAMPLER_NOT_RUNNING:  snprintf(r->value, sizeof(r->value), "Off: other app's worker?"); break;
    case SAMPLER_OLD_FIRMWARE: snprintf(r->value, sizeof(r->value), "Needs PebbleOS 4.32+"); break;
    case SAMPLER_HRV_REFUSED:  snprintf(r->value, sizeof(r->value), "HRV refused by watch"); break;
  }
  r = add_row(3, "HRV windows, 24 h");
  snprintf(r->value, sizeof(r->value), "%d", plat_windows_24h());
  r = add_row(3, "Last burst");
  if (have_hdr && h.last_try_utc) {
    fmt_clock(a, sizeof(a), h.last_try_utc / 60);
    snprintf(r->value, sizeof(r->value), "%s %s %d ok, %d bad", a,
             (h.status & WS_LAST_VALID) ? "kept" : "dropped", h.last_pairs, h.last_artifacts);
  } else {
    snprintf(r->value, sizeof(r->value), "None yet");
  }
  r = add_row(3, "Sensor on for HRV");
  if (have_hdr) {
    snprintf(r->value, sizeof(r->value), "%d min today, %d prev", (h.sensor_s_today + 30) / 60,
             (h.sensor_s_yday + 30) / 60);
  } else {
    snprintf(r->value, sizeof(r->value), "-");
  }
  r = add_row(3, "Watch battery use");
  const BattTrack *bt = plat_batt();
  if (bt->rate10_valid) {
    snprintf(r->value, sizeof(r->value), "%d.%d%%/day over %d h", bt->rate10 / 10, bt->rate10 % 10,
             bt->span_h);
  } else {
    snprintf(r->value, sizeof(r->value), "Measuring");
  }

  // ---- About
  r = add_row(4, "An estimate");
  snprintf(r->value, sizeof(r->value), "Not medical advice");

  uint8_t row = 0;
  for (int sec = 0; sec < NUM_SECTIONS; sec++) {
    s_section_start[sec] = row;
    while (row < s_row_n && s_rows[row].section == sec) row++;
  }
  s_section_start[NUM_SECTIONS] = row;
}

static uint16_t get_num_sections(MenuLayer *menu, void *context) {
  return NUM_SECTIONS;
}

static uint16_t get_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  return s_section_start[section + 1] - s_section_start[section];
}

static int16_t get_header_height(MenuLayer *menu, uint16_t section, void *context) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *context) {
  menu_cell_basic_header_draw(ctx, cell, SECTIONS[section]);
}

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  const Row *r = &s_rows[s_section_start[index->section] + index->row];
  menu_cell_basic_draw(ctx, cell, r->title, r->value, NULL);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_rows = malloc(sizeof(Row) * MAX_ROWS);
  build_rows();
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = get_num_sections,
    .get_num_rows = get_num_rows,
    .get_header_height = get_header_height,
    .draw_header = draw_header,
    .draw_row = draw_row,
  });
  menu_layer_set_normal_colors(s_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu, GColorDarkGray, GColorWhite);
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void window_unload(Window *window) {
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  free(s_rows);
  s_rows = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void ui_detail_push(void) {
  if (s_window) return;
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

void ui_detail_refresh(void) {
  if (!s_menu || !s_rows) return;
  build_rows();
  menu_layer_reload_data(s_menu);
}
