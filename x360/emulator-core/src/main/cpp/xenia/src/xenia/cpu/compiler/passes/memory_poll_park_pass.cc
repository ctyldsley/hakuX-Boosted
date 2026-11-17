/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/memory_poll_park_pass.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/hir/hir_builder.h"

DEFINE_bool(
    park_memory_poll_loops, true,
    "Give indefinite guest memory-poll loops (load, test, branch back - the "
    "shape GPU fence and frame waits take) the same adaptive spin-then-park "
    "backoff collapsed spin loops use, so a long wait stops burning a core. "
    "Nothing is removed or reordered: every iteration still runs, a young "
    "wait still spins cheap, only a wait that has proven long sleeps in "
    "bounded slices. Loops that read the clock, store, call or use atomics "
    "keep their real form.",
    "CPU");

// hakuX-Boosted: the busy wait this pass did not catch (run 90's profile:
// one guest thread at 61.6% of a core in a loop that pauses, reads a few
// words of memory, compares and goes round again). The loop is several
// blocks, and its pause is a counted loop of its own, so it is not the
// one-block shape above. See MarkPausePollLoops.
DEFINE_bool(
    park_pause_poll_loops, false,
    "Recognise guest loops of any number of blocks that do nothing but pause "
    "(the console's hint no-ops, a collapsed spin sled, or a counted loop of "
    "them) and read guest memory, and call the scheduler on every pass, so "
    "that with guest_scheduler_park_waits a thread stuck in one is parked "
    "until what it reads can have changed. Nothing is removed from the "
    "loop. Read when a function is translated.",
    "CPU");

DEFINE_bool(log_memory_poll_park, false,
            "Log every candidate memory-poll self-loop this pass rejects, "
            "with the guest address and the failing condition. Accepted "
            "loops are always logged.",
            "CPU");

// hakuX-Boosted (park-hot-loops, boosted/notes/park-hot-loops.md): run 146
// on Fable II marked the same three-block, four-load loop "every pass the
// same unless memory changes" in about eighty functions, and the main
// thread's poll sites then saw its registers change at 97.5% of looks
// (115,594 of 118,484 a second). A loop that loads its next address from
// memory through a register it wrote on the pass before ("p = p->next") is
// a walk, not a wait, and the stateless rule took it because it counted
// every loaded value as clean. With this on a loaded value is clean only if
// its address is.
DEFINE_bool(
    park_poll_loops_fixed_addresses, true,
    "With park_pause_poll_loops: a loop without a pause counts as a wait "
    "only if every address it loads from is the same on every pass unless "
    "memory changes (a walk along a list, which loads its next address "
    "through a register it wrote on the pass before, is refused). Fewer "
    "loops get a poll site; none that could be parked loses one unless its "
    "registers repeat with the walk. Read when a function is translated.",
    "CPU");
// hakuX-Boosted (park-hot-loops): briefs/park-rework-followup.md, item 2.
// A one-block loop that park_memory_poll_loops marks gets a plain backoff,
// which under the guest scheduler only yields once a slice: the scheduler's
// busy-wait parking never sees it.
DEFINE_bool(
    park_one_block_poll_loops, false,
    "With guest_scheduler_park_waits: the one-block memory-poll loops "
    "park_memory_poll_loops marks become poll sites like the loops of "
    "park_pause_poll_loops, so a thread stuck in one is parked once its "
    "registers have stayed the same for a while. Only loops whose load "
    "addresses are the same on every pass. Read when a function is "
    "translated.",
    "CPU");
// hakuX-Boosted (park-hot-loops): run 146's profile has Fable II's 3D Engine
// thread at 45.6% of its samples in sub_82B9BF90, whose only loop the log
// names is a one-block sled of pause hints at 82B9BFAC that loads nothing
// ("parked poll loop" was logged for it: it got a plain backoff site).
DEFINE_bool(
    park_delay_sleds, false,
    "With guest_scheduler_park_waits: a one-block counted loop of the "
    "console's pause hints that loads nothing (a delay the title spins in, "
    "marked by park_memory_poll_loops) reports to the scheduler every 64 "
    "passes. A thread that spends nearly all its time in such loops for "
    "guest_scheduler_park_wait_after_us is parked like an empty-yield "
    "spinner, for at most guest_scheduler_park_wait_max_us at a time. "
    "Nothing in the loop changes: the delay only lasts longer. Read when a "
    "function is translated.",
    "CPU");
