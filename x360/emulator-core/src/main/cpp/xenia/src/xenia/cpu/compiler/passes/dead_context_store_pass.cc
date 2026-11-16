/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: removes stores to guest registers and flag bits that no     *
 * path reads. Released under the BSD license.                                *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/dead_context_store_pass.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/cpu/compiler/translation_stats.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

DEFINE_int32(
    eliminate_dead_context_stores, 0,
    "Remove stores to guest state that every path overwrites before anything "
    "reads it, looking across the whole function instead of one block. "
    "0: off. 1: condition register bits and XER bits (a compare stores three "
    "bits and the branch after it reads one). 2: the general registers and "
    "CTR as well. Calls, returns, traps and the scheduler's check count as "
    "reading everything. 3: as 2, and the condition register fields the "
    "PowerPC ABI leaves undefined over a call (cr0, cr1, cr5, cr6, cr7) "
    "count as not read by a call or a return: right for compiled code, "
    "wrong for a hand-written routine that hands a result back in one of "
    "them; run a title once with dead_context_store_verify before trusting "
    "3 on it. Takes effect for functions translated after it is set.",
    "CPU");

// hakuX-Boosted
DEFINE_bool(
    dead_context_stores_over_preempt_checks, false,
    "With eliminate_dead_context_stores: the cooperative scheduler's check "
    "at a loop head (emitted when guest_scheduler is on) does not count as "
    "reading the guest context. Without this no store inside a loop is ever "
    "dead, because the check sits on every path round the loop. It is safe "
    "for the guest's own execution: when the check yields, no guest code "
    "runs on this thread's context before it resumes, and a store taken for "
    "dead is one that every path overwrites before reading. What can see "
    "the difference is something that inspects a thread's registers while "
    "it is parked at such a yield (a debugger, a crash report, a title "
    "reading another thread's context): it may find an older value in a "
    "flag or register that the thread was about to overwrite. Takes effect "
    "for functions translated after it is set.",
    "CPU");

DEFINE_bool(
    dead_context_store_verify, false,
    "Self-check for eliminate_dead_context_stores, for a checking run only: "
    "nothing is removed; a store the pass would have removed stores a marker "
    "value instead, and every load of such a slot checks for the marker. A "
    "line starting 'TranslatorVerify:' means a store the pass took for dead "
    "was read. ARM64 backend only.",
    "CPU");

DECLARE_bool(debug);

// hakuX-Boosted: see AbiGuard below.
DEFINE_bool(
    dead_context_stores_abi_guard, true,
    "With eliminate_dead_context_stores = 3: look for the code that level "
    "is wrong for and translate round it. A caller that reads a condition "
    "field after a call without setting it names its callee, whose flags "
    "are then kept at its returns (if the call was through a pointer, "
    "every function's are, from then on); a callee whose first "
    "instructions read a field before setting it has the flags kept at "
    "the calls to it. What it cannot repair it counts and logs ('found "
    "too late'): a callee translated before the caller that showed it "
    "returns a flag. log_translation_stats prints the counts.",
    "CPU");

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Block;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::ppc::PPCContext;

bool IsDeadStoreCandidateSlot(uint32_t offset, uint32_t size, int level) {
  if (level <= 0) {
    return false;
  }
  if (size == 1) {
    const uint32_t cr_base = static_cast<uint32_t>(offsetof(PPCContext, cr0));
    if (offset >= cr_base && offset < cr_base + 32) {
      return true;
    }
    return offset == offsetof(PPCContext, xer_ca) ||
           offset == offsetof(PPCContext, xer_ov) ||
           offset == offsetof(PPCContext, xer_so);
  }
  if (level >= 2 && size == 8) {
    const uint32_t gpr_base = static_cast<uint32_t>(offsetof(PPCContext, r));
    if (offset >= gpr_base && offset < gpr_base + 32 * 8 &&
        (offset - gpr_base) % 8 == 0) {
      // Not the stack pointer, r2 or the PCR pointer: host code reads those
      // at places this pass does not model (see GuestRegisterPinningPass).
      const uint32_t n = (offset - gpr_base) / 8;
      return n != 1 && n != 2 && n != 13;
    }
    return offset == offsetof(PPCContext, ctr);
  }
  return false;
}

