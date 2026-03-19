#pragma once
// =============================================================================
// RendererGLES.hpp - OpenGL ES 2.0 rendering backend for th06
// Implements IRenderer using shaders + VBO instead of fixed-function pipeline.
// =============================================================================

#include "IRenderer.hpp"
#include "sdl2_renderer.hpp" // for vertex structs, BlendMode enum
#include <vector>
#ifdef __ANDROID__
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif
#else
#include <SDL_opengl.h>
#endif

namespace th06
{

struct RendererGLES : public IRenderer
{
    enum Flush2DReason
    {
        Flush2D_Unknown = 0,
        Flush2D_State,
        Flush2D_Capacity,
        Flush2D_LeavePass,
        Flush2D_Surface,
        Flush2D_ImmediateFallback,
        Flush2D_Count
    };

    enum Flush3DReason
    {
        Flush3D_Unknown = 0,
        Flush3D_State,
        Flush3D_Capacity,
        Flush3D_Begin2D,
        Flush3D_BeginFrame,
        Flush3D_ViewportOrClear,
        Flush3D_FallbackImmediate,
        Flush3D_BlitOrReadback,
        Flush3D_EndFrame,
        Flush3D_Count
    };

    // Shader program
    GLuint shaderProgram;
    GLint loc_a_Position;
    GLint loc_a_Color;
    GLint loc_a_TexCoord;
    GLint loc_a_FogFactor;
    GLint loc_u_MVP;
    GLint loc_u_TexMatrix;
    GLint loc_u_ModelView;
    GLint loc_u_Texture;
    GLint loc_u_TextureEnabled;
    GLint loc_u_ColorOp;
    GLint loc_u_AlphaRef;
    GLint loc_u_FogEnabled;
    GLint loc_u_FogStart;
    GLint loc_u_FogEnd;
    GLint loc_u_FogColor;
    GLint loc_u_UseVertexFog;

    // CPU-side matrix state (replaces GL matrix stack)
    D3DXMATRIX modelviewMatrix;   // view * world
    D3DXMATRIX worldMatrix;
    D3DXMATRIX textureMatrix;

    // Current state tracking
    u8 textureEnabled;

    // FBO (GLES core — not EXT)
    GLuint fbo;
    GLuint fboColorTex;
    GLuint fboDepthRb;

    // Dynamic VBO for streaming vertices (used by 3D draws & fallback)
    GLuint vbo;

    // === 2D Quad Batch System ===
    struct BatchVertex {
        f32 x, y, z;     // 12 bytes
        u8 r, g, b, a;   //  4 bytes (normalized to float by GL)
        f32 u, v;         //  8 bytes
    };                    // 24 bytes total (was 36 with float4 color)
    static const i32 BATCH_MAX_QUADS = 2048;
    GLuint batchVBO;      // VBO for batch vertex data
    GLuint quadIBO;       // static IBO: 6 indices per quad
    std::vector<BatchVertex> batchBuffer; // CPU staging: 4 verts per quad
    i32 batchQuadCount;
    struct Batch3DVertex {
        f32 x, y, z, w;   // clip-space position
        u8 r, g, b, a;    // normalized color
        f32 u, v;
        f32 fogFactor;
    };
    static const i32 BATCH3D_MAX_QUADS = 2048;
    GLuint batch3DVBO;
    std::vector<Batch3DVertex> batch3DBuffer;
    i32 batch3DQuadCount;
    bool usingVertexFog;

    bool in2DPass;        // true while 2D ortho state is active
    bool mvpDirty;        // true when MVP uniform needs re-upload
    bool fogDirty;        // true when fog uniforms need re-upload

    // Saved 3D state for pass-level Enter/Leave
    GLint saved3D_scissor[4];
    bool saved3D_depthTestEnabled;
    GLboolean saved3D_depthMask;
    f32 viewportMinZ;
    f32 viewportMaxZ;

    // Reusable CPU-side interleaved vertex staging buffer (3D draws)
    std::vector<f32> drawScratch;
    bool attribsEnabled;
    bool fogAttribEnabled;

    // Per-frame debug stats
    struct FrameStats {
        u32 frames;
        u32 drawCalls;
        u32 immediate2DDraws;
        u32 immediate3DDraws;
        u32 batch2DFlushes;
        u32 batch3DFlushes;
        u32 batch2DQuads;
        u32 batch3DQuads;
        u32 batch3DRejectedTexMatrix;
        u32 vertexCount;
        u32 textureBinds;
        u32 bufferUploads;
        u64 bufferUploadBytes;
        u32 textureUploads;
        u64 textureUploadBytes;
        u32 fboBlits;
        u32 readbacks;
        u32 viewportChanges;
        u32 blendChanges;
        u32 colorOpChanges;
        u32 textureStageChanges;
        u32 fogChanges;
        u32 zwriteChanges;
        u32 depthFuncChanges;
        u32 pass2DEnters;
        u32 pass2DLeaves;
        u32 flush2DByReason[Flush2D_Count];
        u32 flush3DByReason[Flush3D_Count];
        double renderCpuMs;

