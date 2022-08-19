#version 110
attribute vec3 sprite_position;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main() {
     gl_Position = projection * view * model * vec4(sprite_position, 1.0);
}