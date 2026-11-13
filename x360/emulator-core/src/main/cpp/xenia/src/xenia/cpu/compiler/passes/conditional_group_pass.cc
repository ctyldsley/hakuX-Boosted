/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/conditional_group_pass.h"

#include <atomic>

#include "xenia/base/cvar.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"

DECLARE_bool(log_translation_stats);

namespace xe {
namespace cpu {
namespace compiler {

// hakuX-Boosted: defined in compiler.cc, for log_translation_stats.
extern std::atomic<uint64_t> translation_stats_group_runs;
extern std::atomic<uint64_t> translation_stats_group_iterations;

namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Block;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::hir::Value;

ConditionalGroupPass::ConditionalGroupPass() : CompilerPass() {}

ConditionalGroupPass::~ConditionalGroupPass() {}

bool ConditionalGroupPass::Initialize(Compiler* compiler) {
  if (!CompilerPass::Initialize(compiler)) {
    return false;
  }

  for (size_t i = 0; i < passes_.size(); ++i) {
    auto& pass = passes_[i];
    if (!pass->Initialize(compiler)) {
      return false;
    }
  }

  return true;
}

bool ConditionalGroupPass::Run(HIRBuilder* builder) {
  bool dirty;
  // hakuX-Boosted: how often the group reruns, for log_translation_stats.
  const bool count_iterations = cvars::log_translation_stats;
  if (count_iterations) {
    translation_stats_group_runs.fetch_add(1, std::memory_order_relaxed);
  }
  // hakuX-Boosted: block stamps (simplification_skip_unchanged_blocks) are
  // comparable from here to the end of this run and no further: clean stamps
  // at or below the floor are ignored.
  builder->set_pass_stamp_floor(builder->NextPassStamp());
  do {
    dirty = false;
    if (count_iterations) {
      translation_stats_group_iterations.fetch_add(1,
                                                   std::memory_order_relaxed);
    }
    for (size_t i = 0; i < passes_.size(); ++i) {
      scratch_arena()->Reset();
      auto& pass = passes_[i];
      auto subpass = dynamic_cast<ConditionalGroupSubpass*>(pass.get());
      if (!subpass) {
        if (!pass->Run(builder)) {
          builder->set_pass_stamp_floor(UINT32_MAX);  // hakuX-Boosted
          return false;
        }
        // hakuX-Boosted: a member that does not report what it changed
        // (only ValidationPass today, which changes nothing) makes every
        // earlier clean stamp worthless.
        builder->set_pass_stamp_floor(builder->NextPassStamp());
      } else {
        bool result = false;
        if (!subpass->Run(builder, result)) {
          builder->set_pass_stamp_floor(UINT32_MAX);  // hakuX-Boosted
          return false;
        }
        dirty |= result;
      }
    }
  } while (dirty);
  builder->set_pass_stamp_floor(UINT32_MAX);  // hakuX-Boosted
  return true;
}

void ConditionalGroupPass::AddPass(std::unique_ptr<CompilerPass> pass) {
  passes_.push_back(std::move(pass));
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
