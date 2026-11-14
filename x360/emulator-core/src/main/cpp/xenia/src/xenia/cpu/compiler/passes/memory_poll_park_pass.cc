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

  // Every backward or self branch names a candidate; of several to one
  // block the last in layout order is kept, so the run covers the others.
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
      bool merged = false;
      for (auto& candidate : candidates) {
        if (candidate.head == h) {
          // Later in layout order (x only grows, and so do the
          // instructions of one block).
          candidate.tail = x;
          candidate.back_branch = instr;
          merged = true;
          break;
        }
      }
      if (!merged) {
        candidates.push_back({h, x, instr});
      }
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
        if (op == &OPCODE_LOAD_info || op == &OPCODE_LOAD_OFFSET_info) {
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
    if (!pause_count) {
      reject("it does not pause", nullptr);
      continue;
    }
    if (!load_count) {
      reject("it loads no guest memory", nullptr);
      continue;
    }

    Instr* site = builder->SpinBackoff(1);
    site->flags = kSpinBackoffPollSite;
    site->src1.offset = (guest_address << 32) | 1;
    site->MoveBefore(candidate.back_branch);
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
        "{} load(s), {} pause(s)",
        guest_address, candidate.tail - candidate.head + 1, load_count,
        pause_count);
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

  XELOGI("MemoryPollPark: parked poll loop at guest {:08X}", guest_address);
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
