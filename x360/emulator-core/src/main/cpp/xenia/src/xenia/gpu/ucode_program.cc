// hakuX-Boosted: the fork's own file, not upstream's. See ucode_program.h.

#include "xenia/gpu/ucode_program.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/math.h"
#include "xenia/gpu/ucode.h"

namespace xe {
namespace gpu {
namespace ucode_program {

namespace {

struct Walk {
  xenos::ShaderType shader_type;
  Program* program;
  uint32_t* instruction_words;
  uint32_t instruction_first;
  uint32_t instruction_count;
  std::vector<bool> instruction_seen;
  ucode::VertexFetchInstruction previous_vfetch_full;
  uint32_t register_bound = 0;

  void UseRegister(uint32_t index) {
    register_bound = std::max(register_bound, (index & 63) + 1);
  }

  void Alu(uint32_t* words) {
    ucode::AluInstruction op;
    std::memcpy(&op, words, sizeof(op));
    ucode::AluVectorOpcode vector_opcode = op.vector_opcode();
    const ucode::AluVectorOpcodeInfo& vector_info =
        ucode::GetAluVectorOpcodeInfo(vector_opcode);
    ucode::AluScalarOpcode scalar_opcode = op.scalar_opcode();
    const ucode::AluScalarOpcodeInfo& scalar_info =
        ucode::GetAluScalarOpcodeInfo(scalar_opcode);
    uint32_t vector_mask = op.GetVectorOpResultWriteMask();
    uint32_t scalar_mask = op.GetScalarOpResultWriteMask();
    bool vector_runs =
        vector_mask || vector_info.changed_state != ucode::kAluOpChangedStateNone;
    uint32_t vector_operand_count = vector_info.GetOperandCount();

    uint32_t info = (vector_mask << kAluVectorMaskShift) |
                    (scalar_mask << kAluScalarMaskShift) |
                    (op.GetConstant0WriteMask() << kAluConst0MaskShift) |
                    (op.GetConstant1WriteMask() << kAluConst1MaskShift) |
                    (vector_runs ? kAluVectorRuns : 0) |
                    (scalar_info.operand_count << kAluScalarOperandsShift) |
                    (vector_operand_count << kAluVectorOperandsShift);
    for (uint32_t i = 1; i <= 3; ++i) {
      if (!op.src_is_temp(i) && op.src_const_is_addressed(i)) {
        info |= kAluAddressed1 << (i - 1);
      }
    }
    words[3] = info;

    uint32_t& features = program->features;
    // What the operands in use address.
    for (uint32_t i = 1; i <= 3; ++i) {
      bool used = (vector_runs && i <= vector_operand_count) ||
                  (i == 3 && scalar_info.operand_count != 0);
      if (!used) {
        continue;
      }
      if (op.src_is_temp(i)) {
        if (i == 3 && scalar_info.operand_count == 2 &&
            !(vector_runs && vector_operand_count >= 3)) {
          // mulsc, addsc, subsc: the third source is a constant.
          continue;
        }
        uint32_t reg = op.src_reg(i);
        UseRegister(ucode::AluInstruction::src_temp_reg(reg));
        if (ucode::AluInstruction::is_src_temp_relative(reg)) {
          features |= kFeatureRelativeAddressing;
        }
      } else if (op.src_const_is_addressed(i)) {
        features |= kFeatureRelativeAddressing;
      }
    }
    if (scalar_info.operand_count == 2) {
      UseRegister(op.scalar_const_reg_op_src_temp_reg());
      if (op.src_const_is_addressed(3)) {
        features |= kFeatureRelativeAddressing;
      }
    }
    if (vector_info.changed_state & ucode::kAluOpChangedStatePixelKill ||
        scalar_info.changed_state & ucode::kAluOpChangedStatePixelKill) {
      features |= kFeatureKill;
    }

    if (op.is_export()) {
      uint32_t export_register = op.vector_dest();
      uint32_t written = vector_mask | scalar_mask |
                         op.GetConstant0WriteMask() | op.GetConstant1WriteMask();
      if (export_register >= uint32_t(ucode::ExportRegister::kExportAddress) &&
          export_register <= uint32_t(ucode::ExportRegister::kExportData4)) {
        features |= kFeatureMemoryExport;
      } else if (shader_type == xenos::ShaderType::kVertex) {
        if (export_register ==
            uint32_t(ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex)) {
          if (written & 0b001) {
            features |= kFeaturePointSize;
          }
          if (written & 0b100) {
            features |= kFeatureVertexKill;
          }
        }
      } else {
        if (export_register == uint32_t(ucode::ExportRegister::kPSDepth)) {
          features |= kFeatureDepthOutput;
        }
      }
      return;
    }

    // Register results.
    if (vector_mask) {
      UseRegister(op.vector_dest());
      if (op.is_vector_dest_relative()) {
        features |= kFeatureRelativeAddressing;
      }
    }
    if (scalar_mask) {
      UseRegister(op.scalar_dest());
      if (op.is_scalar_dest_relative()) {
        features |= kFeatureRelativeAddressing;
      }
    }
  }

