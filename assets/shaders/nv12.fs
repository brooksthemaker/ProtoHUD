// nv12.fs — fragment shader for NV12 DMA camera via GL_TEXTURE_EXTERNAL_OES
//
// The full NV12 buffer (Y + interleaved UV planes) is imported as a single
// multi-plane EGLImage with DRM_FORMAT_NV12 and bound to a GL_TEXTURE_EXTERNAL_OES
// texture.  Mesa's V3D driver on Pi5 handles the YCbCr→RGB conversion in the
// TMU, so sampling the external texture returns linear RGB directly.
//
// If the output appears in wrong colours (driver does not auto-convert), replace
// the body of main() with the manual BT.601 conversion block below.
#extension GL_OES_EGL_image_external : require
precision mediump float;

uniform samplerExternalOES tex;
varying vec2 v_uv;

void main() {
    gl_FragColor = texture2D(tex, v_uv);
}

// ── Manual BT.601 full-range fallback (uncomment if colours are wrong) ─────────
// When the driver does not auto-convert, samplerExternalOES returns raw NV12 data:
//   .r = Y (luma), sampled at full resolution
//   .rg = UV (chroma), driver may or may not interleave automatically.
// In that case, use two separate R8 / RG88 EGLImages and this manual path:
//
// void main() {
//     float y  = texture2D(tex_y,  v_uv).r;
//     float cb = texture2D(tex_uv, v_uv).r - 0.5;
//     float cr = texture2D(tex_uv, v_uv).g - 0.5;
//     float r  = y + 1.402  * cr;
//     float g  = y - 0.3441 * cb - 0.7141 * cr;
//     float b  = y + 1.772  * cb;
//     gl_FragColor = vec4(clamp(r,0.0,1.0), clamp(g,0.0,1.0), clamp(b,0.0,1.0), 1.0);
// }
