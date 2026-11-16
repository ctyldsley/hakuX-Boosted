/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_REGISTER_ALLOCATION_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_REGISTER_ALLOCATION_PASS_H_

#include <algorithm>
#include <bitset>
#include <unordered_map>
#include <functional>
#include <vector>

#include "xenia/cpu/backend/machine_info.h"
#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

class RegisterAllocationPass : public CompilerPass {
 public:
  explicit RegisterAllocationPass(const backend::MachineInfo* machine_info);
  ~RegisterAllocationPass() override;

  bool Run(hir::HIRBuilder* builder) override;

 private:
  // TODO(benvanik): rewrite all this set shit -- too much indirection, the
  // complexity is not needed.
  struct RegisterUsage {
    hir::Value* value;
    hir::Value::Use* use;
    RegisterUsage() : value(nullptr), use(nullptr) {}
    RegisterUsage(hir::Value* value_, hir::Value::Use* use_)
        : value(value_), use(use_) {}
    static bool Compare(const RegisterUsage& a, const RegisterUsage& b) {
      return a.use->instr->ordinal < b.use->instr->ordinal;
    }
  };
  struct RegisterSetUsage {
    const backend::MachineInfo::RegisterSet* set = nullptr;
    uint32_t count = 0;
    std::bitset<32> availability = 0;
    // hakuX-Boosted: registers of this set that hold pinned guest registers
    // in the function being allocated, and so are never handed to a value.
    std::bitset<32> reserved = 0;
    // TODO(benvanik): another data type.
    std::vector<RegisterUsage> upcoming_uses;
  };

  void DumpUsage(const char* name);
  void PrepareBlockState();
  void AdvanceUses(hir::Instr* instr);
  bool IsRegInUse(const hir::RegAssignment& reg);
  RegisterSetUsage* MarkRegUsed(const hir::RegAssignment& reg,
                                hir::Value* value, hir::Value::Use* use);
  RegisterSetUsage* MarkRegAvailable(const hir::RegAssignment& reg);

  bool TryAllocateRegister(hir::Value* value,
                           const hir::RegAssignment& preferred_reg);
  bool TryAllocateRegister(hir::Value* value);
  bool SpillOneRegister(hir::HIRBuilder* builder, hir::Block* block,
                        hir::TypeName required_type);

  RegisterSetUsage* RegisterSetForValue(const hir::Value* value);

  void SortUsageList(hir::Value* value);

  // hakuX-Boosted: pin_guest_registers_coalesce. Decides, for one block,
  // which values live in the host register of a pinned guest register
  // instead of one of the allocator's own (see the comment above the
  // definition). The answers are in pin_homes_.
  void PlanPinHomes(hir::HIRBuilder* builder, hir::Block* block);
  struct PinHome {
    RegisterSetUsage* set;
    uint32_t reg_index;
    uint8_t slot;     // index into the builder's pinned_context_slots()
    bool is_def;      // computed into the pin (not a load of it)
    bool is_truncate = false;  // a 32-bit truncation of a value living there
  };
  std::unordered_map<const hir::Value*, PinHome> pin_homes_;
  // hakuX-Boosted: register_allocation_elide_copies. True if the instruction
  // only copies its operand (an assignment, a truncation to 32 bits whose
  // result is only ever read as 32 bits, a denormal flush on a machine
  // where that is a copy), so that its result can share the operand's
  // register.
  bool IsElidableCopy(const hir::Instr* instr) const;
  const backend::MachineInfo* machine_info_ = nullptr;

 private:
  struct {
    RegisterSetUsage* int_set = nullptr;
    RegisterSetUsage* float_set = nullptr;
    RegisterSetUsage* vec_set = nullptr;
    RegisterSetUsage* all_sets[3];
  } usage_sets_;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_REGISTER_ALLOCATION_PASS_H_
