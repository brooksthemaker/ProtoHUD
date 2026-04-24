#version 100
attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
varying vec2 fragTexCoord;
uniform mat4 mvp;

void main() {
    fragTexCoord = vertexTexCoord;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
