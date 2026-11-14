#version 460
#extension GL_GOOGLE_include_directive : require

// hakuX-Boosted: the guest shader interpreter, version 2, as the vertex
// shader of point sprites drawn as two-triangle strips.
// See ucode_program.glsli and ucode_program_vertex.glsli.
// gen_android_spirv.py compiles it to bytecode/vulkan_spirv/ucode_program_point_strip_vs.h.
#define XE_UP_HOST XE_UP_HOST_POINT_STRIP
#include "ucode_program_vertex.glsli"
