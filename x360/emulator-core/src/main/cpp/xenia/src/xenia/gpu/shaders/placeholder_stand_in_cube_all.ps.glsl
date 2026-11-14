#version 460
#extension GL_GOOGLE_include_directive : require

// hakuX-Boosted: one variant of the stand-in pixel shader, see
// placeholder_stand_in.glsli: with every interpolator as an input, for the
// shared stand-in pipelines of boosted/notes/standin-precompile.md.
// gen_android_spirv.py compiles it to
// bytecode/vulkan_spirv/placeholder_stand_in_cube_all_ps.h.
#define XE_STAND_IN_TEXTURE 2
#define XE_STAND_IN_COLOR 1
#define XE_STAND_IN_ALL 1
#include "placeholder_stand_in.glsli"
