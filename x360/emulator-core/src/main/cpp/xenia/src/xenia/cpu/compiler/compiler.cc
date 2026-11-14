/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/compiler.h"

#include <atomic>
#include <chrono>
#include <string>
#include <typeinfo>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/compiler/compiler_pass.h"
#include "xenia/cpu/compiler/translation_stats.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_context.h"

// hakuX-Boosted: where translation time goes, by pass, without a profiler.
DEFINE_bool(
    log_translation_stats, false,
    "Time every HIR compiler pass and log the running totals (functions "
    "compiled, milliseconds per pass, and how many times the simplification "
    "and constant propagation group reran before it settled) every 2048 "
    "functions, with a count of the guest register loads and stores left in "
    "the code after the passes, by register. Two clock reads per pass and "
    "one walk of the finished HIR per function while on; nothing while off.",
    "CPU");

namespace xe {
namespace cpu {
namespace compiler {

// hakuX-Boosted: totals for log_translation_stats. Several guest threads
// translate at once, each with its own Compiler from the translator pool but
// the same pass list, so the totals are shared and indexed by pass position.
TranslationStats translation_stats;
std::atomic<uint64_t> translation_stats_group_runs{0};
std::atomic<uint64_t> translation_stats_group_iterations{0};

namespace {
constexpr size_t kMaxTimedPasses = 48;
std::atomic<uint64_t> pass_nanoseconds[kMaxTimedPasses];
std::atomic<uint64_t> functions_compiled{0};
constexpr uint64_t kStatsLogInterval = 2048;

// What is left in the HIR when the passes are done, summed over every
// function translated: these are static counts (each instruction once,
// however often it runs), enough to see which guest registers the
// per-block model reloads and re-stores most.
std::atomic<uint64_t> hir_blocks{0};
std::atomic<uint64_t> hir_instrs{0};
std::atomic<uint64_t> gpr_loads[32];
std::atomic<uint64_t> gpr_stores[32];
// 0 lr, 1 ctr, 2 cr bytes, 3 xer bytes, 4 fpr, 5 vr, 6 anything else.
constexpr size_t kOtherBuckets = 7;
const char* const kOtherBucketNames[kOtherBuckets] = {
    "lr", "ctr", "cr", "xer", "fpr", "vr", "other"};
std::atomic<uint64_t> other_loads[kOtherBuckets];
std::atomic<uint64_t> other_stores[kOtherBuckets];

void CountContextAccess(uint64_t offset, bool is_store) {
  using ppc::PPCContext;
  const auto bump = [is_store](std::atomic<uint64_t>* loads,
                               std::atomic<uint64_t>* stores, size_t index) {
    (is_store ? stores : loads)[index].fetch_add(1, std::memory_order_relaxed);
  };
  const uint64_t gpr_base = offsetof(PPCContext, r);
  if (offset >= gpr_base && offset < gpr_base + 32 * 8) {
    bump(gpr_loads, gpr_stores, static_cast<size_t>((offset - gpr_base) / 8));
    return;
  }
  size_t bucket = 6;
  if (offset >= offsetof(PPCContext, lr) &&
      offset < offsetof(PPCContext, lr) + 8) {
    bucket = 0;
  } else if (offset >= offsetof(PPCContext, ctr) &&
             offset < offsetof(PPCContext, ctr) + 8) {
    bucket = 1;
  } else if (offset >= offsetof(PPCContext, cr0) &&
             offset < offsetof(PPCContext, cr0) + 32) {
    bucket = 2;
  } else if (offset >= offsetof(PPCContext, xer_ca) &&
             offset <= offsetof(PPCContext, xer_so)) {
    bucket = 3;
  } else if (offset >= offsetof(PPCContext, f) &&
             offset < offsetof(PPCContext, f) + 32 * 8) {
    bucket = 4;
  } else if (offset >= offsetof(PPCContext, v) &&
             offset < offsetof(PPCContext, v) + 128 * 16) {
    bucket = 5;
  }
  bump(other_loads, other_stores, bucket);
}

void CountFinishedHir(hir::HIRBuilder* builder) {
  uint64_t blocks = 0;
  uint64_t instrs = 0;
  // hakuX-Boosted: static block counts by whether the function is pinned.
  // (The dynamic counts are made by the emitted code.)
  for (auto block = builder->first_block(); block; block = block->next) {
    ++blocks;
    for (auto i = block->instr_head; i; i = i->next) {
      ++instrs;
      if (i->opcode == &hir::OPCODE_LOAD_CONTEXT_info) {
        CountContextAccess(i->src1.offset, false);
      } else if (i->opcode == &hir::OPCODE_STORE_CONTEXT_info) {
        CountContextAccess(i->src1.offset, true);
      }
    }
  }
  (builder->pinned_context_slots().empty() ? translation_stats.blocks_unpinned
                                           : translation_stats.blocks_pinned)
      .fetch_add(blocks, std::memory_order_relaxed);
  hir_blocks.fetch_add(blocks, std::memory_order_relaxed);
  hir_instrs.fetch_add(instrs, std::memory_order_relaxed);
}

std::string FormatContextCounts() {
  std::string line = fmt::format(
      " blocks {}, HIR instructions {}; guest register loads/stores left:",
      hir_blocks.load(std::memory_order_relaxed),
      hir_instrs.load(std::memory_order_relaxed));
  for (size_t n = 0; n < 32; ++n) {
    line += fmt::format(" r{} {}/{}", n,
                        gpr_loads[n].load(std::memory_order_relaxed),
                        gpr_stores[n].load(std::memory_order_relaxed));
  }
  for (size_t n = 0; n < kOtherBuckets; ++n) {
    line += fmt::format(" {} {}/{}", kOtherBucketNames[n],
                        other_loads[n].load(std::memory_order_relaxed),
                        other_stores[n].load(std::memory_order_relaxed));
  }
  return line;
}

// hakuX-Boosted: the counters of translation_stats.h, on a line of their own.
std::string FormatExtraStats() {
  const auto get = [](const std::atomic<uint64_t>& v) {
    return v.load(std::memory_order_relaxed);
  };
  const auto& t = translation_stats;
  std::string line = fmt::format(
      " pinning: {} functions considered, {} with loops, {} pinned; slots "
      "gpr {} fpr {} vr {}; context loads {} and stores {} became register "
      "moves; {} sync sites, {} resume stubs, {} resume entries, {} verify "
      "mismatches; pinned gprs:",
      get(t.pin_functions_considered), get(t.pin_functions_with_loops),
      get(t.pin_functions_pinned), get(t.pin_gpr_slots), get(t.pin_fpr_slots),
      get(t.pin_vr_slots), get(t.pin_loads), get(t.pin_stores),
      get(t.pin_sync_sites), get(t.pin_resume_stubs),
      get(t.pin_resume_entries), get(t.pin_verify_mismatches));
  for (size_t n = 0; n < 34; ++n) {
    const uint64_t count = get(t.pin_by_gpr[n]);
    if (count) {
      line += n < 32 ? fmt::format(" r{} {}", n, count)
                     : fmt::format(" {} {}", n == 32 ? "ctr" : "lr", count);
    }
  }
  // What pin_guest_registers_wide adds, and how much of the code pinning
  // reaches: statically (blocks translated) and, in code translated while
  // this setting was on, dynamically (blocks executed).
  const uint64_t static_pinned = get(t.blocks_pinned);
  const uint64_t static_all = static_pinned + get(t.blocks_unpinned);
  const uint64_t run_pinned = t.blocks_run_pinned;
  const uint64_t run_all = run_pinned + t.blocks_run_unpinned;
  line += fmt::format(
      "; wide: {} pinned functions have no loop, {} flag slots, sync loads "
      "{} and stores {} emitted; blocks translated in pinned functions {} "
      "of {} ({:.1f}%); blocks executed in pinned functions {} of {} "
      "({:.1f}%)",
      get(t.pin_functions_pinned_no_loop), get(t.pin_flag_slots),
      get(t.pin_sync_loads), get(t.pin_sync_stores), static_pinned,
      static_all, static_all ? 100.0 * static_pinned / static_all : 0.0,
      run_pinned, run_all, run_all ? 100.0 * run_pinned / run_all : 0.0);
  line += fmt::format(
      "; simplification: {} block walks, {} skipped, {} skip mismatches",
      get(t.simplify_block_walks), get(t.simplify_block_skips),
      get(t.simplify_skip_mismatches));
  line += fmt::format(
      "; dead context stores: {} of {} candidate stores removed; "
      "self-check mismatches (TranslatorVerify) {}",
      get(t.dead_context_stores_removed), get(t.dead_context_stores_seen),
      get(t.verify_mismatches));
  return line;
}
}  // namespace

uint64_t SelfCheckMismatches() {
  const auto& t = translation_stats;
  return t.pin_verify_mismatches.load(std::memory_order_relaxed) +
         t.simplify_skip_mismatches.load(std::memory_order_relaxed) +
         t.verify_mismatches.load(std::memory_order_relaxed);
}

std::string FormatTranslationStats() {
  return fmt::format(
      "TranslationStats: {} functions (on request)\nTranslationStats:{}\n"
      "TranslationStats:{}\n",
      functions_compiled.load(std::memory_order_relaxed),
      FormatContextCounts(), FormatExtraStats());
}

Compiler::Compiler(Processor* processor) : processor_(processor) {}

Compiler::~Compiler() { Reset(); }

void Compiler::AddPass(std::unique_ptr<CompilerPass> pass) {
  pass->Initialize(this);
  passes_.push_back(std::move(pass));
}

void Compiler::Reset() {}

bool Compiler::Compile(xe::cpu::hir::HIRBuilder* builder) {
  if (cvars::log_translation_stats) {
    // hakuX-Boosted: the same loop as below with a clock around each pass.
    using clock = std::chrono::steady_clock;
    for (size_t i = 0; i < passes_.size(); ++i) {
      auto& pass = passes_[i];
      scratch_arena_.Reset();
      const auto start = clock::now();
      const bool ok = pass->Run(builder);
      const uint64_t elapsed =
          static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  clock::now() - start)
                  .count());
      if (i < kMaxTimedPasses) {
        pass_nanoseconds[i].fetch_add(elapsed, std::memory_order_relaxed);
      }
      if (!ok) {
        return false;
      }
    }
    CountFinishedHir(builder);
    const uint64_t count =
        functions_compiled.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count % kStatsLogInterval == 0) {
      std::string line;
      uint64_t total = 0;
      for (size_t i = 0; i < passes_.size() && i < kMaxTimedPasses; ++i) {
        const uint64_t ns =
            pass_nanoseconds[i].load(std::memory_order_relaxed);
        total += ns;
        const auto& pass_ref = *passes_[i];
        line += fmt::format(" [{}] {} {:.1f}ms;", i, typeid(pass_ref).name(),
                            static_cast<double>(ns) / 1e6);
      }
      XELOGI(
          "TranslationStats: {} functions, HIR passes {:.1f} ms in all; "
          "simplify/constprop group ran {} times, {} iterations;{}",
          count, static_cast<double>(total) / 1e6,
          translation_stats_group_runs.load(std::memory_order_relaxed),
          translation_stats_group_iterations.load(std::memory_order_relaxed),
          line);
      XELOGI("TranslationStats:{}", FormatContextCounts());
      XELOGI("TranslationStats:{}", FormatExtraStats());
    }
    return true;
  }

  // TODO(benvanik): sophisticated stuff. Run passes in parallel, run until they
  //                 stop changing things, etc.
  for (size_t i = 0; i < passes_.size(); ++i) {
    auto& pass = passes_[i];
    scratch_arena_.Reset();
    if (!pass->Run(builder)) {
      return false;
    }
  }

  return true;
}

}  // namespace compiler
}  // namespace cpu
}  // namespace xe
