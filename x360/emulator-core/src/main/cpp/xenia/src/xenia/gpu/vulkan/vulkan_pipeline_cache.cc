/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <set>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/shader_compile_counter.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/pipeline_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_builtin_geometry_shader.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/vulkan_util.h"

// Shader bytecode.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/adaptive_quad_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/adaptive_triangle_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/continuous_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/discrete_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/tessellation_adaptive_vs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/tessellation_indexed_vs.h"
// Placeholder pixel shader for pipeline hot-swap.
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_ps.h"
// Ucode interpreter VS placeholder + its debug color pixel shader.
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_color_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/ucode_interpreter_vs.h"
// hakuX-Boosted: the stand-in pixel shaders and the interpreter vertex shader
// that feeds them (vulkan_placeholder_ps_mode).
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_stand_in_2d_color_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_stand_in_2d_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_stand_in_cube_color_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_stand_in_cube_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_stand_in_flat_color_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/placeholder_stand_in_flat_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/ucode_interpreter_stand_in_vs.h"
// hakuX-Boosted: the guest shader interpreter, version 2.
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/ucode_program_vs.h"
}  // namespace shaders

DECLARE_bool(guest_threads_above_helpers);

DEFINE_bool(
    pipeline_storage_precreate, true,
    "Create the pipelines described by previous runs when a title starts, so "
    "they do not compile mid-gameplay. Off still loads and reuses the stored "
    "shaders.",
    "GPU");

DEFINE_int32(
    vulkan_pipeline_creation_threads, -1,
    "Number of threads used for graphics pipeline creation. -1 to calculate "
    "automatically (75% of logical CPU cores), a positive number to specify "
    "the number of threads explicitly (up to the number of logical CPU cores), "
    "0 to disable multithreaded pipeline creation.",
    "Vulkan");
DEFINE_bool(
    vulkan_async_skip_draws, false,
    "With asynchronous shader compilation: don't wait for pipeline creation "
    "at the submission boundary - draws whose pipeline hasn't finished "
    "compiling yet are dropped for that submission (brief pop-in of new "
    "objects instead of any wait). When false, the GPU thread waits at the "
    "end of each submission until every pipeline it recorded is ready, so no "
    "draw is ever lost.",
    "Vulkan");

// hakuX-Boosted: see RequirePipelineForSubmission.
DEFINE_bool(
    vulkan_async_wait_fullscreen_draws, true,
    "With vulkan_async_skip_draws: still wait, at the submission boundary, "
    "for the pipelines of full-screen passes (rectangle lists and unindexed "
    "draws of at most four vertices), because a dropped post-process or "
    "composite pass is the whole frame wrong while a dropped object draw is "
    "only late. Each such pipeline costs one wait, the first time it is "
    "drawn. Off: every draw of an unready pipeline is dropped.",
    "Vulkan");
DEFINE_int32(
    vulkan_async_wait_fullscreen_draws_ms, 1500,
    "The longest one submission waits for the pipelines "
    "vulkan_async_wait_fullscreen_draws names, in milliseconds; after it the "
    "draws are dropped as any other.",
    "Vulkan");

DEFINE_bool(
    vulkan_placeholder_pipelines, false,
    "Asynchronous shader compilation async model (A/B). Default false: the "
    "fork's deferred-bind model - no placeholder is created, the pipeline slot "
    "stays VK_NULL_HANDLE, the command processor records a deferred bind of the "
    "stable slot pointer, and the submission boundary either waits for creation "
    "(vulkan_async_skip_draws=false) or drops the draws of still-unready "
    "pipelines at replay. Preferred because the skipped-draw pop-in is smoother "
    "than the jarring artifacts produced by the placeholder pipeline. When true, "
    "upstream xenia-edge's model is used instead: an immediate placeholder "
    "pipeline (no-op pixel shader, or a ucode-interpreter vertex shader) is "
    "created and hot-swapped for the real pipeline when it finishes compiling - "
    "the draw renders through the placeholder meanwhile. Both share the "
    "async_shader_compilation master switch and the creation threads.",
    "Vulkan");

// hakuX-Boosted: the stand-in pixel shader and what else decides which draws
// get a placeholder. All three only matter with vulkan_placeholder_pipelines.
DEFINE_int32(
    vulkan_placeholder_ps_mode, 0,
    "With vulkan_placeholder_pipelines: what a placeholder pipeline draws "
    "while the real one compiles. 0: nothing but depth (the no-op pixel "
    "shader). 1: an approximation - the texture the guest pixel shader "
    "fetches first, by the interpolator that fetch reads, times the vertex "
    "colour if the vertex shader passes one through, alpha 1; without a "
    "usable texture the vertex colour or a mid grey. 2: as 1, but alpha is "
    "the texture's times the vertex colour's and the guest alpha test is "
    "applied, so blended and cut-out surfaces keep their shape. Depth test "
    "and write are the real pipeline's; depth is never written from the "
    "stand-in. Host render target path only.",
    "Vulkan");
DEFINE_bool(
    vulkan_placeholder_real_vs, true,
    "With vulkan_placeholder_pipelines: when the ucode interpreter cannot "
    "stand in for a vertex shader that is already translated, make the "
    "placeholder from the translated vertex shader. That is a driver compile "
    "of the vertex shader on the GPU command thread (a hitch as long as the "
    "compile). Off: such a draw has no placeholder and is dropped until its "
    "real pipeline is ready.",
    "Vulkan");
DEFINE_bool(
    vulkan_placeholder_interpreter_translated_vs, false,
    "With vulkan_placeholder_pipelines and async_shader_vs_interpreter: use "
    "the ucode interpreter for a new pipeline's placeholder even when its "
    "vertex shader is already translated (a known vertex shader with a new "
    "pixel shader or new state), instead of compiling a placeholder from the "
    "translated vertex shader on the GPU command thread.",
    "Vulkan");

DEFINE_int32(
    vulkan_placeholder_interpreter, 1,
    "With vulkan_placeholder_pipelines and async_shader_vs_interpreter: which "
    "interpreter draws a placeholder's vertices. 1: XenDroid's, which reads "
    "the microcode from guest memory (not a shader loaded inline, not one "
    "with loops). 2: the guest shader interpreter, version 2, which is fed a "
    "prepared copy of the microcode, follows the translated shader in "
    "arithmetic, control flow, loops and addressing, and so also stands in "
    "for inline shaders and shaders with loops. Read once at start-up.",
    "Vulkan");

DEFINE_bool(
    vulkan_placeholder_depth_only_interpreter, false,
    "With vulkan_placeholder_pipelines and async_shader_vs_interpreter: a "
    "draw without a pixel shader (a depth-only pass: shadow maps, depth "
    "pre-passes) whose vertex shader the ucode interpreter can run gets an "
    "interpreter placeholder and its real pipeline is compiled in the "
    "background. Off: such a pipeline is translated and compiled on the GPU "
    "command thread when first drawn, a hitch as long as the compile.",
    "Vulkan");
DEFINE_bool(
    vulkan_placeholder_interpreter_geometry, false,
    "With vulkan_placeholder_pipelines and async_shader_vs_interpreter: let "
    "the ucode interpreter stand in for the vertex shader of rectangle lists "
    "(how titles draw full-screen passes) and quad lists too, through a "
    "geometry shader built for what the interpreter outputs. Off: such a "
    "draw has no interpreter placeholder.",
    "Vulkan");

// hakuX-Boosted: covering more draws while a pipeline compiles, and bounding
// what a placeholder costs the GPU command thread
// (boosted/notes/shader-ahead.md). All only matter with
// vulkan_placeholder_pipelines; each default is what ran before.
DEFINE_bool(
    vulkan_placeholder_late, false,
    "With vulkan_placeholder_pipelines: a pipeline that could not be given a "
    "placeholder when it was first drawn (its vertex shader was not "
    "translated yet and the ucode interpreter could not stand in) gets one "
    "at a later draw, as soon as one can be made. Off: its draws are dropped "
    "until the real pipeline is ready.",
    "Vulkan");
DEFINE_bool(
    vulkan_placeholder_async, false,
    "With vulkan_placeholder_pipelines: create placeholder pipelines on the "
    "pipeline creation threads, ahead of every other job, instead of on the "
    "GPU command thread. The GPU command thread waits for one for at most "
    "what is left of vulkan_placeholder_async_wait_ms in the frame, and "
    "drops the draw if the placeholder is not there by then (it is at the "
    "next draw). Bounds what a slow placeholder costs a frame.",
    "Vulkan");
DEFINE_int32(
    vulkan_placeholder_async_wait_ms, 8,
    "With vulkan_placeholder_async: the most the GPU command thread waits "
    "for placeholders in one guest frame, in milliseconds, all of them "
    "together. 0: never waits (a new pipeline's first draws are dropped "
    "until its placeholder is there).",
    "Vulkan");
DEFINE_bool(
    vulkan_placeholder_translate_first, false,
    "With vulkan_placeholder_pipelines: when a draw gets no placeholder "
    "because its vertex shader is not translated yet, translate that vertex "
    "shader on a creation thread ahead of the queued pipelines, so that "
    "vulkan_placeholder_late can make a placeholder from it a frame or two "
    "later instead of when its turn in the queue comes.",
    "Vulkan");
DEFINE_int32(
    vulkan_placeholder_wait_fullscreen_ms, 0,
    "With vulkan_placeholder_pipelines: full-screen passes (rectangle lists "
    "and unindexed draws of at most four vertices) get no placeholder: the "
    "GPU command thread translates their shaders, has the real pipeline "
    "compiled ahead of everything else and waits for it, for at most this "
    "many milliseconds, once for each such pipeline. A post-process or "
    "composite pass drawn with a stand-in is the whole frame wrong; this "
    "trades that for one wait. 0: off, such draws are treated as any other.",
    "Vulkan");

// hakuX-Boosted: the bounds of shader_ahead_speculate (gpu_flags.cc).
DEFINE_int32(
    shader_ahead_max_outstanding, 2,
    "With shader_ahead_speculate: the most speculative pipelines queued or "
    "compiling at once. A prediction made while that many are outstanding "
    "is dropped.",
    "GPU");
DEFINE_int32(
    shader_ahead_max_unused, 16,
    "With shader_ahead_speculate: the most speculative pipelines no draw has "
    "used yet. No new one is made while that many are waiting to be used or "
    "written off.",
    "GPU");
DEFINE_int32(
    shader_ahead_max_wasted, 48,
    "With shader_ahead_speculate: once this many speculative pipelines have "
    "been written off as never used, no more are made in this run.",
    "GPU");
DEFINE_int32(
    shader_ahead_write_off_s, 20,
    "With shader_ahead_speculate: a speculative pipeline no draw has used "
    "this many seconds after it was predicted is written off as wasted.",
    "GPU");
DEFINE_int32(
    shader_ahead_templates, 2,
    "With shader_ahead_speculate: for a pair of shaders seen ahead, how many "
    "of the most recent pipeline states of its vertex (or pixel) shader a "
    "speculative pipeline is made for, and how many sibling states are "
    "predicted from one new pipeline.",
    "GPU");
DEFINE_int32(
    shader_ahead_threads, 1,
    "With shader_ahead_speculate: how many pipeline creation threads may "
    "compile speculative pipelines at the same time. A speculative compile "
    "cannot be interrupted, so each thread on one is a thread a real "
    "pipeline may have to wait for.",
    "GPU");

DEFINE_bool(
    vulkan_dynamic_pipeline_state, true,
    "Use VK_EXT_extended_dynamic_state (and state 2/3 where available) to move "
    "cull/front-face/topology/depth/stencil/blend out of the baked pipeline "
    "key into dynamic state, collapsing pipeline permutations and reducing "
    "shader compilation stutter. Disable to fall back to fully static "
    "pipelines.",
    "Vulkan");

DECLARE_bool(vulkan_dynamic_rendering);
DECLARE_bool(spirv_disable_rounding_mode_rte);
DECLARE_bool(precise_interpolation);

namespace xe {
namespace gpu {
namespace vulkan {

// hakuX-Boosted: the stand-in pixel shaders as compiled, by
// [VulkanPipelineCache::StandInPixelShader - 1][has a colour input].
namespace {

struct StandInPixelShaderCode {
  const uint32_t* words;
  size_t size_bytes;
};
const StandInPixelShaderCode kStandInPixelShaderCode[3][2] = {
    {{shaders::placeholder_stand_in_flat_ps,
      sizeof(shaders::placeholder_stand_in_flat_ps)},
     {shaders::placeholder_stand_in_flat_color_ps,
      sizeof(shaders::placeholder_stand_in_flat_color_ps)}},
    {{shaders::placeholder_stand_in_2d_ps,
      sizeof(shaders::placeholder_stand_in_2d_ps)},
     {shaders::placeholder_stand_in_2d_color_ps,
      sizeof(shaders::placeholder_stand_in_2d_color_ps)}},
    {{shaders::placeholder_stand_in_cube_ps,
      sizeof(shaders::placeholder_stand_in_cube_ps)},
     {shaders::placeholder_stand_in_cube_color_ps,
      sizeof(shaders::placeholder_stand_in_cube_color_ps)}},
};

// Moves the two inputs of a stand-in pixel shader (the texture coordinate,
// compiled at location 0, and the vertex colour, at location 1) to the
// locations a translated guest vertex shader writes those interpolators to.
// The module declares no other input with a location, and a variant without
// one of the two simply has no decoration to rewrite. False, with the words
// possibly half rewritten, if the module is not what was expected.
bool MoveStandInPixelShaderInputs(std::vector<uint32_t>& words,
                                  uint32_t texcoord_location,
                                  uint32_t color_location) {
  constexpr size_t kHeaderWords = 5;
  if (words.size() < kHeaderWords || words[0] != uint32_t(spv::MagicNumber)) {
    return false;
  }
  // The decorations come before the variables they decorate: first find which
  // ids are inputs.
  std::vector<uint32_t> input_ids;
  for (size_t i = kHeaderWords; i < words.size();) {
    uint32_t word_count = words[i] >> 16;
    uint32_t opcode = words[i] & 0xFFFF;
    if (!word_count || i + word_count > words.size()) {
      return false;
    }
    if (opcode == uint32_t(spv::OpVariable) && word_count >= 4 &&
        words[i + 3] == uint32_t(spv::StorageClassInput)) {
      input_ids.push_back(words[i + 2]);
    }
    i += word_count;
  }
  for (size_t i = kHeaderWords; i < words.size();) {
    uint32_t word_count = words[i] >> 16;
    uint32_t opcode = words[i] & 0xFFFF;
    if (opcode == uint32_t(spv::OpDecorate) && word_count >= 4 &&
        words[i + 2] == uint32_t(spv::DecorationLocation) &&
        std::find(input_ids.begin(), input_ids.end(), words[i + 1]) !=
            input_ids.end()) {
      uint32_t& location = words[i + 3];
      if (location == 0) {
        location = texcoord_location;
      } else if (location == 1) {
        location = color_location;
      } else {
        return false;
      }
    }
    i += word_count;
  }
  return true;
}

}  // namespace

VulkanPipelineCache::VulkanPipelineCache(
    VulkanCommandProcessor& command_processor,
    const RegisterFile& register_file,
    VulkanRenderTargetCache& render_target_cache,
    VkShaderStageFlags guest_shader_vertex_stages)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      guest_shader_vertex_stages_(guest_shader_vertex_stages),
      guest_shader_cache_(*this, register_file, render_target_cache) {}

VulkanPipelineCache::~VulkanPipelineCache() { Shutdown(); }

std::unique_ptr<SpirvShaderTranslator> VulkanPipelineCache::CreateTranslator()
    const {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  return std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(vulkan_device),
      render_target_cache_.msaa_2x_attachments_supported(),
      render_target_cache_.msaa_2x_no_attachments_supported(),
      edram_fragment_shader_interlock,
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y());
}

bool VulkanPipelineCache::precise_interpolation_supported() const {
  // The translator gates emission on the device barycentric feature, so the
  // cvar is the only extra control here.
  return cvars::precise_interpolation;
}

bool VulkanPipelineCache::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();

  // Cache device features for geometry shader creation.
  const SpirvShaderTranslator::Features features(vulkan_device);
  spirv_version_ = features.spirv_version;
  signed_zero_inf_nan_preserve_float32_ =
      features.signed_zero_inf_nan_preserve_float32;
  denorm_flush_to_zero_float32_ = features.denorm_flush_to_zero_float32;
  rounding_mode_rte_float32_ = features.rounding_mode_rte_float32 &&
                               !cvars::spirv_disable_rounding_mode_rte;

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;

  // Resolve the extended-dynamic-state capabilities once. The fragment shader
  // interlock (FSI) render path configures depth / stencil / color blend in the
  // shader rather than via fixed-function state, so the dynamic state would
  // have no effect there - disable the feature entirely on that path so the
  // baked key and the dynamic emission can never disagree.
  {
    const ui::vulkan::VulkanDevice::Properties& device_properties =
        vulkan_device->properties();
    DynamicStateCapabilities& caps = dynamic_state_capabilities_;
    caps = DynamicStateCapabilities();
    if (cvars::vulkan_dynamic_pipeline_state &&
        !edram_fragment_shader_interlock &&
        device_properties.extendedDynamicState) {
      caps.extended_dynamic_state = true;
      caps.depth_clamp_enable =
          device_properties.extendedDynamicState3DepthClampEnable;
      caps.polygon_mode = device_properties.extendedDynamicState3PolygonMode;
      caps.color_blend_enable =
          device_properties.extendedDynamicState3ColorBlendEnable;
      caps.color_blend_equation =
          device_properties.extendedDynamicState3ColorBlendEquation;
      caps.color_write_mask =
          device_properties.extendedDynamicState3ColorWriteMask;
      caps.primitive_topology_unrestricted =
          device_properties
              .extendedDynamicState3PrimitiveTopologyUnrestricted;
    }
  }

  if (!guest_shader_cache_.Initialize()) {
    return false;
  }

  if (edram_fragment_shader_interlock) {
    std::vector<uint8_t> depth_only_fragment_shader_code =
        guest_shader_cache_.translator().CreateDepthOnlyFragmentShader();
    depth_only_fragment_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device,
        reinterpret_cast<const uint32_t*>(
            depth_only_fragment_shader_code.data()),
        depth_only_fragment_shader_code.size());
    if (depth_only_fragment_shader_ == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanPipelineCache: Failed to create the depth/stencil-only "
          "fragment shader for the fragment shader interlock render backend "
          "implementation");
      return false;
    }
  }

  // Substitute fragment shaders for guest depth-only draws when in-PS float24
  // conversion is active - keep the depth buffer's encoding consistent with
  // PS-converted draws (matches the DXBC backend's
  // float24_{truncate,round}_ps).
  if (render_target_cache_.depth_float24_convert_in_pixel_shader()) {
    using DepthStencilMode =
        SpirvShaderTranslator::Modification::DepthStencilMode;
    auto build = [&](DepthStencilMode mode, VkShaderModule& out) -> bool {
      std::vector<uint8_t> code =
          guest_shader_cache_.translator().CreateDepthOnlyFragmentShader(mode);
      out = ui::vulkan::util::CreateShaderModule(
          vulkan_device, reinterpret_cast<const uint32_t*>(code.data()),
          code.size());
      return out != VK_NULL_HANDLE;
    };
    if (!build(DepthStencilMode::kFloat24Truncating,
               float24_truncate_fragment_shader_) ||
        !build(DepthStencilMode::kFloat24Rounding,
               float24_round_fragment_shader_)) {
      XELOGE(
          "VulkanPipelineCache: Failed to create the float24 substitute "
          "depth-only fragment shaders");
      return false;
    }
  }

  // Create tessellation shaders if tessellation is supported.
  if (vulkan_device->properties().tessellationShader) {
    // Vertex shaders for tessellation.
    tessellation_indexed_vs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::tessellation_indexed_vs,
        sizeof(shaders::tessellation_indexed_vs));
    tessellation_adaptive_vs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::tessellation_adaptive_vs,
        sizeof(shaders::tessellation_adaptive_vs));
    // Discrete mode hull shaders.
    discrete_triangle_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_triangle_1cp_hs,
        sizeof(shaders::discrete_triangle_1cp_hs));
    discrete_triangle_3cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_triangle_3cp_hs,
        sizeof(shaders::discrete_triangle_3cp_hs));
    discrete_quad_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_quad_1cp_hs,
        sizeof(shaders::discrete_quad_1cp_hs));
    discrete_quad_4cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::discrete_quad_4cp_hs,
        sizeof(shaders::discrete_quad_4cp_hs));
    // Continuous mode hull shaders.
    continuous_triangle_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_triangle_1cp_hs,
        sizeof(shaders::continuous_triangle_1cp_hs));
    continuous_triangle_3cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_triangle_3cp_hs,
        sizeof(shaders::continuous_triangle_3cp_hs));
    continuous_quad_1cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_quad_1cp_hs,
        sizeof(shaders::continuous_quad_1cp_hs));
    continuous_quad_4cp_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::continuous_quad_4cp_hs,
        sizeof(shaders::continuous_quad_4cp_hs));
    // Adaptive mode hull shaders.
    adaptive_triangle_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::adaptive_triangle_hs,
        sizeof(shaders::adaptive_triangle_hs));
    adaptive_quad_hs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::adaptive_quad_hs,
        sizeof(shaders::adaptive_quad_hs));

    // Verify all tessellation shaders were created successfully.
    if (tessellation_indexed_vs_ == VK_NULL_HANDLE ||
        tessellation_adaptive_vs_ == VK_NULL_HANDLE ||
        discrete_triangle_1cp_hs_ == VK_NULL_HANDLE ||
        discrete_triangle_3cp_hs_ == VK_NULL_HANDLE ||
        discrete_quad_1cp_hs_ == VK_NULL_HANDLE ||
        discrete_quad_4cp_hs_ == VK_NULL_HANDLE ||
        continuous_triangle_1cp_hs_ == VK_NULL_HANDLE ||
        continuous_triangle_3cp_hs_ == VK_NULL_HANDLE ||
        continuous_quad_1cp_hs_ == VK_NULL_HANDLE ||
        continuous_quad_4cp_hs_ == VK_NULL_HANDLE ||
        adaptive_triangle_hs_ == VK_NULL_HANDLE ||
        adaptive_quad_hs_ == VK_NULL_HANDLE) {
      XELOGW(
          "VulkanPipelineCache: Failed to create one or more tessellation "
          "shaders - tessellation will not be available");
    }
  }

  // Create placeholder pixel shader for pipeline hot-swap (stutter reduction).
  placeholder_pixel_shader_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::placeholder_ps, sizeof(shaders::placeholder_ps));
  if (placeholder_pixel_shader_ == VK_NULL_HANDLE) {
    XELOGW(
        "VulkanPipelineCache: Failed to create placeholder pixel shader - "
        "pipeline hot-swap will not be available");
  }

  // Create the ucode interpreter VS placeholder (and its debug color PS). If it
  // fails, the interpreter is simply not used - the normal placeholder path
  // still applies.
  ucode_interpreter_vs_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::ucode_interpreter_vs,
      sizeof(shaders::ucode_interpreter_vs));
  if (ucode_interpreter_vs_ == VK_NULL_HANDLE) {
    XELOGW(
        "VulkanPipelineCache: Failed to create the ucode interpreter vertex "
        "shader - the VS interpreter placeholder will not be available");
  }
  placeholder_color_pixel_shader_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::placeholder_color_ps,
      sizeof(shaders::placeholder_color_ps));

  // hakuX-Boosted: the guest shader interpreter, version 2, only when it is
  // asked for. A failure leaves version 1 in use.
  interpreter_program_ = false;
  if (cvars::vulkan_placeholder_interpreter == 2) {
    ucode_program_vs_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::ucode_program_vs,
        sizeof(shaders::ucode_program_vs));
    if (ucode_program_vs_ != VK_NULL_HANDLE) {
      interpreter_program_ = true;
      XELOGI(
          "VulkanPipelineCache: placeholders use the guest shader "
          "interpreter, version 2 (vulkan_placeholder_interpreter = 2), {} "
          "bytes of SPIR-V",
          sizeof(shaders::ucode_program_vs));
    } else {
      XELOGW(
          "VulkanPipelineCache: Failed to create the vertex shader of the "
          "guest shader interpreter, version 2 - version 1 is used");
    }
  }

  // hakuX-Boosted: the stand-in pixel shaders and their interpreter vertex
  // shader. Made whatever vulkan_placeholder_ps_mode says (it can be changed
  // while running); a failure only means no stand-in.
  ucode_interpreter_stand_in_vs_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::ucode_interpreter_stand_in_vs,
      sizeof(shaders::ucode_interpreter_stand_in_vs));
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t j = 0; j < 2; ++j) {
      const StandInPixelShaderCode& code = kStandInPixelShaderCode[i][j];
      stand_in_pixel_shaders_[i][j] = ui::vulkan::util::CreateShaderModule(
          vulkan_device, code.words, code.size_bytes);
      if (stand_in_pixel_shaders_[i][j] == VK_NULL_HANDLE) {
        XELOGW(
            "VulkanPipelineCache: Failed to create a stand-in pixel shader - "
            "placeholders that need it will draw nothing");
      }
    }
  }

  // Create Vulkan pipeline cache for faster pipeline creation.
  VkPipelineCacheCreateInfo pipeline_cache_create_info = {};
  pipeline_cache_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  if (vulkan_device->functions().vkCreatePipelineCache(
          vulkan_device->device(), &pipeline_cache_create_info, nullptr,
          &vk_pipeline_cache_) != VK_SUCCESS) {
    XELOGW("VulkanPipelineCache: Failed to create pipeline cache");
    vk_pipeline_cache_ = VK_NULL_HANDLE;
  }

  uint32_t logical_processor_count = xe::threading::logical_processor_count();
  if (!logical_processor_count) {
    // Pick some reasonable amount if couldn't determine the number of cores.
    logical_processor_count = 6;
  }
  creation_completion_event_ =
      xe::threading::Event::CreateManualResetEvent(true);
  assert_not_null(creation_completion_event_);
  if (cvars::vulkan_pipeline_creation_threads != 0) {
    size_t creation_thread_count;
    if (cvars::vulkan_pipeline_creation_threads < 0) {
      creation_thread_count =
          std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count =
          std::min(uint32_t(cvars::vulkan_pipeline_creation_threads),
                   logical_processor_count);
    }
    creation_threads_shutdown_ = false;
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<xe::threading::Thread> creation_thread =
          xe::threading::Thread::Create({}, [this]() { CreationThread(); });
      assert_not_null(creation_thread);
      creation_thread->set_name("Vulkan Pipelines");
      // hakuX-Boosted: below the guest's threads, see
      // guest_threads_above_helpers in kernel/guest_scheduler.cc.
      if (cvars::guest_threads_above_helpers) {
        creation_thread->set_priority(
            xe::threading::ThreadPriority::kBelowNormal);
      }
      creation_threads_.push_back(std::move(creation_thread));
    }
  }

  return true;
}

