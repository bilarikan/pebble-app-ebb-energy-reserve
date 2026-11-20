#!/usr/bin/env python3
"""Turns the log of an ER_DEBUG build into the files replay.c reads.

    python3 tools/replay/parse_log.py LOG OUTDIR

LOG is the output of `pebble install --logs` for a build made with ER_DEBUG=1 (see
docs/DEVELOPMENT.md). OUTDIR gets:

    minutes.txt   epoch-minute steps heart-rate motion-counts flags     one line per minute
    wins.txt      epoch-minute asleep(0/1) rmssd-in-0.1-ms              the HRV windows
    nights.txt    local-day rmssd10 sleep-minutes resting-hr windows    the baseline
    curve.txt     epoch-minute score                                    what the watch showed
    meta.txt      tz_min

The log is the wearer's raw health record: keep LOG and OUTDIR out of the repository
(.local/ is ignored for this).

One trap: the sleep flag in the minute lines is only right for the most recent night,
because the watch hands out only its latest sleep sessions. Start a replay after the last
unflagged night, or fix the flags by hand.
"""
import os
import re
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    log, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    lines = open(log, errors="replace").read().splitlines()

    minutes = {}
    stamp_to_epoch = {}   # (day of month) -> epoch minute of that local midnight
    for line in lines:
        m = re.search(r"> M (\d{6}) (\d+) ([0-9a-f]+)", line)
        if not m:
            continue
        stamp, t0, hexs = int(m.group(1)), int(m.group(2)), m.group(3)
        day, hh, mm = stamp // 10000, stamp // 100 % 100, stamp % 100
        stamp_to_epoch[day] = t0 - hh * 60 - mm
        for i in range(len(hexs) // 8):
            c = hexs[i * 8:i * 8 + 8]
            minutes[t0 + i] = (int(c[0:2], 16), int(c[2:4], 16), int(c[4:7], 16), int(c[7], 16))
    if not minutes:
        sys.exit("no minute lines (M) in the log: was it an ER_DEBUG build, left to finish its dump?")

    def epoch(stamp):
        """ddhhmm local -> epoch minute, by the nearest day the minute lines cover."""
        day, hh, mm = stamp // 10000, stamp // 100 % 100, stamp % 100
        if day in stamp_to_epoch:
            return stamp_to_epoch[day] + hh * 60 + mm
        near = min(stamp_to_epoch, key=lambda d: abs(d - day))   # same month assumed
        return stamp_to_epoch[near] + (day - near) * 1440 + hh * 60 + mm

    midnight = next(iter(stamp_to_epoch.values()))
    tz_min = -(midnight % 1440)
    if tz_min <= -720:
        tz_min += 1440

    with open(os.path.join(out, "minutes.txt"), "w") as f:
        for t in sorted(minutes):
            f.write("%d %d %d %d %d\n" % ((t,) + minutes[t]))

    n_win = 0
    with open(os.path.join(out, "wins.txt"), "w") as f:
        for line in lines:
            if "> W " not in line:
                continue
            for stamp, kind, rmssd in re.findall(r"(\d{6})([sa])(\d+)", line.split("> W ")[1]):
                f.write("%d %d %s\n" % (epoch(int(stamp)), 1 if kind == "s" else 0, rmssd))
                n_win += 1

    n_night = 0
    with open(os.path.join(out, "nights.txt"), "w") as f:
        for line in lines:
            m = re.search(r"D night\d+ day(\d+) rmssd(\d+) sleep(\d+) rhr(\d+) wins(\d+)", line)
            if m:
                f.write(" ".join(m.groups()) + "\n")
                n_night += 1

    n_curve = 0
    with open(os.path.join(out, "curve.txt"), "w") as f:
        for line in lines:
            m = re.search(r"> C (\d{6}) (.*)", line)
            if not m:
                continue
            t = epoch(int(m.group(1)))
            vals = m.group(2).split()
            if len(vals) > 24:
                vals = vals[:24]
            elif len(vals) == 24 and not line.endswith(" "):
                vals = vals[:23]   # the watch cuts long lines; the last number may be half of one
            for i, v in enumerate(vals):
                if v != "255":
                    f.write("%d %s\n" % (t + i * 15, v))
                    n_curve += 1

    with open(os.path.join(out, "meta.txt"), "w") as f:
        f.write("%d\n" % tz_min)

    ts = sorted(minutes)
    print("%d minutes (%d h), %d windows, %d nights, %d curve points, tz %+d min -> %s"
          % (len(ts), len(ts) // 60, n_win, n_night, n_curve, tz_min, out))


if __name__ == "__main__":
    main()
