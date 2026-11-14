# The translator, overhauled: registers, calls, flags, memory

Branch `claude/translator-overhaul`, cut from `claude/integration-1115` at
`5aea2e2`. Written from 2026-11-14 by a cloud session. It follows
`boosted/notes/translator-wide.md` (the tests, wide pinning, the cheaper-code
settings) and `boosted/notes/cpu-translation.md` (the study).

**Nothing here has run on a device.** Every figure is either a count of host
instructions run per guest instruction on the bench loops (exact; the same
count on the workflow's ARM64 runner and under qemu in the session's
container, see "How it is measured") or says what else it is. No time is
quoted as a speed-up: runner times are runner times.

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/` unless
they start with another directory.

## Done / in progress / next

Done:
- Step 0: why the "Translator tests" job failed on `claude/integration-1115`
  (Actions run 135), and the fix (section 0).
- A way to count host instructions per guest instruction without a
  performance counter (`boosted/bench/tools/xlat-count-qemu.sh`, section
  "How it is measured").

In progress:
- The register model (section 1).

Next: see the end of this file.

## 0. The failed "Translator tests" job of run 135

It was neither an interaction between the merged branches nor a break of the
small test build. It was the allocator abort the earlier notes describe, and
it has a cause that can be named and removed.

What failed: one line of the matrix, the last, "everything, 16k". Two of its
127 processes aborted with `Fatal glibc error: malloc.c:2599 (sysmalloc):
assertion failed: ... ((unsigned long) old_end & (pagesize - 1)) == 0`; every
test that did run passed (138,919 of 141,899) and no state differed from the
reference. Every other line passed, "everything" at 4 KiB pages among them.
The same line passes in the session's container, 141,899 of 141,899, and the
failing process's exact list of suites passed 24 times of 24 there.

The cause is in how qemu-user starts a program when it is asked for a page
size that is not the host's (`-p 16384` on a 4 KiB host):
- The program break starts at the end of the executable's data, which is
  4 KiB aligned, not 16 KiB aligned (seen with `qemu -strace`: the test
  runner's first `brk(NULL)` returns an address ending `...ee000`).
- glibc's allocator copes with that only if its first `brk()` succeeds (it
  then pads the break to a page boundary itself). Under qemu the first
  `brk()` usually fails, because one of qemu's own mappings sits a few pages
  above the executable (seen from the host's `/proc/<pid>/maps`). The
  allocator then switches to `mmap` and from there on assumes that any later
  `brk()` returns page-aligned memory, as it would on a real kernel.
- When qemu frees that mapping of its own while the tests run, the next
  `brk()` succeeds from the unaligned break, the allocator's top chunk ends
  off a page boundary, and the next time the heap has to grow the assertion
  fires.

That explains everything the earlier session saw and could not: it happens
with every translator setting off, only at 16 KiB, at a point that depends on
the machine (where the host placed qemu's mappings and when qemu frees one),
shorter processes make it rarer but not impossible, glibc's heap checker
finds nothing (there is no overrun), and telling glibc to keep large blocks
off the `brk` heap did not help (the top chunk is still grown with `brk`).
The merge moved nothing but the timing.

What I could and could not confirm: the unaligned break, the failing first
`brk()` and qemu's mapping above the executable are observed in the
container; that an unaligned top chunk fails this assertion is read from
glibc's `sysmalloc`. The abort itself never happened in the container (there
`brk()` keeps failing for the whole run, so the allocator stays on `mmap`),
so the last step, qemu freeing the blocking mapping on GitHub's runner, is
inferred, not seen.

The fix (`cpu/ppc/testing/ci/stubs.cc`, test build only): a constructor that
runs before anything else and, only when the break is not page aligned,
moves it up to a page boundary and maps one inaccessible page there. `brk()`
can then never grow, so the allocator stays on `mmap` for the whole run,
which is the state every passing run in the container was in. On a real
kernel (and at 4 KiB under qemu) the break is aligned and nothing is done.
The runner prints one line when it applies.

The matrix still runs the `#16k` lines in processes of at most 1,500 tests.
With the cause removed that should no longer be needed, and it costs time
(a process takes 14 s to start under qemu): the next thing to try is one
`#16k` line unsplit. Not done in the same push as the fix, so that a green
job says the fix did no harm before anything else changes.

## How it is measured