void VulkanPipelineCache::Shutdown() {
  // Shut down shader storage first.
  ShutdownShaderStorage();

  // Shut down all threads, before destroying the pipelines since they may be
  // creating them.
  if (!creation_threads_.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_threads_shutdown_ = true;
    }
    creation_request_cond_.notify_all();
    for (size_t i = 0; i < creation_threads_.size(); ++i) {
      xe::threading::Wait(creation_threads_[i].get(), false);
    }
    creation_threads_.clear();
  }
  // Clear any pending completion callback (may capture 'this') and reset
  // startup state.
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    creation_completion_callback_ = nullptr;
  }
  startup_loading_ = false;
  creation_completion_event_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Process any remaining deferred destructions (force destroy all since
  // device should be idle at shutdown).
  {
    std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
    for (const auto& pipeline_pair : deferred_destroy_pipelines_) {
      if (pipeline_pair.first != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, pipeline_pair.first, nullptr);
      }
    }
    deferred_destroy_pipelines_.clear();
  }

  // Destroy all pipelines.
  last_pipeline_ = nullptr;
  for (const auto& pipeline_pair : pipelines_) {
    if (pipeline_pair.second.pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline_pair.second.pipeline, nullptr);
    }
  }
  pipelines_.clear();

  // Destroy the pipeline cache.
  if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineCache(device, vk_pipeline_cache_, nullptr);
    vk_pipeline_cache_ = VK_NULL_HANDLE;
  }

  // Destroy all internal shaders.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         depth_only_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         float24_truncate_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         float24_round_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         placeholder_pixel_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         ucode_interpreter_vs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         placeholder_color_pixel_shader_);
  // hakuX-Boosted: the stand-in's modules.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         ucode_interpreter_stand_in_vs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         ucode_program_vs_);
  interpreter_program_ = false;
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t j = 0; j < 2; ++j) {
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                             stand_in_pixel_shaders_[i][j]);
    }
  }
  for (const auto& stand_in_pair : stand_in_pixel_shaders_moved_) {
    if (stand_in_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, stand_in_pair.second, nullptr);
    }
  }
  stand_in_pixel_shaders_moved_.clear();
  // Destroy tessellation shaders.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         tessellation_indexed_vs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         tessellation_adaptive_vs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_triangle_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_triangle_3cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_quad_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         discrete_quad_4cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_triangle_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_triangle_3cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_quad_1cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         continuous_quad_4cp_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         adaptive_triangle_hs_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         adaptive_quad_hs_);
  for (const auto& geometry_shader_pair : geometry_shaders_) {
    if (geometry_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, geometry_shader_pair.second, nullptr);
    }
  }
  geometry_shaders_.clear();

  // Destroy all translated shaders.
  for (auto it : shaders_) {
    delete it.second;
  }
  shaders_.clear();
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();

  // Shut down shader translation.
  guest_shader_cache_.Shutdown();
}

VulkanShader* VulkanPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                              const uint32_t* host_address,
                                              uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  uint64_t data_hash =
      XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }
  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  VulkanShader* shader =
      new VulkanShader(command_processor_.GetVulkanDevice(), shader_type,
                       data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

SpirvShaderTranslator::Modification
VulkanPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  return SpirvShaderTranslator::Modification(
      guest_shader_cache_.GetVertexShaderModification(
          shader, host_vertex_shader_type, interpolator_mask,
          ps_param_gen_used));
}

SpirvShaderTranslator::Modification
VulkanPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, bool apply_polygon_offset_in_shader) const {
  return SpirvShaderTranslator::Modification(
      guest_shader_cache_.GetPixelShaderModification(
          shader, interpolator_mask, param_gen_pos, normalized_depth_control,
          normalized_color_mask, apply_polygon_offset_in_shader));
}

bool VulkanPipelineCache::EnsureShadersTranslated(
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader,
    SpirvShaderTranslator* translator) {
  // Edge flags are not supported yet (because polygon primitives are not).
  assert_true(register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
              register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  assert_false(register_file_.Get<reg::SQ_PROGRAM_CNTL>().gen_index_vtx);
  // The shared translator is single-threaded, so background (creation-thread)
  // callers pass their own worker translator. Runtime translation always claims
  // the translation so it happens once even if draw and creation threads race
  // the same modification. The ucode interpreter defers translation entirely by
  // not calling this on the draw thread - the creation thread does it here.
  SpirvShaderTranslator& used_translator =
      translator ? *translator : guest_shader_cache_.translator();
  // hakuX-Boosted: what the GPU command thread itself had to translate, for
  // the once-a-second line of shader_ahead (LogStatsIfDue).
  struct DrawThreadTranslationTimer {
    VulkanPipelineCache* cache = nullptr;
    std::chrono::steady_clock::time_point start;
    ~DrawThreadTranslationTimer() {
      if (cache) {
        ++cache->draw_thread_translations_;
        cache->draw_thread_translation_ms_ +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start)
                .count();
      }
    }
  } draw_thread_translation_timer;
  if (!translator &&
      (!vertex_shader->is_translated() ||
       (pixel_shader != nullptr && !pixel_shader->is_translated()))) {
    draw_thread_translation_timer.cache = this;
    draw_thread_translation_timer.start = std::chrono::steady_clock::now();
  }
  if (!vertex_shader->is_translated()) {
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
    if (!TranslateAnalyzedShader(used_translator, *vertex_shader,
                                 /*use_try_claim=*/true)) {
      XELOGE("Failed to translate the vertex shader!");
      return false;
    }
  }
  if (!vertex_shader->is_valid()) {
    // Translation attempted previously, but not valid.
    return false;
  }
  if (pixel_shader != nullptr) {
    if (!pixel_shader->is_translated()) {
      pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
      if (!TranslateAnalyzedShader(used_translator, *pixel_shader,
                                   /*use_try_claim=*/true)) {
        XELOGE("Failed to translate the pixel shader!");
        return false;
      }
    }
    if (!pixel_shader->is_valid()) {
      // Translation attempted previously, but not valid.
      return false;
    }
  }
  return true;
}

bool VulkanPipelineCache::CanCreatePipelineAsync(bool has_pixel_shader,
                                                 bool use_interpreter) const {
  // hakuX-Boosted: a draw without a pixel shader has nothing to stand in for
  // but its vertex shader, so with the interpreter its placeholder is exact
  // (vulkan_placeholder_depth_only_interpreter).
  return cvars::async_shader_compilation && !creation_threads_.empty() &&
         (has_pixel_shader ||
          (use_interpreter && cvars::vulkan_placeholder_pipelines &&
           cvars::vulkan_placeholder_depth_only_interpreter &&
           GetInterpreterVertexShader(false) != VK_NULL_HANDLE)) &&
         placeholder_pixel_shader_ != VK_NULL_HANDLE;
}

const VulkanPipelineCache::PipelineLayoutProvider*
VulkanPipelineCache::GetGuestGraphicsPipelineLayout(
    const VulkanShader::VulkanTranslation* vertex_shader,
    const VulkanShader::VulkanTranslation* pixel_shader,
    bool* counts_complete_out) {
  // Binding counts come from translation. A shader whose bindings aren't ready
  // yet (being translated on a creation thread) reports 0 - reading its binding
  // vectors here would race the creation thread populating them. That yields
  // the minimal layout, and counts_complete_out reports false so the caller
  // can mark the layout for a later upgrade. bindings_ready() is monotonic, so
  // a false read can only under-report (never claim real counts it didn't
  // use).
  bool counts_complete = true;
  auto texture_count =
      [&counts_complete](
          const VulkanShader::VulkanTranslation* translation) -> size_t {
    const VulkanShader& shader =
        static_cast<const VulkanShader&>(translation->shader());
    if (!shader.bindings_ready()) {
      counts_complete = false;
      return 0;
    }
    return shader.GetTextureBindingsAfterTranslation().size();
  };
  auto sampler_count =
      [&counts_complete](
          const VulkanShader::VulkanTranslation* translation) -> size_t {
    const VulkanShader& shader =
        static_cast<const VulkanShader&>(translation->shader());
    if (!shader.bindings_ready()) {
      counts_complete = false;
      return 0;
    }
    return shader.GetSamplerBindingsAfterTranslation().size();
  };
  const PipelineLayoutProvider* layout = command_processor_.GetPipelineLayout(
      pixel_shader ? texture_count(pixel_shader) : 0,
      pixel_shader ? sampler_count(pixel_shader) : 0,
      texture_count(vertex_shader), sampler_count(vertex_shader));
  if (counts_complete_out) {
    *counts_complete_out = counts_complete;
  }
  return layout;
}

void VulkanPipelineCache::RefreshPipelineLayoutIfStale(
    Pipeline& pipeline, const VulkanShader::VulkanTranslation* vertex_shader,
    const VulkanShader::VulkanTranslation* pixel_shader) {
  if (!pipeline.pipeline_layout_stale.load(std::memory_order_acquire)) {
    return;
  }
  bool counts_complete;
  const PipelineLayoutProvider* layout =
      GetGuestGraphicsPipelineLayout(vertex_shader, pixel_shader,
                                     &counts_complete);
  if (layout && counts_complete) {
    // The creation job may store the same layout concurrently - the providers
    // are cached per count key, so both threads store an identical pointer.
    pipeline.pipeline_layout.store(layout, std::memory_order_release);
    pipeline.pipeline_layout_stale.store(false, std::memory_order_release);
  }
}

bool VulkanPipelineCache::ConfigurePipeline(
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key,
    bool use_interpreter, VulkanPipelineCache::Pipeline** pipeline_out,
    bool require_real) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  PipelineDescription description;
  if (!GetCurrentStateDescription(
          vertex_shader, pixel_shader, primitive_processing_result,
          normalized_depth_control, normalized_color_mask, render_pass_key,
          description)) {
    return false;
  }
  // Device feature requirements (triangle fans, depth clamp, polygon mode, ...)
  // hinge on description fields that CanonicalizePipelineDescription normalizes
  // away when they are dynamic, so the post-canonicalization check inside
  // EnsurePipelineCreated would no longer see them. Validate against the real
  // (non-canonicalized) description here.
  if (!ArePipelineRequirementsMet(description)) {
    return false;
  }
  // Resolve the per-draw dynamic state from the non-canonicalized description
  // (the exact values the static pipeline would have baked), then canonicalize
  // the description so draws differing only in dynamic state share one pipeline.
  DynamicState dynamic_state;
  FillDynamicState(description, dynamic_state);
  CanonicalizePipelineDescription(description);
  if (last_pipeline_ && last_pipeline_->first == description) {
    last_pipeline_->second.dynamic_state = dynamic_state;
    RefreshPipelineLayoutIfStale(last_pipeline_->second, vertex_shader,
                                 pixel_shader);
    // hakuX-Boosted: see OnPipelineReused.
    OnPipelineReused(*last_pipeline_, vertex_shader, pixel_shader,
                     use_interpreter, require_real);
    *pipeline_out = &last_pipeline_->second;
    return true;
  }
  auto it = pipelines_.find(description);
  if (it != pipelines_.end()) {
    it->second.dynamic_state = dynamic_state;
    RefreshPipelineLayoutIfStale(it->second, vertex_shader, pixel_shader);
    // hakuX-Boosted: see OnPipelineReused.
    OnPipelineReused(*it, vertex_shader, pixel_shader, use_interpreter,
                     require_real);
    last_pipeline_ = &*it;
    *pipeline_out = &it->second;
    return true;
  }

  // Create the pipeline if not the latest and not already existing. For an
  // interpreter placeholder the shaders aren't translated yet, so their binding
  // counts read as 0 and this yields the minimal (no-texture) layout - it is
  // upgraded to the real layout by the creation thread after translation, or
  // by RefreshPipelineLayoutIfStale on an earlier cache hit.
  bool layout_counts_complete;
  const PipelineLayoutProvider* pipeline_layout =
      GetGuestGraphicsPipelineLayout(vertex_shader, pixel_shader,
                                     &layout_counts_complete);
  if (!pipeline_layout) {
    return false;
  }

  VkShaderModule geometry_shader = VK_NULL_HANDLE;
  if (description.geometry_shader != PipelineGeometryShader::kNone) {
    GeometryShaderKey geometry_shader_key;
    GuestSpirvShaderCache::GetGeometryShaderKey(
        description.geometry_shader, vertex_shader->modification(),
        pixel_shader ? pixel_shader->modification() : 0, geometry_shader_key);
    geometry_shader = GetGeometryShader(geometry_shader_key);
    if (geometry_shader == VK_NULL_HANDLE) {
      return false;
    }
  }

  VkRenderPass render_pass =
      render_target_cache_.GetPath() ==
              RenderTargetCache::Path::kPixelShaderInterlock
          ? render_target_cache_.GetFragmentShaderInterlockRenderPass()
          : render_target_cache_.GetHostRenderTargetsRenderPass(
                render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return false;
  }

  auto& pipeline_pair =
      *pipelines_.emplace(description, Pipeline(pipeline_layout)).first;
  pipeline_pair.second.dynamic_state = dynamic_state;
  pipeline_pair.second.pipeline_layout_stale.store(!layout_counts_complete,
                                                   std::memory_order_release);

  if (storage_writer_.is_active()) {
    VulkanShader& vs = static_cast<VulkanShader&>(vertex_shader->shader());
    if (vs.try_set_ucode_storage_index(storage_writer_.storage_index())) {
      shader_storage_file_flush_needed_ = true;
      storage_writer_.QueueShaderWrite(&vs);
    }
    if (pixel_shader) {
      VulkanShader& ps = static_cast<VulkanShader&>(pixel_shader->shader());
      if (ps.try_set_ucode_storage_index(storage_writer_.storage_index())) {
        shader_storage_file_flush_needed_ = true;
        storage_writer_.QueueShaderWrite(&ps);
      }
    }

    pipeline_storage_file_flush_needed_ = true;
    PipelineStoredDescription stored_description;
    stored_description.description_hash = description.GetHash();
    std::memcpy(&stored_description.description, &description,
                sizeof(description));
    storage_writer_.QueuePipelineWrite(stored_description);
  }

  // Get tessellation shaders if needed.
  VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
  VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
  XELOGD(
      "VulkanPipelineCache: ConfigurePipeline tessellation_mode={} patch={} "
      "control_points={}",
      static_cast<uint32_t>(description.tessellation_mode),
      static_cast<uint32_t>(description.tessellation_patch),
      GetTessellationPatchControlPointCount(description));
  if (description.tessellation_mode != PipelineTessellationMode::kNone) {
    tessellation_vertex_shader =
        GetTessellationVertexShader(description.tessellation_mode);
    // Determine if we should use multi-control-point hull shaders.
    // For adaptive mode, we always use multi-control-point (per-edge factors).
    // For discrete/continuous, the host vertex shader type determines this.
    // hakuX-Boosted: and now it does - the 3cp / 4cp control shaders for the
    // control-point-indexed domains (the guest's tessellated triangle and quad
    // lists), which used to get the 1cp ones like the patch-indexed domains.
    bool use_control_point_count =
        (description.tessellation_mode ==
         PipelineTessellationMode::kAdaptive) ||
        IsTessellationControlPointIndexed(description);
    tessellation_control_shader = GetTessellationControlShader(
        description.tessellation_mode, description.tessellation_patch,
        use_control_point_count);
    if (tessellation_vertex_shader == VK_NULL_HANDLE ||
        tessellation_control_shader == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanPipelineCache: Failed to get tessellation shaders for mode {} "
          "patch {}",
          static_cast<uint32_t>(description.tessellation_mode),
          static_cast<uint32_t>(description.tessellation_patch));
      return false;
    }
  }

  // Base creation arguments shared by both async models. Only the async
  // BEHAVIOR differs and is selected by the vulkan_placeholder_pipelines cvar:
  //   on  -> upstream placeholder hot-swap (immediate stand-in, swapped later)
  //   off -> fork deferred-bind (slot stays VK_NULL_HANDLE, bound by pointer,
  //          resolved/dropped at the submission boundary)
  PipelineCreationArguments creation_arguments;
  creation_arguments.pipeline = &pipeline_pair;
  creation_arguments.vertex_shader = vertex_shader;
  creation_arguments.pixel_shader = pixel_shader;
  creation_arguments.geometry_shader = geometry_shader;
  creation_arguments.tessellation_vertex_shader = tessellation_vertex_shader;
  creation_arguments.tessellation_control_shader = tessellation_control_shader;
  creation_arguments.render_pass = render_pass;
  creation_arguments.render_pass_key = render_pass_key;

  if (cvars::vulkan_placeholder_pipelines) {
    // === UPSTREAM: placeholder hot-swap async model ===
    // Create a fast placeholder pipeline immediately (fast compile) and queue
    // the real pipeline creation in the background. This reduces stutter from
    // pipeline compilation.
    bool use_async =
        CanCreatePipelineAsync(pixel_shader != nullptr, use_interpreter);

    // The interpreter can only stand in via the async placeholder path.
    use_interpreter = use_interpreter && use_async &&
                      GetInterpreterVertexShader(false) != VK_NULL_HANDLE;

    if (use_async) {
      // The draw thread never translates for async pipelines (so it never
      // stalls on background translation). Create an immediate placeholder when
      // we can - the interpreter VS, or the real VS if it's already translated -
      // and queue the real pipeline (translate + create) on a background thread.
      // A non-interpretable draw whose shaders aren't translated yet gets NO
      // placeholder; the caller skips it until the real pipeline is ready. The
      // pipeline layout starts minimal (0 texture counts from untranslated
      // shaders) and is upgraded to the real one on the creation thread.
      pipeline_pair.second.uses_interpreter.store(use_interpreter,
                                                  std::memory_order_release);
      // hakuX-Boosted: vulkan_placeholder_real_vs off leaves a draw the
      // interpreter cannot stand in for without a placeholder (dropped until
      // the real pipeline is ready) instead of compiling its translated vertex
      // shader into one here, on the draw thread.
      // hakuX-Boosted: the creation of the placeholder itself is now
      // MakePlaceholder (below), so that a slot can also be given one late
      // or from a creation thread. A full-screen pass that waits for its
      // real pipeline (require_real) gets none.
      bool make_placeholder =
          !require_real &&
          (use_interpreter || (vertex_shader->is_translated() &&
                               cvars::vulkan_placeholder_real_vs));
      if (make_placeholder) {
        MakePlaceholder(creation_arguments, use_interpreter, false);
      } else if (!require_real && cvars::vulkan_placeholder_translate_first &&
                 !vertex_shader->is_translated()) {
        // Nothing to make a placeholder from yet: have the vertex shader
        // translated ahead of the queued pipelines.
        PipelineCreationArguments translate_arguments;
        translate_arguments.pipeline = nullptr;
        translate_arguments.vertex_shader = vertex_shader;
        translate_arguments.pixel_shader = nullptr;
        translate_arguments.geometry_shader = VK_NULL_HANDLE;
        translate_arguments.tessellation_vertex_shader = VK_NULL_HANDLE;
        translate_arguments.tessellation_control_shader = VK_NULL_HANDLE;
        translate_arguments.render_pass = VK_NULL_HANDLE;
        translate_arguments.kind = PipelineCreationArguments::Kind::kTranslate;
        translate_arguments.priority = kPriorityTranslateForPlaceholder;
        PushCreationJob(translate_arguments);
      }

      // Queue the real pipeline creation in the background.
      if (pixel_shader) {
        uint32_t bound_rts =
            pipeline_util::GetBoundRTMaskFromNormalizedColorMask(
                normalized_color_mask);
        creation_arguments.priority = pipeline_util::CalculatePipelinePriority(
            bound_rts, pixel_shader->shader().writes_color_targets(),
            pixel_shader->shader().writes_depth());
      }
      // hakuX-Boosted: a pipeline the draw waits for goes first.
      if (require_real) {
        creation_arguments.priority = kPriorityWaitedFor;
      }
      {
        std::lock_guard<std::mutex> lock(creation_request_lock_);
        creation_queue_.push(creation_arguments);
      }
      creation_request_cond_.notify_one();
      if (require_real) {
        WaitForRealPipeline(pipeline_pair);
      }
    } else {
      // Sync mode (no creation threads / async off / no pixel shader):
      // translate on this thread and create the pipeline immediately.
      if (!vertex_shader->is_translated()) {
        if (!EnsureShadersTranslated(vertex_shader, pixel_shader)) {
          return false;
        }
        const PipelineLayoutProvider* real_layout =
            GetGuestGraphicsPipelineLayout(vertex_shader, pixel_shader);
        if (!real_layout) {
          return false;
        }
        pipeline_pair.second.pipeline_layout.store(real_layout,
                                                   std::memory_order_release);
        pipeline_pair.second.pipeline_layout_stale.store(
            false, std::memory_order_release);
      }
      if (!EnsurePipelineCreated(creation_arguments)) {
        return false;
      }
    }
  } else {
    // === FORK: deferred-bind async model (no placeholder ever created) ===
    // Queue the pipeline to the creation threads and return the stable slot
    // immediately with the handle still VK_NULL_HANDLE. Nothing is compiled on
    // the GPU thread - the command processor records a deferred bind that
    // dereferences the slot at replay, and the submission boundary either waits
    // for creation to complete (default) or drops the draws of still-unready
    // pipelines (vulkan_async_skip_draws).
    bool use_async =
        cvars::async_shader_compilation && !creation_threads_.empty();
    if (use_async) {
      // Prioritize pipelines that write to visible render targets so the most
      // noticeable draws come back first.
      if (pixel_shader) {
        uint32_t bound_rts =
            pipeline_util::GetBoundRTMaskFromNormalizedColorMask(
                normalized_color_mask);
        creation_arguments.priority = pipeline_util::CalculatePipelinePriority(
            bound_rts, pixel_shader->shader().writes_color_targets(),
            pixel_shader->shader().writes_depth());
      }
      {
        std::lock_guard<std::mutex> lock(creation_request_lock_);
        creation_queue_.push(creation_arguments);
      }
      creation_request_cond_.notify_one();
    } else {
      // Sync mode or no creation threads: create inline on the GPU thread.
      if (!EnsurePipelineCreated(creation_arguments)) {
        return false;
      }
    }
  }

  // hakuX-Boosted: a pipeline no prediction had made (shader_ahead).
  if (IsAheadActive()) {
    NoteRealPipeline(pipeline_pair, false);
  }

  last_pipeline_ = &pipeline_pair;
  *pipeline_out = &pipeline_pair.second;
  return true;
}

// hakuX-Boosted: from here to MaybeSaveVkPipelineCache, the code of
// boosted/notes/shader-ahead.md. GPU command thread only, except where a
// function says otherwise.

void VulkanPipelineCache::PushCreationJob(
    const PipelineCreationArguments& arguments) {
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    creation_queue_.push(arguments);
  }
  creation_request_cond_.notify_one();
}

uint8_t VulkanPipelineCache::GetRealPipelinePriority(
    const PipelineDescription& description,
    const VulkanShader::VulkanTranslation* pixel_shader) const {
  if (!pixel_shader) {
    return 0;
  }
  // ConfigurePipeline has the draw's colour mask for this; a slot taken up
  // later only has its description, whose render pass says which colour
  // targets are bound.
  return pipeline_util::CalculatePipelinePriority(
      description.render_pass_key.depth_and_color_used >> 1,
      pixel_shader->shader().writes_color_targets(),
      pixel_shader->shader().writes_depth());
}

