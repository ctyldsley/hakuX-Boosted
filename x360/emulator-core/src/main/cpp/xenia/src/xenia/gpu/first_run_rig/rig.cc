// hakuX-Boosted: the fork's own file, not part of the app (README.md here).
//
// The first-run rig. It checks the guest shader interpreter, version 2
// (shaders/ucode_program*.glsl*), against the emulator's own shader
// translator: for a test shader written here it
//   - translates the microcode with SpirvShaderTranslator, as the app does,
//   - prepares the same microcode for the interpreter (ucode_program.cc),
//   - draws the same vertices (or pixels) once through each on a Vulkan
//     device (Mesa's lavapipe: no GPU is needed),
//   - and compares what the two wrote.
// The translated shader is what the real pipeline will draw, so a difference
// is a placeholder that draws something else.
//
// It says nothing about a phone's driver, and nothing about speed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "third_party/glslang/SPIRV/spirv.hpp11"
#include "xenia/base/cvar.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/first_run_rig/rig_asm.h"
#include "xenia/gpu/first_run_rig/rig_vulkan.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/ucode_program.h"
#include "xenia/gpu/xenos.h"

DECLARE_bool(precise_interpolation);
DECLARE_bool(spirv_tfetch_vector_signs);

namespace rig {

using xe::gpu::SpirvShaderTranslator;
namespace ucode_program = xe::gpu::ucode_program;

// -----------------------------------------------------------------------------
// What both shaders are given.
// -----------------------------------------------------------------------------

constexpr uint32_t kVertexCount = 48;
constexpr uint32_t kVertexStrideWords = 16;
constexpr uint32_t kVertexBufferAddressWords = 0x1000;
constexpr uint32_t kSharedMemoryWords = 0x4000;
constexpr uint32_t kVertexFetchConstant = 95;
// Position and sixteen interpolators.
constexpr uint32_t kCaptureVectors = 17;

struct Environment {
  // 512 float constants: the vertex half, then the pixel half.
  float float_constants[512][4];
  uint32_t bool_constants[8];
  uint32_t loop_constants[32];
  uint32_t fetch_constants[32 * 6];
  SpirvShaderTranslator::SystemConstants system_constants;
  // Guest memory in host byte order (the fetch constant says the guest's is
  // 8-in-32 swapped; the rig swaps when it uploads).
  std::vector<uint32_t> shared_memory;
};

uint32_t Swap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

void FillEnvironment(Environment& environment, std::mt19937& rng) {
  std::uniform_real_distribution<float> real(-2.0f, 2.0f);
  std::uniform_int_distribution<int> small_int(-3, 3);
  std::uniform_int_distribution<int> percent(0, 99);
  for (auto& constant : environment.float_constants) {
    for (float& component : constant) {
      int roll = percent(rng);
      if (roll < 10) {
        component = 0.0f;
      } else if (roll < 25) {
        component = float(small_int(rng));
      } else {
        component = real(rng);
      }
    }
  }
  for (uint32_t& word : environment.bool_constants) {
    word = rng();
  }
  for (uint32_t i = 0; i < 32; ++i) {
    // Count, start and step of a loop: small, so that aL stays inside the
    // registers and constants the tests address with it.
    uint32_t count = uint32_t(percent(rng) % 5);
    uint32_t start = uint32_t(percent(rng) % 3);
    int32_t step = percent(rng) % 3 - 1;
    environment.loop_constants[i] =
        count | (start << 8) | ((uint32_t(step) & 0xFF) << 16);
  }
  std::memset(environment.fetch_constants, 0,
              sizeof(environment.fetch_constants));
  // Vertex fetch constant 95: two words, the address and the size in words
  // (each shifted left by two), type and endianness in the low bits.
  uint32_t* vertex_fetch = environment.fetch_constants + kVertexFetchConstant * 2;
  vertex_fetch[0] = (kVertexBufferAddressWords << 2) | 3;
  vertex_fetch[1] = ((kVertexCount * kVertexStrideWords) << 2) | 2;

  environment.shared_memory.assign(kSharedMemoryWords, 0);
  for (uint32_t i = 0; i < kVertexCount * kVertexStrideWords; ++i) {
    uint32_t word;
    uint32_t column = i % kVertexStrideWords;
    if (column < 12) {
      // Floats.
      float value = (percent(rng) < 12) ? 0.0f : real(rng);
      if (column == 3) {
        // Keeps W of the first attribute away from zero: it is the position's
        // W in most tests.
        value = 1.0f + std::abs(value);
      }
      std::memcpy(&word, &value, sizeof(word));
    } else {
      // Packed integer formats.
      word = rng();
    }
    environment.shared_memory[kVertexBufferAddressWords + i] = word;
  }

  SpirvShaderTranslator::SystemConstants& system = environment.system_constants;
  std::memset(&system, 0, sizeof(system));
  system.flags = 0;
  system.vertex_index_count = kVertexCount;
  system.vertex_index_endian = xe::gpu::xenos::Endian::kNone;
  system.vertex_base_index = 0;
  system.ndc_scale[0] = 0.5f;
  system.ndc_scale[1] = -0.25f;
  system.ndc_scale[2] = 1.0f;
  system.ndc_offset[0] = 0.125f;
  system.ndc_offset[1] = -0.0625f;
  system.ndc_offset[2] = 0.0f;
  system.point_vertex_diameter_min = 1.0f;
  system.point_vertex_diameter_max = 256.0f;
  system.point_constant_diameter[0] = 4.0f;
  system.point_constant_diameter[1] = 6.0f;
  system.point_screen_diameter_to_ndc_radius[0] = 1.0f / 640.0f;
  system.point_screen_diameter_to_ndc_radius[1] = 1.0f / 360.0f;
  for (float& bias : system.color_exp_bias) {
    bias = 1.0f;
  }
}

// -----------------------------------------------------------------------------
// Test shaders.
// -----------------------------------------------------------------------------

struct Test {
  std::string name;
  xe::gpu::xenos::ShaderType type = xe::gpu::xenos::ShaderType::kVertex;
  std::vector<uint32_t> ucode;
  // Changes to the environment for this test.
  std::function<void(Environment&)> prepare;
  // Pixel tests: the register given the generated parameter, or -1.
  int param_gen_register = -1;
  // Largest difference allowed, relative to the larger magnitude (and
  // absolute below 1).
  float tolerance = 2e-5f;
};

// The start every vertex test has: r1, r2, r3 from three float attributes,
// r4 from packed ones.
void VertexPrologue(Asm& assembler) {
  using xe::gpu::xenos::VertexFormat;
  assembler.Exec(std::vector<Ins>{
      VertexFetch(1, 0, kVertexFetchConstant,
                  uint32_t(VertexFormat::k_32_32_32_32_FLOAT),
                  kVertexStrideWords, 0),
      VertexFetch(2, 0, kVertexFetchConstant,
                  uint32_t(VertexFormat::k_32_32_32_32_FLOAT),
                  kVertexStrideWords, 4, true),
      VertexFetch(3, 0, kVertexFetchConstant,
                  uint32_t(VertexFormat::k_32_32_32_32_FLOAT),
                  kVertexStrideWords, 8, true),
      VertexFetch(4, 0, kVertexFetchConstant, uint32_t(VertexFormat::k_8_8_8_8),
                  kVertexStrideWords, 12, true),
  });
}

// The end every vertex test has: the position from r1, registers 2 to 13 in
// interpolators 0 to 11.
void VertexEpilogue(Asm& assembler) {
  std::vector<Alu> exports;
  exports.push_back(Export(62, R(1)));
  for (uint32_t i = 2; i <= 13; ++i) {
    exports.push_back(Export(i - 2, R(i)));
  }
  assembler.Exec(exports);
  assembler.End();
}

Test MakeVertexTest(const std::string& name,
                    const std::function<void(Asm&)>& body) {
  Test test;
  test.name = name;
  Asm assembler;
  VertexPrologue(assembler);
  body(assembler);
  VertexEpilogue(assembler);
  test.ucode = assembler.Assemble();
  return test;
}

const char* const kSwizzles[] = {"xyzw", "yzwx", "wzyx", "xxxx", "zzzz",
                                 "wwww", "xyxy", "yxwz", "zwxy", "xzyw"};
const char* const kMasks[] = {"xyzw", "xyzw", "xyz_", "xy__", "x___",
                              "_y__", "__z_", "___w", "x_z_", "_yzw"};

template <typename T, size_t N>
const T& Pick(const T (&array)[N], std::mt19937& rng) {
  return array[rng() % N];
}

Src RandomSource(std::mt19937& rng, uint32_t register_bound) {
  if (rng() % 4 == 0) {
    return C(rng() % 48, Pick(kSwizzles, rng), rng() % 5 == 0);
  }
  return R(1 + rng() % (register_bound - 1), Pick(kSwizzles, rng),
           rng() % 5 == 0, rng() % 7 == 0);
}

// Every vector operation that only computes (no predicate, kill or address
// register), and every scalar one likewise.
const uint32_t kPlainVectorOps[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
                                    11, 12, 13, 14, 15, 16, 17, 18, 19, 28};
const uint32_t kPlainScalarOps[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,
                                    9,  10, 11, 12, 13, 14, 15, 16, 17,
                                    18, 19, 20, 21, 22, 25, 26, 40, 50};

Alu RandomAlu(std::mt19937& rng, uint32_t register_bound,
              bool allow_predicate_ops) {
  Alu alu;
  alu.vector_op = Pick(kPlainVectorOps, rng);
  alu.scalar_op = Pick(kPlainScalarOps, rng);
  if (allow_predicate_ops && rng() % 6 == 0) {
    // setp_eq, setp_ne, setp_gt, setp_ge, setp_inv, setp_pop, setp_clr,
    // setp_rstr.
    alu.scalar_op = 27 + rng() % 8;
  }
  alu.vector_dest = 2 + rng() % (register_bound - 2);
  alu.scalar_dest = 2 + rng() % (register_bound - 2);
  alu.vector_mask = rng() % 5 ? WriteMask(Pick(kMasks, rng)) : 0;
  alu.scalar_mask = rng() % 3 ? 0 : WriteMask(Pick(kMasks, rng));
  alu.vector_clamp = rng() % 6 == 0;
  alu.scalar_clamp = rng() % 6 == 0;
  alu.abs_constants = rng() % 8 == 0;
  alu.src1 = RandomSource(rng, register_bound);
  alu.src2 = RandomSource(rng, register_bound);
  alu.src3 = RandomSource(rng, register_bound);
  return alu;
}

void AddVertexTests(std::vector<Test>& tests) {
  // Each vector operation on its own, with operands that have zeros, equal
  // and unequal components.
  for (uint32_t op : kPlainVectorOps) {
    tests.push_back(MakeVertexTest(
        "vs_vector_op_" + std::to_string(op), [op](Asm& assembler) {
          std::vector<Alu> alus = {
              Vec(op, 5, "xyzw", R(2), R(3), R(1)),
              Vec(op, 6, "xyzw", R(2, "wzyx"), C(7), C(9)),
              Vec(op, 7, "xyzw", R(4), R(4, "yxwz"), R(2, "xxxx", true)),
              Vec(op, 8, "xyzw", C(3), R(3, "xyzw", false, true), R(4)),
              Vec(op, 9, "x_z_", R(1), R(2, "zzzz"), C(11, "wzyx", true)),
          };
          assembler.Exec(alus);
        }));
  }
  // Each scalar operation: operands in W and X of the swizzle.
  for (uint32_t op : kPlainScalarOps) {
    tests.push_back(MakeVertexTest(
        "vs_scalar_op_" + std::to_string(op), [op](Asm& assembler) {
          assembler.Exec(std::vector<Alu>{
              Scal(op, 5, "xyzw", R(2)),
              Scal(op, 6, "xyzw", R(3, "wzyx")),
              Scal(op, 7, "x__w", R(1, "xxxx", true)),
              Scal(op, 8, "xyzw", C(5, "yzwx")),
              Scal(op, 9, "xyzw", R(2, "zzzz", false, true)),
              // The previous scalar carried between instructions.
              Scal(1, 10, "xyzw", R(4)),
              Scal(3, 11, "xyzw", R(3)),
              Scal(26, 12, "xyzw", R(2)),
          });
        }));
  }
  // The same with infinities, NaN and signed zeros among the operands (c60 to
  // c63, set by the test's preparation): what a max, a compare or the zero
  // rule of a multiply makes of them.
  auto special_values = [](Environment& environment) {
    const float kInf = INFINITY, kNan = NAN;
    const float kValues[4][4] = {{kInf, -kInf, kNan, 0.0f},
                                 {kNan, 1.0f, -0.0f, kInf},
                                 {0.0f, kNan, -kInf, -1.0f},
                                 {-0.0f, kInf, 2.0f, kNan}};
    std::memcpy(environment.float_constants[60], kValues, sizeof(kValues));
  };
  for (uint32_t op : kPlainVectorOps) {
    tests.push_back(MakeVertexTest(
        "vs_vector_special_" + std::to_string(op), [op](Asm& assembler) {
          assembler.Exec(std::vector<Alu>{
              Vec(op, 5, "xyzw", C(60), C(61), C(62)),
              Vec(op, 6, "xyzw", C(61), C(60), C(63)),
              Vec(op, 7, "xyzw", R(2), C(62), C(60)),
              Vec(op, 8, "xyzw", C(63), R(3), C(61)),
              Vec(op, 9, "xyzw", C(62, "wzyx"), C(63, "yxwz"), R(2)),
              Vec(op, 10, "xyzw", C(60), C(60), C(60, "zzzz")),
              Vec(op, 11, "xyzw", C(63, "xyzw", true), C(61, "zzzz"), C(62)),
          });
        }));
    tests.back().prepare = special_values;
  }
  for (uint32_t op : kPlainScalarOps) {
    tests.push_back(MakeVertexTest(
        "vs_scalar_special_" + std::to_string(op), [op](Asm& assembler) {
          std::vector<Alu> alus;
          const char* const kPairs[] = {"xyzw", "yzwx", "zwxy", "wxyz",
                                        "xxxx", "zzzz"};
          for (uint32_t i = 0; i < 4; ++i) {
            // The previous scalar is a special value too for the *_prev ones.
            alus.push_back(Scal(5, 0, "____", C(60 + i, "xxxx")));
            for (uint32_t j = 0; j < 2; ++j) {
              alus.push_back(
                  Scal(op, 5 + i * 2 + j, "xyzw", C(60 + i, kPairs[i + j])));
            }
          }
          assembler.Exec(alus);
        }));
    tests.back().prepare = special_values;
  }
  // mulsc, addsc, subsc: a constant and a register.
  for (uint32_t op = 42; op <= 47; ++op) {
    tests.push_back(MakeVertexTest(
        "vs_scalar_const_op_" + std::to_string(op), [op](Asm& assembler) {
          std::vector<Alu> alus;
          for (uint32_t i = 0; i < 4; ++i) {
            Alu alu;
            alu.scalar_op = op;
            alu.scalar_dest = 5 + i;
            alu.scalar_mask = 0b1111;
            // The register is (scalar_op & 1) | (src3_sel << 1) |
            // (src3_swiz & 0x3C); the constant is the source field.
            alu.src3.field = 3 + i;
            alu.src3.is_temp = (i & 1) != 0;
            alu.src3.swizzle = (i * 0x41) & 0xFF;
            alu.src3.negate = i == 2;
            alu.abs_constants = i == 3;
            alus.push_back(alu);
          }
          assembler.Exec(alus);
        }));
  }
  // Both halves of an instruction at once, clamps, and exports that mix the
  // vector result, the scalar result and the constants 0 and 1.
  tests.push_back(MakeVertexTest("vs_both_halves", [](Asm& assembler) {
    Alu both = Vec(11, 5, "xy__", R(2), R(3), R(4));
    both.scalar_op = 2;
    both.scalar_dest = 5;
    both.scalar_mask = WriteMask("__zw");
    Alu clamped = Vec(0, 6, "xyzw", R(2), R(3));
    clamped.vector_clamp = true;
    clamped.scalar_op = 25;
    clamped.scalar_dest = 7;
    clamped.scalar_mask = 0b1111;
    clamped.scalar_clamp = true;
    clamped.src3 = R(1, "yxzw");
    assembler.Exec(std::vector<Alu>{both, clamped});
    // oPos.xy from the vector half, .z from the scalar half, .w = 1; an
    // interpolator with zeros in what neither writes.
    Alu position = Vec(0, 62, "xy_w", R(2), R(3));
    position.is_export = true;
    position.scalar_op = 0;
    position.scalar_mask = WriteMask("__zw");
    position.src3 = R(1);
    Alu zeros = Vec(1, 14, "x___", R(2), R(4));
    zeros.is_export = true;
    zeros.scalar_op = 5;
    zeros.scalar_mask = WriteMask("_y__");
    zeros.scalar_dest_relative = true;
    zeros.src3 = R(3);
    assembler.Exec(std::vector<Alu>{position, zeros});
  }));
  // Every vertex format.
  {
    using xe::gpu::xenos::VertexFormat;
    const VertexFormat kFormats[] = {
        VertexFormat::k_8_8_8_8,          VertexFormat::k_2_10_10_10,
        VertexFormat::k_10_11_11,         VertexFormat::k_11_11_10,
        VertexFormat::k_16_16,            VertexFormat::k_16_16_16_16,
        VertexFormat::k_16_16_FLOAT,      VertexFormat::k_16_16_16_16_FLOAT,
        VertexFormat::k_32,               VertexFormat::k_32_32,
        VertexFormat::k_32_32_32_32,      VertexFormat::k_32_FLOAT,
        VertexFormat::k_32_32_FLOAT,      VertexFormat::k_32_32_32_32_FLOAT,
        VertexFormat::k_32_32_32_FLOAT,
    };
    for (VertexFormat format : kFormats) {
      for (uint32_t variant = 0; variant < 6; ++variant) {
        bool is_signed = (variant & 1) != 0;
        bool is_integer = variant == 2 || variant == 3;
        bool no_zero = variant == 5;
        if (variant == 4) {
          is_signed = true;
        }
        Test test = MakeVertexTest(
            "vs_format_" + std::to_string(uint32_t(format)) + "_" +
                std::to_string(variant),
            [=](Asm& assembler) {
              assembler.Exec(std::vector<Ins>{
                  VertexFetch(5, 0, kVertexFetchConstant, uint32_t(format),
                              kVertexStrideWords, 12, false, is_signed,
                              is_integer, variant == 4 ? 3 : 0, no_zero),
                  // Across the end of the buffer for the last vertices.
                  VertexFetch(6, 0, kVertexFetchConstant, uint32_t(format),
                              kVertexStrideWords, 14 + 16 * 2, true, is_signed,
                              is_integer, variant == 4 ? -2 : 0, no_zero),
              });
            });
        // 32-bit integers as floats: allow the last bit.
        test.tolerance = 1e-4f;
        tests.push_back(test);
      }
    }
  }
  // Execs conditional on bool constants, on the predicate, and instructions
  // predicated one by one.
  tests.push_back(MakeVertexTest("vs_conditional_execs", [](Asm& assembler) {
    for (uint32_t i = 0; i < 6; ++i) {
      assembler.Exec(std::vector<Alu>{Vec(0, 5 + i, "xyzw", R(5 + i), C(i + 1))},
                     Asm::Condition::kBool, 3 + i * 37, (i & 1) != 0);
    }
    assembler.Exec(std::vector<Alu>{Scal(29, 0, "____", R(2, "xxxx"))});
    assembler.Exec(std::vector<Alu>{Vec(0, 11, "xyzw", R(3), C(20))},
                   Asm::Condition::kPredicate, 0, true);
    assembler.Exec(std::vector<Alu>{Vec(0, 12, "xyzw", R(3), C(21))},
                   Asm::Condition::kPredicate, 0, false);
    Alu when_set = Vec(1, 13, "xyzw", R(2), C(22));
    when_set.predicated = true;
    when_set.predicate_condition = true;
    Alu when_clear = Vec(1, 13, "xyzw", R(4), C(23));
    when_clear.predicated = true;
    when_clear.predicate_condition = false;
    Ins fetch_when_set = VertexFetch(
        10, 0, kVertexFetchConstant,
        uint32_t(xe::gpu::xenos::VertexFormat::k_32_32_32_32_FLOAT),
        kVertexStrideWords, 4);
    fetch_when_set.dwords[1] |= 1u << 31;
    fetch_when_set.dwords[2] |= 1u << 31;
    assembler.Exec(
        std::vector<Ins>{when_set.Encode(), when_clear.Encode(), fetch_when_set});
  }));
  // The predicate stack operations.
  tests.push_back(MakeVertexTest("vs_predicate_ops", [](Asm& assembler) {
    std::vector<Alu> alus;
    for (uint32_t op = 20; op <= 23; ++op) {
      alus.push_back(Vec(op, 5 + (op - 20), "xyzw", R(2), R(3)));
      Alu use = Vec(0, 9, "xyzw", R(9), C(op));
      use.predicated = true;
      use.predicate_condition = true;
      alus.push_back(use);
    }
    for (uint32_t op = 27; op <= 34; ++op) {
      alus.push_back(Scal(op, 10, "xyzw", R(2 + (op & 1), "wzyx")));
      Alu use = Vec(0, 11, "xyzw", R(11), C(op));
      use.predicated = true;
      use.predicate_condition = (op & 1) != 0;
      alus.push_back(use);
      alus.push_back(Scal(1, 12, "xyzw", R(12)));
    }
    assembler.Exec(alus);
  }));
  // Jumps.
  tests.push_back(MakeVertexTest("vs_jumps", [](Asm& assembler) {
    int skip_1 = assembler.NewLabel();
    int skip_2 = assembler.NewLabel();
    int skip_3 = assembler.NewLabel();
    assembler.Jump(skip_1, Asm::Condition::kBool, 40, true);
    assembler.Exec(std::vector<Alu>{Vec(0, 5, "xyzw", R(2), C(1))});
    assembler.Bind(skip_1);
    assembler.Exec(std::vector<Alu>{Scal(30, 0, "____", R(3, "yyyy"))});
    assembler.Jump(skip_2, Asm::Condition::kPredicate, 0, false);
    assembler.Exec(std::vector<Alu>{Vec(0, 6, "xyzw", R(3), C(2))});
    assembler.Bind(skip_2);
    assembler.Jump(skip_3);
    assembler.Exec(std::vector<Alu>{Vec(0, 7, "xyzw", R(4), C(3))});
    assembler.Bind(skip_3);
    assembler.Exec(std::vector<Alu>{Vec(0, 8, "xyzw", R(4), C(4))});
  }));
  // The address register and constants addressed by it.
  tests.push_back(MakeVertexTest("vs_a0_relative", [](Asm& assembler) {
    Alu set_a0 = Vec(29, 5, "xyzw", C(40), R(2));
    Alu use = Vec(0, 6, "xyzw", R(3), C(8));
    use.const_a0_relative = true;
    use.const_0_addressed = true;
    Alu set_a0_scalar = Scal(23, 7, "xyzw", C(41, "wzyx"));
    Alu use_2 = Vec(11, 8, "xyzw", C(2), C(12), C(13));
    use_2.const_a0_relative = true;
    use_2.const_0_addressed = false;
    use_2.const_1_addressed = true;
    Alu set_a0_floor = Scal(24, 9, "xyzw", R(4, "wwww"));
    Alu use_3 = Scal(0, 10, "xyzw", C(6, "wzyx"));
    use_3.const_a0_relative = true;
    use_3.const_0_addressed = true;
    use_3.src1 = R(1);
    use_3.src2 = R(1);
    assembler.Exec(
        std::vector<Alu>{set_a0, use, set_a0_scalar, use_2, set_a0_floor, use_3});
  }));
  {
    Test& test = tests.back();
    test.prepare = [](Environment& environment) {
      // c40.w and c41.w: the offsets a0 gets (rounded).
      environment.float_constants[40][3] = 3.4f;
      environment.float_constants[41][3] = 6.6f;
    };
  }
  // Loops: the count, aL as it starts and steps, registers and constants
  // addressed by aL, a loop that does not run, loops inside loops, and a
  // break on the predicate.
  tests.push_back(MakeVertexTest("vs_loops", [](Asm& assembler) {
    auto body = [](uint32_t dest, uint32_t constant) {
      Alu add = Vec(0, dest, "xyzw", R(dest), C(constant));
      add.const_0_addressed = true;
      Alu relative = Vec(0, 8, "xyzw", R(2, "xyzw", false, false, true), R(8));
      Alu store = Vec(0, 10, "x___", R(10), C(30));
      store.vector_dest_relative = true;
      return std::vector<Alu>{add, relative, store};
    };
    for (uint32_t loop = 0; loop < 3; ++loop) {
      int skip = assembler.NewLabel();
      int top = assembler.NewLabel();
      assembler.LoopStart(loop, skip);
      assembler.Bind(top);
      assembler.Exec(body(5 + loop, 16));
      assembler.LoopEnd(loop, top);
      assembler.Bind(skip);
    }
    // Nested.
    int outer_skip = assembler.NewLabel();
    int outer_top = assembler.NewLabel();
    int inner_skip = assembler.NewLabel();
    int inner_top = assembler.NewLabel();
    assembler.LoopStart(3, outer_skip);
    assembler.Bind(outer_top);
    assembler.Exec(body(9, 18));
    assembler.LoopStart(4, inner_skip);
    assembler.Bind(inner_top);
    assembler.Exec(body(9, 20));
    assembler.LoopEnd(4, inner_top);
    assembler.Bind(inner_skip);
    assembler.LoopEnd(3, outer_top);
    assembler.Bind(outer_skip);
    // A break when the predicate is set.
    int break_skip = assembler.NewLabel();
    int break_top = assembler.NewLabel();
    assembler.LoopStart(5, break_skip);
    assembler.Bind(break_top);
    assembler.Exec(std::vector<Alu>{Vec(0, 13, "xyzw", R(13), C(31)),
                                    Scal(29, 0, "____", R(13, "xxxx"))});
    assembler.LoopEnd(5, break_top, true, true);
    assembler.Bind(break_skip);
  }));
  {
    Test& test = tests.back();
    test.prepare = [](Environment& environment) {
      // count, start, step.
      environment.loop_constants[0] = 3 | (1 << 8) | (1 << 16);
      environment.loop_constants[1] = 0 | (2 << 8) | (1 << 16);
      environment.loop_constants[2] = 4 | (3 << 8) | (0xFF << 16);
      environment.loop_constants[3] = 2 | (0 << 8) | (2 << 16);
      environment.loop_constants[4] = 3 | (1 << 8) | (1 << 16);
      environment.loop_constants[5] = 6 | (0 << 8) | (1 << 16);
      for (uint32_t i = 30; i < 32; ++i) {
        for (uint32_t j = 0; j < 4; ++j) {
          environment.float_constants[i][j] = 0.75f;
        }
      }
    };
  }
  // The point size and the kill flag.
  tests.push_back(MakeVertexTest("vs_point_size_kill", [](Asm& assembler) {
    Alu point_size = Vec(1, 63, "x___", R(2), C(50));
    point_size.is_export = true;
    assembler.Exec(std::vector<Alu>{point_size});
  }));
  tests.push_back(MakeVertexTest("vs_kill_or", [](Asm& assembler) {
    Alu kill = Vec(5, 63, "__z_", R(2, "xxxx"), C(51));
    kill.is_export = true;
    assembler.Exec(std::vector<Alu>{kill});
  }));
  // What is done to the position after the guest code: the three ways the
  // guest can have divided by W, and a base added to the vertex index.
  for (uint32_t variant = 1; variant < 8; ++variant) {
    tests.push_back(MakeVertexTest(
        "vs_position_flags_" + std::to_string(variant), [](Asm& assembler) {
          assembler.Exec(std::vector<Alu>{Vec(1, 5, "xyzw", R(2), R(3))});
        }));
    tests.back().prepare = [variant](Environment& environment) {
      environment.system_constants.flags =
          ((variant & 1) ? SpirvShaderTranslator::kSysFlag_XYDividedByW : 0) |
          ((variant & 2) ? SpirvShaderTranslator::kSysFlag_ZDividedByW : 0) |
          ((variant & 4) ? SpirvShaderTranslator::kSysFlag_WNotReciprocal : 0);
      environment.system_constants.vertex_base_index = int32_t(variant) - 3;
    };
  }
  // Random shaders: short ones in number, so that a difference of one unit
  // in the last place is not blown up before it reaches an output, and a few
  // long ones.
  std::mt19937 rng(20261113);
  for (uint32_t i = 0; i < 160; ++i) {
    uint32_t length = i < 120 ? 6 + i % 7 : 40 + i;
    tests.push_back(MakeVertexTest(
        "vs_random_" + std::to_string(i), [&rng, length](Asm& assembler) {
          std::vector<Alu> alus;
          for (uint32_t j = 0; j < length; ++j) {
            Alu alu = RandomAlu(rng, 14, true);
            if (rng() % 4 == 0) {
              alu.predicated = true;
              alu.predicate_condition = (rng() & 1) != 0;
            }
            alus.push_back(alu);
          }
          assembler.Exec(alus);
        }));
    // Long chains of exp, log and reciprocals drift.
    tests.back().tolerance = i < 120 ? 1e-4f : 1e-2f;
  }
}

// -----------------------------------------------------------------------------
// Pixel test shaders. Registers 0 to 15 start as the interpolators.
// -----------------------------------------------------------------------------

Test MakePixelTest(const std::string& name,
                   const std::function<void(Asm&)>& body,
                   std::function<void(Environment&)> prepare = nullptr) {
  Test test;
  test.name = name;
  test.type = xe::gpu::xenos::ShaderType::kPixel;
  Asm assembler;
  body(assembler);
  assembler.End();
  test.ucode = assembler.Assemble();
  test.prepare = std::move(prepare);
  return test;
}

// A fetch instruction that only sets state: setTexLOD (24), setGradientH
// (25), setGradientV (26).
Ins SetFetchState(uint32_t opcode, uint32_t src, const char* swizzle) {
  Ins ins;
  ins.is_fetch = true;
  ins.dwords[0] = opcode | (src << 5) |
                  ((ComponentIndex(swizzle[0]) |
                    (ComponentIndex(swizzle[1]) << 2) |
                    (ComponentIndex(swizzle[2]) << 4))
                   << 26);
  return ins;
}

void SetSigns(Environment& environment, uint32_t fetch_constant,
              uint32_t signs) {
  uint32_t& word =
      environment.system_constants.texture_swizzled_signs[fetch_constant >> 2];
  uint32_t shift = 8 * (fetch_constant & 3);
  word = (word & ~(UINT32_C(0xFF) << shift)) | (signs << shift);
}

void AddPixelTests(std::vector<Test>& tests) {
  auto export_colors = [](Asm& assembler, uint32_t first_register) {
    assembler.Exec(std::vector<Alu>{Export(0, R(first_register)),
                                    Export(1, R(first_register + 1)),
                                    Export(2, R(first_register + 2)),
                                    Export(3, R(first_register + 3))});
  };
  tests.push_back(MakePixelTest(
      "ps_passthrough", [&](Asm& assembler) { export_colors(assembler, 0); }));
  tests.push_back(MakePixelTest("ps_one_color_masked", [](Asm& assembler) {
    assembler.Exec(std::vector<Alu>{Export(0, R(1), "x_z_"),
                                    Export(2, R(2, "wzyx"), "_y_w")});
  }));

  // Arithmetic, as the vertex tests have it, on the interpolators.
  {
    std::mt19937 rng(77);
    for (uint32_t i = 0; i < 60; ++i) {
      std::vector<Alu> alus;
      uint32_t count = 2 + rng() % 9;
      for (uint32_t j = 0; j < count; ++j) {
        alus.push_back(RandomAlu(rng, 16, i >= 30));
        if (i >= 30 && rng() % 4 == 0) {
          alus.back().predicated = true;
          alus.back().predicate_condition = rng() % 2 != 0;
        }
      }
      tests.push_back(MakePixelTest("ps_alu_random_" + std::to_string(i),
                                    [&](Asm& assembler) {
                                      assembler.Exec(alus);
                                      export_colors(assembler, 2);
                                    }));
    }
  }

  // Kills: vector (kill_eq 24 to kill_ne 27) and scalar (kills_eq 35 to
  // kills_one 39).
  for (uint32_t op = 24; op <= 27; ++op) {
    tests.push_back(MakePixelTest("ps_kill_vector_" + std::to_string(op),
                                  [&](Asm& assembler) {
                                    // Two interpolators that cross
                                    // each other on the screen.
                                    assembler.Exec(std::vector<Alu>{
                                        Vec(op, 5, "xyzw", R(1, "wwww"),
                                            R(3, "xxxx"))});
                                    export_colors(assembler, 2);
                                  }));
  }
  for (uint32_t op = 35; op <= 39; ++op) {
    tests.push_back(MakePixelTest("ps_kill_scalar_" + std::to_string(op),
                                  [&](Asm& assembler) {
                                    // A value that changes sign down
                                    // the screen.
                                    assembler.Exec(std::vector<Alu>{
                                        Scal(op, 5, "x___", R(1, "wwww"))});
                                    export_colors(assembler, 2);
                                  }));
  }

  // The alpha test: every function, the reference inside the alphas drawn.
  for (uint32_t function = 0; function < 8; ++function) {
    tests.push_back(MakePixelTest(
        "ps_alpha_test_" + std::to_string(function),
        [&](Asm& assembler) {
          assembler.Exec(std::vector<Alu>{Export(0, R(1))});
        },
        [function](Environment& environment) {
          // Interpolator 1's w goes from 0.6 at the top to -0.4.
          environment.system_constants.flags = function << 11;
          environment.system_constants.alpha_test_reference = 0.1f;
        }));
  }
  // Without colour 0 there is no alpha test.
  tests.push_back(MakePixelTest(
      "ps_alpha_test_no_color_0",
      [](Asm& assembler) {
        assembler.Exec(std::vector<Alu>{Export(1, R(1))});
      },
      [](Environment& environment) {
        environment.system_constants.flags = 0;
      }));
  // Exponent biases and gamma for each target.
  for (uint32_t i = 0; i < 3; ++i) {
    tests.push_back(MakePixelTest(
        "ps_exp_bias_gamma_" + std::to_string(i),
        [&](Asm& assembler) { export_colors(assembler, 1); },
        [i](Environment& environment) {
          environment.system_constants.flags =
              (7 << 11) | (i == 0 ? 0x4000 * 0b0101 : (i == 1 ? 0x4000 * 0xF
                                                              : 0x4000 * 2));
          environment.system_constants.color_exp_bias[0] = 0.25f;
          environment.system_constants.color_exp_bias[1] = 2.0f;
          environment.system_constants.color_exp_bias[3] = 0.125f;
        }));
  }
  // The generated parameter, for a filled primitive and for a line.
  for (uint32_t i = 0; i < 3; ++i) {
    Test test = MakePixelTest(
        "ps_param_gen_" + std::to_string(i),
        [&](Asm& assembler) { export_colors(assembler, 2); },
        [i](Environment& environment) {
          environment.system_constants.flags =
              (7 << 11) | (i == 1 ? 0x40 : (i == 2 ? 0x80 : 0));
        });
    test.param_gen_register = 3 + int(i);
    tests.push_back(test);
  }

  // Texture fetches. Fetch constants 0 to 3 are the four pairings of two
  // textures and two samplers.
  struct FetchCase {
    const char* name;
    uint32_t fetch_constant;
    uint32_t dimension;
    std::function<void(Ins&)> change;
    std::function<void(Environment&)> prepare;
    // 0 none, 1 setTexLOD, 2 setGradientH and V.
    uint32_t state = 0;
  };
  const uint32_t k2D = 1, kCube = 3;
  std::vector<FetchCase> cases = {
      {"2d_plain_0", 0, k2D},
      {"2d_plain_1", 1, k2D},
      {"2d_plain_2", 2, k2D},
      {"2d_plain_3", 3, k2D},
      {"2d_unnormalized", 0, k2D,
       [](Ins& ins) { ins.dwords[0] |= 1u << 25; }},
      {"2d_offsets", 1, k2D,
       [](Ins& ins) { ins.dwords[2] |= (3u << 16) | ((uint32_t(-5) & 31) << 21); }},
      {"2d_unnormalized_offsets", 2, k2D,
       [](Ins& ins) {
         ins.dwords[0] |= 1u << 25;
         ins.dwords[2] |= ((uint32_t(-2) & 31) << 16) | (7u << 21);
       }},
      {"2d_lod_bias", 0, k2D,
       [](Ins& ins) { ins.dwords[2] |= (uint32_t(20) & 0x7F) << 2; }},
      {"2d_lod_bias_negative", 0, k2D,
       [](Ins& ins) { ins.dwords[2] |= (uint32_t(-9) & 0x7F) << 2; }},
      {"2d_constant_lod_bias", 0, k2D, nullptr,
       [](Environment& environment) {
         environment.fetch_constants[4] |= (uint32_t(45) & 0x3FF) << 12;
       }},
      {"2d_explicit_lod", 0, k2D,
       [](Ins& ins) { ins.dwords[1] &= ~(1u << 28); },
       [](Environment& environment) {
         environment.fetch_constants[4] |= (uint32_t(50) & 0x3FF) << 12;
       }},
      {"2d_register_lod", 0, k2D,
       [](Ins& ins) {
         ins.dwords[1] &= ~(1u << 28);
         ins.dwords[1] |= 1u << 29;
       },
       nullptr, 1},
      {"2d_register_lod_computed", 0, k2D,
       [](Ins& ins) { ins.dwords[1] |= 1u << 29; }, nullptr, 1},
      {"2d_register_gradients", 0, k2D, [](Ins& ins) { ins.dwords[2] |= 1; },
       nullptr, 2},
      {"2d_register_gradients_unnormalized", 0, k2D,
       [](Ins& ins) {
         ins.dwords[2] |= 1;
         ins.dwords[0] |= 1u << 25;
       },
       nullptr, 2},
      {"2d_signed", 0, k2D, nullptr,
       [](Environment& environment) { SetSigns(environment, 0, 0x55); }},
      {"2d_biased", 1, k2D, nullptr,
       [](Environment& environment) { SetSigns(environment, 1, 0xAA); }},
      {"2d_gamma", 2, k2D, nullptr,
       [](Environment& environment) { SetSigns(environment, 2, 0xFF); }},
      {"2d_mixed_signs", 3, k2D, nullptr,
       [](Environment& environment) { SetSigns(environment, 3, 0x1B); }},
      {"2d_mixed_signs_2", 0, k2D, nullptr,
       [](Environment& environment) { SetSigns(environment, 0, 0xE4); }},
      {"2d_exponent_bias", 0, k2D, nullptr,
       [](Environment& environment) {
         environment.fetch_constants[3] |= (uint32_t(3) & 0x3F) << 13;
       }},
      {"2d_exponent_bias_negative", 1, k2D, nullptr,
       [](Environment& environment) {
         environment.fetch_constants[6 + 3] |= (uint32_t(-4) & 0x3F) << 13;
         SetSigns(environment, 1, 0x66);
       }},
      // The destination swizzle: w, z, 1, kept; then x alone with 0s.
      {"2d_dest_swizzle", 0, k2D,
       [](Ins& ins) {
         ins.dwords[1] = (ins.dwords[1] & ~0xFFFu) | 3 | (2 << 3) | (5 << 6) |
                         (7 << 9);
       }},
      {"2d_dest_swizzle_2", 1, k2D,
       [](Ins& ins) {
         ins.dwords[1] = (ins.dwords[1] & ~0xFFFu) | 4 | (0 << 3) | (4 << 6) |
                         (0 << 9);
       },
       [](Environment& environment) { SetSigns(environment, 1, 0x39); }},
      {"cube_plain_0", 0, kCube},
      {"cube_plain_1", 1, kCube},
      {"cube_plain_3", 3, kCube},
      {"cube_offsets", 0, kCube,
       [](Ins& ins) {
         ins.dwords[2] |= (2u << 16) | ((uint32_t(-3) & 31) << 21) | (2u << 26);
       }},
      {"cube_unnormalized", 0, kCube,
       [](Ins& ins) { ins.dwords[0] |= 1u << 25; }},
      {"cube_explicit_lod", 1, kCube,
       [](Ins& ins) {
         ins.dwords[1] &= ~(1u << 28);
         ins.dwords[1] |= 1u << 29;
       },
       nullptr, 1},
      {"cube_register_gradients", 0, kCube,
       [](Ins& ins) { ins.dwords[2] |= 1; }, nullptr, 2},
      {"cube_lod_bias", 0, kCube,
       [](Ins& ins) { ins.dwords[2] |= (uint32_t(24) & 0x7F) << 2; }},
      {"cube_signed", 2, kCube, nullptr,
       [](Environment& environment) { SetSigns(environment, 2, 0x55); }},
      {"cube_mixed_signs", 1, kCube, nullptr,
       [](Environment& environment) { SetSigns(environment, 1, 0x93); }},
  };
  for (const FetchCase& fetch_case : cases) {
    tests.push_back(MakePixelTest(
        std::string("ps_tfetch_") + fetch_case.name,
        [&](Asm& assembler) {
          std::vector<Ins> instructions;
          // Texture coordinates over more than one copy of the texture.
          instructions.push_back(
              Vec(1 /* mul */, 4, "xyzw", R(0), R(9, "xxxw")).Encode());
          instructions.push_back(
              Vec(0 /* add */, 5, "xyzw", R(0, "xyzz"), R(3, "wwww")).Encode());
          instructions.push_back(Vec(2, 6, "xyzw", R(1), R(1)).Encode());
          if (fetch_case.state == 1) {
            instructions.push_back(SetFetchState(24, 1, "yyy"));
          } else if (fetch_case.state == 2) {
            instructions.push_back(SetFetchState(25, 2, "xyz"));
            instructions.push_back(SetFetchState(26, 3, "yzx"));
          }
          Ins fetch = TextureFetch(6, fetch_case.dimension == kCube ? 0 : 5,
                                   fetch_case.fetch_constant,
                                   fetch_case.dimension);
          if (fetch_case.change) {
            fetch_case.change(fetch);
          }
          instructions.push_back(fetch);
          instructions.push_back(Export(0, R(6)).Encode());
          instructions.push_back(Export(1, R(6, "wzyx")).Encode());
          assembler.Exec(instructions);
        },
        fetch_case.prepare));
  }
  // Several textures in one shader, more fetches than textures, a fetch that
  // is predicated away for some pixels, one in an exec a bool constant
  // decides, and a slot used with two sets of filters.
  tests.push_back(MakePixelTest(
      "ps_tfetch_many",
      [](Asm& assembler) {
        std::vector<Ins> instructions;
        instructions.push_back(TextureFetch(4, 0, 0, 1));
        instructions.push_back(TextureFetch(5, 1, 1, 1, "yxz"));
        instructions.push_back(TextureFetch(6, 0, 2, 3));
        instructions.push_back(TextureFetch(7, 2, 3, 1));
        instructions.push_back(TextureFetch(8, 1, 0, 1));
        Ins point_filtered = TextureFetch(9, 0, 0, 1);
        point_filtered.dwords[1] &= ~((3u << 12) | (3u << 14));
        instructions.push_back(point_filtered);
        instructions.push_back(
            Vec(1 /* mul */, 4, "xyzw", R(4), R(5)).Encode());
        instructions.push_back(
            Vec(0 /* add */, 6, "xyzw", R(6), R(7)).Encode());
        // setp_gt on a coordinate, then a fetch only where it holds.
        instructions.push_back(Scal(29, 10, "____", R(1, "xxxx")).Encode());
        Ins predicated = TextureFetch(8, 2, 1, 1);
        // With an explicit level of detail: where the predicate differs
        // inside a 2 by 2 block of pixels the screen's derivatives are
        // undefined, for the translated shader as for the interpreter, and
        // the two then differ there (seen: 15 pixels along the edge).
        predicated.dwords[1] &= ~(1u << 28);
        predicated.dwords[1] |= 1u << 31;
        predicated.dwords[2] |= 1u << 31;
        instructions.push_back(predicated);
        assembler.Exec(instructions);
        assembler.Exec(std::vector<Ins>{TextureFetch(9, 3, 2, 1, "yxz")},
                       Asm::Condition::kBool, 40, true);
        assembler.Exec(std::vector<Alu>{Export(0, R(4)), Export(1, R(6)),
                                        Export(2, R(8)), Export(3, R(9))});
      },
      [](Environment& environment) {
        SetSigns(environment, 1, 0x55);
        SetSigns(environment, 3, 0xAA);
      }));
}

// -----------------------------------------------------------------------------
// Running.
// -----------------------------------------------------------------------------

struct Options {
  std::string spirv_directory;
  std::string filter;
  bool verbose = false;
  bool list = false;
};

std::vector<uint32_t> ReadSpirv(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    std::fprintf(stderr, "rig: cannot read %s\n", path.c_str());
    std::exit(1);
  }
  std::fseek(file, 0, SEEK_END);
  long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<uint32_t> words(size_t(size) / 4);
  if (std::fread(words.data(), 4, words.size(), file) != words.size()) {
    std::fprintf(stderr, "rig: short read of %s\n", path.c_str());
    std::exit(1);
  }
  std::fclose(file);
  return words;
}

