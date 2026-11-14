// Scoring model parameters. Every number here is a placeholder to be fitted during
// validation (PRD "Scoring model"); none comes from another vendor's method.
// docs/SCORING.md explains what each one does. Rates are in 1/100 reserve point per minute.
#pragma once

// Recharge (g)
#define M_G_SLEEP             15    // asleep, HRV at baseline: 9.0 points per hour
#define M_RESTFUL_PCT         120   // multiplier for restful (deep) sleep minutes, percent
#define M_G_REST              3     // awake, still, heart rate near resting: 1.8 points per hour

// Activity drain (d_a) = BASE + GAIN * intensity^2, intensity as a fraction of HR reserve
#define M_D_ACT_BASE          6
#define M_D_ACT_GAIN          70
#define M_STEPS_MOVING        20    // steps in a minute that count as moving
#define M_INTEN_ACTIVE_PCT    35    // HR reserve share that counts as exercise without steps...
#define M_VMC_MOVING          400   // ...when motion counts are at least this (cycling, weights)
#define M_COOLDOWN_MIN        10    // after moving, elevated HR is recovery, not stress

// Stress drain (d_s) = BASE + GAIN * stress level, counted only at low motion
#define M_D_STRESS_BASE       4
#define M_D_STRESS_GAIN       14
#define M_STRESS_HR_MARGIN    15    // bpm above resting HR before heart rate reads as stress
#define M_STRESS_HR_SPAN      35    // bpm over which HR stress goes from 0 to 1
#define M_STRESS_HRV_Q_PCT    80    // awake HRV below this share of the awake reference is stress
#define M_STRESS_HRV_SPAN     40    // percentage points over which HRV stress goes from 0 to 1

// HRV against the wearer's own overnight baseline
#define M_DAY_REF_PCT         75    // expected awake RMSSD as a share of the overnight baseline
#define M_HRV_FRESH_MIN       45    // a window older than this no longer steers the rates
#define M_HRV_MULT_MIN_PCT    60    // recharge multiplier at half of baseline or below
#define M_HRV_MULT_MAX_PCT    115   // recharge multiplier at 130% of baseline or above
#define M_MIN_BASE_NIGHTS     3     // nights with HRV before the baseline is trusted
#define M_MIN_NIGHT_WINDOWS   3     // HRV windows needed for a night to count

// Signal handling
#define M_HR_HOLD_MIN         15    // a heart-rate reading stands in for this many minutes
#define M_NOT_WORN_MIN        30    // no heart rate for this long: frozen and marked stale
#define M_MAIN_SLEEP_MIN      180   // asleep minutes that make a sleep the day's main sleep
#define M_SLEEP_MERGE_MIN     30    // a wake shorter than this continues the same sleep
#define M_RHR_PERCENTILE      10    // resting HR = this percentile of heart rate while asleep
#define M_RHR_MIN_SAMPLES     5

// Defaults and limits
#define M_DEFAULT_RHR         60
#define M_DEFAULT_MAX_HR      185
#define M_START_RESERVE100    6000  // first-run starting point before any night is seen
#define M_FIRST_LOOKBACK_MIN  (36 * 60)
#define M_MAX_REPLAY_MIN      (48 * 60)
#define M_RECENT_LAG_MIN      20    // newest minutes may not be in the history yet

// States (PRD proposed bands)
#define M_BAND_HIGH           76
#define M_BAND_MEDIUM         51
#define M_BAND_LOW            26
