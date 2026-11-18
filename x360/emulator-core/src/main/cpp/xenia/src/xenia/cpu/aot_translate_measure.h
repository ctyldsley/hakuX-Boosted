/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: aot_translate_measure. Released under the BSD license.      *
 ******************************************************************************
 */

#ifndef XENIA_CPU_AOT_TRANSLATE_MEASURE_H_
#define XENIA_CPU_AOT_TRANSLATE_MEASURE_H_

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace xe {
namespace cpu {

class Module;
class Processor;

// hakuX-Boosted: aot_translate_measure. A measurement, not an optimisation:
// how long would it take to translate the whole title executable ahead of
// time on this device, and what would it cost in memory?
//
// One background thread, at the lowest priority the host gives an
// unprivileged thread (nice 19 on Linux and Android), goes through every
// known function start of the executable (its .pdata exception table and
// the functions the loader had declared by the end of the load) and
// translates each through Processor::ResolveFunction, the path a guest
// thread takes on its first call. That path takes the entry table's and the
// symbol's per-function locks, so a function is translated once, by
// whichever thread gets there first, and the other waits for it; dispatch
// is not touched. While the thread is translating a function, a guest
// thread that has to wait for any function being translated lends it its
// own priority (BoostIfBusy, from the entry table's wait), as translate_ahead
// does for its helper.
//
// Every ~5 s a progress line is logged; at the end, a summary (counts, wall
// and thread CPU time, per-function time distribution, the slowest
// functions, failures, peak RSS, code cache use). All lines start
// "AOT measure:".
//
// Stop() sets a flag the thread checks between functions and joins it (it
// finishes the function it is on: other threads may be waiting on its
// entry). The Processor stops it when the executable is unloaded (before its
// guest memory is released) and in its destructor.
class AotTranslateMeasure {
 public:
  AotTranslateMeasure(Processor* processor, Module* module,
                      std::vector<uint32_t> addresses);
  ~AotTranslateMeasure();

  AotTranslateMeasure(const AotTranslateMeasure&) = delete;
  AotTranslateMeasure& operator=(const AotTranslateMeasure&) = delete;

  const Module* module() const { return module_; }

  void Start();
  void Stop();

  // True on the measuring thread.
  static bool IsMeasureThread();
  // Called by a thread about to wait for a function being translated: if
  // the measuring thread is in the middle of one, raise it to the caller's
  // priority until that function is done.
  static void BoostIfBusy();

 private:
  void Run();

  Processor* processor_ = nullptr;
  Module* module_ = nullptr;
  std::vector<uint32_t> addresses_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_AOT_TRANSLATE_MEASURE_H_