// hakuX-Boosted (park-hot-loops): the same run had
// xe::kernel::PollSiteVisitFiber at 4.5% of all samples on the main thread:
// about 950,000 calls a second into a site whose registers almost never
// stayed the same.
DEFINE_bool(
    park_poll_site_backoff, true,
    "With park_pause_poll_loops: a poll site counts its passes inline and "
    "calls the scheduler only on the passes it looks at (one in eight), and "
    "a site whose registers keep changing is looked at less and less often "
    "(down to one pass in about 4,000), until a look finds them the same. "
    "False is the call on every pass. Read when a function is translated and "
    "at start-up.",
    "CPU");

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

using xe::cpu::hir::Block;
using xe::cpu::hir::Edge;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::hir::Value;

MemoryPollParkPass::MemoryPollParkPass() : CompilerPass() {}

MemoryPollParkPass::~MemoryPollParkPass() {}

bool MemoryPollParkPass::Run(HIRBuilder* builder) {
  marked_blocks_.clear();  // hakuX-Boosted: per function
  if (!cvars::park_memory_poll_loops) {
    return true;
  }
  // hakuX-Boosted: the pause-and-poll loops first; a block they took is
  // not looked at again below.
  if (cvars::park_pause_poll_loops) {
    MarkPausePollLoops(builder);
  }
  for (auto block = builder->first_block(); block; block = block->next) {
    TryInstrumentLoop(builder, block);
  }
  return true;
}

namespace {

// Pure data ops a poll body may massage the loaded value with. Each is
// time-invariant and side-effect free, so slowing the iteration cannot
// change what the loop computes.
bool IsAllowedDataOp(const OpcodeInfo* op) {
  return op == &OPCODE_ASSIGN_info || op == &OPCODE_CAST_info ||
         op == &OPCODE_ROTATE_LEFT_info || op == &OPCODE_SELECT_info ||
         op == &OPCODE_BYTE_SWAP_info || op == &OPCODE_TRUNCATE_info ||
         op == &OPCODE_ZERO_EXTEND_info || op == &OPCODE_SIGN_EXTEND_info ||
         op == &OPCODE_AND_info || op == &OPCODE_OR_info ||
         op == &OPCODE_XOR_info || op == &OPCODE_NOT_info ||
         op == &OPCODE_SHL_info || op == &OPCODE_SHR_info ||
         op == &OPCODE_SHA_info || op == &OPCODE_ADD_info ||
         op == &OPCODE_SUB_info || op == &OPCODE_COMPARE_EQ_info ||
         op == &OPCODE_COMPARE_NE_info || op == &OPCODE_COMPARE_SLT_info ||
         op == &OPCODE_COMPARE_SLE_info || op == &OPCODE_COMPARE_SGT_info ||
         op == &OPCODE_COMPARE_SGE_info || op == &OPCODE_COMPARE_ULT_info ||
         op == &OPCODE_COMPARE_ULE_info || op == &OPCODE_COMPARE_UGT_info ||
         op == &OPCODE_COMPARE_UGE_info;
}

}  // namespace

