/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/register_allocation_pass.h"

#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/compiler/translation_stats.h"

// hakuX-Boosted: see RegisterAllocationPass::PlanPinHomes.
DEFINE_bool(
    pin_guest_registers_coalesce, false,
    "With pin_guest_registers: a value loaded from a pinned guest register "
    "is used from the pin's host register directly, and a value that is "
    "about to be stored to a pinned guest register is computed straight "
    "into the pin's host register, wherever the block's code allows it. "
    "The register moves that pinning otherwise leaves in place of every "
    "load and store then disappear, so that 'add r4, r4, r5' with all three "
    "pinned becomes one host instruction. Takes effect for functions "
    "translated after it is set.",
    "CPU");
DEFINE_bool(
    pin_guest_registers_coalesce_verify, false,
    "Self-check for pin_guest_registers_coalesce, for a checking run only: "
    "every value the setting would keep in a pin's host register gets a "
    "register of its own as well, the pin is written exactly where the "
    "setting would write it, and each use of such a value first compares "
    "the two. A difference means the setting would have used a pin whose "
    "contents had changed; it is counted and logged (TranslatorVerify:). "
    "Slower code. Takes effect for functions translated after it is set.",
    "CPU");
DEFINE_bool(
    register_allocation_from_top, false,
    "For the translator's tests, not for play: the register allocator hands "
    "out the highest numbered free host register instead of the lowest, so "
    "that the registers it otherwise reaches only in crowded blocks (x9-x15 "
    "with a64_extended_gpr_pool) hold values in every test.",
    "CPU");

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::backend::MachineInfo;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::hir::OpcodeSignatureType;
using xe::cpu::hir::RegAssignment;
using xe::cpu::hir::TypeName;
using xe::cpu::hir::Value;

#define ASSERT_NO_CYCLES 0

RegisterAllocationPass::RegisterAllocationPass(const MachineInfo* machine_info)
    : CompilerPass() {
  // Initialize register sets.
  // TODO(benvanik): rewrite in a way that makes sense - this is terrible.
  auto mi_sets = machine_info->register_sets;
  std::memset(&usage_sets_, 0, sizeof(usage_sets_));
  uint32_t n = 0;
  while (mi_sets[n].count) {
    auto& mi_set = mi_sets[n];
    auto usage_set = new RegisterSetUsage();
    usage_sets_.all_sets[n] = usage_set;
    usage_set->count = mi_set.count;
    usage_set->set = &mi_set;
    if (mi_set.types & MachineInfo::RegisterSet::INT_TYPES) {
      usage_sets_.int_set = usage_set;
    }
    if (mi_set.types & MachineInfo::RegisterSet::FLOAT_TYPES) {
      usage_sets_.float_set = usage_set;
    }
    if (mi_set.types & MachineInfo::RegisterSet::VEC_TYPES) {
      usage_sets_.vec_set = usage_set;
    }
    n++;
  }
}

RegisterAllocationPass::~RegisterAllocationPass() {
  for (size_t n = 0; n < xe::countof(usage_sets_.all_sets); n++) {
    if (!usage_sets_.all_sets[n]) {
      break;
    }
    delete usage_sets_.all_sets[n];
  }
}