        void Reset();
        void Accumulate(const FrameStats &other);
    } stats, statsInterval;
    u64 frameBeginCounter;
    double perfCounterToMs;
    u32 statsLastLogTicks;

    // Internal batch helpers
    void Enter2DPass();
    void Leave2DPass();
    void FlushBatch(Flush2DReason reason = Flush2D_Unknown);
    void Flush3DBatch(Flush3DReason reason = Flush3D_Unknown);
    bool CanBatch3DQuad() const;
    void FinishFrameStats();

    // --- IRenderer interface ---
    void Init(SDL_Window *win, SDL_GLContext ctx, i32 w, i32 h) override;
    void InitDevice(u32 opts) override;
    void Release() override;
    void ResizeTarget() override;
    void BeginScene() override;
    void EndScene() override;
    void BeginFrame() override;
    void EndFrame() override;

    void Clear(D3DCOLOR color, i32 clearColor, i32 clearDepth) override;
    void SetViewport(i32 x, i32 y, i32 w, i32 h, f32 minZ = 0.0f, f32 maxZ = 1.0f) override;

    void SetBlendMode(u8 mode) override;
    void SetColorOp(u8 colorOp) override;
    void SetTexture(u32 tex) override;
    void SetTextureFactor(D3DCOLOR factor) override;
    void SetZWriteDisable(u8 disable) override;
    void SetDepthFunc(i32 alwaysPass) override;
    void SetDestBlendInvSrcAlpha() override;
    void SetDestBlendOne() override;
    void SetTextureStageSelectDiffuse() override;
    void SetTextureStageModulateTexture() override;
    void SetFog(i32 enable, D3DCOLOR color, f32 start, f32 end) override;

    void SetViewTransform(const D3DXMATRIX *matrix) override;
    void SetProjectionTransform(const D3DXMATRIX *matrix) override;
    void SetWorldTransform(const D3DXMATRIX *matrix) override;
    void SetTextureTransform(const D3DXMATRIX *matrix) override;

    void DrawTriangleStrip(const VertexDiffuseXyzrwh *verts, i32 count) override;
    void DrawTriangleStripTex(const VertexTex1Xyzrwh *verts, i32 count) override;
    void DrawTriangleStripTextured(const VertexTex1DiffuseXyzrwh *verts, i32 count) override;
    void DrawTriangleStripTextured3D(const VertexTex1DiffuseXyz *verts, i32 count) override;
    void DrawTriangleFanTextured(const VertexTex1DiffuseXyzrwh *verts, i32 count) override;
    void DrawTriangleFanTextured3D(const VertexTex1DiffuseXyz *verts, i32 count) override;
    void DrawVertexBuffer3D(const RenderVertexInfo *verts, i32 count) override;

    u32 CreateTextureFromMemory(const u8 *data, i32 dataLen, D3DCOLOR colorKey, i32 *outWidth, i32 *outHeight) override;
    u32 CreateEmptyTexture(i32 width, i32 height) override;
    void DeleteTexture(u32 tex) override;
    void CopyAlphaChannel(u32 dstTex, const u8 *srcData, i32 dataLen, i32 width, i32 height) override;
    void UpdateTextureSubImage(u32 tex, i32 x, i32 y, i32 w, i32 h, const u8 *rgbaPixels) override;

    u32 LoadSurfaceFromFile(const u8 *data, i32 dataLen, i32 *outWidth, i32 *outHeight) override;
    u32 LoadSurfaceFromFile(const u8 *data, i32 dataLen, D3DXIMAGE_INFO *info) override;
    void CopySurfaceToScreen(u32 surfaceTex, i32 srcX, i32 srcY, i32 dstX, i32 dstY, i32 w, i32 h,
                             i32 texW, i32 texH) override;
    void CopySurfaceToScreen(u32 surfaceTex, i32 texW, i32 texH, i32 dstX, i32 dstY) override;
    void CopySurfaceRectToScreen(u32 surfaceTex, i32 srcX, i32 srcY, i32 srcW, i32 srcH,
                                 i32 dstX, i32 dstY, i32 texW, i32 texH) override;
    void TakeScreenshot(u32 dstTex, i32 left, i32 top, i32 width, i32 height) override;

    // Internal helpers
    void BlitFBOToScreen();
    void UploadUniforms();
    void UploadMVP();
    void DrawArrays(GLenum mode, const f32 *positions, const f32 *colors, const f32 *texcoords, i32 vertCount);
};

} // namespace th06
