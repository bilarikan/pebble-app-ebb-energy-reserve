# Development notes

Working detail for anyone changing the code, human or coding agent: where the project stands,
how it is put together, and what the first days on a wrist showed. The user-facing summary
is the [README](../README.md) and the [user guide](USER-GUIDE.md); the scoring model is in
[SCORING.md](SCORING.md). The user guide describes the screens, messages and settings, so
update it when any of those change.

## Status

One night and one day recorded on a Pebble Time 2 (PebbleOS 4.44.4). The scoring engine is
unit-tested, the background worker sampled HRV through a night, and the app scored that night
and day from the watch's own history. The sleep recharge and then the daytime drain were
retuned after comparing them with Garmin Body Battery readings. Not yet measured: hard
exercise and a quiet day against a reference, battery cost, and the baseline (it needs three
nights).

Version 0.2.1 is on the Pebble app store as
[Ebb: Body Energy Reserve](https://apps.repebble.com/afae45fe97914bc5b35d3711), marked as an
early release. To publish an update, raise `version` in `package.json`, build, and run
`pebble publish`; the icons and screenshots used for the listing are in `store/`.

"Ebb" is a working name. The PRD leaves naming open and rules out "Body Battery" and
"Energy Reserve"; changing it is one line (`displayName` in `package.json`).

## Emulator, debug builds and tests

The emulator has no health history, so a normal build shows a frozen score there. For the
emulator, build the demo variant, which feeds the same engine a synthetic day:

```bash
ER_DEMO=1 pebble build && pebble install --emulator emery
```

Run `pebble wipe` when switching between demo and normal builds in the emulator, because the
demo's saved state would otherwise carry over.

For debugging on the wrist, `ER_DEBUG=1 pebble build` makes the app log what the score was
built from twelve seconds after every launch (state, nights, sleep sessions, the 24-hour
curve, every stored HRV window, an hourly summary of the health history, and the last 26
hours minute by minute), then start an HRV measurement and log the raw beat intervals. The
minute lines (`M`) are enough to replay a day through `engine.c` on a computer, which is how
the daytime rates were fitted. Read it with `--logs` straight after
installing; set `PYTHONUNBUFFERED=1` when sending the tool's output to a file. In any build,
a long press on Select asks the sampler for a measurement now instead of waiting for its
schedule.

Engine tests run on the host and need no SDK:

```bash
make -C tests
```

The build prints a warning that `package.json` declares no `usesPermissions`. That is
intended: under manifest version 2 it means the phone-side JavaScript cannot make network
requests.

## Layout

| Path | Role |
| --- | --- |
| `worker_src/c/worker.c` | Background HRV sampler (FR1). 2.3 KiB of the worker's 10 KiB |
| `src/c/engine.c`, `engine.h` | Score engine (FR3). Pure C, no Pebble calls |
| `src/c/model.h` | Every tunable number in the model |
| `src/c/platform.c` | The one place that talks to health, storage and worker APIs |
| `src/c/shared.h` | Storage keys and record formats shared with the worker |
| `src/c/ui_main.c`, `ui_detail.c`, `ui_onboard.c` | Main screen, breakdown, first-launch notice |
| `src/c/comm.c`, `src/pkjs/` | Settings page and baseline backup |
| `src/c/demo_data.c` | Synthetic data, compiled only with `ER_DEMO` |
| `tests/` | Host-side engine tests |

How the pieces fit: the worker only samples. It appends one RMSSD value per burst to a ring
in persistent storage. The app does the scoring by replaying the watch's own per-minute
health history from where it last stopped, so the answer is the same whether the app was open
all day or is catching up after hours away.

## PRD coverage

| ID | Requirement | State |
| --- | --- | --- |
| FR1 | Burst sampler that always releases the sensor | Built; first windows taken on a watch |
| FR2 | Store HRV windows, resting HR, timestamps for days | Built: 210 windows (about 2.5 days) and 14 nightly summaries |
| FR3 | Engine recomputes on a fixed interval | Built: every minute while open, and a full catch-up on launch |
| FR4 | Score, state, curve, updated time, confidence flag | Built |
| FR5 | Not worn: freeze, mark stale, resume low confidence | Built and tested. One departure: in the waking day, time off the wrist drains at an assumed 3 points per hour instead of freezing (see the first day below). After a day or more off, the score restarts at the first-run value instead of resuming |
| FR6 | Onboarding: estimate, not medical advice | Built |
| FR7 | Reason line | Built |
| FR8 | Breakdown view | Built |
| FR10 | Settings: units, intensity, reset baseline | Built; "units" is read as HRV units (ms or ln) |
| FR9, FR11 to FR13 | Launcher glance, watchface, breathing, CSV export | Deferred, as in the PRD |

Also in place: a small versioned score record for a later watchface (`PK_SCORE`), the baseline
backed up to the phone and restored if the watch loses it, a firmware check at launch, and a
notice when another app's worker holds the single background slot.

Storage used is about 2 KiB of the 4 KiB each app gets.

## First wear (14 Nov 2026)

Pebble Time 2, PebbleOS 4.44.4, about ten minutes on the wrist.

| Question | Answer |
| --- | --- |
| Can a background worker request HRV? | Yes. The firmware accepted the request and delivered intervals to the worker |
| How long until the first interval? | 22 s from cold, 6 to 7 s when the sensor had just been on |
| Are the intervals per beat? | Yes, about one per heartbeat, but with repeated values and a rough first ten or so |
| Does the filter cope? | One window rejected (29 pairs kept, 42 intervals rejected), one kept (58 pairs, 12 rejected or repeated, RMSSD 64.1 ms) |
| Does history replay work? | Yes: 2159 minutes, 1812 records, 157 with heart rate |
| Off the wrist? | The sensor reports it; the app showed "Not worn: score frozen" while Pebble Health called the same two hours sleep |

Three bugs found and fixed this way: the catch-up stalled when recent minutes were missing
but later ones existed; the burst gave up after 30 s, before a cold sensor's first interval;
and repeated intervals were counted as clean beat pairs. One firmware quirk worth knowing:
`health_service_get_minute_history` returned nothing for a start time that was not on a
whole minute.

## First night (14 to 15 Nov 2026)

Same watch, worn from mid-afternoon. A Garmin Instinct Solar was worn on and off for its Body
Battery reading. Two drinks in the evening.

| Question | Answer |
| --- | --- |
| Does the worker sample HRV overnight? | Yes. 41 windows were kept between 21:35 and 07:00, one every 10 minutes; 31 of the 34 bursts after midnight gave a window |
| What did the night look like? | Asleep 20:28 to 07:08 by Pebble Health, with a wake around 01:30 that the wearer confirms (607 minutes, 119 restful). Median RMSSD 32.2 ms, resting heart rate 62 |
| Did the drinks show in the signals? | Yes. From 21:45 to 22:15 RMSSD was 12 to 19 ms with heart rate in the 70s; from 03:15 to 05:50 it was 36 to 71 ms with heart rate 61 to 67 |
| Sensor time | 46 minutes between midnight and 07:46, about 80 s per burst |
| Battery | No drop the app could measure over a 7-hour stretch; the gauge is coarse, so this is not a figure |

Against the reference, the first version over-credited sleep:

| | Garmin Body Battery | Ebb as first installed | Ebb after retuning (same night replayed on the watch) |
| --- | --- | --- | --- |
| Evening, before sleep | about 30 | 40 | 40 |
| 01:30 | about 50 | 89 | 65 |
| 07:00 | about 80 | 100 (pinned from 03:00) | 97 |
| Gain over the night | about +50 | +58 before hitting 100; about +95 had there been room | +57 |

What changed, and why:

- **Sleep recharge 9.0 to 6.6 points per hour.** The reference gained about 5 points per hour
  of the same sleep.
- **Sleep at a raised heart rate recharges more slowly**, down to 30% at 20 bpm above resting.
  The first version treated every sleeping minute alike until the HRV baseline exists (three
  nights), so sleep at 78 bpm early in the night counted the same as sleep at 61 bpm later.
- **A still wrist is no longer taken for an empty one.** Heart-rate records paused for 30
  minutes or more while the watch was worn, which froze the score and marked it low confidence
  for the rest of the day. Recent motion now bridges such a pause, and a freeze of under an
  hour no longer lowers confidence.
- **First-run starting point 60 to 40.** It is arbitrary either way; 40 leaves room for the
  first night.

One night against one reference, itself new to the wrist, is thin evidence. The remaining
difference is mostly the two devices' starting points.

## First day (15 Nov 2026)

The day after that night. Both watches worn until 12:09, both off until about 16:30. The
wearer judged the Garmin figure the closer one to how they felt at all three readings.

| | Garmin Body Battery | Ebb 0.1.0 as worn | Ebb 0.2.0 (same record replayed on the watch) |
| --- | --- | --- | --- |
| 07:00, waking | about 80 | 97 (100 on the build worn overnight) | 80 |
| 09:27, after chatting and housework | 67 | 95 | 72 |
| 12:09, after breakfast out, a walk and window shopping | 49 | 85 | 54 |
| 16:30, after 4 hours 20 minutes off the wrist | 34 | 84 | 41 |

The minute record showed where the drain went missing. Of the 301 minutes between waking and
12:09:

| What the first version did | Minutes | Why |
| --- | --- | --- |
| Charged nothing | 94 | Within 10 minutes of walking: meant to stop a raised heart rate being read as stress |
| Charged nothing | 58 | No heart-rate reading in the last 15 minutes. Readings come about every 10 minutes when still, and paused once for 74 minutes |
| Activity | 87 | 20 or more steps in the minute |
| Stress or rest | 62 | Still, with a reading. Heart rate was 75 to 94 against a resting 62; anything up to 77 counted as recharging |

Most of the uncharged minutes had plenty of motion and few steps: housework, and standing
between short walks.

What changed, and why:

- **Light movement is a term of its own**, 4.8 points per hour, for minutes with 600 or more
  motion counts and for the 10 minutes after walking. It needs no heart rate.
- **Walking costs more at the low end:** the base rate went from 3.6 to 5.4 points per hour.
  The intensity part is unchanged.
- **Sitting awake is no longer a recharge.** The resting band went from 15 bpm above resting
  heart rate to 6, and the stress drain now starts there at 1.8 points per hour, reaching
  10.8 at 40 bpm above. Resting heart rate is measured asleep, so the old band took in
  ordinary sitting.
- **Time off the wrist in the waking day drains at 3 points per hour** instead of freezing,
  shown as "Not worn: estimating" and listed as "off wrist". The reference lost 15 points
  over the same stretch. It applies for 16 hours after the last main wake and never to a
  minute Pebble Health calls asleep, so a night off the wrist still freezes.

The same change re-scored the evening before: 27 at bedtime (reference about 30), 51 at
01:30 (about 50) and 80 on waking (about 80). Part of that agreement is luck, because the
first-run starting point of 40 is arbitrary.

This is one morning of light activity against one reference. It says nothing yet about hard
exercise, a seated day or an evening.

## Not done, or not known

- **Daytime drain is fitted to one morning.** Hard exercise, a seated day and an evening have
  not been compared with anything.
- **The HRV baseline has one night of three.** Until then HRV does not steer the rates.
- **Battery:** the app reports sensor-on minutes (measured) and whole-watch drain per day
  (measured between launches). It does not convert sensor minutes into a percentage, because
  the sensor's current draw has not been measured.
- **Baseline needs the app opened at least every two days or so.** Nights are summarised
  when the app next runs; if the ring of windows has wrapped by then, that night counts
  without HRV.
- Interval pacing, the motion threshold and every weight are open; see
  [SCORING.md](SCORING.md).