bool RegisterAllocationPass::Run(HIRBuilder* builder) {
  // Simple per-block allocator that operates on SSA form.
  // Registers do not move across blocks, though this could be
  // optimized with some intra-block analysis (dominators/etc).
  // Really, it'd just be nice to have someone who knew what they
  // were doing lower SSA and do this right.

  // hakuX-Boosted: take the host registers GuestRegisterPinningPass gave to
  // guest registers out of each set for this function. With nothing pinned
  // (pin_guest_registers off) every mask is empty and nothing changes.
  for (size_t n = 0; n < xe::countof(usage_sets_.all_sets); ++n) {
    if (usage_sets_.all_sets[n]) {
      usage_sets_.all_sets[n]->reserved.reset();
    }
  }
  for (const auto& slot : builder->pinned_context_slots()) {
    RegisterSetUsage* usage_set =
        slot.reg_set == 0 ? usage_sets_.int_set : usage_sets_.vec_set;
    if (usage_set && slot.reg_index < usage_set->count) {
      usage_set->reserved.set(slot.reg_index);
    }
  }

  uint16_t block_ordinal = 0;
  uint32_t instr_ordinal = 0;
  auto block = builder->first_block();
  while (block) {
    // Sequential block ordinals.
    block->ordinal = block_ordinal++;

    // Reset all state.
    PrepareBlockState();

    // Renumber all instructions in the block. This is required so that
    // we can sort the usage pointers below.
    auto instr = block->instr_head;
    while (instr) {
      // Sequential global instruction ordinals.
      instr->ordinal = instr_ordinal++;
      instr = instr->next;
    }

    // hakuX-Boosted: which values of this block live in a pin.
    pin_homes_.clear();
    // (Only with pin_guest_registers_wide: the first form of pinning reloads
    // every pin at every barrier and has no per-instruction masks for the
    // backend to adjust.)
    if (cvars::pin_guest_registers_coalesce &&
        builder->pin_sync().by_liveness &&
        !builder->pinned_context_slots().empty()) {
      PlanPinHomes(builder, block);
    }

    instr = block->instr_head;
    while (instr) {
      const auto info = instr->opcode;
      uint32_t signature = info->signature;

      // Update the register use heaps.
      AdvanceUses(instr);

      // Check sources for retirement. If any are unused after this instruction
      // we can eagerly evict them to speed up register allocation.
      // Since X64 (and other platforms) can often take advantage of dest==src1
      // register mappings we track retired src1 so that we can attempt to
      // reuse it.
      // NOTE: these checks require that the usage list be sorted!
      bool has_preferred_reg = false;
      RegAssignment preferred_reg = {0};
      if (GET_OPCODE_SIG_TYPE_SRC1(signature) == OPCODE_SIG_TYPE_V &&
          !instr->src1.value->IsConstant()) {
        if (!instr->src1_use->next) {
          // Pull off preferred register. We will try to reuse this for the
          // dest.
          // NOTE: set may be null if this is a store local.
          if (preferred_reg.set) {
            has_preferred_reg = true;
            preferred_reg = instr->src1.value->reg;
          }
        }
      }

      // hakuX-Boosted: a value whose home is a pinned guest register's host
      // register takes none of the allocator's. (Under the verify setting it
      // takes one all the same, and the backend compares the two.)
      if (GET_OPCODE_SIG_TYPE_DEST(signature) == OPCODE_SIG_TYPE_V &&
          !pin_homes_.empty()) {
        auto home = pin_homes_.find(instr->dest);
        if (home != pin_homes_.end()) {
          auto& stats = translation_stats;
          (home->second.is_def ? stats.pin_coalesced_defs
                               : stats.pin_coalesced_loads)
              .fetch_add(1, std::memory_order_relaxed);
          if (cvars::pin_guest_registers_coalesce_verify) {
            builder->pin_home_verify()[instr->dest] =
                {home->second.slot, home->second.is_def};
          } else {
            SortUsageList(instr->dest);
            instr->dest->reg.set = home->second.set->set;
            instr->dest->reg.index = home->second.reg_index;
            instr = instr->next;
            continue;
          }
        }
      }

      if (GET_OPCODE_SIG_TYPE_DEST(signature) == OPCODE_SIG_TYPE_V) {
        // Must not have been set already.
        assert_null(instr->dest->reg.set);

        // Sort the usage list. We depend on this in future uses of this
        // variable.
        SortUsageList(instr->dest);

        // If we have a preferred register, use that.
        // This way we can help along the stupid X86 two opcode instructions.
        bool allocated;
        if (has_preferred_reg) {
          // Allocate with the given preferred register. If the register is in
          // the wrong set it will not be reused.
          allocated = TryAllocateRegister(instr->dest, preferred_reg);
        } else {
          // Allocate a register. This will either reserve a free one or
          // spill and reuse an active one.
          allocated = TryAllocateRegister(instr->dest);
        }
        if (!allocated) {
          // Failed to allocate register -- need to spill and try again.
          // We spill only those registers we aren't using.
          if (!SpillOneRegister(builder, block, instr->dest->type)) {
            // Unable to spill anything - this shouldn't happen.
            XELOGE("Unable to spill any registers");
            assert_always();
            return false;
          }

          // Demand allocation.
          if (!TryAllocateRegister(instr->dest)) {
            // Boned.
            XELOGE("Register allocation failed");
            assert_always();
            return false;
          }
        }
      }

      instr = instr->next;
    }
    block = block->next;
  }

  return true;
}