SpirvShaderTranslator::Features RigFeatures() {
  // What the translator is told about the device: everything but the float
  // control modes and barycentrics, which the software device does not have
  // and which change nothing the rig compares.
  SpirvShaderTranslator::Features features(true);
  features.spirv_version = spv::Spv_1_0;
  features.signed_zero_inf_nan_preserve_float32 = false;
  features.denorm_flush_to_zero_float32 = false;
  features.rounding_mode_rte_float32 = false;
  features.fragment_shader_sample_interlock = false;
  features.fragment_shader_barycentric = false;
  features.demote_to_helper_invocation = false;
  return features;
}

class VertexRunner {
 public:
  VertexRunner(Device& device, const Options& options)
      : device_(device), options_(options) {
    VkDevice vk = device_.device;
    // Set 0: guest memory. Set 1: the five constant buffers. Sets 2 and 3:
    // textures, none here. Set 4: where the capture geometry shader writes.
    const VkShaderStageFlags kStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT;
    VkDescriptorSetLayoutBinding memory_binding = {
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kStages, nullptr};
    set_layouts_[0] = MakeSetLayout(&memory_binding, 1);
    VkDescriptorSetLayoutBinding constant_bindings[5];
    for (uint32_t i = 0; i < 5; ++i) {
      constant_bindings[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kStages,
                              nullptr};
    }
    set_layouts_[1] = MakeSetLayout(constant_bindings, 5);
    set_layouts_[2] = MakeSetLayout(nullptr, 0);
    set_layouts_[3] = MakeSetLayout(nullptr, 0);
    VkDescriptorSetLayoutBinding capture_binding = {
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_GEOMETRY_BIT,
        nullptr};
    set_layouts_[4] = MakeSetLayout(&capture_binding, 1);
    VkPipelineLayoutCreateInfo layout_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 5;
    layout_info.pSetLayouts = set_layouts_;
    RIG_VK(vkCreatePipelineLayout(vk, &layout_info, nullptr, &pipeline_layout_));

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
    };
    VkDescriptorPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 16;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    RIG_VK(vkCreateDescriptorPool(vk, &pool_info, nullptr, &descriptor_pool_));

