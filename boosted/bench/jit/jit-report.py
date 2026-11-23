#!/usr/bin/env python3
"""hakuX-Boosted jit bench: the table of one or two runs of run-jit.sh.

    jit-report.py --head head.jsonl [--base base.jsonl] [--head-name TEXT] [--base-name TEXT]

Markdown on stdout. Each number is the lowest over the rounds (noise on a
shared runner only adds). "change" is head against base, negative is less.
Exit status 1 if the head run has a WRONG or NOT MEASURED note.
"""
import argparse
import json
import math
import sys


def load(path):
    values, notes = {}, []
    if not path:
        return values, notes
    for line in open(path):
        line = line.strip()
        if not line.startswith("{"):
            continue
        row = json.loads(line)
        if "note" in row:
            if (row["name"], row["note"]) not in notes:
                notes.append((row["name"], row["note"]))
            continue
        key = (row["name"], row["config"], row["metric"])
        values[key] = min(values.get(key, math.inf), row["value"])
    return values, notes


def fmt(value, digits=3):
    if value is None:
        return "-"
    if value >= 1000:
        return f"{value:,.0f}"
    return f"{value:.{digits}g}" if value < 100 else f"{value:.1f}"


def change(base, head):
    if base is None or head is None or base == 0:
        return "-"
    pct = (head / base - 1) * 100
    text = f"{pct:+.1f}%"
    return f"**{text}**" if abs(pct) >= 3 else text


def geomean(pairs):
    logs = [math.log(h / b) for b, h in pairs if b and h and b > 0 and h > 0]
    return math.exp(sum(logs) / len(logs)) if logs else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--head", required=True)
    ap.add_argument("--base")
    ap.add_argument("--head-name", default="head")
    ap.add_argument("--base-name", default="base")
    args = ap.parse_args()
    head, head_notes = load(args.head)
    base, base_notes = load(args.base)
    have_base = bool(base)
    names = []
    for name, _config, _metric in list(head) + list(base):
        if name not in names:
            names.append(name)
    images = [n for n in names if n.endswith("(whole image)")]
    calls = [n for n in names if "/" in n]

    def get(table, name, config, metric):
        return table.get((name, config, metric))

    print("## Translator benchmark (jit-bench)\n")
    if have_base:
        print(f"`{args.head_name}` against `{args.base_name}`, built and run in the same job, turn about.\n")
    print("### Code size and translation time, whole images\n")
    print("Host bytes emitted per PowerPC instruction, over every function of the image; "
          "16 KiB is the phone's page size (run under qemu, size only). "
          "Translation time is thread CPU time per PowerPC instruction.\n")
    cols = ["image", "functions", "PowerPC insns"]
    for metric in ("bytes/insn, 4 KiB", "bytes/insn, 16 KiB", "translate us/insn"):
        cols += ([f"{metric}: base", "head", "change"] if have_base else [metric])
    print("| " + " | ".join(cols) + " |")
    print("|" + "---|" * len(cols))
    means = {0: [], 1: [], 2: []}
    for name in images:
        row = [name.replace(" (whole image)", ""), fmt(get(head, name, "4k", "functions"), 6),
               fmt(get(head, name, "4k", "guest_insns"), 6)]
        for i, (config, metric) in enumerate((("4k", "host_bytes_per_guest_insn"), ("16k", "host_bytes_per_guest_insn"),
                                              ("4k", "translate_us_per_guest_insn"))):
            h, b = get(head, name, config, metric), get(base, name, config, metric)
            if have_base:
                row += [fmt(b), fmt(h), change(b, h)]
                means[i].append((b, h))
            else:
                row += [fmt(h)]
        print("| " + " | ".join(row) + " |")
    if have_base and images:
        row = ["**geometric mean**", "", ""]
        for i in range(3):
            g = geomean(means[i])
            row += ["", "", f"**{(g - 1) * 100:+.1f}%**" if g else "-"]
        print("| " + " | ".join(row) + " |")
    refused = [(n, get(head, n, "4k", "functions_refused")) for n in images]
    refused = [f"{n.replace(' (whole image)', '')} {int(r)}" for n, r in refused if r]
    if refused:
        print("\nFunctions the translator refused (data taken for code by the function list): " + ", ".join(refused) + ".")

    print("\n### Run time, workloads\n")
    print("Each workload's timed call: milliseconds (lowest of the samples) and host instructions retired "
          "(the CPU's counter; exact, where the runner has one). The result of every call is checked against "
          "the host C compiler's.\n")
    cols = ["workload"]
    for metric in ("run ms", "host insns, millions"):
        cols += ([f"{metric}: base", "head", "change"] if have_base else [metric])
    print("| " + " | ".join(cols) + " |")
    print("|" + "---|" * len(cols))
    means = {0: [], 1: []}
    for name in calls:
        row = [name.split("/", 1)[1]]
        for i, metric in enumerate(("run_ms", "host_insns_millions")):
            h, b = get(head, name, "4k", metric), get(base, name, "4k", metric)
            if have_base:
                row += [fmt(b, 4), fmt(h, 4), change(b, h)]
                means[i].append((b, h))
            else:
                row += [fmt(h, 4)]
        print("| " + " | ".join(row) + " |")
    if have_base and calls:
        row = ["**geometric mean**"]
        for i in range(2):
            g = geomean(means[i])
            row += ["", "", f"**{(g - 1) * 100:+.1f}%**" if g else "-"]
        print("| " + " | ".join(row) + " |")

    bad = False
    for title, notes in ((args.head_name, head_notes), (args.base_name, base_notes)):
        if notes:
            print(f"\n### Notes, {title}\n")
            for name, note in notes:
                print(f"- {name}: {note}")
                if title == args.head_name and ("WRONG" in note or "NOT MEASURED" in note):
                    bad = True
    print("\nThese are a GitHub runner's numbers, not a phone's: nothing here is a speed-up of a game "
          "until it is measured on the device (boosted/docs/PLAN.md).")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