// hakuX-Boosted: park_pause_poll_loops.
//
// What is recognised. A loop, of one block or several, taken as the run of
// blocks in layout order from the target of a backward (or self) branch to
// the block holding that branch, for which all of this holds:
//  1. it is short: at most kMaxRegionBlocks blocks and kMaxRegionInstrs
//     instructions;
//  2. every instruction in it (in the last block: up to the backward
//     branch; what follows that is the code after the loop) is one of:
//     a no-op or a pause hint, a safepoint, a memory barrier, a read or
//     write of the thread's own registers, a load of guest memory, one of
//     the pure arithmetic and compare operations of IsAllowedDataOp, a
//     collapsed spin sled, or a branch to a label. Anything else rejects
//     the loop: a store to guest memory, a call of any kind, a return, an
//     atomic or reserved access, a clock read, a trap, a local, a
//     floating-point or vector operation, MMIO helpers;
//  3. it pauses: it holds a pause hint, a collapsed sled, or an inner block
//     that loops on itself without loading memory (the counted loop of
//     no-op register moves);
//  4. it loads guest memory at least once;
//  5. no block of it other than the first is entered from outside it.
// The pause is the marker that the code itself means to wait: a loop that
// scans memory (a string length, a search) is load, compare, branch as
// well, and does not pause.
//
// What is done. One OPCODE_SPIN_BACKOFF flagged kSpinBackoffPollSite is put
// in front of the backward branch. Nothing is removed, reordered or made
// conditional: every pass still pauses, loads and tests as it did. The
// backend calls the scheduler there on each pass, and the scheduler parks
// the thread only once its registers have been the same on pass after pass
// for a while (GuestScheduler::PollSiteVisit has that half and why a park
// can only cost time).
namespace {

constexpr size_t kMaxRegionBlocks = 8;
constexpr size_t kMaxRegionInstrs = 256;

Label* BranchTarget(const Instr* instr) {
  if (instr->opcode == &OPCODE_BRANCH_info) {
    return instr->src1.label;
  }
  if (instr->opcode == &OPCODE_BRANCH_TRUE_info ||
      instr->opcode == &OPCODE_BRANCH_FALSE_info) {
    return instr->src2.label;
  }
  return nullptr;
}

bool IsBranch(const Instr* instr) {
  return instr->opcode == &OPCODE_BRANCH_info ||
         instr->opcode == &OPCODE_BRANCH_TRUE_info ||
         instr->opcode == &OPCODE_BRANCH_FALSE_info;
}

uint64_t BlockGuestAddress(const Block* block) {
  for (const Instr* instr = block->instr_head; instr; instr = instr->next) {
    if (instr->opcode == &OPCODE_SOURCE_OFFSET_info) {
      return instr->src1.offset;
    }
  }
  return 0;
}

// hakuX-Boosted: is every pass of the loop the same as the last unless
// memory changes? ("Stateless": the second way a loop is known to be a
// wait, beside holding a pause.) True when no register the loop writes is
// written with a value that depends on what the loop itself left in a
// register on an earlier pass: every value stored to the context comes from
// constants, from loads of guest memory, from registers the loop never
// writes, or from registers it has already written, in this pass, with such
// a value. A scan or a search advances a pointer or an index from its own
// last value and fails this; so does a retry counter. What a loop that
// passes it computes can only differ from pass to pass if the memory it
// loads differs: it is waiting for memory to change, or it never ends.
//
// The check is deliberately short-sighted. What a block knows of the
// registers written so far is carried only into a block whose single way
// in is the block before it in the loop; any other join starts knowing
// nothing, and an unknown is taken as state. Wrong answers are then all on
// the side of "not stateless", which only leaves a loop as it was.
struct ContextRange {
  uint64_t begin;
  uint64_t end;
};

bool Overlaps(const std::vector<ContextRange>& ranges, uint64_t begin,
              uint64_t end) {
  for (const ContextRange& r : ranges) {
    if (begin < r.end && r.begin < end) {
      return true;
    }
  }
  return false;
}

// hakuX-Boosted (park-hot-loops): true when every value operand of |instr|
// passes |is_clean| (for a load: its address, and the offset of a
// load_offset).
template <typename Pred>
bool ValueSourcesClean(const Instr* instr, Pred is_clean) {
  const uint32_t sig = instr->opcode->signature;
  if (GET_OPCODE_SIG_TYPE_SRC1(sig) == OPCODE_SIG_TYPE_V &&
      !is_clean(instr->src1.value)) {
    return false;
  }
  if (GET_OPCODE_SIG_TYPE_SRC2(sig) == OPCODE_SIG_TYPE_V &&
      !is_clean(instr->src2.value)) {
    return false;
  }
  if (GET_OPCODE_SIG_TYPE_SRC3(sig) == OPCODE_SIG_TYPE_V &&
      !is_clean(instr->src3.value)) {
    return false;
  }
  return true;
}

// hakuX-Boosted (park-hot-loops): park_one_block_poll_loops. Is every
// address the one-block loop loads from the same on every pass? An address
// counts as fixed when it is built only from constants, from registers the
// loop never writes, and from registers this pass has already written with
// such a value; a loaded value never counts (what memory holds may change,
// and the brief asks for addresses that do not). Unknowns are taken as not
// fixed, which leaves the loop with the plain backoff it had.
bool OneBlockLoadAddressesFixed(const Block* block, const Instr* loop_branch) {
  std::vector<ContextRange> written;
  for (const Instr* instr = block->instr_head; instr && instr != loop_branch;
       instr = instr->next) {
    if (instr->opcode == &OPCODE_STORE_CONTEXT_info) {
      const uint64_t at = instr->src1.offset;
      written.push_back({at, at + GetTypeSize(instr->src2.value->type)});
    }
  }
  std::unordered_set<const Value*> fixed;
  std::vector<ContextRange> known;
  auto is_fixed = [&fixed](const Value* value) {
    return value && (value->IsConstant() || fixed.count(value));
  };
  size_t loads = 0;
  for (const Instr* instr = block->instr_head; instr && instr != loop_branch;
       instr = instr->next) {
    const OpcodeInfo* op = instr->opcode;
    if (op == &OPCODE_LOAD_CONTEXT_info) {
      const uint64_t at = instr->src1.offset;
      const uint64_t end = at + GetTypeSize(instr->dest->type);
      bool ok = !Overlaps(written, at, end);
      for (const ContextRange& r : known) {
        if (!ok && r.begin == at && r.end == end) {
          ok = true;
        }
      }
      if (ok) {
        fixed.insert(instr->dest);
      }
      continue;
    }
    if (op == &OPCODE_STORE_CONTEXT_info) {
      const uint64_t at = instr->src1.offset;
      const uint64_t end = at + GetTypeSize(instr->src2.value->type);
      known.erase(std::remove_if(known.begin(), known.end(),
                                 [at, end](const ContextRange& r) {
                                   return at < r.end && r.begin < end;
                                 }),
                  known.end());
      if (is_fixed(instr->src2.value)) {
        known.push_back({at, end});
      }
      continue;
    }
    if (op == &OPCODE_LOAD_info || op == &OPCODE_LOAD_OFFSET_info ||
        op == &OPCODE_RESERVED_LOAD_info) {
      if (!ValueSourcesClean(instr, is_fixed)) {
        return false;
      }
      ++loads;
      continue;  // its value is not fixed
    }
    if (instr->dest && ValueSourcesClean(instr, is_fixed)) {
      fixed.insert(instr->dest);
    }
  }
  return loads != 0;
}

bool IsStatelessLoop(const std::vector<Block*>& blocks, size_t head,
                     size_t tail, const Instr* back_branch) {
  // Every stretch of the context the loop writes.
  std::vector<ContextRange> written;
  for (size_t b = head; b <= tail; ++b) {
    for (const Instr* instr = blocks[b]->instr_head; instr;
         instr = instr->next) {
      if (b == tail && instr == back_branch) {
        break;
      }
      if (instr->opcode == &OPCODE_STORE_CONTEXT_info) {
        const uint64_t at = instr->src1.offset;
        written.push_back(
            {at, at + GetTypeSize(instr->src2.value->type)});
      }
    }
  }
  std::unordered_set<const Value*> clean;
  // Per block, the written stretches that hold a clean value at its end.
  std::vector<std::vector<ContextRange>> clean_out(tail - head + 1);
  auto value_clean = [&clean](const Value* value) {
    return value && (value->IsConstant() || clean.count(value));
  };
  for (size_t b = head; b <= tail; ++b) {
    Block* block = blocks[b];
    std::vector<ContextRange> known;
    if (b != head) {
      const Edge* in = block->incoming_edge_head;
      if (in && !in->incoming_next && in->src == blocks[b - 1]) {
        known = clean_out[b - 1 - head];
      }
    }
    for (const Instr* instr = block->instr_head; instr; instr = instr->next) {
      if (b == tail && instr == back_branch) {
        break;
      }
      const OpcodeInfo* op = instr->opcode;
      if (op == &OPCODE_LOAD_CONTEXT_info) {
        const uint64_t at = instr->src1.offset;
        const uint64_t end = at + GetTypeSize(instr->dest->type);
        bool ok = !Overlaps(written, at, end);
        if (!ok) {
          // Written by the loop: clean only if this pass has already
          // written exactly this stretch with a clean value.
          for (const ContextRange& r : known) {
            if (r.begin == at && r.end == end) {
              ok = true;
              break;
            }
          }
        }
        if (ok) {
          clean.insert(instr->dest);
        }
        continue;
      }
      if (op == &OPCODE_STORE_CONTEXT_info) {
        const uint64_t at = instr->src1.offset;
        const uint64_t end = at + GetTypeSize(instr->src2.value->type);
        if (!value_clean(instr->src2.value)) {
          return false;
        }
        // Whatever was known of an overlapping stretch is replaced.
        known.erase(std::remove_if(known.begin(), known.end(),
                                   [at, end](const ContextRange& r) {
                                     return at < r.end && r.begin < end;
                                   }),
                    known.end());
        known.push_back({at, end});
        continue;
      }
      if (op == &OPCODE_LOAD_info || op == &OPCODE_LOAD_OFFSET_info ||
          op == &OPCODE_RESERVED_LOAD_info) {
        // What memory holds is what the loop waits for. (The address is a
        // register value; a loop that walks memory has changed a register
        // from its own last value and has already failed above.)
        // hakuX-Boosted (park-hot-loops): not so. "p = p->next" stores a
        // loaded value, which was counted clean whatever its address. With
        // park_poll_loops_fixed_addresses a loaded value is clean only if
        // its address is, so the walk's store of p fails below.
        if (cvars::park_poll_loops_fixed_addresses &&
            !ValueSourcesClean(instr, value_clean)) {
          continue;
        }
        if (instr->dest) {
          clean.insert(instr->dest);
        }
        continue;
      }
      if (instr->dest) {
        // A data operation: clean if everything it reads is.
        const uint32_t sig = op->signature;
        bool ok = true;
        if (GET_OPCODE_SIG_TYPE_SRC1(sig) == OPCODE_SIG_TYPE_V) {
          ok = ok && value_clean(instr->src1.value);
        }
        if (GET_OPCODE_SIG_TYPE_SRC2(sig) == OPCODE_SIG_TYPE_V) {
          ok = ok && value_clean(instr->src2.value);
        }
        if (GET_OPCODE_SIG_TYPE_SRC3(sig) == OPCODE_SIG_TYPE_V) {
          ok = ok && value_clean(instr->src3.value);
        }
        if (ok) {
          clean.insert(instr->dest);
        }
      }
    }
    clean_out[b - head] = std::move(known);
  }
  return true;
}

struct RegionCandidate {
  size_t head;        // position of the loop's first block
  size_t tail;        // position of the block with the backward branch
  Instr* back_branch;
};

}  // namespace

