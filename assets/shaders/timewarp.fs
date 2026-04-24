#version 100
precision mediump float;
varying vec2 fragTexCoord;
uniform sampler2D texture0;
uniform mat4 warp_matrix;

void main() {
    vec4 warped = warp_matrix * vec4(fragTexCoord, 1.0, 1.0);
    vec2 uv = warped.xy / warped.z;

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        gl_FragColor = texture2D(texture0, uv);
    }
}
