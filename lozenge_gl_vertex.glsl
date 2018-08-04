#version 110

uniform vec2 lozenge_center;
uniform vec2 lozenge_size;
attribute vec2 position;
varying vec2 lozenge_position;

void main() {
  gl_Position = vec4(position*lozenge_size + lozenge_center, 0.0, 1.0);
  lozenge_position = position;
}
  