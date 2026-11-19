# Scoring model

The reserve is a running balance from 0 to 100. It refills while you sleep and rest and
drains with exertion and stress, and both slow down as it nears the end they are heading
for. Everything is computed on the watch.

**Every number below is a placeholder** to be fitted during validation. None comes from
another vendor's method. The constants live in [`src/c/model.h`](../src/c/model.h) and the
logic in [`src/c/engine.c`](../src/c/engine.c); this page describes both.

```
each minute:  R = R - d x (R + 20) / 70   when the minute's term is a drain d
              R = R + g x (100 - R) / 50  when it is a recharge g
```

`d` and `g` are the rates in the tables below, which are therefore the rates at a reserve of
50. See [Scaling with the reserve](#scaling-with-the-reserve).

The engine advances one minute at a time from the watch's own per-minute health history
(steps, motion counts, heart rate), the sleep sessions Pebble Health detected, and the HRV
windows the background sampler stored. At most one term applies in any minute.

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

## The terms

Rates are in reserve points per hour at a reserve of 50. `i` is exercise intensity as a
share of heart-rate reserve: `(HR - resting) / (max - resting)`, 0 to 1. The first row that
matches a minute is the one that applies.

| Term | Applies when | Rate |
| --- | --- | --- |
| Recharge, asleep | Pebble Health says asleep | `+7.2 x q`, and x 1.2 in restful sleep |
| Activity drain `d_a` | 20+ steps in the minute, or `i` at least 0.35 with clear motion | `-(5.4 + 42 x i^2)` |
| Light movement (counted as activity) | 600+ motion counts in the minute, or within 10 minutes of the row above | `-4.8` |
| Stress drain `d_s` | Still, and heart rate more than 6 bpm above resting, or HRV low | `-(1.8 + 9.0 x s)` |
| Recharge, awake | Still, heart rate within 6 bpm of resting, HRV not low | `+3.6 x m` |

A still minute with no heart-rate reading in the last 15 gets no term.

Resting heart rate is measured asleep, and sitting awake is normally 8 to 15 bpm above it,
so at a reserve of 50 a quiet hour at a desk costs 2 to 4 points; an hour on your feet about
5; an hour's easy walking 7 to 9; a hard hour's exercise 25 or more. Recharging while awake
takes a heart rate close to its sleeping level, which mostly means lying down.

- **`m`, the HRV multiplier.** The latest window of the right kind (asleep or awake, at most
  45 minutes old) divided by its reference: the overnight baseline while asleep, 75% of it
  while awake. `m` runs from 0.6 at half the reference to 1.0 at the reference and 1.15 at
  130% or more. With no fresh window, or while calibrating, `m` is 1.
- **`q`, sleep quality**, is `m` unless heart rate says otherwise. Sleeping heart rate within
  5 bpm of resting counts in full; above that the rate falls, reaching 30% at 20 bpm above.
  `q` is the lower of that and `m`: both read the same thing, so they are not multiplied.
  This needs a resting heart rate, so it starts after the first night; it does not wait for
  the HRV baseline.
- **`s`, the stress level**, is the larger of two readings: heart rate more than 6 bpm above
  resting (reaching 1 at 40 bpm above), and awake HRV below 80% of its reference (reaching 1
  at 40%).
- **Stress is counted only when still**, and not for 10 minutes after moving, so a heart rate
  raised by movement is not read as strain. Those minutes cost the flat light-movement rate.
- **Light movement needs no heart rate.** Housework, cooking and standing about show up as
  motion counts with few steps, and heart-rate readings are sparse then (one every 10
  minutes or so).
- If heart rate is unknown while moving, intensity is estimated from step cadence.

## Scaling with the reserve

Every rate is multiplied by how far the reserve is from the end it is moving towards.

- **A recharge fills a share of what is missing:** `(100 - R) / 50`.
- **A drain takes a share of what is left, softened:** `(R + 20) / 70`. Straight proportion
  (`R / 50`) made the score crawl once it was low; the 20 keeps it moving.

| Reserve | Drains run at | Recharge runs at |
| --- | --- | --- |
| 10 | 0.43 x | 1.8 x |
| 30 | 0.71 x | 1.4 x |
| 50 | 1 x | 1 x |
| 70 | 1.29 x | 0.6 x |
| 90 | 1.57 x | 0.2 x |

What follows from it:

- **The score settles where a day's cost and a night's gain balance.** With fixed rates it
  had nothing to stop it: a wearer whose days cost more than their nights gave back sat at 0
  every evening (the second day in [DEVELOPMENT.md](DEVELOPMENT.md)).
- **It slows towards 0 and 100.** The top takes many hours of recharge to reach. The bottom
  can be reached, but only on a day far heavier than the nights around it.
- **A wrong starting value fades** within a day or two, because both runs are pulled to the
  same balance.

Where it settles, by the table rates added up over a day and a night:

| Day costs | Night gives | Evening low | Morning high |
| --- | --- | --- | --- |
| 84 (a workout, a commute, an active evening) | 46 (a broken 7 hours) | about 2 | about 58 |
| 55 | 60 (a calm 8 hours) | about 24 | about 77 |
| 35 (a quiet day) | 65 (a long calm night) | about 44 | about 85 |

The first row is the wearer's heaviest recorded day repeated; the others are worked out from
the formula. Mornings above 90 would need several easy days and long nights in a row.

## Morning charge (R0)

There is no separate formula. The reserve refills through the night at the asleep rate, steered
by heart rate and each overnight HRV window, and R0 is simply its value when the night's
sleep ends. A longer night gives more minutes of refill; a heart rate near resting and HRV at
or above baseline give more per minute; and a lower starting point gives more, because
recharge scales with what is missing. A full-quality 8-hour night takes a reserve of 20 to
about 76, and one of 60 to about 88.

What counts as one night:

- A main sleep is 3 hours or more.
- A wake of under 30 minutes continues the same sleep.
- A longer wake, followed within 3 hours by at least another hour of sleep, is still the same
  night. The minutes are added, the HRV windows of both parts are used, and the morning
  charge is taken at the final wake.

**Sleep is labelled late.** Pebble Health decides that a stretch was sleep an hour or more
after it began. The app scores up to the present whenever it is open, so it may already have
scored those minutes as awake. It therefore keeps the engine state from at least four hours
back and the sleep label each minute since was scored with; when a label changes, it rebuilds
the score from that state. Opening the app during the night does not cost the sleep around
it. A label that changes more than four hours after the fact is not picked up.

At each such wake the night is recorded (sleep minutes, median overnight RMSSD, resting heart
rate) and the baseline is recomputed. There is one record per night. A night needs at least
3 HRV windows to count toward the HRV baseline.

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
| No heart rate for 30 minutes, no steps, and too little motion: awake, fewer than 5 of the last 15 minutes with any; asleep, none | Counted as not worn; the curve shows a gap | See the next two rows |
| No heart rate for 2 hours and no steps | Counted as not worn even if the watch is moving | See the next two rows |
| Not worn, within 16 hours of the last main wake, and Pebble Health does not call the minute asleep | Score drains at 3 points per hour (at a reserve of 50), roughly an ordinary day | "Not worn: estimating", STALE |
| Not worn at any other time (a night off the wrist, before the first night, a watch in a drawer) | Score freezes | "Not worn: score frozen", STALE |
| Worn again after an hour or more off | Score resumes from where it was | "Resumed after a gap", LOW CONF until the next full night |
| Worn again after a day or more off | Score restarts at 40, the first-run value; the baseline is kept | "Restarted after time off", LOW CONF until the next full night |
| Worn again after a shorter time off | Score resumes from where it was | Nothing |
| A night with no usable HRV, once calibrated | Night is recorded without HRV | LOW CONF until the next night with HRV |
| More than 48 hours since the app last ran | The last 48 hours are replayed; the stretch before them is skipped and counts as time off the wrist, so a day or more of it restarts the score | As for time off of that length |

Sleep on its own does not count as worn: Pebble's sleep detection is motion-based, and a
watch lying on a table can look asleep. Motion counts tell the two apart, which matters
because heart-rate records can pause for half an hour or more on a wrist. Awake, a wrist
moves in most minutes, while a watch on a desk moves only when it buzzes or is nudged, so
one stray movement does not count: it takes 5 minutes with motion in the last 15. Asleep, a
wrist is still most of the time, so any movement in the last 15 minutes counts.

The notice appears 30 to 50 minutes after the watch comes off: 30 for the heart-rate hold,
and up to 20 more because the watch writes its history with a delay.

The first-run starting point is 40. It is arbitrary, and an error in it fades within a day
or two. The same value is used after a day or more off the wrist: the nights slept without
the watch were never seen, so the old score says nothing about now.

## The reason line

The engine keeps a running total of what each term actually changed, after scaling, so the
parts always add up to the score. The main screen shows the difference since you
last closed the app, for example `-8 activity, -3 stress, +5 rest`. Points assumed while the
watch was off the wrist are listed separately as `off wrist`. The breakdown view shows the
same totals since the morning charge.

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
- **The rates are fitted to three nights and two days on one wrist** (see
  [DEVELOPMENT.md](DEVELOPMENT.md)), against one reference device and the wearer's own
  judgement, which did not always agree with each other. A quiet desk day and a long calm
  night have not been recorded yet.
- **The top of the scale is hard to reach.** Recharge slows as the reserve fills, so a
  wearer used to seeing 100 after a good night elsewhere will see 75 to 85 here.
- **The scaling is a simple one that balances**: straight proportion for recharge, softened
  proportion for drains. Nothing says the body follows it. The softening was set by how the
  score felt to the wearer over two further days (too slow to fall once low), not by a
  measurement.
- **Quiet rest counts for less than a reference device gives it.** Lying awake with a heart
  rate at its sleeping level recharges at half the sleep rate. One reference reading rose 15
  points over 2 hours 45 minutes of that; this model gives about 10 at a reserve of 50.
- **The sensor sometimes reports a heart rate with the watch off the wrist.** One desk
  stretch held readings of 119 to 122 with no motion at all, which was scored as 38 minutes
  of strain (about 4 points). The app cannot see the sensor's confidence, and ignoring
  motionless readings would also ignore a wearer lying still, so they are accepted.
- **Time off the wrist is a guess.** The app cannot know whether the wearer sat or ran. It
  assumes an ordinary day and says so.
- **The label "stress" covers plain sitting.** Any still, awake minute with a heart rate
  above the resting band is booked under stress, including the small cost of being up.
- **The reference uses things this app does not have.** Its method takes age, sex, height,
  weight and an activity class to estimate maximum heart rate and fitness, which scale how
  much a given heart rate costs. Here the only personal inputs are the maximum heart rate
  setting and what the watch measures (resting heart rate, HRV baseline).
- Maximum heart rate is a setting (default 185), not measured.
- The motion-count thresholds for "exercise without steps" and for light movement are
  guesses; the second was picked from one day's record (asleep and seated minutes mostly
  under 300 counts, minutes on the feet mostly over 600).
- Agreement with a reference device has not been measured. See the PRD's validation plan.
