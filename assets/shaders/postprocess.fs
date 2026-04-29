// postprocess.fs — edge highlight + background desaturation post-processing pass
//
// Samples a 3×3 neighbourhood (9 texels, fully unrolled for GLES 2.0 compatibility)
// to compute:
//   1. Sobel edge magnitude on the luma channel
//   2. Local luma contrast (max-min over 3×3) as a background depth proxy
//      — or real depth from a hardware depth camera when u_has_depth=1.0
//
// Both effects are independently gated by their strength uniforms (0 = off).
//
// Depth hardware path (future):
//   Bind an 8-bit GL_LUMINANCE depth texture to unit 1 and set u_has_depth=1.0.
//   Near objects should map to dark values (0), far to bright (1) so that
//   bg_weight = depth value (high depth = background = desaturate).

precision mediump float;

uniform sampler2D u_scene;      // camera FBO (RGBA8), unit 0
uniform sampler2D u_depth;      // depth map (GL_LUMINANCE), unit 1 (optional)
uniform vec2      u_texel;      // vec2(1/width, 1/height)
uniform float     u_edge_str;   // 0.0–1.0 edge overlay intensity
uniform float     u_desat_str;  // 0.0–1.0 background desaturation intensity
uniform vec3      u_edge_col;   // edge overlay colour (normalised RGB)
uniform float     u_threshold;  // local contrast below this = background (0.07–0.25)
uniform float     u_has_depth;  // 0.0 = use contrast proxy, 1.0 = sample u_depth
uniform float     u_edge_scale; // sampling step multiplier (1.0–5.0); larger = coarser outline
uniform float     u_edge_thresh;// minimum edge magnitude (0.0–0.6); suppresses weak interior edges

varying vec2 v_uv;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    // ── Sample 3×3 neighbourhood ──────────────────────────────────────────────
    // u_edge_scale widens the kernel: scale=1 detects all texture, scale=5 = silhouette only
    vec2 step = u_texel * u_edge_scale;
    vec3 c00 = texture2D(u_scene, v_uv + vec2(-1.0, -1.0) * step).rgb;
    vec3 c10 = texture2D(u_scene, v_uv + vec2( 0.0, -1.0) * step).rgb;
    vec3 c20 = texture2D(u_scene, v_uv + vec2( 1.0, -1.0) * step).rgb;
    vec3 c01 = texture2D(u_scene, v_uv + vec2(-1.0,  0.0) * step).rgb;
    vec3 c11 = texture2D(u_scene, v_uv).rgb;
    vec3 c21 = texture2D(u_scene, v_uv + vec2( 1.0,  0.0) * step).rgb;
    vec3 c02 = texture2D(u_scene, v_uv + vec2(-1.0,  1.0) * step).rgb;
    vec3 c12 = texture2D(u_scene, v_uv + vec2( 0.0,  1.0) * step).rgb;
    vec3 c22 = texture2D(u_scene, v_uv + vec2( 1.0,  1.0) * step).rgb;

    float l00 = luma(c00); float l10 = luma(c10); float l20 = luma(c20);
    float l01 = luma(c01); float l11 = luma(c11); float l21 = luma(c21);
    float l02 = luma(c02); float l12 = luma(c12); float l22 = luma(c22);

    // ── Sobel edge detection ──────────────────────────────────────────────────
    float gx = -l00 - 2.0*l01 - l02 + l20 + 2.0*l21 + l22;
    float gy = -l00 - 2.0*l10 - l20 + l02 + 2.0*l12 + l22;
    float raw_edge = clamp(sqrt(gx*gx + gy*gy) * 4.0, 0.0, 1.0);
    // Remap so edges below u_edge_thresh vanish and strong edges stay at 1.0
    float edge = clamp((raw_edge - u_edge_thresh) / (1.0 - u_edge_thresh + 0.001), 0.0, 1.0);

    // ── Background weight ─────────────────────────────────────────────────────
    float bg_weight;
    if (u_has_depth > 0.5) {
        bg_weight = texture2D(u_depth, v_uv).r;
    } else {
        // Local contrast proxy: low contrast = background
        float lo = min(min(min(l00, l10), min(l20, l01)),
                       min(min(l11, l21), min(l02, min(l12, l22))));
        float hi = max(max(max(l00, l10), max(l20, l01)),
                       max(max(l11, l21), max(l02, max(l12, l22))));
        float contrast = hi - lo;
        bg_weight = clamp(1.0 - contrast / max(u_threshold, 0.01), 0.0, 1.0);
    }

    // ── Desaturate background ─────────────────────────────────────────────────
    // Outlined pixels stay in full color: strong edges suppress desaturation.
    // When edge detection is off (u_edge_str=0) this reduces to standard bg_weight behaviour.
    vec3  grey  = vec3(l11);
    vec3  color = mix(c11, grey, bg_weight * (1.0 - edge * u_edge_str) * u_desat_str);

    // ── Overlay edges ─────────────────────────────────────────────────────────
    color = mix(color, u_edge_col, edge * u_edge_str);

    gl_FragColor = vec4(color, 1.0);
}
