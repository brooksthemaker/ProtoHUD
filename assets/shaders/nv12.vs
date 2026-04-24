// Passthrough vertex shader — GLSL 1.00 (GLES 2.0 compatible)
// Raylib populates vertexPosition, vertexTexCoord, vertexColor, and mvp.
#version 100

attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
attribute vec4 vertexColor;

varying vec2 fragTexCoord;
varying vec4 fragColor;

uniform mat4 mvp;

void main() {
    fragTexCoord = vertexTexCoord;
    fragColor    = vertexColor;
    gl_Position  = mvp * vec4(vertexPosition, 1.0);
}
