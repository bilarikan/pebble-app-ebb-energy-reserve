// App-side platform code: settings, the background worker's status, battery tracking and
// diagnostics. The scoring is in score.c, which the worker shares.
#pragma once
#include <pebble.h>
#include "engine.h"
#include "shared.h"

// Overall watch battery drain, measured between app launches.
typedef struct __attribute__((packed)) {
  uint32_t t_utc;          // when pct was sampled
  uint8_t  pct;
  uint8_t  rate10_valid;
  uint16_t rate10;         // percent per day x10
  uint16_t span_h;         // hours the rate was measured over
} BattTrack;

void plat_load(Engine *e, Settings *settings);   // settings, the stored score, battery tracking
void plat_save_settings(const Settings *settings);

uint32_t plat_now_min(void);

// Sampler (background worker) status.
typedef enum {
  SAMPLER_OK = 0,
  SAMPLER_STARTING,      // launch requested, or the watch is asking the owner to confirm
  SAMPLER_NOT_RUNNING,   // refused, or another app's worker holds the slot
  SAMPLER_OLD_FIRMWARE,  // PebbleOS older than 4.32: no HRV
  SAMPLER_HRV_REFUSED,   // firmware turned the HRV request down (heart rate switched off?)
} SamplerState;

void plat_sampler_start(void);
SamplerState plat_sampler_state(void);
bool plat_worker_hdr(WorkerHdr *out);
uint16_t plat_windows_24h(void);         // valid HRV windows in the last 24 h

const BattTrack *plat_batt(void);
void plat_log_diagnostics(void);          // what the health service gives this app, to the log
#if defined(ER_DEBUG) && !defined(ER_DEMO)
// Logs what the score was built from. Call with step 0, 1, 2... until it returns false.
bool plat_debug_dump(const Engine *e, int step);
#endif