bool VulkanPipelineCache::GetCreationArguments(
    const PipelineDescription& description,
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader,
    PipelineCreationArguments& arguments_out) {
  // The same lookups, in the same order, as ConfigurePipeline's for a new
  // slot.
  PipelineCreationArguments arguments;
  arguments.pipeline = nullptr;
  arguments.vertex_shader = vertex_shader;
  arguments.pixel_shader = pixel_shader;
  arguments.geometry_shader = VK_NULL_HANDLE;
  arguments.tessellation_vertex_shader = VK_NULL_HANDLE;
  arguments.tessellation_control_shader = VK_NULL_HANDLE;
  if (description.geometry_shader != PipelineGeometryShader::kNone) {
    GeometryShaderKey geometry_shader_key;
    GuestSpirvShaderCache::GetGeometryShaderKey(
        description.geometry_shader, vertex_shader->modification(),
        pixel_shader ? pixel_shader->modification() : 0, geometry_shader_key);
    arguments.geometry_shader = GetGeometryShader(geometry_shader_key);
    if (arguments.geometry_shader == VK_NULL_HANDLE) {
      return false;
    }
  }
  arguments.render_pass =
      render_target_cache_.GetPath() ==
              RenderTargetCache::Path::kPixelShaderInterlock
          ? render_target_cache_.GetFragmentShaderInterlockRenderPass()
          : render_target_cache_.GetHostRenderTargetsRenderPass(
                description.render_pass_key);
  if (arguments.render_pass == VK_NULL_HANDLE) {
    return false;
  }
  arguments.render_pass_key = description.render_pass_key;
  if (description.tessellation_mode != PipelineTessellationMode::kNone) {
    arguments.tessellation_vertex_shader =
        GetTessellationVertexShader(description.tessellation_mode);
    bool use_control_point_count =
        (description.tessellation_mode ==
         PipelineTessellationMode::kAdaptive) ||
        IsTessellationControlPointIndexed(description);
    arguments.tessellation_control_shader = GetTessellationControlShader(
        description.tessellation_mode, description.tessellation_patch,
        use_control_point_count);
    if (arguments.tessellation_vertex_shader == VK_NULL_HANDLE ||
        arguments.tessellation_control_shader == VK_NULL_HANDLE) {
      return false;
    }
  }
  arguments_out = arguments;
  return true;
}

void VulkanPipelineCache::QueueStorageWrites(
    const PipelineDescription& description,
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader) {
  // What ConfigurePipeline does for a new slot: a speculative slot is only
  // stored once a draw has used it, so a wrong guess is not compiled again at
  // the next start.
  if (!storage_writer_.is_active()) {
    return;
  }
  VulkanShader& vs = static_cast<VulkanShader&>(vertex_shader->shader());
  if (vs.try_set_ucode_storage_index(storage_writer_.storage_index())) {
    shader_storage_file_flush_needed_ = true;
    storage_writer_.QueueShaderWrite(&vs);
  }
  if (pixel_shader) {
    VulkanShader& ps = static_cast<VulkanShader&>(pixel_shader->shader());
    if (ps.try_set_ucode_storage_index(storage_writer_.storage_index())) {
      shader_storage_file_flush_needed_ = true;
      storage_writer_.QueueShaderWrite(&ps);
    }
  }
  pipeline_storage_file_flush_needed_ = true;
  PipelineStoredDescription stored_description;
  stored_description.description_hash = description.GetHash();
  std::memcpy(&stored_description.description, &description,
              sizeof(description));
  storage_writer_.QueuePipelineWrite(stored_description);
}

void VulkanPipelineCache::MakePlaceholder(
    const PipelineCreationArguments& real_arguments, bool use_interpreter,
    bool late) {
  // Moved here from ConfigurePipeline, where it was the body of
  // `if (make_placeholder)`. What is new: the three modules and the layout
  // are chosen first, and the pipeline is then created either here, as
  // before, or by a creation thread (vulkan_placeholder_async).
  std::pair<const PipelineDescription, Pipeline>& pipeline_pair =
      *real_arguments.pipeline;
  const PipelineDescription& description = pipeline_pair.first;
  VulkanShader::VulkanTranslation* vertex_shader = real_arguments.vertex_shader;
  VulkanShader::VulkanTranslation* pixel_shader = real_arguments.pixel_shader;
  pipeline_pair.second.placeholder_attempted = true;

  // Set is_placeholder BEFORE creating the pipeline to avoid a race with
  // the creation thread checking this flag.
  pipeline_pair.second.is_placeholder.store(true, std::memory_order_release);
  PipelineCreationArguments placeholder_args = real_arguments;
  placeholder_args.pixel_shader = nullptr;  // Will use placeholder PS
  placeholder_args.priority = 0;
  placeholder_args.kind = PipelineCreationArguments::Kind::kReal;
  placeholder_args.skip_if_claimed = false;
  placeholder_args.speculative = false;
  // hakuX-Boosted: a stand-in pixel shader instead of the no-op one, with
  // the layout that has room for its texture (and the variant of the
  // interpreter that outputs what it reads).
  StandIn stand_in;
  VkShaderModule stand_in_pixel_shader = VK_NULL_HANDLE;
  const PipelineLayoutProvider* stand_in_layout = nullptr;
  bool use_stand_in =
      GetStandIn(vertex_shader, pixel_shader, use_interpreter, stand_in,
                 stand_in_pixel_shader, stand_in_layout);
  pipeline_pair.second.placeholder_created_ms =
      xe::Clock::QueryHostUptimeMillis();
  // hakuX-Boosted: the geometry shader of a rectangle or quad list
  // passes on as many interpolators as the translated vertex shader
  // writes. The interpreter writes the stand-in's two, or none, so its
  // placeholder gets a geometry shader built for that
  // (vulkan_placeholder_interpreter_geometry lets such draws get here).
  bool placeholder_possible = true;
  if (use_interpreter &&
      description.geometry_shader != PipelineGeometryShader::kNone) {
    GeometryShaderKey placeholder_geometry_shader_key;
    placeholder_geometry_shader_key.type = description.geometry_shader;
    // Version 2 writes the interpolators at their own locations: those up
    // to the last one the stand-in reads are passed on.
    uint32_t placeholder_interpolator_count = use_stand_in ? 2 : 0;
    if (use_stand_in && interpreter_program_) {
      placeholder_interpolator_count = 0;
      if (stand_in.shader != StandInPixelShader::kFlat) {
        placeholder_interpolator_count = stand_in.texcoord_interpolator + 1;
      }
      if (stand_in.color_interpolator != StandIn::kNoInterpolator) {
        placeholder_interpolator_count = std::max(
            placeholder_interpolator_count, stand_in.color_interpolator + 1);
      }
    }
    placeholder_geometry_shader_key.interpolator_count =
        placeholder_interpolator_count;
    placeholder_args.geometry_shader =
        GetGeometryShader(placeholder_geometry_shader_key);
    placeholder_possible = placeholder_args.geometry_shader != VK_NULL_HANDLE;
  }

  // The modules and the layout of the three kinds of placeholder.
  VkShaderModule fragment_shader_override = VK_NULL_HANDLE;
  VkShaderModule vertex_shader_override = VK_NULL_HANDLE;
  const PipelineLayoutProvider* layout_override = nullptr;
  if (placeholder_possible) {
    if (use_stand_in) {
      pipeline_pair.second.placeholder_pipeline_layout.store(
          stand_in_layout, std::memory_order_release);
      pipeline_pair.second.placeholder_stand_in.store(
          stand_in.Pack(), std::memory_order_release);
      fragment_shader_override = stand_in_pixel_shader;
      vertex_shader_override =
          use_interpreter ? GetInterpreterVertexShader(true) : VK_NULL_HANDLE;
      layout_override = stand_in_layout;
    } else if (use_interpreter) {
      fragment_shader_override = GetInterpreterPlaceholderPixelShader(
          placeholder_args, pixel_shader == nullptr);
      vertex_shader_override = GetInterpreterVertexShader(false);
    } else {
      fragment_shader_override = placeholder_pixel_shader_;
    }
    placeholder_possible = fragment_shader_override != VK_NULL_HANDLE;
  }

  bool placeholder_created = false;
  bool placeholder_queued = false;
  if (!placeholder_possible) {
    placeholder_created = false;
  } else if (cvars::vulkan_placeholder_async && !creation_threads_.empty()) {
    // A creation thread creates it, ahead of everything else queued, and
    // this thread waits for it for what is left of the frame's allowance.
    placeholder_args.kind = PipelineCreationArguments::Kind::kPlaceholder;
    placeholder_args.priority = kPriorityPlaceholder;
    placeholder_args.placeholder_fragment_shader = fragment_shader_override;
    placeholder_args.placeholder_vertex_shader = vertex_shader_override;
    placeholder_args.placeholder_layout = layout_override;
    pipeline_pair.second.placeholder_pending.store(true,
                                                   std::memory_order_release);
    PushCreationJob(placeholder_args);
    placeholder_queued = true;
    ++placeholders_async_;
    if (placeholder_wait_left_ms_ > 0.0) {
      Pipeline& slot = pipeline_pair.second;
      const auto wait_start = std::chrono::steady_clock::now();
      {
        std::unique_lock<std::mutex> lock(creation_request_lock_);
        creation_progress_cond_.wait_for(
            lock,
            std::chrono::duration<double, std::milli>(placeholder_wait_left_ms_),
            [&slot]() {
              return slot.pipeline.load(std::memory_order_acquire) !=
                         VK_NULL_HANDLE ||
                     !slot.placeholder_pending.load(std::memory_order_acquire);
            });
      }
      double waited_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - wait_start)
                             .count();
      placeholder_wait_left_ms_ -= waited_ms;
      ++placeholder_waits_;
      placeholder_wait_ms_ += waited_ms;
    }
  } else {
    const auto create_start = std::chrono::steady_clock::now();
    placeholder_created =
        EnsurePipelineCreated(placeholder_args, fragment_shader_override,
                              vertex_shader_override, layout_override, late);
    double create_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - create_start)
                           .count();
    ++placeholders_inline_;
    placeholder_inline_worst_ms_ =
        std::max(placeholder_inline_worst_ms_, create_ms);
  }

  if (use_stand_in && placeholder_possible) {
    if (!placeholder_created && !placeholder_queued) {
      pipeline_pair.second.placeholder_stand_in.store(
          0, std::memory_order_release);
      pipeline_pair.second.placeholder_pipeline_layout.store(
          nullptr, std::memory_order_release);
    } else if (cvars::shader_profiling) {
      XELOGI(
          "shader_profiling: stand-in made for VS {:016X}, PS {:016X}: "
          "{} vertex shader, {} pixel shader, texture coordinate {}, "
          "vertex colour {}{}{}",
          vertex_shader->shader().ucode_data_hash(),
          pixel_shader->shader().ucode_data_hash(),
          use_interpreter ? "interpreter" : "translated",
          GetStandInPixelShaderName(stand_in.shader),
          stand_in.shader == StandInPixelShader::kFlat
              ? "none"
              : (use_interpreter && !interpreter_program_
                     ? "chosen per draw"
                     : fmt::format("interpolator {}",
                                   stand_in.texcoord_interpolator)),
          stand_in.color_interpolator == StandIn::kNoInterpolator
              ? "none"
              : fmt::format("interpolator {}", stand_in.color_interpolator),
          placeholder_queued ? " (queued for a creation thread)" : "",
          late ? " (late)" : "");
    }
  }
  if (placeholder_created || placeholder_queued) {
    if (use_interpreter && !use_stand_in) {
      XELOGI(
          "VS interpreter placeholder created (interpreter VS + no-op "
          "PS): VS {:016X}, PS {:016X}",
          vertex_shader->shader().ucode_data_hash(),
          pixel_shader ? pixel_shader->shader().ucode_data_hash() : 0);
    }
  } else {
    XELOGW(
        "VS {} placeholder FAILED to create - dropping draws until the "
        "real pipeline is ready: VS {:016X}, PS {:016X}",
        use_interpreter ? "interpreter" : "real-VS",
        vertex_shader->shader().ucode_data_hash(),
        pixel_shader ? pixel_shader->shader().ucode_data_hash() : 0);
    // No placeholder - fall through to queue-only (drop until ready).
    // hakuX-Boosted: unless the real pipeline arrived meanwhile (a late
    // placeholder), whose flags are its own.
    if (pipeline_pair.second.pipeline.load(std::memory_order_acquire) ==
        VK_NULL_HANDLE) {
      pipeline_pair.second.is_placeholder.store(false,
                                                std::memory_order_release);
      pipeline_pair.second.uses_interpreter.store(false,
                                                  std::memory_order_release);
    }
  }
}

bool VulkanPipelineCache::IsFullscreenRealPipelineWaited() const {
  return cvars::vulkan_placeholder_pipelines &&
         cvars::vulkan_placeholder_wait_fullscreen_ms > 0 &&
         cvars::async_shader_compilation && !creation_threads_.empty() &&
         !startup_loading_;
}

void VulkanPipelineCache::NoteFrameBoundary() {
  placeholder_wait_left_ms_ =
      double(std::max(int32_t(0), cvars::vulkan_placeholder_async_wait_ms));
}

void VulkanPipelineCache::WaitForRealPipeline(
    std::pair<const PipelineDescription, Pipeline>& pipeline_pair) {
  Pipeline& slot = pipeline_pair.second;
  if (slot.wait_given_up || creation_threads_.empty()) {
    return;
  }
  auto ready = [&slot]() {
    return (slot.pipeline.load(std::memory_order_acquire) != VK_NULL_HANDLE &&
            !slot.is_placeholder.load(std::memory_order_acquire)) ||
           slot.creation_finished.load(std::memory_order_acquire);
  };
  if (ready()) {
    return;
  }
  const auto wait_start = std::chrono::steady_clock::now();
  bool timed_out;
  {
    std::unique_lock<std::mutex> lock(creation_request_lock_);
    timed_out = !creation_progress_cond_.wait_for(
        lock,
        std::chrono::milliseconds(
            std::max(int32_t(0), cvars::vulkan_placeholder_wait_fullscreen_ms)),
        ready);
  }
  double waited_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - wait_start)
                         .count();
  ++fullscreen_waits_;
  fullscreen_wait_ms_ += waited_ms;
  if (cvars::shader_profiling) {
    XELOGI(
        "shader_profiling: draw waited {:.3f} ms for the real pipeline of a "
        "full-screen pass, VS {:016X}, PS {:016X}{}",
        waited_ms, pipeline_pair.first.vertex_shader_hash,
        pipeline_pair.first.pixel_shader_hash, timed_out ? ", gave up" : "");
  }
  if (timed_out) {
    // Not waited for again: its draws are dropped (or drawn with the
    // placeholder it has) until the pipeline arrives.
    slot.wait_given_up = true;
  }
}

void VulkanPipelineCache::OnPipelineReused(
    std::pair<const PipelineDescription, Pipeline>& pipeline_pair,
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader, bool use_interpreter,
    bool require_real) {
  Pipeline& slot = pipeline_pair.second;
  const bool was_speculative = slot.speculative != 0;
  if (!was_speculative && !require_real && !cvars::vulkan_placeholder_late) {
    // Every slot with the default settings: nothing to do, as before.
    return;
  }

  if (was_speculative) {
    // The first draw of a pipeline made on a prediction: from here on it is a
    // pipeline like any other.
    ++speculative_hits_[slot.speculative_rule < kRuleCount
                            ? slot.speculative_rule
                            : 0];
    if (slot.speculative == 2) {
      ++speculative_hits_written_off_;
    } else {
      auto unused_it = std::find(speculative_unused_.begin(),
                                 speculative_unused_.end(), &pipeline_pair);
      if (unused_it != speculative_unused_.end()) {
        speculative_unused_.erase(unused_it);
      }
    }
    slot.speculative = 0;
    if (slot.pipeline.load(std::memory_order_acquire) != VK_NULL_HANDLE) {
      ++speculative_hits_ready_;
    } else if (slot.creation_claimed.load(std::memory_order_acquire)) {
      ++speculative_hits_compiling_;
    } else {
      // Still in the speculative queue: queue it again as the real request
      // it now is. Whichever job reaches the slot second is dropped.
      ++speculative_hits_queued_;
      PipelineCreationArguments promoted_arguments;
      if (GetCreationArguments(pipeline_pair.first, vertex_shader,
                               pixel_shader, promoted_arguments)) {
        promoted_arguments.pipeline = &pipeline_pair;
        promoted_arguments.priority =
            GetRealPipelinePriority(pipeline_pair.first, pixel_shader);
        promoted_arguments.skip_if_claimed = true;
        PushCreationJob(promoted_arguments);
      }
    }
    QueueStorageWrites(pipeline_pair.first, vertex_shader, pixel_shader);
    NoteRealPipeline(pipeline_pair, true);
  }

  if (!cvars::vulkan_placeholder_pipelines ||
      !CanCreatePipelineAsync(pixel_shader != nullptr, use_interpreter)) {
    return;
  }

  if (require_real) {
    if (slot.wait_given_up ||
        slot.creation_finished.load(std::memory_order_acquire) ||
        (slot.pipeline.load(std::memory_order_acquire) != VK_NULL_HANDLE &&
         !slot.is_placeholder.load(std::memory_order_acquire))) {
      return;
    }
    if (!slot.creation_claimed.load(std::memory_order_acquire)) {
      // Queued behind other pipelines: ask for it again ahead of them.
      PipelineCreationArguments boosted_arguments;
      if (GetCreationArguments(pipeline_pair.first, vertex_shader,
                               pixel_shader, boosted_arguments)) {
        boosted_arguments.pipeline = &pipeline_pair;
        boosted_arguments.priority = kPriorityWaitedFor;
        boosted_arguments.skip_if_claimed = true;
        PushCreationJob(boosted_arguments);
      }
    }
    WaitForRealPipeline(pipeline_pair);
    return;
  }

  // A slot with neither a placeholder nor its real pipeline: one that was
  // first drawn before its vertex shader was translated (and that the
  // interpreter could not stand in for), or one made on a prediction. Give it
  // the placeholder it would have had if it had been first drawn now.
  if (slot.pipeline.load(std::memory_order_acquire) != VK_NULL_HANDLE ||
      slot.placeholder_attempted ||
      slot.creation_finished.load(std::memory_order_acquire)) {
    return;
  }
  bool interpreter = use_interpreter &&
                     GetInterpreterVertexShader(false) != VK_NULL_HANDLE;
  bool make_placeholder = interpreter || (vertex_shader->is_translated() &&
                                          cvars::vulkan_placeholder_real_vs);
  if (!make_placeholder) {
    return;
  }
  if (!interpreter &&
      slot.pipeline_layout_stale.load(std::memory_order_acquire)) {
    // A placeholder with the translated vertex shader is created with, and
    // bound through, the slot's own layout, which is not yet the one for
    // these shaders. It is once both are translated: tried again then.
    return;
  }
  PipelineCreationArguments placeholder_arguments;
  if (!GetCreationArguments(pipeline_pair.first, vertex_shader, pixel_shader,
                            placeholder_arguments)) {
    slot.placeholder_attempted = true;
    return;
  }
  placeholder_arguments.pipeline = &pipeline_pair;
  slot.uses_interpreter.store(interpreter, std::memory_order_release);
  ++placeholders_late_;
  MakePlaceholder(placeholder_arguments, interpreter, true);
}

void VulkanPipelineCache::NoteDrawSkipped(uint64_t vertex_shader_hash,
                                          uint64_t pixel_shader_hash,
                                          uint32_t reasons,
                                          const Pipeline* pipeline) {
  if (pipeline && (reasons & kSkipNoPipeline)) {
    if (pipeline->placeholder_pending.load(std::memory_order_acquire)) {
      reasons |= kSkipPlaceholderCompiling;
    } else if (pipeline->wait_given_up) {
      reasons |= kSkipRealWaitedFor;
    } else if (pipeline->placeholder_attempted) {
      reasons |= kSkipPlaceholderFailed;
    } else if ((reasons & kSkipInterpreterMask) &&
               !cvars::vulkan_placeholder_real_vs) {
      reasons |= kSkipPlaceholderNotAllowed;
    } else {
      reasons |= kSkipPlaceholderNeverTried;
    }
  }
  ++skipped_draws_;
  if (reasons & kSkipVertexShaderNotTranslated) {
    ++skipped_untranslated_vs_;
  } else if (reasons & kSkipPlaceholderCompiling) {
    ++skipped_placeholder_compiling_;
  } else if (reasons & kSkipNoPipeline) {
    ++skipped_no_placeholder_;
  } else {
    ++skipped_other_;
  }
  // Once for each pair of shaders and set of reasons.
  uint64_t key = PairKey(vertex_shader_hash, pixel_shader_hash) ^
                 (uint64_t(reasons) * UINT64_C(0xD6E8FEB86659FD93));
  if (skip_reasons_logged_.size() >= 4096 ||
      !skip_reasons_logged_.insert(key).second) {
    return;
  }
  static const struct {
    uint32_t bit;
    const char* text;
  } kReasonTexts[] = {
      {kSkipVertexShaderNotTranslated,
       "the vertex shader is not translated yet, so there is nothing to make "
       "a placeholder from"},
      {kSkipNoPipeline, "the slot has no placeholder and no real pipeline yet"},
      {kSkipPlaceholderCompiling,
       "its placeholder is being compiled on a creation thread"},
      {kSkipPlaceholderFailed,
       "a placeholder was asked for and is not there (it failed, or the "
       "driver has not finished it)"},
      {kSkipPlaceholderNotAllowed,
       "vulkan_placeholder_real_vs is off, so no placeholder is made from "
       "the translated vertex shader"},
      {kSkipPlaceholderNeverTried,
       "no placeholder could be made when it was first drawn and none was "
       "made since (vulkan_placeholder_late is off, or still nothing to make "
       "one from)"},
      {kSkipRealWaitedFor,
       "a full-screen pass whose real pipeline was waited for and did not "
       "arrive in time"},
      {kSkipInterpreterPlaceholderInlineShader,
       "the slot's placeholder is the interpreter's and this draw's vertex "
       "shader was loaded inline, which the interpreter cannot read"},
      {kSkipStandInNoTexture,
       "the stand-in pixel shader has no texture it can use for this draw"},
      {kSkipStandInVertexBindings,
       "the placeholder's translated vertex shader samples textures whose "
       "bindings were not ready when this draw was set up"},
      {kSkipStandInNoSampler,
       "no sampler was left in this submission for the stand-in"},
      {kSkipInterpreterOff,
       "interpreter refused: async_shader_vs_interpreter is off"},
      {kSkipVertexShaderTranslated,
       "interpreter refused: the vertex shader is already translated and "
       "vulkan_placeholder_interpreter_translated_vs is off"},
      {kSkipInlineShader,
       "interpreter refused: the vertex shader was loaded inline "
       "(IM_LOAD_IMMEDIATE), so it has no address to read the microcode from"},
      {kSkipDeviceIndices,
       "interpreter refused: the device lacks full 32-bit indices"},
      {kSkipDeviceSharedMemory,
       "interpreter refused: shared memory is split over several bindings on "
       "this device"},
      {kSkipTessellated, "interpreter refused: the draw is tessellated"},
      {kSkipHostVertexShaderType,
       "interpreter refused: the primitive is expanded in the vertex shader "
       "(no geometry shader on this device)"},
      {kSkipPointList,
       "interpreter refused: point list (point sprites need a point size "
       "the interpreter does not output)"},
      {kSkipRectangleOrQuadList,
       "interpreter refused: rectangle or quad list and "
       "vulkan_placeholder_interpreter_geometry is off"},
      {kSkipVertexTextureFetch,
       "interpreter refused: the vertex shader fetches textures"},
      {kSkipSubroutineCalls,
       "interpreter refused: the vertex shader calls subroutines"},
      {kSkipMemoryExport,
       "interpreter refused: the vertex shader exports to memory"},
      {kSkipLoops, "interpreter refused: the vertex shader has loops"},
      {kSkipInterpreterProgram,
       "interpreter refused: the vertex shader's microcode is malformed or "
       "longer than the interpreter's buffer"},
  };
  std::string text;
  for (const auto& reason_text : kReasonTexts) {
    if (reasons & reason_text.bit) {
      if (!text.empty()) {
        text += "; ";
      }
      text += reason_text.text;
    }
  }
  XELOGI("Draw skip reason: VS {:016X}, PS {:016X} (mask {:08X}): {}",
         vertex_shader_hash, pixel_shader_hash, reasons,
         text.empty() ? "unknown" : text);
}

bool VulkanPipelineCache::IsAheadActive() const {
  return cvars::shader_ahead && cvars::async_shader_compilation &&
         !creation_threads_.empty() && !startup_loading_ &&
         render_target_cache_.GetPath() ==
             RenderTargetCache::Path::kHostRenderTargets;
}

