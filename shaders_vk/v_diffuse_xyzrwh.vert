#version 450
// VertexDiffuseXyzrwh: pre-transformed screen-space pos + diffuse color, NO uv.
// Used by RendererVulkan::DrawTriangleStrip (no-texture path).

layout(push_constant) uniform PC {
    vec2 invScreen;  // 1/screenW, 1/screenH for ortho mapping
    vec2 _pad;
    mat4 mvp;        // unused for 2D; layout fixed for shader-stage-compat
    vec4 fogColor;   // unused by 2D no-tex path; layout-shared with textured frag
    vec4 fogParams;
    vec4 textureFactor;  // unused by this layout (per-vertex color present); kept for PC layout parity
} pc;

layout(location = 0) in vec4 in_pos;     // (x, y, z, rhw) — screen pixels
layout(location = 1) in vec4 in_color;   // unpacked from D3DCOLOR (BGRA -> RGBA done by VK_FORMAT_B8G8R8A8_UNORM)

layout(location = 0) out vec4 v_color;

void main() {
    // Vulkan NDC: y points DOWN; pixel y already grows downward → no flip.
    float x_ndc = in_pos.x * pc.invScreen.x * 2.0 - 1.0;
    float y_ndc = in_pos.y * pc.invScreen.y * 2.0 - 1.0;
    gl_Position = vec4(x_ndc, y_ndc, in_pos.z, 1.0);
    v_color = in_color;
}
