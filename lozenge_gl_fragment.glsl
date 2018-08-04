#version 110

uniform vec3 lozenge_color;
uniform float lozenge_body_width;
varying vec2 lozenge_position;

void main() {
  vec4 rgb = vec4(0.,0.,0.,0);
  float cutoff = 1. - 1./(lozenge_body_width+1.);
  
  if (abs(lozenge_position.x) > cutoff) {
    vec2 a = vec2((lozenge_position.x-cutoff*sign(lozenge_position.x)) * 1./(1.-cutoff), lozenge_position.y);
    float m = dot(a, a);
      if (m < 1.) {
           rgb = vec4(lozenge_color, 1.);
       } 
  } else {
    rgb = vec4(lozenge_color, 1.);
  }

  gl_FragColor = rgb;
}
  