void VulkanPipelineCache::NoteRealPipeline(
    std::pair<const PipelineDescription, Pipeline>& pipeline_pair,
    bool was_speculative) {
  if (!IsAheadActive()) {
    return;
  }
  const PipelineDescription& description = pipeline_pair.first;
  if (!was_speculative) {
    ++ahead_misses_;
  }
  if (description.pixel_shader_hash) {
    uint64_t pair_key = PairKey(description.vertex_shader_hash,
                                description.pixel_shader_hash);
    if (real_pairs_.insert(pair_key).second) {
      auto ahead_it = ahead_pairs_.find(pair_key);
      if (ahead_it != ahead_pairs_.end()) {
        uint64_t lead_ms =
            xe::Clock::QueryHostUptimeMillis() - ahead_it->second;
        ++ahead_lead_count_;
        ahead_lead_total_ms_ += lead_ms;
        ahead_lead_max_ms_ = std::max(ahead_lead_max_ms_, lead_ms);
      }
    }
    descriptions_by_pixel_shader_[description.pixel_shader_hash].push_back(
        &description);
  }
  descriptions_by_vertex_shader_[description.vertex_shader_hash].push_back(
      &description);
  last_real_description_ = &description;
  SpeculateSiblings(description);
}

namespace {
// Everything in a pipeline description but which shaders it has.
template <typename Description>
Description StateOfDescription(const Description& description) {
  Description state(description);
  state.vertex_shader_hash = 0;
  state.vertex_shader_modification = 0;
  state.pixel_shader_hash = 0;
  state.pixel_shader_modification = 0;
  return state;
}
}  // namespace

bool VulkanPipelineCache::BuildDescriptionFromTemplate(
    const PipelineDescription& state_template, VulkanShader& vertex_shader,
    VulkanShader& pixel_shader, PipelineDescription& description_out) {
  // The state, and whatever in the two modifications comes from the state, is
  // the template's. What comes from the shaders is worked out for this pair,
  // and where that cannot be done from the shaders alone the template is not
  // used. A description built wrongly can never be drawn with (a draw only
  // finds a slot by the description it computes itself), so the price of a
  // wrong guess here is a wasted compile, never a wrong picture.
  if (!state_template.pixel_shader_hash ||
      state_template.tessellation_mode != PipelineTessellationMode::kNone ||
      state_template.geometry_shader == PipelineGeometryShader::kPointList) {
    return false;
  }
  auto template_vs_it = shaders_.find(state_template.vertex_shader_hash);
  auto template_ps_it = shaders_.find(state_template.pixel_shader_hash);
  if (template_vs_it == shaders_.end() || template_ps_it == shaders_.end()) {
    return false;
  }
  const VulkanShader& template_vs = *template_vs_it->second;
  const VulkanShader& template_ps = *template_ps_it->second;
  if (!template_vs.is_ucode_analyzed() || !template_ps.is_ucode_analyzed() ||
      !vertex_shader.is_ucode_analyzed() || !pixel_shader.is_ucode_analyzed()) {
    return false;
  }
  SpirvShaderTranslator::Modification vertex_modification(
      state_template.vertex_shader_modification);
  SpirvShaderTranslator::Modification pixel_modification(
      state_template.pixel_shader_modification);
  if (vertex_modification.vertex.host_vertex_shader_type !=
          Shader::HostVertexShaderType::kVertex ||
      vertex_modification.vertex.dynamic_addressable_register_count ||
      pixel_modification.pixel.dynamic_addressable_register_count ||
      pixel_modification.pixel.param_gen_enable ||
      vertex_shader.uses_register_dynamic_addressing() ||
      pixel_shader.uses_register_dynamic_addressing() ||
      template_vs.uses_register_dynamic_addressing() ||
      template_ps.uses_register_dynamic_addressing()) {
    return false;
  }
  // The bits of the modifications and of the state that follow what the
  // shader writes: only taken over between shaders that write the same.
  if (vertex_shader.writes_point_size_edge_flag_kill_vertex() !=
          template_vs.writes_point_size_edge_flag_kill_vertex() ||
      pixel_shader.writes_color_targets() !=
          template_ps.writes_color_targets() ||
      pixel_shader.writes_depth() != template_ps.writes_depth() ||
      pixel_shader.kills_pixels() != template_ps.kills_pixels()) {
    return false;
  }
  // The interpolators that reach the pixel shader, as IssueDraw works them
  // out, for a pixel shader without PsParamGen (checked above for the
  // template; the registers that would say so for this pair are not written
  // yet).
  reg::SQ_PROGRAM_CNTL sq_program_cntl =
      register_file_.Get<reg::SQ_PROGRAM_CNTL>();
  sq_program_cntl.param_gen = 0;
  uint32_t param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      vertex_shader.writes_interpolators() &
      pixel_shader.GetInterpolatorInputMask(
          sq_program_cntl, register_file_.Get<reg::SQ_CONTEXT_MISC>(),
          param_gen_pos);
  uint32_t template_interpolator_mask =
      pixel_modification.pixel.interpolator_mask;
  uint32_t template_centroid = pixel_modification.pixel.interpolators_centroid;
  if (template_centroid != 0 && template_centroid != template_interpolator_mask) {
    // Centroid for some interpolators only: which, for this pair, is not
    // known.
    return false;
  }
  vertex_modification.vertex.interpolator_mask = interpolator_mask;
  pixel_modification.pixel.interpolator_mask = interpolator_mask;
  pixel_modification.pixel.interpolators_centroid =
      template_centroid ? interpolator_mask : 0;

  description_out = state_template;
  description_out.vertex_shader_hash = vertex_shader.ucode_data_hash();
  description_out.vertex_shader_modification = vertex_modification.value;
  description_out.pixel_shader_hash = pixel_shader.ucode_data_hash();
  description_out.pixel_shader_modification = pixel_modification.value;
  return true;
}

bool VulkanPipelineCache::SpeculatePipeline(
    const PipelineDescription& description, SpeculationRule rule) {
  // Without vulkan_async_skip_draws the end of a submission waits for every
  // queued pipeline, and it does not know about these.
  if (!cvars::shader_ahead_speculate || !cvars::vulkan_async_skip_draws ||
      !IsAheadActive()) {
    return false;
  }
  if (!description.pixel_shader_hash ||
      description.tessellation_mode != PipelineTessellationMode::kNone ||
      pipelines_.find(description) != pipelines_.end()) {
    return false;
  }
  // The bounds.
  if (speculative_written_off_ >=
      uint64_t(std::max(int32_t(0), cvars::shader_ahead_max_wasted))) {
    if (!speculative_stopped_logged_) {
      speculative_stopped_logged_ = true;
      XELOGI(
          "shader ahead: {} speculative pipelines were never used "
          "(shader_ahead_max_wasted): no more are made in this run",
          speculative_written_off_);
    }
    ++speculative_refused_budget_;
    return false;
  }
  if (speculative_outstanding_.load(std::memory_order_relaxed) >=
          uint32_t(std::max(int32_t(0), cvars::shader_ahead_max_outstanding)) ||
      speculative_unused_.size() >=
          size_t(std::max(int32_t(0), cvars::shader_ahead_max_unused))) {
    ++speculative_refused_budget_;
    return false;
  }
  if (!ArePipelineRequirementsMet(description)) {
    return false;
  }
  auto vs_it = shaders_.find(description.vertex_shader_hash);
  auto ps_it = shaders_.find(description.pixel_shader_hash);
  if (vs_it == shaders_.end() || ps_it == shaders_.end() ||
      !vs_it->second->is_ucode_analyzed() ||
      !ps_it->second->is_ucode_analyzed()) {
    return false;
  }
  auto* vertex_shader = static_cast<VulkanShader::VulkanTranslation*>(
      vs_it->second->GetOrCreateTranslation(
          description.vertex_shader_modification));
  auto* pixel_shader = static_cast<VulkanShader::VulkanTranslation*>(
      ps_it->second->GetOrCreateTranslation(
          description.pixel_shader_modification));
  if (!vertex_shader || !pixel_shader) {
    return false;
  }
  PipelineCreationArguments creation_arguments;
  if (!GetCreationArguments(description, vertex_shader, pixel_shader,
                            creation_arguments)) {
    return false;
  }
  bool layout_counts_complete;
  const PipelineLayoutProvider* pipeline_layout =
      GetGuestGraphicsPipelineLayout(vertex_shader, pixel_shader,
                                     &layout_counts_complete);
  if (!pipeline_layout) {
    return false;
  }
  auto& pipeline_pair =
      *pipelines_.emplace(description, Pipeline(pipeline_layout)).first;
  pipeline_pair.second.pipeline_layout_stale.store(!layout_counts_complete,
                                                   std::memory_order_release);
  pipeline_pair.second.speculative = 1;
  pipeline_pair.second.speculative_rule = uint8_t(rule);
  pipeline_pair.second.speculative_made_ms = xe::Clock::QueryHostUptimeMillis();
  speculative_unused_.push_back(&pipeline_pair);
  ++speculative_made_[rule];
  creation_arguments.pipeline = &pipeline_pair;
  creation_arguments.priority = 0;
  creation_arguments.speculative = true;
  creation_arguments.skip_if_claimed = true;
  speculative_outstanding_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    speculative_queue_.push_back(creation_arguments);
  }
  creation_request_cond_.notify_one();
  return true;
}

void VulkanPipelineCache::SpeculateSiblings(
    const PipelineDescription& description) {
  // A pair drawn in a state it had not been drawn in. Its vertex shader has
  // other pixel shaders (and its pixel shader other vertex shaders), and some
  // of those pairs have been drawn both in this state and in another one: the
  // same objects drawn into another render target, as a rule. Predict that
  // this pair will be drawn in that other state too.
  if (!cvars::shader_ahead_speculate || !description.pixel_shader_hash) {
    return;
  }
  constexpr size_t kLookBack = 64;
  const PipelineDescription description_state = StateOfDescription(description);
  auto same_stages = [&description](const PipelineDescription& other) {
    return other.geometry_shader == description.geometry_shader &&
           other.tessellation_mode == description.tessellation_mode &&
           other.tessellation_patch == description.tessellation_patch &&
           other.primitive_topology == description.primitive_topology &&
           other.primitive_restart == description.primitive_restart &&
           other.topology_class == description.topology_class;
  };
  int32_t budget = std::max(int32_t(1), cvars::shader_ahead_templates);
  for (uint32_t by_pixel = 0; by_pixel < 2 && budget > 0; ++by_pixel) {
    const auto& index =
        by_pixel ? descriptions_by_pixel_shader_ : descriptions_by_vertex_shader_;
    auto index_it = index.find(by_pixel ? description.pixel_shader_hash
                                        : description.vertex_shader_hash);
    if (index_it == index.end()) {
      continue;
    }
    const std::vector<const PipelineDescription*>& others = index_it->second;
    size_t first = others.size() > kLookBack ? others.size() - kLookBack : 0;
    for (size_t i = others.size(); i > first && budget > 0; --i) {
      const PipelineDescription& other = *others[i - 1];
      // The shader of `other` that is not shared with this pair.
      uint64_t other_partner =
          by_pixel ? other.vertex_shader_hash : other.pixel_shader_hash;
      uint64_t this_partner = by_pixel ? description.vertex_shader_hash
                                       : description.pixel_shader_hash;
      if (!other.pixel_shader_hash || other_partner == this_partner ||
          !same_stages(other)) {
        continue;
      }
      const PipelineDescription other_state = StateOfDescription(other);
      if (other_state == description_state) {
        continue;
      }
      // Has that other pair been drawn in this pair's state as well?
      bool linked = false;
      for (size_t j = others.size(); j > first && !linked; --j) {
        const PipelineDescription& third = *others[j - 1];
        uint64_t third_partner =
            by_pixel ? third.vertex_shader_hash : third.pixel_shader_hash;
        linked = third_partner == other_partner &&
                 StateOfDescription(third) == description_state;
      }
      if (!linked) {
        continue;
      }
      PipelineDescription predicted(other);
      predicted.vertex_shader_hash = description.vertex_shader_hash;
      predicted.vertex_shader_modification =
          description.vertex_shader_modification;
      predicted.pixel_shader_hash = description.pixel_shader_hash;
      predicted.pixel_shader_modification =
          description.pixel_shader_modification;
      if (SpeculatePipeline(predicted, kRuleSibling)) {
        --budget;
      }
    }
  }
}

void VulkanPipelineCache::NoteShaderPairAhead(
    VulkanShader* vertex_shader, VulkanShader* pixel_shader,
    xenos::PrimitiveType guest_primitive_type) {
  if (!vertex_shader || !pixel_shader || !IsAheadActive()) {
    return;
  }
  uint64_t pair_key = PairKey(vertex_shader->ucode_data_hash(),
                              pixel_shader->ucode_data_hash());
  if (ahead_pairs_.size() >= 65536) {
    ahead_pairs_.clear();
  }
  if (!ahead_pairs_.emplace(pair_key, xe::Clock::QueryHostUptimeMillis())
           .second) {
    return;
  }
  if (real_pairs_.find(pair_key) != real_pairs_.end()) {
    // Drawn before: whatever it needs was asked for by its draws.
    return;
  }
  ++ahead_pairs_new_;
  // The analysis every draw needs first, done here instead of at the draw.
  if (!vertex_shader->is_ucode_analyzed()) {
    ++ahead_shaders_new_;
    AnalyzeShaderUcode(*vertex_shader);
  }
  if (!pixel_shader->is_ucode_analyzed()) {
    ++ahead_shaders_new_;
    AnalyzeShaderUcode(*pixel_shader);
  }
  if (guest_primitive_type == xenos::PrimitiveType::kPointList) {
    // Point sprites change both modifications: not predicted.
    return;
  }

  // The states the pair is most likely to be drawn in: those of the newest
  // pipelines that have its vertex shader, failing that its pixel shader,
  // failing that the pipeline a draw asked for last.
  const size_t template_count =
      size_t(std::max(int32_t(1), cvars::shader_ahead_templates));
  std::vector<const PipelineDescription*> templates;
  SpeculationRule rule = kRuleSameVertexShader;
  auto collect = [&templates, template_count](
                     const std::vector<const PipelineDescription*>& from) {
    for (size_t i = from.size(); i > 0 && templates.size() < template_count;
         --i) {
      if (!from[i - 1]->pixel_shader_hash) {
        continue;
      }
      // One of each state.
      bool repeated = false;
      for (const PipelineDescription* chosen : templates) {
        repeated |= StateOfDescription(*chosen) ==
                    StateOfDescription(*from[i - 1]);
      }
      if (!repeated) {
        templates.push_back(from[i - 1]);
      }
    }
  };
  auto by_vs_it =
      descriptions_by_vertex_shader_.find(vertex_shader->ucode_data_hash());
  if (by_vs_it != descriptions_by_vertex_shader_.end()) {
    collect(by_vs_it->second);
  }
  if (templates.empty()) {
    rule = kRuleSamePixelShader;
    auto by_ps_it =
        descriptions_by_pixel_shader_.find(pixel_shader->ucode_data_hash());
    if (by_ps_it != descriptions_by_pixel_shader_.end()) {
      collect(by_ps_it->second);
    }
  }
  if (templates.empty()) {
    rule = kRuleNeighbour;
    if (last_real_description_ && last_real_description_->pixel_shader_hash) {
      templates.push_back(last_real_description_);
    }
  }

  bool translation_queued = false;
  for (const PipelineDescription* state_template : templates) {
    PipelineDescription predicted;
    if (!BuildDescriptionFromTemplate(*state_template, *vertex_shader,
                                      *pixel_shader, predicted)) {
      continue;
    }
    if (!translation_queued) {
      // The translations of the likeliest prediction, on a creation thread,
      // ahead of the queued pipelines (a translation is short, and a draw
      // that finds its shaders untranslated either translates them itself or
      // is dropped).
      translation_queued = true;
      auto* vertex_translation = static_cast<VulkanShader::VulkanTranslation*>(
          vertex_shader->GetOrCreateTranslation(
              predicted.vertex_shader_modification));
      auto* pixel_translation = static_cast<VulkanShader::VulkanTranslation*>(
          pixel_shader->GetOrCreateTranslation(
              predicted.pixel_shader_modification));
      if ((vertex_translation && !vertex_translation->is_translated()) ||
          (pixel_translation && !pixel_translation->is_translated())) {
        PipelineCreationArguments translate_arguments;
        translate_arguments.pipeline = nullptr;
        translate_arguments.vertex_shader = vertex_translation;
        translate_arguments.pixel_shader = pixel_translation;
        translate_arguments.geometry_shader = VK_NULL_HANDLE;
        translate_arguments.tessellation_vertex_shader = VK_NULL_HANDLE;
        translate_arguments.tessellation_control_shader = VK_NULL_HANDLE;
        translate_arguments.render_pass = VK_NULL_HANDLE;
        translate_arguments.kind = PipelineCreationArguments::Kind::kTranslate;
        translate_arguments.priority = kPriorityTranslateAhead;
        PushCreationJob(translate_arguments);
        ++ahead_translations_queued_;
      }
    }
    SpeculatePipeline(predicted, rule);
  }
}

void VulkanPipelineCache::LogStatsIfDue() {
  const bool ahead = cvars::shader_ahead;
  const bool cover = cvars::vulkan_placeholder_pipelines &&
                     cvars::shader_profiling;
  if (!ahead && !cover) {
    return;
  }
  uint64_t now_ms = xe::Clock::QueryHostUptimeMillis();
  if (now_ms - stats_logged_ms_ < 1000) {
    return;
  }
  stats_logged_ms_ = now_ms;

  // Write off the speculative pipelines no draw has come for.
  uint64_t write_off_ms =
      uint64_t(std::max(int32_t(1), cvars::shader_ahead_write_off_s)) * 1000;
  for (size_t i = 0; i < speculative_unused_.size();) {
    Pipeline& slot = speculative_unused_[i]->second;
    if (now_ms - slot.speculative_made_ms >= write_off_ms) {
      slot.speculative = 2;
      ++speculative_written_off_;
      speculative_unused_[i] = speculative_unused_.back();
      speculative_unused_.pop_back();
    } else {
      ++i;
    }
  }

  uint64_t made = 0, hits = 0;
  for (uint32_t i = 0; i < kRuleCount; ++i) {
    made += speculative_made_[i];
    hits += speculative_hits_[i];
  }
  uint64_t compiled = speculative_compiled_.load(std::memory_order_relaxed);
  // Only when something moved since the last line.
  uint64_t signature =
      ahead_pairs_new_ + ahead_translations_queued_ * 3 + ahead_misses_ * 5 +
      made * 7 + hits * 11 + speculative_written_off_ * 13 + compiled * 17 +
      draw_thread_translations_ * 19 + skipped_draws_ * 23 +
      placeholders_inline_ * 29 + placeholders_async_ * 31 +
      placeholders_late_ * 37 + fullscreen_waits_ * 41 +
      speculative_refused_budget_ * 43;
  if (signature == stats_logged_signature_) {
    return;
  }
  stats_logged_signature_ = signature;

  if (ahead) {
    XELOGI(
        "shader ahead: pairs seen ahead {} (shaders new to the cache {}), "
        "translations queued ahead {}, first pipeline of a pair asked for "
        "{:.1f} ms after the look ahead on average over {} pair(s) (longest "
        "{} ms); translations done on the GPU command thread {} ({:.1f} ms); "
        "speculative pipelines made {} (same VS {}, same PS {}, neighbour {}, "
        "sibling {}), hits {} (same VS {}, same PS {}, neighbour {}, sibling "
        "{}; ready {}, compiling {}, still queued {}, after being written off "
        "{}), misses {}, written off as wasted {}, not used yet {}, "
        "outstanding {}, compiled {} in {:.0f} ms, dropped before compiling "
        "{}, refused by the caps {}",
        ahead_pairs_new_, ahead_shaders_new_, ahead_translations_queued_,
        ahead_lead_count_
            ? double(ahead_lead_total_ms_) / double(ahead_lead_count_)
            : 0.0,
        ahead_lead_count_, ahead_lead_max_ms_, draw_thread_translations_,
        draw_thread_translation_ms_, made,
        speculative_made_[kRuleSameVertexShader],
        speculative_made_[kRuleSamePixelShader],
        speculative_made_[kRuleNeighbour], speculative_made_[kRuleSibling],
        hits, speculative_hits_[kRuleSameVertexShader],
        speculative_hits_[kRuleSamePixelShader],
        speculative_hits_[kRuleNeighbour], speculative_hits_[kRuleSibling],
        speculative_hits_ready_, speculative_hits_compiling_,
        speculative_hits_queued_, speculative_hits_written_off_, ahead_misses_,
        speculative_written_off_, speculative_unused_.size(),
        speculative_outstanding_.load(std::memory_order_relaxed), compiled,
        double(speculative_compile_us_.load(std::memory_order_relaxed)) /
            1000.0,
        speculative_dropped_.load(std::memory_order_relaxed),
        speculative_refused_budget_);
  }
  if (cover) {
    XELOGI(
        "placeholder cover: draws skipped {} (vertex shader not translated "
        "{}, no placeholder {}, placeholder compiling {}, other {}); "
        "placeholders made on the GPU command thread {} (slowest {:.1f} ms), "
        "handed to creation threads {}, of all those late {}; waits for a "
        "creation thread's placeholder {} ({:.1f} ms), for a full-screen "
        "pass's real pipeline {} ({:.1f} ms); translations done on the GPU "
        "command thread {} ({:.1f} ms)",
        skipped_draws_, skipped_untranslated_vs_, skipped_no_placeholder_,
        skipped_placeholder_compiling_, skipped_other_, placeholders_inline_,
        placeholder_inline_worst_ms_, placeholders_async_, placeholders_late_,
        placeholder_waits_, placeholder_wait_ms_, fullscreen_waits_,
        fullscreen_wait_ms_, draw_thread_translations_,
        draw_thread_translation_ms_);
  }
}

void VulkanPipelineCache::MaybeSaveVkPipelineCache() {
  // Periodically persist vk_pipeline_cache_ to disk so it survives Android
  // process kills (upstream only writes it in ShutdownShaderStorage, which
  // Android frequently skips). Throttled, and only when new pipelines were
  // created since the last save. Safe on the GPU thread concurrently with the
  // creation threads (the pipeline cache is internally synchronized).
  if (vk_pipeline_cache_dirty_.load(std::memory_order_relaxed) &&
      vk_pipeline_cache_ != VK_NULL_HANDLE && !vk_pipeline_cache_path_.empty()) {
    static constexpr uint64_t kVkPipelineCacheSaveIntervalMs = 20000;
    const uint64_t now = xe::Clock::QueryHostUptimeMillis();
    if (vk_pipeline_cache_last_save_ms_ == 0 ||
        now - vk_pipeline_cache_last_save_ms_ >= kVkPipelineCacheSaveIntervalMs) {
      vk_pipeline_cache_dirty_.store(false, std::memory_order_relaxed);
      vk_pipeline_cache_last_save_ms_ = now;
      SaveVkPipelineCache();
    }
  }
}

void VulkanPipelineCache::EndSubmission() {
  MaybeSaveVkPipelineCache();
  // hakuX-Boosted: the once-a-second lines of boosted/notes/shader-ahead.md.
  LogStatsIfDue();

  if (shader_storage_file_flush_needed_ ||
      pipeline_storage_file_flush_needed_) {
    storage_writer_.RequestFlush(shader_storage_file_flush_needed_,
                                 pipeline_storage_file_flush_needed_);
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (creation_threads_.empty()) {
    // Process deferred destructions when GPU is idle
    ProcessDeferredDestructions();
    return;
  }

  if (startup_loading_ || cvars::vulkan_async_skip_draws) {
    // Fully asynchronous: never block the GPU thread on pipeline creation.
    // Deferred pipeline binds that still resolve to VK_NULL_HANDLE when the
    // command buffer is replayed drop their draws for that submission
    // (during the storage preload at startup, or always with
    // vulkan_async_skip_draws - pop-in instead of a wait).
    creation_request_cond_.notify_one();
    // hakuX-Boosted: except the pipelines of this submission's full-screen
    // passes, which are waited for (see RequirePipelineForSubmission).
    if (!required_pipelines_.empty()) {
      auto pending = [this]() {
        for (const Pipeline* required : required_pipelines_) {
          if (required->pipeline.load(std::memory_order_acquire) ==
                  VK_NULL_HANDLE &&
              !required->creation_finished.load(std::memory_order_acquire)) {
            return true;
          }
        }
        return false;
      };
      const auto wait_start = std::chrono::steady_clock::now();
      bool timed_out = false;
      {
        std::unique_lock<std::mutex> lock(creation_request_lock_);
        timed_out = !creation_progress_cond_.wait_for(
            lock,
            std::chrono::milliseconds(std::max(
                int32_t(0), cvars::vulkan_async_wait_fullscreen_draws_ms)),
            [&pending]() { return !pending(); });
      }
      if (cvars::shader_profiling) {
        XELOGI(
            "shader_profiling: submission waited {:.3f} ms for {} full-screen "
            "pass pipeline(s){}",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wait_start)
                .count(),
            required_pipelines_.size(), timed_out ? ", gave up" : "");
      }
      if (timed_out) {
        // Not waited for again: a slot nobody is creating (or one slower than
        // the limit) would otherwise cost the limit in every submission that
        // draws it. Its draws are dropped until its handle appears.
        for (Pipeline* required : required_pipelines_) {
          if (required->pipeline.load(std::memory_order_acquire) ==
              VK_NULL_HANDLE) {
            required->creation_finished.store(true, std::memory_order_release);
          }
        }
      }
      required_pipelines_.clear();
    }
  } else {
    // Blocking: wait for all queued pipelines so every deferred pipeline bind
    // recorded in this submission resolves to a created handle - no draws are
    // ever dropped in this mode.
    bool await_creation_completion_event;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      await_creation_completion_event =
          !creation_queue_.empty() || creation_threads_busy_ != 0;
      if (await_creation_completion_event) {
        creation_completion_event_->Reset();
        creation_completion_set_event_.store(true, std::memory_order_release);
      }
    }
    if (await_creation_completion_event) {
      creation_request_cond_.notify_one();
      xe::threading::Wait(creation_completion_event_.get(), false);
    }
  }

  // Process deferred destructions
  ProcessDeferredDestructions();
}

