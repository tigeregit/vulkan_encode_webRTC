#version 450
layout(location=0) in vec3 n;
layout(location=1) in vec4 c;
layout(location=0) out vec4 outColor;
void main(){float light=.28+.72*abs(dot(normalize(n),normalize(vec3(.4,.8,.6))));outColor=vec4(c.rgb*light,c.a);}
