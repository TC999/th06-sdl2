#version 450
// VertexTex1Xyzrwh: pre-transformed screen-space pos + uv, NO diffuse.
// Used by RendererVulkan::DrawTriangleStripTex.
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 _pad;
    mat4 mvp;
    vec4 fogColor;
    vec4 fogParams;
} pc;

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out float v_viewZ;

void main() {
    float x_ndc = in_pos.x * pc.invScreen.x * 2.0 - 1.0;
    float y_ndc = in_pos.y * pc.invScreen.y * 2.0 - 1.0;
    gl_Position = vec4(x_ndc, y_ndc, in_pos.z, 1.0);
    v_color = vec4(1.0);  // implicit white
    v_uv    = in_uv;
    v_viewZ = 0.0;        // 2D path: fog disabled at CPU side regardless
}
