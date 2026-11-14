// First-launch notice (PRD FR6): the score is an estimate, not medical advice.
#include "app.h"

static Window *s_window;
static TextLayer *s_title, *s_body, *s_hint;

static void select_click(ClickRecognizerRef recognizer, void *context) {
  persist_write_bool(PK_ONBOARDED, true);
  window_stack_remove(s_window, true);
  app_onboarding_done();
}

static void back_click(ClickRecognizerRef recognizer, void *context) {
  // Leaving without accepting closes the app; nothing starts sampling.
  window_stack_pop_all(true);
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

static TextLayer *make_text(Layer *root, GRect frame, const char *font_key, const char *text) {
  TextLayer *t = text_layer_create(frame);
  text_layer_set_background_color(t, GColorClear);
  text_layer_set_text_color(t, GColorWhite);
  text_layer_set_font(t, fonts_get_system_font(font_key));
  text_layer_set_text_alignment(t, GTextAlignmentCenter);
  text_layer_set_overflow_mode(t, GTextOverflowModeWordWrap);
  text_layer_set_text(t, text);
  layer_add_child(root, text_layer_get_layer(t));
  return t;
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  const GRect b = layer_get_bounds(root);
  s_title = make_text(root, GRect(8, 6, b.size.w - 16, 30), FONT_KEY_GOTHIC_24_BOLD,
                      "An estimate");
  s_body = make_text(root, GRect(8, 38, b.size.w - 16, b.size.h - 74), FONT_KEY_GOTHIC_18,
                     "This score is estimated on your watch from heart rate, heart rate "
                     "variability, movement and sleep.\n\nIt is not medical advice and does "
                     "not diagnose anything.");
  s_hint = make_text(root, GRect(8, b.size.h - 32, b.size.w - 16, 24), FONT_KEY_GOTHIC_18_BOLD,
                     "Select to continue");
}

static void window_unload(Window *window) {
  text_layer_destroy(s_title);
  text_layer_destroy(s_body);
  text_layer_destroy(s_hint);
  window_destroy(s_window);
  s_window = NULL;
}

void ui_onboard_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}
