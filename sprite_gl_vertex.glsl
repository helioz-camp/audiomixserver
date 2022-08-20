#version 110
attribute vec3 sprite_position;

uniform mat4 sprite_model;
uniform mat4 sprite_view;
uniform mat4 sprite_projection;

void main() {
     gl_Position = sprite_projection * sprite_view * sprite_model * vec4(sprite_position, 1.0);
}