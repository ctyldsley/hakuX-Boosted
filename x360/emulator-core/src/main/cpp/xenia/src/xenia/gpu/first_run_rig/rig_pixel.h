// hakuX-Boosted: the fork's own file, not upstream's.
//
// The rig's pixel side (README.md): a guest pixel shader's microcode is
// translated by the real translator and prepared for the interpreter; both
// are drawn over a 16 by 16 target with the same interpolators, constants and
// textures, and the four colour outputs of every pixel are compared. Included
// by rig.cc, inside namespace rig.

constexpr uint32_t kTargetSize = 16;
// Written before each draw: what a pixel that was killed is left with.
constexpr float kTargetClear = -7.0f;
constexpr uint32_t kTextureSize = 8;
constexpr uint32_t kTextureMips = 4;

class PixelRunner {
 public:
  PixelRunner(Device& device, const Options& options)
      : device_(device), options_(options) {
    VkDevice vk = device_.device;
    const VkShaderStageFlags kStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 128},
    };
    VkDescriptorPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 16;
    pool_info.poolSizeCount = 4;
    pool_info.pPoolSizes = pool_sizes;
    RIG_VK(vkCreateDescriptorPool(vk, &pool_info, nullptr, &descriptor_pool_));

    shared_memory_ = device_.MakeBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    system_constants_ =
        device_.MakeBuffer(sizeof(SpirvShaderTranslator::SystemConstants),
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float_vertex_ =
        device_.MakeBuffer(256 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float_full_ =
        device_.MakeBuffer(256 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float_packed_ =
        device_.MakeBuffer(256 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    bool_loop_program_ = device_.MakeBuffer(ucode_program::kBlockBytes,
                                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    fetch_constants_ =
        device_.MakeBuffer(32 * 6 * 4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    readback_ = device_.MakeBuffer(4 * kTargetSize * kTargetSize * 16,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memory_set_ = AllocateSet(set_layouts_[0]);
    WriteBuffer(memory_set_, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                shared_memory_);
    for (uint32_t i = 0; i < 2; ++i) {
      // 0: for the translated shader, 1: for the interpreter.
      constant_sets_[i] = AllocateSet(set_layouts_[1]);
      WriteBuffer(constant_sets_[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  system_constants_);
      WriteBuffer(constant_sets_[i], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  float_vertex_);
      WriteBuffer(constant_sets_[i], 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  i ? float_full_ : float_packed_);
      WriteBuffer(constant_sets_[i], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  bool_loop_program_);
      WriteBuffer(constant_sets_[i], 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                  fetch_constants_);
    }
    empty_set_ = AllocateSet(set_layouts_[2]);

    // Four float targets.
    VkAttachmentDescription attachments[4] = {};
    VkAttachmentReference references[4];
    VkImageView views[4];
    for (uint32_t i = 0; i < 4; ++i) {
      targets_[i] = device_.MakeImage(
          VK_FORMAT_R32G32B32A32_SFLOAT, kTargetSize, kTargetSize, 1,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          VK_IMAGE_VIEW_TYPE_2D);
      attachments[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
      attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
      attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachments[i].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      references[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      views[i] = targets_[i].view;
    }
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 4;
    subpass.pColorAttachments = references;
    VkRenderPassCreateInfo render_pass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass_info.attachmentCount = 4;
    render_pass_info.pAttachments = attachments;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    RIG_VK(vkCreateRenderPass(vk, &render_pass_info, nullptr, &render_pass_));
    VkFramebufferCreateInfo framebuffer_info = {
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = render_pass_;
    framebuffer_info.attachmentCount = 4;
    framebuffer_info.pAttachments = views;
    framebuffer_info.width = kTargetSize;
    framebuffer_info.height = kTargetSize;
    framebuffer_info.layers = 1;
    RIG_VK(vkCreateFramebuffer(vk, &framebuffer_info, nullptr, &framebuffer_));

    // Textures: for each of two kinds (2D array, cube), unsigned and signed
    // data, two of each so that a fetch constant mixed up with another shows.
    std::mt19937 texture_rng(8);
    for (uint32_t i = 0; i < 8; ++i) {
      bool is_cube = (i & 4) != 0;
      textures_[i] = MakeTexture(
          (i & 2) ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM,
          is_cube, texture_rng);
    }
    for (uint32_t i = 0; i < 2; ++i) {
      VkSamplerCreateInfo sampler_info = {
          VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sampler_info.magFilter = i ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
      sampler_info.minFilter = sampler_info.magFilter;
      sampler_info.mipmapMode = i ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                  : VK_SAMPLER_MIPMAP_MODE_LINEAR;
      sampler_info.addressModeU = sampler_info.addressModeV =
          sampler_info.addressModeW =
              i ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                : VK_SAMPLER_ADDRESS_MODE_REPEAT;
      sampler_info.maxLod = VK_LOD_CLAMP_NONE;
      RIG_VK(vkCreateSampler(vk, &sampler_info, nullptr, &samplers_[i]));
    }

    // The interpreter's side is the same for every test.
    VkDescriptorSetLayoutBinding slot_bindings[24];
    for (uint32_t i = 0; i < 24; ++i) {
      slot_bindings[i] = {i,
                          i < 16 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                 : VK_DESCRIPTOR_TYPE_SAMPLER,
                          1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    interpreter_texture_layout_ = MakeSetLayout(slot_bindings, 24);
    interpreter_layout_ = MakePipelineLayout(interpreter_texture_layout_);
    interpreter_texture_set_ = AllocateSet(interpreter_texture_layout_);
    std::vector<uint32_t> vertex_words =
        ReadSpirv(options_.spirv_directory + "/rig_pixel.vs.spv");
    vertex_shader_ =
        device_.MakeShaderModule(vertex_words.data(), vertex_words.size() * 4);
    std::vector<uint32_t> interpreter_words =
        ReadSpirv(options_.spirv_directory + "/ucode_program.ps.spv");
    VkShaderModule interpreter_shader = device_.MakeShaderModule(
        interpreter_words.data(), interpreter_words.size() * 4);
    interpreter_pipeline_ =
        MakePipeline(interpreter_shader, interpreter_layout_);
  }

  // True if the interpreter wrote what the translated shader wrote.
  bool Run(const Test& test, std::mt19937& rng) {
    VkDevice vk = device_.device;
    Environment environment;
    FillEnvironment(environment, rng);
    // Every texture fetch constant: 8 by 8, a 2D texture, no biases; the
    // alpha test passes everything.
    for (uint32_t i = 0; i < 16; ++i) {
      uint32_t* fetch = environment.fetch_constants + i * 6;
      fetch[2] = (kTextureSize - 1) | ((kTextureSize - 1) << 13);
      fetch[5] = 1 << 9;
    }
    environment.system_constants.flags = 7 << 11;
    if (test.prepare) {
      test.prepare(environment);
    }

    xe::gpu::SpirvShader shader(xe::gpu::xenos::ShaderType::kPixel, 0,
                                test.ucode.data(), test.ucode.size(),
                                std::endian::little);
    xe::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    SpirvShaderTranslator translator(RigFeatures(), true, true, false);
    SpirvShaderTranslator::Modification modification(
        translator.GetDefaultPixelShaderModification(
            shader.GetDynamicAddressableRegisterCount(
                xe::gpu::xenos::kMaxShaderTempRegisters)));
    modification.pixel.interpolator_mask = 0xFFFF;
    modification.pixel.color_targets_used = 0xF;
    // No premultiplication for MIN and MAX blending.
    modification.pixel.rt0_blend_rgb_factor_for_premult =
        xe::gpu::xenos::BlendFactor::kOne;
    modification.pixel.rt0_blend_a_factor_for_premult =
        xe::gpu::xenos::BlendFactor::kOne;
    if (test.param_gen_register >= 0) {
      modification.pixel.param_gen_enable = 1;
      modification.pixel.param_gen_interpolator =
          uint32_t(test.param_gen_register);
    }
    xe::gpu::Shader::Translation* translation =
        shader.GetOrCreateTranslation(modification.value);
    if (!translator.TranslateAnalyzedShader(*translation) ||
        !translation->is_valid()) {
      std::printf("FAIL %s: the translator refused it\n", test.name.c_str());
      return false;
    }
    const std::vector<uint8_t>& translated = translation->translated_binary();
    if (options_.verbose) {
      // For spirv-dis, when the two disagree.
      std::string path = options_.spirv_directory + "/last_translated.ps.spv";
      if (FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(translated.data(), 1, translated.size(), file);
        std::fclose(file);
      }
    }
    VkShaderModule translated_shader = device_.MakeShaderModule(
        reinterpret_cast<const uint32_t*>(translated.data()),
        translated.size());

    ucode_program::Program program;
    ucode_program::Build(xe::gpu::xenos::ShaderType::kPixel, test.ucode.data(),
                         uint32_t(test.ucode.size()), program);
    const uint32_t kUnsupported =
        ucode_program::kFeatureMalformed | ucode_program::kFeatureFetchQuery |
        ucode_program::kFeatureMemoryExport |
        ucode_program::kFeatureTooManyTextures |
        ucode_program::kFeatureTexture3D | ucode_program::kFeatureTexture1D |
        ucode_program::kFeatureDepthOutput;
    if ((program.features & kUnsupported) ||
        program.element_count() > ucode_program::kCodeElements) {
      std::printf("FAIL %s: no program (features %08X, %u elements)\n",
                  test.name.c_str(), program.features, program.element_count());
      return false;
    }

    // The translated shader's textures and samplers, in its binding order.
    const auto& texture_bindings = shader.GetTextureBindingsAfterTranslation();
    const auto& sampler_bindings = shader.GetSamplerBindingsAfterTranslation();
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (size_t i = 0; i < texture_bindings.size() + sampler_bindings.size();
         ++i) {
      bindings.push_back({uint32_t(i),
                          i < texture_bindings.size()
                              ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                              : VK_DESCRIPTOR_TYPE_SAMPLER,
                          1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    }
    VkDescriptorSetLayout translated_texture_layout =
        MakeSetLayout(bindings.data(), uint32_t(bindings.size()));
    VkPipelineLayout translated_layout =
        MakePipelineLayout(translated_texture_layout);
    VkDescriptorSet translated_texture_set =
        AllocateSet(translated_texture_layout);
    for (size_t i = 0; i < texture_bindings.size(); ++i) {
      WriteTexture(translated_texture_set, uint32_t(i),
                   texture_bindings[i].fetch_constant,
                   texture_bindings[i].dimension ==
                       xe::gpu::xenos::FetchOpDimension::kCube,
                   texture_bindings[i].is_signed);
    }
    for (size_t i = 0; i < sampler_bindings.size(); ++i) {
      WriteSampler(translated_texture_set,
                   uint32_t(texture_bindings.size() + i),
                   sampler_bindings[i].fetch_constant);
    }
    VkPipeline translated_pipeline =
        MakePipeline(translated_shader, translated_layout);
    // The interpreter's, by slot. A slot nothing uses still needs something.
    for (uint32_t i = 0; i < ucode_program::kMaxTextures; ++i) {
      const ucode_program::Texture& slot = program.textures[i];
      bool is_cube = i >= ucode_program::kMaxTextures2D;
      WriteTexture(interpreter_texture_set_, i * 2, slot.fetch_constant,
                   is_cube, false);
      WriteTexture(interpreter_texture_set_, i * 2 + 1, slot.fetch_constant,
                   is_cube, true);
      WriteSampler(interpreter_texture_set_, 16 + i, slot.fetch_constant);
    }

    // Upload.
    std::memcpy(system_constants_.data, &environment.system_constants,
                sizeof(environment.system_constants));
    std::memcpy(float_full_.data, environment.float_constants[256], 256 * 16);
    const xe::gpu::Shader::ConstantRegisterMap& constant_map =
        shader.constant_register_map();
    uint8_t* packed = float_packed_.data;
    for (uint32_t i = 0; i < 256; ++i) {
      if (constant_map.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63))) {
        std::memcpy(packed, environment.float_constants[256 + i], 16);
        packed += 16;
      }
    }
    uint32_t* block = reinterpret_cast<uint32_t*>(bool_loop_program_.data);
    std::memcpy(block, environment.bool_constants, 32);
    std::memcpy(block + 8, environment.loop_constants, 128);
    uint32_t* head = block + ucode_program::kBoolLoopElements * 4;
    std::memset(head, 0, ucode_program::kHeadElements * 16);
    // The pixel program at element 0, and the pixel stage's word.
    head[1] = 0;
    head[3] = test.param_gen_register >= 0
                  ? 1 | (uint32_t(test.param_gen_register) << 1)
                  : 0;
    std::memcpy(head + ucode_program::kHeadElements * 4, program.words.data(),
                program.words.size() * 4);
    std::memcpy(fetch_constants_.data, environment.fetch_constants,
                sizeof(environment.fetch_constants));

    std::vector<float> results[2];
    Draw(translated_pipeline, translated_layout, 0, translated_texture_set,
         results[0]);
    Draw(interpreter_pipeline_, interpreter_layout_, 1,
         interpreter_texture_set_, results[1]);
    vkDestroyPipeline(vk, translated_pipeline, nullptr);
    vkDestroyShaderModule(vk, translated_shader, nullptr);
    vkFreeDescriptorSets(vk, descriptor_pool_, 1, &translated_texture_set);
    vkDestroyPipelineLayout(vk, translated_layout, nullptr);
    vkDestroyDescriptorSetLayout(vk, translated_texture_layout, nullptr);

    uint32_t mismatches = 0, killed = 0;
    float worst = 0.0f;
    for (size_t i = 0; i < results[0].size(); ++i) {
      // A target the guest shader never writes has no output in the
      // translated shader, and a write mask of 0 in a real pipeline.
      if (!(shader.writes_color_targets() &
            (1u << (i / (kTargetSize * kTargetSize * 4))))) {
        continue;
      }
      float e = results[0][i], a = results[1][i];
      if (i < kTargetSize * kTargetSize * 4 && (i & 3) == 0 &&
          e == kTargetClear) {
        ++killed;
      }
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
          size_t pixel = (i / 4) % (kTargetSize * kTargetSize);
          std::printf(
              "  %s: colour %zu, pixel %zu,%zu, %c: translated %.9g, "
              "interpreter %.9g\n",
              test.name.c_str(), i / (kTargetSize * kTargetSize * 4),
              pixel % kTargetSize, pixel / kTargetSize, "rgba"[i & 3],
              double(e), double(a));
        }
        ++mismatches;
      }
      worst = std::max(worst, difference);
    }
    if (mismatches) {
      std::printf("FAIL %s: %u of %zu values differ (worst %g)\n",
                  test.name.c_str(), mismatches, results[0].size(),
                  double(worst));
      if (options_.verbose) {
        std::printf("%s\n", shader.ucode_disassembly().c_str());
      }
      return false;
    }
    if (options_.verbose) {
      std::printf(
          "ok   %s (worst difference %g, %u pixels killed, program %u "
          "elements)\n",
          test.name.c_str(), double(worst), killed, program.element_count());
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

  VkPipelineLayout MakePipelineLayout(VkDescriptorSetLayout texture_layout) {
    VkDescriptorSetLayout layouts[4] = {set_layouts_[0], set_layouts_[1],
                                        set_layouts_[2], texture_layout};
    VkPipelineLayoutCreateInfo layout_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 4;
    layout_info.pSetLayouts = layouts;
    VkPipelineLayout layout;
    RIG_VK(vkCreatePipelineLayout(device_.device, &layout_info, nullptr,
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

  // Which of the eight textures a fetch constant has: by its lowest bit.
  void WriteTexture(VkDescriptorSet set, uint32_t binding,
                    uint32_t fetch_constant, bool is_cube, bool is_signed) {
    VkDescriptorImageInfo image_info = {
        VK_NULL_HANDLE,
        textures_[(fetch_constant & 1) | (is_signed ? 2 : 0) |
                  (is_cube ? 4 : 0)]
            .view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_.device, 1, &write, 0, nullptr);
  }

  void WriteSampler(VkDescriptorSet set, uint32_t binding,
                    uint32_t fetch_constant) {
    VkDescriptorImageInfo image_info = {samplers_[(fetch_constant >> 1) & 1],
                                        VK_NULL_HANDLE,
                                        VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_.device, 1, &write, 0, nullptr);
  }

  Image MakeTexture(VkFormat format, bool is_cube, std::mt19937& rng) {
    uint32_t layers = is_cube ? 6 : 1;
    Image image = device_.MakeImage(
        format, kTextureSize, kTextureSize, layers,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        is_cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        is_cube, kTextureMips);
    Buffer staging = device_.MakeBuffer(kTextureSize * kTextureSize * 4 * 6,
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkCommandBuffer command_buffer = device_.Begin();
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kTextureMips, 0,
                                layers};
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    // Every level its own noise (the same bytes for every level's first
    // texels would hide a wrong level of detail less well than this hides
    // nothing: each level is drawn from the generator in turn).
    for (uint32_t i = 0; i < kTextureSize * kTextureSize * 4 * 6; ++i) {
      staging.data[i] = uint8_t(rng());
    }
    for (uint32_t mip = 0; mip < kTextureMips; ++mip) {
      VkBufferImageCopy region = {};
      region.bufferOffset = mip * 64;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers};
      region.imageExtent = {kTextureSize >> mip, kTextureSize >> mip, 1};
      vkCmdCopyBufferToImage(command_buffer, staging.buffer, image.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    device_.SubmitAndWait(command_buffer);
    return image;
  }

  VkPipeline MakePipeline(VkShaderModule pixel_shader, VkPipelineLayout layout) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pixel_shader;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport = {0.0f, 0.0f, float(kTargetSize), float(kTargetSize),
                           0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {kTargetSize, kTargetSize}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachments[4] = {};
    for (VkPipelineColorBlendAttachmentState& attachment : blend_attachments) {
      attachment.colorWriteMask = 0xF;
    }
    VkPipelineColorBlendStateCreateInfo blend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 4;
    blend.pAttachments = blend_attachments;
    VkGraphicsPipelineCreateInfo pipeline_info = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = layout;
    pipeline_info.renderPass = render_pass_;
    VkPipeline pipeline;
    RIG_VK(vkCreateGraphicsPipelines(device_.device, VK_NULL_HANDLE, 1,
                                     &pipeline_info, nullptr, &pipeline));
    return pipeline;
  }

  void Draw(VkPipeline pipeline, VkPipelineLayout layout, uint32_t which,
            VkDescriptorSet texture_set, std::vector<float>& result) {
    VkCommandBuffer command_buffer = device_.Begin();
    VkClearValue clears[4];
    for (VkClearValue& clear : clears) {
      for (float& component : clear.color.float32) {
        component = kTargetClear;
      }
    }
    VkRenderPassBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin_info.renderPass = render_pass_;
    begin_info.framebuffer = framebuffer_;
    begin_info.renderArea = {{0, 0}, {kTargetSize, kTargetSize}};
    begin_info.clearValueCount = 4;
    begin_info.pClearValues = clears;
    vkCmdBeginRenderPass(command_buffer, &begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline);
    VkDescriptorSet sets[4] = {memory_set_, constant_sets_[which], empty_set_,
                               texture_set};
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            layout, 0, 4, sets, 0, nullptr);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(command_buffer);
    for (uint32_t i = 0; i < 4; ++i) {
      VkBufferImageCopy region = {};
      region.bufferOffset = i * kTargetSize * kTargetSize * 16;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {kTargetSize, kTargetSize, 1};
      vkCmdCopyImageToBuffer(command_buffer, targets_[i].image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback_.buffer, 1, &region);
    }
    device_.SubmitAndWait(command_buffer);
    result.resize(4 * kTargetSize * kTargetSize * 4);
    std::memcpy(result.data(), readback_.data, result.size() * 4);
  }

  Device& device_;
  const Options& options_;
  VkDescriptorSetLayout set_layouts_[3] = {};
  VkDescriptorSetLayout interpreter_texture_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout interpreter_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  Buffer shared_memory_, system_constants_, float_vertex_, float_full_,
      float_packed_, bool_loop_program_, fetch_constants_, readback_;
  VkDescriptorSet memory_set_ = VK_NULL_HANDLE;
  VkDescriptorSet constant_sets_[2] = {};
  VkDescriptorSet empty_set_ = VK_NULL_HANDLE;
  VkDescriptorSet interpreter_texture_set_ = VK_NULL_HANDLE;
  Image targets_[4];
  Image textures_[8];
  VkSampler samplers_[2] = {};
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
  VkShaderModule vertex_shader_ = VK_NULL_HANDLE;
  VkPipeline interpreter_pipeline_ = VK_NULL_HANDLE;
};
