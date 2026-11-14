#version 460

// hakuX-Boosted: part of the first-run rig (README.md here), not of the app.
// Writes what a vertex shader under test put out (the position and sixteen
// interpolators) into a buffer, one point a vertex.

layout(points) in;
layout(points, max_vertices = 1) out;

layout(location = 0) in vec4 rig_in_interpolators[][16];

layout(std430, set = 4, binding = 0) buffer rig_capture_block {
  vec4 rig_capture[];
};

void main() {
  uint base = uint(gl_PrimitiveIDIn) * 17u;
  rig_capture[base] = gl_in[0].gl_Position;
  for (uint i = 0u; i < 16u; ++i) {
    rig_capture[base + 1u + i] = rig_in_interpolators[0][i];
  }
}
