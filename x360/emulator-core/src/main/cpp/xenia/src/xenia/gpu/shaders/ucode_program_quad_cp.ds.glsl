#version 460
#extension GL_GOOGLE_include_directive : require

// hakuX-Boosted: the guest shader interpreter, version 2, as the
// tessellation evaluation shader of quad patches with indexed control points.
// See ucode_program.glsli and ucode_program_vertex.glsli.
// gen_android_spirv.py compiles it to bytecode/vulkan_spirv/ucode_program_quad_cp_ds.h.
// Equal spacing as compiled; VulkanPipelineCache patches the execution mode
// for the fractional spacing of the continuous and adaptive modes.
layout(quads, equal_spacing, cw) in;
#define XE_UP_HOST XE_UP_HOST_QUAD_CP
#include "ucode_program_vertex.glsli"
