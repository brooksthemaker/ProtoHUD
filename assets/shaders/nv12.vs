// nv12.vs — vertex shader for camera NV12 → RGB blit
// Attributes bound at locations 0 (a_pos) and 1 (a_uv) by gl_utils link_program().
attribute vec2 a_pos;
attribute vec2 a_uv;

// Digital zoom: zoom=1.0 → identity; zoom>1.0 → crops to center 1/zoom of frame.
// center is normalized screen-space (0.5,0.5 = frame center).
uniform float u_zoom;
uniform vec2  u_center;

// CSI camera rotation in radians (0, π/2, π, 3π/2). Free in the vertex
// shader — it's just two multiplies and a 2x2 matrix on the 4 quad
// vertices. At 90°/270° the rotated UV square doesn't perfectly cover
// non-square cameras; CLAMP_TO_EDGE on the texture hides the seams.
uniform float u_rotation_rad;

varying vec2 v_uv;

void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    // Camera DMA buffers have origin at top-left; flip V for GL bottom-left convention.
    vec2 uv = vec2(a_uv.x, 1.0 - a_uv.y);
    // Apply rotation around (0.5, 0.5) before zoom so the zoom centre stays
    // anchored on the displayed image rather than the raw sensor frame.
    float c = cos(u_rotation_rad);
    float s = sin(u_rotation_rad);
    uv = mat2(c, -s, s, c) * (uv - 0.5) + 0.5;
    // Apply zoom crop: scale UV around the crop center.
    v_uv = (uv - u_center) / u_zoom + u_center;
}
