// Score engine (PRD FR3): a running balance that drains with activity and stress and
// refills with rest and sleep. Pure C with no Pebble dependencies, so the same file is
// compiled into the watchapp and into the host-side tests.
//
// The engine is driven one minute at a time, in order. The platform adapter feeds it the
// watch's per-minute health history, so it gives the same answer whether the app was open
// all day or is catching up after hours in the background.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "shared.h"

#define ENGINE_VERSION   7
#define CURVE_SLOTS      96      // 24 h at 15 minutes
#define CURVE_SLOT_MIN   15
#define CURVE_NONE       255     // slot with no data (not worn)
#define NIGHTS_MAX       14      // rolling baseline window, nights
#define RHR_BINS         64
#define RHR_BIN0         35      // first heart-rate bin, bpm

// EngineState.flags
enum {
  EF_STALE    = 1 << 0,   // not worn
  EF_LOW_CONF = 1 << 1,   // resumed after a long gap, cleared by the next full night
  EF_ASLEEP   = 1 << 2,
  EF_AWAY_EST = 1 << 3,   // not worn, and draining at the assumed daytime rate (else frozen)
  EF_RESTARTED = 1 << 4,  // score restarted after a day or more away, cleared by the next full night
};

// EngMinute.flags
enum {
  MF_VALID   = 1 << 0,
  MF_ASLEEP  = 1 << 1,
  MF_RESTFUL = 1 << 2,
};

typedef struct {
  uint16_t vmc;     // motion counts
  uint8_t  steps;
  uint8_t  hr;      // 0 = no reading this minute
  uint8_t  flags;   // MF_*
} EngMinute;

// One night, as used for the personal baseline. 8 bytes.
typedef struct __attribute__((packed)) {
  uint16_t day;         // local day number of the wake
  uint16_t rmssd10;     // median overnight RMSSD in 0.1 ms, 0 = no usable HRV
  uint16_t sleep_min;
  uint8_t  rhr;         // resting heart rate, 0 = unknown
  uint8_t  wins;        // HRV windows behind rmssd10
} Night;

// Ledger categories. Totals only ever grow; views are differences between snapshots.
enum { CAT_ACT = 0, CAT_STRESS, CAT_REST, CAT_SLEEP, CAT_AWAY, CAT_COUNT };

typedef struct __attribute__((packed)) {
  uint8_t  version;
  uint8_t  flags;              // EF_*
  uint16_t reserve100;         // reserve in 1/100 point
  uint32_t last_min;           // last processed minute (minutes since the Unix epoch)
  uint32_t last_hr_min;        // last minute with a heart-rate reading
  uint32_t last_move_min;
  uint32_t sleep_start_min;
  uint32_t last_asleep_min;
  uint32_t wake_min;           // end of the last main sleep
  uint16_t sleep_min;          // asleep minutes in the current or last sleep
  uint16_t r0_100;             // morning charge
  uint8_t  last_hr;
  uint8_t  night_prev_rhr;     // resting heart rate from earlier sleeps of the same night, 0 = none
  uint32_t tot[CAT_COUNT];     // applied change per category, 1/100 point
  uint32_t wake_tot[CAT_COUNT];   // snapshot at the last main wake
  uint32_t view_tot[CAT_COUNT];   // snapshot at the last time the owner looked
  uint32_t view_min;
  uint16_t view_reserve100;
  uint32_t night_sleep_start;  // start of the first sleep of the night behind the newest record
  uint32_t night_seg_start;    // sleep_start_min of the sleep last folded into that record
  uint16_t night_prev_min;     // asleep minutes in earlier sleeps of the same night
  int16_t  rem;                // what the last scaled change left over, in 1/M_LEVEL_REF100 unit
  uint16_t motion_bits;        // one bit per minute, newest lowest: did the watch move at all
  uint32_t last_motion_min;    // last minute with any motion counts or steps
  uint32_t stale_since_min;    // when the current frozen stretch began
  uint8_t  rhr_hist[RHR_BINS]; // heart-rate histogram for the current sleep
} EngineState;

typedef struct {
  EngineState st;
  uint8_t curve[CURVE_SLOTS];
  Night   nights[NIGHTS_MAX];
  uint8_t night_count;
  bool    nights_dirty;        // set when a night was added, for backup

  // Derived from nights by engine_rebase()
  uint16_t base_rmssd10;
  uint8_t  base_rhr;
  uint8_t  base_nights;        // nights that carry HRV

  // Inputs set by the platform adapter
  const HrvWin *wins;          // snapshot of the worker's ring, any order
  uint16_t win_count;
  int16_t  tz_min;             // local time offset from UTC, minutes
  uint8_t  max_hr;

  // Lookup cache for the latest HRV window at or before a minute
  uint32_t wc_from, wc_until;
  uint32_t wc_t;
  uint16_t wc_rmssd10;
  bool     wc_asleep;
} Engine;

void engine_reset(Engine *e, uint32_t start_min);
void engine_rebase(Engine *e);
void engine_set_windows(Engine *e, const HrvWin *wins, uint16_t count);

// Process one minute. Pass NULL when the watch has no record for it.
void engine_minute(Engine *e, uint32_t t_min, const EngMinute *m);
// Jump over a long stretch with no data: freeze, mark stale, blank the curve.
void engine_skip_to(Engine *e, uint32_t t_min);

void engine_mark_viewed(Engine *e);
void engine_clear_baseline(Engine *e);

int  engine_score(const Engine *e);            // 0..100
int  engine_band(int score);                   // 0 very low .. 3 high
bool engine_calibrating(const Engine *e);
// Change per category since the last view / since the morning, in 1/100 point.
int32_t engine_since_view(const Engine *e, int cat);
int32_t engine_since_wake(const Engine *e, int cat);
// Last night's record, or NULL.
const Night *engine_last_night(const Engine *e);
