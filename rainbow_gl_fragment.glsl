#version 100

precision mediump float;
uniform vec2 resolution;
uniform vec3 background;
uniform float wobble;

vec3 hsv2rgb(vec3 c) {
    vec3 p = abs(fract(c.xxx + vec3(1.,2./3.,1./3.)) * 6.0 - vec3(3));
    return c.z * mix(vec3(1), clamp(p - vec3(1), 0.0, 1.0), c.y);
}

void main() {
  vec2 uv = gl_FragCoord.xy / resolution.xx - vec2(.5,0);

  float h = length(uv)*(resolution.x/resolution.y * 2.2) -1. + sin(uv.x * 10. + wobble) * .03;
	
  vec3 rgb = background;

  if (h > 0. && h < 1.) {
     float hue = .85*(mix(h, 1. - (1. - h*h), .5));
     float sat = 1.;
     float lum = clamp(atan(uv.y, uv.x) + .2, 0., 1.);
     rgb = hsv2rgb(vec3(hue,sat,lum));
     rgb = mix(background, rgb,  clamp(min(h, 1.-h)*100., 0., 1.));
  }
	
  gl_FragColor = vec4(rgb, 1. );
}
  
