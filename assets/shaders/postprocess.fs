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
uniform float     u_focus_str;  // 0.0=contrast proxy only, 1.0=Laplacian sharpness only for bg_weight
uniform float     u_focus_sens; // Laplacian sensitivity (scales with lens proximity)
uniform float     u_gate_scale; // 0.0=off, else=step multiplier for confirmatory coarse Sobel (size filter)
uniform sampler2D u_prev_frame;    // unit 2: previous raw frame (RGBA8) for motion detection
uniform float     u_motion_str;    // 0.0=off; motion highlight intensity
uniform float     u_motion_thresh; // min luma delta to count as motion (0.01–0.15)
uniform float     u_motion_radius; // sampling step in texels (controls detection tightness)
uniform vec3      u_motion_col;    // motion highlight colour (normalised RGB)
uniform float     u_motion_line;   // 0.0=filled blob, 1.0=fine boundary line

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
    float raw_edge = clamp(sqrt(gx*gx + gy*gy) * 1.5, 0.0, 1.0);
    // Remap so edges below u_edge_thresh vanish and strong edges stay at 1.0
    float edge = clamp((raw_edge - u_edge_thresh) / (1.0 - u_edge_thresh + 0.001), 0.0, 1.0);

    // ── Dual-scale size gate ──────────────────────────────────────────────────
    // Confirm edge at a coarser scale: small objects/noise vanish at larger step,
    // so the product suppresses them while large objects pass both tests.
    // g11 (center) is shared with l11 — 8 new samples only.
    if (u_gate_scale >= 1.0) {
        vec2 step2  = step * u_gate_scale;
        float g00 = luma(texture2D(u_scene, v_uv + vec2(-1.0,-1.0)*step2).rgb);
        float g10 = luma(texture2D(u_scene, v_uv + vec2( 0.0,-1.0)*step2).rgb);
        float g20 = luma(texture2D(u_scene, v_uv + vec2( 1.0,-1.0)*step2).rgb);
        float g01 = luma(texture2D(u_scene, v_uv + vec2(-1.0, 0.0)*step2).rgb);
        float g21 = luma(texture2D(u_scene, v_uv + vec2( 1.0, 0.0)*step2).rgb);
        float g02 = luma(texture2D(u_scene, v_uv + vec2(-1.0, 1.0)*step2).rgb);
        float g12 = luma(texture2D(u_scene, v_uv + vec2( 0.0, 1.0)*step2).rgb);
        float g22 = luma(texture2D(u_scene, v_uv + vec2( 1.0, 1.0)*step2).rgb);
        float gx2 = -g00 - 2.0*g01 - g02 + g20 + 2.0*g21 + g22;
        float gy2 = -g00 - 2.0*g10 - g20 + g02 + 2.0*g12 + g22;
        float edge2 = clamp(sqrt(gx2*gx2 + gy2*gy2) * 1.5, 0.0, 1.0);
        edge = edge * edge2;
    }

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

    // ── Focus-based sharpness refinement ──────────────────────────────────────
    // Laplacian at center pixel: measures how sharp/in-focus this pixel is.
    // High = sharp (in focus = foreground). Low = blurry (out of focus = background).
    // Sensitivity scales with lens proximity: close focus → narrow DoF → stronger separation.
    float lap        = abs(8.0*l11 - l00 - l10 - l20 - l01 - l21 - l02 - l12 - l22);
    float sharpness  = clamp(lap * u_focus_sens, 0.0, 1.0);
    bg_weight        = mix(bg_weight, 1.0 - sharpness, u_focus_str);

    // ── Desaturate background ─────────────────────────────────────────────────
    // Outlined pixels stay in full color: strong edges suppress desaturation.
    // When edge detection is off (u_edge_str=0) this reduces to standard bg_weight behaviour.
    vec3  grey  = vec3(l11);
    vec3  color = mix(c11, grey, bg_weight * (1.0 - edge * u_edge_str) * u_desat_str);

    // ── Overlay edges ─────────────────────────────────────────────────────────
    color = mix(color, u_edge_col, edge * u_edge_str);

    // ── Motion highlight (temporal frame diff) ────────────────────────────────
    // Fine-line mode (u_motion_line=1): uses the RANGE of motion values across
    // the 5-sample neighborhood — high range means this pixel is at the boundary
    // between moving and still regions, giving a 1-2px line hugging the silhouette.
    // Fill mode (u_motion_line=0): uses the MAX (dilation), floods the whole region.
    if (u_motion_str > 0.0) {
        vec2 mstep = u_texel * u_motion_radius;
        float p11 = luma(texture2D(u_prev_frame, v_uv).rgb);
        float p00 = luma(texture2D(u_prev_frame, v_uv + vec2(-1.0,-1.0)*mstep).rgb);
        float p20 = luma(texture2D(u_prev_frame, v_uv + vec2( 1.0,-1.0)*mstep).rgb);
        float p02 = luma(texture2D(u_prev_frame, v_uv + vec2(-1.0, 1.0)*mstep).rgb);
        float p22 = luma(texture2D(u_prev_frame, v_uv + vec2( 1.0, 1.0)*mstep).rgb);
        float s00 = luma(texture2D(u_scene, v_uv + vec2(-1.0,-1.0)*mstep).rgb);
        float s20 = luma(texture2D(u_scene, v_uv + vec2( 1.0,-1.0)*mstep).rgb);
        float s02 = luma(texture2D(u_scene, v_uv + vec2(-1.0, 1.0)*mstep).rgb);
        float s22 = luma(texture2D(u_scene, v_uv + vec2( 1.0, 1.0)*mstep).rgb);
        float mc   = abs(l11  - p11);
        float mn00 = abs(s00  - p00);
        float mn20 = abs(s20  - p20);
        float mn02 = abs(s02  - p02);
        float mn22 = abs(s22  - p22);
        float m_max = max(mc, max(max(mn00, mn20), max(mn02, mn22)));
        float m_min = min(mc, min(min(mn00, mn20), min(mn02, mn22)));
        // Fine-line: only pixels at the motion boundary (range = max - min is large)
        float motion = mix(m_max, m_max - m_min, u_motion_line);
        float mw = clamp((motion - u_motion_thresh) / (1.0 - u_motion_thresh + 0.001), 0.0, 1.0);
        color = mix(color, u_motion_col, mw * u_motion_str);
    }

    gl_FragColor = vec4(color, 1.0);
}
