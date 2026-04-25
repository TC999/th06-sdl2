#version 450
// VertexTex1DiffuseXyzClip — CPU-pretransformed clip-space variant of
// VertexTex1DiffuseXyz, used exclusively by the Vulkan sprite batcher.
//
// Phase 6.2: TH06 issues per-sprite SetWorldTransform calls before each
// 3D-layout draw, which causes the per-PSO mvp PushConstant to differ
// between adjacent sprites and force a flush. Telemetry (vk_batch.log
// flush-reason histogram) showed Mvp dominating ~80% of all flushes.
//
// Solution: the batcher pre-multiplies (proj * view * world) on the CPU at
// vertex append time and writes the resulting clip-space vec4 here. This
// shader therefore does NOT consume pc.mvp; one PSO + one big TRIANGLE_LIST
// draw can carry sprites with arbitrarily different world transforms,
// while preserving:
//   - perspective division (pos is clip-space, GPU still does the divide)
//   - fog input semantics (v_viewZ = clip.w == view-space Z for D3D-style
//     perspective MVP, identical to v_tex1_diffuse_xyz.vert line 27)
//   - Vulkan NDC Y-down vs D3D Y-up flip (applied here too, identically)
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 _pad;
    mat4 mvp;            // unused (kept for PushConstants layout parity)
    vec4 fogColor;
    vec4 fogParams;
    vec4 textureFactor;  // unused (per-vertex color present)
} pc;

layout(location = 0) in vec4 in_pos_clip;  // pre-multiplied (proj*view*world*local)
layout(location = 1) in vec4 in_color;     // B8G8R8A8_UNORM, see VkPipelineCache.cpp
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out float v_viewZ;

void main() {
    // Mirror v_tex1_diffuse_xyz.vert line 23 (the standard Vulkan Y flip).
    gl_Position = vec4(in_pos_clip.x, -in_pos_clip.y, in_pos_clip.z, in_pos_clip.w);
    v_color     = in_color;
    v_uv        = in_uv;
    // D3D-style perspective MVP: clip.w == view-space Z (positive depth from
    // camera). Fragment shader's fog code consumes v_viewZ identically to
    // the non-pretransformed path.
    v_viewZ     = in_pos_clip.w;
}
