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
3. Wait for the first interval. From cold the sensor takes about 20 seconds to produce one.
   If nothing arrives in 45 seconds, stop: the watch is probably off the wrist, and the next
   tries are spaced out.
4. Ignore the next 15 seconds. On the wrist the first ten or so intervals were unusable.
5. Collect for 60 seconds (120 on High). Skip an interval identical to the one before it
   (the sensor repeats a value when it has nothing new). Drop an interval outside 300 to
   2000 ms, and any that differs from the one before it by more than 20%.
6. Keep the window only if at least half the expected beat pairs survived, no more than 25%
   of intervals were repeats or dropped, and (awake) no steps were taken.
7. Store `RMSSD = sqrt(mean(successive differences squared))`, then release the sensor by
   setting both the HRV and heart-rate sample periods to 0.

A burst therefore holds the sensor on for roughly 80 to 100 seconds (140 to 160 on High).

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
| Recharge, asleep | Pebble Health says asleep | `+6.6 x q`, and x 1.2 in restful sleep |
| Recharge, awake | Still, heart rate within 15 bpm of resting, HRV not low | `+1.8 x m` |
| Activity drain `d_a` | 20+ steps in the minute, or `i` at least 0.35 with clear motion | `-(3.6 + 42 x i^2)` |
| Stress drain `d_s` | Still, and heart rate or HRV says strained | `-(2.4 + 8.4 x s)` |

- **`m`, the HRV multiplier.** The latest window of the right kind (asleep or awake, at most
  45 minutes old) divided by its reference: the overnight baseline while asleep, 75% of it
  while awake. `m` runs from 0.6 at half the reference to 1.0 at the reference and 1.15 at
  130% or more. With no fresh window, or while calibrating, `m` is 1.
- **`q`, sleep quality**, is `m` unless heart rate says otherwise. Sleeping heart rate within
  5 bpm of resting counts in full; above that the rate falls, reaching 30% at 20 bpm above.
  `q` is the lower of that and `m`: both read the same thing, so they are not multiplied.
  This needs a resting heart rate, so it starts after the first night; it does not wait for
  the HRV baseline.
- **`s`, the stress level**, is the larger of two readings: heart rate more than 15 bpm above
  resting (reaching 1 at 50 bpm above), and awake HRV below 80% of its reference (reaching 1
  at 40%).
- **Stress is counted only when still**, and not for 10 minutes after moving, so exercise and
  the recovery after it are not charged twice.
- If heart rate is unknown while moving, intensity is estimated from step cadence.

## Morning charge (R0)

There is no separate formula. The reserve refills through the night at the asleep rate, steered
by heart rate and each overnight HRV window, and R0 is simply its value when the main sleep
(3 hours or more) ends. A longer night gives more minutes of refill; a heart rate near resting
and HRV at or above baseline give more per minute. A full-quality 8-hour night adds roughly 55
points. A wake of under 30 minutes continues the same sleep.

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
| No heart rate for 30 minutes, no steps, and no motion in the last 15 | Score freezes; the curve shows a gap | "Not worn: score frozen", STALE |
| No heart rate for 2 hours and no steps | Score freezes even if the watch is moving | "Not worn: score frozen", STALE |
| Worn again after a freeze of an hour or more | Score resumes from the frozen value | "Resumed after a gap", LOW CONF until the next full night |
| Worn again after a shorter freeze | Score resumes from the frozen value | Nothing |
| A night with no usable HRV, once calibrated | Night is recorded without HRV | LOW CONF until the next night with HRV |
| More than 48 hours since the last update | The stretch is skipped, not replayed | STALE until worn |

Sleep on its own does not count as worn: Pebble's sleep detection is motion-based, and a
watch lying on a table can look asleep. Motion counts tell the two apart: a table gives none
at all, and a wrist gives some even when still. That matters because heart-rate records can
pause for half an hour or more on a wrist.

The first-run starting point is 40. It is arbitrary, and an offset in it lasts until the
score next reaches 0 or 100.

## The reason line

The engine keeps a running total of what each term actually changed, after clamping at 0 and
100, so the parts always add up to the score. The main screen shows the difference since you
last closed the app, for example `-8 activity, -3 stress, +5 rest`. The breakdown view shows
the same totals since the morning charge.

## Known limits

- **Interval quality varies a lot, and the app cannot see the sensor's own confidence.** The
  driver in PebbleOS 4.44.4 passes on beat-to-beat intervals from the sensor vendor's
  algorithm, a few at a time. Two samples taken minutes apart on the same wrist (14 Nov 2026)
  show the range: one was unusable (intervals from 250 to 1600 ms at a heart rate of 74, with
  runs of repeated values) and was rejected; the next was clean after its warm-up and gave an
  RMSSD of 64.1 ms from 58 beat pairs. The filter above is the only defence. How often awake
  windows survive it, and whether the kept ones agree with a chest strap, is not known yet.
- A single wild interval that slips under the 20% rule still moves RMSSD noticeably in a
  60-second window. The nightly figure is a median over many windows for that reason; an
  awake window stands alone.
- **Pebble Health can call a still watch asleep.** After two hours on a desk the watch
  reported sleep, and kept reporting it for a while once worn. The engine ignores sleep
  without heart rate, but minutes worn-and-still straight after that are treated as sleep.
- **Sleep recharge is fitted to one night against one reference** (see the first night in
  [DEVELOPMENT.md](DEVELOPMENT.md)). The reference gained about 50 points; this model gives 57 for the same night.
- **Daytime drain has not been compared with anything.** If the score sits high all day while
  a reference falls, the awake terms are the next thing to fit.
- **The reference uses things this app does not have.** Its method takes age, sex, height,
  weight and an activity class to estimate maximum heart rate and fitness, which scale how
  much a given heart rate costs. Here the only personal inputs are the maximum heart rate
  setting and what the watch measures (resting heart rate, HRV baseline).
- Maximum heart rate is a setting (default 185), not measured.
- The motion-count threshold for "exercise without steps" is a guess.
- Agreement with a reference device has not been measured. See the PRD's validation plan.
