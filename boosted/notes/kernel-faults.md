# Kernel faults found by the xsdk probes (`claude/kernel-faults`)

Written 2026-11-23 from the build 149 run (`device-logs/xsdk/report.md`, outside the repository) against the
console's logs (`xsdk-probes/results/`). Built by CI only: **nothing here has run on the phone.** Each item says
which titles to rerun (`tools/run-xsdk.sh`, then `tools/xsdk-compare.py`).

## 1. Wait-all that had to block returns the index of the object that completed it

**What the probe showed.** "objects: pending mixed wait-all completes after the target thread ends" (xsdk_test,
m55_check) failed, but not because the wait hung. Our log: `mixed=0x00000000 elapsed=21335779 ns`; the console's:
`mixed=0x00000003 elapsed=20708933 ns`. The wait completed at the same time on both. The console's kernel returns
`STATUS_WAIT_0 + 3`, the position of the thread, the object whose signal finished the wait; we returned
`STATUS_WAIT_0`. With everything signalled on entry the console returns 0 (the "ready control", which passes
already). `ref/xsdk/docs/HARDWARE.md`, "M55 diagnostics v3", records the same.

So the freeze class ("game tick stops while rendering continues") is **not** explained by this probe: a thread's end
already wakes a pending wait-all on the guest scheduler's cooperative path (the run had `guest_scheduler = true`; a
set holding a thread is never gated on the signal epoch, so it is re-polled), within 0.6 ms of the console's time. What can break is a title that tests a wait-all's
result for exactly 0, or uses the index; titles were written against the console's behaviour, so ours was the odd one.

**Change.** `XObject::signal_stamp()`: one global counter, stamped on an object when it becomes signalled and
**before** the host primitive is signalled, so a waiter that wakes always sees it: `XEvent::Set` (only from
unsignalled, as the console's `KeSetEvent` only tests waits then), `XEvent::Pulse`, `XSemaphore::ReleaseSemaphore`,
mutant free on release and on abandon, and thread end (`XThread::Exit`, `XThread::Terminate`). `WaitMultiple` reads
the counter on entry; on a successful wait-all it returns the index of the object with the newest stamp after entry,
or 0 when none is newer (everything was already signalled). Both the cooperative and the host path do this.
Abandoned results are unchanged (`STATUS_ABANDONED_WAIT_0`).

**Risks.** Objects that never stamp (timers, files, other types) read as "signalled before entry", so a wait-all
completed by a timer returns the index of the newest stamped object instead, or 0. A semaphore release that fails
(over its maximum) still stamps. Any title code that compares a wait-all with exactly 0 now sees nonzero, as on the
console. The stamp is a relaxed global `fetch_add` on each signal: negligible next to the host signal itself.

**Rerun:** `m55_check`, `xsdk_test`, `probe_objects`.

## 2. `PAGE_NOACCESS` reads back

**What the probe showed.** "pages: no-access protection restores to writable": `mprotect(PROT_NONE)` succeeded,
but `MmQueryAddressProtect` then returned 0 instead of `PAGE_NOACCESS` (1); a committed page with no access and an
uncommitted page looked the same. The `0xC0000022` (`STATUS_ACCESS_DENIED`) in `xe.log` comes from the next check,
`mprotect` on the hole left by a partial unmap, which the console also refuses and which passes; it is not this fault.

**Change.** `MmQueryAddressProtect`: when the page's protection is 0, the heap's region info decides: committed gives
`X_PAGE_NOACCESS`, otherwise 0 as before.

**Risks.** Low. A title that read 0 as "unmapped" for its own no-access pages now sees 1, as on the console.

**Rerun:** `xsdk_test`.

## 3. Virtual commits and frees move the kernel's free-page count

**What the probe showed.** `MmQueryStatistics` held `title.available_pages` at 125879 through everything: 16 MB of
heap growth, a 256 KB unmap, freeing 32 MB at the heap top. It only counted the physical heaps' reservations, while
malloc's heap and maps live in the virtual heaps (`NtAllocateVirtualMemory`). `title.reserved_virtual_memory_bytes`
was 19 MB against the console's 464 MB, so "sbrk: the heap's reservation covers the title's free memory" failed too.

**Change.** `BaseHeap::committed_page_count()`, kept at every change of a page's commit bit (`AllocFixed`, `Alloc`,
`Decommit`, `Release`, `Reset`, `Restore`). `MmQueryStatistics` adds the committed pages of the 4 KB and 64 KB virtual
heaps (in 4 KB pages) to the used pages, and their reservations to the reserved virtual bytes. Used pages are clamped
so the free count never goes below one page.

**Risks.** Titles that size their pools from `available_pages` now see less free memory, as on a console. Our virtual
heaps can hold more commits than a console's 512 MB could back; then the count bottoms out at 1 instead of wrapping.
The title-pages breakdown (`physical_pages`, `heap_pages` and the rest) is still the old fixed guesses. Commits made
straight through the physical heaps are counted as before (by reservation).

**Rerun:** `xsdk_test` (sbrk, `MmQueryStatistics`, `pages:` checks), `m55_check` (its 32 MB heap check).

## 4. Ordinal `0x36D`: not done

`probe_fault` looks up xboxkrnl ordinal `0x36D` (877), `RtlSetVectoredExceptionHandler`, and `0x36E`, its removal.
Xenia names both and implements neither. Registering the handler is easy, but the console calls it: on a store to 0
the handler ran on the faulting thread with a pointer to `{ record, context }` on the stack, code `C0000005`, the
faulting address and the two access-violation parameters (`xsdk-probes/results/2026-10-15-milestone-40-probe/`), and
may resume at the context. Doing that means turning a host fault in translated code (and illegal instruction and
trap) into a guest exception record and a guest CONTEXT, calling the guest handler on that thread and resuming from
the context it returns: work in the a64 backend's fault path, not a kernel export. A registration-only stub would
let the probe import it and then still crash, and would hide the gap from titles, so it was left out. It is a task of
its own.

## 5. `KeSave`/`KeRestore` floating-point and vector state

**What was wrong.** All four exports were unimplemented (the old `KeSaveFloatingPointState` in `xboxkrnl_misc.cc` was
inside `#if 0`): a caller got 0 and the registers its kernel-side code clobbered were never put back. No xsdk probe
covers them.

**Change.** As rexglue (`ref/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_threading.cpp`): no arguments, one save area
per thread (`XThread::fpu_vmx_save_area`, made on first use). Save copies f0 to f31 and FPSCR, or v0 to v127 and VSCR;
restore copies them back, and the floating-point restore also sets the host's rounding and flush-to-zero from FPSCR's
RN and NI. Save leaves the registers as they are.

**Risks.** Unknown on the console: whether save also clears FPSCR (the reverse-engineered code in `#if 0` set it to
0) and whether the calls nest. One slot per thread, as rexglue. A probe that saves, clobbers and restores on the
console would settle both.

**Rerun:** none covers it; any title that imports them (the xex import list in `xe.log` shows it).

## Rerun list

`m55_check`, `xsdk_test`, `xsdk_test_nohizpass`, `probe_objects`, then `tools/xsdk-compare.py` against the console's
logs. Expected to newly pass: the pending mixed wait-all, the no-access read-back, the unmap and 32 MB heap returns,
`MmQueryStatistics` heap growth and the sbrk reservation. `xsdk_test` still stops at the `jit:` checks
(self-written code, a separate Rewrite 1 item).
