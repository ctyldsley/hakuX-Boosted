#version 460
#extension GL_GOOGLE_include_directive : require

// hakuX-Boosted: the guest shader interpreter, version 2, as a pixel shader.
// See ucode_program.glsli and ucode_program_pixel.glsli.
// gen_android_spirv.py compiles it to bytecode/vulkan_spirv/ucode_program_ps.h.
#include "ucode_program_pixel.glsli"
