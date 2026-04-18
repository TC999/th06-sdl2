#version 450
// RenderVertexInfo: 3D pos + uv (no diffuse). Used by DrawVertexBuffer3D.
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 _pad;
    mat4 mvp;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    v_color = vec4(1.0);
    v_uv    = in_uv;
}
