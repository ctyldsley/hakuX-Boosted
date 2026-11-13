// SPIR-V harness: builds the Xenos shader translator on a desktop, feeds it
// test microcode written here by hand (no game's shaders), and counts the
// SPIR-V words that come out, by opcode, with and without the settings
// spirv_debug_names and spirv_optimize. See README.md in this folder.
//
// The numbers say how big the module is and what it is made of. They say
// nothing about how long a driver takes to compile it: that is measured on
// the phone.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "third_party/glslang/SPIRV/doc.h"
#include "third_party/glslang/SPIRV/spirv.hpp11"
#include "xenia/base/cvar.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/spirv_shader_translator_optimize.h"
#include "xenia/gpu/xenos.h"

DECLARE_bool(precise_interpolation);

namespace {

using xe::gpu::SpirvShaderTranslator;

// -----------------------------------------------------------------------------
// A very small Xenos microcode assembler. Bit layouts are those of
// xenia/gpu/ucode.h (AluInstruction, TextureFetchInstruction,
// VertexFetchInstruction, ControlFlowExecInstruction).
// -----------------------------------------------------------------------------

struct Src {
  uint32_t index = 0;
  bool is_temp = true;  // false: a float constant.
  const char* swizzle = "xyzw";
  bool negate = false;
};
Src R(uint32_t index, const char* swizzle = "xyzw", bool negate = false) {
  return Src{index, true, swizzle, negate};
}
Src C(uint32_t index, const char* swizzle = "xyzw", bool negate = false) {
  return Src{index, false, swizzle, negate};
}

uint32_t ComponentIndex(char c) {
  switch (c) {
    case 'x':
      return 0;
    case 'y':
      return 1;
    case 'z':
      return 2;
    default:
      return 3;
  }
}

// ALU source swizzles are relative: component i reads (i + bits) & 3.
uint32_t AluSwizzle(const char* swizzle) {
  uint32_t bits = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    bits |= ((ComponentIndex(swizzle[i]) - i) & 3) << (2 * i);
  }
  return bits;
}

uint32_t WriteMask(const char* mask) {
  uint32_t bits = 0;
  for (const char* c = mask; *c; ++c) {
    if (*c != '_') {
      bits |= 1u << ComponentIndex(*c);
    }
  }
  return bits;
}

struct Instruction {
  uint32_t dwords[3];
  bool is_fetch;
};

// Vector opcodes (ucode::AluVectorOpcode).
enum : uint32_t {
  kAdd = 0,
  kMul = 1,
  kMax = 2,
  kMin = 3,
  kSge = 6,
  kFrc = 8,
  kMad = 11,
  kCndGe = 13,
  kDp4 = 15,
  kDp3 = 16,
};
// Scalar opcodes (ucode::AluScalarOpcode).
enum : uint32_t {
  kMuls = 2,
  kRsq = 22,
  kSetpGt = 29,
  kSqrt = 40,
  kRetainPrev = 50,
};

// Export register numbers (ucode::ExportRegister).
constexpr uint32_t kExportPosition = 62;
constexpr uint32_t kExportColor0 = 0;

struct AluOptions {
  bool is_export = false;
  bool clamp = false;
  bool predicated = false;
  bool predicate_condition = false;
};

// A vector operation, with the scalar half of the instruction idle.
Instruction Alu(uint32_t vector_opcode, uint32_t dest, const char* write_mask,
                Src src1, Src src2 = Src(), Src src3 = Src(),
                AluOptions options = AluOptions()) {
  Instruction instr = {};
  instr.dwords[0] = dest | (options.is_export ? 1u << 15 : 0) |
                    (WriteMask(write_mask) << 16) |
                    (options.clamp ? 1u << 24 : 0) | (kRetainPrev << 26);
  instr.dwords[1] =
      AluSwizzle(src3.swizzle) | (AluSwizzle(src2.swizzle) << 8) |
      (AluSwizzle(src1.swizzle) << 16) | (src3.negate ? 1u << 24 : 0) |
      (src2.negate ? 1u << 25 : 0) | (src1.negate ? 1u << 26 : 0) |
      (options.predicate_condition ? 1u << 27 : 0) |
      (options.predicated ? 1u << 28 : 0);
  instr.dwords[2] = src3.index | (src2.index << 8) | (src1.index << 16) |
                    (vector_opcode << 24) | (src3.is_temp ? 1u << 29 : 0) |
                    (src2.is_temp ? 1u << 30 : 0) |
                    (src1.is_temp ? 1u << 31 : 0);
  return instr;
}

