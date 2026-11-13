/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: chooses the guest registers a function keeps in host        *
 * registers. Released under the BSD license.                                 *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_GUEST_REGISTER_PINNING_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_GUEST_REGISTER_PINNING_PASS_H_

#include "xenia/cpu/backend/machine_info.h"
#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Analysis only: it changes no instruction. It fills
// HIRBuilder::pinned_context_slots() with the guest registers (PPCContext
// slots) that the function will hold in a host register for its whole
// length, and nothing else. Must run after every pass that adds or removes
// context loads and stores and directly before RegisterAllocationPass, which
// keeps its own values out of the registers named here.
//
// With pin_guest_registers off (the default) the list stays empty and the
// allocator and the backend behave exactly as before.
class GuestRegisterPinningPass : public CompilerPass {
 public:
  explicit GuestRegisterPinningPass(const backend::MachineInfo* machine_info);
  ~GuestRegisterPinningPass() override;

  bool Run(hir::HIRBuilder* builder) override;

 private:
  uint32_t int_register_count_ = 0;
  uint32_t vec_register_count_ = 0;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_GUEST_REGISTER_PINNING_PASS_H_