void VulkanPipelineCache::RequirePipelineForSubmission(Pipeline* pipeline) {
  if (!pipeline || startup_loading_ || creation_threads_.empty() ||
      pipeline->pipeline.load(std::memory_order_acquire) != VK_NULL_HANDLE ||
      pipeline->creation_finished.load(std::memory_order_acquire)) {
    return;
  }
  for (const Pipeline* required : required_pipelines_) {
    if (required == pipeline) {
      return;
    }
  }
  required_pipelines_.push_back(pipeline);
}

bool VulkanPipelineCache::IsCreatingPipelines() {
  if (creation_threads_.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(creation_request_lock_);
  return !creation_queue_.empty() || creation_threads_busy_ != 0;
}

void VulkanPipelineCache::AwaitPipelineCompletion() {
  if (creation_threads_.empty()) {
    return;
  }

  bool await_creation_completion_event;
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    await_creation_completion_event =
        !creation_queue_.empty() || creation_threads_busy_ != 0;
    if (await_creation_completion_event) {
      creation_completion_event_->Reset();
      creation_completion_set_event_.store(true, std::memory_order_release);
    }
  }

  if (await_creation_completion_event) {
    creation_request_cond_.notify_one();
    xe::threading::Wait(creation_completion_event_.get(), false);
  }
}

void VulkanPipelineCache::CreationThread() {
  // Per-thread worker translator - the shared SpirvShaderTranslator is not
  // thread-safe, so the deferred ucode->SPIR-V build (VS interpreter
  // placeholder) runs here, off the main thread, one translator per creation
  // thread.
  std::unique_ptr<SpirvShaderTranslator> worker_translator =
      guest_shader_cache_.CreateWorkerTranslator();

  for (;;) {
    PipelineCreationArguments creation_arguments;
    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      // hakuX-Boosted: speculative pipelines (shader_ahead_speculate) wait in
      // their own queue and are only taken when nothing else is queued, by
      // at most shader_ahead_threads threads at a time. With the setting off
      // that queue is always empty and this is the wait it was.
      const size_t speculative_thread_limit =
          size_t(std::max(int32_t(1), cvars::shader_ahead_threads));
      creation_request_cond_.wait(lock, [this, speculative_thread_limit]() {
        return !creation_queue_.empty() || creation_threads_shutdown_ ||
               (!speculative_queue_.empty() &&
                speculative_busy_ < speculative_thread_limit);
      });
      if (creation_threads_shutdown_) {
        break;
      }
      if (!creation_queue_.empty()) {
        creation_arguments = creation_queue_.top();
        creation_queue_.pop();
        ++creation_threads_busy_;
      } else {
        // Not counted in creation_threads_busy_: nothing that waits for the
        // queued pipelines waits for a guess.
        creation_arguments = speculative_queue_.front();
        speculative_queue_.pop_front();
        ++speculative_busy_;
      }
    }

    // hakuX-Boosted: the three kinds of job this fork adds
    // (boosted/notes/shader-ahead.md). A job of the kind there always was
    // (kReal, not speculative, not skip_if_claimed) runs as before.
    const bool job_is_real =
        creation_arguments.kind == PipelineCreationArguments::Kind::kReal;
    bool job_skipped = false;
    if (creation_arguments.kind ==
        PipelineCreationArguments::Kind::kTranslate) {
      // The shaders were analysed on the GPU command thread before the job
      // was queued.
      if (creation_arguments.vertex_shader &&
          !creation_arguments.vertex_shader->is_translated()) {
        TranslateAnalyzedShader(*worker_translator,
                                *creation_arguments.vertex_shader,
                                /*use_try_claim=*/true);
      }
      if (creation_arguments.pixel_shader &&
          !creation_arguments.pixel_shader->is_translated()) {
        TranslateAnalyzedShader(*worker_translator,
                                *creation_arguments.pixel_shader,
                                /*use_try_claim=*/true);
      }
      job_skipped = true;
    } else if (creation_arguments.kind ==
               PipelineCreationArguments::Kind::kPlaceholder) {
      Pipeline& placeholder_slot = creation_arguments.pipeline->second;
      if (!EnsurePipelineCreated(
              creation_arguments, creation_arguments.placeholder_fragment_shader,
              creation_arguments.placeholder_vertex_shader,
              creation_arguments.placeholder_layout, true)) {
        XELOGW(
            "VS placeholder FAILED to create on a creation thread - dropping "
            "draws until the real pipeline is ready: VS {:016X}",
            creation_arguments.vertex_shader->shader().ucode_data_hash());
        if (placeholder_slot.pipeline.load(std::memory_order_acquire) ==
            VK_NULL_HANDLE) {
          placeholder_slot.is_placeholder.store(false,
                                                std::memory_order_release);
          placeholder_slot.uses_interpreter.store(false,
                                                  std::memory_order_release);
        }
      }
      placeholder_slot.placeholder_pending.store(false,
                                                 std::memory_order_release);
      job_skipped = true;
    } else if (creation_arguments.pipeline->second.creation_claimed.exchange(
                   true, std::memory_order_acq_rel) &&
               creation_arguments.skip_if_claimed) {
      // Another job has this slot's real pipeline in hand, or made it.
      job_skipped = true;
      if (creation_arguments.speculative) {
        speculative_dropped_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    std::chrono::steady_clock::time_point speculative_start;
    if (creation_arguments.speculative && !job_skipped) {
      speculative_start = std::chrono::steady_clock::now();
    }

    // On failure the slot stays VK_NULL_HANDLE permanently - the deferred
    // pipeline bind resolves to null at every replay and the draws using this
    // pipeline are dropped (crash-free degradation).
    if (job_skipped) {
      // hakuX-Boosted: nothing more to do for this job.
    } else if (!EnsureShadersTranslated(creation_arguments.vertex_shader,
                                 creation_arguments.pixel_shader,
                                 worker_translator.get())) {
      XELOGE("Failed to translate shaders for pipeline creation");
    } else {
      // Async pipelines are created with a minimal (no-texture) layout on the
      // draw thread since their shaders weren't translated there (interpreter
      // placeholder, or drop-until-ready with no placeholder). Now that they're
      // translated, compute the real layout before creating the real pipeline.
      // Idempotent when the layout was already real (real-VS placeholder path).
      bool real_layout_counts_complete;
      const PipelineLayoutProvider* real_layout = GetGuestGraphicsPipelineLayout(
          creation_arguments.vertex_shader, creation_arguments.pixel_shader,
          &real_layout_counts_complete);
      if (real_layout) {
        creation_arguments.pipeline->second.pipeline_layout.store(
            real_layout, std::memory_order_release);
        if (real_layout_counts_complete) {
          creation_arguments.pipeline->second.pipeline_layout_stale.store(
              false, std::memory_order_release);
        }
      }
      if (!EnsurePipelineCreated(creation_arguments)) {
        XELOGE("Failed to create Vulkan pipeline");
      }
    }

    // hakuX-Boosted: the cost of a speculative pipeline, for the once-a-second
    // line.
    if (creation_arguments.speculative && !job_skipped) {
      speculative_compiled_.fetch_add(1, std::memory_order_relaxed);
      speculative_compile_us_.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - speculative_start)
                       .count()),
          std::memory_order_relaxed);
    }

    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      if (creation_arguments.speculative) {
        --speculative_busy_;
        speculative_outstanding_.fetch_sub(1, std::memory_order_relaxed);
        if (!speculative_queue_.empty()) {
          creation_request_cond_.notify_one();
        }
      } else {
        --creation_threads_busy_;
      }
      // hakuX-Boosted: this slot's job is over, whichever way it went. Only
      // the job that made (or failed to make) the real pipeline says so: a
      // placeholder, a translation and a dropped duplicate do not.
      if (job_is_real && !job_skipped) {
        creation_arguments.pipeline->second.creation_finished.store(
            true, std::memory_order_release);
      }
      creation_progress_cond_.notify_all();
      if (creation_threads_busy_ == 0 && creation_queue_.empty()) {
        // All pipelines created.
        if (creation_completion_set_event_.load(std::memory_order_acquire)) {
          // Signal the event (blocking mode).
          creation_completion_set_event_.store(false,
                                               std::memory_order_release);
          creation_completion_event_->Set();
        }
        if (creation_completion_callback_) {
          // Invoke completion callback (non-blocking mode).
          auto callback = std::move(creation_completion_callback_);
          creation_completion_callback_ = nullptr;
          lock.unlock();
          callback();
          lock.lock();
        }
      }
    }
  }
}

bool VulkanPipelineCache::TranslateAnalyzedShader(
    SpirvShaderTranslator& translator,
    VulkanShader::VulkanTranslation& translation, bool use_try_claim) {
  VulkanShader& shader = static_cast<VulkanShader&>(translation.shader());

  // Perform translation (optimization is already disabled in translator
  // constructor). If this fails the shader will be marked as invalid and
  // ignored later. The translation object is not thread-safe, so when draw and
  // creation threads can both reach the same modification, claim it so it's
  // translated exactly once and the loser waits for the winner.
  if (!translation.is_translated()) {
    bool should_translate = true;
    if (use_try_claim) {
      should_translate = translation.TryClaimTranslation();
      if (!should_translate) {
        while (!translation.is_translated()) {
          xe::threading::MaybeYield();
        }
      }
    }
    if (should_translate) {
      bool profile = cvars::shader_profiling;
      std::chrono::steady_clock::time_point spirv_gen_start;
      if (profile) {
        spirv_gen_start = std::chrono::steady_clock::now();
      }
      if (!translator.TranslateAnalyzedShader(translation)) {
        XELOGE("Shader {:016X} translation failed; marking as ignored",
               shader.ucode_data_hash());
        return false;
      }
      if (profile) {
        XELOGI(
            "shader_profiling: {} {:016X} ucode->SPIR-V {:.3f} ms {} B SPIR-V",
            shader.type() == xenos::ShaderType::kVertex ? "vertex" : "pixel",
            shader.ucode_data_hash(),
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - spirv_gen_start)
                .count(),
            translation.translated_binary().size());
      }
    }
  }

  if (translation.GetOrCreateShaderModule() == VK_NULL_HANDLE) {
    return false;
  }

  // Dump shader files if desired.
  if (!cvars::dump_shaders.empty()) {
    translation.Dump(cvars::dump_shaders, "vulkan");
  }

  // Set up the texture binding layout.
  if (shader.EnterBindingLayoutUserUIDSetup()) {
    // texture_binding_layout_map_ / texture_binding_layouts_ are shared and may
    // be mutated concurrently now that pixel shaders (which can have textures)
    // are translated on multiple creation threads for deferred interpreter
    // draws.
    std::lock_guard<std::mutex> layout_lock(layouts_mutex_);
    // Obtain the unique IDs of the binding layout if there are any texture
    // bindings, for invalidation in the command processor.
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    const std::vector<VulkanShader::TextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    if (texture_binding_count) {
      size_t texture_binding_layout_bytes =
          texture_binding_count * sizeof(*texture_bindings.data());
      uint64_t texture_binding_layout_hash =
          XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
      auto found_range =
          texture_binding_layout_map_.equal_range(texture_binding_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length == texture_binding_count &&
            !std::memcmp(
                texture_binding_layouts_.data() + it->second.vector_span_offset,
                texture_bindings.data(), texture_binding_layout_bytes)) {
          texture_binding_layout_uid = it->second.uid;
          break;
        }
      }
      if (texture_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(
            kLayoutUIDEmpty == 0,
            "Layout UID is size + 1 because it's assumed that 0 is the UID for "
            "an empty layout");
        texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
        LayoutUID new_uid;
        new_uid.uid = texture_binding_layout_uid;
        new_uid.vector_span_offset = texture_binding_layouts_.size();
        new_uid.vector_span_length = texture_binding_count;
        texture_binding_layouts_.resize(new_uid.vector_span_offset +
                                        texture_binding_count);
        std::memcpy(
            texture_binding_layouts_.data() + new_uid.vector_span_offset,
            texture_bindings.data(), texture_binding_layout_bytes);
        texture_binding_layout_map_.emplace(texture_binding_layout_hash,
                                            new_uid);
      }
    }
    shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);

    // Use the sampler count for samplers because it's the only thing that must
    // be the same for layouts to be compatible in this case
    // (instruction-specified parameters are used as overrides for creating
    // actual samplers).
    static_assert(
        kLayoutUIDEmpty == 0,
        "Empty layout UID is assumed to be 0 because for bindful samplers, the "
        "UID is their count");
    shader.SetSamplerBindingLayoutUserUID(
        shader.GetSamplerBindingsAfterTranslation().size());
  }

  return true;
}

void VulkanPipelineCache::TranslateShadersForStorage(
    const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
    bool edram_fsi_used) {
  uint64_t translation_start = xe::Clock::QueryHostTickCount();

  std::vector<std::pair<VulkanShader*, uint64_t>> translations_to_do;
  translations_to_do.reserve(translations_needed.size());
  for (const auto& needed : translations_needed) {
    auto shader_it = shaders_.find(needed.first);
    if (shader_it == shaders_.end()) {
      continue;
    }
    VulkanShader* shader = shader_it->second;
    VulkanShader::VulkanTranslation* translation =
        static_cast<VulkanShader::VulkanTranslation*>(
            shader->GetOrCreateTranslation(needed.second));
    if (translation && !translation->is_translated()) {
      translations_to_do.emplace_back(shader, needed.second);
    }
  }

  if (translations_to_do.empty()) {
    return;
  }

  std::atomic<size_t> translation_index{0};
  std::atomic<size_t> translations_completed{0};

  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  bool msaa_2x_attachments =
      render_target_cache_.msaa_2x_attachments_supported();
  bool msaa_2x_no_attachments =
      render_target_cache_.msaa_2x_no_attachments_supported();
  uint32_t draw_res_x = render_target_cache_.draw_resolution_scale_x();
  uint32_t draw_res_y = render_target_cache_.draw_resolution_scale_y();

  auto translate_function = [this, &translations_to_do, &translation_index,
                             &translations_completed, vulkan_device,
                             msaa_2x_attachments, msaa_2x_no_attachments,
                             edram_fsi_used, draw_res_x, draw_res_y]() {
    // Each thread needs its own translator.
    SpirvShaderTranslator translator(
        SpirvShaderTranslator::Features(vulkan_device), msaa_2x_attachments,
        msaa_2x_no_attachments, edram_fsi_used, draw_res_x, draw_res_y);

    while (true) {
      size_t index = translation_index.fetch_add(1);
      if (index >= translations_to_do.size()) {
        break;
      }
      VulkanShader* shader = translations_to_do[index].first;
      uint64_t modification = translations_to_do[index].second;
      VulkanShader::VulkanTranslation* translation =
          static_cast<VulkanShader::VulkanTranslation*>(
              shader->GetTranslation(modification));
      if (translation && !translation->is_translated()) {
        if (TranslateAnalyzedShader(translator, *translation)) {
          translations_completed.fetch_add(1);
        }
      }
    }
  };

  size_t thread_count = 0;
  if (cvars::vulkan_pipeline_creation_threads != 0) {
    uint32_t logical_processor_count =
        std::max(uint32_t(1), xe::threading::logical_processor_count());
    if (cvars::vulkan_pipeline_creation_threads < 0) {
      thread_count = std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      thread_count = std::min(uint32_t(cvars::vulkan_pipeline_creation_threads),
                              logical_processor_count);
    }
    thread_count = std::min(thread_count, translations_to_do.size());
  }

  std::vector<std::unique_ptr<xe::threading::Thread>> translation_threads;
  for (size_t i = 0; i < thread_count; ++i) {
    auto thread = xe::threading::Thread::Create({}, translate_function);
    if (thread) {
      thread->set_name("Shader Translation");
      translation_threads.push_back(std::move(thread));
    }
  }

  // Main thread also participates.
  translate_function();

  for (auto& thread : translation_threads) {
    xe::threading::Wait(thread.get(), false);
  }

  XELOGI("Translated {} shaders in {} ms", translations_completed.load(),
         (xe::Clock::QueryHostTickCount() - translation_start) * 1000 /
             xe::Clock::QueryHostTickFrequency());
}

void VulkanPipelineCache::WritePipelineRenderTargetDescription(
    reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
    PipelineRenderTarget& render_target_out) const {
  if (write_mask) {
    assert_zero(write_mask & ~uint32_t(0b1111));
    // 32 because of 0x1F mask, for safety (all unknown to zero).
    static constexpr PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kOneMinusSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kOneMinusSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDstColor,
        /*  9 */ PipelineBlendFactor::kOneMinusDstColor,
        /* 10 */ PipelineBlendFactor::kDstAlpha,
        /* 11 */ PipelineBlendFactor::kOneMinusDstAlpha,
        /* 12 */ PipelineBlendFactor::kConstantColor,
        /* 13 */ PipelineBlendFactor::kOneMinusConstantColor,
        /* 14 */ PipelineBlendFactor::kConstantAlpha,
        /* 15 */ PipelineBlendFactor::kOneMinusConstantAlpha,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSaturate,
    };
    // Like kBlendFactorMap, but with the color factors changed to their alpha
    // equivalents. Alpha is scalar, so hardware treats a _COLOR factor in the
    // alpha slot as the matching _ALPHA factor.
    static constexpr PipelineBlendFactor kBlendFactorAlphaMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcAlpha,
        /*  5 */ PipelineBlendFactor::kOneMinusSrcAlpha,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kOneMinusSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDstAlpha,
        /*  9 */ PipelineBlendFactor::kOneMinusDstAlpha,
        /* 10 */ PipelineBlendFactor::kDstAlpha,
        /* 11 */ PipelineBlendFactor::kOneMinusDstAlpha,
        /* 12 */ PipelineBlendFactor::kConstantAlpha,
        /* 13 */ PipelineBlendFactor::kOneMinusConstantAlpha,
        /* 14 */ PipelineBlendFactor::kConstantAlpha,
        /* 15 */ PipelineBlendFactor::kOneMinusConstantAlpha,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSaturate,
    };
    render_target_out.src_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_srcblend)];
    render_target_out.dst_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_destblend)];
    render_target_out.color_blend_op = blend_control.color_comb_fcn;
    render_target_out.src_alpha_blend_factor =
        kBlendFactorAlphaMap[uint32_t(blend_control.alpha_srcblend)];
    render_target_out.dst_alpha_blend_factor =
        kBlendFactorAlphaMap[uint32_t(blend_control.alpha_destblend)];
    render_target_out.alpha_blend_op = blend_control.alpha_comb_fcn;
    if (!command_processor_.GetVulkanDevice()
             ->properties()
             .constantAlphaColorBlendFactors) {
      if (blend_control.color_srcblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.src_color_blend_factor =
            PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_srcblend ==
                 xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.src_color_blend_factor =
            PipelineBlendFactor::kOneMinusConstantColor;
      }
      if (blend_control.color_destblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.dst_color_blend_factor =
            PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_destblend ==
                 xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.dst_color_blend_factor =
            PipelineBlendFactor::kOneMinusConstantColor;
      }
    }
  } else {
    render_target_out.src_color_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_color_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.color_blend_op = xenos::BlendOp::kAdd;
    render_target_out.src_alpha_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_alpha_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.alpha_blend_op = xenos::BlendOp::kAdd;
  }
  render_target_out.color_write_mask = write_mask;
}

bool VulkanPipelineCache::GetCurrentStateDescription(
    const VulkanShader::VulkanTranslation* vertex_shader,
    const VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key,
    PipelineDescription& description_out) const {
  description_out.Reset();

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  const RegisterFile& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  description_out.vertex_shader_hash =
      vertex_shader->shader().ucode_data_hash();
  description_out.vertex_shader_modification = vertex_shader->modification();
  if (pixel_shader) {
    description_out.pixel_shader_hash =
        pixel_shader->shader().ucode_data_hash();
    description_out.pixel_shader_modification = pixel_shader->modification();
  }
  // Same normalization as the framebuffer key: the loadOp discard bits do not
  // affect pipeline compatibility under dynamic rendering, and letting them
  // into the key duplicates pipelines.
  description_out.render_pass_key =
      render_target_cache_.NormalizeKeyForCacheLookup(render_pass_key);

  // TODO(Triang3l): Implement primitive types currently using geometry shaders
  // without them.
  PipelineGeometryShader geometry_shader = PipelineGeometryShader::kNone;
  PipelinePrimitiveTopology primitive_topology;

  // Handle tessellated and non-tessellated draws separately, like D3D12.
  if (primitive_processing_result.IsTessellated()) {
    // Tessellation is enabled - use patch list topology.
    primitive_topology = PipelinePrimitiveTopology::kPatchList;

    // Get tessellation mode from registers.
    auto vgt_hos_cntl = regs.Get<reg::VGT_HOS_CNTL>();
    switch (vgt_hos_cntl.tess_mode) {
      case xenos::TessellationMode::kDiscrete:
        description_out.tessellation_mode = PipelineTessellationMode::kDiscrete;
        break;
      case xenos::TessellationMode::kContinuous:
        description_out.tessellation_mode =
            PipelineTessellationMode::kContinuous;
        break;
      case xenos::TessellationMode::kAdaptive:
        description_out.tessellation_mode = PipelineTessellationMode::kAdaptive;
        break;
      default:
        // Unknown tessellation mode, fall back to discrete.
        description_out.tessellation_mode = PipelineTessellationMode::kDiscrete;
        break;
    }

    // Determine patch type based on primitive type.
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kTrianglePatch:
        description_out.tessellation_patch =
            PipelineTessellationPatchType::kTriangle;
        break;
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::kQuadPatch:
        description_out.tessellation_patch =
            PipelineTessellationPatchType::kQuad;
        break;
      default:
        XELOGE("VulkanPipelineCache: Unsupported tessellated primitive type {}",
               uint32_t(primitive_processing_result.host_primitive_type));
        return false;
    }
  } else {
    // Non-tessellated draw.
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        geometry_shader = PipelineGeometryShader::kPointList;
        primitive_topology = PipelinePrimitiveTopology::kPointList;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = PipelinePrimitiveTopology::kLineList;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = PipelinePrimitiveTopology::kLineStrip;
        break;
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = PipelinePrimitiveTopology::kTriangleList;
        break;
      case xenos::PrimitiveType::kTriangleFan:
        // The check should be performed at primitive processing time.
        assert_true(device_properties.triangleFans);
        primitive_topology = PipelinePrimitiveTopology::kTriangleFan;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = PipelinePrimitiveTopology::kTriangleStrip;
        break;
      case xenos::PrimitiveType::kRectangleList:
        // Only use geometry shader if not using the fallback AsTriangleStrip
        // vertex shader type (which is used when geometry shaders aren't
        // supported).
        if (primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
          geometry_shader = PipelineGeometryShader::kRectangleList;
        }
        primitive_topology = PipelinePrimitiveTopology::kTriangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        geometry_shader = PipelineGeometryShader::kQuadList;
        primitive_topology = PipelinePrimitiveTopology::kLineListWithAdjacency;
        break;
      default:
        // TODO(Triang3l): Remaining primitive types.
        return false;
    }
  }
  description_out.geometry_shader = geometry_shader;
  description_out.primitive_topology = primitive_topology;
  description_out.primitive_restart =
      primitive_processing_result.host_primitive_reset_enabled;

  description_out.depth_clamp_enable =
      device_properties.depthClamp &&
      regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;

  // TODO(Triang3l): Tessellation.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (primitive_polygonal) {
    // Vulkan only allows the polygon mode to be set for both faces - pick the
    // most special one (more likely to represent the developer's deliberate
    // intentions - fill is very generic, wireframe is common in debug, points
    // are for pretty unusual things, but closer to debug purposes too - on the
    // Xenos, points have the lowest register value and triangles have the
    // highest) based on which faces are not culled.
    bool cull_front = pa_su_sc_mode_cntl.cull_front;
    bool cull_back = pa_su_sc_mode_cntl.cull_back;
    // hakuX-Boosted: tessellation_debug bit 0, tessellated draws not culled.
    if ((cvars::tessellation_debug & 1) &&
        Shader::IsHostVertexShaderTypeDomain(
            primitive_processing_result.host_vertex_shader_type)) {
      cull_front = false;
      cull_back = false;
    }
    description_out.cull_front = cull_front;
    description_out.cull_back = cull_back;
    if (device_properties.fillModeNonSolid) {
      xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
      if (!cull_front) {
        polygon_type =
            std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
      }
      if (!cull_back) {
        polygon_type =
            std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
      }
      if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
        polygon_type = xenos::PolygonType::kTriangles;
      }
      switch (polygon_type) {
        case xenos::PolygonType::kPoints:
          // When points are not supported, use lines instead, preserving
          // debug-like purpose.
          description_out.polygon_mode = device_properties.pointPolygons
                                             ? PipelinePolygonMode::kPoint
                                             : PipelinePolygonMode::kLine;
          break;
        case xenos::PolygonType::kLines:
          description_out.polygon_mode = PipelinePolygonMode::kLine;
          break;
        case xenos::PolygonType::kTriangles:
          description_out.polygon_mode = PipelinePolygonMode::kFill;
          break;
        default:
          assert_unhandled_case(polygon_type);
          return false;
      }
    } else {
      description_out.polygon_mode = PipelinePolygonMode::kFill;
    }
    description_out.front_face_clockwise = pa_su_sc_mode_cntl.face != 0;
  } else {
    description_out.polygon_mode = PipelinePolygonMode::kFill;
  }

  if (render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    if (render_pass_key.depth_and_color_used & 1) {
      if (normalized_depth_control.z_enable) {
        description_out.depth_write_enable =
            normalized_depth_control.z_write_enable;
        description_out.depth_compare_op = normalized_depth_control.zfunc;
      } else {
        description_out.depth_compare_op = xenos::CompareFunction::kAlways;
      }
      if (normalized_depth_control.stencil_enable) {
        description_out.stencil_test_enable = 1;
        description_out.stencil_front_fail_op =
            normalized_depth_control.stencilfail;
        description_out.stencil_front_pass_op =
            normalized_depth_control.stencilzpass;
        description_out.stencil_front_depth_fail_op =
            normalized_depth_control.stencilzfail;
        description_out.stencil_front_compare_op =
            normalized_depth_control.stencilfunc;
        if (primitive_polygonal && normalized_depth_control.backface_enable) {
          description_out.stencil_back_fail_op =
              normalized_depth_control.stencilfail_bf;
          description_out.stencil_back_pass_op =
              normalized_depth_control.stencilzpass_bf;
          description_out.stencil_back_depth_fail_op =
              normalized_depth_control.stencilzfail_bf;
          description_out.stencil_back_compare_op =
              normalized_depth_control.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op =
              description_out.stencil_front_fail_op;
          description_out.stencil_back_pass_op =
              description_out.stencil_front_pass_op;
          description_out.stencil_back_depth_fail_op =
              description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_compare_op =
              description_out.stencil_front_compare_op;
        }
      }
    }

    // Color blending and write masks (filled only for the attachments present
    // in the render pass object).
    uint32_t render_pass_color_rts = render_pass_key.depth_and_color_used >> 1;
    assert_true(device_properties.independentBlend);
    uint32_t render_pass_color_rts_remaining = render_pass_color_rts;
    uint32_t color_rt_index;
    while (xe::bit_scan_forward(render_pass_color_rts_remaining,
                                &color_rt_index)) {
      render_pass_color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      WritePipelineRenderTargetDescription(
          regs.Get<reg::RB_BLENDCONTROL>(
              reg::RB_BLENDCONTROL::rt_register_indices[color_rt_index]),
          (normalized_color_mask >> (color_rt_index * 4)) & 0b1111,
          description_out.render_targets[color_rt_index]);
    }
  }

  return true;
}

