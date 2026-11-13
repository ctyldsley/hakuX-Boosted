/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// hakuX-Boosted: this file is the fork's own, see the header.

#include "xenia/gpu/spirv_shader_translator_optimize.h"

#include <chrono>
#include <string>
#include <utility>

#include "xenia/base/logging.h"

// Defined by gpu/CMakeLists.txt when the SPIRV-Tools optimiser library is part
// of the build.
#ifndef XE_GPU_SPIRV_TOOLS_OPT
#define XE_GPU_SPIRV_TOOLS_OPT 0
#endif

#if XE_GPU_SPIRV_TOOLS_OPT
#include "spirv-tools/optimizer.hpp"
#endif

namespace xe {
namespace gpu {

#if XE_GPU_SPIRV_TOOLS_OPT

namespace {

spv_target_env GetTargetEnv(unsigned int spirv_version) {
  // Only the SPIR-V version matters here: the validator is not run, and the
  // passes used read the environment only to know whether the entry point
  // lists every global variable (1.4 and later) or just inputs and outputs.
  if (spirv_version >= 0x00010600) {
    return SPV_ENV_VULKAN_1_3;
  }
  if (spirv_version >= 0x00010500) {
    return SPV_ENV_VULKAN_1_2;
  }
  if (spirv_version >= 0x00010400) {
    return SPV_ENV_VULKAN_1_1_SPIRV_1_4;
  }
  if (spirv_version >= 0x00010300) {
    return SPV_ENV_VULKAN_1_1;
  }
  return SPV_ENV_VULKAN_1_0;
}

// The short list. Each pass is here for a reason measured on the translator's
// own output (boosted/notes/spirv-slimming.md); the order matters.
//
// None of them changes a floating-point result. The three that could fold
// arithmetic (constant propagation, simplification, redundancy elimination)
// go through Instruction::IsFloatingPointFoldingAllowed, which refuses any
// instruction decorated NoContraction (the translator decorates all guest
// arithmetic) and every instruction of a module that declares a float
// controls capability (the translator declares all the device has).
void RegisterSafePasses(spvtools::Optimizer& optimizer) {
  optimizer
      // The guest's registers are one function-local array of vec4. Split it
      // into a variable per register (possible only when no instruction
      // indexes it with a0 or aL), so the next passes can see each register.
      // 100 elements is the pass's own default limit and above the 64
      // registers a shader can have.
      .RegisterPass(spvtools::CreateScalarReplacementPass(100))
      // The remaining constant-index access chains into local variables (the
      // loop stacks) become loads, inserts and extracts of the whole variable.
      .RegisterPass(spvtools::CreateLocalAccessChainConvertPass())
      // Local variables that are only loaded and stored become SSA values with
      // OpPhi where control flow joins. This removes most OpLoad, OpStore and
      // OpAccessChain, the work the driver otherwise repeats itself.
      .RegisterPass(spvtools::CreateSSARewritePass())
      // Constants through the new SSA values and across branches (integer and
      // boolean in practice, see above).
      .RegisterPass(spvtools::CreateCCPPass())
      // Branches on what is now constant, and the blocks only they reached.
      .RegisterPass(spvtools::CreateDeadBranchElimPass())
      // Everything whose result is not used, including variables, types and
      // constants. true: keep all entry point interface variables.
      .RegisterPass(spvtools::CreateAggressiveDCEPass(true))
      // A block with one predecessor that has one successor is merged into it.
      .RegisterPass(spvtools::CreateBlockMergePass());
}

}  // namespace

bool IsSpirvOptimizerAvailable() { return true; }

SpirvOptimizeResult OptimizeTranslatedSpirv(std::vector<uint32_t>& module,
                                            int level,
                                            unsigned int spirv_version) {
  SpirvOptimizeResult result;
  if (level <= 0 || module.empty()) {
    return result;
  }
  result.ran = true;
  result.words_before = module.size();
  result.words_after = module.size();

  std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();

  spvtools::Optimizer optimizer(GetTargetEnv(spirv_version));
  std::string first_error;
  optimizer.SetMessageConsumer(
      [&first_error](spv_message_level_t message_level, const char* source,
                     const spv_position_t& position, const char* message) {
        if (message_level <= SPV_MSG_ERROR && first_error.empty() && message) {
          first_error = message;
        }
      });
  if (level == 1) {
    RegisterSafePasses(optimizer);
  } else {
    // true: keep all entry point interface variables.
    optimizer.RegisterPerformancePasses(true);
  }

  spvtools::OptimizerOptions options;
  // The module comes from our own translator; validating it costs about as
  // much as the passes do.
  options.set_run_validator(false);
  // The pipeline layout is computed from the translator's own list of
  // bindings, not from the module, so no variable with a descriptor set and
  // binding may disappear even if the shader never reads it.
  options.set_preserve_bindings(true);
  options.set_preserve_spec_constants(true);

  std::vector<uint32_t> optimized;
  bool succeeded =
      optimizer.Run(module.data(), module.size(), &optimized, options) &&
      !optimized.empty();
  if (succeeded) {
    // The old words are freed here, at the end of the call.
    module.swap(optimized);
    result.words_after = module.size();
  } else {
    XELOGW("SPIRV-Tools optimiser (spirv_optimize = {}) failed, the module is "
           "used as translated: {}",
           level, first_error.empty() ? "no message" : first_error.c_str());
  }
  result.succeeded = succeeded;
  result.milliseconds = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  return result;
}

#else  // XE_GPU_SPIRV_TOOLS_OPT

bool IsSpirvOptimizerAvailable() { return false; }

SpirvOptimizeResult OptimizeTranslatedSpirv(std::vector<uint32_t>& module,
                                            int level,
                                            unsigned int spirv_version) {
  return SpirvOptimizeResult();
}

#endif  // XE_GPU_SPIRV_TOOLS_OPT

}  // namespace gpu
}  // namespace xe
