// Platform adapter: every call into PebbleOS health, storage and worker APIs that the
// score depends on goes through this file, so a change to the (new, Time 2-only) HRV API
// touches one place. With ER_DEMO it serves synthetic data instead.
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

void plat_load(Engine *e, Settings *settings);
void plat_save(Engine *e);
void plat_save_settings(const Settings *settings);
void plat_save_nights(const Engine *e);

// Catch the engine up with the watch's per-minute history. Call begin once, then step until
// it returns false, then end. Each step handles at most an hour of history.
void plat_replay_begin(Engine *e);
bool plat_replay_step(Engine *e);
void plat_replay_end(Engine *e);

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
uint16_t plat_windows_24h(void);         // valid HRV windows in the last 24 h, as of the last replay

const BattTrack *plat_batt(void);
void plat_log_diagnostics(void);          // what the health service gives this app, to the log
#if defined(ER_DEBUG) && !defined(ER_DEMO)
// Logs what the score was built from. Call with step 0, 1, 2... until it returns false.
bool plat_debug_dump(const Engine *e, int step);
#endif
