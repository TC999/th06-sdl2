#version 450
// VertexTex1DiffuseXyz: 3D position (xyz) + diffuse + uv. Needs MVP.
// Used by DrawTriangleStripTextured3D / DrawTriangleFanTextured3D.
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 _pad;
    mat4 mvp;        // proj * view * world (precomputed on CPU)
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    v_color = in_color;
    v_uv    = in_uv;
}