void VulkanPipelineCache::CanonicalizePipelineDescription(
    PipelineDescription& description) const {
  const DynamicStateCapabilities& caps = dynamic_state_capabilities_;
  if (!caps.extended_dynamic_state) {
    return;
  }

  // Input assembly (EDS1/EDS2 core).
  if (caps.primitive_topology_unrestricted) {
    description.primitive_topology = PipelinePrimitiveTopology::kPointList;
  } else {
    // Keep only the topology class (0=point, 1=line, 2=triangle, 3=patch) so
    // pipelines of different classes don't collapse together (dynamic topology
    // can't cross the class baked into the pipeline). The concrete topology
    // within the class is set dynamically.
    uint32_t topology_class;
    switch (description.primitive_topology) {
      case PipelinePrimitiveTopology::kPointList:
        topology_class = 0;
        break;
      case PipelinePrimitiveTopology::kLineList:
      case PipelinePrimitiveTopology::kLineStrip:
      case PipelinePrimitiveTopology::kLineListWithAdjacency:
        topology_class = 1;
        break;
      case PipelinePrimitiveTopology::kPatchList:
        topology_class = 3;
        break;
      default:
        topology_class = 2;
        break;
    }
    description.topology_class = topology_class;
    description.primitive_topology = PipelinePrimitiveTopology::kPointList;
  }
  description.primitive_restart = 0;

  // Rasterization (EDS1/EDS2 core).
  description.cull_front = 0;
  description.cull_back = 0;
  description.front_face_clockwise = 0;

  // Depth / stencil (EDS1/EDS2 core).
  description.depth_write_enable = 0;
  description.depth_compare_op = xenos::CompareFunction::kNever;
  description.stencil_test_enable = 0;
  description.stencil_front_fail_op = xenos::StencilOp::kKeep;
  description.stencil_front_pass_op = xenos::StencilOp::kKeep;
  description.stencil_front_depth_fail_op = xenos::StencilOp::kKeep;
  description.stencil_front_compare_op = xenos::CompareFunction::kNever;
  description.stencil_back_fail_op = xenos::StencilOp::kKeep;
  description.stencil_back_pass_op = xenos::StencilOp::kKeep;
  description.stencil_back_depth_fail_op = xenos::StencilOp::kKeep;
  description.stencil_back_compare_op = xenos::CompareFunction::kNever;

  // EDS3 sub-features (conditional - only zero a field when it's actually made
  // dynamic; otherwise keep it baked so differently-baked pipelines don't
  // collide).
  if (caps.depth_clamp_enable) {
    description.depth_clamp_enable = 0;
  }
  if (caps.polygon_mode) {
    description.polygon_mode = PipelinePolygonMode::kFill;
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    PipelineRenderTarget& rt = description.render_targets[i];
    // Blend enable is derived from the equation, so zeroing the equation also
    // collapses the enable. Only zero the equation when both enable and
    // equation are dynamic (the static pipeline needs a valid equation when
    // enable is baked-on but equation is dynamic, and vice versa).
    if (caps.color_blend_enable && caps.color_blend_equation) {
      rt.src_color_blend_factor = PipelineBlendFactor::kOne;
      rt.dst_color_blend_factor = PipelineBlendFactor::kZero;
      rt.color_blend_op = xenos::BlendOp::kAdd;
      rt.src_alpha_blend_factor = PipelineBlendFactor::kOne;
      rt.dst_alpha_blend_factor = PipelineBlendFactor::kZero;
      rt.alpha_blend_op = xenos::BlendOp::kAdd;
    }
    if (caps.color_write_mask) {
      rt.color_write_mask = 0;
    }
  }
}

void VulkanPipelineCache::FillDynamicState(
    const PipelineDescription& description, DynamicState& dynamic_state) const {
  static constexpr VkBlendFactor kBlendFactorMap[] = {
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  static constexpr VkBlendOp kBlendOpMap[] = {
      VK_BLEND_OP_ADD,           VK_BLEND_OP_SUBTRACT,
      VK_BLEND_OP_MIN,           VK_BLEND_OP_MAX,
      VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_ADD,
      VK_BLEND_OP_ADD,           VK_BLEND_OP_ADD};

  // Input assembly.
  switch (description.primitive_topology) {
    case PipelinePrimitiveTopology::kPointList:
      dynamic_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      break;
    case PipelinePrimitiveTopology::kLineList:
      dynamic_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case PipelinePrimitiveTopology::kLineStrip:
      dynamic_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleList:
      dynamic_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case PipelinePrimitiveTopology::kTriangleStrip:
      dynamic_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleFan:
      dynamic_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      break;
    case PipelinePrimitiveTopology::kLineListWithAdjacency:
      dynamic_state.primitive_topology =
          VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      break;
    case PipelinePrimitiveTopology::kPatchList:
      dynamic_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
      break;
    default:
      dynamic_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
  }
  dynamic_state.primitive_restart_enable =
      description.primitive_restart ? VK_TRUE : VK_FALSE;

  // Rasterization.
  dynamic_state.cull_mode = VK_CULL_MODE_NONE;
  if (description.cull_front) {
    dynamic_state.cull_mode |= VK_CULL_MODE_FRONT_BIT;
  }
  if (description.cull_back) {
    dynamic_state.cull_mode |= VK_CULL_MODE_BACK_BIT;
  }
  dynamic_state.front_face = description.front_face_clockwise
                                 ? VK_FRONT_FACE_CLOCKWISE
                                 : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  dynamic_state.depth_clamp_enable =
      description.depth_clamp_enable ? VK_TRUE : VK_FALSE;
  switch (description.polygon_mode) {
    case PipelinePolygonMode::kLine:
      dynamic_state.polygon_mode = VK_POLYGON_MODE_LINE;
      break;
    case PipelinePolygonMode::kPoint:
      dynamic_state.polygon_mode = VK_POLYGON_MODE_POINT;
      break;
    default:
      dynamic_state.polygon_mode = VK_POLYGON_MODE_FILL;
      break;
  }

  // Depth / stencil. Mirror the static path's implicit depth-test-enable
  // (enabled when writing or comparing with anything other than Always).
  bool depth_test_enable =
      description.depth_write_enable ||
      description.depth_compare_op != xenos::CompareFunction::kAlways;
  dynamic_state.depth_test_enable = depth_test_enable ? VK_TRUE : VK_FALSE;
  dynamic_state.depth_write_enable =
      description.depth_write_enable ? VK_TRUE : VK_FALSE;
  dynamic_state.depth_compare_op =
      VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                  uint32_t(description.depth_compare_op));
  dynamic_state.stencil_test_enable =
      description.stencil_test_enable ? VK_TRUE : VK_FALSE;
  dynamic_state.stencil_front.failOp =
      VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                  uint32_t(description.stencil_front_fail_op));
  dynamic_state.stencil_front.passOp =
      VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                  uint32_t(description.stencil_front_pass_op));
  dynamic_state.stencil_front.depthFailOp =
      VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                  uint32_t(description.stencil_front_depth_fail_op));
  dynamic_state.stencil_front.compareOp =
      VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                  uint32_t(description.stencil_front_compare_op));
  dynamic_state.stencil_front.compareMask = 0;
  dynamic_state.stencil_front.writeMask = 0;
  dynamic_state.stencil_front.reference = 0;
  dynamic_state.stencil_back.failOp =
      VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                  uint32_t(description.stencil_back_fail_op));
  dynamic_state.stencil_back.passOp =
      VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                  uint32_t(description.stencil_back_pass_op));
  dynamic_state.stencil_back.depthFailOp =
      VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                  uint32_t(description.stencil_back_depth_fail_op));
  dynamic_state.stencil_back.compareOp =
      VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                  uint32_t(description.stencil_back_compare_op));
  dynamic_state.stencil_back.compareMask = 0;
  dynamic_state.stencil_back.writeMask = 0;
  dynamic_state.stencil_back.reference = 0;

  // Color blend (per render target).
  uint32_t color_rts_used = description.render_pass_key.depth_and_color_used >> 1;
  dynamic_state.color_rts_used = color_rts_used;
  bool rt0_rgb_premult = false;
  bool rt0_a_premult = false;
  {
    SpirvShaderTranslator::Modification pixel_shader_modification(
        description.pixel_shader_modification);
    rt0_rgb_premult =
        pixel_shader_modification.pixel.rt0_blend_rgb_factor_for_premult !=
        xenos::BlendFactor::kOne;
    rt0_a_premult =
        pixel_shader_modification.pixel.rt0_blend_a_factor_for_premult !=
        xenos::BlendFactor::kOne;
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const PipelineRenderTarget& rt = description.render_targets[i];
    bool blend_enable =
        rt.src_color_blend_factor != PipelineBlendFactor::kOne ||
        rt.dst_color_blend_factor != PipelineBlendFactor::kZero ||
        rt.color_blend_op != xenos::BlendOp::kAdd ||
        rt.src_alpha_blend_factor != PipelineBlendFactor::kOne ||
        rt.dst_alpha_blend_factor != PipelineBlendFactor::kZero ||
        rt.alpha_blend_op != xenos::BlendOp::kAdd;
    dynamic_state.color_blend_enable[i] = blend_enable ? VK_TRUE : VK_FALSE;
    VkColorBlendEquationEXT& equation = dynamic_state.color_blend_equation[i];
    equation.srcColorBlendFactor =
        kBlendFactorMap[uint32_t(rt.src_color_blend_factor)];
    equation.dstColorBlendFactor =
        kBlendFactorMap[uint32_t(rt.dst_color_blend_factor)];
    equation.colorBlendOp = kBlendOpMap[uint32_t(rt.color_blend_op)];
    equation.srcAlphaBlendFactor =
        kBlendFactorMap[uint32_t(rt.src_alpha_blend_factor)];
    equation.dstAlphaBlendFactor =
        kBlendFactorMap[uint32_t(rt.dst_alpha_blend_factor)];
    equation.alphaBlendOp = kBlendOpMap[uint32_t(rt.alpha_blend_op)];
    // Mirror the static path's premultiplied-source-factor fixup for RT0.
    if (i == 0 && blend_enable) {
      if (rt0_rgb_premult) {
        equation.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      }
      if (rt0_a_premult) {
        equation.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      }
    }
    dynamic_state.color_write_mask[i] =
        VkColorComponentFlags(rt.color_write_mask);
  }
}

bool VulkanPipelineCache::ArePipelineRequirementsMet(
    const PipelineDescription& description) const {
  VkShaderStageFlags vertex_shader_stage =
      Shader::IsHostVertexShaderTypeDomain(
          SpirvShaderTranslator::Modification(
              description.vertex_shader_modification)
              .vertex.host_vertex_shader_type)
          ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
          : VK_SHADER_STAGE_VERTEX_BIT;
  if (!(guest_shader_vertex_stages_ & vertex_shader_stage)) {
    return false;
  }

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  if (!device_properties.geometryShader &&
      description.geometry_shader != PipelineGeometryShader::kNone) {
    return false;
  }

  if (!device_properties.triangleFans &&
      description.primitive_topology ==
          PipelinePrimitiveTopology::kTriangleFan) {
    return false;
  }

  if (!device_properties.depthClamp && description.depth_clamp_enable) {
    return false;
  }

  if (!device_properties.pointPolygons &&
      description.polygon_mode == PipelinePolygonMode::kPoint) {
    return false;
  }

  if (!device_properties.fillModeNonSolid &&
      description.polygon_mode != PipelinePolygonMode::kFill) {
    return false;
  }

  assert_true(device_properties.independentBlend);

  if (!device_properties.constantAlphaColorBlendFactors) {
    uint32_t color_rts_remaining =
        description.render_pass_key.depth_and_color_used >> 1;
    uint32_t color_rt_index;
    while (xe::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
      color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      const PipelineRenderTarget& color_rt =
          description.render_targets[color_rt_index];
      if (color_rt.src_color_blend_factor ==
              PipelineBlendFactor::kConstantAlpha ||
          color_rt.src_color_blend_factor ==
              PipelineBlendFactor::kOneMinusConstantAlpha ||
          color_rt.dst_color_blend_factor ==
              PipelineBlendFactor::kConstantAlpha ||
          color_rt.dst_color_blend_factor ==
              PipelineBlendFactor::kOneMinusConstantAlpha) {
        return false;
      }
    }
  }

  return true;
}

VkShaderModule VulkanPipelineCache::GetGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }

  // When user clip planes are present, the max count is used to reduce
  // variants.
  std::vector<unsigned int> shader_code =
      BuildGuestPrimitiveGeometryShaderSpirv(
          BuiltinGeometryShaderType(uint32_t(key.type)), key.interpolator_count,
          key.user_clip_plane_count > 0 ? 6u : 0u,
          bool(key.user_clip_plane_cull), bool(key.has_vertex_kill_and),
          bool(key.has_point_size), bool(key.has_point_coordinates),
          spirv_version_, denorm_flush_to_zero_float32_,
          signed_zero_inf_nan_preserve_float32_, rounding_mode_rte_float32_);

  // With --dump_shaders, write the built-in GS SPIR-V for signature checks.
  if (!cvars::dump_shaders.empty() && !shader_code.empty()) {
    std::filesystem::path dir = std::filesystem::absolute(cvars::dump_shaders);
    std::filesystem::create_directories(dir);
    std::filesystem::path spirv_path =
        dir / fmt::format("shader_geometry_{:08X}.spirv.bin.geom", key.key);
    if (FILE* spirv_file = xe::filesystem::OpenFile(spirv_path, "wb")) {
      fwrite(shader_code.data(), sizeof(uint32_t), shader_code.size(),
             spirv_file);
      fclose(spirv_file);
    }
  }

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      command_processor_.GetVulkanDevice(),
      reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    XELOGE(
        "VulkanPipelineCache: Failed to create the primitive type geometry "
        "shader 0x{:08X}",
        key.key);
  }
  geometry_shaders_.emplace(key, shader_module);
  return shader_module;
}

VkShaderModule VulkanPipelineCache::GetTessellationControlShader(
    PipelineTessellationMode mode, PipelineTessellationPatchType patch_type,
    bool use_control_point_count) const {
  if (mode == PipelineTessellationMode::kNone ||
      patch_type == PipelineTessellationPatchType::kNone) {
    return VK_NULL_HANDLE;
  }

  switch (mode) {
    case PipelineTessellationMode::kDiscrete:
      if (patch_type == PipelineTessellationPatchType::kTriangle) {
        return use_control_point_count ? discrete_triangle_3cp_hs_
                                       : discrete_triangle_1cp_hs_;
      } else {
        return use_control_point_count ? discrete_quad_4cp_hs_
                                       : discrete_quad_1cp_hs_;
      }
    case PipelineTessellationMode::kContinuous:
      if (patch_type == PipelineTessellationPatchType::kTriangle) {
        return use_control_point_count ? continuous_triangle_3cp_hs_
                                       : continuous_triangle_1cp_hs_;
      } else {
        return use_control_point_count ? continuous_quad_4cp_hs_
                                       : continuous_quad_1cp_hs_;
      }
    case PipelineTessellationMode::kAdaptive:
      // Adaptive mode always uses per-corner control points.
      if (patch_type == PipelineTessellationPatchType::kTriangle) {
        return adaptive_triangle_hs_;
      } else {
        return adaptive_quad_hs_;
      }
    default:
      return VK_NULL_HANDLE;
  }
}

// hakuX-Boosted: see the declaration.
bool VulkanPipelineCache::IsTessellationControlPointIndexed(
    const PipelineDescription& description) {
  if (description.tessellation_mode != PipelineTessellationMode::kDiscrete &&
      description.tessellation_mode != PipelineTessellationMode::kContinuous) {
    return false;
  }
  switch (SpirvShaderTranslator::Modification(
              description.vertex_shader_modification)
              .vertex.host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      return description.tessellation_patch ==
             PipelineTessellationPatchType::kTriangle;
    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      return description.tessellation_patch ==
             PipelineTessellationPatchType::kQuad;
    default:
      return false;
  }
}

// hakuX-Boosted: see the declaration.
uint32_t VulkanPipelineCache::GetTessellationPatchControlPointCount(
    const PipelineDescription& description) {
  if (description.tessellation_mode == PipelineTessellationMode::kNone) {
    return 0;
  }
  // Adaptive: one control point per patch corner, each carrying the factor of
  // an edge. Discrete and continuous: one per corner (its vertex index) if the
  // domain is control-point-indexed, the count the translated guest vertex
  // shader (the evaluation shader) declares and reads and the 3cp / 4cp control
  // shaders pass through; otherwise 1, the patch index.
  if (description.tessellation_mode == PipelineTessellationMode::kAdaptive ||
      IsTessellationControlPointIndexed(description)) {
    return description.tessellation_patch ==
                   PipelineTessellationPatchType::kTriangle
               ? 3
               : 4;
  }
  return 1;
}

VkShaderModule VulkanPipelineCache::GetTessellationVertexShader(
    PipelineTessellationMode mode) const {
  if (mode == PipelineTessellationMode::kNone) {
    return VK_NULL_HANDLE;
  }
  // Adaptive mode reads edge factors from index buffer; other modes pass
  // vertex indices.
  return (mode == PipelineTessellationMode::kAdaptive)
             ? tessellation_adaptive_vs_
             : tessellation_indexed_vs_;
}

// hakuX-Boosted: for the logs.
const char* VulkanPipelineCache::GetStandInPixelShaderName(
    StandInPixelShader shader) {
  switch (shader) {
    case StandInPixelShader::kFlat:
      return "flat";
    case StandInPixelShader::k2D:
      return "2D texture";
    case StandInPixelShader::kCube:
      return "cube texture";
    default:
      return "no-op";
  }
}

// hakuX-Boosted: see the declaration.
bool VulkanPipelineCache::GetStandIn(
    VulkanShader::VulkanTranslation* vertex_shader,
    VulkanShader::VulkanTranslation* pixel_shader, bool use_interpreter,
    StandIn& stand_in_out, VkShaderModule& pixel_shader_module_out,
    const PipelineLayoutProvider*& pipeline_layout_out) {
  if (cvars::vulkan_placeholder_ps_mode <= 0 || !pixel_shader ||
      render_target_cache_.GetPath() ==
          RenderTargetCache::Path::kPixelShaderInterlock) {
    return false;
  }
  VulkanShader& guest_vertex_shader =
      static_cast<VulkanShader&>(vertex_shader->shader());
  VulkanShader& guest_pixel_shader =
      static_cast<VulkanShader&>(pixel_shader->shader());
  if (!guest_pixel_shader.writes_color_targets()) {
    // Depth only: the no-op pixel shader already draws all there is.
    return false;
  }
  if (use_interpreter && GetInterpreterVertexShader(true) == VK_NULL_HANDLE) {
    return false;
  }
  // Version 1 of the interpreter routes the interpolators the stand-in reads
  // to two fixed outputs, chosen for each draw. Version 2 writes every
  // interpolator at its own location, so for it, as for a translated vertex
  // shader, the two are baked into (a copy of) the pixel shader module.
  const bool per_draw_interpolators = use_interpreter && !interpreter_program_;

  // The interpolators that reach the pixel shader: written by the vertex
  // shader, read by the pixel shader, and not replaced by PsParamGen.
  uint32_t interpolator_mask =
      SpirvShaderTranslator::Modification(vertex_shader->modification())
          .vertex.interpolator_mask;
  const VulkanShader::StandInInfo& pixel_info =
      guest_pixel_shader.GetStandInInfo();
  const VulkanShader::StandInInfo& vertex_info =
      guest_vertex_shader.GetStandInInfo();

  // The kind of the first usable texture decides the pixel shader module.
  // With the interpreter the texture and its interpolator are chosen again
  // for each draw among the candidates of that kind (see the draw path); with
  // the translated vertex shader the interpolator is baked into the module.
  StandIn stand_in;
  stand_in.shader = StandInPixelShader::kFlat;
  uint32_t texcoord_interpolators = 0;
  for (uint32_t i = 0; i < pixel_info.texture_count; ++i) {
    const VulkanShader::StandInTexture& texture = pixel_info.textures[i];
    if (!(interpolator_mask & (uint32_t(1) << texture.interpolator))) {
      continue;
    }
    StandInPixelShader texture_shader =
        texture.dimension == xenos::FetchOpDimension::kCube
            ? StandInPixelShader::kCube
            : StandInPixelShader::k2D;
    if (stand_in.shader == StandInPixelShader::kFlat) {
      stand_in.shader = texture_shader;
      if (!per_draw_interpolators) {
        stand_in.texcoord_interpolator = texture.interpolator;
      }
    }
    if (texture_shader == stand_in.shader &&
        (per_draw_interpolators ||
         texture.interpolator == stand_in.texcoord_interpolator)) {
      texcoord_interpolators |= uint32_t(1) << texture.interpolator;
    }
  }
  uint32_t color_interpolators = vertex_info.color_interpolators &
                                 interpolator_mask & ~texcoord_interpolators;
  if (color_interpolators) {
    stand_in.color_interpolator = xe::tzcnt(color_interpolators);
  }
  bool has_texture = stand_in.shader != StandInPixelShader::kFlat;
  bool has_color = stand_in.color_interpolator != StandIn::kNoInterpolator;

  VkShaderModule pixel_shader_module;
  size_t texture_count_vertex = 0, sampler_count_vertex = 0;
  if (per_draw_interpolators) {
    // Always the variant with a colour input: the interpreter outputs 1 when
    // there is no colour, and every interpreter placeholder of a kind then
    // shares one pair of modules, which the driver's cache can answer.
    pixel_shader_module =
        stand_in_pixel_shaders_[uint32_t(stand_in.shader) - 1][1];
  } else if (use_interpreter) {
    // Version 2: an interpolator's location is its index.
    pixel_shader_module = GetStandInPixelShader(
        stand_in.shader, has_color,
        has_texture ? stand_in.texcoord_interpolator : 0,
        has_color ? stand_in.color_interpolator : 1);
  } else {
    if (!guest_vertex_shader.bindings_ready()) {
      return false;
    }
    texture_count_vertex =
        guest_vertex_shader.GetTextureBindingsAfterTranslation().size();
    sampler_count_vertex =
        guest_vertex_shader.GetSamplerBindingsAfterTranslation().size();
    // The translated vertex shader writes its interpolators to consecutive
    // locations, in the order of their indices.
    auto location = [interpolator_mask](uint32_t interpolator) {
      return xe::bit_count(interpolator_mask &
                           ((uint32_t(1) << interpolator) - 1));
    };
    pixel_shader_module = GetStandInPixelShader(
        stand_in.shader, has_color,
        has_texture ? location(stand_in.texcoord_interpolator) : 0,
        has_color ? location(stand_in.color_interpolator) : 1);
  }
  if (pixel_shader_module == VK_NULL_HANDLE) {
    return false;
  }
  const PipelineLayoutProvider* pipeline_layout =
      command_processor_.GetPipelineLayout(has_texture ? 1 : 0,
                                           has_texture ? 1 : 0,
                                           texture_count_vertex,
                                           sampler_count_vertex);
  if (!pipeline_layout) {
    return false;
  }
  stand_in_out = stand_in;
  pixel_shader_module_out = pixel_shader_module;
  pipeline_layout_out = pipeline_layout;
  return true;
}

