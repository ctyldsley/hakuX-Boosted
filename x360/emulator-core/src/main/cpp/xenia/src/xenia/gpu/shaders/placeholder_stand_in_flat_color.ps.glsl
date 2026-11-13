#version 460
#extension GL_GOOGLE_include_directive : require

// hakuX-Boosted: one variant of the stand-in pixel shader, see
// placeholder_stand_in.glsli. gen_android_spirv.py compiles it to
// bytecode/vulkan_spirv/placeholder_stand_in_flat_color_ps.h.
#define XE_STAND_IN_TEXTURE 0
#define XE_STAND_IN_COLOR 1
#include "placeholder_stand_in.glsli"
