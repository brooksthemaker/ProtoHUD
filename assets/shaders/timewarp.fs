// timewarp.fs — fragment shader for async rotational timewarp
//
// warp_matrix : 3×3 homography (column-major mat3, uploaded by timewarp.cpp)
//               Maps output UV coords → source eye-buffer UV coords.
//               Represents the rotational difference between the pose at render
//               time and the latest IMU pose at scan-out time.
//
// u_tex       : source eye-buffer (rendered frame from the previous draw pass)
//
// Pixels that map outside [0,1] UV range are filled with black (out-of-frame area).
precision mediump float;

uniform sampler2D u_tex;
uniform mat3      warp_matrix;

varying vec2 v_uv;

void main() {
    // Apply the 3×3 homography: output UV → source UV
    vec3 h      = warp_matrix * vec3(v_uv, 1.0);
    vec2 src_uv = h.xy / h.z;

    // Mask out-of-frame pixels (use step() to avoid a conditional on the texture fetch)
    float valid = step(0.0, src_uv.x) * step(src_uv.x, 1.0)
                * step(0.0, src_uv.y) * step(src_uv.y, 1.0);

    vec4 color  = texture2D(u_tex, clamp(src_uv, 0.0, 1.0));
    gl_FragColor = vec4(color.rgb * valid, 1.0);
}
