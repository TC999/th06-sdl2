#version 450
// Textured fragment shader with spec-const-driven combiner op.
// COLOR_OP=0 -> MODULATE (vertex_color * tex)         — D3DTOP_MODULATE / GL_MODULATE baseline
// COLOR_OP=1 -> ADD      (saturate(vertex_color + tex)) — D3DTOP_ADD / GL_ADD
//
// Spec consts are baked into VkPipeline at create time and participate in the L2 cache key
// (ADR-007). Switching combiner op = different VkPipeline.

layout(constant_id = 0) const int COLOR_OP = 0;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 t = texture(u_tex, v_uv);
    if (COLOR_OP == 1) {
        out_color = clamp(v_color + t, 0.0, 1.0);
        out_color.a = v_color.a * t.a;  // alpha stays multiplicative (D3D8 default)
    } else {
        out_color = v_color * t;
    }
}
