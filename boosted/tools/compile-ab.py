"""Shader compile stutter in the first seconds of one launch, for an A/B.

  python boosted/tools/compile-ab.py <xe.log> [seconds, default 60] [<xe.log> ...]

Needs shader_profiling = true. Time is the sum of the once-a-second
"first run split: <ms> ms" lines, so it starts at the first guest draw or
frame, not at app start. For each log, within that window:

- which path the run took ("pipeline libraries: in use yes|no" at start-up);
- frames, the worst frame, and the seconds whose longest frame was over
  100 ms and over 250 ms;
- the last "pipeline compile totals:" line in the window (real pipelines:
  count, total ms, over 100 ms, slowest, linked from libraries;
  placeholders and stand-ins);
- the last "pipeline compiles by shaders the driver had compiled before:"
  line;
- with libraries on, the last "placeholder cover: pipeline libraries:" line;
- errors that matter: "pipeline libraries: failed", "Failed to create",
  device loss.

Several logs side by side: give each path after the seconds.
"""
import re
import sys


def summarise(path, window_s):
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    in_use = None
    elapsed = 0.0
    started = False
    frames = 0
    worst = 0.0
    over_100 = over_250 = 0
    last = {}
    errors = []
    for line in lines:
        m = re.search(r'pipeline libraries: in use (yes|no)', line)
        if m and in_use is None:
            in_use = m.group(1)
        m = re.search(
            r'first run split: (\d+) ms, (\d+) frame\(s\) \(longest ([\d.]+) ms\)',
            line)
        if m:
            started = True
            if elapsed >= window_s:
                break
            elapsed += int(m.group(1)) / 1000.0
            frames += int(m.group(2))
            longest = float(m.group(3))
            worst = max(worst, longest)
            over_100 += longest > 100.0
            over_250 += longest > 250.0
            continue
        if started and elapsed >= window_s:
            continue
        for key, pattern in (
                ('totals', 'pipeline compile totals:'),
                ('seen', 'pipeline compiles by shaders the driver had compiled before:'),
                ('libraries', 'placeholder cover: pipeline libraries:')):
            if pattern in line:
                last[key] = line[line.index(pattern):]
        if re.search(r'pipeline libraries: failed|Failed to create|DEVICE_LOST|device lost',
                     line):
            errors.append(line.strip())
    print(f'== {path}')
    print(f'pipeline libraries in use: {in_use}')
    print(f'window: {elapsed:.0f} s from the first draw; frames {frames}; worst frame '
          f'{worst:.1f} ms; seconds with a frame over 100 ms {over_100}, over 250 ms '
          f'{over_250}')
    for key in ('totals', 'seen', 'libraries'):
        if key in last:
            print(last[key])
    print(f'errors: {len(errors)}')
    for e in errors[:8]:
        print('  ' + e)
    print()


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    paths = [args[0]]
    window_s = 60.0
    rest = args[1:]
    if rest:
        try:
            window_s = float(rest[0])
            rest = rest[1:]
        except ValueError:
            pass
    paths += rest
    for path in paths:
        summarise(path, window_s)


if __name__ == '__main__':
    main()
