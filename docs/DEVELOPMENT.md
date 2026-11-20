# Development notes

Working detail for anyone changing the code, human or coding agent: where the project stands,
how it is put together, and what the first days on a wrist showed. The user-facing summary
is the [README](../README.md) and the [user guide](USER-GUIDE.md); the scoring model is in
[SCORING.md](SCORING.md). The user guide describes the screens, messages and settings, so
update it when any of those change.

## Status

Six nights and five days recorded on a Pebble Time 2 (PebbleOS 4.44.4, then 4.47.2). The scoring engine
is unit-tested, and the background worker samples HRV and scores from the watch's own
history every 15 minutes; the app shows the result. The sleep recharge, then the daytime drain, then the shape
of the model (rates that scale with the reserve) were changed after comparing the score with
Garmin Body Battery readings and with how the wearer felt. Not yet recorded: a quiet seated
day, a long calm night, and battery cost.

Version 0.5.2 is on the Pebble app store as
[Ebb: Body Energy Reserve](https://apps.repebble.com/afae45fe97914bc5b35d3711), marked as an
early release. The icons and screenshots used for the listing are in `store/`.

To publish an update: run the tests; check the worker's size; raise `version` in
`package.json` and the version line in `USER-GUIDE.md`; build; install that build on a watch
and check `worker scored` in its log shows sleep sessions (not 255); then
`pebble publish --non-interactive --pbw <file> --is-published --release-notes "..."`, which
finds the listing by the app's UUID; and change the version in the line above.

The launcher icon (`resources/images/menu_icon.png`, 25 by 25) is drawn with a black outline
and a white and green fill. The first one was plain white on transparent and could only be
seen on the highlighted row of the watch's app menu. The launcher shows the green as grey.

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
curve, every stored HRV window, an hourly summary of the health history, and the last 46
hours minute by minute), then start an HRV measurement. The
minute lines (`M`) are enough to replay a day through `engine.c` on a computer, which is how
the rates were fitted. One trap: the sleep flag in those lines is only right for the most
recent night, because the watch's sleep-session query returned only the latest sessions.
After the dump the debug build flips one remembered sleep label, which makes the next
run rebuild the last four hours from the anchor; the `shown:` log line after it should match
the one before, give or take the minute that passed. Reinstall the normal build afterwards;
the debug build is not the store build.

The worker logs nothing in these builds, because it has no memory to spare for the strings.
`ER_WLOG=1` adds its burst and scoring lines and `ER_PPI=1` every raw beat interval. Either
leaves the worker too little memory to score, so it reports that and the app scores instead:
use them to watch the sampler. Every `worker scored` line in the app's log carries the
worker's free heap before the run and the number of sleep sessions it was given (255: it
abandoned the run). Read it with `--logs` straight after
installing; set `PYTHONUNBUFFERED=1` when sending the tool's output to a file. In any build,
a long press on Select asks the sampler for a measurement now instead of waiting for its
schedule.

### Replaying a record

The rates were fitted by replaying the watch's own minute record through `engine.c` on a
computer, with one constant or another changed, and comparing the result with what the
wearer reported. `tools/replay/` does this:

```bash
python3 tools/replay/parse_log.py watch.log data/
MARKS="19 20:30,20 05:06" tools/replay/run.sh data/ "19 06:00" 50 M_G_SLEEP=12
```

- **`parse_log.py`** turns the log of an `ER_DEBUG` build into the minute record, HRV
  windows, baseline nights and the watch's own 24-hour curve.
- **`run.sh DATADIR "DD HH:MM" RESERVE [M_NAME=value ...]`** starts the engine at that local
  time with that reserve and the baseline as it stood, and prints the score each hour (or at
  `MARKS`) beside what the watch showed. Constants named on the command line are changed in a
  throwaway copy of `model.h`.
- **Take the start from the watch's curve,** and start after the last night whose sleep
  flags are missing (see the trap above).