void RegisterAllocationPass::DumpUsage(const char* name) {
#if 0
  fprintf(stdout, "\n%s:\n", name);
  for (size_t i = 0; i < xe::countof(usage_sets_.all_sets); ++i) {
    auto usage_set = usage_sets_.all_sets[i];
    if (usage_set) {
      fprintf(stdout, "set %s:\n", usage_set->set->name);
      fprintf(stdout, "  avail: %s\n",
              usage_set->availability.to_string().c_str());
      fprintf(stdout, "  upcoming uses:\n");
      for (auto it = usage_set->upcoming_uses.begin();
           it != usage_set->upcoming_uses.end(); ++it) {
        fprintf(stdout, "    v%d, used at %d\n",
                it->value->ordinal,
                it->use->instr->ordinal);
      }
    }
  }
  fflush(stdout);
#endif
}

void RegisterAllocationPass::PrepareBlockState() {
  for (size_t i = 0; i < xe::countof(usage_sets_.all_sets); ++i) {
    auto usage_set = usage_sets_.all_sets[i];
    if (usage_set) {
      usage_set->availability.set();
      // hakuX-Boosted: never available, so never allocated or spilled.
      usage_set->availability &= ~usage_set->reserved;
      usage_set->upcoming_uses.clear();
    }
  }
  DumpUsage("PrepareBlockState");
}

void RegisterAllocationPass::AdvanceUses(Instr* instr) {
  for (size_t i = 0; i < xe::countof(usage_sets_.all_sets); ++i) {
    auto usage_set = usage_sets_.all_sets[i];
    if (!usage_set) {
      break;
    }
    auto& upcoming_uses = usage_set->upcoming_uses;
    for (size_t j = 0; j < upcoming_uses.size();) {
      auto& upcoming_use = upcoming_uses.at(j);
      if (!upcoming_use.use) {
        // No uses at all - we can remove right away.
        // This comes up from instructions where the dest is never used,
        // like the ATOMIC ops.
        MarkRegAvailable(upcoming_use.value->reg);
        upcoming_uses.erase(upcoming_uses.begin() + j);
        // i remains the same.
        continue;
      }
      if (upcoming_use.use->instr != instr) {
        // Not yet at this instruction.
        ++j;
        continue;
      }
      // The use is from this instruction.
      if (!upcoming_use.use->next) {
        // Last use of the value. We can retire it now.
        MarkRegAvailable(upcoming_use.value->reg);
        upcoming_uses.erase(upcoming_uses.begin() + j);
        // i remains the same.
        continue;
      } else {
        // Used again. Push back the next use.
        // Note that we may be used multiple times this instruction, so
        // eat those.
        auto next_use = upcoming_use.use->next;
        while (next_use->next && next_use->instr == instr) {
          next_use = next_use->next;
        }
        // Remove the iterator.
        auto value = upcoming_use.value;
        upcoming_uses.erase(upcoming_uses.begin() + j);
        assert_true(next_use->instr->block == instr->block);
        assert_true(value->def->block == instr->block);
        upcoming_uses.emplace_back(value, next_use);
        // i remains the same.
        continue;
      }
    }
  }
  DumpUsage("AdvanceUses");
}

bool RegisterAllocationPass::IsRegInUse(const RegAssignment& reg) {
  RegisterSetUsage* usage_set;
  if (reg.set == usage_sets_.int_set->set) {
    usage_set = usage_sets_.int_set;
  } else if (reg.set == usage_sets_.float_set->set) {
    usage_set = usage_sets_.float_set;
  } else {
    usage_set = usage_sets_.vec_set;
  }
  return !usage_set->availability.test(reg.index);
}

RegisterAllocationPass::RegisterSetUsage* RegisterAllocationPass::MarkRegUsed(
    const RegAssignment& reg, Value* value, Value::Use* use) {
  auto usage_set = RegisterSetForValue(value);
  usage_set->availability.set(reg.index, false);
  usage_set->upcoming_uses.emplace_back(value, use);
  DumpUsage("MarkRegUsed");
  return usage_set;
}