// A scalar operation on one register operand, with the vector half idle (a
// max of the operand with itself, written nowhere).
Instruction AluScalar(uint32_t scalar_opcode, uint32_t dest,
                      const char* write_mask, Src src,
                      AluOptions options = AluOptions()) {
  Instruction instr = {};
  instr.dwords[0] = (dest << 8) | (WriteMask(write_mask) << 20) |
                    (options.clamp ? 1u << 25 : 0) | (scalar_opcode << 26);
  instr.dwords[1] = AluSwizzle(src.swizzle) |
                    (options.predicate_condition ? 1u << 27 : 0) |
                    (options.predicated ? 1u << 28 : 0);
  instr.dwords[2] = src.index | (kMax << 24) | (src.is_temp ? 1u << 29 : 0) |
                    (1u << 30) | (1u << 31);
  return instr;
}

// tfetch2D / tfetch3D / tfetchCube dest, src.xyz, tf<fetch_constant>, with the
// filters, anisotropy and LOD taken from the fetch constant, as most game
// shaders have them.
Instruction TextureFetch(uint32_t dest, uint32_t src, uint32_t fetch_constant,
                         xe::gpu::xenos::FetchOpDimension dimension,
                         bool use_computed_lod = true) {
  Instruction instr = {};
  instr.is_fetch = true;
  const uint32_t kSrcSwizzleXyz = 0 | (1 << 2) | (2 << 4);
  const uint32_t kDestSwizzleXyzw = 0 | (1 << 3) | (2 << 6) | (3 << 9);
  instr.dwords[0] = 1 /* kTextureFetch */ | (src << 5) | (dest << 12) |
                    (fetch_constant << 20) | (kSrcSwizzleXyz << 26);
  instr.dwords[1] = kDestSwizzleXyzw | (3u << 12) | (3u << 14) | (3u << 16) |
                    (7u << 18) | (7u << 21) | (3u << 24) | (3u << 26) |
                    (use_computed_lod ? 1u << 28 : 0);
  instr.dwords[2] = uint32_t(dimension) << 14;
  return instr;
}

// vfetch dest, src.x, vf<fetch_constant> for a float format.
Instruction VertexFetch(uint32_t dest, uint32_t src, uint32_t fetch_constant,
                        xe::gpu::xenos::VertexFormat format, uint32_t stride_dwords,
                        uint32_t offset_dwords) {
  Instruction instr = {};
  instr.is_fetch = true;
  const uint32_t kDestSwizzleXyzw = 0 | (1 << 3) | (2 << 6) | (3 << 9);
  instr.dwords[0] = 0 /* kVertexFetch */ | (src << 5) | (dest << 12) |
                    (1u << 19) | ((fetch_constant / 3) << 20) |
                    ((fetch_constant % 3) << 25);
  instr.dwords[1] = kDestSwizzleXyzw | (1u << 12) | (uint32_t(format) << 16);
  instr.dwords[2] = stride_dwords | (offset_dwords << 8);
  return instr;
}

