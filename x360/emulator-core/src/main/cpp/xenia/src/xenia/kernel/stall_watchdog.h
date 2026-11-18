/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: stall_watchdog.                                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// hakuX-Boosted: the stall watchdog (cvar stall_watchdog, on by default; it
// only observes). Every guest thread keeps a ring of the last kernel calls it
// made, written by the export trampoline (kernel/util/shim_utils.h). A host
// thread looks once a second: when frames keep being presented but no guest
// thread other than the one calling VdSwap has made or returned from a kernel
// call for stall_watchdog_seconds, it writes one STALL REPORT to the log with
// what every guest thread is doing, waiting on and last called.

#ifndef XENIA_KERNEL_STALL_WATCHDOG_H_
#define XENIA_KERNEL_STALL_WATCHDOG_H_

#include <atomic>
#include <cstdint>

#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace kernel {

class KernelState;

// One kernel call. Written only by the thread that made the call; the
// watchdog reads it without a lock, so a torn record is possible and is
// detected (seq is cleared while the record is rewritten).
struct KernelCallRecord {
  const char* name;    // export name, static storage (cpu::Export::name)
  uint64_t host_tick;  // Clock::QueryHostTickCount() at entry
  uint32_t seq;        // 1-based call number on this thread, 0 = empty/torn
  uint32_t lr;         // guest LR at the call, the return address in the title
  uint32_t args[6];    // r3..r8 at entry
  uint32_t result;     // r3 on return (0 for exports returning void)
  uint32_t frame;      // logging::GetFrameNumber() at entry
  uint32_t returned;   // 0 while the call is still in progress
};

struct KernelCallTrace {
  static constexpr uint32_t kSize = 16;
  KernelCallRecord records[kSize] = {};
  // Calls made in all; record seq lives at records[(seq - 1) % kSize].
  std::atomic<uint32_t> count{0};
  // Host tick of the last kernel call entry or return on this thread.
  std::atomic<uint64_t> last_activity_tick{0};
};

// Set once by StallWatchdogStart before any guest code runs.
extern bool g_kernel_call_trace_enabled;

// Called by the export trampoline around every kernel call. Enter returns the
// record's seq (0 when the context has no guest thread), which Leave takes.
uint32_t KernelCallTraceEnter(cpu::ppc::PPCContext* ppc_context,
                              const char* name);
void KernelCallTraceLeave(cpu::ppc::PPCContext* ppc_context, uint32_t seq,
                          uint32_t result);

// Lifetime of the watchdog host thread, tied to KernelState.
void StallWatchdogStart(KernelState* kernel_state);
void StallWatchdogStop();

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_STALL_WATCHDOG_H_
