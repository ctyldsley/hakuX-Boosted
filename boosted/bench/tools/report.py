#!/usr/bin/env python3
"""hakuX-Boosted bench: turns the benchmark programs' result lines into a
Markdown report, with the difference against a second set when one is given.

Each input file holds one JSON object a line (see common/bench.h). A number
that appears more than once (the run script makes several rounds) is reduced
to its lowest value: on a shared runner noise only ever adds time.
"""
import argparse
import json
import statistics
import sys

SECTION_TITLES = [
    ("prim", "1. Host primitives",
     "One call of a function that is not inlined; `call_empty` is the call "
     "and return alone, to subtract. Nanoseconds, lowest of the samples."),
    ("prim-bionic", "1 again, on bionic",
     "The same program built for Android and run in a Termux container on "
     "the same runner: Android's libc and dynamic linker, the runner's CPU."),
    ("fiber", "1b. Fiber switch through the core's own Fiber class",
     "`xe::threading::Fiber::SwitchTo` as the scheduler calls it, per switch."),
    ("sched", "2. The guest scheduler's paths",
     "The core's `guest_scheduler.cc`, `xthread.cc`, `xobject.cc`, "
     "`xevent.cc` and `xboxkrnl_threading.cc`, unmodified. Guest threads made "
     "with `XThread::Create` run PowerPC loops that call the kernel exports "
     "through import thunks. One operation is one pass of the loop: one "
     "kernel call, or for `wait_wake_round_trip` a set and a blocking wait "
     "on each of two threads. `loop_alone` and "
     "`kernel_call_that_does_nothing` are what to subtract."),
    ("xlat", "3. The translator",
     "Synthetic PowerPC loops written for this rig, translated and run by "
     "the core's frontend, passes and ARM64 backend. Each result is checked "
     "against a C++ model of the loop. \"Emitted\" counts the whole "
     "function's host code, prologue and cold paths included, over its "
     "guest instructions; \"run\" is what the CPU's counter saw per guest "
     "instruction inside the loop."),
]

METRIC_LABELS = {
    "ns_per_op": "ns per operation",
    "cores_busy": "cores busy (process CPU time over wall time)",
    "ns_per_guest_insn": "ns per guest instruction",
    "host_insn_per_guest_insn": "host instructions run per guest instruction",
    "host_code_insn_per_guest_insn":
        "host instructions emitted per guest instruction",
    "translate_us": "time to translate, us",
    "guest_insn": "guest instructions in the function",
    "host_code_bytes": "bytes of host code emitted",
}

# Lower is better for every metric here.
SPREAD_SUFFIX = "_median"


def load(path):
    values = {}
    notes = {}
    order = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if "note" in row:
                notes[(row["section"], row["name"])] = row["note"]
                continue
            key = (row["section"], row["name"], row.get("config", ""),
                   row["metric"])
            if key not in values:
                values[key] = []
                order.append(key)
            values[key].append(float(row["value"]))
    return values, notes, order


def fmt(value):
    if value == 0:
        return "0"
    magnitude = abs(value)
    if magnitude >= 1000:
        return f"{value:,.0f}"
    if magnitude >= 100:
        return f"{value:.1f}"
    if magnitude >= 10:
        return f"{value:.2f}"
    return f"{value:.3f}"


def short(value):
    if value == 0:
        return "0"
    magnitude = abs(value)
    if magnitude >= 100:
        return f"{value:.0f}"
    if magnitude >= 10:
        return f"{value:.1f}"
    if magnitude >= 1:
        return f"{value:.2f}"
    return f"{value:.3f}"


