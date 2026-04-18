#version 450
// VertexTex1DiffuseXyzrwh: pre-transformed pos + diffuse + uv (full 2D).
// Used by DrawTriangleStripTextured / DrawTriangleFanTextured / CopySurfaceToScreen.
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 _pad;
    mat4 mvp;
} pc;

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    float x_ndc = in_pos.x * pc.invScreen.x * 2.0 - 1.0;
    float y_ndc = in_pos.y * pc.invScreen.y * 2.0 - 1.0;
    gl_Position = vec4(x_ndc, y_ndc, in_pos.z, 1.0);
    v_color = in_color;
    v_uv    = in_uv;
}
