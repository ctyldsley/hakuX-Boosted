/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: removes stores to guest registers and flag bits that no     *
 * path reads. Released under the BSD license.                                *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_DEAD_CONTEXT_STORE_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_DEAD_CONTEXT_STORE_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// ContextPromotionPass drops a context store only when a later store in the
// same block overwrites it. This pass does the same over the whole function:
// a store is dropped when, on every path from it, the slot is stored again
// before anything can read it. "Anything" is a context load in this
// function, or any point where code outside the function's own instructions
// may look at the context (calls, returns, traps, the scheduler's check, the
// front end's own barriers, the end of the function).
//
// What it is for: a compare writes three condition register bits and the
// branch after it reads one; the other two are stored only to be overwritten
// by the next compare, usually in another block. With the store gone the
// compare that produced the bit dies too.
//
// A no-op unless eliminate_dead_context_stores is set.
class DeadContextStorePass : public CompilerPass {
 public:
  DeadContextStorePass();
  ~DeadContextStorePass() override;

  bool Run(hir::HIRBuilder* builder) override;
};

// The values dead_context_store_verify stores in place of a store it would
// have removed, so that a read of one can be recognised (by the backend, in
// the same mode). A condition register or XER byte only ever holds 0 or 1.
constexpr uint8_t kDeadStorePoison8 = 0xA5;
constexpr uint64_t kDeadStorePoison64 = 0xDEAD5707E5707E5Dull;

// Which context slots the pass handles: 1 = the condition register and XER
// bytes; 2 = also r0, r3 to r12, r14 to r31, CTR as 64-bit integers.
bool IsDeadStoreCandidateSlot(uint32_t offset, uint32_t size, int level);

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_DEAD_CONTEXT_STORE_PASS_H_
