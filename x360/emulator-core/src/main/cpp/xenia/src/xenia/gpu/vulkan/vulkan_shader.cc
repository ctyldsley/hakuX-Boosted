/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_shader.h"

#include <cstdint>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/ucode.h"
#include "xenia/ui/vulkan/vulkan_provider.h"

namespace xe {
namespace gpu {
namespace vulkan {

VulkanShader::VulkanTranslation::~VulkanTranslation() {
  if (shader_module_ != VK_NULL_HANDLE) {
    const ui::vulkan::VulkanDevice* const vulkan_device =
        static_cast<const VulkanShader&>(shader()).vulkan_device_;
    vulkan_device->functions().vkDestroyShaderModule(vulkan_device->device(),
                                                     shader_module_, nullptr);
  }
}

VkShaderModule VulkanShader::VulkanTranslation::GetOrCreateShaderModule() {
  if (!is_valid()) {
    return VK_NULL_HANDLE;
  }

  // Lock for creation - multiple threads may try to create the same shader
  std::lock_guard<std::mutex> lock(shader_module_mutex_);

  if (shader_module_ != VK_NULL_HANDLE) {
    return shader_module_;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      static_cast<const VulkanShader&>(shader()).vulkan_device_;

  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = translated_binary().size();
  shader_module_create_info.pCode =
      reinterpret_cast<const uint32_t*>(translated_binary().data());

  if (vulkan_device->functions().vkCreateShaderModule(
          vulkan_device->device(), &shader_module_create_info, nullptr,
          &shader_module_) != VK_SUCCESS) {
    XELOGE(
        "VulkanShader::VulkanTranslation: Failed to create a Vulkan shader "
        "module for shader {:016X} modification {:016X}",
        shader().ucode_data_hash(), modification());
    MakeInvalid();
    return VK_NULL_HANDLE;
  }

  return shader_module_;
}

VulkanShader::VulkanShader(const ui::vulkan::VulkanDevice* const vulkan_device,
                           const xenos::ShaderType shader_type,
                           const uint64_t ucode_data_hash,
                           const uint32_t* const ucode_dwords,
                           const size_t ucode_dword_count,
                           const std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian),
      vulkan_device_(vulkan_device) {
  assert_not_null(vulkan_device);
}

// hakuX-Boosted: see the declaration.
const ucode_program::Program& VulkanShader::GetUcodeProgram() {
  if (!ucode_program_valid_) {
    ucode_program_valid_ = true;
    ucode_program::Build(type(), ucode_dwords(), uint32_t(ucode_dword_count()),
                         ucode_program_);
  }
  return ucode_program_;
}

// hakuX-Boosted: see the declaration.
const VulkanShader::StandInInfo& VulkanShader::GetStandInInfo() {
  if (stand_in_info_valid_ || !is_ucode_analyzed()) {
    return stand_in_info_;
  }
  stand_in_info_valid_ = true;

  const bool is_pixel = type() == xenos::ShaderType::kPixel;
  const uint32_t* const ucode = ucode_dwords();
  const size_t dword_count = ucode_dword_count();

  constexpr uint32_t kRegisterCount = 64;
  // Registers written so far. A pixel shader's registers [0-15] hold the
  // interpolators until they are written.
  uint64_t written = 0;
  // Pixel shaders: for a written register, the interpolator its components
  // were computed from by a per-component operation (-1 if none), and which
  // component of that interpolator each of its X, Y and Z came from.
  int8_t derived_from[kRegisterCount];
  uint8_t derived_swizzle[kRegisterCount];
  std::memset(derived_from, -1, sizeof(derived_from));
  std::memset(derived_swizzle, 0, sizeof(derived_swizzle));
  // Vertex shaders: registers holding a vertex colour attribute.
  uint64_t color_registers = 0;
  // Candidates that read a computed register, appended after the direct ones.
  StandInTexture derived_textures[StandInInfo::kMaxTextures];
  uint32_t derived_texture_count = 0;

  constexpr uint8_t kIdentitySwizzle = 0 | (1 << 2) | (2 << 4);
  auto swizzle_component = [](SwizzleSource source) -> uint32_t {
    // Constants 0 and 1 cannot be coordinates: read X instead.
    return source <= SwizzleSource::kW ? uint32_t(source) : 0;
  };
  // If the operand is an interpolator, or a register computed from one,
  // returns which, and the swizzle from the interpolator to the operand's
  // first three components.
  auto operand_interpolator = [&](const InstructionOperand& operand,
                                  uint8_t& swizzle_out, bool& direct_out) {
    if (operand.storage_source != InstructionStorageSource::kRegister ||
        operand.storage_addressing_mode !=
            InstructionStorageAddressingMode::kAbsolute ||
        operand.storage_index >= kRegisterCount) {
      return -1;
    }
    uint32_t index = operand.storage_index;
    int interpolator;
    uint8_t base_swizzle;
    if (!(written & (uint64_t(1) << index))) {
      if (index >= xenos::kMaxInterpolators) {
        return -1;
      }
      interpolator = int(index);
      base_swizzle = kIdentitySwizzle;
      direct_out = true;
    } else {
      interpolator = derived_from[index];
      if (interpolator < 0) {
        return -1;
      }
      base_swizzle = derived_swizzle[index];
      direct_out = false;
    }
    uint8_t swizzle = 0;
    for (uint32_t i = 0; i < 3; ++i) {
      uint32_t component = swizzle_component(operand.GetComponent(i));
      // Z of a computed register is only tracked for X, Y and Z sources.
      uint32_t source =
          component < 3 ? (base_swizzle >> (2 * component)) & 3 : component;
      swizzle |= uint8_t(source << (2 * i));
    }
    swizzle_out = swizzle;
    return interpolator;
  };
  auto register_written = [&](const InstructionResult& result) -> bool {
    return result.storage_target == InstructionStorageTarget::kRegister &&
           result.storage_addressing_mode ==
               InstructionStorageAddressingMode::kAbsolute &&
           result.storage_index < kRegisterCount && result.GetUsedWriteMask();
  };

  ucode::VertexFetchInstruction previous_vfetch_full;
  std::memset(&previous_vfetch_full, 0, sizeof(previous_vfetch_full));

  for (uint32_t i = 0; i < cf_pair_index_bound(); ++i) {
    if (size_t(i + 1) * 3 > dword_count) {
      break;
    }
    ucode::ControlFlowInstruction cf_ab[2];
    ucode::UnpackControlFlowInstructions(ucode + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      const ucode::ControlFlowInstruction& cf = cf_ab[j];
      // Conditional and predicated blocks are walked as if they ran: this is
      // a guess at what the shader mostly does, not a translation.
      ParsedExecInstruction exec;
      switch (cf.opcode()) {
        case ucode::ControlFlowOpcode::kExec:
        case ucode::ControlFlowOpcode::kExecEnd:
          ParseControlFlowExec(cf.exec, i * 2 + j, exec);
          break;
        case ucode::ControlFlowOpcode::kCondExec:
        case ucode::ControlFlowOpcode::kCondExecEnd:
        case ucode::ControlFlowOpcode::kCondExecPredClean:
        case ucode::ControlFlowOpcode::kCondExecPredCleanEnd:
          ParseControlFlowCondExec(cf.cond_exec, i * 2 + j, exec);
          break;
        case ucode::ControlFlowOpcode::kCondExecPred:
        case ucode::ControlFlowOpcode::kCondExecPredEnd:
          ParseControlFlowCondExecPred(cf.cond_exec_pred, i * 2 + j, exec);
          break;
        default:
          continue;
      }
      uint32_t sequence = exec.sequence;
      for (uint32_t instr_offset = exec.instruction_address;
           instr_offset < exec.instruction_address + exec.instruction_count;
           ++instr_offset, sequence >>= 2) {
        if (size_t(instr_offset + 1) * 3 > dword_count) {
          break;
        }
        const uint32_t* op_ptr = ucode + instr_offset * 3;
        if (sequence & 0b01) {
          const auto& op =
              *reinterpret_cast<const ucode::FetchInstruction*>(op_ptr);
          if (op.opcode() == ucode::FetchOpcode::kVertexFetch) {
            ParsedVertexFetchInstruction fetch;
            if (ParseVertexFetchInstruction(op.vertex_fetch(),
                                            previous_vfetch_full, fetch)) {
              previous_vfetch_full = op.vertex_fetch();
            }
            if (register_written(fetch.result)) {
              uint64_t bit = uint64_t(1) << fetch.result.storage_index;
              written |= bit;
              derived_from[fetch.result.storage_index] = -1;
              bool is_color =
                  !is_pixel &&
                  fetch.attributes.data_format ==
                      xenos::VertexFormat::k_8_8_8_8 &&
                  !fetch.attributes.is_integer && !fetch.attributes.is_signed &&
                  (fetch.result.GetUsedWriteMask() & 0b0111) == 0b0111;
              if (is_color) {
                color_registers |= bit;
              } else {
                color_registers &= ~bit;
              }
            }
          } else {
            ParsedTextureFetchInstruction fetch;
            ParseTextureFetchInstruction(op.texture_fetch(), fetch);
            if (is_pixel && op.opcode() == ucode::FetchOpcode::kTextureFetch &&
                fetch.operand_count == 2 &&
                fetch.dimension != xenos::FetchOpDimension::k3DOrStacked) {
              uint8_t swizzle = 0;
              bool direct = false;
              int interpolator =
                  operand_interpolator(fetch.operands[0], swizzle, direct);
              uint32_t fetch_constant = fetch.operands[1].storage_index & 31;
              bool known = false;
              for (uint32_t k = 0; k < stand_in_info_.texture_count; ++k) {
                known |= stand_in_info_.textures[k].fetch_constant ==
                         fetch_constant;
              }
              for (uint32_t k = 0; k < derived_texture_count; ++k) {
                known |= derived_textures[k].fetch_constant == fetch_constant;
              }
              if (interpolator >= 0 && !known) {
                StandInTexture candidate;
                candidate.fetch_constant = uint8_t(fetch_constant);
                candidate.interpolator = uint8_t(interpolator);
                candidate.swizzle = swizzle;
                candidate.dimension = fetch.dimension;
                candidate.unnormalized =
                    fetch.attributes.unnormalized_coordinates;
                candidate.direct = direct;
                if (direct) {
                  if (stand_in_info_.texture_count <
                      StandInInfo::kMaxTextures) {
                    stand_in_info_.textures[stand_in_info_.texture_count++] =
                        candidate;
                  }
                } else if (derived_texture_count < StandInInfo::kMaxTextures) {
                  derived_textures[derived_texture_count++] = candidate;
                }
              }
            }
            if (fetch.has_result() && register_written(fetch.result)) {
              uint64_t bit = uint64_t(1) << fetch.result.storage_index;
              written |= bit;
              derived_from[fetch.result.storage_index] = -1;
              color_registers &= ~bit;
            }
          }
        } else {
          const auto& op =
              *reinterpret_cast<const ucode::AluInstruction*>(op_ptr);
          ParsedAluInstruction alu;
          ParseAluInstruction(op, type(), alu);
          const InstructionResult& vector_result =
              alu.vector_and_constant_result;
          // Everything is read before anything is written.
          // Is the vector result a copy of a colour register, or one times a
          // constant?
          bool vector_is_color = false;
          if (!is_pixel && alu.vector_operand_count >= 2) {
            const InstructionOperand& a = alu.vector_operands[0];
            const InstructionOperand& b = alu.vector_operands[1];
            auto is_color_register = [&](const InstructionOperand& operand) {
              return operand.storage_source ==
                         InstructionStorageSource::kRegister &&
                     operand.storage_addressing_mode ==
                         InstructionStorageAddressingMode::kAbsolute &&
                     operand.storage_index < kRegisterCount &&
                     (color_registers &
                      (uint64_t(1) << operand.storage_index)) != 0;
            };
            switch (alu.vector_opcode) {
              case ucode::AluVectorOpcode::kMax:
              case ucode::AluVectorOpcode::kMin:
                // How the console's compiler writes a move.
                vector_is_color = is_color_register(a) &&
                                  is_color_register(b) &&
                                  a.storage_index == b.storage_index;
                break;
              case ucode::AluVectorOpcode::kMul:
                vector_is_color =
                    (is_color_register(a) &&
                     b.storage_source ==
                         InstructionStorageSource::kConstantFloat) ||
                    (is_color_register(b) &&
                     a.storage_source ==
                         InstructionStorageSource::kConstantFloat);
                break;
              default:
                break;
            }
            // Red, green and blue must all come from the operation.
            vector_is_color =
                vector_is_color &&
                (vector_result.GetUsedWriteMask() & 0b0111) == 0b0111 &&
                vector_result.components[0] <= SwizzleSource::kW &&
                vector_result.components[1] <= SwizzleSource::kW &&
                vector_result.components[2] <= SwizzleSource::kW;
          }
          // Is the vector result computed, component by component, from an
          // interpolator?
          int vector_interpolator = -1;
          uint8_t vector_swizzle = 0;
          if (is_pixel && register_written(vector_result) &&
              (vector_result.GetUsedWriteMask() & 0b0011) == 0b0011) {
            switch (alu.vector_opcode) {
              case ucode::AluVectorOpcode::kAdd:
              case ucode::AluVectorOpcode::kMul:
              case ucode::AluVectorOpcode::kMax:
              case ucode::AluVectorOpcode::kMin:
              case ucode::AluVectorOpcode::kMad:
                for (uint32_t k = 0;
                     k < alu.vector_operand_count && vector_interpolator < 0;
                     ++k) {
                  bool direct;
                  vector_interpolator = operand_interpolator(
                      alu.vector_operands[k], vector_swizzle, direct);
                }
                break;
              default:
                break;
            }
          }

          if (vector_result.storage_target ==
                  InstructionStorageTarget::kInterpolator &&
              vector_result.storage_index < xenos::kMaxInterpolators &&
              (vector_result.GetUsedWriteMask() & 0b0111)) {
            uint32_t bit = uint32_t(1) << vector_result.storage_index;
            if (vector_is_color) {
              stand_in_info_.color_interpolators |= bit;
            } else {
              stand_in_info_.color_interpolators &= ~bit;
            }
          }
          if (alu.scalar_result.storage_target ==
                  InstructionStorageTarget::kInterpolator &&
              alu.scalar_result.storage_index < xenos::kMaxInterpolators &&
              (alu.scalar_result.GetUsedWriteMask() & 0b0111)) {
            stand_in_info_.color_interpolators &=
                ~(uint32_t(1) << alu.scalar_result.storage_index);
          }
          if (register_written(vector_result)) {
            uint64_t bit = uint64_t(1) << vector_result.storage_index;
            written |= bit;
            derived_from[vector_result.storage_index] =
                int8_t(vector_interpolator);
            derived_swizzle[vector_result.storage_index] = vector_swizzle;
            if (vector_is_color) {
              color_registers |= bit;
            } else {
              color_registers &= ~bit;
            }
          }
          if (register_written(alu.scalar_result)) {
            uint64_t bit = uint64_t(1) << alu.scalar_result.storage_index;
            // A scalar written into a register the vector part of the same
            // instruction wrote leaves that part's tracking as it is only if
            // it misses X and Y.
            if (!(written & bit) ||
                (alu.scalar_result.GetUsedWriteMask() & 0b0011)) {
              derived_from[alu.scalar_result.storage_index] = -1;
            }
            written |= bit;
            if (alu.scalar_result.GetUsedWriteMask() & 0b0111) {
              color_registers &= ~bit;
            }
          }
        }
      }
    }
  }

  for (uint32_t k = 0; k < derived_texture_count &&
                       stand_in_info_.texture_count < StandInInfo::kMaxTextures;
       ++k) {
    stand_in_info_.textures[stand_in_info_.texture_count++] =
        derived_textures[k];
  }
  if (is_pixel) {
    stand_in_info_.color_interpolators = 0;
  }
  return stand_in_info_;
}

Shader::Translation* VulkanShader::CreateTranslationInstance(
    uint64_t modification) {
  return new VulkanTranslation(*this, modification);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
