/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: counters behind log_translation_stats that code outside     *
 * compiler.cc adds to. Released under the BSD license.                       *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_TRANSLATION_STATS_H_
#define XENIA_CPU_COMPILER_TRANSLATION_STATS_H_

#include <atomic>
#include <string>
#include <cstdint>

namespace xe {
namespace cpu {
namespace compiler {

// Totals since launch, over every translator. All are counts of things in
// translated code or of translation work; none is a measurement of speed.
// Updated with relaxed atomics whether or not log_translation_stats is on
// (a handful of increments per translated function); printed only when it is.
struct TranslationStats {
  // pin_guest_registers.
  std::atomic<uint64_t> pin_functions_considered{0};
  std::atomic<uint64_t> pin_functions_with_loops{0};
  std::atomic<uint64_t> pin_functions_pinned{0};
  std::atomic<uint64_t> pin_gpr_slots{0};
  std::atomic<uint64_t> pin_fpr_slots{0};
  std::atomic<uint64_t> pin_vr_slots{0};
  // Context loads and stores of pinned slots, i.e. emitted as register moves.
  std::atomic<uint64_t> pin_loads{0};
  std::atomic<uint64_t> pin_stores{0};
  // Places in emitted code where the pinned registers are written back to
  // the context and (usually) read back after.
  std::atomic<uint64_t> pin_sync_sites{0};
  std::atomic<uint64_t> pin_resume_stubs{0};
  // Run time: a longjmp-style return into the middle of a function with
  // pinned registers went through its resume stub.
  std::atomic<uint64_t> pin_resume_entries{0};
  // Run time: mismatches reported by a64_pin_verify.
  std::atomic<uint64_t> pin_verify_mismatches{0};
  // Which guest register each pinned integer slot was (r0..r31, 32 = ctr,
  // 33 = lr).
  std::atomic<uint64_t> pin_by_gpr[34];
  // pin_guest_registers_wide.
  std::atomic<uint64_t> pin_functions_pinned_no_loop{0};
  std::atomic<uint64_t> pin_flag_slots{0};
  // Static: the loads and stores emitted to keep pinned registers and the
  // context in step (at entry, round calls, at exits), to set against
  // pin_loads and pin_stores.
  std::atomic<uint64_t> pin_sync_loads{0};
  std::atomic<uint64_t> pin_sync_stores{0};
  // Static: blocks translated in functions with and without pinned
  // registers.
  std::atomic<uint64_t> blocks_pinned{0};
  std::atomic<uint64_t> blocks_unpinned{0};
  // Run time, only counted in code translated with log_translation_stats
  // on: blocks executed in functions with and without pinned registers.
  // Plain memory the translated code increments without a lock.
  uint64_t blocks_run_pinned = 0;
  uint64_t blocks_run_unpinned = 0;

  // simplification_skip_unchanged_blocks.
  std::atomic<uint64_t> simplify_block_walks{0};
  std::atomic<uint64_t> simplify_block_skips{0};
  // simplification_skip_verify: a walk changed a block the skip would have
  // passed over. Any value but zero means the skip is not exact.
  std::atomic<uint64_t> simplify_skip_mismatches{0};
};

extern TranslationStats translation_stats;

// The TranslationStats lines as text, now (log_translation_stats logs them
// every 2048 functions; a short run, a test run among them, ends before
// that).
std::string FormatTranslationStats();

}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_TRANSLATION_STATS_H_