// Control flow in front of the instructions: one exec of up to six
// instructions after another, the last an exece.
std::vector<uint32_t> Assemble(const std::vector<Instruction>& instructions) {
  struct ControlFlow {
    uint32_t dword_0;
    uint32_t dword_1;  // 16 bits used.
  };
  size_t exec_count = (instructions.size() + 5) / 6;
  size_t cf_count = (exec_count + 1) & ~size_t(1);
  uint32_t first_instruction = uint32_t(cf_count / 2);
  std::vector<ControlFlow> cfs(cf_count, ControlFlow{0, 0});
  for (size_t i = 0; i < exec_count; ++i) {
    size_t first = i * 6;
    size_t count = std::min(size_t(6), instructions.size() - first);
    uint32_t sequence = 0;
    for (size_t j = 0; j < count; ++j) {
      if (instructions[first + j].is_fetch) {
        sequence |= 1u << (2 * j);
      }
    }
    bool is_last = i + 1 == exec_count;
    cfs[i].dword_0 = (first_instruction + uint32_t(first)) |
                     (uint32_t(count) << 12) | (sequence << 16);
    cfs[i].dword_1 = (is_last ? 2u /* kExecEnd */ : 1u /* kExec */) << 12;
  }
  std::vector<uint32_t> dwords;
  for (size_t i = 0; i < cf_count; i += 2) {
    const ControlFlow& a = cfs[i];
    const ControlFlow& b = cfs[i + 1];
    dwords.push_back(a.dword_0);
    dwords.push_back((a.dword_1 & 0xFFFF) | (b.dword_0 << 16));
    dwords.push_back((b.dword_0 >> 16) | (b.dword_1 << 16));
  }
  for (const Instruction& instruction : instructions) {
    dwords.insert(dwords.end(), instruction.dwords, instruction.dwords + 3);
  }
  return dwords;
}

// -----------------------------------------------------------------------------
// The test shaders. Shapes game shaders have, not any game's shader.
// -----------------------------------------------------------------------------

struct TestShader {
  std::string name;
  xe::gpu::xenos::ShaderType type;
  std::vector<Instruction> instructions;
  uint32_t interpolator_mask;
  // How many guest instructions the series parameter added, for the "words
  // per instruction" column.
  uint32_t series_count;
};

// One of eight forms of arithmetic instruction, by turn: swizzled and masked
// the way compiled HLSL is. Registers r2 to r9 are worked on; r0 and r1 are
// inputs. Nothing is dead: instruction i writes register 2 + i % 8, the
// instruction five later reads the components it wrote, and SumRegisters at
// the end of the shader reads all eight. (A shader whose results are mostly
// overwritten unread would make dead code elimination look far better than
// it is on a game's shader.)
Instruction MixedAlu(uint32_t i) {
  uint32_t a = 2 + (i % 8);
  uint32_t b = 2 + ((i + 3) % 8);
  uint32_t c = (i % 5) ? 2 + ((i + 5) % 8) : (i % 2);
  uint32_t k = i % 24;
  switch (i % 8) {
    case 0:
      return Alu(kMul, a, "xyz_", R(b, "xyzz"), C(k, "xyzz"));
    case 1:
      return Alu(kMad, a, "xyzw", R(b), R(c, "xxxx"), R(a));
    case 2:
      return Alu(kDp4, a, "x___", R(b), R(c));
    case 3:
      return Alu(kAdd, a, "xy__", R(b, "yzyz"), R(c, "xyxy", true));
    case 4:
      return Alu(kMax, a, "___w", R(b, "xxxx"), C(k, "yyyy"));
    case 5:
      return Alu(kMul, a, "xyzw", R(b, "wzyx"), R(c));
    case 6:
      return Alu(kDp4, a, "_y__", R(b), C(k));
    default:
      return AluScalar(kRsq, a, "x___", R(b, "xxxx"));
  }
}

// r2 = r2 + ... + r9, so that every register is read.
void SumRegisters(std::vector<Instruction>& instructions) {
  for (uint32_t i = 3; i < 10; ++i) {
    instructions.push_back(Alu(kAdd, 2, "xyzw", R(2), R(i)));
  }
}

TestShader PixelMinimal() {
  TestShader shader{"ps_minimal", xe::gpu::xenos::ShaderType::kPixel, {}, 0b1, 0};
  shader.instructions.push_back(
      Alu(kMax, kExportColor0, "xyzw", R(0), R(0), Src(), {true}));
  return shader;
}

