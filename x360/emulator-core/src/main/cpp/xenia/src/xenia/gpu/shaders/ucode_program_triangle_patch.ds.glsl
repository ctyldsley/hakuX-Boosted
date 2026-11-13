#version 460
#extension GL_GOOGLE_include_directive : require

// hakuX-Boosted: the guest shader interpreter, version 2, as the
// tessellation evaluation shader of triangle patches indexed by patch.
// See ucode_program.glsli and ucode_program_vertex.glsli.
// gen_android_spirv.py compiles it to bytecode/vulkan_spirv/ucode_program_triangle_patch_ds.h.
// Equal spacing as compiled; VulkanPipelineCache patches the execution mode
// for the fractional spacing of the continuous and adaptive modes.
layout(triangles, equal_spacing, cw) in;
#define XE_UP_HOST XE_UP_HOST_TRIANGLE_PATCH
#include "ucode_program_vertex.glsli"
