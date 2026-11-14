# Ebb

A 0 to 100 energy reserve for the Pebble Time 2, computed on the watch from heart rate
variability, heart rate, movement and sleep. No account, no cloud, no network calls.

"Ebb" is a working name. The PRD leaves naming open and rules out "Body Battery" and
"Energy Reserve"; changing it is one line (`displayName` in `package.json`).

**Status: builds and runs in the emulator; not yet worn.** The scoring engine is unit-tested
and the app has been exercised in the Pebble Time 2 emulator with synthetic data. HRV capture
cannot be emulated, so nothing about real-wrist behaviour (signal quality, battery cost,
agreement with a reference) has been measured. That is the PRD's spike.

It is an estimate, not medical advice.

| Main | With a reason line | Breakdown | Not worn |
| --- | --- | --- | --- |
| ![Main screen](docs/screenshots/main.png) | ![Reason line](docs/screenshots/main-reason.png) | ![Breakdown](docs/screenshots/breakdown.png) | ![Not worn](docs/screenshots/not-worn.png) |

Emulator screenshots. The first three use the synthetic demo data.

## What it does

- **Main screen:** score, state in words, a one-line reason for what changed since you last
  looked, a 24-hour curve, last-updated time and a confidence flag.
- **Select:** breakdown of every term behind the score, last night's inputs, your baseline,
  and what the sampler is doing (windows kept, sensor-on minutes, watch battery use).
- **Background sampler:** measures HRV in short bursts while you are asleep or still, then
  releases the sensor.
- **Phone settings:** sampling intensity, HRV units, maximum heart rate, reset baseline.

The scoring model is written up in [docs/SCORING.md](docs/SCORING.md).

## Requirements

| | Version | Why |
| --- | --- | --- |
| Watch | Pebble Time 2 (`emery`) | The only watch whose sensor reports HRV |
| PebbleOS | 4.32 or later | First firmware with the HRV API; the app checks at launch |
| Pebble SDK | 4.44 | First SDK checked here whose worker header carries the HRV calls |
| pebble-tool | 5.1.0 or later | Required by SDK 4.44 |

Heart-rate monitoring must be on in the watch's Health settings. The firmware refuses HRV to
a background worker otherwise, and the app says so.

## Build and run

```bash
npm install
```

```bash
pebble build
```

```bash
pebble install --phone <phone-ip>
```

The emulator has no health history, so a normal build shows a frozen score there. For the
emulator, build the demo variant, which feeds the same engine a synthetic day:

```bash
ER_DEMO=1 pebble build && pebble install --emulator emery
```

Run `pebble wipe` when switching between demo and normal builds in the emulator, because the
demo's saved state would otherwise carry over.

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
| FR1 | Burst sampler that always releases the sensor | Built; untested on hardware |
| FR2 | Store HRV windows, resting HR, timestamps for days | Built: 210 windows (about 2.5 days) and 14 nightly summaries |
| FR3 | Engine recomputes on a fixed interval | Built: every minute while open, and a full catch-up on launch |
| FR4 | Score, state, curve, updated time, confidence flag | Built |
| FR5 | Not worn: freeze, mark stale, resume low confidence | Built and tested |
| FR6 | Onboarding: estimate, not medical advice | Built |
| FR7 | Reason line | Built |
| FR8 | Breakdown view | Built |
| FR10 | Settings: units, intensity, reset baseline | Built; "units" is read as HRV units (ms or ln) |
| FR9, FR11 to FR13 | Launcher glance, watchface, breathing, CSV export | Deferred, as in the PRD |

Also in place: a small versioned score record for a later watchface (`PK_SCORE`), the baseline
backed up to the phone and restored if the watch loses it, a firmware check at launch, and a
notice when another app's worker holds the single background slot.

Storage used is about 2 KiB of the 4 KiB each app gets.

## Not done, or not known

- **Nothing is validated on a wrist.** Overnight capture, window quality and battery cost are
  the first things to check.
- **Battery:** the app reports sensor-on minutes (measured) and whole-watch drain per day
  (measured between launches). It does not convert sensor minutes into a percentage, because
  the sensor's current draw has not been measured.
- **Baseline needs the app opened at least every two days or so.** Nights are summarised
  when the app next runs; if the ring of windows has wrapped by then, that night counts
  without HRV.
- **No licence file.** The PRD leaves the licence open.
- Interval pacing, the motion threshold and every weight are open; see
  [docs/SCORING.md](docs/SCORING.md).
