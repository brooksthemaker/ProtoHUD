// nv12.vs — vertex shader for camera NV12 → RGB blit
// Attributes bound at locations 0 (a_pos) and 1 (a_uv) by gl_utils link_program().
attribute vec2 a_pos;
attribute vec2 a_uv;

// Digital zoom: zoom=1.0 → identity; zoom>1.0 → crops to center 1/zoom of frame.
// center is normalized screen-space (0.5,0.5 = frame center).
uniform float u_zoom;
uniform vec2  u_center;

varying vec2 v_uv;

void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    // Camera DMA buffers have origin at top-left; flip V for GL bottom-left convention.
    vec2 uv = vec2(a_uv.x, 1.0 - a_uv.y);
    // Apply zoom crop: scale UV around the crop center.
    v_uv = (uv - u_center) / u_zoom + u_center;
}
