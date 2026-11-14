// Phone link. The phone is used for two things only: the settings page (PRD FR10) and
// keeping a copy of the nightly baseline in case the watch loses its storage. Nothing here
// touches the network.
#include "app.h"

static int clamp_i32(int32_t v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : (int)v);
}

static bool nights_plausible(const Night *n, int count) {
  for (int i = 0; i < count; i++) {
    if (n[i].sleep_min > 1440 || n[i].rhr > 150 || n[i].rmssd10 > 5000) return false;
  }
  return true;
}

static void inbox_received(DictionaryIterator *it, void *context) {
  Settings *set = app_settings();
  bool changed = false;
  Tuple *t;

  if ((t = dict_find(it, MESSAGE_KEY_Intensity))) {
    set->intensity = (uint8_t)clamp_i32(t->value->int32, INTENSITY_LOW, INTENSITY_HIGH);
    changed = true;
  }
  if ((t = dict_find(it, MESSAGE_KEY_HrvUnits))) {
    set->hrv_units = (uint8_t)clamp_i32(t->value->int32, HRV_UNITS_MS, HRV_UNITS_LN);
    changed = true;
  }
  if ((t = dict_find(it, MESSAGE_KEY_MaxHr))) {
    set->max_hr = (uint8_t)clamp_i32(t->value->int32, 120, 230);
    changed = true;
  }
  if (changed) app_settings_changed();

  if ((t = dict_find(it, MESSAGE_KEY_ResetBaseline)) && t->value->int32) {
    app_reset_baseline();
  }

  // The phone answers an empty baseline with its backup, if it has one.
  if ((t = dict_find(it, MESSAGE_KEY_RestoreNights)) && t->type == TUPLE_BYTE_ARRAY) {
    Engine *e = app_engine();
    const int count = t->length / (int)sizeof(Night);
    if (e->night_count == 0 && count > 0 && count <= NIGHTS_MAX &&
        t->length % sizeof(Night) == 0 && nights_plausible((const Night *)t->value->data, count)) {
      memcpy(e->nights, t->value->data, (size_t)count * sizeof(Night));
      e->night_count = (uint8_t)count;
      engine_rebase(e);
      app_nights_restored();
    }
  }
}

void comm_backup_nights(void) {
  Engine *e = app_engine();
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;   // phone away: try next time
  dict_write_int32(it, MESSAGE_KEY_NightsCount, e->night_count);
  if (e->night_count) {
    dict_write_data(it, MESSAGE_KEY_NightsBlob, (const uint8_t *)e->nights,
                    (uint16_t)(e->night_count * sizeof(Night)));
  }
  if (app_message_outbox_send() == APP_MSG_OK) e->nights_dirty = false;
}

void comm_init(void) {
  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 256);
}
