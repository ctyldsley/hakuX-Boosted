/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_PIPELINE_STATE_CACHE_H_
#define XENIA_GPU_VULKAN_VULKAN_PIPELINE_STATE_CACHE_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/hash.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/guest_spirv_shader_cache.h"
#include "xenia/gpu/primitive_processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

// TODO(Triang3l): Create a common base for both the Vulkan and the Direct3D
// implementations.
class VulkanPipelineCache : public GuestSpirvShaderCache::Host {
 public:
  class PipelineLayoutProvider {
   public:
    virtual ~PipelineLayoutProvider() {}
    virtual VkPipelineLayout GetPipelineLayout() const = 0;
    // hakuX-Boosted: the layout a pipeline library of this layout's shaders
    // is created with (boosted/notes/pipeline-libraries.md): for the
    // fragment shader's library or for the library of the stages before
    // rasterisation. With independent sets it names only the sets those
    // stages use; otherwise it is the whole layout.
    virtual VkPipelineLayout GetLibraryPipelineLayout(bool fragment) const {
      return GetPipelineLayout();
    }

   protected:
    PipelineLayoutProvider() = default;
  };

  // Pre-mapped dynamic-state values for one pipeline, derived from the same
  // (non-canonicalized) state the static pipeline would have baked. The command
  // processor reads these to emit the extended-dynamic-state setters per draw
  // with dirty tracking, guaranteeing the dynamic values match the static
  // encoding exactly. Only fields whose capability bit is set are dynamic; the
  // rest are baked and their values here are ignored.
  struct DynamicState {
    VkPrimitiveTopology primitive_topology;
    VkBool32 primitive_restart_enable;
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkBool32 depth_test_enable;
    VkBool32 depth_write_enable;
    VkCompareOp depth_compare_op;
    VkBool32 stencil_test_enable;
    VkStencilOpState stencil_front;
    VkStencilOpState stencil_back;
    VkBool32 depth_clamp_enable;
    VkPolygonMode polygon_mode;
    // Per color render target. color_rts_used is the bitmask of valid indices
    // (matching render_pass_key.depth_and_color_used >> 1).
    uint32_t color_rts_used;
    VkBool32 color_blend_enable[xenos::kMaxColorRenderTargets];
    VkColorBlendEquationEXT color_blend_equation[xenos::kMaxColorRenderTargets];
    VkColorComponentFlags color_write_mask[xenos::kMaxColorRenderTargets];
  };

  // hakuX-Boosted: the stand-in pixel shader of a placeholder pipeline
  // (vulkan_placeholder_ps_mode), which draws an approximation of the guest
  // pixel shader instead of nothing while the real pipeline compiles. See
  // shaders/placeholder_stand_in.glsli for what it draws.
  enum class StandInPixelShader : uint32_t {
    // The no-op placeholder pixel shader (no stand-in).
    kNone,
    // The vertex colour or a mid grey.
    kFlat,
    // A 1D or 2D texture, times the vertex colour.
    k2D,
    // A cube texture, times the vertex colour.
    kCube,
    // hakuX-Boosted: not an approximation: the guest pixel shader's own
    // microcode, run by the guest shader interpreter, version 2
    // (vulkan_placeholder_ps_mode = 3, shaders/ucode_program_pixel.glsli).
    // Only with that interpreter's vertex stage.
    kProgram,
  };
  struct StandIn {
    static constexpr uint32_t kNoInterpolator = 0xFF;
    StandInPixelShader shader = StandInPixelShader::kNone;
    // The interpolator the texture coordinate is read from when the placeholder
    // runs the translated guest vertex shader (the input location is baked in
    // the pixel shader module). kNoInterpolator with the interpreter vertex
    // shader, which is told for each draw which interpolator to output, and
    // when there is no texture.
    uint32_t texcoord_interpolator = kNoInterpolator;
    // The interpolator carrying the vertex colour, kNoInterpolator if none.
    uint32_t color_interpolator = kNoInterpolator;

    uint32_t Pack() const {
      return uint32_t(shader) | (texcoord_interpolator << 8) |
             (color_interpolator << 16);
    }
    static StandIn Unpack(uint32_t packed) {
      StandIn stand_in;
      stand_in.shader = StandInPixelShader(packed & 0xFF);
      stand_in.texcoord_interpolator = (packed >> 8) & 0xFF;
      stand_in.color_interpolator = (packed >> 16) & 0xFF;
      return stand_in;
    }
  };

  static const char* GetStandInPixelShaderName(StandInPixelShader shader);

  struct Pipeline {
    // VK_NULL_HANDLE while asynchronous creation is pending (or after it
    // failed). The slot address is STABLE for the cache's lifetime
    // (unordered_map nodes never relocate, and entries are never erased while
    // running), so the deferred command buffer records &pipeline and loads it
    // at replay.
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    // The layouts are owned by the VulkanCommandProcessor, and must not be
    // destroyed by it while the pipeline cache is active. Atomic because an
    // interpreter placeholder is created with a minimal (no-texture) layout on
    // the draw thread, then upgraded to the real layout by the creation thread
    // once the deferred shaders are translated (before the pipeline hot-swap).
    // It is also the field the fork's deferred-bind model reads at draw time
    // (starts at the minimal layout from untranslated shaders, upgraded to the
    // real one by the creation thread) - the atomic serves both async models.
    std::atomic<const PipelineLayoutProvider*> pipeline_layout;
    // True while pipeline_layout still holds the minimal layout computed from
    // shaders whose bindings weren't ready at insertion. Shaders can become
    // ready before this entry's own job runs, so cache hits re-check and
    // upgrade on the draw thread - otherwise a draw in that window allocates
    // texture descriptors from a zero-binding layout.
    std::atomic<bool> pipeline_layout_stale{false};
    // hakuX-Boosted: set by the creation thread once its job for this slot is
    // over, created or failed, so a wait on this one pipeline
    // (RequirePipelineForSubmission) can tell a failed slot, whose handle stays
    // null for good, from one still compiling.
    std::atomic<bool> creation_finished{false};
    // Pre-mapped dynamic-state values for this pipeline (see DynamicState).
    DynamicState dynamic_state;

    // Placeholder pipeline support for reduced stutter (upstream model, used
    // when the vulkan_placeholder_pipelines cvar is on; all stay inert - false /
    // VK_NULL_HANDLE - when it is off and the deferred-bind model is used).
    // When true, the current pipeline uses a placeholder pixel shader and
    // the real pipeline is being compiled in the background.
    std::atomic<bool> is_placeholder{false};
    // When true, the placeholder rasterizes with the ucode interpreter VS (so
    // the draw must feed it full float constants and the ucode location). Set
    // once when the interpreter placeholder is built, gated by is_placeholder.
    std::atomic<bool> uses_interpreter{false};
    // The placeholder VkPipeline handle (VK_NULL_HANDLE if none). A draw
    // compares the pipeline handle it bound against this to know whether it
    // bound the placeholder - consistent with the single `pipeline` load,
    // unlike the separate is_placeholder flag which is cleared a few
    // instructions after the real pipeline is swapped in.
    std::atomic<VkPipeline> placeholder_pipeline{VK_NULL_HANDLE};
    // hakuX-Boosted: the stand-in pixel shader of the placeholder, as
    // StandIn::Pack (0 if the placeholder has the no-op pixel shader), and the
    // layout the placeholder was created with, which has room for the
    // stand-in's one texture and one sampler and so is neither the minimal
    // layout nor the real one in pipeline_layout. Both are written once, before
    // the placeholder handle is published, and a draw that binds the
    // placeholder binds its descriptor sets with this layout.
    std::atomic<uint32_t> placeholder_stand_in{0};
    std::atomic<const PipelineLayoutProvider*> placeholder_pipeline_layout{
        nullptr};
    // hakuX-Boosted: for the shader_profiling log, when the placeholder was
    // made (written before the real pipeline's creation is queued) and how
    // many draws were recorded with it.
    uint64_t placeholder_created_ms = 0;
    std::atomic<uint32_t> placeholder_draws{0};

