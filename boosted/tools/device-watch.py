#!/usr/bin/env python3
"""hakuX-Boosted: watch a test run on a phone over adb.

A glitch that lasts a second is gone before anyone can describe it, and a
stall leaves nothing in the log by itself. This keeps a rolling recording of
the phone's screen while a game is tested and, when something happens, takes
everything at once: the last seconds of the screen as still frames, the
emulator's log, the state of every guest thread, how evenly frames arrived.

  device-watch.py start          begin recording the screen in 60 s pieces
                                 (the last 8 are kept on the phone)
  device-watch.py grab [secs]    something just happened: the last `secs`
                                 seconds (default 20) as frames, a screenshot,
                                 xe.log, a thread dump, frame pacing
  device-watch.py fps [secs]     frame pacing alone, over `secs` (default 10)
  device-watch.py dump           the thread dump alone (twice, 3 s apart)
  device-watch.py stop           stop recording, remove the pieces

Everything lands under --out (default: device-logs/watch beside the
repository), one folder per grab. Needs adb and, for frames, ffmpeg.
The 360 side only; the debug package unless --package says otherwise.
"""
import argparse
import datetime
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

PHONE_DIR = "/sdcard/Movies/hakux-watch"
SEGMENT_SECONDS = 60
KEEP = 8

# Git Bash rewrites arguments that look like Unix paths; adb's are the phone's.
ENV = dict(os.environ, MSYS_NO_PATHCONV="1", MSYS2_ARG_CONV_EXCL="*")


