#version 460

// hakuX-Boosted: the vertex shader of the rig's pixel tests. A triangle over
// the whole target, with sixteen interpolators that differ from each other
// and from pixel to pixel. Interpolator 0 is a texture coordinate: X and Y
// from 0 to 1, Z over the six faces of a cube.

layout(location = 0) out vec4 xe_rig_interpolators[16];

void main() {
  vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(corner * 2.0 - 1.0, 0.5, 1.0);
  xe_rig_interpolators[0] =
      vec4(corner, (corner.x + corner.y) * 3.0, 1.0 - corner.x * 0.5);
  for (int i = 1; i < 16; ++i) {
    float n = float(i);
    xe_rig_interpolators[i] =
        vec4(corner.x * (1.0 + 0.25 * n) - 0.07 * n,
             corner.y * (2.0 - 0.125 * n) + 0.03 * n,
             (corner.x - corner.y) * n * 0.5, 0.5 + 0.1 * n - corner.y);
  }
}