// hakuX-Boosted: see the declaration.
VkShaderModule VulkanPipelineCache::GetStandInPixelShader(
    StandInPixelShader shader, bool has_color, uint32_t texcoord_location,
    uint32_t color_location) {
  if (shader == StandInPixelShader::kNone) {
    return VK_NULL_HANDLE;
  }
  uint32_t shader_index = uint32_t(shader) - 1;
  bool has_texture = shader != StandInPixelShader::kFlat;
  if ((!has_texture || texcoord_location == 0) &&
      (!has_color || color_location == 1)) {
    return stand_in_pixel_shaders_[shader_index][has_color ? 1 : 0];
  }
  uint32_t key = shader_index | (uint32_t(has_color) << 2) |
                 (texcoord_location << 4) | (color_location << 12);
  auto it = stand_in_pixel_shaders_moved_.find(key);
  if (it != stand_in_pixel_shaders_moved_.end()) {
    return it->second;
  }
  const StandInPixelShaderCode& code =
      kStandInPixelShaderCode[shader_index][has_color ? 1 : 0];
  std::vector<uint32_t> words(code.words,
                              code.words + code.size_bytes / sizeof(uint32_t));
  VkShaderModule shader_module = VK_NULL_HANDLE;
  if (MoveStandInPixelShaderInputs(words, texcoord_location, color_location)) {
    shader_module = ui::vulkan::util::CreateShaderModule(
        command_processor_.GetVulkanDevice(), words.data(),
        words.size() * sizeof(uint32_t));
  }
  if (shader_module == VK_NULL_HANDLE) {
    XELOGW(
        "VulkanPipelineCache: Failed to make the {} stand-in pixel shader with "
        "its inputs at locations {} and {}",
        GetStandInPixelShaderName(shader), texcoord_location, color_location);
  }
  // A failure is stored too, so it is not tried again for every pipeline.
  stand_in_pixel_shaders_moved_.emplace(key, shader_module);
  return shader_module;
}

bool VulkanPipelineCache::EnsurePipelineCreatedWithInterpreterPlaceholder(
    const PipelineCreationArguments& creation_arguments, bool depth_only) {
  // hakuX-Boosted: the choice of the pixel shader module is its own function,
  // so that MakePlaceholder can hand it to a creation thread.
  return EnsurePipelineCreated(
      creation_arguments,
      GetInterpreterPlaceholderPixelShader(creation_arguments, depth_only),
      GetInterpreterVertexShader(false));
}

VkShaderModule VulkanPipelineCache::GetInterpreterPlaceholderPixelShader(
    const PipelineCreationArguments& creation_arguments, bool depth_only) {
  VkShaderModule placeholder_ps =
      (cvars::async_shader_vs_interpreter_debug_color &&
       placeholder_color_pixel_shader_ != VK_NULL_HANDLE)
          ? placeholder_color_pixel_shader_
          : placeholder_pixel_shader_;
  if (depth_only) {
    // hakuX-Boosted: the placeholder of a draw without a pixel shader
    // (vulkan_placeholder_depth_only_interpreter) writes depth as the real
    // pipeline will: with the same float24 conversion where that applies (the
    // condition is EnsurePipelineCreated's for a draw without a pixel shader),
    // otherwise with the no-op pixel shader, which is no pixel shader.
    const PipelineDescription& description = creation_arguments.pipeline->first;
    placeholder_ps = placeholder_pixel_shader_;
    if (render_target_cache_.GetPath() !=
            RenderTargetCache::Path::kPixelShaderInterlock &&
        render_target_cache_.depth_float24_convert_in_pixel_shader() &&
        (description.depth_write_enable ||
         description.depth_compare_op != xenos::CompareFunction::kAlways) &&
        (description.render_pass_key.depth_and_color_used & 0b1) &&
        description.render_pass_key.depth_format ==
            xenos::DepthRenderTargetFormat::kD24FS8) {
      VkShaderModule float24_ps = render_target_cache_.depth_float24_round()
                                      ? float24_round_fragment_shader_
                                      : float24_truncate_fragment_shader_;
      if (float24_ps != VK_NULL_HANDLE) {
        placeholder_ps = float24_ps;
      }
    }
  }
  return placeholder_ps;
}

bool VulkanPipelineCache::EnsurePipelineCreated(
    const PipelineCreationArguments& creation_arguments,
    VkShaderModule fragment_shader_override,
    VkShaderModule vertex_shader_override,
    const PipelineLayoutProvider* layout_override, bool publish_if_empty) {
  // Check if we already have a pipeline.
  // If it's a placeholder and we're not creating another placeholder,
  // we need to replace it with the real pipeline.
  VkPipeline existing_pipeline =
      creation_arguments.pipeline->second.pipeline.load(
          std::memory_order_acquire);
  bool is_placeholder = creation_arguments.pipeline->second.is_placeholder.load(
      std::memory_order_acquire);
  bool creating_placeholder = fragment_shader_override != VK_NULL_HANDLE;

  if (existing_pipeline != VK_NULL_HANDLE) {
    if (!is_placeholder || creating_placeholder) {
      // Already have a real pipeline, or trying to create another placeholder.
      // hakuX-Boosted: a late placeholder that the real pipeline beat. Its
      // maker set the placeholder flags before coming here; the real pipeline
      // is what is bound from now on.
      if (creating_placeholder && publish_if_empty &&
          existing_pipeline !=
              creation_arguments.pipeline->second.placeholder_pipeline.load(
                  std::memory_order_acquire)) {
        creation_arguments.pipeline->second.is_placeholder.store(
            false, std::memory_order_release);
        creation_arguments.pipeline->second.uses_interpreter.store(
            false, std::memory_order_release);
      }
      return true;
    }
    // Have a placeholder, and we're creating the real pipeline to replace it.
  }

  // This function preferably should validate the description to prevent
  // unsupported behavior that may be dangerous/crashing because pipelines can
  // be created from the disk storage.

  if (creation_arguments.pixel_shader) {
    XELOGGPU("Creating graphics pipeline state with VS {:016X}, PS {:016X}",
             creation_arguments.vertex_shader->shader().ucode_data_hash(),
             creation_arguments.pixel_shader->shader().ucode_data_hash());
  } else {
    XELOGGPU("Creating graphics pipeline state with VS {:016X}",
             creation_arguments.vertex_shader->shader().ucode_data_hash());
  }

  const PipelineDescription& description = creation_arguments.pipeline->first;
  if (!ArePipelineRequirementsMet(description)) {
    assert_always(
        "When creating a new pipeline, the description must not require "
        "unsupported features, and when loading the pipeline storage, "
        "pipelines with unsupported features must be filtered out");
    return false;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;

  bool is_tessellated =
      description.tessellation_mode != PipelineTessellationMode::kNone;

  // Up to 5 shader stages: VS, TCS, TES, GS, FS.
  std::array<VkPipelineShaderStageCreateInfo, 5> shader_stages;
  uint32_t shader_stage_count = 0;

  // Vertex shader or tessellation evaluation shader. The interpreter
  // placeholder substitutes its own fixed VS module, so the guest VS is
  // intentionally not translated yet in that case.
  if (vertex_shader_override == VK_NULL_HANDLE) {
    assert_true(creation_arguments.vertex_shader->is_translated());
    if (!creation_arguments.vertex_shader->is_valid()) {
      return false;
    }
  }

  if (is_tessellated) {
    // For tessellation: use our pre-compiled VS for passing data to TCS,
    // then TCS (hull shader), then the translated Xenos vertex shader as TES
    // (domain shader).

    // Tessellation vertex shader (passes indices to TCS).
    VkPipelineShaderStageCreateInfo& shader_stage_tess_vs =
        shader_stages[shader_stage_count++];
    shader_stage_tess_vs.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tess_vs.pNext = nullptr;
    shader_stage_tess_vs.flags = 0;
    shader_stage_tess_vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stage_tess_vs.module = creation_arguments.tessellation_vertex_shader;
    shader_stage_tess_vs.pName = "main";
    shader_stage_tess_vs.pSpecializationInfo = nullptr;

    // Tessellation control shader (hull shader).
    VkPipelineShaderStageCreateInfo& shader_stage_tcs =
        shader_stages[shader_stage_count++];
    shader_stage_tcs.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tcs.pNext = nullptr;
    shader_stage_tcs.flags = 0;
    shader_stage_tcs.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    shader_stage_tcs.module = creation_arguments.tessellation_control_shader;
    shader_stage_tcs.pName = "main";
    shader_stage_tcs.pSpecializationInfo = nullptr;

    // Tessellation evaluation shader (domain shader) - the translated Xenos
    // vertex shader.
    VkPipelineShaderStageCreateInfo& shader_stage_tes =
        shader_stages[shader_stage_count++];
    shader_stage_tes.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tes.pNext = nullptr;
    shader_stage_tes.flags = 0;
    shader_stage_tes.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    // GetOrCreateShaderModule, not shader_module(): the module is created
    // lazily after is_translated() is set, and another thread may have
    // translated the VS without creating the module yet, so shader_module()
    // could still be null.
    shader_stage_tes.module =
        creation_arguments.vertex_shader->GetOrCreateShaderModule();
    if (shader_stage_tes.module == VK_NULL_HANDLE) {
      return false;
    }
    shader_stage_tes.pName = "main";
    shader_stage_tes.pSpecializationInfo = nullptr;
  } else {
    // Non-tessellated: standard vertex shader.
    VkPipelineShaderStageCreateInfo& shader_stage_vertex =
        shader_stages[shader_stage_count++];
    shader_stage_vertex.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_vertex.pNext = nullptr;
    shader_stage_vertex.flags = 0;
    shader_stage_vertex.stage = VK_SHADER_STAGE_VERTEX_BIT;
    // GetOrCreateShaderModule, not shader_module() - see the TES note above.
    shader_stage_vertex.module =
        vertex_shader_override != VK_NULL_HANDLE
            ? vertex_shader_override
            : creation_arguments.vertex_shader->GetOrCreateShaderModule();
    if (shader_stage_vertex.module == VK_NULL_HANDLE) {
      return false;
    }
    shader_stage_vertex.pName = "main";
    shader_stage_vertex.pSpecializationInfo = nullptr;
  }

  // Geometry shader.
  if (creation_arguments.geometry_shader != VK_NULL_HANDLE) {
    VkPipelineShaderStageCreateInfo& shader_stage_geometry =
        shader_stages[shader_stage_count++];
    shader_stage_geometry.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_geometry.pNext = nullptr;
    shader_stage_geometry.flags = 0;
    shader_stage_geometry.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    shader_stage_geometry.module = creation_arguments.geometry_shader;
    shader_stage_geometry.pName = "main";
    shader_stage_geometry.pSpecializationInfo = nullptr;
  }
  // Fragment shader.
  VkPipelineShaderStageCreateInfo& shader_stage_fragment =
      shader_stages[shader_stage_count++];
  shader_stage_fragment.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stage_fragment.pNext = nullptr;
  shader_stage_fragment.flags = 0;
  shader_stage_fragment.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stage_fragment.module = VK_NULL_HANDLE;
  shader_stage_fragment.pName = "main";
  shader_stage_fragment.pSpecializationInfo = nullptr;
  if (fragment_shader_override != VK_NULL_HANDLE) {
    // Use the override shader (for placeholder pipelines).
    shader_stage_fragment.module = fragment_shader_override;
  } else if (creation_arguments.pixel_shader) {
    assert_true(creation_arguments.pixel_shader->is_translated());
    if (!creation_arguments.pixel_shader->is_valid()) {
      return false;
    }
    shader_stage_fragment.module =
        creation_arguments.pixel_shader->GetOrCreateShaderModule();
    if (shader_stage_fragment.module == VK_NULL_HANDLE) {
      return false;
    }
  } else {
    if (edram_fragment_shader_interlock) {
      shader_stage_fragment.module = depth_only_fragment_shader_;
    } else if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
               (description.depth_write_enable ||
                description.depth_compare_op !=
                    xenos::CompareFunction::kAlways) &&
               (description.render_pass_key.depth_and_color_used & 0b1) &&
               description.render_pass_key.depth_format ==
                   xenos::DepthRenderTargetFormat::kD24FS8) {
      // No guest pixel shader, but depth matters and the host buffer is
      // float24 - bind a substitute that converts gl_FragCoord.z so the
      // depth buffer encoding stays consistent with PS-converted draws.
      shader_stage_fragment.module = render_target_cache_.depth_float24_round()
                                         ? float24_round_fragment_shader_
                                         : float24_truncate_fragment_shader_;
    }
  }
  if (shader_stage_fragment.module == VK_NULL_HANDLE) {
    --shader_stage_count;
  }

  VkPipelineVertexInputStateCreateInfo vertex_input_state = {};
  vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  switch (description.primitive_topology) {
    case PipelinePrimitiveTopology::kPointList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kTriangleStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleFan:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      break;
    case PipelinePrimitiveTopology::kLineListWithAdjacency:
      input_assembly_state.topology =
          VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kPatchList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    default:
      assert_unhandled_case(description.primitive_topology);
      return false;
  }
  input_assembly_state.primitiveRestartEnable =
      description.primitive_restart ? VK_TRUE : VK_FALSE;
  // When topology is dynamic but may not cross its class, the description was
  // canonicalized to kPointList with the real class kept in topology_class.
  // The pipeline must be baked with a topology of that class so dynamic
  // topology stays within it (Vulkan ignores the concrete value otherwise).
  const DynamicStateCapabilities& dyn_caps = dynamic_state_capabilities_;
  if (dyn_caps.extended_dynamic_state &&
      !dyn_caps.primitive_topology_unrestricted) {
    switch (description.topology_class) {
      case 0:
        input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        break;
      case 1:
        input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        break;
      case 3:
        input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        break;
      default:
        input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
    }
  }

  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.depthClampEnable =
      description.depth_clamp_enable ? VK_TRUE : VK_FALSE;
  switch (description.polygon_mode) {
    case PipelinePolygonMode::kFill:
      rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
      break;
    case PipelinePolygonMode::kLine:
      rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
      break;
    case PipelinePolygonMode::kPoint:
      rasterization_state.polygonMode = VK_POLYGON_MODE_POINT;
      break;
    default:
      assert_unhandled_case(description.polygon_mode);
      return false;
  }
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  if (description.cull_front) {
    rasterization_state.cullMode |= VK_CULL_MODE_FRONT_BIT;
  }
  if (description.cull_back) {
    rasterization_state.cullMode |= VK_CULL_MODE_BACK_BIT;
  }
  rasterization_state.frontFace = description.front_face_clockwise
                                      ? VK_FRONT_FACE_CLOCKWISE
                                      : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Depth bias is dynamic (even toggling - pipeline creation is expensive).
  // "If no depth attachment is present, r is undefined" in the depth bias
  // formula, though Z has no effect on anything if a depth attachment is not
  // used (the guest shader can't access Z), enabling only when there's a
  // depth / stencil attachment for correctness.
  rasterization_state.depthBiasEnable =
      (!edram_fragment_shader_interlock &&
       (description.render_pass_key.depth_and_color_used & 0b1))
          ? VK_TRUE
          : VK_FALSE;
  // TODO(Triang3l): Wide lines.
  rasterization_state.lineWidth = 1.0f;

  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  if (description.render_pass_key.msaa_samples == xenos::MsaaSamples::k2X &&
      !render_target_cache_.IsMsaa2xSupported(
          !edram_fragment_shader_interlock &&
          description.render_pass_key.depth_and_color_used != 0)) {
    // Using sample 0 as 0 and 3 as 1 for 2x instead (not exactly the same
    // sample locations, but still top-left and bottom-right - however, this can
    // be adjusted with custom sample locations).
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    sample_mask = 0b1001;
    // TODO(Triang3l): Research sample mask behavior without attachments (in
    // Direct3D, it's completely ignored in this case).
    multisample_state.pSampleMask = &sample_mask;
  } else {
    multisample_state.rasterizationSamples = VkSampleCountFlagBits(
        uint32_t(1) << uint32_t(description.render_pass_key.msaa_samples));
  }

  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil_state.pNext = nullptr;
  if (!edram_fragment_shader_interlock) {
    if (description.depth_write_enable ||
        description.depth_compare_op != xenos::CompareFunction::kAlways) {
      depth_stencil_state.depthTestEnable = VK_TRUE;
      depth_stencil_state.depthWriteEnable =
          description.depth_write_enable ? VK_TRUE : VK_FALSE;
      depth_stencil_state.depthCompareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                      uint32_t(description.depth_compare_op));
    }
    if (description.stencil_test_enable) {
      depth_stencil_state.stencilTestEnable = VK_TRUE;
      depth_stencil_state.front.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_front_fail_op));
      depth_stencil_state.front.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_front_pass_op));
      depth_stencil_state.front.depthFailOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_front_depth_fail_op));
      depth_stencil_state.front.compareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                      uint32_t(description.stencil_front_compare_op));
      depth_stencil_state.back.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_back_fail_op));
      depth_stencil_state.back.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_back_pass_op));
      depth_stencil_state.back.depthFailOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) +
                      uint32_t(description.stencil_back_depth_fail_op));
      depth_stencil_state.back.compareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) +
                      uint32_t(description.stencil_back_compare_op));
    }
  }

  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  VkPipelineColorBlendAttachmentState
      color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  if (!edram_fragment_shader_interlock) {
    uint32_t color_rts_used =
        description.render_pass_key.depth_and_color_used >> 1;
    {
      static constexpr VkBlendFactor kBlendFactorMap[] = {
          VK_BLEND_FACTOR_ZERO,
          VK_BLEND_FACTOR_ONE,
          VK_BLEND_FACTOR_SRC_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
          VK_BLEND_FACTOR_DST_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
          VK_BLEND_FACTOR_SRC_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          VK_BLEND_FACTOR_DST_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
          VK_BLEND_FACTOR_CONSTANT_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
          VK_BLEND_FACTOR_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
      };
      // 8 entries for safety since 3 bits from the guest are passed directly.
      static constexpr VkBlendOp kBlendOpMap[] = {VK_BLEND_OP_ADD,
                                                  VK_BLEND_OP_SUBTRACT,
                                                  VK_BLEND_OP_MIN,
                                                  VK_BLEND_OP_MAX,
                                                  VK_BLEND_OP_REVERSE_SUBTRACT,
                                                  VK_BLEND_OP_ADD,
                                                  VK_BLEND_OP_ADD,
                                                  VK_BLEND_OP_ADD};
      // Check if the shader pre-multiplies by blend factors for MIN/MAX.
      SpirvShaderTranslator::Modification pixel_shader_modification(
          description.pixel_shader_modification);
      bool rt0_rgb_premult =
          pixel_shader_modification.pixel.rt0_blend_rgb_factor_for_premult !=
          xenos::BlendFactor::kOne;
      bool rt0_a_premult =
          pixel_shader_modification.pixel.rt0_blend_a_factor_for_premult !=
          xenos::BlendFactor::kOne;

      uint32_t color_rts_remaining = color_rts_used;
      uint32_t color_rt_index;
      while (xe::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
        color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
        VkPipelineColorBlendAttachmentState& color_blend_attachment =
            color_blend_attachments[color_rt_index];
        const PipelineRenderTarget& color_rt =
            description.render_targets[color_rt_index];
        if (color_rt.src_color_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_color_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.color_blend_op != xenos::BlendOp::kAdd ||
            color_rt.src_alpha_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_alpha_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.alpha_blend_op != xenos::BlendOp::kAdd) {
          color_blend_attachment.blendEnable = VK_TRUE;
          color_blend_attachment.srcColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_color_blend_factor)];
          color_blend_attachment.dstColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_color_blend_factor)];
          color_blend_attachment.colorBlendOp =
              kBlendOpMap[uint32_t(color_rt.color_blend_op)];
          color_blend_attachment.srcAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_alpha_blend_factor)];
          color_blend_attachment.dstAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_alpha_blend_factor)];
          color_blend_attachment.alphaBlendOp =
              kBlendOpMap[uint32_t(color_rt.alpha_blend_op)];

          // If the shader pre-multiplies by the source blend factor for RT0
          // MIN/MAX, set the pipeline source factor to ONE since it's already
          // applied in the shader.
          if (color_rt_index == 0) {
            if (rt0_rgb_premult) {
              color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            }
            if (rt0_a_premult) {
              color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            }
          }
        }
        color_blend_attachment.colorWriteMask =
            VkColorComponentFlags(color_rt.color_write_mask);
      }
    }
    color_blend_state.attachmentCount = 32 - xe::lzcnt(color_rts_used);
    color_blend_state.pAttachments = color_blend_attachments;
  }

  std::array<VkDynamicState, 24> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  // Regardless of whether some of this state actually has any effect on the
  // pipeline, marking all as dynamic because otherwise, binding any pipeline
  // with such state not marked as dynamic will cause the dynamic state to be
  // invalidated (again, even if it has no effect).
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (!edram_fragment_shader_interlock) {
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_DEPTH_BIAS;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_REFERENCE;
  }
  // Extended dynamic state - must exactly match the fields canonicalized out of
  // the pipeline key (see CanonicalizePipelineDescription) and emitted per draw
  // by VulkanCommandProcessor::UpdateDynamicState.
  if (dyn_caps.extended_dynamic_state) {
    // EDS1 / EDS2 (core in Vulkan 1.3).
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_CULL_MODE;
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_FRONT_FACE;
    if (!edram_fragment_shader_interlock) {
      dynamic_states[dynamic_state.dynamicStateCount++] =
          VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE;
      dynamic_states[dynamic_state.dynamicStateCount++] =
          VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE;
      dynamic_states[dynamic_state.dynamicStateCount++] =
          VK_DYNAMIC_STATE_DEPTH_COMPARE_OP;
      dynamic_states[dynamic_state.dynamicStateCount++] =
          VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE;
      dynamic_states[dynamic_state.dynamicStateCount++] =
          VK_DYNAMIC_STATE_STENCIL_OP;
    }
    // EDS3 sub-features (each gated separately).
    if (dyn_caps.depth_clamp_enable) {
      dynamic_states[dynamic_state.dynamicStateCount++] =
          VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    }
    if (dyn_caps.polygon_mode) {
      dynamic_states[dynamic_state.dynamicStateCount++] =
          VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
    }
    if (!edram_fragment_shader_interlock) {
      if (dyn_caps.color_blend_enable) {
        dynamic_states[dynamic_state.dynamicStateCount++] =
            VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
      }
      if (dyn_caps.color_blend_equation) {
        dynamic_states[dynamic_state.dynamicStateCount++] =
            VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
      }
      if (dyn_caps.color_write_mask) {
        dynamic_states[dynamic_state.dynamicStateCount++] =
            VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
      }
    }
  }
  assert_true(dynamic_state.dynamicStateCount <= dynamic_states.size());

  // Tessellation state (only used when tessellation is active).
  VkPipelineTessellationStateCreateInfo tessellation_state = {};
  tessellation_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
  if (is_tessellated) {
    // Determine patch control point count based on mode and patch type.
    // For adaptive mode, we use the actual patch corner count (3 for triangles,
    // 4 for quads) since each control point has its own edge factor.
    // For discrete/continuous modes, we use 1 control point (the Xenos vertex
    // shader receives the patch index and computes all corners internally).
    // hakuX-Boosted: 1 only if the domain is patch-indexed. For a
    // control-point-indexed one (a tessellated triangle or quad list) it's the
    // corner count too: the evaluation shader reads an index per corner, and
    // with 1 every index was a patch of its own and the other corners' indices
    // were read out of bounds.
    tessellation_state.patchControlPoints =
        GetTessellationPatchControlPointCount(description);
  }

  // Dynamic rendering support (VK_KHR_dynamic_rendering / Vulkan 1.3).
  VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {};
  VkFormat color_attachment_formats[xenos::kMaxColorRenderTargets] = {};
  bool use_dynamic_rendering = cvars::vulkan_dynamic_rendering &&
                               vulkan_device->properties().dynamicRendering;

  if (use_dynamic_rendering) {
    pipeline_rendering_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VulkanRenderTargetCache::RenderPassKey key =
        creation_arguments.render_pass_key;

    // Set up color attachment formats.
    xenos::ColorRenderTargetFormat color_formats[] = {
        key.color_0_view_format, key.color_1_view_format,
        key.color_2_view_format, key.color_3_view_format};
    uint32_t color_attachment_count = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (key.depth_and_color_used & (1 << (1 + i))) {
        color_attachment_formats[i] =
            render_target_cache_.GetColorVulkanFormat(color_formats[i]);
        color_attachment_count = i + 1;
      }
    }
    pipeline_rendering_create_info.colorAttachmentCount =
        color_attachment_count;
    pipeline_rendering_create_info.pColorAttachmentFormats =
        color_attachment_formats;

    // Set up depth/stencil format.
    if (key.depth_and_color_used & 0b1) {
      VkFormat depth_format =
          render_target_cache_.GetDepthVulkanFormat(key.depth_format);
      pipeline_rendering_create_info.depthAttachmentFormat = depth_format;
      pipeline_rendering_create_info.stencilAttachmentFormat = depth_format;
    }
  }

  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext =
      use_dynamic_rendering ? &pipeline_rendering_create_info : nullptr;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stageCount = shader_stage_count;
  pipeline_create_info.pStages = shader_stages.data();
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState =
      is_tessellated ? &tessellation_state : nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  // hakuX-Boosted: a stand-in placeholder brings its own layout.
  pipeline_create_info.layout =
      layout_override ? layout_override->GetPipelineLayout()
                      : creation_arguments.pipeline->second.pipeline_layout
                            .load(std::memory_order_acquire)
                            ->GetPipelineLayout();
  pipeline_create_info.renderPass =
      use_dynamic_rendering ? VK_NULL_HANDLE : creation_arguments.render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // Count this pipeline compile for the debug overlay's "compiling" counter
  // (decremented automatically when this scope exits).
  xe::ScopedShaderCompile compile_guard;
  bool profile = cvars::shader_profiling;
  std::chrono::steady_clock::time_point pso_create_start;
  if (profile) {
    pso_create_start = std::chrono::steady_clock::now();
  }
  VkPipeline pipeline;
  VkResult result = dfn.vkCreateGraphicsPipelines(
      device, vk_pipeline_cache_, 1, &pipeline_create_info, nullptr, &pipeline);
  if (result == VK_SUCCESS) {
    // Mark the persistent VkPipelineCache dirty so EndSubmission re-saves it.
    vk_pipeline_cache_dirty_.store(true, std::memory_order_relaxed);
    if (profile) {
      // Driver SPIR-V->ISA compile + link time.
      XELOGI(
          "shader_profiling: pipeline create ({}) VS {:016X} PS {:016X} {:.3f} "
          "ms",
          creating_placeholder ? "placeholder" : "real",
          creation_arguments.vertex_shader->shader().ucode_data_hash(),
          creation_arguments.pixel_shader
              ? creation_arguments.pixel_shader->shader().ucode_data_hash()
              : 0,
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - pso_create_start)
              .count());
      // hakuX-Boosted: what the placeholder was made of and where, so that a
      // slow one can be put down to the interpreter's module or to a guest
      // vertex shader's (boosted/notes/shader-ahead.md).
      if (creating_placeholder) {
        const bool interpreter_vs = vertex_shader_override != VK_NULL_HANDLE;
        size_t vertex_spirv_bytes =
            interpreter_vs
                ? (vertex_shader_override == ucode_program_vs_
                       ? sizeof(shaders::ucode_program_vs)
                       : (vertex_shader_override ==
                                  ucode_interpreter_stand_in_vs_
                              ? sizeof(shaders::ucode_interpreter_stand_in_vs)
                              : sizeof(shaders::ucode_interpreter_vs)))
                : creation_arguments.vertex_shader->translated_binary().size();
        XELOGI(
            "shader_profiling: placeholder detail: VS {:016X}: {} vertex "
            "shader, {} bytes of SPIR-V, {}{}{}",
            creation_arguments.vertex_shader->shader().ucode_data_hash(),
            interpreter_vs ? (vertex_shader_override == ucode_program_vs_
                                  ? "interpreter 2"
                                  : "interpreter")
                           : "translated guest",
            vertex_spirv_bytes,
            layout_override ? "stand-in pixel shader" : "no-op pixel shader",
            creation_arguments.geometry_shader != VK_NULL_HANDLE
                ? ", geometry shader"
                : "",
            creation_arguments.kind == PipelineCreationArguments::Kind::kPlaceholder
                ? ", made on a creation thread"
                : (publish_if_empty ? ", made late on the GPU command thread"
                                    : ", made on the GPU command thread"));
      }
    }
  }
  if (result != VK_SUCCESS) {
    if (creation_arguments.pixel_shader) {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X}, PS {:016X} "
          "(tessellated={}, result={})",
          creation_arguments.vertex_shader->shader().ucode_data_hash(),
          creation_arguments.pixel_shader->shader().ucode_data_hash(),
          is_tessellated, static_cast<int>(result));
    } else {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X} "
          "(tessellated={}, result={})",
          creation_arguments.vertex_shader->shader().ucode_data_hash(),
          is_tessellated, static_cast<int>(result));
    }
    return false;
  }

  // Publish the new pipeline. Each slot is normally created exactly once, but
  // if a concurrent create slipped through (e.g. the storage preload and a
  // draw racing on the same description), defer the destruction of the
  // replaced handle until the GPU is done with any submission that may
  // reference it.
  //
  // Record the placeholder handle before publishing it, so a draw that observes
  // this pipeline handle also observes it as the placeholder (see Pipeline::
  // placeholder_pipeline). Stored before the exchange so the release on the
  // exchange carries it.
  if (creating_placeholder) {
    creation_arguments.pipeline->second.placeholder_pipeline.store(
        pipeline, std::memory_order_release);
  }
  // hakuX-Boosted: a placeholder made late or on a creation thread can lose
  // the race with the slot's real pipeline. It only takes an empty slot; if
  // the real pipeline is already there it is destroyed unused.
  if (creating_placeholder && publish_if_empty) {
    VkPipeline expected_pipeline = VK_NULL_HANDLE;
    if (!creation_arguments.pipeline->second.pipeline.compare_exchange_strong(
            expected_pipeline, pipeline, std::memory_order_acq_rel)) {
      creation_arguments.pipeline->second.placeholder_pipeline.store(
          VK_NULL_HANDLE, std::memory_order_release);
      creation_arguments.pipeline->second.is_placeholder.store(
          false, std::memory_order_release);
      creation_arguments.pipeline->second.uses_interpreter.store(
          false, std::memory_order_release);
      uint64_t unused_submission = command_processor_.GetCurrentSubmission();
      std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
      deferred_destroy_pipelines_.emplace_back(pipeline, unused_submission);
    }
    return true;
  }
  VkPipeline old_pipeline =
      creation_arguments.pipeline->second.pipeline.exchange(
          pipeline, std::memory_order_acq_rel);
  if (old_pipeline != VK_NULL_HANDLE) {
    uint64_t current_submission = command_processor_.GetCurrentSubmission();
    {
      std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
      deferred_destroy_pipelines_.emplace_back(old_pipeline,
                                               current_submission);
    }
  }

  // Mark as no longer a placeholder (for the case where we just created real).
  if (!creating_placeholder) {
    // hakuX-Boosted: when a placeholder gives way to the real pipeline.
    if (cvars::shader_profiling && old_pipeline != VK_NULL_HANDLE &&
        old_pipeline ==
            creation_arguments.pipeline->second.placeholder_pipeline.load(
                std::memory_order_acquire)) {
      XELOGI(
          "shader_profiling: placeholder swapped for the real pipeline: VS "
          "{:016X}, PS {:016X}, {} vertex shader, {} pixel shader, {} ms after "
          "it was made, {} draw(s) recorded with it",
          creation_arguments.vertex_shader->shader().ucode_data_hash(),
          creation_arguments.pixel_shader
              ? creation_arguments.pixel_shader->shader().ucode_data_hash()
              : 0,
          creation_arguments.pipeline->second.uses_interpreter.load(
              std::memory_order_acquire)
              ? "interpreter"
              : "translated",
          GetStandInPixelShaderName(
              StandIn::Unpack(
                  creation_arguments.pipeline->second.placeholder_stand_in
                      .load(std::memory_order_acquire))
                  .shader),
          xe::Clock::QueryHostUptimeMillis() -
              creation_arguments.pipeline->second.placeholder_created_ms,
          creation_arguments.pipeline->second.placeholder_draws.load(
              std::memory_order_relaxed));
    }
    creation_arguments.pipeline->second.is_placeholder.store(
        false, std::memory_order_release);
    XELOGI("Pipeline created for VS {:016X}, PS {:016X}",
           creation_arguments.vertex_shader->shader().ucode_data_hash(),
           creation_arguments.pixel_shader
               ? creation_arguments.pixel_shader->shader().ucode_data_hash()
               : 0);
  }

  return true;
}