RegisterAllocationPass::RegisterSetUsage*
RegisterAllocationPass::MarkRegAvailable(const hir::RegAssignment& reg) {
  RegisterSetUsage* usage_set;
  if (reg.set == usage_sets_.int_set->set) {
    usage_set = usage_sets_.int_set;
  } else if (reg.set == usage_sets_.float_set->set) {
    usage_set = usage_sets_.float_set;
  } else {
    usage_set = usage_sets_.vec_set;
  }
  usage_set->availability.set(reg.index, true);
  return usage_set;
}

bool RegisterAllocationPass::TryAllocateRegister(
    Value* value, const RegAssignment& preferred_reg) {
  // If the preferred register matches type and is available, use it.
  auto usage_set = RegisterSetForValue(value);
  if (usage_set->set == preferred_reg.set) {
    // Check if available.
    if (!IsRegInUse(preferred_reg)) {
      // Mark as in-use and return. Best case.
      MarkRegUsed(preferred_reg, value, value->use_head);
      value->reg = preferred_reg;
      return true;
    }
  }

  // Otherwise, fallback to allocating like normal.
  return TryAllocateRegister(value);
}

bool RegisterAllocationPass::TryAllocateRegister(Value* value) {
  // Get the set this register is in.
  RegisterSetUsage* usage_set = RegisterSetForValue(value);

  // Find the first free register, if any.
  // We have to ensure it's a valid one (in our count).
  uint32_t first_unused = 0;
  // hakuX-Boosted: register_allocation_from_top, for the tests. Bits at and
  // above the set's count are never cleared, so they are masked off first.
  uint32_t available =
      static_cast<uint32_t>(usage_set->availability.to_ulong());
  bool none_used;
  if (cvars::register_allocation_from_top) {
    if (usage_set->count < 32) {
      available &= (uint32_t(1) << usage_set->count) - 1;
    }
    none_used = available != 0;
    if (none_used) {
      first_unused = 31 - static_cast<uint32_t>(__builtin_clz(available));
    }
  } else {
    none_used = xe::bit_scan_forward(available, &first_unused);
  }
  if (none_used && first_unused < usage_set->count) {
    // Available! Use it!
    value->reg.set = usage_set->set;
    value->reg.index = first_unused;
    MarkRegUsed(value->reg, value, value->use_head);
    return true;
  }

  // None available! Spill required.
  return false;
}