- **Check it first with no changes:** it should reproduce what the watch showed. It runs
  `engine.c` only, so late sleep labels and time the app was closed are not modelled.
- **The log is the wearer's health record.** Keep it and the parsed files out of the
  repository; `.local/` is ignored for this.

### Tests

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
| `worker_src/c/worker.c` | Background HRV sampler (FR1) and the scoring schedule. With the two files below it is 7.8 KiB of the worker's 10 KiB |
| `worker_src/c/engine_w.c`, `score_w.c` | One-line wrappers that build `engine.c` and `score.c` into the worker |
| `src/c/engine.c`, `engine.h` | Score engine (FR3). Pure C, no Pebble calls |
| `src/c/score.c`, `score.h` | Scoring core: reads the watch's history, runs the engine, stores the result. Built into the worker and the app |
| `src/c/model.h` | Every tunable number in the model |
| `src/c/platform.c` | App only: settings, the worker's status, battery tracking, diagnostics |
| `src/c/shared.h` | Storage keys and record formats shared with the worker |
| `src/c/ui_main.c`, `ui_detail.c`, `ui_onboard.c` | Main screen, breakdown, first-launch notice |
| `src/c/comm.c`, `src/pkjs/` | Settings page and baseline backup |
| `src/c/demo_data.c` | Synthetic data, compiled only with `ER_DEMO` |
| `tests/` | Host-side engine tests |
| `tools/replay/` | Replays a watch log through the engine on a computer, for fitting the rates |

How the pieces fit:

- **The worker samples and scores.** It appends one RMSSD value per burst to a ring in
  persistent storage, and every 15 minutes (and whenever the app asks) it brings the score up
  to date by replaying the watch's per-minute health history from where it last stopped.
- **The app shows what is stored.** Each minute while open it asks the worker to score and
  reloads the result. It scores for itself, with the same code, only when no worker is
  running, when the worker reports that it could not do the run, and in `ER_DEMO` builds.
- **Nothing is carried in memory between runs.** A run starts from what is stored and ends
  by storing what it produced, so either process can do the next one.
- **Late sleep labels.** `score.c` stores two older copies of the engine state (`PK_ANCHOR`,
  `PK_NEXT`) and the sleep label each recent minute was scored with (`PK_SETTLE`), and
  rebuilds from the anchor when a label changes. See "Sleep is labelled late" in
  [SCORING.md](SCORING.md).

### The worker's memory

A worker has 10,240 bytes for code, data and heap together. Two things eat it:

- **The code.** `arm-none-eabi-size build/emery/pebble-worker.elf` must stay under about
  7.9 KiB; it is 7,789 bytes. That leaves a heap of 2,428.
- **The firmware's health cache: 2,052 bytes of the worker's own heap** (the same on
  PebbleOS 4.44.4 and 4.47.2), allocated whenever
  the process subscribes to health events, reads sleep sessions, or sums a health metric,
  and freed only by `health_service_events_unsubscribe()`. With less than that free,
  `health_service_activities_iterate` returns no sessions and no error.

So a run is in two parts that never hold memory together. `score_prepare()` reads the sleep
sessions into 96 bytes and has the cache released; then the engine (436 bytes), a 15-minute
history buffer, one ring page and the label store are allocated, used and freed. The worker
subscribes to health events only while a burst is running, and a burst goes without events
for the length of a run.

If the sessions cannot be read, the run is abandoned and nothing is stored: scoring on would
turn the night into waking hours. That is what the first version did (score 51 to 38).

## PRD coverage

