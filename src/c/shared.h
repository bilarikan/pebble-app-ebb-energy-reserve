// Formats shared by the watchapp and the background worker.
// Plain C only: this header is included from both pebble.h and pebble_worker.h builds,
// and from the host-side engine tests.
#pragma once
#include <stdint.h>

// The SDK headers do not provide these.
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef ABS
#define ABS(a) ((a) < 0 ? -(a) : (a))
#endif

// ---- Persistent storage keys -------------------------------------------------------------
// Each process owns its keys so the app and the worker never write the same record.
#define PK_SETTINGS   1    // Settings      (app writes, worker reads)
#define PK_WHDR       2    // WorkerHdr     (worker writes, app reads)
#define PK_RING0      10   // HRV window ring, keys PK_RING0 .. PK_RING0 + RING_KEYS - 1 (worker)
#define PK_ENGINE     20   // EngineState   (app)
#define PK_SCORE      21   // ScoreRecord   (app) small versioned record for future readers
#define PK_CURVE      22   // 24 h curve    (app)
#define PK_NIGHTS     23   // nightly summaries, the personal baseline (app)
#define PK_ONBOARDED  24   // onboarding notice acknowledged (app)
#define PK_BATT       25   // BattTrack     (app)

// ---- Settings ------------------------------------------------------------------------------
#define SETTINGS_VERSION 1
enum { INTENSITY_LOW = 0, INTENSITY_BALANCED = 1, INTENSITY_HIGH = 2 };
enum { HRV_UNITS_MS = 0, HRV_UNITS_LN = 1 };

typedef struct __attribute__((packed)) {
  uint8_t version;
  uint8_t intensity;   // INTENSITY_*
  uint8_t hrv_units;   // HRV_UNITS_*
  uint8_t max_hr;      // beats per minute
} Settings;

// ---- HRV window ring -----------------------------------------------------------------------
// One record per completed sampling burst. 6 bytes each, 42 per key, 5 keys = 210 windows,
// about 2.5 days at the default sampling intensity.
#define RING_KEYS     5
#define RING_PER_KEY  42
#define RING_CAP      (RING_KEYS * RING_PER_KEY)

#define WIN_T_MASK    0x03FFFFFFu          // bits 0..25: minutes since the Unix epoch
#define WIN_Q_SHIFT   26                   // bits 26..30: valid beat pairs / 8 (capped at 31)
#define WIN_Q_MASK    0x1Fu
#define WIN_ASLEEP    0x80000000u          // bit 31: wearer was asleep when sampled

typedef struct __attribute__((packed)) {
  uint32_t t;         // time, quality and sleep flag, see WIN_* above
  uint16_t rmssd10;   // RMSSD in 0.1 ms
} HrvWin;

// ---- Worker header -------------------------------------------------------------------------
#define WHDR_VERSION 1
enum {
  WS_HRV_OK       = 1 << 0,   // the last request for HRV sampling was accepted
  WS_HRV_REFUSED  = 1 << 1,   // firmware refused HRV sampling (unsupported, or HR turned off)
  WS_LAST_VALID   = 1 << 2,   // the last burst produced a usable window
};

typedef struct __attribute__((packed)) {
  uint8_t  version;
  uint8_t  status;           // WS_*
  uint16_t head;             // next ring index to write
  uint16_t count;            // valid ring records
  uint32_t last_try_utc;     // start of the last burst
  uint32_t last_ok_utc;      // end of the last burst that produced a window
  uint16_t day;              // local day number the *_today counters belong to
  uint16_t sensor_s_today;   // seconds the sensor was held on for HRV today
  uint16_t sensor_s_yday;    // same, previous day
  uint8_t  bursts_today;
  uint8_t  ok_today;
  uint8_t  last_pairs;       // valid beat pairs in the last burst
  uint8_t  last_artifacts;   // rejected intervals in the last burst
} WorkerHdr;

// ---- Public score record -------------------------------------------------------------------
// Kept deliberately small and versioned so a later watchface or relay can read it.
#define SCORE_VERSION 1
enum {
  SF_STALE       = 1 << 0,   // not worn: score is frozen
  SF_LOW_CONF    = 1 << 1,   // resumed after a gap, or no HRV data
  SF_CALIBRATING = 1 << 2,   // personal baseline not established yet
};

typedef struct __attribute__((packed)) {
  uint8_t  version;
  uint8_t  score;         // 0..100
  uint8_t  band;          // 0 very low, 1 low, 2 medium, 3 high
  uint8_t  flags;         // SF_*
  uint32_t updated_utc;
} ScoreRecord;

// Messages between the app and its worker.
#define WMSG_WINDOW_DONE 1   // worker -> app: a burst finished
#define WMSG_MEASURE_NOW 2   // app -> worker: start a burst now, whatever the schedule says
