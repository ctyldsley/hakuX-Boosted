/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// hakuX-Boosted: this file is the fork's own. The optional SPIRV-Tools pass
// over a module translated from guest microcode, chosen by the spirv_optimize
// setting. Why, what each pass is for and how to measure it:
// boosted/notes/spirv-slimming.md.

#ifndef XENIA_GPU_SPIRV_SHADER_TRANSLATOR_OPTIMIZE_H_
#define XENIA_GPU_SPIRV_SHADER_TRANSLATOR_OPTIMIZE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xe {
namespace gpu {

struct SpirvOptimizeResult {
  // False if the optimiser was not run at all (level 0, or a build without
  // SPIRV-Tools); the module is untouched and the other fields are not set.
  bool ran = false;
  // False if the optimiser refused the module; the module is untouched.
  bool succeeded = false;
  size_t words_before = 0;
  size_t words_after = 0;
  // The optimiser's own time, on the calling thread.
  double milliseconds = 0.0;
};

// Whether this build has the optimiser linked in.
bool IsSpirvOptimizerAvailable();

// Runs the pass list of the given level (the values of the spirv_optimize
// setting: 1 the short list that leaves floating-point results alone, 2
// SPIRV-Tools' performance list) over the module in place, on the calling
// thread. Uses no state shared between threads. The module is replaced only if
// the optimiser succeeds, and the old words are freed before returning, so
// two copies exist only for the length of the call.
//
// Both levels keep what the pipeline layout and the rest of the fork rely on:
// every variable with a descriptor set and binding, every entry point
// interface variable, specialization constants, execution modes, capabilities
// and the NoContraction decoration of each instruction that survives.
//
// spirv_version is the version the module was built for, in the encoding of
// its header (0x00010000 for 1.0).
SpirvOptimizeResult OptimizeTranslatedSpirv(std::vector<uint32_t>& module,
                                            int level,
                                            unsigned int spirv_version);

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SPIRV_SHADER_TRANSLATOR_OPTIMIZE_H_