void MemoryPollParkPass::MarkPausePollLoops(HIRBuilder* builder) {
  std::vector<Block*> blocks;
  std::unordered_map<const Block*, size_t> position;
  for (auto block = builder->first_block(); block; block = block->next) {
    position[block] = blocks.size();
    blocks.push_back(block);
  }

  // Every backward or self branch names a candidate of its own.
  // hakuX-Boosted: they used to be merged per head block, keeping only the
  // branch that comes last in layout order. A wait loop whose head is also
  // the head of a larger loop ("wait; then work; and round again", which is
  // how a worker's loop and the first bench shape are laid out) was then
  // only ever judged as the larger loop, which calls and stores, and was
  // refused. Each is now judged, the largest first, so a refused outer loop
  // leaves the inner one to be taken.
  std::vector<RegionCandidate> candidates;
  for (size_t x = 0; x < blocks.size(); ++x) {
    for (Instr* instr = blocks[x]->instr_head; instr; instr = instr->next) {
      Label* target = BranchTarget(instr);
      if (!target || !target->block) {
        continue;
      }
      auto it = position.find(target->block);
      if (it == position.end() || it->second > x) {
        continue;
      }
      const size_t h = it->second;
      if (x - h + 1 > kMaxRegionBlocks) {
        continue;
      }
      candidates.push_back({h, x, instr});
    }
  }
  if (candidates.empty()) {
    return;
  }
  // Outer loops first: the wait is the outer loop, its pause the inner.
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const RegionCandidate& a, const RegionCandidate& b) {
                     return (a.tail - a.head) > (b.tail - b.head);
                   });

  std::vector<bool> taken(blocks.size(), false);
  for (const RegionCandidate& candidate : candidates) {
    const uint64_t guest_address = BlockGuestAddress(blocks[candidate.head]);
    auto reject = [&](const char* reason, const char* detail) {
      if (cvars::log_memory_poll_park) {
        XELOGI(
            "MemoryPollPark: loop at guest {:08X} ({} block(s)) is not a "
            "pause-and-poll loop: {}{}",
            guest_address, candidate.tail - candidate.head + 1, reason,
            detail ? detail : "");
      }
    };
    bool overlaps = false;
    for (size_t b = candidate.head; b <= candidate.tail; ++b) {
      if (taken[b]) {
        overlaps = true;
        break;
      }
    }
    if (overlaps) {
      continue;
    }

    size_t instr_count = 0;
    size_t load_count = 0;
    size_t pause_count = 0;
    bool ok = true;
    for (size_t b = candidate.head; b <= candidate.tail && ok; ++b) {
      Block* block = blocks[b];
      // 5: entered from outside only at the top.
      if (b != candidate.head) {
        for (Edge* e = block->incoming_edge_head; e; e = e->incoming_next) {
          auto it = position.find(e->src);
          if (it == position.end() || it->second < candidate.head ||
              it->second > candidate.tail) {
            reject("a block inside it is entered from outside", nullptr);
            ok = false;
            break;
          }
        }
        if (!ok) {
          break;
        }
      }
      bool self_loop = false;
      for (Edge* e = block->outgoing_edge_head; e; e = e->outgoing_next) {
        if (e->dest == block) {
          self_loop = true;
          break;
        }
      }
      size_t block_loads = 0;
      for (Instr* instr = block->instr_head; instr; instr = instr->next) {
        if (b == candidate.tail && instr == candidate.back_branch) {
          break;
        }
        if (instr->IsFake()) {
          continue;
        }
        if (++instr_count > kMaxRegionInstrs) {
          reject("too long", nullptr);
          ok = false;
          break;
        }
        const OpcodeInfo* op = instr->opcode;
        if (op == &OPCODE_DELAY_EXECUTION_info ||
            op == &OPCODE_SPIN_BACKOFF_info) {
          if (op == &OPCODE_SPIN_BACKOFF_info &&
              (instr->flags & kSpinBackoffPollSite)) {
            reject("already holds a poll site", nullptr);
            ok = false;
            break;
          }
          ++pause_count;
          continue;
        }
        if (op == &OPCODE_CHECK_PREEMPT_info ||
            op == &OPCODE_MEMORY_BARRIER_info ||
            op == &OPCODE_LOAD_CONTEXT_info ||
            op == &OPCODE_STORE_CONTEXT_info) {
          continue;
        }
        // hakuX-Boosted: a reserved load (lwarx) reads like any load. A
        // spin lock's wait is "lwarx, compare, branch back while held"; the
        // conditional store that takes the lock is outside such a loop (a
        // loop holding it is refused below as before), and every pass
        // starts with its own lwarx, so a park between two passes loses no
        // reservation the guest relies on.
        if (op == &OPCODE_LOAD_info || op == &OPCODE_LOAD_OFFSET_info ||
            op == &OPCODE_RESERVED_LOAD_info) {
          ++block_loads;
          continue;
        }
        if (IsBranch(instr)) {
          if (!BranchTarget(instr)) {
            reject("a branch without a label", nullptr);
            ok = false;
            break;
          }
          continue;
        }
        if (IsAllowedDataOp(op)) {
          // Integer work only: a loop that computes in floating point or
          // vectors is not a pause-and-poll loop.
          if (instr->dest && (instr->dest->type == FLOAT32_TYPE ||
                              instr->dest->type == FLOAT64_TYPE ||
                              instr->dest->type == VEC128_TYPE)) {
            reject("floating-point or vector work in it", nullptr);
            ok = false;
            break;
          }
          continue;
        }
        reject("an instruction that is neither pause nor poll: ",
               GetOpcodeName(op));
        ok = false;
        break;
      }
      load_count += block_loads;
      // 3: an inner loop that reads no memory is a pause.
      if (self_loop && !block_loads &&
          !(b == candidate.head && b == candidate.tail)) {
        ++pause_count;
      }
    }
    if (!ok) {
      continue;
    }
    if (!load_count) {
      reject("it loads no guest memory", nullptr);
      continue;
    }
    // hakuX-Boosted: a loop that holds no pause is still a wait when every
    // pass of it is the same unless memory changes (IsStatelessLoop):
    // "while (!flag) {}" with no hint in it.
    const bool stateless = IsStatelessLoop(blocks, candidate.head,
                                           candidate.tail,
                                           candidate.back_branch);
    if (!pause_count && !stateless) {
      reject("it does not pause, and a register it writes depends on an "
             "earlier pass",
             nullptr);
      continue;
    }

    // hakuX-Boosted: one site in front of every branch of the loop that
    // goes back to its head, not only the last in layout order. A pass
    // takes exactly one of them, so it still visits exactly one site; with
    // the site at the last branch only, a loop whose waiting path goes back
    // from an earlier block (the bench's spin lock: the wait returns to the
    // head before the block that takes the lock) never reached it.
    for (const RegionCandidate& other : candidates) {
      if (other.head != candidate.head || other.tail > candidate.tail) {
        continue;
      }
      Instr* site = builder->SpinBackoff(1);
      site->flags = kSpinBackoffPollSite;
      site->src1.offset = (guest_address << 32) | 1;
      site->MoveBefore(other.back_branch);
    }
    for (size_t b = candidate.head; b <= candidate.tail; ++b) {
      taken[b] = true;
    }
    marked_blocks_.clear();
    for (size_t b = 0; b < blocks.size(); ++b) {
      if (taken[b]) {
        marked_blocks_.push_back(blocks[b]);
      }
    }
    XELOGI(
        "MemoryPollPark: pause-and-poll loop at guest {:08X}: {} block(s), "
        "{} load(s), {} pause(s){}",
        guest_address, candidate.tail - candidate.head + 1, load_count,
        pause_count,
        stateless ? ", every pass the same unless memory changes" : "");
  }
}