void VulkanPipelineCache::ProcessDeferredDestructions() {
  std::vector<VkPipeline> pipelines_to_destroy;

  uint64_t completed_submission = command_processor_.GetCompletedSubmission();

  {
    std::lock_guard<std::mutex> lock(deferred_destroy_mutex_);
    if (deferred_destroy_pipelines_.empty()) {
      return;
    }

    // Only destroy pipelines whose submission has completed on the GPU.
    // Keep pipelines that are still potentially in-flight.
    auto it = deferred_destroy_pipelines_.begin();
    while (it != deferred_destroy_pipelines_.end()) {
      if (it->second <= completed_submission) {
        // This submission has completed, safe to destroy.
        pipelines_to_destroy.push_back(it->first);
        it = deferred_destroy_pipelines_.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (pipelines_to_destroy.empty()) {
    return;
  }

  // Destroy pipelines now that we know GPU is done with them.
  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  for (VkPipeline pipeline : pipelines_to_destroy) {
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
}

void VulkanPipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  ShutdownShaderStorage();

  shader_storage_title_id_ = title_id;

  if (!blocking) {
    startup_loading_ = true;
    if (completion_callback) {
      completion_callback = [this, orig = std::move(completion_callback)]() {
        startup_loading_ = false;
        orig();
      };
    } else {
      completion_callback = [this]() { startup_loading_ = false; };
    }
  }

  bool edram_fsi_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  ShaderStorageWriter<PipelineStoredDescription>::PipelineStorageConfig
      pipeline_config;
  pipeline_config.file_suffix =
      fmt::format(".{}.vk.xpso", edram_fsi_used ? "fsi" : "fbo");
  pipeline_config.api_magic = kPipelineStorageAPIMagicVulkan;
  pipeline_config.version =
      std::max(PipelineDescription::kVersion,
               SpirvShaderTranslator::Modification::kVersion);

  uint32_t storage_index = storage_writer_.storage_index() + 1;

  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
  if (!storage_writer_.InitializeShaderStorage(
          cache_root, title_id, pipeline_config,
          // Shader load callback.
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
              uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            VulkanShader* shader =
                LoadShader(type, ucode_dwords, ucode_dword_count);
            if (!shader || shader->ucode_storage_index() == storage_index) {
              return true;  // Continue reading.
            }
            shader->set_ucode_storage_index(storage_index);
            if (!shader->is_ucode_analyzed()) {
              shader->AnalyzeUcode(ucode_disasm_buffer_);
            }
            return true;
          },
          // Shader translate callback - handles parallel translation.
          [this, edram_fsi_used](const std::set<std::pair<uint64_t, uint64_t>>
                                     & translations_needed) {
            TranslateShadersForStorage(translations_needed, edram_fsi_used);
          },
          pipeline_stored_descriptions)) {
    if (completion_callback) {
      completion_callback();
    }
    return;
  }
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;

  // Load VkPipelineCache from disk if available.
  auto shader_storage_local_root = GetShaderStorageLocalRoot(cache_root);
  if (!std::filesystem::exists(shader_storage_local_root)) {
    std::error_code ec;
    std::filesystem::create_directories(shader_storage_local_root, ec);
  }
  vk_pipeline_cache_path_ =
      shader_storage_local_root /
      fmt::format("{:08X}.vk.bin", shader_storage_title_id_);

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Try to load existing VkPipelineCache data.
  std::vector<uint8_t> pipeline_cache_data;
  if (FILE* cache_file =
          xe::filesystem::OpenFile(vk_pipeline_cache_path_, "rb")) {
    xe::filesystem::Seek(cache_file, 0, SEEK_END);
    int64_t cache_size = xe::filesystem::Tell(cache_file);
    if (cache_size > 0) {
      pipeline_cache_data.resize(size_t(cache_size));
      xe::filesystem::Seek(cache_file, 0, SEEK_SET);
      pipeline_cache_data.resize(fread(pipeline_cache_data.data(), 1,
                                       pipeline_cache_data.size(), cache_file));
    }
    fclose(cache_file);
    XELOGI("Loaded {} bytes of VkPipelineCache data",
           pipeline_cache_data.size());
  }

  // Recreate the VkPipelineCache with the loaded data.
  if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineCache(device, vk_pipeline_cache_, nullptr);
    vk_pipeline_cache_ = VK_NULL_HANDLE;
  }
  VkPipelineCacheCreateInfo pipeline_cache_create_info = {};
  pipeline_cache_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  pipeline_cache_create_info.initialDataSize = pipeline_cache_data.size();
  pipeline_cache_create_info.pInitialData =
      pipeline_cache_data.empty() ? nullptr : pipeline_cache_data.data();
  if (dfn.vkCreatePipelineCache(device, &pipeline_cache_create_info, nullptr,
                                &vk_pipeline_cache_) != VK_SUCCESS) {
    XELOGW(
        "VulkanPipelineCache: Failed to create pipeline cache with "
        "initial data, creating empty cache");
    vk_pipeline_cache_ = VK_NULL_HANDLE;
    pipeline_cache_create_info.initialDataSize = 0;
    pipeline_cache_create_info.pInitialData = nullptr;
    dfn.vkCreatePipelineCache(device, &pipeline_cache_create_info, nullptr,
                              &vk_pipeline_cache_);
  }

  // Create pipelines from stored descriptions.
  if (!cvars::pipeline_storage_precreate) {
    XELOGI("Pipeline cache: skipping pre-creation of {} stored pipelines",
           pipeline_stored_descriptions.size());
    pipeline_stored_descriptions.clear();
  }
  if (!pipeline_stored_descriptions.empty()) {
    uint64_t pipeline_creation_start = xe::Clock::QueryHostTickCount();

    size_t pipelines_created = 0;
    size_t pipelines_already_exist = 0;
    size_t pipelines_vs_not_found = 0;
    size_t pipelines_vs_translation_missing = 0;
    size_t pipelines_ps_not_found = 0;
    size_t pipelines_ps_translation_missing = 0;
    size_t pipelines_render_pass_failed = 0;
    size_t pipelines_layout_failed = 0;
    size_t pipelines_requirements_not_met = 0;

    for (const PipelineStoredDescription& pipeline_stored_description :
         pipeline_stored_descriptions) {
      const PipelineDescription& pipeline_description =
          pipeline_stored_description.description;

      // Skip pipelines not supported by this device.
      if (!ArePipelineRequirementsMet(pipeline_description)) {
        ++pipelines_requirements_not_met;
        continue;
      }

      // Skip already known pipelines.
      auto it = pipelines_.find(pipeline_description);
      if (it != pipelines_.end()) {
        ++pipelines_already_exist;
        continue;
      }

      // Look up vertex shader.
      auto vertex_shader_it =
          shaders_.find(pipeline_description.vertex_shader_hash);
      if (vertex_shader_it == shaders_.end()) {
        ++pipelines_vs_not_found;
        continue;
      }
      VulkanShader* vertex_shader = vertex_shader_it->second;
      VulkanShader::VulkanTranslation* vertex_translation =
          static_cast<VulkanShader::VulkanTranslation*>(
              vertex_shader->GetTranslation(
                  pipeline_description.vertex_shader_modification));
      if (!vertex_translation || !vertex_translation->is_translated() ||
          !vertex_translation->is_valid()) {
        ++pipelines_vs_translation_missing;
        continue;
      }

      // Look up pixel shader if present.
      VulkanShader* pixel_shader = nullptr;
      VulkanShader::VulkanTranslation* pixel_translation = nullptr;
      if (pipeline_description.pixel_shader_hash) {
        auto pixel_shader_it =
            shaders_.find(pipeline_description.pixel_shader_hash);
        if (pixel_shader_it == shaders_.end()) {
          ++pipelines_ps_not_found;
          continue;
        }
        pixel_shader = pixel_shader_it->second;
        pixel_translation = static_cast<VulkanShader::VulkanTranslation*>(
            pixel_shader->GetTranslation(
                pipeline_description.pixel_shader_modification));
        if (!pixel_translation || !pixel_translation->is_translated() ||
            !pixel_translation->is_valid()) {
          ++pipelines_ps_translation_missing;
          continue;
        }
      }

      // Get render pass.
      VkRenderPass render_pass =
          render_target_cache_.GetPath() ==
                  RenderTargetCache::Path::kPixelShaderInterlock
              ? render_target_cache_.GetFragmentShaderInterlockRenderPass()
              : render_target_cache_.GetHostRenderTargetsRenderPass(
                    pipeline_description.render_pass_key);
      if (render_pass == VK_NULL_HANDLE) {
        ++pipelines_render_pass_failed;
        continue;
      }

      // Get pipeline layout.
      const PipelineLayoutProvider* pipeline_layout =
          command_processor_.GetPipelineLayout(
              pixel_shader
                  ? pixel_shader->GetTextureBindingsAfterTranslation().size()
                  : 0,
              pixel_shader
                  ? pixel_shader->GetSamplerBindingsAfterTranslation().size()
                  : 0,
              vertex_shader->GetTextureBindingsAfterTranslation().size(),
              vertex_shader->GetSamplerBindingsAfterTranslation().size());
      if (!pipeline_layout) {
        ++pipelines_layout_failed;
        continue;
      }

      // Get geometry shader if needed.
      VkShaderModule geometry_shader = VK_NULL_HANDLE;
      if (pipeline_description.geometry_shader !=
          PipelineGeometryShader::kNone) {
        GeometryShaderKey geometry_shader_key;
        GuestSpirvShaderCache::GetGeometryShaderKey(
            pipeline_description.geometry_shader,
            vertex_translation->modification(),
            pixel_translation ? pixel_translation->modification() : 0,
            geometry_shader_key);
        geometry_shader = GetGeometryShader(geometry_shader_key);
        if (geometry_shader == VK_NULL_HANDLE) {
          continue;
        }
      }

      // Get tessellation shaders if needed.
      VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
      VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
      if (pipeline_description.tessellation_mode !=
          PipelineTessellationMode::kNone) {
        tessellation_vertex_shader =
            GetTessellationVertexShader(pipeline_description.tessellation_mode);
        // hakuX-Boosted: as in ConfigurePipeline, the 3cp / 4cp control
        // shaders for the control-point-indexed domains.
        bool use_control_point_count =
            (pipeline_description.tessellation_mode ==
             PipelineTessellationMode::kAdaptive) ||
            IsTessellationControlPointIndexed(pipeline_description);
        tessellation_control_shader = GetTessellationControlShader(
            pipeline_description.tessellation_mode,
            pipeline_description.tessellation_patch, use_control_point_count);
        if (tessellation_vertex_shader == VK_NULL_HANDLE ||
            tessellation_control_shader == VK_NULL_HANDLE) {
          continue;
        }
      }

      // Create the pipeline entry.
      auto& pipeline_pair =
          *pipelines_.emplace(pipeline_description, Pipeline(pipeline_layout))
               .first;

      // Queue for creation.
      if (!creation_threads_.empty()) {
        // Calculate priority based on whether shader writes to visible RTs.
        uint8_t priority = 0;
        if (pixel_shader) {
          uint32_t bound_rts =
              (pipeline_description.render_targets[0].color_write_mask ? 1
                                                                       : 0) |
              (pipeline_description.render_targets[1].color_write_mask ? 2
                                                                       : 0) |
              (pipeline_description.render_targets[2].color_write_mask ? 4
                                                                       : 0) |
              (pipeline_description.render_targets[3].color_write_mask ? 8 : 0);
          priority = pipeline_util::CalculatePipelinePriority(
              bound_rts, pixel_shader->writes_color_targets(),
              pixel_shader->writes_depth());
        }

        std::lock_guard<std::mutex> lock(creation_request_lock_);
        PipelineCreationArguments creation_arguments;
        creation_arguments.pipeline = &pipeline_pair;
        creation_arguments.vertex_shader = vertex_translation;
        creation_arguments.pixel_shader = pixel_translation;
        creation_arguments.geometry_shader = geometry_shader;
        creation_arguments.tessellation_vertex_shader =
            tessellation_vertex_shader;
        creation_arguments.tessellation_control_shader =
            tessellation_control_shader;
        creation_arguments.render_pass = render_pass;
        creation_arguments.render_pass_key =
            pipeline_description.render_pass_key;
        creation_arguments.priority = priority;
        creation_queue_.push(creation_arguments);
        creation_request_cond_.notify_one();
      } else {
        // No creation threads - create synchronously.
        PipelineCreationArguments creation_arguments;
        creation_arguments.pipeline = &pipeline_pair;
        creation_arguments.vertex_shader = vertex_translation;
        creation_arguments.pixel_shader = pixel_translation;
        creation_arguments.geometry_shader = geometry_shader;
        creation_arguments.tessellation_vertex_shader =
            tessellation_vertex_shader;
        creation_arguments.tessellation_control_shader =
            tessellation_control_shader;
        creation_arguments.render_pass = render_pass;
        creation_arguments.render_pass_key =
            pipeline_description.render_pass_key;
        EnsurePipelineCreated(creation_arguments);
      }

      ++pipelines_created;
    }

    if (!creation_threads_.empty()) {
      if (blocking) {
        // Blocking mode: wait for all pipelines to be created.
        bool await_creation_completion_event;
        {
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          await_creation_completion_event =
              !creation_queue_.empty() || creation_threads_busy_ != 0;
          if (await_creation_completion_event) {
            creation_completion_event_->Reset();
            creation_completion_set_event_.store(true,
                                                 std::memory_order_release);
          }
        }
        if (await_creation_completion_event) {
          creation_request_cond_.notify_one();
          xe::threading::Wait(creation_completion_event_.get(), false);
        }
      } else {
        // Non-blocking mode: store callback for later invocation.
        std::lock_guard<std::mutex> lock(creation_request_lock_);
        if (creation_queue_.empty() && creation_threads_busy_ == 0) {
          // No work pending - callback will be invoked at end of function.
        } else {
          creation_completion_callback_ = std::move(completion_callback);
          completion_callback = nullptr;  // Prevent invocation at end
        }
      }
    }

    XELOGI("Pipeline cache: {} created, {} already exist, {} total in {} ms",
           pipelines_created, pipelines_already_exist,
           pipeline_stored_descriptions.size(),
           (xe::Clock::QueryHostTickCount() - pipeline_creation_start) * 1000 /
               xe::Clock::QueryHostTickFrequency());

    if (pipelines_vs_not_found || pipelines_vs_translation_missing ||
        pipelines_ps_not_found || pipelines_ps_translation_missing ||
        pipelines_render_pass_failed || pipelines_layout_failed ||
        pipelines_requirements_not_met) {
      XELOGI(
          "Pipeline cache skipped: {} VS not found, {} VS translation missing, "
          "{} PS not found, {} PS translation missing, {} render pass failed, "
          "{} layout failed, {} requirements not met",
          pipelines_vs_not_found, pipelines_vs_translation_missing,
          pipelines_ps_not_found, pipelines_ps_translation_missing,
          pipelines_render_pass_failed, pipelines_layout_failed,
          pipelines_requirements_not_met);
    }
  }

  // Invoke completion callback if no async work was queued.
  if (completion_callback) {
    completion_callback();
  }
}

void VulkanPipelineCache::SaveVkPipelineCache() {
  if (vk_pipeline_cache_ == VK_NULL_HANDLE || vk_pipeline_cache_path_.empty()) {
    return;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  size_t cache_size = 0;
  if (dfn.vkGetPipelineCacheData(device, vk_pipeline_cache_, &cache_size,
                                 nullptr) == VK_SUCCESS &&
      cache_size > 0) {
    std::vector<uint8_t> cache_data(cache_size);
    if (dfn.vkGetPipelineCacheData(device, vk_pipeline_cache_, &cache_size,
                                   cache_data.data()) == VK_SUCCESS) {
      if (FILE* cache_file =
              xe::filesystem::OpenFile(vk_pipeline_cache_path_, "wb")) {
        fwrite(cache_data.data(), 1, cache_size, cache_file);
        fclose(cache_file);
        XELOGI("Saved {} bytes of VkPipelineCache data", cache_size);
      } else {
        XELOGE("Failed to open VkPipelineCache file for writing: {}",
               xe::path_to_utf8(vk_pipeline_cache_path_));
      }
    }
  } else {
    XELOGW("VkPipelineCache data unavailable for saving (size {})",
           cache_size);
  }
}

void VulkanPipelineCache::ShutdownShaderStorage() {
  // Save VkPipelineCache to disk before shutting down storage.
  SaveVkPipelineCache();
  vk_pipeline_cache_path_.clear();

  // Shut down the storage writer (closes files, stops write thread).
  storage_writer_.ShutdownShaderStorage();
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;
  shader_storage_title_id_ = 0;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