| ID | Requirement | State |
| --- | --- | --- |
| FR1 | Burst sampler that always releases the sensor | Built; first windows taken on a watch |
| FR2 | Store HRV windows, resting HR, timestamps for days | Built: 210 windows (about 2.5 days) and 14 nightly summaries |
| FR3 | Engine recomputes on a fixed interval | Built: every 15 minutes in the background worker, every minute while the app is open |
| FR4 | Score, state, curve, updated time, confidence flag | Built |
| FR5 | Not worn: freeze, mark stale, resume low confidence | Built and tested. One departure: in the waking day, time off the wrist drains at an assumed 3 points per hour instead of freezing (see the first day below). After a day or more off, the score restarts at the first-run value instead of resuming |
| FR6 | Onboarding: estimate, not medical advice | Built |
| FR7 | Reason line | Built |
| FR8 | Breakdown view | Built |
| FR10 | Settings: units, intensity, reset baseline | Built; "units" is read as HRV units (ms or ln) |
| FR9, FR11 to FR13 | Launcher glance, watchface, breathing, CSV export | Deferred, as in the PRD |

Also in place: the baseline
backed up to the phone and restored if the watch loses it, a firmware check at launch, and a
notice when another app's worker holds the single background slot.

Storage used is about 2.4 KiB of the 4 KiB each app gets.

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

## Second day (16 Nov 2026)

Version 0.2.1, worn through the night, a morning workout (a 1 km run and calisthenics), a
commute and a desk day with the watch on and off, then an active evening. A broken night
followed: asleep 20:58 to 01:03, awake for an hour, asleep again to 05:21.

| | Garmin Body Battery | Ebb 0.2.1 as worn | How the wearer felt | Ebb 0.3.0 (same record replayed) |
| --- | --- | --- | --- | --- |
| 15 Nov 21:13, bedtime | 30 | 32 | | 31 |
| 05:00, waking | 51 | 73 | Between the two | 68 |
| 06:55, after the workout | 47 | 60 | Both fell by a similar amount | 53 |
| 09:00, at the office | 33 | 45 | | 40 |
| 12:15 | 13 | 28 | 28, or even 45, nearer than 13 | 28 |
| 16:50 | 36 after reaching 0 | 11 | | 20 |
| 18:45 | 30 | 0 | Not 0 | 16 |
| 17 Nov 05:21, waking | | 37 | | 62 |

The reference's jump from 0 to the 30s is not understood. Its readings just after being put
back on were about 30, 34, 30 and 36 on four occasions, so it may restart after time off the
wrist. That is a guess.

Why the score reached 0: at the 0.2.1 rates the day cost about 84 points (61 activity, 10
stress, 13 estimated off the wrist) and the night gave back 42. With fixed rates nothing
stops that: 0 every evening, about 40 every morning.

What was tried on the record, and what happened when the last day was repeated for a week:

| Change | 16 Nov 12:15 | 16 Nov 18:45 | Repeating the day |
| --- | --- | --- | --- |
| None (0.2.1) | 26 | 0 | 0 in the evening, 41 in the morning |
| Add 20 whenever it reaches 0 | 26 | 17 | Two jumps a day |
| Drains cut to 65% | 59 | 40 | Back at 0 in the evening by the third day |
| Drains slowed below 40, recharge slowed above 60 | 28 | 11 | 0 in the evening, 43 in the morning |
| Rates in proportion to the reserve (chosen) | 28 | 16 | 11 in the evening, 60 in the morning |

What changed, and why:

- **Every rate now scales with the reserve:** drains by `R / 50`, recharge by
  `(100 - R) / 50`. No rate was retuned. It fitted both recorded days as they stood: the first
  day replays as 70, 49 and 37 against the reference's 67, 49 and 34, which the wearer had
  judged the closer figures that day.
- **A night woken in the middle is one night.** The hour awake was longer than the 30
  minutes that continue a sleep, so the night was recorded twice and the baseline counted
  four nights where there had been three. A later sleep now joins the night if it starts
  within 3 hours of the wake and lasts an hour or more, there is one record per day, and
  duplicates left by 0.2.1 are removed when that day is replayed.

The replay behind these figures rebuilt the sleep times for the night of 15 to 16 Nov from
the motion record, because the dump did not carry them; it gives 69 at 05:00 under 0.2.1
where the watch showed 73.