bool RegisterAllocationPass::SpillOneRegister(HIRBuilder* builder, Block* block,
                                              TypeName required_type) {
  // Get the set that we will be picking from.
  RegisterSetUsage* usage_set;
  if (required_type <= INT64_TYPE) {
    usage_set = usage_sets_.int_set;
  } else if (required_type <= FLOAT64_TYPE) {
    usage_set = usage_sets_.float_set;
  } else {
    usage_set = usage_sets_.vec_set;
  }

  DumpUsage("SpillOneRegister (pre)");
  // Pick the one with the furthest next use.
  assert_true(!usage_set->upcoming_uses.empty());
  auto furthest_usage =
      std::max_element(usage_set->upcoming_uses.begin(),
                       usage_set->upcoming_uses.end(), &RegisterUsage::Compare);
  assert_true(furthest_usage->value->def->block == block);
  assert_true(furthest_usage->use->instr->block == block);
  auto spill_value = furthest_usage->value;
  Value::Use* prev_use = furthest_usage->use->prev;
  Value::Use* next_use = furthest_usage->use;
  assert_not_null(next_use);
  usage_set->upcoming_uses.erase(furthest_usage);
  DumpUsage("SpillOneRegister (post)");
  const auto reg = spill_value->reg;

  // We know the spill_value use list is sorted, so we can cut it right now.
  // This makes it easier down below.
  auto new_head_use = next_use;

  // Allocate local.
  if (spill_value->HasLocalSlot()) {
    // Value is already assigned a slot. Since we allocate in order and this is
    // all SSA we know the stored value will be exactly what we want. Yay,
    // we can prevent the redundant store!
    // In fact, we may even want to pin this spilled value so that we always
    // use the spilled value and prevent the need for more locals.
  } else {
    // Allocate a local slot.
    spill_value->SetLocalSlot(builder->AllocLocal(spill_value->type));

    // Add store.
    builder->StoreLocal(spill_value->GetLocalSlot(), spill_value);
    auto spill_store = builder->last_instr();
    auto spill_store_use = spill_store->src2_use;
    assert_null(spill_store_use->prev);
    if (prev_use && prev_use->instr->opcode->flags & OPCODE_FLAG_PAIRED_PREV) {
      // Instruction is paired. This is bad. We will insert the spill after the
      // paired instruction.
      assert_not_null(prev_use->instr->next);
      spill_store->MoveBefore(prev_use->instr->next);

      // Update last use.
      spill_value->last_use = spill_store;
    } else if (prev_use) {
      // We insert the store immediately before the previous use.
      // If we were smarter we could then re-run allocation and reuse the
      // register
      // once dropped.
      spill_store->MoveBefore(prev_use->instr);

      // Update last use.
      spill_value->last_use = prev_use->instr;
    } else {
      // This is the first use, so the only thing we have is the define.
      // Move the store to right after that.
      spill_store->MoveBefore(spill_value->def->next);

      // Update last use.
      spill_value->last_use = spill_store;
    }
  }

#if ASSERT_NO_CYCLES
  builder->AssertNoCycles();
  spill_value->def->block->AssertNoCycles();
#endif  // ASSERT_NO_CYCLES

  // Add load.
  // Inserted immediately before the next use. Since by definition the next
  // use is after the instruction requesting the spill we know we haven't
  // done allocation for that code yet and can let that be handled
  // automatically when we get to it.
  auto new_value = builder->LoadLocal(spill_value->GetLocalSlot());
  auto spill_load = builder->last_instr();
  spill_load->MoveBefore(next_use->instr);
  // Note: implicit first use added.

#if ASSERT_NO_CYCLES
  builder->AssertNoCycles();
  spill_value->def->block->AssertNoCycles();
#endif  // ASSERT_NO_CYCLES

  // Set the local slot of the new value to our existing one. This way we will
  // reuse that same memory if needed.
  new_value->SetLocalSlot(spill_value->GetLocalSlot());

  // Rename all future uses of the SSA value to the new value as loaded
  // from the local.
  // We can quickly do this by walking the use list. Because the list is
  // already sorted we know we are going to end up with a sorted list.
  auto walk_use = new_head_use;
  auto new_use_tail = walk_use;
  while (walk_use) {
    auto next_walk_use = walk_use->next;
    auto instr = walk_use->instr;

    uint32_t signature = instr->opcode->signature;
    if (GET_OPCODE_SIG_TYPE_SRC1(signature) == OPCODE_SIG_TYPE_V) {
      if (instr->src1.value == spill_value) {
        instr->set_src1(new_value);
      }
    }
    if (GET_OPCODE_SIG_TYPE_SRC2(signature) == OPCODE_SIG_TYPE_V) {
      if (instr->src2.value == spill_value) {
        instr->set_src2(new_value);
      }
    }
    if (GET_OPCODE_SIG_TYPE_SRC3(signature) == OPCODE_SIG_TYPE_V) {
      if (instr->src3.value == spill_value) {
        instr->set_src3(new_value);
      }
    }

    walk_use = next_walk_use;
    if (walk_use) {
      new_use_tail = walk_use;
    }
  }
  new_value->last_use = new_use_tail->instr;

  // Update tracking.
  MarkRegAvailable(reg);

  return true;
}

