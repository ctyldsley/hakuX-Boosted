#version 460
#extension GL_GOOGLE_include_directive : require

// hakuX-Boosted: the guest shader interpreter, version 2, as a vertex shader.
// See ucode_program.glsli and ucode_program_vertex.glsli.
// gen_android_spirv.py compiles it to bytecode/vulkan_spirv/ucode_program_vs.h.
#define XE_UP_HOST XE_UP_HOST_VERTEX
#include "ucode_program_vertex.glsli"