  void Fetch(uint32_t* words) {
    ucode::FetchInstruction op;
    std::memcpy(&op, words, sizeof(op));
    uint32_t& features = program->features;
    FetchKind kind = FetchKind::kZero;
    uint32_t info = 0;
    bool writes_result = true;
    switch (op.opcode()) {
      case ucode::FetchOpcode::kVertexFetch: {
        const ucode::VertexFetchInstruction& vfetch = op.vertex_fetch();
        kind = FetchKind::kVertex;
        if (!vfetch.is_mini_fetch()) {
          previous_vfetch_full = vfetch;
          UseRegister(vfetch.src());
          if (vfetch.is_src_relative()) {
            features |= kFeatureRelativeAddressing;
          }
        }
        info |= previous_vfetch_full.fetch_constant_index()
                << kFetchVertexConstantShift;
      } break;
      case ucode::FetchOpcode::kTextureFetch: {
        const ucode::TextureFetchInstruction& tfetch = op.texture_fetch();
        features |= kFeatureTextureFetch;
        ++program->texture_fetch_count;
        UseRegister(tfetch.src());
        if (tfetch.is_src_relative()) {
          features |= kFeatureRelativeAddressing;
        }
        if (tfetch.use_register_lod() || tfetch.use_register_gradients()) {
          features |= kFeatureExplicitLod;
        }
        switch (tfetch.dimension()) {
          case xenos::FetchOpDimension::k1D:
            features |= kFeatureTexture1D;
            break;
          case xenos::FetchOpDimension::k3DOrStacked:
            features |= kFeatureTexture3D;
            break;
          case xenos::FetchOpDimension::kCube:
            features |= kFeatureTextureCube;
            break;
          default:
            break;
        }
        uint32_t slot = 0;
        for (; slot < program->textures.size(); ++slot) {
          const Texture& texture = program->textures[slot];
          if (texture.fetch_constant == tfetch.fetch_constant_index() &&
              texture.dimension == tfetch.dimension()) {
            break;
          }
        }
        if (slot == program->textures.size()) {
          if (slot >= kMaxTextures) {
            features |= kFeatureTooManyTextures;
            break;
          }
          Texture texture;
          texture.fetch_constant = uint8_t(tfetch.fetch_constant_index());
          texture.dimension = tfetch.dimension();
          texture.mag_filter = tfetch.mag_filter();
          texture.min_filter = tfetch.min_filter();
          texture.mip_filter = tfetch.mip_filter();
          texture.aniso_filter = tfetch.aniso_filter();
          program->textures.push_back(texture);
        }
        kind = FetchKind::kTexture;
        info |= slot << kFetchTextureSlotShift;
      } break;
      case ucode::FetchOpcode::kSetTextureLod:
        kind = FetchKind::kSetLod;
        features |= kFeatureExplicitLod;
        writes_result = false;
        UseRegister(op.src());
        break;
      case ucode::FetchOpcode::kSetTextureGradientsHorz:
        kind = FetchKind::kSetGradientsHorizontal;
        features |= kFeatureExplicitLod;
        writes_result = false;
        UseRegister(op.src());
        break;
      case ucode::FetchOpcode::kSetTextureGradientsVert:
        kind = FetchKind::kSetGradientsVertical;
        features |= kFeatureExplicitLod;
        writes_result = false;
        UseRegister(op.src());
        break;
      default:
        features |= kFeatureFetchQuery;
        break;
    }
    words[3] = info | uint32_t(kind);
    if (writes_result) {
      UseRegister(op.dest());
      if (op.is_dest_relative()) {
        features |= kFeatureRelativeAddressing;
      }
    }
  }

