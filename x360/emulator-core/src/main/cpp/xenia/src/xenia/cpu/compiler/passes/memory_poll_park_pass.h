/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_MEMORY_POLL_PARK_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_MEMORY_POLL_PARK_PASS_H_

#include <cstdint>
#include <vector>

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// hakuX-Boosted: an OPCODE_SPIN_BACKOFF this pass placed in a pause-and-poll
// loop (park_pause_poll_loops) carries this in Instr::flags, and the loop's
// guest address in the upper half of src1.offset (the lower half stays the
// unit count every other reader takes). The backend then calls the
// scheduler on every pass of the loop (a64_seq_memory.cc).
constexpr uint16_t kSpinBackoffPollSite = 1;

// Injects an adaptive backoff into indefinite guest memory-poll self-loops
// (load, test, branch back) so a long wait parks instead of burning a core.
// Removes nothing - every iteration still runs, only slower once the wait has
// proven long. Loops using LOAD_CLOCK, stores, calls or atomics are skipped.
class MemoryPollParkPass : public CompilerPass {
 public:
  MemoryPollParkPass();
  ~MemoryPollParkPass() override;

  bool Run(hir::HIRBuilder* builder) override;

 private:
  bool TryInstrumentLoop(hir::HIRBuilder* builder, hir::Block* block);
  // hakuX-Boosted: park_pause_poll_loops, see the .cc file.
  void MarkPausePollLoops(hir::HIRBuilder* builder);
  // The blocks of the loops it marked in the function being compiled.
  std::vector<hir::Block*> marked_blocks_;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_MEMORY_POLL_PARK_PASS_H_