def digest(values, base_values, notes, order):
    """One row a benchmark and metric, one column a setting, lowest values.
    Then, with a baseline, every number that moved by more than 3%."""
    keys = [k for k in order if not k[3].endswith(SPREAD_SUFFIX)]
    sections = []
    for k in keys:
        if k[0] not in sections:
            sections.append(k[0])
    for section in sections:
        configs = []
        rows = []
        for k in keys:
            if k[0] != section:
                continue
            if k[2] not in configs:
                configs.append(k[2])
            if (k[1], k[3]) not in rows:
                rows.append((k[1], k[3]))
        print(f"== {section}: columns = " + " ; ".join(
            f"{i}:{c or '-'}" for i, c in enumerate(configs)))
        for name, metric in rows:
            cells = []
            for config in configs:
                key = (section, name, config, metric)
                cells.append(short(min(values[key])) if key in values else "-")
            if all(c in ("0", "-") for c in cells):
                continue
            print(f"{name} {metric}: " + " ".join(cells))
    if base_values:
        print("== moved by more than 3% against the baseline "
              "(value, baseline, change)")
        for k in keys:
            if k not in base_values:
                print(f"{'/'.join(k)}: {short(min(values[k]))} not-in-baseline")
                continue
            best, base = min(values[k]), min(base_values[k])
            if base > 0 and abs(best - base) / base > 0.03:
                print(f"{'/'.join(k)}: {short(best)} {short(base)} "
                      f"{(best - base) / base * 100:+.1f}%")
        for k in base_values:
            if k not in values and not k[3].endswith(SPREAD_SUFFIX):
                print(f"{'/'.join(k)}: missing-here {short(min(base_values[k]))}")
    print("== notes")
    for (section, name), text in notes.items():
        if name.startswith("thread-locals (") and "dynamic" in text:
            continue
        print(f"{section} {name}: {text}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True)
    parser.add_argument("--target-name", default="this branch")
    parser.add_argument("--baseline")
    parser.add_argument("--baseline-name", default="baseline")
    parser.add_argument("--title", default="hakuX-Boosted bench")
    parser.add_argument("--out", help="write the report here as well")
    parser.add_argument("--json-out", help="write the reduced numbers here")
    parser.add_argument("--digest", action="store_true",
                        help="print a short pivoted digest instead of the "
                             "report: one row a benchmark, one column a "
                             "setting")
    parser.add_argument("--compact", action="store_true",
                        help="print one short line a number instead of the "
                             "report (for reading a job log)")
    args = parser.parse_args()

    values, notes, order = load(args.target)
    base_values, base_notes = {}, {}
    if args.baseline:
        base_values, base_notes, _ = load(args.baseline)

    if args.digest:
        digest(values, base_values, notes, order)
        return

    if args.compact:
        for key in order:
            if key[3].endswith(SPREAD_SUFFIX):
                continue
            best = min(values[key])
            line = [key[0], key[1], key[2], key[3], fmt(best)]
            if key in base_values:
                base = min(base_values[key])
                line.append(fmt(base))
                if base > 0:
                    line.append(f"{(best - base) / base * 100:+.1f}%")
            print("|".join(line))
        for (section, name), text in notes.items():
            print("|".join(["note", section, name, text]))
        return

    out = []
    out.append(f"# {args.title}")
    out.append("")
    out.append(f"Measured: `{args.target_name}`"
               + (f", against `{args.baseline_name}` built and run in the "
                  "same job on the same machine." if args.baseline else "."))
    out.append("")
    out.append("These are costs of code paths on a GitHub Actions ARM64 Linux "
               "runner: a server CPU, glibc's loader, no thermal limit. They "
               "rank code paths and show whether a change made one cheaper. "
               "They are not phone numbers and say nothing about frame rate.")
    out.append("")

    env = [(k, v) for k, v in notes.items()
           if k[1] in ("cpu", "kernel", "libc", "lse_atomics", "cores",
                       "instruction_counter", "compiler")]
    if env:
        out.append("| Runner | |")
        out.append("|---|---|")
        seen = set()
        for (section, name), text in env:
            if name in seen:
                continue
            seen.add(name)
            out.append(f"| {name} | {text} |")
        out.append("")

    reduced = {}
    sections_seen = []
    for key in order:
        if key[0] not in sections_seen:
            sections_seen.append(key[0])
    titled = [s for s, _, _ in SECTION_TITLES]
    section_list = [s for s in titled if s in sections_seen] + \
                   [s for s in sections_seen if s not in titled]
    # A section that produced only notes (a build that failed) still shows.
    for (section, _), _ in notes.items():
        if section not in section_list:
            section_list.append(section)

    for section in section_list:
        title = next((t for s, t, _ in SECTION_TITLES if s == section), section)
        blurb = next((b for s, _, b in SECTION_TITLES if s == section), "")
        out.append(f"## {title}")
        out.append("")
        if blurb:
            out.append(blurb)
            out.append("")
        header = ["Benchmark", "Setting", "Metric", args.target_name, "noise"]
        if args.baseline:
            header += [args.baseline_name, "change"]
        rows = []
        for key in order:
            if key[0] != section or key[3].endswith(SPREAD_SUFFIX):
                continue
            _, name, config, metric = key
            best = min(values[key])
            reduced["/".join(key)] = best
            spread_key = (section, name, config, metric + SPREAD_SUFFIX)
            spread = ""
            spread_fraction = 0.0
            if spread_key in values and best > 0:
                middle = statistics.median(values[spread_key])
                spread_fraction = max(0.0, (middle - best) / best)
                spread = f"+{spread_fraction * 100:.0f}%"
            row = [f"`{name}`", config, METRIC_LABELS.get(metric, metric),
                   fmt(best), spread]
            if args.baseline:
                if key in base_values:
                    base = min(base_values[key])
                    row.append(fmt(base))
                    if base > 0:
                        change = (best - base) / base
                        text = f"{change * 100:+.1f}%"
                        # Bold only what stands clear of the noise.
                        if abs(change) > max(0.05, 2 * spread_fraction):
                            text = f"**{text}**"
                        row.append(text)
                    else:
                        row.append("")
                else:
                    row += ["not present", ""]
            rows.append(row)
        if rows:
            out.append("| " + " | ".join(header) + " |")
            out.append("|" + "|".join(
                ["---", "---", "---", "---:", "---:", "---:", "---:"]
                [:len(header)]) + "|")
            for row in rows:
                out.append("| " + " | ".join(row) + " |")
            out.append("")
        section_notes = [(k[1], v) for k, v in notes.items()
                         if k[0] == section and (k, v) not in env]
        if section_notes:
            for name, text in section_notes:
                out.append(f"- `{name}`: {text}")
            out.append("")
        if args.baseline:
            missing = [k for k in base_values
                       if k[0] == section and k not in values
                       and not k[3].endswith(SPREAD_SUFFIX)]
            if missing:
                out.append(f"Present in `{args.baseline_name}` and missing "
                           "here: " + ", ".join(
                               f"`{k[1]}` ({k[2] or 'default'})"
                               for k in missing[:20]) + ".")
                out.append("")

    if args.baseline:
        out.append("\"change\" is this branch against the baseline, negative "
                   "is cheaper. \"noise\" is how far the middle sample sat "
                   "above the lowest; a change is in bold only when it is "
                   "over 5% and over twice that noise. Treat anything else "
                   "as no difference.")
        out.append("")

    text = "\n".join(out) + "\n"
    sys.stdout.write(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as handle:
            json.dump(reduced, handle, indent=1, sort_keys=True)
            handle.write("\n")


if __name__ == "__main__":
    main()
