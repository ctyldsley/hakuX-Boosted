/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: spin probe.                                                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// hakuX-Boosted: the spin probe. On request (a file called spin_probe beside
// the log file, picked up by the guest scheduler's watchdog within a second),
// for spin_probe_seconds the core records, per guest thread:
//  - where it is: at every watchdog tick each running fiber is asked to stop
//    at its next JIT safepoint, and the guest address of that safepoint (the
//    head of the block, usually a loop head) is counted. The address is
//    stamped into PPCContext::last_safepoint_pc by the yield path of the
//    safepoint only, so the check translated code runs every time costs
//    nothing more;
//  - every kernel and XAM export it calls, counted per call site (the guest
//    LR) with the first few calls' arguments and return values, taken from
//    the stall watchdog's call trace (forced on for the probe);
//  - the loads of the loop it stands at: at a sampled safepoint the guest
//    code from that address to the loop's back edge is decoded, and each
//    integer load's address is computed from the registers as they are at
//    the safepoint and read (through a pipe, so an unmapped address returns
//    EFAULT instead of faulting).
// At the end one report goes to the log, followed by the usual thread dump.
// With nothing requested nothing is recorded: the kernel call hook tests one
// flag in a path that already tests one, and the safepoint hook runs only
// when the watchdog raised the flag for the probe.

#ifndef XENIA_KERNEL_SPIN_PROBE_H_
#define XENIA_KERNEL_SPIN_PROBE_H_

#include <atomic>
#include <cstdint>

namespace xe {
namespace kernel {

class KernelState;
class XThread;

// True between SpinProbeBegin and SpinProbeEnd.
extern std::atomic<bool> g_spin_probe_active;

// From KernelCallTraceLeave, on the calling thread, after the export
// returned. args are r3..r8 at entry.
void SpinProbeNoteCall(uint32_t thread_id, const char* name, uint32_t lr,
                       const uint32_t* args, uint32_t result);

// From the safepoint handler, on the fiber's own thread, when the watchdog
// raised the preempt flag for the probe.
void SpinProbeNoteSafepoint(KernelState* kernel_state, XThread* self);

// The watchdog's side. Begin clears the tables and starts recording; End
// stops recording and writes the report. seconds is the window's length,
// for the rates.
void SpinProbeBegin();
void SpinProbeEnd(KernelState* kernel_state, double seconds,
                  uint32_t watchdog_period_us);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_SPIN_PROBE_H_