namespace {

constexpr size_t kMaxSlots = 256;
using SlotSet = std::bitset<kMaxSlots>;

enum class Kind : uint8_t {
  kOther,
  kLoad,       // reads `slots`
  kStore,      // writes slot `slot` whole
  kReadsAll,   // code outside the function may read anything here
  kEnd,        // reads everything and control does not continue
  kCall,       // a guest or kernel call that comes back: reads what a
               // callee may read, and what is live after it stays live
  kCallEnd,    // a return or tail call: reads what a caller or callee may
};

// The condition register fields the ABI leaves undefined across a call:
// a callee does not take them as input and a caller does not read them
// after the call returns.
bool IsAbiVolatileFlag(uint32_t offset, uint32_t size) {
  const uint32_t cr_base = static_cast<uint32_t>(offsetof(PPCContext, cr0));
  if (size != 1 || offset < cr_base || offset >= cr_base + 32) {
    return false;
  }
  const uint32_t field = (offset - cr_base) / 4;
  return field == 0 || field == 1 || field >= 5;
}

struct Item {
  Instr* instr;
  Kind kind;
  int16_t slot;      // kStore: the slot; else -1
  int32_t target;    // branch target block or -1
  bool unconditional;
  uint32_t load_offset;
  uint32_t load_size;
};

// hakuX-Boosted: dead_context_stores_abi_guard. Level 3 takes cr0, cr1 and
// cr5 to cr7 for dead at calls and returns, which the ABI allows and
// compiled code keeps to. Hand-written code may not: a leaf that returns
// its answer in cr0, or one that takes a flag as an argument. Both can be
// seen in the code, from the side that reads:
//   - a load of such a field that a call may have been the last to write
//     (no store of this function's own since) is a caller reading what its
//     callee returned. The callee is named and its returns keep the flags;
//   - a callee whose first instructions read a field before writing it
//     takes it as an argument, and the calls to it keep the flags. That is
//     looked for in the callee's guest code when the caller is translated
//     (the caller is normally translated first), and found again when the
//     callee itself is.
struct AbiGuard {
  std::mutex mutex;
  // Functions a caller was seen reading a flag from.
  std::unordered_set<uint32_t> returns_flags;
  // Functions translated with their flags taken for dead at returns.
  std::unordered_set<uint32_t> dead_at_return;
  // Functions seen reading a flag at entry, and the scan's answers.
  std::unordered_map<uint32_t, bool> reads_at_entry;
  // A caller read a flag after a call through a pointer: any function may
  // be the callee.
  bool every_function_returns_flags = false;
};
AbiGuard abi_guard;

constexpr uint32_t kVolatileCrFields = 0b11100011;  // bit n: field n
// The same as a mask of the 32 bytes the fields occupy in the context.
constexpr uint32_t kVolatileCrBytes = 0xFFF000FFu;

// Does the code at |address| read cr0, cr1, cr5, cr6 or cr7 before writing
// it? Straight-line from the entry, through unconditional jumps, up to the
// first call, return or conditional branch (whose own condition counts).
// Unreadable memory answers yes: keeping the flags is the safe side.
bool GuestCodeReadsFlagsAtEntry(Memory* memory, uint32_t address) {
  uint32_t written = 0;
  int jumps = 0;
  uint32_t checked_page = ~0u;
  for (int n = 0; n < 64; ++n) {
    if ((address & ~0xFFFu) != checked_page) {
      checked_page = address & ~0xFFFu;
      auto heap = memory->LookupHeap(address);
      uint32_t protect = 0;
      if (!heap || !heap->QueryProtect(address, &protect) ||
          !(protect & kMemoryProtectRead)) {
        return true;
      }
    }
    const uint32_t code =
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
    const uint32_t op = code >> 26;
    const uint32_t xo = (code >> 1) & 0x3FF;
    const uint32_t crfd = 1u << ((code >> 23) & 7);
    uint32_t reads = 0;
    uint32_t writes = 0;
    bool stop = false;
    const auto condition = [&]() {
      const uint32_t bo = (code >> 21) & 31;
      const uint32_t bi = (code >> 16) & 31;
      if (!(bo & 0x10)) {
        reads |= 1u << (bi >> 2);
      }
    };
    switch (op) {
      case 16:  // bc
        condition();
        stop = true;
        break;
      case 17:  // sc
        stop = true;
        break;
      case 18:  // b, bl
        if ((code & 1) || ++jumps > 4) {
          stop = true;
        } else {
          int32_t offset = static_cast<int32_t>(code << 6) >> 6;
          offset &= ~3;
          address = ((code & 2) ? 0 : address) + offset - 4;
        }
        break;
      case 19:
        if (xo == 16 || xo == 528) {  // bclr, bcctr
          condition();
          stop = true;
        } else if (xo == 0) {  // mcrf
          reads |= 1u << ((code >> 18) & 7);
          writes |= crfd;
        } else if (xo == 33 || xo == 129 || xo == 193 || xo == 225 ||
                   xo == 257 || xo == 289 || xo == 417 || xo == 449) {
          reads |= 1u << (((code >> 16) & 31) >> 2);
          reads |= 1u << (((code >> 11) & 31) >> 2);
        }
        break;
      case 10:  // cmpli
      case 11:  // cmpi
        writes |= crfd;
        break;
      case 28:  // andi.
      case 29:  // andis.
        writes |= 1u;
        break;
      case 31:
        if (xo == 19) {  // mfcr
          reads |= 0xFF;
        } else if (xo == 0 || xo == 32) {  // cmp, cmpl
          writes |= crfd;
        } else if (xo == 144) {  // mtcrf
          const uint32_t mask = (code >> 12) & 0xFF;
          for (uint32_t field = 0; field < 8; ++field) {
            if (mask & (0x80u >> field)) {
              writes |= 1u << field;
            }
          }
        } else if (code & 1) {  // a record form
          writes |= 1u;
        }
        break;
      case 63:
        if (xo == 0 || xo == 32 || xo == 64) {  // fcmpu, fcmpo, mcrfs
          writes |= crfd;
        }
        break;
      default:
        break;
    }
    if (reads & ~written & kVolatileCrFields) {
      return true;
    }
    if (stop) {
      return false;
    }
    written |= writes;
    address += 4;
  }
  return false;
}

// What the forward analysis found in one function.
struct AbiGuardFindings {
  std::vector<uint32_t> callees;  // direct callees whose flags are read
  bool indirect = false;          // ...and a callee reached through a pointer
  bool reads_at_entry = false;    // this function reads its caller's flags
  // Where this function leaves by a jump instead of a return: if its
  // flags are read by its caller, so are theirs.
  std::vector<uint32_t> tail_callees;
  bool tail_indirect = false;
};

// Forward to a fixed point over the function: which volatile flag bytes
// may hold what a call (or the caller, at entry) left there, and which
// calls. A load of such a byte is a finding. Any block may follow the one
// before it and every branch target is followed: more paths than there
// are, which only finds more.
AbiGuardFindings FindFlagReadsAfterCalls(HIRBuilder* builder) {
  constexpr size_t kMaxCalls = 128;
  constexpr size_t kEntry = 0;
  constexpr size_t kOverflow = kMaxCalls - 1;
  using CallSet = std::bitset<kMaxCalls>;
  struct State {
    uint32_t bytes = 0;
    CallSet calls;
    bool operator==(const State& other) const {
      return bytes == other.bytes && calls == other.calls;
    }
    void Merge(const State& other) {
      bytes |= other.bytes;
      calls |= other.calls;
    }
  };
  // What each call id is: a guest address, 0 for the entry or a host
  // function, ~0 for a pointer.
  std::vector<uint32_t> call_targets = {0};
  std::unordered_map<const Instr*, size_t> call_ids;
  std::vector<Block*> blocks;
  std::unordered_map<const Block*, uint32_t> block_index;
  for (auto block = builder->first_block(); block; block = block->next) {
    block_index[block] = static_cast<uint32_t>(blocks.size());
    blocks.push_back(block);
  }
  const uint32_t cr_base = static_cast<uint32_t>(offsetof(PPCContext, cr0));
  const auto cr_bytes = [&](uint32_t offset, uint32_t size) -> uint32_t {
    uint32_t mask = 0;
    for (uint32_t n = 0; n < 32; ++n) {
      if (cr_base + n >= offset && cr_base + n < offset + size) {
        mask |= 1u << n;
      }
    }
    return mask;
  };
  AbiGuardFindings findings;
  CallSet culprits;
  std::vector<State> in(blocks.size());
  in[0].bytes = kVolatileCrBytes;
  in[0].calls.set(kEntry);
  const auto walk = [&](uint32_t b, bool report) {
    State state = in[b];
    const auto flow_to = [&](uint32_t target, bool& changed) {
      State merged = in[target];
      merged.Merge(state);
      if (!(merged == in[target])) {
        in[target] = merged;
        changed = true;
      }
    };
    bool changed = false;
    for (auto i = blocks[b]->instr_head; i; i = i->next) {
      const Opcode num = i->opcode->num;
      if (num == OPCODE_STORE_CONTEXT) {
        state.bytes &= ~cr_bytes(
            static_cast<uint32_t>(i->src1.offset),
            static_cast<uint32_t>(GetTypeSize(i->src2.value->type)));
      } else if (num == OPCODE_LOAD_CONTEXT) {
        if (report &&
            (state.bytes &
             cr_bytes(static_cast<uint32_t>(i->src1.offset),
                      static_cast<uint32_t>(GetTypeSize(i->dest->type))))) {
          culprits |= state.calls;
        }
      } else if (num == OPCODE_CALL || num == OPCODE_CALL_TRUE ||
                 num == OPCODE_CALL_INDIRECT ||
                 num == OPCODE_CALL_INDIRECT_TRUE ||
                 num == OPCODE_CALL_EXTERN) {
        const bool conditional =
            num == OPCODE_CALL_TRUE || num == OPCODE_CALL_INDIRECT_TRUE;
        if (num != OPCODE_CALL_EXTERN && (i->flags & CALL_TAIL)) {
          // Control does not come back; what the target reads is the
          // other half of the guard.
          if (report) {
            if (num == OPCODE_CALL || num == OPCODE_CALL_TRUE) {
              const Function* symbol =
                  num == OPCODE_CALL ? i->src1.symbol : i->src2.symbol;
              if (symbol && symbol->is_guest() &&
                  symbol->behavior() == Function::Behavior::kDefault) {
                findings.tail_callees.push_back(symbol->address());
              }
            } else if (!(i->flags & CALL_POSSIBLE_RETURN)) {
              findings.tail_indirect = true;
            }
          }
          if (!conditional) {
            state = State();
          }
          continue;
        }
        auto it = call_ids.find(i);
        if (it == call_ids.end()) {
          size_t id = kOverflow;
          uint32_t target = ~0u;
          if (num == OPCODE_CALL_EXTERN) {
            target = 0;
          } else if (num == OPCODE_CALL || num == OPCODE_CALL_TRUE) {
            const Function* symbol =
                num == OPCODE_CALL ? i->src1.symbol : i->src2.symbol;
            // A host function sets no guest flag: nothing to keep.
            target = symbol && symbol->is_guest() &&
                             symbol->behavior() == Function::Behavior::kDefault
                         ? symbol->address()
                         : 0;
          }
          if (call_targets.size() < kOverflow) {
            id = call_targets.size();
            call_targets.push_back(target);
          }
          it = call_ids.emplace(i, id).first;
        }
        if (conditional) {
          state.calls.set(it->second);
        } else {
          state.calls.reset();
          state.calls.set(it->second);
        }
        state.bytes = conditional ? (state.bytes | kVolatileCrBytes)
                                  : kVolatileCrBytes;
      } else if (num == OPCODE_BRANCH || num == OPCODE_BRANCH_TRUE ||
                 num == OPCODE_BRANCH_FALSE) {
        const hir::Label* label =
            num == OPCODE_BRANCH ? i->src1.label : i->src2.label;
        auto it = (label && label->block) ? block_index.find(label->block)
                                          : block_index.end();
        if (it != block_index.end()) {
          flow_to(it->second, changed);
        }
      }
    }
    if (b + 1 < blocks.size()) {
      flow_to(b + 1, changed);
    }
    return changed;
  };
  if (blocks.empty()) {
    return findings;
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t b = 0; b < blocks.size(); ++b) {
      changed |= walk(b, false);
    }
  }
  for (uint32_t b = 0; b < blocks.size(); ++b) {
    walk(b, true);
  }
  findings.reads_at_entry = culprits.test(kEntry);
  for (size_t id = 1; id < call_targets.size(); ++id) {
    if (!culprits.test(id)) {
      continue;
    }
    if (call_targets[id] == ~0u) {
      findings.indirect = true;
    } else if (call_targets[id] != 0) {
      findings.callees.push_back(call_targets[id]);
    }
  }
  // More calls than ids: the last id stands for all the rest, whatever
  // they were.
  if (culprits.test(kOverflow) && call_targets.size() >= kOverflow) {
    findings.indirect = true;
  }
  return findings;
}

}  // namespace

