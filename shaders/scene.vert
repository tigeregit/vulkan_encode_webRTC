#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec4 color;
layout(push_constant) uniform Constants { mat4 mvp; } pc;
layout(location=0) out vec3 n;
layout(location=1) out vec4 c;
void main(){gl_Position=pc.mvp*vec4(position,1);n=normal;c=color;}
