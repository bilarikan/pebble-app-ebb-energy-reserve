# Scoring model

The reserve is a running balance from 0 to 100. It refills while you sleep and rest and
drains with exertion and stress. Everything is computed on the watch.

**Every number below is a placeholder** to be fitted during validation. None comes from
another vendor's method. The constants live in [`src/c/model.h`](../src/c/model.h) and the
logic in [`src/c/engine.c`](../src/c/engine.c); this page describes both.

```
R(t) = clamp[0,100]( R0 - sum over minutes of ( d_a + d_s - g ) )
```

The engine advances one minute at a time from the watch's own per-minute health history
(steps, motion counts, heart rate), the sleep sessions Pebble Health detected, and the HRV
windows the background sampler stored. Exactly one term applies in any minute.

## Inputs

| Input | Source | Notes |
| --- | --- | --- |
| Heart rate | Per-minute history, filtered BPM | A reading stands in for up to 15 minutes; the default cadence is one every 10 minutes |
| Steps, motion counts | Per-minute history | |
| Asleep, restful sleep | Pebble Health sleep sessions | No custom sleep staging |
| HRV | Background sampler, RMSSD per window | See below |
| Resting heart rate | 10th percentile of heart rate while asleep | Median over the last 14 nights |
| HRV baseline | Median overnight RMSSD over the last 14 nights | Needs 3 nights before it is used |

## HRV windows

PebbleOS (4.32 and later, Pebble Time 2) hands apps one beat-to-beat interval at a time, and
only while an app is asking for it. The sampler therefore works in bursts:

1. Wait until the wearer is asleep, or awake with no steps for 3 minutes, and off the charger.
2. Ask for HRV at a 1-second period. That keeps the optical sensor on for the whole window, so
   successive intervals come from neighbouring beats. Longer periods let the firmware
   switch the sensor off between readings.
3. Ignore the first 10 seconds while the signal settles. If nothing at all arrives in
   30 seconds, stop: the watch is probably off the wrist, and the next tries are spaced out.
4. Collect for 60 seconds (120 on High). Drop an interval outside 300 to 2000 ms, and any that
   differs from the one before it by more than 20%.
5. Keep the window only if at least half the expected beat pairs survived, no more than 25%
   of intervals were dropped, and (awake) no steps were taken.
6. Store `RMSSD = sqrt(mean(successive differences squared))`, then release the sensor by
   setting both the HRV and heart-rate sample periods to 0.

| Intensity | Asleep | Awake and still | Window |
| --- | --- | --- | --- |
| Low | every 20 min | every 60 min | 60 s |
| Balanced (default) | every 10 min | every 30 min | 60 s |
| High | every 5 min | every 15 min | 120 s |

The firmware also computes a 0 to 100 confidence for each interval, but SDK 4.44 does not
expose it to apps, so quality is judged from the intervals themselves.

## The four terms

Rates are in reserve points per hour. `i` is exercise intensity as a share of heart-rate
reserve: `(HR - resting) / (max - resting)`, 0 to 1.

| Term | Applies when | Rate |
| --- | --- | --- |
| Recharge, asleep | Pebble Health says asleep | `+9.0 x m`, and x 1.2 in restful sleep |
| Recharge, awake | Still, heart rate within 15 bpm of resting, HRV not low | `+1.8 x m` |
| Activity drain `d_a` | 20+ steps in the minute, or `i` at least 0.35 with clear motion | `-(3.6 + 42 x i^2)` |
| Stress drain `d_s` | Still, and heart rate or HRV says strained | `-(2.4 + 8.4 x s)` |

- **`m`, the HRV multiplier.** The latest window of the right kind (asleep or awake, at most
  45 minutes old) divided by its reference: the overnight baseline while asleep, 75% of it
  while awake. `m` runs from 0.6 at half the reference to 1.0 at the reference and 1.15 at
  130% or more. With no fresh window, or while calibrating, `m` is 1.
- **`s`, the stress level**, is the larger of two readings: heart rate more than 15 bpm above
  resting (reaching 1 at 50 bpm above), and awake HRV below 80% of its reference (reaching 1
  at 40%).
- **Stress is counted only when still**, and not for 10 minutes after moving, so exercise and
  the recovery after it are not charged twice.
- If heart rate is unknown while moving, intensity is estimated from step cadence.

## Morning charge (R0)

There is no separate formula. The reserve refills through the night at the asleep rate, steered
by each overnight HRV window, and R0 is simply its value when the main sleep (3 hours or more)
ends. A longer night gives more minutes of refill; HRV at or above baseline gives more per
minute. A wake of under 30 minutes continues the same sleep.

At that moment the night is recorded (sleep minutes, median overnight RMSSD, resting heart
rate) and the baseline is recomputed. A night needs at least 3 HRV windows to count toward the
HRV baseline.

## States

| Score | State |
| --- | --- |
| 76 to 100 | High |
| 51 to 75 | Medium |
| 26 to 50 | Low |
| 0 to 25 | Very low |

## When the app is guessing

| Situation | What happens | Shown as |
| --- | --- | --- |
| Fewer than 3 nights with HRV | Score runs on heart rate, sleep and motion only (`m` = 1) | "Calibrating: night n of 3", LOW CONF |
| No heart rate for 30 minutes and no steps | Score freezes; the curve shows a gap | "Not worn: score frozen", STALE |
| Worn again after a gap | Score resumes from the frozen value | "Resumed after a gap", LOW CONF until the next full night |
| A night with no usable HRV, once calibrated | Night is recorded without HRV | LOW CONF until the next night with HRV |
| More than 48 hours since the last update | The stretch is skipped, not replayed | STALE until worn |

Sleep on its own does not count as worn: Pebble's sleep detection is motion-based, and a
watch lying on a table can look asleep.

## The reason line

The engine keeps a running total of what each term actually changed, after clamping at 0 and
100, so the parts always add up to the score. The main screen shows the difference since you
last closed the app, for example `-8 activity, -3 stress, +5 rest`. The breakdown view shows
the same totals since the morning charge.

## Known limits

- How PebbleOS paces interval events is not documented. If it reports roughly one interval per
  second rather than one per beat, some beats are skipped at higher heart rates and RMSSD
  reads a little high. The effect is the same for the baseline and for each new window, and
  the model only uses their ratio, but absolute RMSSD should not be compared with a chest
  strap until this is checked on the wrist.
- Maximum heart rate is a setting (default 185), not measured.
- The motion-count threshold for "exercise without steps" is a guess.
- Agreement with a reference device has not been measured. See the PRD's validation plan.
