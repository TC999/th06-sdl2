#version 450
// RenderVertexInfo: 3D pos + uv (no diffuse). Used by DrawVertexBuffer3D.
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 _pad;
    mat4 mvp;
    vec4 fogColor;
    vec4 fogParams;
    vec4 textureFactor;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out float v_viewZ;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    gl_Position.y = -gl_Position.y;  // Vulkan NDC Y-down vs GL/D3D Y-up
    // Fix 19: GL DrawVertexBuffer3D applies textureFactor via glColor4ub before draw.
    v_color = pc.textureFactor;
    v_uv    = in_uv;
    v_viewZ = gl_Position.w;
}
