#version 110

uniform vec3 sprite_color;

void main() {
  gl_FragColor = vec4(sprite_color, 1.0);
}