// count texture fetches from count textures (at most 16 different ones),
// each multiplied by a constant and added up. With shared_coordinates all
// read at the same interpolated coordinates (a material's maps on one UV
// set); without, each at coordinates computed from the previous result, so
// that no two fetches have anything in common.
TestShader PixelTextures(uint32_t count,
                         xe::gpu::xenos::FetchOpDimension dimension,
                         const char* name, bool shared_coordinates = true) {
  TestShader shader{std::string(name) + "_" + std::to_string(count),
                    xe::gpu::xenos::ShaderType::kPixel,
                    {},
                    0b11,
                    count};
  shader.instructions.push_back(Alu(kMax, 3, "xyzw", R(1), R(1)));
  for (uint32_t i = 0; i < count; ++i) {
    shader.instructions.push_back(
        TextureFetch(2, shared_coordinates ? 0 : 3, i % 16, dimension));
    shader.instructions.push_back(Alu(kMad, 3, "xyzw", R(2), C(i % 24), R(3)));
  }
  shader.instructions.push_back(
      Alu(kMax, kExportColor0, "xyzw", R(3), R(3), Src(), {true}));
  return shader;
}

TestShader PixelArithmetic(uint32_t count) {
  TestShader shader{"ps_alu_" + std::to_string(count),
                    xe::gpu::xenos::ShaderType::kPixel,
                    {},
                    0b11,
                    count};
  for (uint32_t i = 2; i < 10; ++i) {
    shader.instructions.push_back(Alu(kMax, i, "xyzw", R(i & 1), R(i & 1)));
  }
  for (uint32_t i = 0; i < count; ++i) {
    shader.instructions.push_back(MixedAlu(i));
  }
  SumRegisters(shader.instructions);
  shader.instructions.push_back(
      Alu(kMax, kExportColor0, "xyzw", R(2), R(2), Src(), {true}));
  return shader;
}

// As PixelArithmetic, but every fourth instruction sets the predicate and the
// three after it run only if it is set.
TestShader PixelPredicated(uint32_t count) {
  TestShader shader{"ps_alu_predicated_" + std::to_string(count),
                    xe::gpu::xenos::ShaderType::kPixel,
                    {},
                    0b11,
                    count};
  for (uint32_t i = 2; i < 10; ++i) {
    shader.instructions.push_back(Alu(kMax, i, "xyzw", R(i & 1), R(i & 1)));
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!(i % 4)) {
      shader.instructions.push_back(
          AluScalar(kSetpGt, 0, "____", R(2 + (i % 8), "xxxx")));
    } else {
      Instruction instruction = MixedAlu(i);
      instruction.dwords[1] |= (1u << 28) | (1u << 27);
      shader.instructions.push_back(instruction);
    }
  }
  SumRegisters(shader.instructions);
  shader.instructions.push_back(
      Alu(kMax, kExportColor0, "xyzw", R(2), R(2), Src(), {true}));
  return shader;
}

TestShader VertexMinimal() {
  TestShader shader{"vs_minimal", xe::gpu::xenos::ShaderType::kVertex, {}, 0, 0};
  shader.instructions.push_back(
      VertexFetch(1, 0, 95, xe::gpu::xenos::VertexFormat::k_32_32_32_FLOAT, 8, 0));
  shader.instructions.push_back(
      Alu(kMax, kExportPosition, "xyzw", R(1), R(1), Src(), {true}));
  return shader;
}

// Three vertex attributes, count arithmetic instructions, a position and two
// interpolators out.
TestShader VertexArithmetic(uint32_t count) {
  TestShader shader{"vs_alu_" + std::to_string(count),
                    xe::gpu::xenos::ShaderType::kVertex,
                    {},
                    0b11,
                    count};
  shader.instructions.push_back(
      VertexFetch(1, 0, 95, xe::gpu::xenos::VertexFormat::k_32_32_32_FLOAT, 8, 0));
  shader.instructions.push_back(
      VertexFetch(2, 0, 95, xe::gpu::xenos::VertexFormat::k_32_32_32_FLOAT, 8, 3));
  shader.instructions.push_back(
      VertexFetch(3, 0, 95, xe::gpu::xenos::VertexFormat::k_32_32_FLOAT, 8, 6));
  for (uint32_t i = 4; i < 10; ++i) {
    shader.instructions.push_back(
        Alu(kMax, i, "xyzw", R(1 + i % 3), R(1 + i % 3)));
  }
  for (uint32_t i = 0; i < count; ++i) {
    shader.instructions.push_back(MixedAlu(i));
  }
  SumRegisters(shader.instructions);
  shader.instructions.push_back(
      Alu(kMax, kExportPosition, "xyzw", R(2), R(2), Src(), {true}));
  shader.instructions.push_back(Alu(kMax, 0, "xyzw", R(3), R(3), Src(), {true}));
  shader.instructions.push_back(Alu(kMax, 1, "xyzw", R(4), R(4), Src(), {true}));
  return shader;
}