def adb(*args, check=False, binary=False, timeout=120):
    r = subprocess.run(["adb", *args], capture_output=True, env=ENV, timeout=timeout)
    if check and r.returncode != 0:
        raise SystemExit(f"adb {' '.join(args)}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout if binary else r.stdout.decode(errors="replace")


def shell(cmd, **kw):
    return adb("shell", cmd, **kw)


def compose_dir(package):
    return f"/sdcard/Android/data/{package}/files/compose"


def start(args):
    stop(args, quiet=True)
    shell(f"mkdir -p {PHONE_DIR}")
    # One screenrecord after another, each closed properly (a piece that is
    # still being written has no index and cannot be read), the oldest removed.
    loop = (
        f"while [ -e {PHONE_DIR}/.on ]; do "
        f"screenrecord --time-limit {SEGMENT_SECONDS} --bit-rate 8000000 --size 1304x600 "
        f"{PHONE_DIR}/seg-$(date +%s).mp4 || sleep 3; "
        # a piece of nothing: the screen was off, screenrecord cannot start
        f"find {PHONE_DIR} -name seg-*.mp4 -size 0 -delete; "
        f"ls -t {PHONE_DIR}/seg-*.mp4 | tail -n +{KEEP + 1} | xargs -r rm -f; "
        f"done"
    )
    shell(f"touch {PHONE_DIR}/.on; nohup sh -c '{loop}' >/dev/null 2>&1 &")
    shell("logcat -G 64M")
    time.sleep(1)
    awake = "Awake" in shell("dumpsys power | grep mWakefulness=")
    running = shell("pidof screenrecord").strip()
    if not awake:
        print("the phone's screen is off: recording begins by itself once it is on")
    else:
        print("recording" if running else "screenrecord did not start")


def stop(args, quiet=False):
    shell(f"rm -f {PHONE_DIR}/.on; pkill -2 screenrecord; sleep 1; rm -rf {PHONE_DIR}")
    if not quiet:
        print("stopped")


def surface_layer(package):
    """The game's SurfaceView layer, as SurfaceFlinger names it."""
    layers = shell("dumpsys SurfaceFlinger --list").splitlines()
    for name in layers:
        # the game draws into a SurfaceView; the activity's own window layers
        # carry the same names and no game frames
        if package in name and "EmulatorHostActivity" in name and "SurfaceView[" in name \
                and "Background for" not in name and "Bounds for" not in name:
            # Android 14 and later list "RequestedLayerState{id NAME parentId=..}":
            # --latency wants the name alone
            m = re.search(r"SurfaceView\[[^\]]+\]\(BLAST\)#\d+", name)
            return m.group(0) if m else name.strip()
    return None


def pacing(package, seconds, out=None):
    """Frame times of the game's surface over `seconds`, from SurfaceFlinger."""
    layer = surface_layer(package)
    if not layer:
        print("frame pacing: the game's surface is not on screen")
        return
    shell(f"dumpsys SurfaceFlinger --latency-clear '{layer}'")
    time.sleep(seconds)
    rows = shell(f"dumpsys SurfaceFlinger --latency '{layer}'").splitlines()[1:]
    present = []
    for row in rows:
        parts = row.split()
        if len(parts) == 3 and parts[1].isdigit():
            t = int(parts[1])
            # not yet presented frames carry INT64_MAX, empty slots 0
            if 0 < t < (1 << 62):
                present.append(t)
    present = sorted(set(present))
    if len(present) < 3:
        print(f"frame pacing: too few frames ({len(present)}) from {layer}")
        return
    gaps = [(b - a) / 1e6 for a, b in zip(present, present[1:])]
    span = (present[-1] - present[0]) / 1e9
    ordered = sorted(gaps)
    pick = lambda q: ordered[min(len(ordered) - 1, int(q * len(ordered)))]
    text = (
        f"frame pacing over {span:.1f} s ({len(present)} frames, SurfaceFlinger keeps the last 127):\n"
        f"  average {len(gaps) / span:.1f} fps, median frame {statistics.median(gaps):.1f} ms\n"
        f"  95% of frames within {pick(0.95):.1f} ms, 99% within {pick(0.99):.1f} ms, worst {ordered[-1]:.1f} ms\n"
        f"  frames over 50 ms: {sum(g > 50 for g in gaps)}, over 100 ms: {sum(g > 100 for g in gaps)}"
    )
    print(text)
    if out:
        with open(os.path.join(out, "pacing.txt"), "w", encoding="utf-8") as f:
            f.write(text + "\n" + "\n".join(f"{g:.2f}" for g in gaps) + "\n")


def thread_dump(package, times=1, gap=3):
    for i in range(times):
        shell(f"touch {compose_dir(package)}/dump_threads")
        time.sleep(gap if i + 1 < times else 2)


def log_summary(path):
    """What the newest session in xe.log holds that matters."""
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    begin = max((i for i, l in enumerate(lines) if "CONFIG DUMP -----------" in l), default=0)
    lines = lines[begin:]
    counts = {
        "draws skipped": "skipping the draw",
        "host-side crashes": "Host-side crash",
        "no-frame reports": "no guest frame presented",
        "fiber not switching": "has not switched fibers",
        "XMA kicks unserviced": "unserviced",
        "thread dumps on request": "thread dump on request",
    }
    for label, needle in counts.items():
        n = sum(needle in l for l in lines)
        if n:
            print(f"  {label}: {n}")
    driver = next((l.split("driverID:")[1].strip() for l in lines if "driverID:" in l), "?")
    frame = next((m.group(1) for l in reversed(lines) if (m := re.match(r".> f:(\d+)", l))), "?")
    print(f"  driver {driver}, last frame {int(frame) if frame.isdigit() else frame}")


def grab(args):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = os.path.join(args.out, stamp)
    os.makedirs(out, exist_ok=True)
    package = args.package

    # the picture first: it is what goes stale
    with open(os.path.join(out, "now.png"), "wb") as f:
        f.write(adb("exec-out", "screencap", "-p", binary=True))
    recording = bool(shell(f"ls {PHONE_DIR}/.on 2>/dev/null").strip())
    if recording:
        # closes the piece being written; the loop starts the next
        shell("pkill -2 screenrecord")
        time.sleep(3)

    thread_dump(package, times=2)
    pacing(package, 6, out)

    adb("pull", f"{compose_dir(package)}/xe.log", os.path.join(out, "xe.log"))
    if os.path.exists(os.path.join(out, "xe.log")):
        print("xe.log:")
        log_summary(os.path.join(out, "xe.log"))
    with open(os.path.join(out, "logcat.txt"), "wb") as f:
        f.write(adb("logcat", "-d", "-t", "4000", binary=True))

    if recording:
        pieces = sorted(shell(f"find {PHONE_DIR} -name 'seg-*.mp4' -size +0 2>/dev/null").split())
        # the newest is the piece the loop has just begun: it has no index
        # until it is closed, and what just happened is in the one before it
        if len(pieces) > 1 and shell("pidof screenrecord").strip():
            pieces = pieces[:-1]
        wanted, need = [], args.seconds
        for piece in reversed(pieces):
            wanted.insert(0, piece)
            need -= SEGMENT_SECONDS
            if need <= 0:
                break
        frames = os.path.join(out, "frames")
        os.makedirs(frames, exist_ok=True)
        remaining = args.seconds
        for n, piece in reversed(list(enumerate(wanted))):
            local = os.path.join(out, os.path.basename(piece))
            adb("pull", piece, local)
            if not shutil.which("ffmpeg") or not os.path.exists(local):
                continue
            # the last `remaining` seconds of this piece, `--rate` frames a second
            subprocess.run(
                ["ffmpeg", "-v", "error", "-sseof", f"-{remaining}", "-i", local,
                 "-vf", f"fps={args.rate}", "-q:v", "3",
                 os.path.join(frames, f"p{n}-%04d.jpg")],
                check=False)
            length = subprocess.run(
                ["ffprobe", "-v", "error", "-show_entries", "format=duration",
                 "-of", "csv=p=0", local], capture_output=True, text=True).stdout.strip()
            try:
                remaining -= float(length)
            except ValueError:
                break
            if remaining <= 0:
                break
        print(f"frames: {len(os.listdir(frames))} in {frames}")
    else:
        print("no recording was running (device-watch.py start): only the screenshot")
    print(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.normpath(os.path.join(here, "..", "..", "..", "device-logs", "watch"))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["start", "grab", "fps", "dump", "stop"])
    ap.add_argument("seconds", nargs="?", type=int)
    ap.add_argument("--package", default="com.rfandango.haku_x.debug")
    ap.add_argument("--out", default=default_out)
    ap.add_argument("--rate", type=int, default=10, help="frames a second taken from the recording")
    args = ap.parse_args()
    if "device" not in adb("get-state"):
        raise SystemExit("no phone on adb (plugged in, USB debugging allowed?)")
    if args.command == "start":
        start(args)
    elif args.command == "stop":
        stop(args)
    elif args.command == "fps":
        pacing(args.package, args.seconds or 10)
    elif args.command == "dump":
        thread_dump(args.package, times=2)
        print(f"asked twice; read {compose_dir(args.package)}/xe.log")
    else:
        args.seconds = args.seconds or 20
        grab(args)


if __name__ == "__main__":
    sys.exit(main())
