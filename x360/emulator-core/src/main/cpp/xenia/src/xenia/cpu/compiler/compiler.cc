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

// hakuX-Boosted: where translation time goes, by pass, without a profiler.
DEFINE_bool(
    log_translation_stats, false,
    "Time every HIR compiler pass and log the running totals (functions "
    "compiled, milliseconds per pass, and how many times the simplification "
    "and constant propagation group reran before it settled) every 2048 "
    "functions. Two clock reads per pass per function while on; nothing "
    "while off.",
    "CPU");

namespace xe {
namespace cpu {
namespace compiler {

// hakuX-Boosted: totals for log_translation_stats. Several guest threads
// translate at once, each with its own Compiler from the translator pool but
// the same pass list, so the totals are shared and indexed by pass position.
std::atomic<uint64_t> translation_stats_group_runs{0};
std::atomic<uint64_t> translation_stats_group_iterations{0};

namespace {
constexpr size_t kMaxTimedPasses = 48;
std::atomic<uint64_t> pass_nanoseconds[kMaxTimedPasses];
std::atomic<uint64_t> functions_compiled{0};
constexpr uint64_t kStatsLogInterval = 2048;
}  // namespace

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
