/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: constants in guest registers, carried across blocks.        *
 * Released under the BSD license.                                            *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_CONTEXT_CONSTANT_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_CONTEXT_CONSTANT_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// ContextPromotionPass forwards a value stored to a guest register to later
// loads of it in the same block. A constant stored in one block and loaded
// in another (li r3, 0 before a branch, the use after it) is still a load
// from memory there, and nothing downstream can fold it.
//
// This pass finds, for each load of an integer guest register or flag byte,
// whether every path to it last stored the same constant with nothing in
// between that could have changed the register behind the function's back
// (a call, a trap, a non-branch barrier: anything volatile), and if so
// replaces the load with the constant. It runs before the simplification
// and constant propagation group, which then folds what the constant feeds.
//
// A no-op unless propagate_context_constants is set.
class ContextConstantPass : public CompilerPass {
 public:
  ContextConstantPass();
  ~ContextConstantPass() override;

  bool Run(hir::HIRBuilder* builder) override;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_CONTEXT_CONSTANT_PASS_H_