    // hakuX-Boosted: boosted/notes/shader-ahead.md. None of these is copied by
    // the constructors below: a slot is only copied when it is put in the map,
    // before anything is written to them.
    // Set by the creation thread that takes this slot's real pipeline job, so
    // a second job for the same slot (a speculative pipeline promoted when a
    // draw arrived, or one asked for again at a higher priority) can be
    // dropped.
    std::atomic<bool> creation_claimed{false};
    // A placeholder for this slot is being created on a creation thread
    // (vulkan_placeholder_async).
    std::atomic<bool> placeholder_pending{false};
    // hakuX-Boosted: the placeholder's vertex stage is the guest shader
    // interpreter, version 2 (vulkan_placeholder_interpreter = 2,
    // boosted/notes/shader-first-run.md): a draw that binds it uploads the
    // shaders' prepared microcode instead of telling the interpreter where
    // it is in guest memory. Written with uses_interpreter, before the
    // placeholder is published.
    std::atomic<bool> placeholder_program{false};
    // hakuX-Boosted: the placeholder handle is a shared stand-in pipeline's
    // (vulkan_standin_precompile, boosted/notes/standin-precompile.md): it
    // belongs to the cache, not to this slot, and is never destroyed when
    // the real pipeline replaces it or with the slot. Written before the
    // handle is published.
    std::atomic<bool> placeholder_shared{false};
    // The rest is the GPU command thread's alone.
    // hakuX-Boosted: the shared stand-in (a SharedStandIn) this slot is
    // waiting for, and the StandIn::Pack it will be drawn with; null once the
    // slot has taken its handle or given up.
    void* stand_in_pending = nullptr;
    uint32_t stand_in_pending_packed = 0;
    // A placeholder was asked for once, made or not: not asked for again.
    bool placeholder_attempted = false;
    // A wait for this slot's real pipeline ran out
    // (vulkan_placeholder_wait_fullscreen_ms): not waited for again.
    bool wait_given_up = false;
    // 0: asked for by a draw. 1: made on a prediction, no draw has used it
    // yet. 2: as 1, and written off as wasted.
    uint8_t speculative = 0;
    // Which prediction made it (SpeculationRule).
    uint8_t speculative_rule = 0;
    uint64_t speculative_made_ms = 0;

    Pipeline(const PipelineLayoutProvider* pipeline_layout_provider)
        : pipeline_layout(pipeline_layout_provider), dynamic_state{} {}

    // Copy constructor needed for unordered_map
    Pipeline(const Pipeline& other)
        : pipeline(other.pipeline.load(std::memory_order_acquire)),
          pipeline_layout(
              other.pipeline_layout.load(std::memory_order_acquire)),
          pipeline_layout_stale(
              other.pipeline_layout_stale.load(std::memory_order_acquire)),
          dynamic_state(other.dynamic_state),
          is_placeholder(other.is_placeholder.load(std::memory_order_acquire)),
          uses_interpreter(
              other.uses_interpreter.load(std::memory_order_acquire)),
          placeholder_pipeline(
              other.placeholder_pipeline.load(std::memory_order_acquire)),
          placeholder_stand_in(
              other.placeholder_stand_in.load(std::memory_order_acquire)),
          placeholder_pipeline_layout(other.placeholder_pipeline_layout.load(
              std::memory_order_acquire)),
          placeholder_created_ms(other.placeholder_created_ms),
          placeholder_draws(
              other.placeholder_draws.load(std::memory_order_acquire)) {}

    // Move constructor
    Pipeline(Pipeline&& other) noexcept
        : pipeline(other.pipeline.load(std::memory_order_acquire)),
          pipeline_layout(
              other.pipeline_layout.load(std::memory_order_acquire)),
          pipeline_layout_stale(
              other.pipeline_layout_stale.load(std::memory_order_acquire)),
          dynamic_state(other.dynamic_state),
          is_placeholder(other.is_placeholder.load(std::memory_order_acquire)),
          uses_interpreter(
              other.uses_interpreter.load(std::memory_order_acquire)),
          placeholder_pipeline(
              other.placeholder_pipeline.load(std::memory_order_acquire)),
          placeholder_stand_in(
              other.placeholder_stand_in.load(std::memory_order_acquire)),
          placeholder_pipeline_layout(other.placeholder_pipeline_layout.load(
              std::memory_order_acquire)),
          placeholder_created_ms(other.placeholder_created_ms),
          placeholder_draws(
              other.placeholder_draws.load(std::memory_order_acquire)) {}

    // Deleted copy assignment to prevent accidental copying
    Pipeline& operator=(const Pipeline&) = delete;

    // Deleted move assignment
    Pipeline& operator=(Pipeline&&) = delete;
  };

  // Resolved extended-dynamic-state capabilities for the current device and the
  // vulkan_dynamic_pipeline_state cvar. A field is dynamic only when its bit is
  // set here; otherwise it is baked into the pipeline key as before. EDS1/EDS2
  // fields share `extended_dynamic_state` (core in Vulkan 1.3); each EDS3
  // sub-feature has its own bit. Computed once in Initialize().
  struct DynamicStateCapabilities {
    // EDS1/EDS2 (core in 1.3): cull mode, front face, primitive topology,
    // primitive restart enable, depth test/write/compare op, stencil test
    // enable, stencil op.
    bool extended_dynamic_state = false;
    // EDS3 sub-features.
    bool depth_clamp_enable = false;
    bool polygon_mode = false;
    bool color_blend_enable = false;
    bool color_blend_equation = false;
    bool color_write_mask = false;
    // When false, dynamic topology must stay within its class, so the topology
    // class is kept in the pipeline key.
    bool primitive_topology_unrestricted = false;
  };
  const DynamicStateCapabilities& dynamic_state_capabilities() const {
    return dynamic_state_capabilities_;
  }

  static constexpr size_t kLayoutUIDEmpty = 0;

  VulkanPipelineCache(VulkanCommandProcessor& command_processor,
                      const RegisterFile& register_file,
                      VulkanRenderTargetCache& render_target_cache,
                      VkShaderStageFlags guest_shader_vertex_stages);
  ~VulkanPipelineCache();

  bool Initialize();
  void Shutdown();

