// Glue between main.c, the windows and the phone link.
#pragma once
#include <pebble.h>
#include "engine.h"
#include "shared.h"

Engine *app_engine(void);
Settings *app_settings(void);
bool app_is_updating(void);

void app_settings_changed(void);          // settings were edited on the phone
void app_reset_baseline(void);
void app_nights_restored(void);           // a backup arrived from the phone
void app_onboarding_done(void);

// Formatting helpers shared by the windows.
void fmt_clock(char *buf, size_t size, uint32_t t_min);        // local wall-clock time
void fmt_hrv(char *buf, size_t size, uint16_t rmssd10);        // in the unit the owner chose
const char *band_name(int band);

// Windows
void ui_main_push(void);
void ui_main_refresh(void);
void ui_main_measure_now(void);           // ask the sampler for an HRV window right away
void ui_main_measure_done(void);          // a requested HRV measurement finished
void ui_detail_push(void);
void ui_detail_refresh(void);
void ui_onboard_push(void);

// Phone link
void comm_init(void);
void comm_backup_nights(void);
