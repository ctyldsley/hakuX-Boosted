// hakuX-Boosted: the fork's own file, not upstream's.
//
// A guest shader's microcode prepared for the guest shader interpreter,
// version 2 (shaders/ucode_program.glsli, which describes the layout this
// file fills, and boosted/notes/shader-first-run.md). Built once for a shader
// from its microcode alone: no register state, no translation, nothing of the
// device. So it can be made the moment a shader is first seen.

#ifndef XENIA_GPU_UCODE_PROGRAM_H_
#define XENIA_GPU_UCODE_PROGRAM_H_

#include <cstdint>
#include <vector>

#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace ucode_program {

// The interpreter's uniform block after the bool and loop constants, in
// 16-byte elements. Keep in step with shaders/ucode_program.glsli.
constexpr uint32_t kBoolLoopElements = 10;
constexpr uint32_t kHeadElements = 6;
constexpr uint32_t kCodeElements = 1008;
constexpr uint32_t kBlockBytes =
    (kBoolLoopElements + kHeadElements + kCodeElements) * 16;

// The fourth word of an ALU instruction's element.
constexpr uint32_t kAluVectorMaskShift = 0;
constexpr uint32_t kAluScalarMaskShift = 4;
constexpr uint32_t kAluConst0MaskShift = 8;
constexpr uint32_t kAluConst1MaskShift = 12;
constexpr uint32_t kAluVectorRuns = UINT32_C(1) << 16;
constexpr uint32_t kAluScalarOperandsShift = 17;
constexpr uint32_t kAluAddressed1 = UINT32_C(1) << 20;
constexpr uint32_t kAluAddressed2 = UINT32_C(1) << 21;
constexpr uint32_t kAluAddressed3 = UINT32_C(1) << 22;
constexpr uint32_t kAluVectorOperandsShift = 24;

// The fourth word of a fetch instruction's element. Bits 0:2 the kind; for a
// vertex fetch bits 8:15 the vertex fetch constant; for a texture fetch bits
// 4:7 the texture slot (Program::textures).
enum class FetchKind : uint32_t {
  kVertex = 0,
  kTexture = 1,
  kSetLod = 2,
  kSetGradientsHorizontal = 3,
  kSetGradientsVertical = 4,
  // Any other fetch opcode: the result is 0.
  kZero = 5,
};
constexpr uint32_t kFetchKindMask = 7;
constexpr uint32_t kFetchVertexConstantShift = 8;
constexpr uint32_t kFetchTextureSlotShift = 4;

// What a shader does that the interpreter does not do as the translated
// shader would. Which of these keep a shader from the interpreter is the
// caller's decision (and the log's subject).
enum Feature : uint32_t {
  // No control flow instruction ends the shader, or an exec names
  // instructions outside the microcode: not interpreted at all.
  kFeatureMalformed = UINT32_C(1) << 0,
  kFeatureLoops = UINT32_C(1) << 1,
  kFeatureCalls = UINT32_C(1) << 2,
  // Registers or float constants addressed by aL or a0.
  kFeatureRelativeAddressing = UINT32_C(1) << 3,
  // Texture fetches (their count and kinds are in Program::textures).
  kFeatureTextureFetch = UINT32_C(1) << 4,
  // getBCF, getCompTexLOD, getGradients, getWeights: the result is 0.
  kFeatureFetchQuery = UINT32_C(1) << 5,
  // setTexLOD, setGradientH, setGradientV, or a fetch that uses them.
  kFeatureExplicitLod = UINT32_C(1) << 6,
  // Memory export: the exports are dropped.
  kFeatureMemoryExport = UINT32_C(1) << 7,
  // More different textures than kMaxTextures: the extra fetches read 0.
  kFeatureTooManyTextures = UINT32_C(1) << 8,
  // A 3D or stacked texture is fetched.
  kFeatureTexture3D = UINT32_C(1) << 9,
  kFeatureTextureCube = UINT32_C(1) << 10,
  kFeatureTexture1D = UINT32_C(1) << 11,
  // The pixel shader writes depth.
  kFeatureDepthOutput = UINT32_C(1) << 12,
  // The pixel shader can kill pixels.
  kFeatureKill = UINT32_C(1) << 13,
  // Vertex shader: writes the point size, or the kill flag.
  kFeaturePointSize = UINT32_C(1) << 15,
  kFeatureVertexKill = UINT32_C(1) << 16,
};

constexpr uint32_t kMaxTextures = 8;

struct Texture {
  // Which fetch constant and which kind of texture a slot samples.
  uint8_t fetch_constant;
  xenos::FetchOpDimension dimension;
  // The filters of the first fetch that used the slot (kUseFetchConst where
  // the instruction leaves them to the fetch constant), for the sampler.
  xenos::TextureFilter mag_filter;
  xenos::TextureFilter min_filter;
  xenos::TextureFilter mip_filter;
  xenos::AnisoFilter aniso_filter;
};

struct Program {
  // Four words an element. Empty if the shader is kFeatureMalformed.
  std::vector<uint32_t> words;
  uint32_t features = 0;
  // The slots the texture fetches were given, at most kMaxTextures.
  std::vector<Texture> textures;
  uint32_t instruction_count = 0;
  uint32_t texture_fetch_count = 0;

  uint32_t element_count() const { return uint32_t(words.size() / 4); }
};

// ucode_dwords in host byte order, as Shader::ucode_dwords() has them.
void Build(xenos::ShaderType shader_type, const uint32_t* ucode_dwords,
           uint32_t ucode_dword_count, Program& program_out);

// The features as words, for a log line ("loops, texture fetches").
const char* GetFeatureName(uint32_t feature_bit_index);

}  // namespace ucode_program
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_UCODE_PROGRAM_H_