  // Shader and pipeline storage.
  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr);
  void ShutdownShaderStorage();

  // Submission-boundary hook; the command processor calls this on the GPU
  // thread right before replaying the deferred command buffer. Persists the
  // VkPipelineCache blob (throttled) and storage files, then waits until all
  // queued pipeline creations complete so every deferred pipeline bind in the
  // stream resolves to a created handle - unless vulkan_async_skip_draws is
  // enabled or the startup storage preload is running, in which case it
  // doesn't block and unready pipelines have their draws dropped at replay.
  void EndSubmission();
  // hakuX-Boosted: with vulkan_async_skip_draws, the draws of a pipeline that
  // is still compiling are dropped at replay. For most draws that is an
  // object appearing a few frames late; for a full-screen pass (a
  // post-process step, the final composite) it is the whole picture wrong for
  // those frames. The command processor names such a pipeline here and
  // EndSubmission waits for it, and only for it, before the replay. GPU
  // thread only.
  void RequirePipelineForSubmission(Pipeline* pipeline);
  // Throttled persist of the VkPipelineCache blob. Cheap no-op unless new
  // pipelines were created and the save interval elapsed. Called from
  // EndSubmission().
  void MaybeSaveVkPipelineCache();
  bool IsCreatingPipelines();
  // Waits for any pipeline creation needed by the current draw path to finish
  // before state is consumed. This was added so strict ZPD query paths stop
  // racing pipeline compilation and then blocking work on incomplete state.
  void AwaitPipelineCompletion();

  VulkanShader* LoadShader(xenos::ShaderType shader_type,
                           const uint32_t* host_address, uint32_t dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) {
    shader.AnalyzeUcode(ucode_disasm_buffer_);
  }

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed.
  SpirvShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, bool ps_param_gen_used) const;
  SpirvShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      bool apply_polygon_offset_in_shader) const;

  // The ucode interpreter placeholder defers translation by not calling this on
  // the draw thread; the creation thread calls it (passing its own worker
  // translator, since the shared one is single-threaded). nullptr uses the
  // shared translator.
  bool EnsureShadersTranslated(VulkanShader::VulkanTranslation* vertex_shader,
                               VulkanShader::VulkanTranslation* pixel_shader,
                               SpirvShaderTranslator* translator = nullptr);
  // use_interpreter requests the ucode interpreter VS placeholder for a new,
  // untranslated vertex shader (falls back to normal translation if the async
  // placeholder path can't be taken for this draw).
  bool ConfigurePipeline(
      VulkanShader::VulkanTranslation* vertex_shader,
      VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key,
      bool use_interpreter, Pipeline** pipeline_out,
      bool require_real = false);

  // hakuX-Boosted: everything from here to IsInterpreterPlaceholder is
  // described in boosted/notes/shader-ahead.md. GPU command thread only.

  // Why a draw of the placeholder model was not drawn. The first group says
  // which condition kept the ucode interpreter from standing in for the
  // vertex shader (several can be set), the second what state the pipeline
  // slot was in.
  enum DrawSkipReason : uint32_t {
    kSkipInterpreterOff = 1u << 0,
    kSkipInlineShader = 1u << 1,
    kSkipDeviceIndices = 1u << 2,
    kSkipDeviceSharedMemory = 1u << 3,
    kSkipTessellated = 1u << 4,
    kSkipHostVertexShaderType = 1u << 5,
    kSkipPointList = 1u << 6,
    kSkipRectangleOrQuadList = 1u << 7,
    kSkipVertexTextureFetch = 1u << 8,
    kSkipSubroutineCalls = 1u << 9,
    kSkipMemoryExport = 1u << 10,
    kSkipLoops = 1u << 11,
    kSkipVertexShaderTranslated = 1u << 12,
    // hakuX-Boosted: the two the interpreter's version 2 adds.
    kSkipProgramTooLarge = 1u << 13,
    kSkipProgramMalformed = 1u << 14,
    kSkipInterpreterMask = (1u << 15) - 1,

    // The vertex shader was not translated yet, so there was nothing to make
    // a placeholder from ("no interpreter placeholder, real pipeline not
    // ready").
    kSkipVertexShaderNotTranslated = 1u << 16,
    // The slot has neither a placeholder nor the real pipeline ("pipeline
    // not ready yet"): see the next four for why it has no placeholder.
    kSkipNoPipeline = 1u << 17,
    kSkipPlaceholderNotAllowed = 1u << 18,
    kSkipPlaceholderFailed = 1u << 19,
    kSkipPlaceholderCompiling = 1u << 20,
    kSkipPlaceholderNeverTried = 1u << 21,
    kSkipRealWaitedFor = 1u << 22,
    // The slot has an interpreter placeholder, and this draw's vertex shader
    // was loaded inline, which the interpreter cannot read.
    kSkipInterpreterPlaceholderInlineShader = 1u << 23,
    // The stand-in pixel shader had nothing valid to bind for this draw.
    kSkipStandInNoTexture = 1u << 24,
    kSkipStandInVertexBindings = 1u << 25,
    kSkipStandInNoSampler = 1u << 26,
    // hakuX-Boosted: the frame's allowance of vertices drawn through the
    // interpreter is spent (vulkan_placeholder_interpreter_vertex_budget).
    kSkipInterpreterBudget = 1u << 27,
  };
  // Counts the skipped draw and says why in the log, once for each pair of
  // shaders and set of reasons. Adds what the slot itself says to `reasons`.
  void NoteDrawSkipped(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                       uint32_t reasons, const Pipeline* pipeline);
  // Called once for each guest frame (at the swap): the budget of
  // vulkan_placeholder_async_wait_ms is per frame.
  // hakuX-Boosted: how far the stored pipelines of this title are with being
  // made again at launch, for a progress display. False once all are ready
  // (or there were none); total_out is then how many there were.
  bool GetStartupProgress(uint32_t& done_out, uint32_t& total_out) const;
  void NoteFrameBoundary();
  // hakuX-Boosted: vulkan_pipeline_creation_adaptive: how many creation
  // threads take jobs and at what priority, by how the frames are going.
  void UpdateCreationPolicy(bool frame_boundary);
  // Whether full-screen passes wait for their real pipeline instead of being
  // drawn with a placeholder (vulkan_placeholder_wait_fullscreen_ms).
  bool IsFullscreenRealPipelineWaited() const;

  // shader_ahead: the command stream that is about to be executed draws with
  // this pair of shaders (found by CommandProcessor's look ahead). Starts
  // their translation, and with shader_ahead_speculate the pipelines the pair
  // is most likely to be drawn with, on the creation threads.
  void NoteShaderPairAhead(VulkanShader* vertex_shader,
                           VulkanShader* pixel_shader,
                           xenos::PrimitiveType guest_primitive_type);

  // hakuX-Boosted: the guest shader interpreter, version 2
  // (boosted/notes/shader-first-run.md). Whether new interpreter placeholders
  // are made with it (vulkan_placeholder_interpreter = 2, and its modules
  // exist).
  bool IsProgramInterpreterActive() const;
  // Why it cannot stand in for this vertex shader in a draw of this kind, as
  // DrawSkipReason bits of the interpreter group; 0 if it can. The shader
  // must have its microcode analysed. GPU command thread only.
  uint32_t GetProgramInterpreterRefusal(
      VulkanShader& vertex_shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      xenos::PrimitiveType host_primitive_type);

  // hakuX-Boosted: why the pixel side of the interpreter cannot draw this
  // pixel shader's pixels (the placeholder then gets the stand-in it would
  // have had with vulkan_placeholder_ps_mode = 2); 0 if it can.
  enum ProgramPixelRefusal : uint32_t {
    kPixelRefusalMalformed = 1u << 0,
    kPixelRefusalTooLarge = 1u << 1,
    kPixelRefusalFetchQuery = 1u << 2,
    kPixelRefusalMemoryExport = 1u << 3,
    kPixelRefusalTooManyTextures = 1u << 4,
    kPixelRefusalTextureKind = 1u << 5,
    kPixelRefusalDepthOutput = 1u << 6,
    kPixelRefusalPointSprite = 1u << 7,
    kPixelRefusalDepthMode = 1u << 8,
    kPixelRefusalBlendPremultiply = 1u << 9,
    kPixelRefusalResolutionScale = 1u << 10,
    kPixelRefusalSetting = 1u << 11,
    kPixelRefusalNoModule = 1u << 12,
    kPixelRefusalCount = 13,
  };
  uint32_t GetProgramPixelRefusal(VulkanShader& vertex_shader,
                                  VulkanShader& pixel_shader,
                                  uint64_t pixel_shader_modification);
  // The pixel stage's per-draw word (shaders/ucode_program_pixel.glsli).
  static uint32_t GetProgramPixelWord(uint64_t pixel_shader_modification);

  // hakuX-Boosted: boosted/notes/standin-precompile.md. Whether the
  // placeholders of draws the interpreter's version 2 takes are shared
  // stand-in pipelines made on the creation threads
  // (vulkan_standin_precompile, with the placeholder model and that
  // interpreter). Any thread.
  bool IsStandInPrecompileActive() const;

  // hakuX-Boosted: boosted/notes/pipeline-libraries.md. Whether real
  // pipelines are linked from libraries on this device
  // (vulkan_pipeline_libraries, with the extension, its feature and
  // independent interpolation decoration), and whether the libraries'
  // layouts name only their own stages' sets
  // (vulkan_pipeline_libraries_independent_sets), in which case every guest
  // pipeline layout is created with the independent sets flag. Any thread.
  static bool ArePipelineLibrariesUsable(
      const ui::vulkan::VulkanDevice* vulkan_device);
  static bool ArePipelineLibraryLayoutsIndependent(
      const ui::vulkan::VulkanDevice* vulkan_device);

  // True while this draw must be fed the ucode interpreter's inputs (full float
  // constants + ucode location). False once hot-swapped to the real VS.
  static bool IsInterpreterPlaceholder(const Pipeline* pipeline) {
    return pipeline->uses_interpreter.load(std::memory_order_acquire) &&
           pipeline->is_placeholder.load(std::memory_order_acquire);
  }

  // Whether ConfigurePipeline will create the pipeline asynchronously (so the
  // draw thread must not translate shaders itself). Matches the use_async
  // condition inside ConfigurePipeline. has_pixel_shader because the async
  // placeholder path needs a pixel shader.
  // hakuX-Boosted: use_interpreter says the ucode interpreter can stand in for
  // this draw's vertex shader, which is what a draw without a pixel shader
  // needs to be asynchronous (vulkan_placeholder_depth_only_interpreter).
  bool CanCreatePipelineAsync(bool has_pixel_shader,
                              bool use_interpreter = false) const;

 private:
  // PipelineGeometryShader and GeometryShaderKey come from
  // GuestSpirvShaderCache.
  using GeometryShaderKey = GuestSpirvShaderCache::GeometryShaderKey;

  enum class PipelinePrimitiveTopology : uint32_t {
    kPointList,
    kLineList,
    kLineStrip,
    kTriangleList,
    kTriangleStrip,
    kTriangleFan,
    kLineListWithAdjacency,
    kPatchList,
  };

  enum class PipelinePolygonMode : uint32_t {
    kFill,
    kLine,
    kPoint,
  };

  // Tessellation mode for pipeline creation.
  // Must match the TCS (hull shader) selection logic.
  enum class PipelineTessellationMode : uint32_t {
    kNone,
    kDiscrete,    // Integer tessellation factors.
    kContinuous,  // Fractional (fractional_even) tessellation factors.
    kAdaptive,    // Per-edge factors from index buffer.
  };

  // Tessellation primitive type.
  enum class PipelineTessellationPatchType : uint32_t {
    kNone,
    kTriangle,
    kQuad,
  };

  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kOneMinusSrcColor,
    kDstColor,
    kOneMinusDstColor,
    kSrcAlpha,
    kOneMinusSrcAlpha,
    kDstAlpha,
    kOneMinusDstAlpha,
    kConstantColor,
    kOneMinusConstantColor,
    kConstantAlpha,
    kOneMinusConstantAlpha,
    kSrcAlphaSaturate,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  XEPACKEDSTRUCT(PipelineRenderTarget, {
    PipelineBlendFactor src_color_blend_factor : 4;  // 4
    PipelineBlendFactor dst_color_blend_factor : 4;  // 8
    xenos::BlendOp color_blend_op : 3;               // 11
    PipelineBlendFactor src_alpha_blend_factor : 4;  // 15
    PipelineBlendFactor dst_alpha_blend_factor : 4;  // 19
    xenos::BlendOp alpha_blend_op : 3;               // 22
    uint32_t color_write_mask : 4;                   // 26
  });

  XEPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 if no pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;
    VulkanRenderTargetCache::RenderPassKey render_pass_key;

    // Shader stages.
    PipelineGeometryShader geometry_shader : 2;            // 2
    PipelineTessellationMode tessellation_mode : 2;        // 4
    PipelineTessellationPatchType tessellation_patch : 2;  // 6
    // Input assembly.
    PipelinePrimitiveTopology primitive_topology : 3;  // 9
    uint32_t primitive_restart : 1;                    // 10
    // Rasterization.
    uint32_t depth_clamp_enable : 1;       // 7
    PipelinePolygonMode polygon_mode : 2;  // 9
    uint32_t cull_front : 1;               // 10
    uint32_t cull_back : 1;                // 11
    uint32_t front_face_clockwise : 1;     // 12
    // Depth / stencil.
    uint32_t depth_write_enable : 1;                      // 13
    xenos::CompareFunction depth_compare_op : 3;          // 15
    uint32_t stencil_test_enable : 1;                     // 17
    xenos::StencilOp stencil_front_fail_op : 3;           // 20
    xenos::StencilOp stencil_front_pass_op : 3;           // 23
    xenos::StencilOp stencil_front_depth_fail_op : 3;     // 26
    xenos::CompareFunction stencil_front_compare_op : 3;  // 29
    xenos::StencilOp stencil_back_fail_op : 3;            // 32

    xenos::StencilOp stencil_back_pass_op : 3;           // 3
    xenos::StencilOp stencil_back_depth_fail_op : 3;     // 6
    xenos::CompareFunction stencil_back_compare_op : 3;  // 9

    // Topology class (0=point, 1=line, 2=triangle, 3=patch). Only kept in the
    // key when dynamic primitive topology is enabled but topology may not cross
    // classes (dynamicPrimitiveTopologyUnrestricted absent). Zero otherwise.
    uint32_t topology_class : 2;  // 11

    // Filled only for the attachments present in the render pass object.
    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    // Including all the padding, for a stable hash.
    PipelineDescription() { Reset(); }
    PipelineDescription(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
    }
    PipelineDescription& operator=(const PipelineDescription& description) {
      std::memcpy(this, &description, sizeof(*this));
      return *this;
    }
    bool operator==(const PipelineDescription& description) const {
      return std::memcmp(this, &description, sizeof(*this)) == 0;
    }
    void Reset() { std::memset(this, 0, sizeof(*this)); }
    uint64_t GetHash() const { return XXH3_64bits(this, sizeof(*this)); }
    struct Hasher {
      size_t operator()(const PipelineDescription& description) const {
        return size_t(description.GetHash());
      }
    };

    static constexpr uint32_t kVersion = 0x20260720;
  });

  // Pipeline storage constants.
  static constexpr uint32_t kPipelineStorageVersionWithoutAPI = 0x20201219;
  static constexpr uint32_t kPipelineStorageAPIMagicVulkan = 'VLKN';

  // Pipeline storage description.
  XEPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

  // creation threads, with everything needed from caches pre-looked-up.
  struct PipelineCreationArguments {
    std::pair<const PipelineDescription, Pipeline>* pipeline;
    VulkanShader::VulkanTranslation* vertex_shader;
    VulkanShader::VulkanTranslation* pixel_shader;
    VkShaderModule geometry_shader;
    // Tessellation shaders (only used when tessellation is active).
    VkShaderModule tessellation_vertex_shader;   // VS for passing data to TCS.
    VkShaderModule tessellation_control_shader;  // TCS (hull shader).
    VkRenderPass render_pass;
    // For dynamic rendering (VK_KHR_dynamic_rendering / Vulkan 1.3).
    VulkanRenderTargetCache::RenderPassKey render_pass_key;
    // Priority for async compilation (higher = compiled sooner).
    // Pipelines that write to visible render targets get higher priority.
    uint8_t priority = 0;

    // hakuX-Boosted: boosted/notes/shader-ahead.md.
    enum class Kind : uint8_t {
      // The pipeline of the slot, with the translated guest shaders.
      kReal,
      // A placeholder for the slot, created away from the GPU command thread
      // (vulkan_placeholder_async), with the three modules and layout below.
      kPlaceholder,
      // Only the translation of the shaders given (either may be null);
      // `pipeline` is null.
      kTranslate,
      // hakuX-Boosted: a shared stand-in pipeline (`stand_in`), which has no
      // guest shaders: `vertex_shader` and `pixel_shader` are null and
      // `pipeline` is the stand-in's own slot.
      kStandIn,
    };
    Kind kind = Kind::kReal;
    // Dropped if another job has already taken the slot's real pipeline.
    bool skip_if_claimed = false;
    // Made on a prediction: taken from the speculative queue.
    bool speculative = false;
    // hakuX-Boosted: one of the stored pipelines made again at launch.
    bool startup = false;
    VkShaderModule placeholder_fragment_shader = VK_NULL_HANDLE;
    VkShaderModule placeholder_vertex_shader = VK_NULL_HANDLE;
    const PipelineLayoutProvider* placeholder_layout = nullptr;
    // hakuX-Boosted: for Kind::kStandIn, a SharedStandIn.
    void* stand_in = nullptr;
    // hakuX-Boosted: only link the pipeline from libraries that are ready
    // (vulkan_pipeline_libraries_inline_link): EnsurePipelineCreated returns
    // false, having compiled nothing, if a shader's library is not.
    bool link_only = false;
  };

  // hakuX-Boosted: priorities of the jobs this fork adds, above every real
  // pipeline's (pipeline_util: 0 to 3). A placeholder is what a draw is
  // waiting for now; a translation is short and lets a placeholder be made.
  static constexpr uint8_t kPriorityPlaceholder = 255;
  static constexpr uint8_t kPriorityWaitedFor = 254;
  static constexpr uint8_t kPriorityTranslateForPlaceholder = 253;
  static constexpr uint8_t kPriorityTranslateAhead = 4;
  // hakuX-Boosted: a shared stand-in made before a draw asks for it: ahead of
  // every real pipeline (it is what will cover their compiles), behind what a
  // draw is waiting for now.
  static constexpr uint8_t kPriorityStandInAhead = 250;

  // hakuX-Boosted: which prediction made a speculative pipeline.
  enum SpeculationRule : uint8_t {
    // The pair seen ahead in the command stream, in the state of a pipeline
    // that already has its vertex shader.
    kRuleSameVertexShader,
    // ...that already has its pixel shader.
    kRuleSamePixelShader,
    // ...in the state of the pipeline a draw asked for last.
    kRuleNeighbour,
    // A pair that was just drawn in one state, in the other states its
    // shaders' other pairs have been drawn in.
    kRuleSibling,
    kRuleCount,
  };

  // Comparator for priority queue - higher priority first.
  struct PipelineCreationPriorityCompare {
    bool operator()(const PipelineCreationArguments& a,
                    const PipelineCreationArguments& b) const {
      return a.priority < b.priority;  // max-heap: lower priority at bottom
    }
  };

  // Can be called from multiple threads. use_try_claim atomically claims the
  // translation so concurrent callers (draw thread + creation threads)
  // translate it exactly once, the losers waiting for the winner.
  bool TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                               VulkanShader::VulkanTranslation& translation,
                               bool use_try_claim = false);

  // Translates shaders in parallel for storage loading.
  void TranslateShadersForStorage(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
      bool edram_fsi_used);

  // Guest graphics pipeline layout for the given (translated) shaders. Binding
  // counts come from translation, so untranslated shaders yield the minimal
  // no-texture layout used by the interpreter placeholder. Thread-safe.
  // counts_complete_out (optional) reports whether every consulted shader had
  // its bindings ready, i.e. whether the returned layout is the real one.
  const PipelineLayoutProvider* GetGuestGraphicsPipelineLayout(
      const VulkanShader::VulkanTranslation* vertex_shader,
      const VulkanShader::VulkanTranslation* pixel_shader,
      bool* counts_complete_out = nullptr);
  // Upgrades a cache-hit pipeline's layout on the draw thread once its
  // shaders' bindings are ready, closing the window between translation
  // finishing and the entry's own creation job running.
  void RefreshPipelineLayoutIfStale(
      Pipeline& pipeline, const VulkanShader::VulkanTranslation* vertex_shader,
      const VulkanShader::VulkanTranslation* pixel_shader);

  void WritePipelineRenderTargetDescription(
      reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
      PipelineRenderTarget& render_target_out) const;
  bool GetCurrentStateDescription(
      const VulkanShader::VulkanTranslation* vertex_shader,
      const VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key,
      PipelineDescription& description_out) const;

  // Whether the pipeline for the given description is supported by the device.
  bool ArePipelineRequirementsMet(const PipelineDescription& description) const;

  VkShaderModule GetGeometryShader(GeometryShaderKey key);

  // GuestSpirvShaderCache::Host.
  std::unique_ptr<SpirvShaderTranslator> CreateTranslator() const override;
  bool precise_interpolation_supported() const override;
  bool depth_float24_round() const override {
    return render_target_cache_.depth_float24_round();
  }
  bool depth_float24_convert_in_pixel_shader() const override {
    return render_target_cache_.depth_float24_convert_in_pixel_shader();
  }

  // Get the appropriate tessellation control shader (hull shader) module.
  VkShaderModule GetTessellationControlShader(
      PipelineTessellationMode mode, PipelineTessellationPatchType patch_type,
      bool use_control_point_count) const;

  // Get the appropriate tessellation vertex shader module.
  VkShaderModule GetTessellationVertexShader(
      PipelineTessellationMode mode) const;

  // hakuX-Boosted: whether a discrete or continuous tessellated pipeline takes
  // one control point per patch corner (the guest's triangle and quad lists,
  // kTriangleDomainCPIndexed and kQuadDomainCPIndexed) rather than one per
  // patch (kTriangleDomainPatchIndexed, kQuadDomainPatchIndexed). Read from the
  // host vertex shader type in the description's vertex shader modification,
  // as the Direct3D 12 backend does, so the description's layout (and the
  // pipeline storage) is as it was.
  static bool IsTessellationControlPointIndexed(
      const PipelineDescription& description);
  // hakuX-Boosted: VkPipelineTessellationStateCreateInfo::patchControlPoints
  // for the description, 0 if it's not tessellated.
  static uint32_t GetTessellationPatchControlPointCount(
      const PipelineDescription& description);

  // Can be called from creation threads - all needed data must be fully set up
  // at the point of the call: shaders must be translated, pipeline layout and
  // render pass objects must be available.
  // If fragment_shader_override is not VK_NULL_HANDLE, it is used instead of
  // the pixel shader from creation_arguments (for placeholder pipelines).
  // vertex_shader_override, if not VK_NULL_HANDLE, is used instead of the
  // translated vertex shader module (for the ucode interpreter placeholder,
  // whose guest VS is intentionally not translated yet).
  // hakuX-Boosted: layout_override, if not null, is the layout to create the
  // pipeline with instead of the slot's (a placeholder with a stand-in pixel
  // shader, whose layout is its own).
  bool EnsurePipelineCreated(
      const PipelineCreationArguments& creation_arguments,
      VkShaderModule fragment_shader_override = VK_NULL_HANDLE,
      VkShaderModule vertex_shader_override = VK_NULL_HANDLE,
      const PipelineLayoutProvider* layout_override = nullptr,
      bool publish_if_empty = false);

  // hakuX-Boosted: boosted/notes/shader-ahead.md.
  // The modules and render pass a pipeline of this description is created
  // with (what ConfigurePipeline looks up for a new slot), for a slot that
  // already exists or is about to be made on a prediction. `pipeline` and
  // `priority` are left for the caller. GPU command thread only.
  bool GetCreationArguments(const PipelineDescription& description,
                            VulkanShader::VulkanTranslation* vertex_shader,
                            VulkanShader::VulkanTranslation* pixel_shader,
                            PipelineCreationArguments& arguments_out);
  // Makes the placeholder of a slot: the block that used to be inside
  // ConfigurePipeline. `late` when the slot's real pipeline may already be
  // on its way (the placeholder must then not replace it).
  void MakePlaceholder(const PipelineCreationArguments& real_arguments,
                       bool use_interpreter, bool late);
  // What ConfigurePipeline does for a slot it found in the map: a speculative
  // slot is taken over by the draw, a slot without a placeholder may get one
  // late, a full-screen pass may wait for its real pipeline.
  void OnPipelineReused(
      std::pair<const PipelineDescription, Pipeline>& pipeline_pair,
      VulkanShader::VulkanTranslation* vertex_shader,
      VulkanShader::VulkanTranslation* pixel_shader, bool use_interpreter,
      bool require_real);
  // Waits, on the GPU command thread, for the slot's real pipeline.
  void WaitForRealPipeline(
      std::pair<const PipelineDescription, Pipeline>& pipeline_pair);
  void QueueStorageWrites(const PipelineDescription& description,
                          VulkanShader::VulkanTranslation* vertex_shader,
                          VulkanShader::VulkanTranslation* pixel_shader);
  void PushCreationJob(const PipelineCreationArguments& arguments);
  uint8_t GetRealPipelinePriority(
      const PipelineDescription& description,
      const VulkanShader::VulkanTranslation* pixel_shader) const;
  // The pixel shader module of an interpreter placeholder without a stand-in.
  VkShaderModule GetInterpreterPlaceholderPixelShader(
      const PipelineCreationArguments& creation_arguments, bool depth_only);

  // Prediction (shader_ahead, shader_ahead_speculate).
  bool IsAheadActive() const;
  void NoteRealPipeline(
      std::pair<const PipelineDescription, Pipeline>& pipeline_pair,
      bool was_speculative);
  bool BuildDescriptionFromTemplate(const PipelineDescription& state_template,
                                    VulkanShader& vertex_shader,
                                    VulkanShader& pixel_shader,
                                    PipelineDescription& description_out);
  bool SpeculatePipeline(const PipelineDescription& description,
                         SpeculationRule rule);
  void SpeculateSiblings(const PipelineDescription& description);
  void LogStatsIfDue();

  // hakuX-Boosted: decides what stands in for the pixel shader of a new
  // placeholder pipeline (the draw thread only). Returns false, with the
  // outputs untouched, when the placeholder gets the no-op pixel shader as
  // before: vulkan_placeholder_ps_mode is 0, the render target path is the
  // fragment shader interlock one, or the pixel shader writes no colour.
  // Otherwise fills what the stand-in is, its pixel shader module and the
  // pipeline layout to create the placeholder with.
  // hakuX-Boosted: use_program for the interpreter's version 2, which
  // writes every interpolator at the location of its index.
  bool GetStandIn(VulkanShader::VulkanTranslation* vertex_shader,
                  VulkanShader::VulkanTranslation* pixel_shader,
                  bool use_interpreter, bool use_program,
                  StandIn& stand_in_out,
                  VkShaderModule& pixel_shader_module_out,
                  const PipelineLayoutProvider*& pipeline_layout_out);
  // hakuX-Boosted: the interpreter's version 2 as the vertex stage of a draw
  // of this host vertex shader type (a vertex shader, or a tessellation
  // evaluation shader with equal or fractional spacing). VK_NULL_HANDLE if
  // there is none for it. Any thread.
  VkShaderModule GetProgramVertexShader(
      Shader::HostVertexShaderType host_vertex_shader_type,
      bool fractional_spacing) const;
  // The stand-in pixel shader module with its texture coordinate and colour
  // inputs at the given locations (0 and 1 are the modules as compiled, which
  // is what the interpreter vertex shader outputs). VK_NULL_HANDLE on failure.
  VkShaderModule GetStandInPixelShader(StandInPixelShader shader,
                                       bool has_color,
                                       uint32_t texcoord_location,
                                       uint32_t color_location);

  // Creates a placeholder pipeline using the placeholder pixel shader.
  // Used for pipeline hot-swap to reduce stutter.
  bool EnsurePipelineCreatedWithPlaceholder(
      const PipelineCreationArguments& creation_arguments) {
    return EnsurePipelineCreated(creation_arguments, placeholder_pixel_shader_);
  }

  // Creates a placeholder pipeline that rasterizes the guest geometry via the
  // ucode interpreter VS while the real VS+PS compile in the background.
  // hakuX-Boosted: depth_only for a draw that has no pixel shader.
  bool EnsurePipelineCreatedWithInterpreterPlaceholder(
      const PipelineCreationArguments& creation_arguments,
      bool depth_only = false);

  // hakuX-Boosted: shared stand-in pipelines, made ahead
  // (vulkan_standin_precompile, boosted/notes/standin-precompile.md).
  // GPU command thread only unless said otherwise.

  // What the pixel stage of a shared stand-in is.
  enum class StandInPixelForm : uint32_t {
    // The no-op pixel shader: depth only.
    kNoOp,
    // The flat grey of async_shader_vs_interpreter_debug_color.
    kDebugColor,
    // The float24 depth conversion of a draw without a pixel shader.
    kFloat24,
    // The pixel interpreter.
    kProgram,
    // The stand-in pixel shaders, for what the pixel interpreter refuses.
    kFlat,
    k2D,
    kCube,
    kCount,
  };
  // Everything a shared stand-in pipeline is made from, with nothing of a
  // guest shader or of the device's handles in it, so that it can be kept in
  // a file. `description` is a slot's canonical description with the shader
  // hashes removed and the modifications cut down to the host vertex shader
  // type and the two blend factors a MIN or MAX blend premultiplies by.
  XEPACKEDSTRUCT(StandInRecipe, {
    PipelineDescription description;
    StandInPixelForm pixel_form;
    // For kFlat, k2D and kCube: 0 for the module with every interpolator as
    // an input; otherwise (that module could not be made) the location of
    // the texture coordinate input, of the colour input << 8, and whether
    // there is a colour input << 16, which is never 0. 0 for the other
    // forms.
    uint32_t fallback_inputs;

    StandInRecipe() { std::memset(this, 0, sizeof(*this)); }
    StandInRecipe(const StandInRecipe& recipe) {
      std::memcpy(this, &recipe, sizeof(*this));
    }
    StandInRecipe& operator=(const StandInRecipe& recipe) {
      std::memcpy(this, &recipe, sizeof(*this));
      return *this;
    }
    bool operator==(const StandInRecipe& recipe) const {
      return std::memcmp(this, &recipe, sizeof(*this)) == 0;
    }
    struct Hasher {
      size_t operator()(const StandInRecipe& recipe) const {
        return size_t(XXH3_64bits(&recipe, sizeof(recipe)));
      }
    };
  });
  // Why a shared stand-in was queued.
  enum StandInOrigin : uint32_t {
    // The warm-up set at a title's start.
    kStandInOriginStartup,
    // Kept in the title's record by an earlier run.
    kStandInOriginRecord,
    // A render pass key seen for the first time.
    kStandInOriginRenderPass,
    // A draw needed it and it was not there.
    kStandInOriginDraw,
    kStandInOriginCount,
  };
  struct SharedStandIn {
    enum : uint32_t { kQueued, kReady, kFailed };
    SharedStandIn(const StandInRecipe& stand_in_recipe,
                  const PipelineLayoutProvider* pipeline_layout)
        : slot(stand_in_recipe.description, Pipeline(pipeline_layout)),
          recipe(stand_in_recipe),
          layout(pipeline_layout) {}
    // What EnsurePipelineCreated creates into: a slot of its own, with the
    // recipe's description.
    std::pair<const PipelineDescription, Pipeline> slot;
    StandInRecipe recipe;
    const PipelineLayoutProvider* layout;
    // The job that makes it, kept to be queued again at a draw's priority.
    PipelineCreationArguments arguments;
    StandInOrigin origin = kStandInOriginDraw;
    // A draw has asked for it (it is then kept in the record).
    bool used = false;
    // Queued a second time, at a draw's priority.
    bool boosted = false;
    // Set by the creation thread that takes it, so that a second job for it
    // is dropped.
    std::atomic<bool> claimed{false};
    // Written by that creation thread, read by any.
    std::atomic<uint32_t> state{kQueued};
  };
  // Cuts a slot's canonical description down to a recipe's.
  void BuildStandInRecipe(const PipelineDescription& slot_description,
                          StandInPixelForm pixel_form, uint32_t fallback_inputs,
                          StandInRecipe& recipe_out) const;
  // The shared stand-in of a recipe, queued for the creation threads if it
  // is new. Null if it cannot be made on this device. from_draw: a draw is
  // waiting for it, so it goes (or goes again) ahead of everything queued.
  SharedStandIn* GetSharedStandIn(const StandInRecipe& recipe,
                                  StandInOrigin origin, bool from_draw);
  // Gives the slot the handle of the shared stand-in it is waiting for, if
  // that is ready. True if the slot now draws with it.
  bool TryAdoptSharedStandIn(
      std::pair<const PipelineDescription, Pipeline>& pipeline_pair);
  // Queues the stand-ins a render pass key is most likely to need, the first
  // time the key is seen.
  void NoteStandInRenderPass(VulkanRenderTargetCache::RenderPassKey key,
                             StandInOrigin origin);
  // At a title's start, once the driver's cache is the title's: the warm-up
  // set and the recipes of the title's record.
  void StartStandInPrecompile(const std::filesystem::path& cache_root);
  void SaveStandInRecordIfDue(bool force);

  // The stand-in pixel shaders with every interpolator as an input (flat,
  // 2D, cube), which a shared stand-in uses where the pixel interpreter
  // refuses a shader.
  VkShaderModule stand_in_all_pixel_shaders_[3] = {};
  std::unordered_map<StandInRecipe, std::unique_ptr<SharedStandIn>,
                     StandInRecipe::Hasher>
      shared_stand_ins_;
  std::unordered_set<uint32_t> stand_in_render_passes_;
  std::filesystem::path stand_in_record_path_;
  bool stand_in_record_dirty_ = false;
  uint64_t stand_in_record_saved_ms_ = 0;
  // For the log. Queued, by origin; then what the creation threads made.
  uint64_t stand_ins_queued_[kStandInOriginCount] = {};
  std::atomic<uint64_t> stand_ins_created_[kStandInOriginCount] = {};
  std::atomic<uint64_t> stand_ins_created_us_[kStandInOriginCount] = {};
  std::atomic<uint64_t> stand_ins_slowest_us_{0};
  std::atomic<uint64_t> stand_ins_failed_{0};
  uint64_t stand_ins_unavailable_ = 0;
  // Slots whose stand-in was ready when first drawn, that waited for it (and
  // how many of those had it within the wait), that took it at a later draw,
  // and that never did (the real pipeline came first).
  uint64_t stand_in_slots_ready_ = 0;
  uint64_t stand_in_slots_waited_ = 0;
  uint64_t stand_in_slots_waited_in_time_ = 0;
  double stand_in_slots_wait_ms_ = 0.0;
  double stand_in_slots_wait_worst_ms_ = 0.0;
  uint64_t stand_in_slots_later_ = 0;
  uint64_t stand_in_slots_never_ = 0;
  uint64_t stand_in_record_loaded_ = 0;

  // hakuX-Boosted: pipelines linked from libraries
  // (vulkan_pipeline_libraries, boosted/notes/pipeline-libraries.md).
  enum PipelineLibraryPart : uint32_t {
    kLibraryVertexInput,
    kLibraryPreRasterization,
    kLibraryFragmentShader,
    kLibraryFragmentOutput,
    kLibraryPartCount,
  };
  // Exactly what the driver is given for one library: the modules, the
  // layout and the render pass by their handles, and every static value in
  // the order CreatePipelineFromLibraries writes them.
  struct PipelineLibraryKey {
    static constexpr uint32_t kMaxWords = 64;
    uint64_t modules[4];
    uint64_t layout;
    uint64_t render_pass;
    uint32_t part;
    uint32_t word_count;
    uint32_t words[kMaxWords];

    PipelineLibraryKey() { std::memset(this, 0, sizeof(*this)); }
    PipelineLibraryKey(const PipelineLibraryKey& key) {
      std::memcpy(this, &key, sizeof(*this));
    }
    PipelineLibraryKey& operator=(const PipelineLibraryKey& key) {
      std::memcpy(this, &key, sizeof(*this));
      return *this;
    }
    bool operator==(const PipelineLibraryKey& key) const {
      return std::memcmp(this, &key, sizeof(*this)) == 0;
    }
    // False if the key is full: the caller then makes no library.
    bool Add(uint32_t word) {
      if (word_count >= kMaxWords) {
        return false;
      }
      words[word_count++] = word;
      return true;
    }
    struct Hasher {
      size_t operator()(const PipelineLibraryKey& key) const {
        return size_t(XXH3_64bits(&key, sizeof(key)));
      }
    };
  };
  struct PipelineLibrary {
    enum : uint32_t { kCreating, kReady, kFailed };
    // Both under pipeline_libraries_lock_.
    uint32_t state = kCreating;
    VkPipeline pipeline = VK_NULL_HANDLE;
  };
  // The library of the key, made here (with create_info) if no thread has
  // made it or is making it; a thread that finds it being made waits for it.
  // VK_NULL_HANDLE if it could not be made. With find_only a shader library
  // (not an interface one, which compiles nothing) is never made or waited
  // for here: not_ready_out is then set and VK_NULL_HANDLE returned. Any
  // thread.
  VkPipeline GetOrCreatePipelineLibrary(
      const PipelineLibraryKey& key,
      const VkGraphicsPipelineCreateInfo& create_info, bool find_only,
      bool& not_ready_out);
  // Links a real pipeline from its four libraries, given the create info
  // EnsurePipelineCreated has filled for the whole pipeline. False, with
  // nothing to undo, if a library or the link failed (the caller then
  // compiles the pipeline whole) or, with link_only, if a shader's library
  // is not ready. Any thread.
  bool CreatePipelineFromLibraries(
      const VkGraphicsPipelineCreateInfo& pipeline_create_info,
      const VkPipelineRenderingCreateInfo* rendering_create_info,
      const PipelineLayoutProvider* layout_provider, bool link_only,
      VkPipeline& pipeline_out);
  // A new slot whose shaders are translated is linked on the GPU command
  // thread if its libraries are ready and the frame's budget is not spent
  // (vulkan_pipeline_libraries_inline_link). GPU command thread only.
  bool TryLinkPipelineInline(PipelineCreationArguments creation_arguments,
                             bool layout_counts_complete);
  // Set once in Initialize.
  bool pipeline_libraries_active_ = false;
  bool pipeline_libraries_independent_sets_ = false;
  bool pipeline_libraries_fast_linking_ = false;
  std::mutex pipeline_libraries_lock_;
  std::condition_variable pipeline_libraries_cond_;
  std::unordered_map<PipelineLibraryKey, PipelineLibrary,
                     PipelineLibraryKey::Hasher>
      pipeline_libraries_;
  // What is left of vulkan_pipeline_libraries_inline_link_frame_us in this
  // guest frame. The GPU command thread's alone.
  int64_t inline_link_left_us_ = 0;
  // Counters for the 'placeholder cover: pipeline libraries:' line.
  std::atomic<uint64_t> libraries_created_[kLibraryPartCount] = {};
  std::atomic<uint64_t> libraries_created_us_[kLibraryPartCount] = {};
  std::atomic<uint64_t> libraries_slowest_us_[kLibraryPartCount] = {};
  std::atomic<uint64_t> libraries_found_[kLibraryPartCount] = {};
  std::atomic<uint64_t> libraries_waited_[kLibraryPartCount] = {};
  std::atomic<uint64_t> libraries_failed_[kLibraryPartCount] = {};
  std::atomic<uint64_t> library_links_{0};
  std::atomic<uint64_t> library_links_us_{0};
  std::atomic<uint64_t> library_links_slowest_us_{0};
  std::atomic<uint64_t> library_links_compiled_nothing_{0};
  std::atomic<uint64_t> library_fallbacks_library_{0};
  std::atomic<uint64_t> library_fallbacks_link_{0};
  std::atomic<uint64_t> library_fallbacks_key_{0};
  // The GPU command thread's alone.
  uint64_t inline_links_ = 0;
  uint64_t inline_links_us_ = 0;
  uint64_t inline_links_slowest_us_ = 0;
  uint64_t inline_links_not_ready_ = 0;
  uint64_t inline_links_over_budget_ = 0;
  uint64_t inline_links_untranslated_ = 0;
  uint32_t stand_in_render_pass_last_ = UINT32_MAX;

  VulkanCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  VulkanRenderTargetCache& render_target_cache_;
  VkShaderStageFlags guest_shader_vertex_stages_;

  // Cached device features for geometry shader creation.
  unsigned int spirv_version_;
  bool signed_zero_inf_nan_preserve_float32_;
  bool denorm_flush_to_zero_float32_;
  // Already combined with the spirv_disable_rounding_mode_rte cvar.
  bool rounding_mode_rte_float32_;

  // Resolved once in Initialize() from the device features and the
  // vulkan_dynamic_pipeline_state cvar.
  DynamicStateCapabilities dynamic_state_capabilities_;

  // Zeroes the fields of `description` that are made dynamic by
  // dynamic_state_capabilities_ so that draws differing only in dynamic state
  // collapse onto one pipeline. Keeps the topology class when topology is not
  // unrestricted. Must be called right before hashing / cache lookup.
  void CanonicalizePipelineDescription(PipelineDescription& description) const;

  // Maps a (non-canonicalized) description to the pre-resolved Vulkan dynamic
  // state values the command processor emits per draw. Always computes all
  // fields; the command processor only emits the ones whose capability bit is
  // set.
  void FillDynamicState(const PipelineDescription& description,
                        DynamicState& dynamic_state) const;

  // Temporary storage for AnalyzeUcode calls on the processor thread.
  StringBuffer ucode_disasm_buffer_;
  // Shared guest SPIR-V translator, modification derivation and geometry keys.
  GuestSpirvShaderCache guest_shader_cache_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  // Texture binding layouts of different shaders, for obtaining layout UIDs.
  std::vector<VulkanShader::TextureBinding> texture_binding_layouts_;
  // Map of texture binding layouts used by shaders, for obtaining UIDs. Keys
  // are XXH3 hashes of layouts, values need manual collision resolution using
  // layout_vector_offset:layout_length of texture_binding_layouts_.
  std::unordered_multimap<uint64_t, LayoutUID,
                          xe::hash::IdentityHasher<uint64_t>>
      texture_binding_layout_map_;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, VulkanShader*,
                     xe::hash::IdentityHasher<uint64_t>>
      shaders_;

  // Geometry shaders for Xenos primitive types not supported by Vulkan.
  // Stores VK_NULL_HANDLE if failed to create.
  std::unordered_map<GeometryShaderKey, VkShaderModule,
                     GeometryShaderKey::Hasher>
      geometry_shaders_;

  // Empty depth-only pixel shader for writing to depth buffer using fragment
  // shader interlock when no Xenos pixel shader provided.
  VkShaderModule depth_only_fragment_shader_ = VK_NULL_HANDLE;

  // Substitute depth-only pixel shaders that perform float24 conversion of the
  // rasterizer's depth, bound for guest depth-only draws when in-PS float24
  // conversion is active and the depth buffer is D24FS8. Mirrors the DXBC
  // backend's float24_{truncate,round}_ps.
  VkShaderModule float24_truncate_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule float24_round_fragment_shader_ = VK_NULL_HANDLE;

  // Placeholder pixel shader for pipeline hot-swap to reduce stutter.
  // Outputs transparent black while the real shader compiles in background.
  VkShaderModule placeholder_pixel_shader_ = VK_NULL_HANDLE;

  // Ucode interpreter vertex shader - interprets a guest VS's ucode to render
  // its real geometry while the real VS translates+compiles in the background.
  VkShaderModule ucode_interpreter_vs_ = VK_NULL_HANDLE;
  // Flat grey debug pixel shader for interpreter placeholders, so the interim
  // geometry is visible (host-render-target path only).
  VkShaderModule placeholder_color_pixel_shader_ = VK_NULL_HANDLE;

  // hakuX-Boosted: the interpreter vertex shader that also outputs the two
  // interpolators the stand-in pixel shaders read, and those pixel shaders:
  // [StandInPixelShader - 1][has a colour input], as compiled (inputs at
  // locations 0 and 1), then copies with the inputs at other locations, by
  // GetStandInPixelShader's key (the draw thread only).
  VkShaderModule ucode_interpreter_stand_in_vs_ = VK_NULL_HANDLE;
  VkShaderModule stand_in_pixel_shaders_[3][2] = {};
  // hakuX-Boosted: the guest shader interpreter, version 2: a vertex shader,
  // the vertex shader of point sprites drawn as strips, and the tessellation
  // evaluation shaders [triangle control points, triangle patch, quad
  // control points, quad patch][equal spacing, fractional even spacing].
  VkShaderModule ucode_program_vs_ = VK_NULL_HANDLE;
  VkShaderModule ucode_program_point_strip_vs_ = VK_NULL_HANDLE;
  VkShaderModule ucode_program_ps_ = VK_NULL_HANDLE;
  // For the log: placeholders whose pixels the interpreter draws, those of
  // vulkan_placeholder_ps_mode = 3 it refused, and why (ProgramPixelRefusal).
  uint64_t placeholders_program_pixel_ = 0;
  uint64_t placeholders_program_pixel_refused_ = 0;
  uint64_t program_pixel_refusals_[kPixelRefusalCount] = {};
  VkShaderModule ucode_program_ds_[4][2] = {};
  // Interpreter placeholders made with it, for the once-a-second line.
  uint64_t placeholders_program_ = 0;
  std::unordered_map<uint32_t, VkShaderModule> stand_in_pixel_shaders_moved_;

  // Tessellation shaders.
  // Vertex shaders for tessellation - pass indices/factors to TCS.
  VkShaderModule tessellation_indexed_vs_ = VK_NULL_HANDLE;
  VkShaderModule tessellation_adaptive_vs_ = VK_NULL_HANDLE;
  // Tessellation control shaders (hull shaders) for different modes and
  // primitive types.
  // Discrete mode (integer tessellation factors).
  VkShaderModule discrete_triangle_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule discrete_triangle_3cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule discrete_quad_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule discrete_quad_4cp_hs_ = VK_NULL_HANDLE;
  // Continuous mode (fractional_even tessellation factors).
  VkShaderModule continuous_triangle_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule continuous_triangle_3cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule continuous_quad_1cp_hs_ = VK_NULL_HANDLE;
  VkShaderModule continuous_quad_4cp_hs_ = VK_NULL_HANDLE;
  // Adaptive mode (per-edge factors from index buffer).
  VkShaderModule adaptive_triangle_hs_ = VK_NULL_HANDLE;
  VkShaderModule adaptive_quad_hs_ = VK_NULL_HANDLE;

  // Vulkan pipeline cache for faster pipeline creation.
  VkPipelineCache vk_pipeline_cache_ = VK_NULL_HANDLE;

  std::unordered_map<PipelineDescription, Pipeline, PipelineDescription::Hasher>
      pipelines_;

  // Previously used pipeline, to avoid lookups if the state wasn't changed.
  std::pair<const PipelineDescription, Pipeline>* last_pipeline_ = nullptr;

  void CreationThread();

  // For asynchronous creation.
  std::vector<std::unique_ptr<xe::threading::Thread>> creation_threads_;
  std::atomic<bool> creation_threads_shutdown_{false};
  std::atomic<size_t> creation_threads_busy_{0};
  // hakuX-Boosted: vulkan_pipeline_creation_adaptive. The limit is above any
  // count unless the setting is on.
  // hakuX-Boosted: real pipeline compile time by how many of the pipeline's
  // guest shaders an earlier real pipeline had (0, 1, 2). Creation threads
  // and the GPU command thread.
  std::mutex compiled_shaders_lock_;
  std::unordered_set<const void*> compiled_shaders_;
  uint64_t compiled_by_seen_count_[3] = {};
  double compiled_by_seen_ms_[3] = {};
  // hakuX-Boosted: the kept record.
  enum class DriverCacheState { kNone, kMatch, kMismatch, kUnknown };
  DriverCacheState driver_cache_state_ = DriverCacheState::kNone;
  std::filesystem::path shader_record_files_[2];
  std::atomic<uint32_t> startup_pipelines_total_{0};
  std::atomic<uint32_t> startup_pipelines_left_{0};
  uint64_t startup_started_ms_ = 0;
  uint64_t startup_progress_logged_ms_ = 0;
  void LogStartupProgressIfDue();
  void ImportShaderRecord();
  void ExportShaderRecord();
  std::atomic<size_t> creation_thread_limit_{SIZE_MAX};
  size_t creation_threads_calm_ = 0;
  bool creation_boosted_ = false;
  uint64_t policy_frame_ms_last_ = 0;
  uint64_t policy_frame_ms_ = 0;
  uint64_t creation_policy_switches_ = 0;
  uint64_t creation_policy_logged_ms_ = 0;
  // Priority queue contains pointers to map entries. Pipelines are never
  // evicted as games have a finite set that should all remain cached for
  // performance. Higher priority pipelines (those writing to visible RTs)
  // are compiled first.
  std::priority_queue<PipelineCreationArguments,
                      std::vector<PipelineCreationArguments>,
                      PipelineCreationPriorityCompare>
      creation_queue_;
  std::mutex creation_request_lock_;
  std::condition_variable creation_request_cond_;
  // hakuX-Boosted: notified, under creation_request_lock_, each time a
  // creation thread finishes a slot; EndSubmission's wait for the required
  // pipelines sleeps on it. required_pipelines_ is the GPU thread's alone.
  std::condition_variable creation_progress_cond_;
  std::vector<Pipeline*> required_pipelines_;
  std::unique_ptr<xe::threading::Event> creation_completion_event_ = nullptr;
  std::atomic<bool> creation_completion_set_event_{false};
  std::function<void()> creation_completion_callback_;
  // During startup loading, don't block on pipeline creation to allow game
  // boot.
  bool startup_loading_ = false;

  // hakuX-Boosted: boosted/notes/shader-ahead.md. Everything below is the
  // GPU command thread's alone unless it is atomic or said to be under
  // creation_request_lock_.
  // Speculative pipelines wait here, behind every job in creation_queue_, and
  // at most shader_ahead_threads creation threads work on them at a time.
  // Both under creation_request_lock_.
  std::deque<PipelineCreationArguments> speculative_queue_;
  size_t speculative_busy_ = 0;
  // Speculative jobs queued or being compiled.
  std::atomic<uint32_t> speculative_outstanding_{0};
  std::atomic<uint64_t> speculative_compiled_{0};
  std::atomic<uint64_t> speculative_compile_us_{0};
  std::atomic<uint64_t> speculative_dropped_{0};
  // Slots made on a prediction that no draw has used yet (speculative == 1).
  std::vector<std::pair<const PipelineDescription, Pipeline>*>
      speculative_unused_;
  uint64_t speculative_made_[kRuleCount] = {};
  uint64_t speculative_hits_[kRuleCount] = {};
  uint64_t speculative_hits_ready_ = 0;
  uint64_t speculative_hits_compiling_ = 0;
  uint64_t speculative_hits_queued_ = 0;
  uint64_t speculative_hits_written_off_ = 0;
  uint64_t speculative_written_off_ = 0;
  uint64_t speculative_refused_budget_ = 0;
  bool speculative_stopped_logged_ = false;
  // Pipelines a draw asked for that no prediction had made.
  uint64_t ahead_misses_ = 0;
  // Pairs told by the look ahead, by PairKey, with when (host uptime, ms).
  std::unordered_map<uint64_t, uint64_t> ahead_pairs_;
  uint64_t ahead_pairs_new_ = 0;
  uint64_t ahead_shaders_new_ = 0;
  uint64_t ahead_translations_queued_ = 0;
  // At the first pipeline a draw asked for with a pair seen ahead: how long
  // ahead it was seen.
  uint64_t ahead_lead_count_ = 0;
  uint64_t ahead_lead_total_ms_ = 0;
  uint64_t ahead_lead_max_ms_ = 0;
  // The pipelines draws asked for, by vertex and by pixel shader, newest
  // last, and the pairs among them.
  std::unordered_map<uint64_t, std::vector<const PipelineDescription*>>
      descriptions_by_vertex_shader_;
  std::unordered_map<uint64_t, std::vector<const PipelineDescription*>>
      descriptions_by_pixel_shader_;
  std::unordered_set<uint64_t> real_pairs_;
  const PipelineDescription* last_real_description_ = nullptr;
  static uint64_t PairKey(uint64_t vertex_shader_hash,
                          uint64_t pixel_shader_hash) {
    return vertex_shader_hash ^ ((pixel_shader_hash << 31) |
                                 (pixel_shader_hash >> 33)) ^
           (pixel_shader_hash * UINT64_C(0x9E3779B97F4A7C15));
  }

  // Placeholder coverage.
  double placeholder_wait_left_ms_ = 0.0;
  std::unordered_set<uint64_t> skip_reasons_logged_;
  uint64_t skipped_draws_ = 0;
  uint64_t skipped_untranslated_vs_ = 0;
  uint64_t skipped_no_placeholder_ = 0;
  uint64_t skipped_placeholder_compiling_ = 0;
  uint64_t skipped_other_ = 0;
  uint64_t placeholders_inline_ = 0;
  uint64_t placeholders_async_ = 0;
  uint64_t placeholders_late_ = 0;
  uint64_t placeholder_waits_ = 0;
  double placeholder_wait_ms_ = 0.0;
  double placeholder_inline_worst_ms_ = 0.0;
  uint64_t fullscreen_waits_ = 0;
  double fullscreen_wait_ms_ = 0.0;
  // Translations EnsureShadersTranslated had to do on the GPU command thread.
  uint64_t draw_thread_translations_ = 0;
  double draw_thread_translation_ms_ = 0.0;
  // hakuX-Boosted: real pipelines ConfigurePipeline created on the GPU
  // command thread itself (a draw that could not be asynchronous).
  uint64_t draw_thread_pipelines_ = 0;
  double draw_thread_pipeline_ms_ = 0.0;
  double draw_thread_pipeline_worst_ms_ = 0.0;
  uint64_t skipped_interpreter_budget_ = 0;
  uint64_t stats_logged_ms_ = 0;
  uint64_t stats_logged_signature_ = 0;

  // Deferred destruction of pipelines.
  // Pipelines are only destroyed after the GPU submission that might reference
  // them has completed (tracked via submission numbers from command processor).
  void ProcessDeferredDestructions();
  std::vector<std::pair<VkPipeline, uint64_t>> deferred_destroy_pipelines_;
  std::mutex deferred_destroy_mutex_;

  // Shader and pipeline storage.
  uint32_t shader_storage_title_id_ = 0;
  std::atomic<bool> shader_storage_file_flush_needed_{false};
  std::atomic<bool> pipeline_storage_file_flush_needed_{false};

  // Storage writer for shaders and pipelines (owns file handles and storage
  // index).
  ShaderStorageWriter<PipelineStoredDescription> storage_writer_;

  // VkPipelineCache persistence path.
  std::filesystem::path vk_pipeline_cache_path_;
  // Saves vk_pipeline_cache_ to vk_pipeline_cache_path_ (no-op if either is
  // unset). Safe to call from the GPU thread concurrently with pipeline
  // creation (the pipeline cache is internally synchronized by the driver).
  void SaveVkPipelineCache();
  // Periodically persist vk_pipeline_cache_ on the GPU thread so the cache
  // survives Android process kills (upstream only saves at clean shutdown,
  // which Android frequently skips when terminating the process).
  std::atomic<bool> vk_pipeline_cache_dirty_{false};
  uint64_t vk_pipeline_cache_last_save_ms_ = 0;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_PIPELINE_STATE_CACHE_H_
