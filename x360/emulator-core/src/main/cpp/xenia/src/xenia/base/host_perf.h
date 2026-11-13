/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: where the host runs the emulator's threads, and how the     *
 * phone is told what they need. See host_perf.cc and                         *
 * boosted/notes/thread-placement.md.                                         *
 ******************************************************************************
 */

#ifndef XENIA_BASE_HOST_PERF_H_
#define XENIA_BASE_HOST_PERF_H_

namespace xe {
namespace host_perf {

// Called once per presented guest frame, by the thread that presents
// (xe::RecordGuestPresent in base/frame_stats.h, the GPU command thread).
//
// With every host_perf_* setting off this is three loads and a return and
// nothing else in this file ever runs. The first call with one of them on
// starts a worker thread that, once a second, finds the emulator's threads by
// name, applies the core placement policy, keeps the performance hint session
// and writes the log line. The call itself reports the frame's work duration
// to the hint session when there is one.
void OnGuestFramePresented();

}  // namespace host_perf
}  // namespace xe

#endif  // XENIA_BASE_HOST_PERF_H_