Installing 0.3.0 rebuilt the score from the watch's history, as every change of
`ENGINE_VERSION` does. The watch no longer had sleep sessions for the night of 15 to 16 Nov,
so the rebuild gave that night no recharge and read 31 on the morning of the 16th where the
full replay gives 53. A day later the two were 3 points apart (55 and 58), which is the
fading the scaling is meant to give. A rebuild is therefore only trustworthy for its most
recent night.

## Off the wrist, but counted as worn (17 Nov 2026)

The wearer reported that "Not worn" no longer appeared. The watch had been on a desk for
almost two hours with the score still running. Since the first night, any motion in the last
15 minutes had been allowed to stand in for a heart rate for up to two hours, to cover
heart-rate pauses on a still wrist. A desk is not that still: in 110 minutes the watch
recorded motion in 22 separate minutes (notification buzzes, nudges, being picked up).

On the record there were eight heart-rate gaps of half an hour or more. Seven were times the
watch was off, with motion in 2% to 22% of their minutes; the eighth was a commute with 1,377
steps, which counts as worn by its steps. Worn and awake, the median was 12 minutes with
motion in every 15. While awake the rule now needs motion in 5 of the last 15 minutes;
asleep it is unchanged. Replayed, the notice would have appeared 31 minutes after the last
heart rate that morning, and the two recorded days move by under 2 points.

## Fourth night (18 to 19 Nov 2026)

Version 0.3.1. The wearer found the score too slow to move: sitting in the high 20s through
the afternoon and evening, 47 at 03:00 after a short night, and unchanged after lying in bed
until 05:45, where a reference device put on at 03:00 read about 60 and then about 75. The
60 is the reference's own figure on being put on, not a reading of the night.

What the record showed:

- **Sleep labelled after the fact was lost.** Asleep 23:37 to 02:52, up briefly, then still
  in bed from 03:10. Pebble Health labelled 03:10 to 04:30 as sleep, but only from about
  04:15 (the sampler still saw "awake" at 03:34 and 04:05). The app had been opened in
  between and had already scored those minutes as awake, so they were never credited: about
  9 points.
- **The night itself was scored as intended:** 17 to 46 in 3 hours 15 minutes.
- **The evening crawl was the scaling.** At a reserve of 20 a drain ran at 0.4 of its rate.

What changed:

- **The last four hours are rebuilt when a sleep label changes** (see "Sleep is labelled
  late" in [SCORING.md](SCORING.md)). Forcing a rebuild on the watch reproduced the same
  state exactly.
- **Drain scaling softened** from `R / 50` to `(R + 20) / 70`: 0.57 of the rate at a reserve
  of 20, and 1.71 at 100 where it was 2.
- **Sleep 6.6 to 7.2 points per hour, quiet rest awake 1.8 to 3.6.**

Replayed on the records, old against new:

| | 0.3.1 | 0.4.0 |
| --- | --- | --- |
| 16 Nov, waking (felt about 62) | 67 | 70 |
| 16 Nov 12:15 (felt 28 to 45) | 28 | 28 |
| 16 Nov, bedtime | 13 | 7 |
| 18 Nov 17:30 | 19 | 15 |
| 18 Nov 23:15 | 13 | 8 |
| 19 Nov 02:52, waking | 44 | 43 |
| 19 Nov 05:00 | 52 with the label fix, 45 as worn | 52 |

The softer drains lower the evening by about 5 points and, because the night starts lower,
leave the morning where it was. Reaching the 75 the wearer felt nearer to would take about
three times the sleep rate, which would break the nights that did agree. The wearer's own
judgement has now pulled both ways (73 was "a bit too high" on 16 Nov), so the next step is
more days, not more tuning.

## Scoring moved into the worker (19 Nov 2026)

Version 0.5.0. Until now the worker only sampled and the app scored when opened. The score
is now kept current in the background, which a watchface or launcher glance would need, and
which removes the restart after two days unopened.

Whether it would fit was the open question. What was measured on the watch:

| Free heap when sleep sessions were read | Sessions returned | Heap afterwards |
| --- | --- | --- |
| 7,428 | 6 | 5,376 |
| 3,004 | 6 | 952 |
| 2,404 | 6 | 352 |
| 2,004 | 0 | 2,004 |
| 1,404 to 1,804 | 0 | unchanged |

The first build fitted in 9.2 KiB and silently scored the night as awake, because the
firmware needs 2,052 bytes of the worker's heap to return any sessions at all. Getting under
that took: reading HRV windows through one ring page instead of loading all 210; finding
the night's median by bisection instead of copying the windows; allocating the engine and
every buffer for the run only; subscribing to health events only during a burst; dropping
the worker's log strings and an unused score record; and an explicit guard that abandons a
run with no sessions.

Checked on the watch: a 36-hour rebuild by the worker (about 55 seconds, six sessions, last
night credited); a forced rebuild giving the same state; a run during an HRV burst; a kept
HRV window (54 beat pairs, RMSSD 41.5 ms) with the new subscription; and, with a logging
build too big to score, the app taking over.

Not yet checked: the score staying current over hours with the app closed (every test had
the app open, which asks the worker each minute), and the battery cost of a run every 15
minutes.

## A day on 0.5.0 (19 to 20 Nov 2026)

Worn for a day, then the watch was updated to PebbleOS 4.47.2.

- **Background scoring held up.** Connecting after a day found the stored score 10 minutes
  old. The worker still had 376 bytes spare and was given nine sleep sessions on 4.47.2.
- **No change in battery drain that the gauge can show:** 5.1% a day over the 23 hours with
  background scoring (74% to 69%), against 5.0 to 5.1% a day over the four days before.
- **Evening:** 13 when taken off for 2 hours 15 minutes, 9 when put back on (the off-wrist
  estimate). The reference read 30 on being put on, as it has each evening; the wearer
  suspects it starts from a set value by time of day (30 in the evening, 60 in the morning).
- **Night:** asleep 21:09 to 05:06, 199 minutes restful, median RMSSD 38.9 ms against a
  baseline of 33.0. Ebb went from 9 to 65, the reference from 30 to 95.

The wearer asked for a slightly faster rise in sleep, or for the score to start from a set
value after time off the wrist as the reference seems to. Sleep went from 7.2 to 8.4 points
per hour. Replayed, that night ends at 71; the short night before it at 57 where it was 52;
and the night of 15 to 16 Nov, which the wearer had put at about 62, at 74 where it was 70.
The heaviest recorded day, repeated, settles at 4 in the evening and 64 on waking.

Starting from a set value was not taken up for a gap of hours: the score had fallen from 50
to 13 through a day of worn measurement, and replacing that with 30 would discard it. After
a day or more off the wrist the score already restarts (at 40).

## Not done, or not known

- **376 bytes is all the worker has to spare.** A firmware release with a larger health
  cache would stop the worker scoring; the app would then score when opened, as before.
- **The fit rests on two days.** A seated day, a long calm night and an evening at rest have
  not been recorded, and the reference and the wearer's own judgement disagreed on the
  second day.
- **Whether mornings ever read high.** With recharge slowing as the reserve fills, a good
  night should give 75 to 85. No such night has been recorded.
- **The HRV baseline has one night of three.** Until then HRV does not steer the rates.
- **Battery:** the app reports sensor-on minutes (measured) and whole-watch drain per day
  (measured between launches). It does not convert sensor minutes into a percentage, because
  the sensor's current draw has not been measured.
- **Without the worker, the baseline needs the app opened at least every two days or so.**
  Nights are then summarised when the app next runs; if the ring of windows has wrapped by
  then, that night counts without HRV.
- Interval pacing, the motion threshold and every weight are open; see
  [SCORING.md](SCORING.md).