// -----------------------------------------------------------------------------
// Translating and counting.
// -----------------------------------------------------------------------------

struct Counts {
  size_t words = 0;
  std::map<uint32_t, size_t> words_by_opcode;
  std::map<uint32_t, size_t> instructions_by_opcode;
};

Counts Count(const std::vector<uint8_t>& module) {
  Counts counts;
  const uint32_t* words = reinterpret_cast<const uint32_t*>(module.data());
  size_t word_count = module.size() / sizeof(uint32_t);
  counts.words = word_count;
  for (size_t i = 5; i < word_count;) {
    uint32_t opcode = words[i] & 0xFFFF;
    uint32_t length = words[i] >> 16;
    if (!length) {
      break;
    }
    counts.words_by_opcode[opcode] += length;
    counts.instructions_by_opcode[opcode] += 1;
    i += length;
  }
  return counts;
}

// What the pipeline layout and the float behaviour rest on, read out of a
// module, to compare a module with its optimised form.
struct Essentials {
  std::vector<std::vector<uint32_t>> capabilities;
  std::vector<std::vector<uint32_t>> extensions;
  std::vector<std::vector<uint32_t>> entry_points;
  std::vector<std::vector<uint32_t>> execution_modes;
  // (set, binding) of each variable that has both.
  std::vector<std::pair<uint32_t, uint32_t>> bindings;
  // Location and BuiltIn decorations, as (decoration, value) pairs.
  std::vector<std::pair<uint32_t, uint32_t>> interface_decorations;
  size_t float_arithmetic = 0;
  size_t float_arithmetic_no_contraction = 0;
  size_t image_samples = 0;

  bool SameLayoutAs(const Essentials& other) const {
    return capabilities == other.capabilities &&
           extensions == other.extensions &&
           entry_points == other.entry_points &&
           execution_modes == other.execution_modes &&
           bindings == other.bindings &&
           interface_decorations == other.interface_decorations;
  }
};