bool MemoryPollParkPass::TryInstrumentLoop(HIRBuilder* builder, Block* block) {
  // hakuX-Boosted: a block of a pause-and-poll loop has its site already.
  if (std::find(marked_blocks_.begin(), marked_blocks_.end(), block) !=
      marked_blocks_.end()) {
    return false;
  }
  // Candidacy by CFG ground truth, exactly like the collapse passes. Entry
  // blocks are eligible: real poll loops sit there (the guard that excluded
  // them elsewhere cost weeks once already).
  bool has_self_edge = false;
  for (Edge* e = block->outgoing_edge_head; e; e = e->outgoing_next) {
    if (e->dest == block) {
      has_self_edge = true;
      break;
    }
  }
  if (!has_self_edge) {
    return false;
  }

  uint64_t guest_address = 0;
  for (Instr* instr = block->instr_head; instr; instr = instr->next) {
    if (instr->opcode == &OPCODE_SOURCE_OFFSET_info) {
      guest_address = instr->src1.offset;
      break;
    }
  }
  auto reject = [&](const char* reason, const char* detail) {
    if (cvars::log_memory_poll_park) {
      XELOGI("MemoryPollPark: self-loop at guest {:08X} rejected: {}{}",
             guest_address, reason, detail ? detail : "");
    }
    return false;
  };

  // The loop-back branch. Conditional either way around: polls exit on
  // "value changed" (branch_true on NE) or "flag still clear" (branch_false
  // on EQ) depending on how the guest compiler leaned.
  Instr* loop_branch = nullptr;
  for (Instr* instr = block->instr_head; instr; instr = instr->next) {
    Label* target = nullptr;
    if (instr->opcode == &OPCODE_BRANCH_info) {
      target = instr->src1.label;
    } else if (instr->opcode == &OPCODE_BRANCH_TRUE_info ||
               instr->opcode == &OPCODE_BRANCH_FALSE_info) {
      target = instr->src2.label;
    } else {
      continue;
    }
    if (!target || target->block != block) {
      continue;
    }
    if (instr->opcode == &OPCODE_BRANCH_info) {
      return reject("self-branch is unconditional", nullptr);
    }
    if (loop_branch) {
      return reject("multiple self-branches", nullptr);
    }
    loop_branch = instr;
  }
  if (!loop_branch) {
    return reject("self edge exists but no self-targeting branch", nullptr);
  }

  // Body scan up to the loop-back branch. Default-deny: anything outside the
  // allowlist keeps the loop untouched. Stores to guest MEMORY, calls,
  // atomics and clock reads disqualify; context traffic is this thread's own
  // architectural state and merely repeats, so it is safe to slow down.
  bool saw_load = false;
  size_t delay_count = 0;
  std::unordered_set<const Value*> load_tainted;
  for (Instr* instr = block->instr_head; instr != loop_branch;
       instr = instr->next) {
    if (instr->IsFake()) {
      continue;
    }
    const OpcodeInfo* op = instr->opcode;
    if (op == &OPCODE_DELAY_EXECUTION_info) {
      ++delay_count;
      continue;
    }
    if (op == &OPCODE_CHECK_PREEMPT_info ||
        op == &OPCODE_MEMORY_BARRIER_info ||
        op == &OPCODE_LOAD_CONTEXT_info ||
        op == &OPCODE_STORE_CONTEXT_info) {
      continue;
    }
    if (op == &OPCODE_LOAD_info || op == &OPCODE_LOAD_OFFSET_info) {
      saw_load = true;
      load_tainted.insert(instr->dest);
      continue;
    }
    if (IsAllowedDataOp(op)) {
      // Propagate "derived from the polled load" through the data chain.
      if (instr->dest) {
        Value* srcs[3] = {nullptr, nullptr, nullptr};
        uint32_t sig = op->signature;
        if (GET_OPCODE_SIG_TYPE_SRC1(sig) == OPCODE_SIG_TYPE_V) {
          srcs[0] = instr->src1.value;
        }
        if (GET_OPCODE_SIG_TYPE_SRC2(sig) == OPCODE_SIG_TYPE_V) {
          srcs[1] = instr->src2.value;
        }
        if (GET_OPCODE_SIG_TYPE_SRC3(sig) == OPCODE_SIG_TYPE_V) {
          srcs[2] = instr->src3.value;
        }
        for (Value* src : srcs) {
          if (src && load_tainted.count(src)) {
            load_tainted.insert(instr->dest);
            break;
          }
        }
      }
      continue;
    }
    return reject("non-whitelisted opcode in body: ", GetOpcodeName(op));
  }
  // Two acceptable shapes: a memory poll must exit on the value it loads; a
  // delay sled (db16cyc run with a GPR counter) exists to burn time, so
  // parking in it is its purpose. A lone hint does not qualify.
  constexpr size_t kMinDelaySled = 4;
  bool is_delay_loop = delay_count >= kMinDelaySled;
  if (!saw_load && !is_delay_loop) {
    return reject("no memory load and no delay sled in body", nullptr);
  }
  if (saw_load && !is_delay_loop &&
      !load_tainted.count(loop_branch->src1.value)) {
    return reject("branch condition not derived from the polled load",
                  nullptr);
  }

  // Inject the adaptive backoff ahead of the loop-back branch, feeding the
  // same spin-then-park escalation collapsed spin loops already use. It runs
  // on the exiting iteration too, so a parked wait pays at most one extra
  // sleep quantum of exit latency - the same order as its polling cadence.
  Instr* backoff = builder->SpinBackoff(1);
  backoff->MoveBefore(loop_branch);

  // hakuX-Boosted (park-hot-loops): this line is logged when the loop is
  // marked, not when a thread sleeps in it; the plain backoff above only
  // yields once a slice under the guest scheduler. With
  // park_one_block_poll_loops or park_delay_sleds the site reports to the
  // scheduler instead, which can park the thread.
  const uint16_t kind =
      OneBlockSiteKind(block, loop_branch, saw_load, is_delay_loop);
  if (kind) {
    backoff->flags = kind;
    backoff->src1.offset = (guest_address << 32) | 1;
  }
  XELOGI("MemoryPollPark: parked poll loop at guest {:08X}{}", guest_address,
         kind == kSpinBackoffPollSite
             ? " (a poll site: its load addresses are the same every pass)"
             : kind == kSpinBackoffDelaySite
                   ? " (a delay site: a sled of pause hints that loads nothing)"
                   : "");
  return true;
}

uint16_t MemoryPollParkPass::OneBlockSiteKind(Block* block, Instr* loop_branch,
                                              bool saw_load,
                                              bool is_delay_loop) {
  if (!saw_load) {
    // Accepted above only as a delay sled.
    return (cvars::park_delay_sleds && is_delay_loop) ? kSpinBackoffDelaySite
                                                      : 0;
  }
  if (!cvars::park_one_block_poll_loops) {
    return 0;
  }
  if (!OneBlockLoadAddressesFixed(block, loop_branch)) {
    if (cvars::log_memory_poll_park) {
      XELOGI(
          "MemoryPollPark: self-loop at guest {:08X} stays a plain backoff: "
          "an address it loads from can change between passes",
          BlockGuestAddress(block));
    }
    return 0;
  }
  return kSpinBackoffPollSite;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