DeadContextStorePass::DeadContextStorePass() : CompilerPass() {}

DeadContextStorePass::~DeadContextStorePass() = default;

bool DeadContextStorePass::Run(HIRBuilder* builder) {
  const int level = cvars::eliminate_dead_context_stores;
  // With the debugger on, the context is what it shows.
  if (level <= 0 || cvars::debug) {
    return true;
  }

  // hakuX-Boosted: dead_context_stores_abi_guard (see AbiGuard). Done
  // before anything below can leave the function alone: what this function
  // reads decides how *other* functions are translated.
  const bool guard = level >= 3 && cvars::dead_context_stores_abi_guard;
  bool keep_flags_at_returns = false;
  if (guard) {
    const uint32_t self = builder->guest_function_address();
    const AbiGuardFindings findings = FindFlagReadsAfterCalls(builder);
    std::lock_guard<std::mutex> lock(abi_guard.mutex);
    for (uint32_t callee : findings.callees) {
      if (abi_guard.returns_flags.insert(callee).second &&
          abi_guard.dead_at_return.count(callee)) {
        translation_stats.abi_guard_late.fetch_add(1,
                                                   std::memory_order_relaxed);
        XELOGW(
            "dead_context_stores_abi_guard: {:08X} reads a condition field "
            "that {:08X} returns, and {:08X} was translated before that was "
            "known, without it. eliminate_dead_context_stores = 3 is not "
            "safe for this title; use 2.",
            self, callee, callee);
      }
    }
    if (findings.indirect && !abi_guard.every_function_returns_flags) {
      abi_guard.every_function_returns_flags = true;
      translation_stats.abi_guard_indirect.fetch_add(
          1, std::memory_order_relaxed);
      XELOGW(
          "dead_context_stores_abi_guard: {:08X} reads a condition field "
          "after a call through a pointer; every function translated from "
          "here on keeps its flags at returns ({} were translated without)",
          self, abi_guard.dead_at_return.size());
    }
    if (findings.reads_at_entry && self) {
      auto it = abi_guard.reads_at_entry.find(self);
      if (it == abi_guard.reads_at_entry.end() || !it->second) {
        abi_guard.reads_at_entry[self] = true;
        translation_stats.abi_guard_entry_readers.fetch_add(
            1, std::memory_order_relaxed);
      }
    }
    keep_flags_at_returns = !self || abi_guard.every_function_returns_flags ||
                            abi_guard.returns_flags.count(self);
    if (keep_flags_at_returns) {
      translation_stats.abi_guard_returns_kept.fetch_add(
          1, std::memory_order_relaxed);
      // What it jumps to returns to the same caller.
      for (uint32_t callee : findings.tail_callees) {
        if (abi_guard.returns_flags.insert(callee).second &&
            abi_guard.dead_at_return.count(callee)) {
          translation_stats.abi_guard_late.fetch_add(
              1, std::memory_order_relaxed);
          XELOGW(
              "dead_context_stores_abi_guard: {:08X}, whose flags a caller "
              "reads, jumps to {:08X}, which was translated before that "
              "was known. eliminate_dead_context_stores = 3 is not safe "
              "for this title; use 2.",
              self, callee);
        }
      }
      if (findings.tail_indirect && self &&
          !abi_guard.every_function_returns_flags) {
        abi_guard.every_function_returns_flags = true;
        translation_stats.abi_guard_indirect.fetch_add(
            1, std::memory_order_relaxed);
        XELOGW(
            "dead_context_stores_abi_guard: {:08X}, whose flags a caller "
            "reads, jumps through a pointer; every function translated "
            "from here on keeps its flags at returns",
            self);
      }
    } else {
      abi_guard.dead_at_return.insert(self);
    }
  }
  // Whether the calls to |target| keep the flags: it reads one at entry.
  const auto callee_reads_flags = [&](const Function* target) {
    if (!guard || !target || !target->is_guest() ||
        target->behavior() != Function::Behavior::kDefault) {
      return false;
    }
    const uint32_t address = target->address();
    {
      std::lock_guard<std::mutex> lock(abi_guard.mutex);
      auto it = abi_guard.reads_at_entry.find(address);
      if (it != abi_guard.reads_at_entry.end()) {
        return it->second;
      }
    }
    const bool reads =
        GuestCodeReadsFlagsAtEntry(processor_->memory(), address);
    std::lock_guard<std::mutex> lock(abi_guard.mutex);
    abi_guard.reads_at_entry.emplace(address, reads);
    return reads;
  };

  std::vector<Block*> blocks;
  std::unordered_map<const Block*, uint32_t> block_index;
  for (auto block = builder->first_block(); block; block = block->next) {
    block_index[block] = static_cast<uint32_t>(blocks.size());
    blocks.push_back(block);
  }
  const uint32_t block_count = static_cast<uint32_t>(blocks.size());
  if (block_count < 2 && level < 3) {
    // Within one block ContextPromotionPass has done this already. (At
    // level 3 a store can be dead at the block's return.)
    return true;
  }

  // The slots: every (offset, size) a candidate store in this function
  // writes.
  struct Slot {
    uint32_t offset;
    uint32_t size;
  };
  std::vector<Slot> slots;
  std::unordered_map<uint64_t, int16_t> slot_index;
  std::vector<std::vector<Item>> code(block_count);
  for (uint32_t b = 0; b < block_count; ++b) {
    for (auto i = blocks[b]->instr_head; i; i = i->next) {
      Item item = {i, Kind::kOther, -1, -1, false, 0, 0};
      const Opcode num = i->opcode->num;
      if (num == OPCODE_STORE_CONTEXT) {
        const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
        const uint32_t size =
            static_cast<uint32_t>(GetTypeSize(i->src2.value->type));
        if (IsDeadStoreCandidateSlot(offset, size, level)) {
          const uint64_t key = (static_cast<uint64_t>(offset) << 8) | size;
          auto it = slot_index.find(key);
          if (it == slot_index.end()) {
            if (slots.size() >= kMaxSlots) {
              return true;  // too many to track: leave the function alone
            }
            it = slot_index.emplace(key, static_cast<int16_t>(slots.size()))
                     .first;
            slots.push_back({offset, size});
          }
          item.kind = Kind::kStore;
          item.slot = it->second;
        }
        // A store of another shape overwrites nothing this pass tracks as a
        // whole; it reads nothing either.
      } else if (num == OPCODE_LOAD_CONTEXT) {
        item.kind = Kind::kLoad;
        item.load_offset = static_cast<uint32_t>(i->src1.offset);
        item.load_size = static_cast<uint32_t>(GetTypeSize(i->dest->type));
      } else if (num == OPCODE_BRANCH) {
        item.unconditional = true;
      } else if (num == OPCODE_BRANCH_TRUE || num == OPCODE_BRANCH_FALSE) {
        // A branch within the function: followed below.
      } else if (num == OPCODE_RETURN || num == OPCODE_CALL ||
                 num == OPCODE_CALL_INDIRECT || num == OPCODE_RETURN_TRUE ||
                 num == OPCODE_CALL_TRUE ||
                 num == OPCODE_CALL_INDIRECT_TRUE ||
                 num == OPCODE_CALL_EXTERN) {
        // Ways out of the function. Which of them are a call or a return in
        // the guest's own sense matters at level 3 only, and it is not what
        // the opcode says: the scanner ends a function at an unconditional
        // branch and the front end turns that branch, and any branch to an
        // address outside the function's range, into a tail call. Such a
        // "call" is a jump inside one guest function, and the code it goes
        // to reads flags set before it like any other code after a branch.
        // So:
        //   a call that sets LR (no CALL_TAIL) and a kernel call are calls;
        //   a jump to LR (CALL_POSSIBLE_RETURN) and RETURN are returns;
        //   every other tail call reads everything.
        const bool conditional = num == OPCODE_RETURN_TRUE ||
                                 num == OPCODE_CALL_TRUE ||
                                 num == OPCODE_CALL_INDIRECT_TRUE;
        const bool is_return =
            num == OPCODE_RETURN || num == OPCODE_RETURN_TRUE ||
            ((num == OPCODE_CALL_INDIRECT ||
              num == OPCODE_CALL_INDIRECT_TRUE) &&
             (i->flags & CALL_POSSIBLE_RETURN));
        const bool is_tail =
            is_return || ((num == OPCODE_CALL || num == OPCODE_CALL_TRUE ||
                           num == OPCODE_CALL_INDIRECT ||
                           num == OPCODE_CALL_INDIRECT_TRUE) &&
                          (i->flags & CALL_TAIL));
        bool abi_point = level >= 3 && (is_return || !is_tail);
        // hakuX-Boosted: the guard's two exceptions.
        if (abi_point && is_return && keep_flags_at_returns) {
          abi_point = false;
        }
        if (abi_point && !is_return &&
            (num == OPCODE_CALL || num == OPCODE_CALL_TRUE) &&
            callee_reads_flags(num == OPCODE_CALL ? i->src1.symbol
                                                  : i->src2.symbol)) {
          abi_point = false;
          translation_stats.abi_guard_calls_kept.fetch_add(
              1, std::memory_order_relaxed);
        }
        if (is_tail && !conditional) {
          item.kind = abi_point ? Kind::kCallEnd : Kind::kEnd;
        } else {
          item.kind = abi_point ? Kind::kCall : Kind::kReadsAll;
        }
      } else if (num == OPCODE_CONTEXT_BARRIER) {
        // The barrier before a branch instruction is there for the per-block
        // passes (ContextBarrierFlags); any other says the context is
        // looked at.
        if (!(i->flags & CONTEXT_BARRIER_FOR_BRANCH)) {
          item.kind = Kind::kReadsAll;
        }
      } else if (num == OPCODE_CHECK_PREEMPT &&
                 cvars::dead_context_stores_over_preempt_checks) {
        // hakuX-Boosted: the scheduler's check reads nothing (see the
        // setting's text): item.kind stays kOther.
      } else if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
        // Calls, conditional returns, traps, debug breaks, the scheduler's
        // check and anything else marked as leaving the function's code.
        item.kind = Kind::kReadsAll;
      }
      if (num == OPCODE_BRANCH || num == OPCODE_BRANCH_TRUE ||
          num == OPCODE_BRANCH_FALSE) {
        const Label* label =
            num == OPCODE_BRANCH ? i->src1.label : i->src2.label;
        auto it = (label && label->block) ? block_index.find(label->block)
                                          : block_index.end();
        if (it == block_index.end()) {
          return true;  // a branch this pass cannot follow
        }
        item.target = static_cast<int32_t>(it->second);
      }
      code[b].push_back(item);
    }
  }
  if (slots.empty()) {
    return true;
  }
  SlotSet all;
  for (size_t n = 0; n < slots.size(); ++n) {
    all.set(n);
  }
  // What a load of [offset, offset + size) reads: every slot it overlaps.
  const auto slots_read = [&](uint32_t offset, uint32_t size) {
    SlotSet read;
    for (size_t n = 0; n < slots.size(); ++n) {
      if (slots[n].offset < offset + size &&
          offset < slots[n].offset + slots[n].size) {
        read.set(n);
      }
    }
    return read;
  };

  // What a call or return reads at level 3: everything but the fields the
  // ABI leaves undefined there.
  SlotSet read_by_call = all;
  for (size_t n = 0; n < slots.size(); ++n) {
    if (IsAbiVolatileFlag(slots[n].offset, slots[n].size)) {
      read_by_call.reset(n);
    }
  }

  const auto falls_through = [&](uint32_t b) {
    if (code[b].empty()) {
      return true;
    }
    const auto& last = code[b].back();
    return !(last.unconditional || last.kind == Kind::kEnd ||
             last.kind == Kind::kCallEnd);
  };

  // Backward to a fixed point: live(slot) = some path from here reads the
  // slot before storing to it. Falling off the last block is a return.
  std::vector<SlotSet> live_in(block_count);
  const auto walk = [&](uint32_t b, bool act) {
    SlotSet state;
    if (falls_through(b)) {
      // Falling off the last block: into whatever follows, which reads
      // anything.
      state = b + 1 < block_count ? live_in[b + 1] : all;
    }
    for (size_t n = code[b].size(); n-- > 0;) {
      Item& item = code[b][n];
      switch (item.kind) {
        case Kind::kStore:
          if (act && !state.test(item.slot)) {
            // Dead: every path from here stores the slot again first.
            item.kind = Kind::kOther;
            translation_stats.dead_context_stores_removed.fetch_add(
                1, std::memory_order_relaxed);
            if (cvars::dead_context_store_verify) {
              // Keep the store, with a value a load can recognise.
              if (slots[item.slot].size == 1) {
                item.instr->set_src2(
                    builder->LoadConstantUint8(kDeadStorePoison8));
              } else {
                item.instr->set_src2(
                    builder->LoadConstantUint64(kDeadStorePoison64));
              }
            } else {
              item.instr->UnlinkAndNOP();
            }
          } else {
            state.reset(item.slot);
          }
          break;
        case Kind::kLoad:
          state |= slots_read(item.load_offset, item.load_size);
          break;
        case Kind::kReadsAll:
          state = all;
          break;
        case Kind::kEnd:
          state = all;
          break;
        case Kind::kCall:
          state |= read_by_call;
          break;
        case Kind::kCallEnd:
          state = read_by_call;
          break;
        default:
          break;
      }
      if (item.target >= 0) {
        if (item.unconditional) {
          state = live_in[item.target];
        } else {
          state |= live_in[item.target];
        }
      }
    }
    return state;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t b = block_count; b-- > 0;) {
      const SlotSet state = walk(b, false);
      if (state != live_in[b]) {
        live_in[b] = state;
        changed = true;
      }
    }
  }
  translation_stats.dead_context_stores_seen.fetch_add(
      [&] {
        uint64_t n = 0;
        for (const auto& block_code : code) {
          for (const auto& item : block_code) {
            n += item.kind == Kind::kStore;
          }
        }
        return n;
      }(),
      std::memory_order_relaxed);
  // Act on the fixed point. Removing a dead store changes no liveness: a
  // dead store's slot is not live after it, so the state before it is the
  // state after it either way.
  for (uint32_t b = 0; b < block_count; ++b) {
    walk(b, true);
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
