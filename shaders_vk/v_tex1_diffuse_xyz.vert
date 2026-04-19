#version 450
// VertexTex1DiffuseXyz: 3D position (xyz) + diffuse + uv. Needs MVP.
// Used by DrawTriangleStripTextured3D / DrawTriangleFanTextured3D.
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 _pad;
    mat4 mvp;        // proj * view * world (precomputed on CPU)
    vec4 fogColor;
    vec4 fogParams;
    vec4 textureFactor;  // unused by this layout (per-vertex color present); kept for PC layout parity
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out float v_viewZ;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    gl_Position.y = -gl_Position.y;  // Vulkan NDC Y-down vs GL/D3D Y-up
    v_color = in_color;
    v_uv    = in_uv;
    // For D3D-style perspective MVP, clip.w == view-space Z (depth from camera, positive).
    v_viewZ = gl_Position.w;
}
