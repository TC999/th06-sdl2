#version 450
// Textured fragment shader with spec-const-driven combiner op + linear D3D fog.
// COLOR_OP=0 -> MODULATE (vertex_color * tex)         — D3DTOP_MODULATE / GL_MODULATE baseline
// COLOR_OP=1 -> ADD      (saturate(vertex_color + tex)) — D3DTOP_ADD / GL_ADD
//
// Spec consts are baked into VkPipeline at create time and participate in the L2 cache key
// (ADR-007). Switching combiner op = different VkPipeline.
//
// Fog: D3DFOGMODE_LINEAR replication of RendererGL::SetFog (sdl2_renderer.cpp:487).
// Activated only when CPU pushes fogParams.z >= 1.0 (see RendererVulkan::drawCommon, gated
// on `depthTest && fogEnabled` so 2D paths can never trigger it even if v_viewZ leaks 0.0).

layout(constant_id = 0) const int COLOR_OP = 0;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in float v_viewZ;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 _pad;
    mat4 mvp;
    vec4 fogColor;
    vec4 fogParams; // [start, end, enabled (1.0/0.0), pad]
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 t = texture(u_tex, v_uv);
    vec4 baseColor;
    if (COLOR_OP == 1) {
        baseColor = clamp(v_color + t, 0.0, 1.0);
        baseColor.a = v_color.a * t.a;  // alpha stays multiplicative (D3D8 default)
    } else {
        baseColor = v_color * t;
    }

    // Alpha test: matches RendererGL Init (sdl2_renderer.cpp:148):
    //   glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GEQUAL, 4.0/255.0);
    // GLES emulates with `discard` (gles_shaders.h:104). TH06 leaves alpha test
    // permanently enabled; threshold 4/255 kills sub-pixel sprite-edge fringes.
    if (baseColor.a < (4.0 / 255.0)) {
        discard;
    }

    if (pc.fogParams.z >= 0.5) {
        // GL_LINEAR / D3DFOGMODE_LINEAR: f = (end - viewZ) / (end - start), clamp 0..1.
        // f = 1 at viewZ <= start (no fog), f = 0 at viewZ >= end (full fog).
        float denom = max(pc.fogParams.y - pc.fogParams.x, 1e-6);
        float f = clamp((pc.fogParams.y - v_viewZ) / denom, 0.0, 1.0);
        baseColor.rgb = mix(pc.fogColor.rgb, baseColor.rgb, f);
    }

    out_color = baseColor;
}