// hakuX-Boosted: pin_guest_registers_coalesce.
//
// GuestRegisterPinningPass gives a guest register one host register for the
// whole function, and the backend turns each load of it into a move from
// that register and each store into a move to it. This decides, block by
// block, which of those moves need not exist:
//
//   a load     v = LOAD_CONTEXT pin    v may *be* the pin's register, as long
//                                      as every use of v comes before the
//                                      register is next written;
//   a store    STORE_CONTEXT pin, w    w may be computed straight into the
//                                      pin's register by the instruction
//                                      that defines it (D), which makes D
//                                      the point where the register is
//                                      written, earlier than the store (S).
//
// "The register is written" at: a store to the pin that is not coalesced
// (at S); a coalesced store (at its D); and any instruction after which the
// pin may have been reloaded from the context or the function left, which
// is taken to be every volatile instruction and context barrier other than
// conditional branches and the barriers in front of branches (a "clobber";
// IsPinClobber). A use *by* the writing instruction itself is fine: sequences
// read their sources before their destination can matter, which the
// allocator already relies on when it hands a retiring source's register to
// the destination.
//
// For a store to be coalesced, all of:
//   1. w is defined in this block by D, before S, has the pin's type, and
//      is not itself living in another pin;
//   2. every instruction strictly between D and S is from a short list of
//      pure ones (IsPureForCoalescing) that cannot call a host helper, and
//      none of them reads or writes this pin. So nothing can observe that
//      the pin changed at D and not at S: no load of it, no write-back of
//      it, no reload;
//   3. every value already living in the pin's register (earlier loads of
//      it, the value an earlier coalesced store left there) has its last
//      use at or before D;
//   4. w itself has its last use at or before the next store to this pin or
//      clobber after S, whichever comes first. (If the next store is then
//      coalesced too, rule 3 applied to it makes sure its D comes no earlier
//      than w's last use.)
//   5. for the 8-bit pins (condition register and carry bytes), D is an
//      instruction that leaves exactly 0 or 1 in the whole register, since
//      the pin has to read as a zero-extended byte.
//
// A load lives in the pin if its last use is at or before the next write of
// the register as defined above. Otherwise it gets a register of its own
// and the move, as before.
//
// Values planned here never enter the allocator's own bookkeeping: they
// hold a register the allocator never hands out (the pin's is reserved),
// they are never spilled, and nothing is freed when they retire.
namespace {

bool IsPinClobber(const Instr* i) {
  switch (i->opcode->num) {
    case OPCODE_BRANCH_TRUE:
    case OPCODE_BRANCH_FALSE:
      // Volatile for the per-block passes, but a branch changes no register
      // and what follows it in the block runs with the same registers.
      return false;
    case OPCODE_CONTEXT_BARRIER:
      // The barrier the front end puts before a branch instruction is not a
      // way out of the function with pin_guest_registers_wide (which this
      // pass requires): the backend emits nothing for it, and context
      // promotion carries values over it.
      return !(i->flags & CONTEXT_BARRIER_FOR_BRANCH);
    default:
      return (i->opcode->flags & OPCODE_FLAG_VOLATILE) != 0;
  }
}

// Instructions that may sit between the definition of a value and its store
// to a pin when the two are coalesced: no host call from inside their
// sequences, nothing that leaves the function.
bool IsPureForCoalescing(const Instr* i) {
  switch (i->opcode->num) {
    case OPCODE_COMMENT:
    case OPCODE_NOP:
    case OPCODE_SOURCE_OFFSET:
    case OPCODE_ASSIGN:
    case OPCODE_CAST:
    case OPCODE_ZERO_EXTEND:
    case OPCODE_SIGN_EXTEND:
    case OPCODE_TRUNCATE:
    case OPCODE_LOAD_CONTEXT:
    case OPCODE_STORE_CONTEXT:
    case OPCODE_LOAD_LOCAL:
    case OPCODE_STORE_LOCAL:
    case OPCODE_COMPARE_EQ:
    case OPCODE_COMPARE_NE:
    case OPCODE_COMPARE_SLT:
    case OPCODE_COMPARE_SLE:
    case OPCODE_COMPARE_SGT:
    case OPCODE_COMPARE_SGE:
    case OPCODE_COMPARE_ULT:
    case OPCODE_COMPARE_ULE:
    case OPCODE_COMPARE_UGT:
    case OPCODE_COMPARE_UGE:
    case OPCODE_ADD:
    case OPCODE_ADD_CARRY:
    case OPCODE_SUB:
    case OPCODE_AND:
    case OPCODE_AND_NOT:
    case OPCODE_OR:
    case OPCODE_XOR:
    case OPCODE_NOT:
    case OPCODE_NEG:
    case OPCODE_SHL:
    case OPCODE_SHR:
    case OPCODE_SHA:
    case OPCODE_ROTATE_LEFT:
    case OPCODE_SELECT:
      return true;
    default:
      return false;
  }
}

// Leaves 0 or 1 in the whole of an integer register.
bool DefinesCleanBit(const Instr* i) {
  switch (i->opcode->num) {
    case OPCODE_COMPARE_EQ:
    case OPCODE_COMPARE_NE:
    case OPCODE_COMPARE_SLT:
    case OPCODE_COMPARE_SLE:
    case OPCODE_COMPARE_SGT:
    case OPCODE_COMPARE_SGE:
    case OPCODE_COMPARE_ULT:
    case OPCODE_COMPARE_ULE:
    case OPCODE_COMPARE_UGT:
    case OPCODE_COMPARE_UGE:
      return true;
    default:
      return false;
  }
}

uint32_t LastUseOrdinal(const Value* value) {
  uint32_t last = value->def ? value->def->ordinal : 0;
  for (auto use = value->use_head; use; use = use->next) {
    last = std::max(last, use->instr->ordinal);
  }
  return last;
}

}  // namespace

