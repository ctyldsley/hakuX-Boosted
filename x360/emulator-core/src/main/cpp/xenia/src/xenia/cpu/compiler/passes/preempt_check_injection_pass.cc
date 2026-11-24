/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/preempt_check_injection_pass.h"

#include <unordered_set>

#include "xenia/base/cvar.h"
#include "xenia/cpu/hir/hir_builder.h"

DECLARE_bool(guest_scheduler);

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

PreemptCheckInjectionPass::PreemptCheckInjectionPass() : CompilerPass() {}

PreemptCheckInjectionPass::~PreemptCheckInjectionPass() {}

bool PreemptCheckInjectionPass::Run(HIRBuilder* builder) {
  // The bool return is pass success, not whether anything changed, and Compile
  // aborts the whole function on false.
  //
  // Read the cvar here, not in the ctor, so a per-title override applies.
  if (!cvars::guest_scheduler || !builder->first_block()) {
    return true;
  }
  // Blocks are laid out in guest address order, so every intra-function cycle
  // branches to an already-seen block. Calls, recursion and indirect branches
  // (bcctr lowers to CallIndirect) re-enter at the entry block.
  std::unordered_set<Block*> seen;
  std::unordered_set<Block*> check_blocks;
  // hakuX-Boosted: the check at a function's entry is there for cycles made
  // of calls (recursion, and indirect branches, which are calls here). A
  // function that calls nothing cannot be on such a cycle except through
  // its caller, and the caller is not a leaf: its own entry or the loop it
  // calls from has the check. So a function without a call of any kind has
  // no check at its entry (it keeps the ones at its loop heads, the entry
  // block's too if a branch comes back to it). Small leaf functions are
  // where the two instructions, and the tail behind them, weigh most.
  bool calls_something = false;
  for (auto block = builder->first_block(); block && !calls_something;
       block = block->next) {
    for (auto instr = block->instr_head; instr; instr = instr->next) {
      const OpcodeInfo* op = instr->opcode;
      // A return is an indirect call flagged CALL_POSSIBLE_RETURN (blr goes
      // to the address in the link register): not a call for this purpose.
      const bool is_return =
          (op == &OPCODE_CALL_INDIRECT_info ||
           op == &OPCODE_CALL_INDIRECT_TRUE_info) &&
          (instr->flags & CALL_POSSIBLE_RETURN);
      if (is_return) {
        continue;
      }
      if (op == &OPCODE_CALL_info || op == &OPCODE_CALL_TRUE_info ||
          op == &OPCODE_CALL_INDIRECT_info ||
          op == &OPCODE_CALL_INDIRECT_TRUE_info ||
          op == &OPCODE_CALL_EXTERN_info) {
        calls_something = true;
        break;
      }
    }
  }
  if (calls_something) {
    check_blocks.insert(builder->first_block());
  }
  for (auto block = builder->first_block(); block != nullptr;
       block = block->next) {
    seen.insert(block);
    // Scan every instruction: a back-edge is not always in the trailing
    // branch run. A loop ending its block with a call hides the loop branch
    // from a tail-backward walk, leaving it to spin with no safepoint.
    for (auto instr = block->instr_head; instr != nullptr;
         instr = instr->next) {
      Label* label = nullptr;
      if (instr->opcode == &OPCODE_BRANCH_info) {
        label = instr->src1.label;
      } else if (instr->opcode == &OPCODE_BRANCH_TRUE_info ||
                 instr->opcode == &OPCODE_BRANCH_FALSE_info) {
        label = instr->src2.label;
      }
      if (label && label->block && seen.count(label->block)) {
        check_blocks.insert(label->block);
      }
    }
  }
  for (auto block : check_blocks) {
    // A block holding only fake instructions falls through, so the check
    // lands in the first real successor, still on every cycle through it.
    for (auto b = block; b != nullptr; b = b->next) {
      auto first = b->instr_head;
      for (; first && first->IsFake(); first = first->next) {
      }
      if (first) {
        if (first->GetOpcodeNum() != OPCODE_CHECK_PREEMPT) {
          // Carry the guest address of the safepoint so the backend can record
          // where a fiber last checked in. A fiber that stops yielding is
          // otherwise only locatable by its link register, which points at the
          // last call it made rather than the loop it is stuck in.
          uint32_t guest_address = 0;
          for (auto s = b->instr_head; s; s = s->next) {
            if (s->opcode == &OPCODE_SOURCE_OFFSET_info) {
              guest_address = uint32_t(s->src1.offset);
              break;
            }
          }
          Instr* check = builder->CheckPreempt();
          check->src1.offset = guest_address;
          check->MoveBefore(first);
        }
        break;
      }
    }
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