  void Exec(const ucode::ControlFlowExecInstruction& exec) {
    uint32_t sequence = exec.sequence();
    for (uint32_t i = 0; i < exec.count(); ++i, sequence >>= 2) {
      uint32_t index = exec.address() + i - instruction_first;
      if (index >= instruction_count || instruction_seen[index]) {
        // Named by an earlier exec as well: prepared already.
        continue;
      }
      instruction_seen[index] = true;
      ++program->instruction_count;
      uint32_t* words = instruction_words + size_t(index) * 4;
      if (sequence & 0b01) {
        Fetch(words);
      } else {
        Alu(words);
      }
    }
  }
};

bool IsExec(ucode::ControlFlowOpcode opcode) {
  return ucode::IsControlFlowOpcodeExec(opcode);
}

}  // namespace

void Build(xenos::ShaderType shader_type, const uint32_t* ucode_dwords,
           uint32_t ucode_dword_count, Program& program_out) {
  program_out = Program();
  Program& program = program_out;

  // The control flow instructions come first, in pairs of three words, up to
  // the first instruction any exec names (as Shader::AnalyzeUcode finds it).
  uint32_t triple_count = ucode_dword_count / 3;
  uint32_t cf_pair_bound = triple_count;
  for (uint32_t i = 0; i < cf_pair_bound; ++i) {
    ucode::ControlFlowInstruction cf_ab[2];
    ucode::UnpackControlFlowInstructions(ucode_dwords + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      if (IsExec(cf_ab[j].opcode())) {
        cf_pair_bound = std::min(cf_pair_bound, cf_ab[j].exec.address());
      }
    }
  }
  uint32_t cf_count = cf_pair_bound * 2;
  std::vector<ucode::ControlFlowInstruction> cfs(cf_count);
  uint32_t instruction_first = UINT32_MAX, instruction_end = 0;
  bool ends = false;
  for (uint32_t i = 0; i < cf_pair_bound; ++i) {
    ucode::UnpackControlFlowInstructions(ucode_dwords + i * 3, &cfs[i * 2]);
  }
  for (const ucode::ControlFlowInstruction& cf : cfs) {
    ucode::ControlFlowOpcode opcode = cf.opcode();
    if (IsExec(opcode)) {
      if (cf.exec.count()) {
        instruction_first = std::min(instruction_first, cf.exec.address());
        instruction_end =
            std::max(instruction_end, cf.exec.address() + cf.exec.count());
      }
      ends |= ucode::DoesControlFlowOpcodeEndShader(opcode);
    }
    switch (opcode) {
      case ucode::ControlFlowOpcode::kLoopStart:
      case ucode::ControlFlowOpcode::kLoopEnd:
        program.features |= kFeatureLoops;
        break;
      case ucode::ControlFlowOpcode::kCondCall:
      case ucode::ControlFlowOpcode::kReturn:
        program.features |= kFeatureCalls;
        break;
      case ucode::ControlFlowOpcode::kAlloc:
        if (cf.alloc.alloc_type() == ucode::AllocType::kMemory) {
          program.features |= kFeatureMemoryExport;
        }
        break;
      default:
        break;
    }
  }
  if (instruction_first == UINT32_MAX) {
    // No instruction at all: a shader of control flow only.
    instruction_first = cf_pair_bound;
    instruction_end = cf_pair_bound;
  }
  if (!cf_count || !ends || instruction_end > triple_count ||
      instruction_first < cf_pair_bound) {
    program.features |= kFeatureMalformed;
    return;
  }

  uint32_t instruction_count = instruction_end - instruction_first;
  program.words.resize(size_t(1 + cf_count + instruction_count) * 4);
  uint32_t* words = program.words.data();
  for (uint32_t i = 0; i < cf_count; ++i) {
    uint32_t* cf_words = words + size_t(1 + i) * 4;
    cf_words[0] = cfs[i].dword_0;
    cf_words[1] = cfs[i].dword_1;
  }
  uint32_t* instruction_words = words + size_t(1 + cf_count) * 4;
  for (uint32_t i = 0; i < instruction_count; ++i) {
    std::memcpy(instruction_words + size_t(i) * 4,
                ucode_dwords + size_t(instruction_first + i) * 3,
                sizeof(uint32_t) * 3);
  }

  Walk walk;
  walk.shader_type = shader_type;
  walk.program = &program;
  walk.instruction_words = instruction_words;
  walk.instruction_first = instruction_first;
  walk.instruction_count = instruction_count;
  walk.instruction_seen.resize(instruction_count);
  std::memset(&walk.previous_vfetch_full, 0, sizeof(walk.previous_vfetch_full));
  for (const ucode::ControlFlowInstruction& cf : cfs) {
    if (IsExec(cf.opcode())) {
      walk.Exec(cf.exec);
    }
  }

  words[0] = cf_count;
  // Where instruction 0 of the microcode would be, relative to this element
  // (it wraps: the interpreter adds an exec's address to it).
  words[1] = 1 + cf_count - instruction_first;
  words[2] = program.features;
  // With relative addressing any register can be reached.
  words[3] = (program.features & kFeatureRelativeAddressing)
                 ? 64
                 : std::min(walk.register_bound, UINT32_C(64));
}

const char* GetFeatureName(uint32_t feature_bit_index) {
  static const char* const kNames[] = {
      "malformed",
      "loops",
      "subroutine calls",
      "relative addressing",
      "texture fetches",
      "fetch queries",
      "explicit LOD or gradients",
      "memory export",
      "too many textures",
      "3D or stacked textures",
      "cube textures",
      "1D textures",
      "depth output",
      "pixel kill",
      "(unused)",
      "point size",
      "vertex kill",
  };
  return feature_bit_index < xe::countof(kNames) ? kNames[feature_bit_index]
                                                 : "unknown";
}

}  // namespace ucode_program
}  // namespace gpu
}  // namespace xe