    shared_memory_ = device_.MakeBuffer(kSharedMemoryWords * 4,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    system_constants_ =
        device_.MakeBuffer(sizeof(SpirvShaderTranslator::SystemConstants),
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float_full_ =
        device_.MakeBuffer(256 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float_packed_ =
        device_.MakeBuffer(256 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float_pixel_ =
        device_.MakeBuffer(256 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    bool_loop_program_ = device_.MakeBuffer(ucode_program::kBlockBytes,
                                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    fetch_constants_ =
        device_.MakeBuffer(32 * 6 * 4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    for (Buffer& capture : captures_) {
      capture = device_.MakeBuffer(kVertexCount * kCaptureVectors * 16,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }

    memory_set_ = AllocateSet(set_layouts_[0]);
    WriteBuffer(memory_set_, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                shared_memory_);
    for (uint32_t i = 0; i < 2; ++i) {
      // 0: for the translated shader (packed float constants, the bool and
      // loop constants alone). 1: for the interpreter (all 256, the program
      // behind the bool and loop constants).
      constant_sets_[i] = AllocateSet(set_layouts_[1]);
      WriteBuffer(constant_sets_[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  system_constants_);
      WriteBuffer(constant_sets_[i], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  i ? float_full_ : float_packed_);
      WriteBuffer(constant_sets_[i], 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  float_pixel_);
      WriteBuffer(constant_sets_[i], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  bool_loop_program_);
      WriteBuffer(constant_sets_[i], 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  fetch_constants_);
      capture_sets_[i] = AllocateSet(set_layouts_[4]);
      WriteBuffer(capture_sets_[i], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                  captures_[i]);
    }
    empty_sets_[0] = AllocateSet(set_layouts_[2]);
    empty_sets_[1] = AllocateSet(set_layouts_[3]);

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo render_pass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    RIG_VK(vkCreateRenderPass(vk, &render_pass_info, nullptr, &render_pass_));
    VkFramebufferCreateInfo framebuffer_info = {
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = render_pass_;
    framebuffer_info.width = 16;
    framebuffer_info.height = 16;
    framebuffer_info.layers = 1;
    RIG_VK(vkCreateFramebuffer(vk, &framebuffer_info, nullptr, &framebuffer_));

    std::vector<uint32_t> capture_words =
        ReadSpirv(options_.spirv_directory + "/rig_capture.gs.spv");
    capture_shader_ = device_.MakeShaderModule(capture_words.data(),
                                               capture_words.size() * 4);
    std::vector<uint32_t> interpreter_words =
        ReadSpirv(options_.spirv_directory + "/ucode_program.vs.spv");
    VkShaderModule interpreter_shader = device_.MakeShaderModule(
        interpreter_words.data(), interpreter_words.size() * 4);
    interpreter_pipeline_ = MakePipeline(interpreter_shader);
  }

  // True if the interpreter wrote what the translated shader wrote.
  bool Run(const Test& test, std::mt19937& rng) {
    Environment environment;
    FillEnvironment(environment, rng);
    if (test.prepare) {
      test.prepare(environment);
    }

    xe::gpu::SpirvShader shader(xe::gpu::xenos::ShaderType::kVertex, 0,
                                test.ucode.data(), test.ucode.size(),
                                std::endian::little);
    xe::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    SpirvShaderTranslator translator(RigFeatures(), true, true, false);
    SpirvShaderTranslator::Modification modification(
        translator.GetDefaultVertexShaderModification(
            shader.GetDynamicAddressableRegisterCount(
                xe::gpu::xenos::kMaxShaderTempRegisters),
            xe::gpu::Shader::HostVertexShaderType::kVertex));
    modification.vertex.interpolator_mask = 0xFFFF;
    xe::gpu::Shader::Translation* translation =
        shader.GetOrCreateTranslation(modification.value);
    if (!translator.TranslateAnalyzedShader(*translation) ||
        !translation->is_valid()) {
      std::printf("FAIL %s: the translator refused it\n", test.name.c_str());
      return false;
    }
    const std::vector<uint8_t>& translated = translation->translated_binary();
    VkShaderModule translated_shader = device_.MakeShaderModule(
        reinterpret_cast<const uint32_t*>(translated.data()),
        translated.size());
    VkPipeline translated_pipeline = MakePipeline(translated_shader);

    ucode_program::Program program;
    ucode_program::Build(xe::gpu::xenos::ShaderType::kVertex, test.ucode.data(),
                         uint32_t(test.ucode.size()), program);
    if (program.features & ucode_program::kFeatureMalformed ||
        program.element_count() > ucode_program::kCodeElements) {
      std::printf("FAIL %s: no program (features %08X, %u elements)\n",
                  test.name.c_str(), program.features, program.element_count());
      return false;
    }

    // Upload.
    for (uint32_t i = 0; i < kSharedMemoryWords; ++i) {
      uint32_t word = Swap32(environment.shared_memory[i]);
      std::memcpy(shared_memory_.data + i * 4, &word, 4);
    }
    std::memcpy(system_constants_.data, &environment.system_constants,
                sizeof(environment.system_constants));
    std::memcpy(float_full_.data, environment.float_constants, 256 * 16);
    const xe::gpu::Shader::ConstantRegisterMap& constant_map =
        shader.constant_register_map();
    uint8_t* packed = float_packed_.data;
    for (uint32_t i = 0; i < 256; ++i) {
      if (constant_map.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63))) {
        std::memcpy(packed, environment.float_constants[i], 16);
        packed += 16;
      }
    }
    uint32_t* block = reinterpret_cast<uint32_t*>(bool_loop_program_.data);
    std::memcpy(block, environment.bool_constants, 32);
    std::memcpy(block + 8, environment.loop_constants, 128);
    uint32_t* head = block + ucode_program::kBoolLoopElements * 4;
    std::memset(head, 0, ucode_program::kHeadElements * 16);
    // The vertex program at element 0; the per-draw vertex word: kill by NaN.
    head[0] = 0;
    head[2] = 1;
    std::memcpy(head + ucode_program::kHeadElements * 4, program.words.data(),
                program.words.size() * 4);
    std::memcpy(fetch_constants_.data, environment.fetch_constants,
                sizeof(environment.fetch_constants));

    for (Buffer& capture : captures_) {
      std::memset(capture.data, 0xCD, size_t(capture.size));
    }
    Draw(translated_pipeline, 0);
    Draw(interpreter_pipeline_, 1);
    vkDestroyPipeline(device_.device, translated_pipeline, nullptr);
    vkDestroyShaderModule(device_.device, translated_shader, nullptr);

    // Compare.
    const float* expected = reinterpret_cast<const float*>(captures_[0].data);
    const float* actual = reinterpret_cast<const float*>(captures_[1].data);
    for (const Buffer& capture : captures_) {
      for (uint32_t i = 0; i < kVertexCount * kCaptureVectors * 4; ++i) {
        uint32_t word;
        std::memcpy(&word, capture.data + i * 4, 4);
        if (word == 0xCDCDCDCDu) {
          std::printf("FAIL %s: a draw wrote nothing at value %u\n",
                      test.name.c_str(), i);
          return false;
        }
      }
    }
    uint32_t mismatches = 0;
    float worst = 0.0f;
    for (uint32_t i = 0; i < kVertexCount * kCaptureVectors * 4; ++i) {
      float e = expected[i], a = actual[i];
      float difference;
      if (std::isnan(e) || std::isnan(a)) {
        difference = (std::isnan(e) && std::isnan(a)) ? 0.0f : INFINITY;
      } else if (std::isinf(e) || std::isinf(a)) {
        difference = e == a ? 0.0f : INFINITY;
      } else {
        difference =
            std::abs(e - a) / std::max(1.0f, std::max(std::abs(e), std::abs(a)));
      }
      if (difference > test.tolerance) {
        if (mismatches < 6 && (options_.verbose || mismatches < 3)) {
          uint32_t vector = (i / 4) % kCaptureVectors;
          std::printf(
              "  %s: vertex %u, %s%s.%c: translated %.9g, interpreter %.9g\n",
              test.name.c_str(), i / (kCaptureVectors * 4),
              vector ? "interpolator " : "position",
              vector ? std::to_string(vector - 1).c_str() : "", "xyzw"[i & 3],
              double(e), double(a));
        }
        ++mismatches;
      }
      worst = std::max(worst, difference);
    }
    if (mismatches) {
      std::printf("FAIL %s: %u of %u values differ (worst %g)\n",
                  test.name.c_str(), mismatches,
                  kVertexCount * kCaptureVectors * 4, double(worst));
      if (options_.verbose) {
        std::printf("%s\n", shader.ucode_disassembly().c_str());
      }
      return false;
    }
    if (options_.verbose) {
      std::printf("ok   %s (worst difference %g, program %u elements)\n",
                  test.name.c_str(), double(worst), program.element_count());
    }
    return true;
  }

 private:
  VkDescriptorSetLayout MakeSetLayout(
      const VkDescriptorSetLayoutBinding* bindings, uint32_t count) {
    VkDescriptorSetLayoutCreateInfo layout_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = count;
    layout_info.pBindings = bindings;
    VkDescriptorSetLayout layout;
    RIG_VK(vkCreateDescriptorSetLayout(device_.device, &layout_info, nullptr,
                                       &layout));
    return layout;
  }

  VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo allocate_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate_info.descriptorPool = descriptor_pool_;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &layout;
    VkDescriptorSet set;
    RIG_VK(vkAllocateDescriptorSets(device_.device, &allocate_info, &set));
    return set;
  }

  void WriteBuffer(VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                   const Buffer& buffer) {
    VkDescriptorBufferInfo buffer_info = {buffer.buffer, 0, buffer.size};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_.device, 1, &write, 0, nullptr);
  }

  VkPipeline MakePipeline(VkShaderModule vertex_shader) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    stages[1].module = capture_shader_;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    VkPipelineRasterizationStateCreateInfo rasterization = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.rasterizerDiscardEnable = VK_TRUE;
    rasterization.lineWidth = 1.0f;
    VkGraphicsPipelineCreateInfo pipeline_info = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass_;
    VkPipeline pipeline;
    RIG_VK(vkCreateGraphicsPipelines(device_.device, VK_NULL_HANDLE, 1,
                                     &pipeline_info, nullptr, &pipeline));
    return pipeline;
  }

  void Draw(VkPipeline pipeline, uint32_t which) {
    VkCommandBuffer command_buffer = device_.Begin();
    VkRenderPassBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin_info.renderPass = render_pass_;
    begin_info.framebuffer = framebuffer_;
    begin_info.renderArea = {{0, 0}, {16, 16}};
    vkCmdBeginRenderPass(command_buffer, &begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline);
    VkDescriptorSet sets[5] = {memory_set_, constant_sets_[which],
                               empty_sets_[0], empty_sets_[1],
                               capture_sets_[which]};
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 5, sets, 0, nullptr);
    vkCmdDraw(command_buffer, kVertexCount, 1, 0, 0);
    vkCmdEndRenderPass(command_buffer);
    device_.SubmitAndWait(command_buffer);
  }

  Device& device_;
  const Options& options_;
  VkDescriptorSetLayout set_layouts_[5] = {};
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  Buffer shared_memory_, system_constants_, float_full_, float_packed_,
      float_pixel_, bool_loop_program_, fetch_constants_;
  Buffer captures_[2];
  VkDescriptorSet memory_set_ = VK_NULL_HANDLE;
  VkDescriptorSet constant_sets_[2] = {};
  VkDescriptorSet capture_sets_[2] = {};
  VkDescriptorSet empty_sets_[2] = {};
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
  VkShaderModule capture_shader_ = VK_NULL_HANDLE;
  VkPipeline interpreter_pipeline_ = VK_NULL_HANDLE;
};

#include "xenia/gpu/first_run_rig/rig_pixel.h"

}  // namespace rig

int main(int argc, char** argv) {
  rig::Options options;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--spv" && i + 1 < argc) {
      options.spirv_directory = argv[++i];
    } else if (arg == "--filter" && i + 1 < argc) {
      options.filter = argv[++i];
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--vector-signs") {
      // The translator's other form of a texture fetch's sign step
      // (spirv_tfetch_vector_signs): it must match the interpreter as well.
      cvars::spirv_tfetch_vector_signs = true;
    } else if (arg == "--list") {
      options.list = true;
    } else {
      std::fprintf(stderr,
                   "usage: %s --spv DIR [--filter TEXT] [--verbose] [--list] "
                   "[--vector-signs]\n",
                   argv[0]);
      return 2;
    }
  }
  if (options.spirv_directory.empty()) {
    std::fprintf(stderr, "rig: --spv DIR is needed (build.sh makes it)\n");
    return 2;
  }
  cvars::precise_interpolation = false;

  std::vector<rig::Test> tests;
  rig::AddVertexTests(tests);
  rig::AddPixelTests(tests);
  if (options.list) {
    for (const rig::Test& test : tests) {
      std::printf("%s\n", test.name.c_str());
    }
    return 0;
  }

  rig::Device device;
  device.Initialize();
  std::printf("device: %s\n", device.name);
  rig::VertexRunner vertex_runner(device, options);
  rig::PixelRunner pixel_runner(device, options);

  std::mt19937 rng(360);
  uint32_t run = 0, failed = 0;
  for (const rig::Test& test : tests) {
    if (!options.filter.empty() &&
        test.name.find(options.filter) == std::string::npos) {
      continue;
    }
    ++run;
    bool ok = test.type == xe::gpu::xenos::ShaderType::kVertex
                  ? vertex_runner.Run(test, rng)
                  : pixel_runner.Run(test, rng);
    if (!ok) {
      ++failed;
    }
  }
  std::printf("%u test shaders, %u differ from the translated shader\n", run,
              failed);
  return failed ? 1 : 0;
}