Essentials ReadEssentials(const std::vector<uint8_t>& module) {
  Essentials essentials;
  const uint32_t* words = reinterpret_cast<const uint32_t*>(module.data());
  size_t word_count = module.size() / sizeof(uint32_t);
  std::map<uint32_t, uint32_t> sets, bindings;
  std::map<uint32_t, bool> no_contraction;
  std::vector<uint32_t> arithmetic_results;
  for (size_t i = 5; i < word_count;) {
    uint32_t opcode = words[i] & 0xFFFF;
    uint32_t length = words[i] >> 16;
    if (!length) {
      break;
    }
    std::vector<uint32_t> operands(words + i + 1, words + i + length);
    switch (spv::Op(opcode)) {
      case spv::Op::OpCapability:
        essentials.capabilities.push_back(operands);
        break;
      case spv::Op::OpExtension:
        essentials.extensions.push_back(operands);
        break;
      case spv::Op::OpEntryPoint:
        essentials.entry_points.push_back(operands);
        break;
      case spv::Op::OpExecutionMode:
        essentials.execution_modes.push_back(operands);
        break;
      case spv::Op::OpDecorate:
        switch (spv::Decoration(operands[1])) {
          case spv::Decoration::DescriptorSet:
            sets[operands[0]] = operands[2];
            break;
          case spv::Decoration::Binding:
            bindings[operands[0]] = operands[2];
            break;
          case spv::Decoration::Location:
          case spv::Decoration::BuiltIn:
            essentials.interface_decorations.emplace_back(operands[1],
                                                          operands[2]);
            break;
          case spv::Decoration::NoContraction:
            no_contraction[operands[0]] = true;
            break;
          default:
            break;
        }
        break;
      case spv::Op::OpFAdd:
      case spv::Op::OpFSub:
      case spv::Op::OpFMul:
      case spv::Op::OpFDiv:
      case spv::Op::OpFNegate:
        arithmetic_results.push_back(operands[1]);
        break;
      case spv::Op::OpImageSampleImplicitLod:
      case spv::Op::OpImageSampleExplicitLod:
      case spv::Op::OpImageFetch:
        ++essentials.image_samples;
        break;
      default:
        break;
    }
    i += length;
  }
  for (const auto& set : sets) {
    auto binding = bindings.find(set.first);
    if (binding != bindings.end()) {
      essentials.bindings.emplace_back(set.second, binding->second);
    }
  }
  // Ids are renumbered by nothing the optimiser is asked to do here, but do
  // not rely on it.
  std::sort(essentials.bindings.begin(), essentials.bindings.end());
  std::sort(essentials.interface_decorations.begin(),
            essentials.interface_decorations.end());
  std::sort(essentials.capabilities.begin(), essentials.capabilities.end());
  essentials.float_arithmetic = arithmetic_results.size();
  for (uint32_t result : arithmetic_results) {
    if (no_contraction.count(result)) {
      ++essentials.float_arithmetic_no_contraction;
    }
  }
  return essentials;
}

struct Config {
  const char* name;
  bool debug_names;
  int optimize;
};
const Config kConfigs[] = {
    {"as-today", true, 0},
    {"no-names", false, 0},
    {"opt1", false, 1},
    {"opt2", false, 2},
};

struct Options {
  std::string out_dir;
  bool histogram = false;
  bool float_controls = true;
  unsigned int spirv_version = spv::Spv_1_0;
  uint32_t depth_stencil_mode = 0;
};

std::vector<uint8_t> Translate(const TestShader& test, const Config& config,
                               const Options& options, std::string* ucode_out) {
  cvars::spirv_debug_names = config.debug_names;
  cvars::spirv_optimize = config.optimize;

  std::vector<uint32_t> ucode = Assemble(test.instructions);
  // Little-endian: the words above are already in host order.
  xe::gpu::SpirvShader shader(test.type, 0, ucode.data(), ucode.size(),
                              std::endian::little);
  xe::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  if (ucode_out) {
    *ucode_out = shader.ucode_disassembly();
  }

  // What the translator is told about the device: everything, as Turnip on a
  // recent Adreno has it, SPIR-V 1.0 as the app's spirv_version_override
  // default asks.
  SpirvShaderTranslator::Features features(true);
  features.spirv_version = options.spirv_version;
  features.fragment_shader_sample_interlock = false;
  if (!options.float_controls) {
    features.signed_zero_inf_nan_preserve_float32 = false;
    features.denorm_flush_to_zero_float32 = false;
    features.rounding_mode_rte_float32 = false;
  }
  SpirvShaderTranslator translator(features, true, true, false);

  SpirvShaderTranslator::Modification modification;
  if (test.type == xe::gpu::xenos::ShaderType::kVertex) {
    modification.value = translator.GetDefaultVertexShaderModification(
        xe::gpu::xenos::kMaxShaderTempRegisters,
        xe::gpu::Shader::HostVertexShaderType::kVertex);
    modification.vertex.dynamic_addressable_register_count = 0;
    modification.vertex.interpolator_mask = test.interpolator_mask;
  } else {
    modification.value = translator.GetDefaultPixelShaderModification(
        xe::gpu::xenos::kMaxShaderTempRegisters);
    modification.pixel.dynamic_addressable_register_count = 0;
    modification.pixel.interpolator_mask = test.interpolator_mask;
    modification.pixel.color_targets_used = 0b1;
    modification.pixel.depth_stencil_mode =
        SpirvShaderTranslator::Modification::DepthStencilMode(
            options.depth_stencil_mode);
  }
  xe::gpu::Shader::Translation* translation =
      shader.GetOrCreateTranslation(modification.value);
  if (!translator.TranslateAnalyzedShader(*translation) ||
      !translation->is_valid()) {
    std::fprintf(stderr, "%s: translation failed\n", test.name.c_str());
    std::exit(1);
  }
  return translation->translated_binary();
}

