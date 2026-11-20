// Scoring model parameters. Every number here is a placeholder to be fitted during
// validation (PRD "Scoring model"); none comes from another vendor's method.
// docs/SCORING.md explains what each one does. Rates are in 1/100 reserve point per minute,
// as they apply at a reserve of 50.
#pragma once

// Every rate is scaled by the reserve itself, so the score settles where a day's cost and a
// night's gain balance instead of sticking at 0 or 100.
//   A recharge fills a share of what is missing:  rate x (100 - reserve) / 50
//   A drain takes a share of what is left, softened so the score keeps moving when it is
//   low:                                          rate x (reserve + 20) / 70
#define M_LEVEL_REF100        5000  // the reserve, in 1/100 point, at which the rates below apply
#define M_DRAIN_SOFT100       2000  // softening of the drain scaling, 1/100 point (0 = none):
                                    // drains run at 0.29x at a reserve of 0, 1.71x at 100
#define M_GAIN_SOFT100        0     // the same for recharge: 2x at 0, nothing at 100

// Recharge (g)
#define M_G_SLEEP             14    // asleep, HRV at baseline: 8.4 points per hour
#define M_RESTFUL_PCT         120   // multiplier for restful (deep) sleep minutes, percent
#define M_G_REST              6     // awake, still, heart rate at resting: 3.6 points per hour
// Sleep with a raised heart rate (alcohol, illness, a late workout, or not really asleep)
// refills more slowly. Needs a resting heart rate, so it starts after the first night.
#define M_SLEEP_HR_MARGIN     5     // bpm above resting HR that still counts as full-rate sleep
#define M_SLEEP_HR_SPAN       15    // bpm beyond the margin over which the rate falls...
#define M_SLEEP_HR_MIN_PCT    30    // ...to this share of the full rate

// Activity drain (d_a) = BASE + GAIN * intensity^2, intensity as a fraction of HR reserve
#define M_D_ACT_BASE          9     // walking at an easy heart rate: 5.4 points per hour
#define M_D_ACT_GAIN          70
#define M_STEPS_MOVING        20    // steps in a minute that count as moving
#define M_INTEN_ACTIVE_PCT    35    // HR reserve share that counts as exercise without steps...
#define M_VMC_MOVING          400   // ...when motion counts are at least this (cycling, weights)
#define M_COOLDOWN_MIN        10    // after moving, elevated HR is recovery, not stress
// On your feet without walking anywhere (housework, cooking, standing between short walks),
// and the minutes straight after moving. Needs no heart rate.
#define M_D_LIGHT             8     // 4.8 points per hour
#define M_VMC_LIGHT           600   // motion counts in a minute that count as on your feet

// Stress drain (d_s) = BASE + GAIN * stress level, counted only at low motion.
// Resting heart rate is measured asleep, so sitting awake is normally some way above it:
// being up costs a little, strain costs more.
#define M_D_STRESS_BASE       3
#define M_D_STRESS_GAIN       15
#define M_STRESS_HR_MARGIN    6     // bpm above resting HR that still counts as at rest
#define M_STRESS_HR_SPAN      34    // bpm over which HR stress goes from 0 to 1

// Not worn during the waking day: assume an ordinary day rather than no cost at all.
#define M_D_AWAY              5     // 3 points per hour at a reserve of 50
#define M_AWAY_DAY_MIN        (16 * 60)   // only this long after the last main wake
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
#define M_NOT_WORN_MIN        30    // no heart rate for this long: frozen and marked stale...
#define M_MOTION_HOLD_MIN     15    // ...unless the watch moved this recently (at most 15)...
#define M_MOTION_DENSE_MIN    5     // ...in at least this many of those minutes when awake: a wrist
                                    // moves most minutes, a watch on a desk only when buzzed or nudged
#define M_HR_GAP_MAX_MIN      120   // ...and the last heart rate is no older than this
#define M_LOW_CONF_GAP_MIN    60    // a frozen stretch shorter than this does not dent confidence
#define M_RESTART_GAP_MIN     (24 * 60)   // off the wrist this long: the old score is dropped
#define M_MAIN_SLEEP_MIN      180   // asleep minutes that make a sleep the day's main sleep
#define M_SLEEP_MERGE_MIN     30    // a wake shorter than this continues the same sleep
#define M_NIGHT_GAP_MIN       180   // back asleep within this long of the main wake: same night...
#define M_NIGHT_MORE_MIN      60    // ...if the later sleep lasts at least this long
#define M_RHR_PERCENTILE      10    // resting HR = this percentile of heart rate while asleep
#define M_RHR_MIN_SAMPLES     5

// Defaults and limits
#define M_DEFAULT_RHR         60
#define M_DEFAULT_MAX_HR      185
#define M_START_RESERVE100    4000  // first-run starting point before any night is seen
#define M_FIRST_LOOKBACK_MIN  (36 * 60)
#define M_MAX_REPLAY_MIN      (48 * 60)
#define M_RECENT_LAG_MIN      20    // newest minutes may not be in the history yet

// States (PRD proposed bands)
#define M_BAND_HIGH           76
#define M_BAND_MEDIUM         51
#define M_BAND_LOW            26
