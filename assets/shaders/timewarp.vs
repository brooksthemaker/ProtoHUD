// timewarp.vs — vertex shader for async rotational timewarp
// Attributes bound at locations 0 (a_pos) and 1 (a_uv) by gl_utils link_program().
// The homography warp is applied per-pixel in the fragment shader (perspective-correct).
attribute vec2 a_pos;
attribute vec2 a_uv;

varying vec2 v_uv;

void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
}