void WriteFile(const std::string& path, const void* data, size_t size) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    std::exit(1);
  }
  std::fwrite(data, 1, size, file);
  std::fclose(file);
}

void PrintHistogram(const Counts& counts, size_t rows) {
  std::vector<std::pair<size_t, uint32_t>> sorted;
  for (const auto& entry : counts.words_by_opcode) {
    sorted.emplace_back(entry.second, entry.first);
  }
  std::sort(sorted.rbegin(), sorted.rend());
  for (size_t i = 0; i < sorted.size() && i < rows; ++i) {
    uint32_t opcode = sorted[i].second;
    std::printf("      %-28s %7zu words %5.1f%%  %6zu instructions\n",
                spv::OpcodeString(int(opcode)), sorted[i].first,
                100.0 * double(sorted[i].first) / double(counts.words),
                counts.instructions_by_opcode.at(opcode));
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--out" && i + 1 < argc) {
      options.out_dir = argv[++i];
    } else if (arg == "--histogram") {
      options.histogram = true;
    } else if (arg == "--no-float-controls") {
      options.float_controls = false;
    } else if (arg == "--spirv15") {
      options.spirv_version = spv::Spv_1_5;
    } else if (arg == "--depth-stencil-mode" && i + 1 < argc) {
      options.depth_stencil_mode = uint32_t(std::atoi(argv[++i]));
    } else if (arg == "--no-precise-interpolation") {
      cvars::precise_interpolation = false;
    } else {
      std::fprintf(
          stderr,
          "usage: %s [--out DIR] [--histogram] [--no-float-controls]\n"
          "          [--spirv15] [--depth-stencil-mode N]\n"
          "          [--no-precise-interpolation]\n",
          argv[0]);
      return 2;
    }
  }

  using xe::gpu::xenos::FetchOpDimension;
  std::vector<TestShader> tests;
  tests.push_back(PixelMinimal());
  for (uint32_t count : {1u, 4u, 16u, 44u}) {
    tests.push_back(PixelTextures(count, FetchOpDimension::k2D, "ps_tex2d"));
  }
  for (uint32_t count : {16u, 44u}) {
    tests.push_back(PixelTextures(count, FetchOpDimension::k2D,
                                  "ps_tex2d_chained", false));
  }
  for (uint32_t count : {1u, 4u}) {
    tests.push_back(
        PixelTextures(count, FetchOpDimension::k3DOrStacked, "ps_tex3d"));
    tests.push_back(PixelTextures(count, FetchOpDimension::kCube, "ps_texcube"));
  }
  for (uint32_t count : {16u, 64u, 256u, 600u}) {
    tests.push_back(PixelArithmetic(count));
  }
  for (uint32_t count : {64u, 256u}) {
    tests.push_back(PixelPredicated(count));
  }
  tests.push_back(VertexMinimal());
  for (uint32_t count : {16u, 64u, 256u, 800u}) {
    tests.push_back(VertexArithmetic(count));
  }

  std::printf("%-28s %6s", "shader", "ucode");
  for (const Config& config : kConfigs) {
    std::printf(" %10s", config.name);
  }
  std::printf("   opt1 ms  opt2 ms\n");
  std::printf("%-28s %6s", "", "instr");
  for (size_t i = 0; i < std::size(kConfigs); ++i) {
    std::printf(" %10s", "bytes");
  }
  std::printf("\n");

  bool preserved = true;
  for (const TestShader& test : tests) {
    std::printf("%-28s %6zu", test.name.c_str(), test.instructions.size());
    Counts counts[std::size(kConfigs)];
    Essentials essentials[std::size(kConfigs)];
    double milliseconds[std::size(kConfigs)] = {};
    for (size_t i = 0; i < std::size(kConfigs); ++i) {
      const Config& config = kConfigs[i];
      std::string ucode_disassembly;
      std::vector<uint8_t> module =
          Translate(test, config, options, &ucode_disassembly);
      counts[i] = Count(module);
      std::printf(" %10zu", module.size());
      essentials[i] = ReadEssentials(module);
      if (i) {
        // Bindings, interface, execution modes, capabilities and extensions
        // as in the module translated today; no arithmetic without the
        // NoContraction it had; no texture sample lost.
        const Essentials& today = essentials[0];
        bool arithmetic_decorated =
            today.float_arithmetic != today.float_arithmetic_no_contraction ||
            essentials[i].float_arithmetic ==
                essentials[i].float_arithmetic_no_contraction;
        if (!essentials[i].SameLayoutAs(today) || !arithmetic_decorated ||
            essentials[i].image_samples != today.image_samples) {
          preserved = false;
          std::fprintf(stderr,
                       "%s %s: NOT PRESERVED (layout %d, NoContraction %zu of "
                       "%zu, samples %zu of %zu)\n",
                       test.name.c_str(), config.name,
                       int(essentials[i].SameLayoutAs(today)),
                       essentials[i].float_arithmetic_no_contraction,
                       essentials[i].float_arithmetic,
                       essentials[i].image_samples, today.image_samples);
        }
      }
      if (config.optimize) {
        // The optimiser's time alone, on the module as translated: the
        // translation above logs it too, but only to the log.
        cvars::spirv_optimize = 0;
        std::vector<uint8_t> unoptimized =
            Translate(test, Config{"", config.debug_names, 0}, options,
                      nullptr);
        std::vector<uint32_t> words(unoptimized.size() / sizeof(uint32_t));
        std::memcpy(words.data(), unoptimized.data(), unoptimized.size());
        milliseconds[i] = xe::gpu::OptimizeTranslatedSpirv(
                              words, config.optimize, options.spirv_version)
                              .milliseconds;
      }
      if (!options.out_dir.empty()) {
        WriteFile(options.out_dir + "/" + test.name + "." + config.name +
                      ".spv",
                  module.data(), module.size());
        if (!i) {
          WriteFile(options.out_dir + "/" + test.name + ".ucode.txt",
                    ucode_disassembly.data(), ucode_disassembly.size());
        }
      }
    }
    std::printf("  %8.2f %8.2f\n", milliseconds[2], milliseconds[3]);
    if (options.histogram) {
      for (size_t i = 0; i < std::size(kConfigs); ++i) {
        if (i == 1) {
          continue;
        }
        std::printf("    %s: %zu words\n", kConfigs[i].name, counts[i].words);
        PrintHistogram(counts[i], 14);
      }
    }
  }
  // The failure path: a module the optimiser has to refuse (its last
  // instruction claims more words than there are) must come back exactly as
  // it went in, float controls extension included.
  {
    std::vector<uint8_t> bytes =
        Translate(tests.front(), Config{"", false, 0}, options, nullptr);
    std::vector<uint32_t> broken(bytes.size() / sizeof(uint32_t));
    std::memcpy(broken.data(), bytes.data(), bytes.size());
    broken[broken.size() - 1] = (uint32_t(100) << 16) | 253;  // OpReturn
    std::vector<uint32_t> before = broken;
    xe::gpu::SpirvOptimizeResult result = xe::gpu::OptimizeTranslatedSpirv(
        broken, 1, options.spirv_version);
    bool untouched = result.ran && !result.succeeded && broken == before;
    std::printf("\na module the optimiser refuses is left as it was: %s\n",
                untouched ? "yes" : "NO");
    preserved = preserved && untouched;
  }

  std::printf(
      "\nbindings, interface variables, execution modes, capabilities and "
      "NoContraction in every optimised module: %s\n",
      preserved ? "as translated" : "CHANGED, see above");
  return preserved ? 0 : 1;
}
