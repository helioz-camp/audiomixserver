#version 110

uniform vec2 resolution;
uniform vec3 background;
uniform float wobble;
uniform float fire_start;

vec3 hsv2rgb(vec3 c) {
    vec3 p = abs(fract(c.xxx + vec3(1.,2./3.,1./3.)) * 6.0 - vec3(3));
    return c.z * mix(vec3(1), clamp(p - vec3(1), 0.0, 1.0), c.y);
}

void rainbow_main() {
  vec2 uv = gl_FragCoord.xy / resolution.xx - vec2(.5,0);

  float h = length(uv)*(resolution.x/resolution.y * 2.2) -1. + sin(uv.x * 10. + wobble) * .03;
	
  vec3 rgb = background;

  if (h > 0. && h < 1.) {
     float inverse_h = 1. - h;
     float hue = .85*(mix(inverse_h, 1. - (1. - inverse_h*inverse_h), .5));
     float sat = 1.;
     float lum = clamp(atan(uv.y, uv.x) + .2, 0., 1.);
     rgb = hsv2rgb(vec3(hue,sat,lum));
     rgb = mix(background, rgb,  clamp(min(h, 1.-inverse_h)*100., 0., 1.));
     gl_FragColor = vec4(rgb, 0.25);
  } else {
    gl_FragColor = vec4(0,0,0,0);
  }
}

// Occlude the top right corner, dark unless the fire is on
void main() {
  if (fire_start == 0.0) {
    vec2 uv = gl_FragCoord.xy / resolution.xy;
	float len = clamp(length(uv) + sin(wobble)*0.1, 0.0, 1.0);
	gl_FragColor = vec4(1. - len, 1. - len, 1. - len, len);
  } else if (fire_start > -10.) {
    vec2 uv = gl_FragCoord.xy / resolution.xy;
	float len = clamp(length(uv) + sin(3.*wobble)*0.1, 0.0, 1.0);
	gl_FragColor = vec4(clamp(len+0.1*cos(wobble), 0.0, 1.0), len*.8, len*.8, len);
  } else {
	rainbow_main();
  }
}