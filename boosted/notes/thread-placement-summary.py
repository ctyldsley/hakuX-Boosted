#!/usr/bin/env python3
"""Sums up the once-a-second "host perf" lines of an xe.log.

    python3 boosted/notes/thread-placement-summary.py xe.log [xe-other.log ...]

For each log: how long each guest CPU dispatch thread and the GPU command
thread ran and waited for a core, overall and in the seconds where the
compile threads were busy; what the cores were clocked at and limited to;
the thermal state; frames and the hint session's reports. Give two logs to
set two runs side by side (see boosted/notes/thread-placement.md, section 1).
"""

import re
import statistics
import sys

LINE = re.compile(r"host perf (\d+) ms \| (.*)")
THREAD = re.compile(r"(G\d|GPU) ([\d.]+)/([\d.]+) (-?\d+) @(-?\d+)")
HELPERS = re.compile(r"helpers\((\d+)\) ([\d.]+)/([\d.]+)")
CORE = re.compile(r"(\d+):(\d+)/(\d+)")
THERMAL = re.compile(r"thermal (-?\d+) (\w+) headroom (\S+)")
FRAMES = re.compile(r"frames (\d+) longest ([\d.]+) ms")
HINT = re.compile(r"hint frame ([\d.]+) ms target ([\d.]+) ms reports (\d+) "
                  r"mean ([\d.]+) ms late (\d+)")
# A second counts as "compiling" when the helpers together ran this long.
BUSY_HELPERS_MS = 500.0


def summarise(path):
    seconds = []
    with open(path, errors="replace") as log:
        for text in log:
            found = LINE.search(text)
            if not found:
                continue
            parts = found.group(2).split(" | ")
            second = {"ms": int(found.group(1)), "threads": {}, "cores": {}}
            for part in parts:
                if part.startswith("MHz"):
                    for core, now, limit in CORE.findall(part):
                        second["cores"][int(core)] = (int(now), int(limit))
                elif part.startswith("thermal"):
                    thermal = THERMAL.search(part)
                    if thermal:
                        second["thermal"] = (int(thermal.group(1)),
                                             thermal.group(2),
                                             thermal.group(3))
                elif part.startswith("run/wait"):
                    for name, run, wait, slices, core in THREAD.findall(part):
                        second["threads"][name] = (float(run), float(wait),
                                                   int(slices), int(core))
                    helpers = HELPERS.search(part)
                    if helpers:
                        second["helpers"] = (float(helpers.group(2)),
                                             float(helpers.group(3)))
                elif part.startswith("frames"):
                    frames = FRAMES.search(part)
                    if frames:
                        second["frames"] = (int(frames.group(1)),
                                            float(frames.group(2)))
                elif part.startswith("hint frame"):
                    hint = HINT.search(part)
                    if hint:
                        second["hint"] = tuple(float(x) for x in hint.groups())
            seconds.append(second)

    print(f"== {path}: {len(seconds)} lines")
    if not seconds:
        print("   no 'host perf' lines: is host_perf_log on?")
        return
    busy = [s for s in seconds
            if s.get("helpers", (0, 0))[0] >= BUSY_HELPERS_MS]
    print(f"   seconds with the compile threads busy "
          f"(>= {BUSY_HELPERS_MS:.0f} ms): {len(busy)}")

    print("   thread   run s   wait s  wait/run  worst wait ms  "
          "| while compiling: run s  wait s  worst ms | cores used")
    names = sorted({n for s in seconds for n in s["threads"]})
    for name in names:
        rows = [s["threads"][name] for s in seconds if name in s["threads"]]
        rows_busy = [s["threads"][name] for s in busy if name in s["threads"]]
        run = sum(r[0] for r in rows) / 1000
        wait = sum(r[1] for r in rows) / 1000
        run_busy = sum(r[0] for r in rows_busy) / 1000
        wait_busy = sum(r[1] for r in rows_busy) / 1000
        cores = sorted({r[3] for r in rows})
        print(f"   {name:<6} {run:7.1f} {wait:8.1f} "
              f"{(wait / run if run else 0):9.2f} "
              f"{max(r[1] for r in rows):14.1f}  | "
              f"{run_busy:22.1f} {wait_busy:7.1f} "
              f"{(max(r[1] for r in rows_busy) if rows_busy else 0):9.1f} | "
              f"{','.join(str(c) for c in cores)}")
    helpers = [s["helpers"] for s in seconds if "helpers" in s]
    if helpers:
        print(f"   helpers {sum(h[0] for h in helpers) / 1000:6.1f} "
              f"{sum(h[1] for h in helpers) / 1000:8.1f}")

    cores = sorted({c for s in seconds for c in s["cores"]})
    if cores:
        print("   core  mean MHz  lowest limit  highest limit  "
              "seconds limited below the highest")
        for core in cores:
            rows = [s["cores"][core] for s in seconds if core in s["cores"]]
            top = max(r[1] for r in rows)
            print(f"   {core:<5} {statistics.mean(r[0] for r in rows):8.0f} "
                  f"{min(r[1] for r in rows):13d} {top:14d} "
                  f"{sum(1 for r in rows if r[1] < top):8d}")
    else:
        print("   no core frequencies in the log (not readable by the app)")

    thermal = [s["thermal"] for s in seconds if "thermal" in s]
    if thermal:
        counts = {}
        for status, name, _ in thermal:
            counts[f"{status} {name}"] = counts.get(f"{status} {name}", 0) + 1
        headroom = [float(t[2]) for t in thermal if t[2] != "-"]
        print("   thermal status, seconds each: "
              + ", ".join(f"{k}: {v}" for k, v in sorted(counts.items())))
        if headroom:
            print(f"   headroom first {headroom[0]:.2f} last {headroom[-1]:.2f} "
                  f"highest {max(headroom):.2f}")

    frames = [s["frames"] for s in seconds if "frames" in s]
    if frames:
        total_s = sum(s["ms"] for s in seconds if "frames" in s) / 1000
        print(f"   frames {sum(f[0] for f in frames)} in {total_s:.0f} s "
              f"({sum(f[0] for f in frames) / total_s:.1f} a second); "
              f"seconds whose longest frame was over 50 ms: "
              f"{sum(1 for f in frames if f[1] > 50)}; "
              f"longest {max(f[1] for f in frames):.1f} ms")
    hint = [s["hint"] for s in seconds if "hint" in s]
    if hint:
        reports = sum(h[2] for h in hint)
        print(f"   hint session: {reports:.0f} reports, "
              f"{sum(h[4] for h in hint):.0f} late, last frame time "
              f"{hint[-1][0]:.1f} ms, last target {hint[-1][1]:.1f} ms")
    else:
        print("   no hint session in the log")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for log_path in sys.argv[1:]:
        summarise(log_path)