void RegisterAllocationPass::PlanPinHomes(HIRBuilder* builder, Block* block) {
  const auto& pinned = builder->pinned_context_slots();
  const size_t pin_count = pinned.size();
  // The values living in each pin's register that the next write of it has
  // yet to be checked against.
  struct Tenant {
    const Value* value;
    uint32_t last_use;
    bool is_def;
  };
  std::vector<std::vector<Tenant>> tenants(pin_count);

  const auto find_pin = [&](const Instr* i, TypeName type) -> int {
    const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
    for (size_t n = 0; n < pin_count; ++n) {
      if (pinned[n].offset == offset && pinned[n].type == type) {
        return static_cast<int>(n);
      }
    }
    return -1;
  };
  const auto set_for = [&](size_t n) {
    return pinned[n].reg_set == 0 ? usage_sets_.int_set : usage_sets_.vec_set;
  };
  // The register of pin n is written at |ordinal|: tenants whose uses all
  // come at or before it stay, the others get registers of their own.
  const auto settle = [&](size_t n, uint32_t ordinal) {
    for (const Tenant& tenant : tenants[n]) {
      if (tenant.is_def) {
        continue;  // settled when it was planned (rule 4)
      }
      if (tenant.last_use <= ordinal) {
        pin_homes_[tenant.value] = {set_for(n), pinned[n].reg_index,
                                    static_cast<uint8_t>(n), false};
      }
    }
    tenants[n].clear();
  };

  for (Instr* i = block->instr_head; i; i = i->next) {
    if (i->opcode == &OPCODE_LOAD_CONTEXT_info) {
      const int n = find_pin(i, i->dest->type);
      if (n >= 0 && set_for(n)) {
        tenants[n].push_back({i->dest, LastUseOrdinal(i->dest), false});
      }
      continue;
    }
    if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
      Value* w = i->src2.value;
      const int n = find_pin(i, w->type);
      if (n < 0 || !set_for(n)) {
        continue;
      }
      bool coalesce = !w->IsConstant() && w->def && w->def->block == block &&
                      w->def->ordinal < i->ordinal &&
                      w->def->opcode != &OPCODE_LOAD_CONTEXT_info &&
                      !pin_homes_.count(w);
      const Instr* def = coalesce ? w->def : nullptr;
      if (coalesce && pinned[n].type == INT8_TYPE && !DefinesCleanBit(def)) {
        coalesce = false;
      }
      // A value loaded from an unpinned slot may be read straight into the
      // pin; one loaded from another pin lives there (or not at all).
      if (!coalesce && !w->IsConstant() && w->def &&
          w->def->block == block && w->def->ordinal < i->ordinal &&
          w->def->opcode == &OPCODE_LOAD_CONTEXT_info &&
          !pin_homes_.count(w) && pinned[n].type != INT8_TYPE) {
        bool from_pin = false;
        const uint32_t offset = static_cast<uint32_t>(w->def->src1.offset);
        for (size_t m = 0; m < pin_count; ++m) {
          if (pinned[m].offset == offset && pinned[m].type == w->type) {
            from_pin = true;
          }
        }
        if (!from_pin) {
          coalesce = true;
          def = w->def;
        }
      }
      if (coalesce) {
        // Rule 2.
        for (const Instr* between = def->next; between != i;
             between = between->next) {
          if (!between || !IsPureForCoalescing(between)) {
            coalesce = false;
            break;
          }
          if ((between->opcode == &OPCODE_LOAD_CONTEXT_info &&
               find_pin(between, between->dest->type) == n) ||
              (between->opcode == &OPCODE_STORE_CONTEXT_info &&
               find_pin(between, between->src2.value->type) == n)) {
            coalesce = false;
            break;
          }
        }
      }
      if (coalesce) {
        // Rule 3.
        for (const Tenant& tenant : tenants[n]) {
          if (tenant.last_use > def->ordinal) {
            coalesce = false;
            break;
          }
        }
      }
      if (coalesce) {
        // Rule 4.
        uint32_t limit = UINT32_MAX;
        for (const Instr* later = i->next; later; later = later->next) {
          if (IsPinClobber(later) ||
              (later->opcode == &OPCODE_STORE_CONTEXT_info &&
               find_pin(later, later->src2.value->type) == n)) {
            limit = later->ordinal;
            break;
          }
        }
        if (LastUseOrdinal(w) > limit) {
          coalesce = false;
        }
      }
      settle(n, coalesce ? def->ordinal : i->ordinal);
      if (coalesce) {
        pin_homes_[w] = {set_for(n), pinned[n].reg_index,
                         static_cast<uint8_t>(n), true};
        tenants[n].push_back({w, LastUseOrdinal(w), true});
      }
      continue;
    }
    if (IsPinClobber(i)) {
      for (size_t n = 0; n < pin_count; ++n) {
        settle(n, i->ordinal);
      }
    }
  }
  for (size_t n = 0; n < pin_count; ++n) {
    settle(n, UINT32_MAX);
  }
}

