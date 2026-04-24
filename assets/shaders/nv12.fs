// NV12 → RGB fragment shader — GLSL 1.00 (GLES 2.0 compatible)
//
// texture0 : Y  plane — R8 single-channel,  full resolution  (bound by Raylib)
// tex_uv   : UV plane — RG8 two-channel, half resolution  (bound manually to slot 1)
//            R = Cb (U),  G = Cr (V)
//
// Colour space: BT.601 full-range YCbCr → linear RGB
#version 100

precision mediump float;

varying vec2 fragTexCoord;

uniform sampler2D texture0;  // Y  plane
uniform sampler2D tex_uv;    // UV plane

void main() {
    float y  = texture2D(texture0, fragTexCoord).r;
    float cb = texture2D(tex_uv,   fragTexCoord).r - 0.5;
    float cr = texture2D(tex_uv,   fragTexCoord).g - 0.5;

    float r = y + 1.402  * cr;
    float g = y - 0.3441 * cb - 0.7141 * cr;
    float b = y + 1.772  * cb;

    gl_FragColor = vec4(clamp(r, 0.0, 1.0),
                        clamp(g, 0.0, 1.0),
                        clamp(b, 0.0, 1.0),
                        1.0);
}