RegisterAllocationPass::RegisterSetUsage*
RegisterAllocationPass::RegisterSetForValue(const Value* value) {
  if (value->type <= INT64_TYPE) {
    return usage_sets_.int_set;
  } else if (value->type <= FLOAT64_TYPE) {
    return usage_sets_.float_set;
  } else {
    return usage_sets_.vec_set;
  }
}

namespace {
int CompareValueUse(const Value::Use* a, const Value::Use* b) {
  return a->instr->ordinal - b->instr->ordinal;
}
}  // namespace
void RegisterAllocationPass::SortUsageList(Value* value) {
  // Modified in-place linked list sort from:
  // https://www.chiark.greenend.org.uk/~sgtatham/algorithms/listsort.c
  if (!value->use_head) {
    return;
  }
  Value::Use* head = value->use_head;
  Value::Use* tail = nullptr;
  int insize = 1;
  while (true) {
    auto p = head;
    head = nullptr;
    tail = nullptr;
    // count number of merges we do in this pass
    int nmerges = 0;
    while (p) {
      // there exists a merge to be done
      nmerges++;
      // step 'insize' places along from p
      auto q = p;
      int psize = 0;
      for (int i = 0; i < insize; i++) {
        psize++;
        q = q->next;
        if (!q) {
          break;
        }
      }
      // if q hasn't fallen off end, we have two lists to merge
      int qsize = insize;
      // now we have two lists; merge them
      while (psize > 0 || (qsize > 0 && q)) {
        // decide whether next element of merge comes from p or q
        Value::Use* e = nullptr;
        if (psize == 0) {
          // p is empty; e must come from q
          e = q;
          q = q->next;
          qsize--;
        } else if (qsize == 0 || !q) {
          // q is empty; e must come from p
          e = p;
          p = p->next;
          psize--;
        } else if (CompareValueUse(p, q) <= 0) {
          // First element of p is lower (or same); e must come from p
          e = p;
          p = p->next;
          psize--;
        } else {
          // First element of q is lower; e must come from q
          e = q;
          q = q->next;
          qsize--;
        }
        // add the next element to the merged list
        if (tail) {
          tail->next = e;
        } else {
          head = e;
        }
        // Maintain reverse pointers in a doubly linked list.
        e->prev = tail;
        tail = e;
      }
      // now p has stepped 'insize' places along, and q has too
      p = q;
    }
    if (tail) {
      tail->next = nullptr;
    }
    // If we have done only one merge, we're finished
    if (nmerges <= 1) {
      // allow for nmerges==0, the empty list case
      break;
    }
    // Otherwise repeat, merging lists twice the size
    insize *= 2;
  }

  value->use_head = head;
  value->last_use = tail->instr;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
