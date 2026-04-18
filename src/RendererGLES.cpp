// =============================================================================
// RendererGLES.cpp - OpenGL ES 2.0 / GL 2.0+ shader-based rendering backend
// Implements IRenderer using shaders + VBO. No fixed-function calls.
// =============================================================================

#include "RendererGLES.hpp"
#include "AnmManager.hpp"
#include "Supervisor.hpp"
#include "TouchVirtualButtons.hpp"
#include "MenuTouchButtons.hpp"
#include "thprac_gui_integration.h"
#include "gles_shaders.h"
#include <SDL_image.h>
#include <cstring>
#include <cstdio>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

// ---------------------------------------------------------------------------
// GL 2.0 shader / FBO function pointers (loaded via SDL_GL_GetProcAddress)
// On desktop GL these are extensions; on real GLES 2.0 they are core.
// We load them at runtime so the same binary works on both paths.
// ---------------------------------------------------------------------------
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void   (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void   (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void   (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void   (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void   (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void   (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void   (APIENTRY *PFN_glUseProgram)(GLuint);
typedef GLint  (APIENTRY *PFN_glGetAttribLocation)(GLuint, const GLchar *);
typedef GLint  (APIENTRY *PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void   (APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform1f)(GLint, GLfloat);
typedef void   (APIENTRY *PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void   (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef void   (APIENTRY *PFN_glDisableVertexAttribArray)(GLuint);
typedef void   (APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void   (APIENTRY *PFN_glDeleteShader)(GLuint);
typedef void   (APIENTRY *PFN_glDeleteProgram)(GLuint);
typedef void   (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void   (APIENTRY *PFN_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
// FBO (core in GL 3.0 / GLES 2.0, ARB extension on GL 2.x desktop)
typedef void   (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void   (APIENTRY *PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);

static PFN_glCreateShader            pglCreateShader;
static PFN_glShaderSource            pglShaderSource;
static PFN_glCompileShader           pglCompileShader;
static PFN_glGetShaderiv             pglGetShaderiv;
static PFN_glGetShaderInfoLog        pglGetShaderInfoLog;
static PFN_glCreateProgram           pglCreateProgram;
static PFN_glAttachShader            pglAttachShader;
static PFN_glLinkProgram             pglLinkProgram;
static PFN_glGetProgramiv            pglGetProgramiv;
static PFN_glGetProgramInfoLog       pglGetProgramInfoLog;
static PFN_glUseProgram              pglUseProgram;
static PFN_glGetAttribLocation       pglGetAttribLocation;
static PFN_glGetUniformLocation      pglGetUniformLocation;
static PFN_glUniform1i               pglUniform1i;
static PFN_glUniform1f               pglUniform1f;
static PFN_glUniform4f               pglUniform4f;
static PFN_glUniformMatrix4fv        pglUniformMatrix4fv;
static PFN_glEnableVertexAttribArray  pglEnableVertexAttribArray;
static PFN_glDisableVertexAttribArray pglDisableVertexAttribArray;
static PFN_glVertexAttribPointer     pglVertexAttribPointer;
static PFN_glDeleteShader            pglDeleteShader;
static PFN_glDeleteProgram           pglDeleteProgram;
static PFN_glGenBuffers              pglGenBuffers;
static PFN_glDeleteBuffers           pglDeleteBuffers;
static PFN_glBindBuffer              pglBindBuffer;
static PFN_glBufferData              pglBufferData;
static PFN_glBufferSubData           pglBufferSubData;
static PFN_glGenFramebuffers         pglGenFramebuffers;
static PFN_glDeleteFramebuffers      pglDeleteFramebuffers;
static PFN_glBindFramebuffer         pglBindFramebuffer;
static PFN_glFramebufferTexture2D    pglFramebufferTexture2D;
static PFN_glGenRenderbuffers        pglGenRenderbuffers;
static PFN_glDeleteRenderbuffers     pglDeleteRenderbuffers;
static PFN_glBindRenderbuffer        pglBindRenderbuffer;
static PFN_glRenderbufferStorage     pglRenderbufferStorage;
static PFN_glFramebufferRenderbuffer pglFramebufferRenderbuffer;
static PFN_glCheckFramebufferStatus  pglCheckFramebufferStatus;

#define GL_FRAMEBUFFER                0x8D40
#define GL_RENDERBUFFER               0x8D41
#define GL_COLOR_ATTACHMENT0          0x8CE0
#define GL_DEPTH_ATTACHMENT           0x8D00
#define GL_DEPTH_COMPONENT16          0x81A5
#define GL_FRAMEBUFFER_COMPLETE       0x8CD5
#define GL_FRAMEBUFFER_BINDING        0x8CA6
#define GL_ARRAY_BUFFER               0x8892
#define GL_ELEMENT_ARRAY_BUFFER       0x8893
#define GL_STATIC_DRAW                0x88E4
#define GL_STREAM_DRAW                0x88E0
#define GL_FRAGMENT_SHADER            0x8B30
#define GL_VERTEX_SHADER              0x8B31
#define GL_COMPILE_STATUS             0x8B81
#define GL_LINK_STATUS                0x8B82
#define GL_INFO_LOG_LENGTH            0x8B84

static bool LoadGLES2Functions()
{
#define LOAD(name) p##name = (PFN_##name)SDL_GL_GetProcAddress(#name); if (!p##name) return false
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glUseProgram);
    LOAD(glGetAttribLocation);
    LOAD(glGetUniformLocation);
    LOAD(glUniform1i);
    LOAD(glUniform1f);
    LOAD(glUniform4f);
    LOAD(glUniformMatrix4fv);
    LOAD(glEnableVertexAttribArray);
    LOAD(glDisableVertexAttribArray);
    LOAD(glVertexAttribPointer);
    LOAD(glDeleteShader);
    LOAD(glDeleteProgram);
    LOAD(glGenBuffers);
    LOAD(glDeleteBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferData);
    LOAD(glBufferSubData);
#undef LOAD
    // FBO — try core names first, then ARB suffix
#define LOADFBO(name) p##name = (PFN_##name)SDL_GL_GetProcAddress(#name); \
    if (!p##name) p##name = (PFN_##name)SDL_GL_GetProcAddress(#name "ARB"); \
    if (!p##name) p##name = (PFN_##name)SDL_GL_GetProcAddress(#name "EXT")
    LOADFBO(glGenFramebuffers);
    LOADFBO(glDeleteFramebuffers);
    LOADFBO(glBindFramebuffer);
    LOADFBO(glFramebufferTexture2D);
    LOADFBO(glGenRenderbuffers);
    LOADFBO(glDeleteRenderbuffers);
    LOADFBO(glBindRenderbuffer);
    LOADFBO(glRenderbufferStorage);
    LOADFBO(glFramebufferRenderbuffer);
    LOADFBO(glCheckFramebufferStatus);
#undef LOADFBO
    return pglGenFramebuffers && pglBindFramebuffer && pglFramebufferTexture2D;
}

// ---------------------------------------------------------------------------
// Shader compilation helpers
// ---------------------------------------------------------------------------
static GLuint CompileShader(GLenum type, const char *src)
{
    GLuint s = pglCreateShader(type);
    pglShaderSource(s, 1, &src, NULL);
    pglCompileShader(s);
    GLint ok = 0;
    pglGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char buf[1024];
        pglGetShaderInfoLog(s, sizeof(buf), NULL, buf);
        fprintf(stderr, "[RendererGLES] Shader compile error:\n%s\n", buf);
    }
    return s;
}

static GLuint BuildProgram(const char *vsSrc, const char *fsSrc)
{
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc);
    GLuint prog = pglCreateProgram();
    pglAttachShader(prog, vs);
    pglAttachShader(prog, fs);
    pglLinkProgram(prog);
    GLint ok = 0;
    pglGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char buf[1024];
        pglGetProgramInfoLog(prog, sizeof(buf), NULL, buf);
        fprintf(stderr, "[RendererGLES] Program link error:\n%s\n", buf);
    }
    pglDeleteShader(vs);
    pglDeleteShader(fs);
    return prog;
}

namespace th06
{

void RendererGLES::FrameStats::Reset()
{
    frames = 0;
    drawCalls = 0;
    immediate2DDraws = 0;
    immediate3DDraws = 0;
    batch2DFlushes = 0;
    batch3DFlushes = 0;
    batch2DQuads = 0;
    batch3DQuads = 0;
    batch3DRejectedTexMatrix = 0;
    vertexCount = 0;
    textureBinds = 0;
    bufferUploads = 0;
    bufferUploadBytes = 0;
    textureUploads = 0;
    textureUploadBytes = 0;
    fboBlits = 0;
    readbacks = 0;
    viewportChanges = 0;
    blendChanges = 0;
    colorOpChanges = 0;
    textureStageChanges = 0;
    fogChanges = 0;
    zwriteChanges = 0;
    depthFuncChanges = 0;
    pass2DEnters = 0;
    pass2DLeaves = 0;
    for (u32 &count : flush2DByReason) count = 0;
    for (u32 &count : flush3DByReason) count = 0;
    renderCpuMs = 0.0;
}

void RendererGLES::FrameStats::Accumulate(const FrameStats &other)
{
    frames += other.frames;
    drawCalls += other.drawCalls;
    immediate2DDraws += other.immediate2DDraws;
    immediate3DDraws += other.immediate3DDraws;
    batch2DFlushes += other.batch2DFlushes;
    batch3DFlushes += other.batch3DFlushes;
    batch2DQuads += other.batch2DQuads;
    batch3DQuads += other.batch3DQuads;
    batch3DRejectedTexMatrix += other.batch3DRejectedTexMatrix;
    vertexCount += other.vertexCount;
    textureBinds += other.textureBinds;
    bufferUploads += other.bufferUploads;
    bufferUploadBytes += other.bufferUploadBytes;
    textureUploads += other.textureUploads;
    textureUploadBytes += other.textureUploadBytes;
    fboBlits += other.fboBlits;
    readbacks += other.readbacks;
    viewportChanges += other.viewportChanges;
    blendChanges += other.blendChanges;
    colorOpChanges += other.colorOpChanges;
    textureStageChanges += other.textureStageChanges;
    fogChanges += other.fogChanges;
    zwriteChanges += other.zwriteChanges;
    depthFuncChanges += other.depthFuncChanges;
    pass2DEnters += other.pass2DEnters;
    pass2DLeaves += other.pass2DLeaves;
    for (int i = 0; i < Flush2D_Count; i++) flush2DByReason[i] += other.flush2DByReason[i];
    for (int i = 0; i < Flush3D_Count; i++) flush3DByReason[i] += other.flush3DByReason[i];
    renderCpuMs += other.renderCpuMs;
}

// ---------------------------------------------------------------------------
// Helper: upload RGBA surface to a GL texture (same as RendererGL version)
// ---------------------------------------------------------------------------
static void UploadRgbaSurface(GLuint tex, SDL_Surface *rgba)
{
    const i32 bpp = 4;
    const i32 tightPitch = rgba->w * bpp;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (rgba->pitch == tightPitch)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
        return;
    }
    std::vector<u8> packed((size_t)tightPitch * (size_t)rgba->h);
    const u8 *src = (const u8 *)rgba->pixels;
    for (i32 y = 0; y < rgba->h; ++y)
        memcpy(&packed[(size_t)y * tightPitch], src + (size_t)y * rgba->pitch, tightPitch);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, packed.data());
}

// =========================================================================
// Init / InitDevice / lifecycle
// =========================================================================

void RendererGLES::Init(SDL_Window *win, SDL_GLContext ctx, i32 w, i32 h)
{
    this->window = win;
    this->glContext = ctx;
    this->screenWidth = w;
    this->screenHeight = h;
    this->currentTexture = 0;
    this->currentBlendMode = 0xff;
    this->currentColorOp = 0xff;
    this->currentVertexShader = 0xff;
    this->currentZWriteDisable = 0xff;
    this->textureFactor = 0xffffffff;
    this->fogEnabled = 0;
    this->fogColor = 0xffa0a0a0;
    this->fogStart = 1000.0f;
    this->fogEnd = 5000.0f;
    this->textureEnabled = 1;
    this->viewportMinZ = 0.0f;
    this->viewportMaxZ = 1.0f;
    this->saved3D_depthTestEnabled = true;
    this->saved3D_depthMask = GL_TRUE;
    this->frameBeginCounter = 0;
    this->perfCounterToMs = 1000.0 / (double)SDL_GetPerformanceFrequency();
    this->statsLastLogTicks = SDL_GetTicks();

    D3DXMatrixIdentity(&this->viewMatrix);
    D3DXMatrixIdentity(&this->projectionMatrix);
    D3DXMatrixIdentity(&this->modelviewMatrix);
    D3DXMatrixIdentity(&this->worldMatrix);
    D3DXMatrixIdentity(&this->textureMatrix);

    i32 drawW, drawH;
    SDL_GL_GetDrawableSize(win, &drawW, &drawH);
    this->realScreenWidth = drawW;
    this->realScreenHeight = drawH;

    // Load GL2/GLES2 functions
    if (!LoadGLES2Functions())
    {
        fprintf(stderr, "[RendererGLES] Failed to load required GL functions\n");
        return;
    }

    // Debug: identify renderer in log
    const char *glVer = (const char *)glGetString(GL_VERSION);
    const char *glRen = (const char *)glGetString(GL_RENDERER);
    fprintf(stderr, "[RendererGLES] GL_VERSION:  %s\n", glVer ? glVer : "?");
    fprintf(stderr, "[RendererGLES] GL_RENDERER: %s\n", glRen ? glRen : "?");

    // Build shader program
    this->shaderProgram = BuildProgram(kGLES_VertexShader, kGLES_FragmentShader);
    pglUseProgram(this->shaderProgram);

    // Cache attribute/uniform locations
    this->loc_a_Position      = pglGetAttribLocation(shaderProgram, "a_Position");
    this->loc_a_Color         = pglGetAttribLocation(shaderProgram, "a_Color");
    this->loc_a_TexCoord      = pglGetAttribLocation(shaderProgram, "a_TexCoord");
    this->loc_a_FogFactor     = pglGetAttribLocation(shaderProgram, "a_FogFactor");
    this->loc_u_MVP           = pglGetUniformLocation(shaderProgram, "u_MVP");
    this->loc_u_TexMatrix     = pglGetUniformLocation(shaderProgram, "u_TexMatrix");
    this->loc_u_ModelView     = pglGetUniformLocation(shaderProgram, "u_ModelView");
    this->loc_u_Texture       = pglGetUniformLocation(shaderProgram, "u_Texture");
    this->loc_u_TextureEnabled = pglGetUniformLocation(shaderProgram, "u_TextureEnabled");
    this->loc_u_ColorOp       = pglGetUniformLocation(shaderProgram, "u_ColorOp");
    this->loc_u_AlphaRef      = pglGetUniformLocation(shaderProgram, "u_AlphaRef");
    this->loc_u_FogEnabled    = pglGetUniformLocation(shaderProgram, "u_FogEnabled");
    this->loc_u_FogStart      = pglGetUniformLocation(shaderProgram, "u_FogStart");
    this->loc_u_FogEnd        = pglGetUniformLocation(shaderProgram, "u_FogEnd");
    this->loc_u_FogColor      = pglGetUniformLocation(shaderProgram, "u_FogColor");
    this->loc_u_UseVertexFog  = pglGetUniformLocation(shaderProgram, "u_UseVertexFog");

    // Set texture unit 0
    pglUniform1i(this->loc_u_Texture, 0);
    pglUniform1f(this->loc_u_AlphaRef, 4.0f / 255.0f);
    pglUniform1i(this->loc_u_TextureEnabled, 1);
    pglUniform1i(this->loc_u_ColorOp, 0);
    pglUniform1i(this->loc_u_UseVertexFog, 0);

    // Create dynamic VBO (for 3D draws & fallback)
    pglGenBuffers(1, &this->vbo);
    this->attribsEnabled = false;
    this->fogAttribEnabled = false;

    // Create batch VBO + static quad IBO for 2D batching
    pglGenBuffers(1, &this->batchVBO);
    pglGenBuffers(1, &this->batch3DVBO);
    pglGenBuffers(1, &this->quadIBO);
    {
        u16 indices[BATCH_MAX_QUADS * 6];
        for (i32 i = 0; i < BATCH_MAX_QUADS; i++)
        {
            u16 base = (u16)(i * 4);
            indices[i * 6 + 0] = base + 0;
            indices[i * 6 + 1] = base + 1;
            indices[i * 6 + 2] = base + 2;
            indices[i * 6 + 3] = base + 2;
            indices[i * 6 + 4] = base + 1;
            indices[i * 6 + 5] = base + 3;
        }
        pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->quadIBO);
        pglBufferData(GL_ELEMENT_ARRAY_BUFFER,
                      (GLsizeiptr)(BATCH_MAX_QUADS * 6 * sizeof(u16)),
                      indices, GL_STATIC_DRAW);
        pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
    this->batchBuffer.resize(BATCH_MAX_QUADS * 4);
    this->batchQuadCount = 0;
    this->batch3DBuffer.resize(BATCH3D_MAX_QUADS * 4);
    this->batch3DQuadCount = 0;
    this->usingVertexFog = false;
    this->in2DPass = false;
    this->mvpDirty = true;
    this->fogDirty = true;
    this->stats.Reset();
    this->statsInterval.Reset();

    // Enable core GL state (no fixed-function)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
#ifdef __ANDROID__
    glDepthRangef(0.0f, 1.0f);
#else
    glDepthRange(0.0, 1.0);
#endif

    // Upload identity texture matrix
    D3DXMATRIX ident;
    D3DXMatrixIdentity(&ident);
    pglUniformMatrix4fv(this->loc_u_TexMatrix, 1, GL_FALSE, &ident.m[0][0]);
    pglUniformMatrix4fv(this->loc_u_ModelView, 1, GL_FALSE, &ident.m[0][0]);

    // Create FBO if screen differs from game size
    this->fbo = 0;
    this->fboColorTex = 0;
    this->fboDepthRb = 0;
    if (drawW != w || drawH != h)
    {
        glGenTextures(1, &this->fboColorTex);
        glBindTexture(GL_TEXTURE_2D, this->fboColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        pglGenRenderbuffers(1, &this->fboDepthRb);
        pglBindRenderbuffer(GL_RENDERBUFFER, this->fboDepthRb);
        pglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);

        pglGenFramebuffers(1, &this->fbo);
        pglBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
        pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->fboColorTex, 0);
        pglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, this->fboDepthRb);

        GLenum status = pglCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            pglBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteTextures(1, &this->fboColorTex);
            pglDeleteRenderbuffers(1, &this->fboDepthRb);
            pglDeleteFramebuffers(1, &this->fbo);
            this->fbo = 0;
            this->fboColorTex = 0;
            this->fboDepthRb = 0;
        }
        else
        {
            pglBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    SetViewport(0, 0, w, h);
    BeginFrame();
}

void RendererGLES::InitDevice(u32 opts)
{
    pglUseProgram(this->shaderProgram);

    if (((opts >> GCOS_TURN_OFF_DEPTH_TEST) & 1) == 0)
    {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    }
    else
    {
        glDisable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Alpha test threshold via shader uniform
    pglUniform1f(this->loc_u_AlphaRef, 4.0f / 255.0f);

    // Fog
    if (((opts >> GCOS_DONT_USE_FOG) & 1) == 0)
    {
        this->fogEnabled = 1;
    }
    else
    {
        this->fogEnabled = 0;
    }
    this->fogColor = 0xffa0a0a0;
    this->fogStart = 1000.0f;
    this->fogEnd = 5000.0f;
    pglUniform1i(this->loc_u_FogEnabled, this->fogEnabled);
    pglUniform1f(this->loc_u_FogStart, this->fogStart);
    pglUniform1f(this->loc_u_FogEnd, this->fogEnd);
    pglUniform4f(this->loc_u_FogColor, 0.627f, 0.627f, 0.627f, 1.0f);

    // Default texture mode: modulate
    pglUniform1i(this->loc_u_ColorOp, 0);
    pglUniform1i(this->loc_u_TextureEnabled, 1);
    this->textureEnabled = 1;
}

void RendererGLES::Release()
{
    if (this->fbo != 0)
    {
        pglBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &this->fboColorTex);
        pglDeleteRenderbuffers(1, &this->fboDepthRb);
        pglDeleteFramebuffers(1, &this->fbo);
        this->fbo = 0;
        this->fboColorTex = 0;
        this->fboDepthRb = 0;
    }
    if (this->vbo != 0)
    {
        pglDeleteBuffers(1, &this->vbo);
        this->vbo = 0;
    }
    if (this->batchVBO != 0)
    {
        pglDeleteBuffers(1, &this->batchVBO);
        this->batchVBO = 0;
    }
    if (this->batch3DVBO != 0)
    {
        pglDeleteBuffers(1, &this->batch3DVBO);
        this->batch3DVBO = 0;
    }
    if (this->quadIBO != 0)
    {
        pglDeleteBuffers(1, &this->quadIBO);
        this->quadIBO = 0;
    }
    if (this->shaderProgram != 0)
    {
        pglDeleteProgram(this->shaderProgram);
        this->shaderProgram = 0;
    }
    this->window = nullptr;
    this->glContext = nullptr;
}

void RendererGLES::ResizeTarget()
{
    // Clean up old FBO resources
    if (this->fbo != 0)
    {
        pglBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &this->fboColorTex);
        pglDeleteRenderbuffers(1, &this->fboDepthRb);
        pglDeleteFramebuffers(1, &this->fbo);
        this->fbo = 0;
        this->fboColorTex = 0;
        this->fboDepthRb = 0;
    }

    // Use pre-set realScreenWidth/realScreenHeight (set by caller before this)
    i32 rw = this->realScreenWidth;
    i32 rh = this->realScreenHeight;
    i32 w = this->screenWidth;
    i32 h = this->screenHeight;
    if (rw != w || rh != h)
    {
        glGenTextures(1, &this->fboColorTex);
        glBindTexture(GL_TEXTURE_2D, this->fboColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        pglGenRenderbuffers(1, &this->fboDepthRb);
        pglBindRenderbuffer(GL_RENDERBUFFER, this->fboDepthRb);
        pglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);

        pglGenFramebuffers(1, &this->fbo);
        pglBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
        pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->fboColorTex, 0);
        pglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, this->fboDepthRb);

        GLenum status = pglCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            pglBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteTextures(1, &this->fboColorTex);
            pglDeleteRenderbuffers(1, &this->fboDepthRb);
            pglDeleteFramebuffers(1, &this->fbo);
            this->fbo = 0;
            this->fboColorTex = 0;
            this->fboDepthRb = 0;
        }
        else
        {
            pglBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    SetViewport(0, 0, w, h);
    BeginFrame();
}

void RendererGLES::BeginScene() {}
void RendererGLES::EndScene() {}

void RendererGLES::BeginFrame()
{
    this->frameBeginCounter = SDL_GetPerformanceCounter();
    pglUseProgram(this->shaderProgram);
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_BeginFrame);
    if (this->fbo != 0)
    {
        pglBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
        glViewport(this->viewportX,
                   this->screenHeight - this->viewportY - this->viewportH,
                   this->viewportW, this->viewportH);
        glScissor(this->viewportX,
                  this->screenHeight - this->viewportY - this->viewportH,
                  this->viewportW, this->viewportH);
    }
}

// continued below — Clear, SetViewport, state setters

void RendererGLES::Clear(D3DCOLOR color, i32 clearColor, i32 clearDepth)
{
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_ViewportOrClear);
    GLbitfield mask = 0;
    if (clearColor)
    {
        f32 a = D3DCOLOR_A(color) / 255.0f;
        f32 r = D3DCOLOR_R(color) / 255.0f;
        f32 g = D3DCOLOR_G(color) / 255.0f;
        f32 b = D3DCOLOR_B(color) / 255.0f;
        glClearColor(r, g, b, a);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (clearDepth)
        mask |= GL_DEPTH_BUFFER_BIT;
    if (mask)
    {
        GLboolean prevDepthMask;
        GLboolean prevColorMask[4];
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClear(mask);
        glDepthMask(prevDepthMask);
        glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
    }
}

void RendererGLES::SetViewport(i32 x, i32 y, i32 w, i32 h, f32 minZ, f32 maxZ)
{
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_ViewportOrClear);
    if (this->in2DPass)
        Leave2DPass();

    this->viewportX = x;
    this->viewportY = y;
    this->viewportW = w;
    this->viewportH = h;
    this->viewportMinZ = minZ;
    this->viewportMaxZ = maxZ;
    this->stats.viewportChanges++;
    glViewport(x, this->screenHeight - y - h, w, h);
    glScissor(x, this->screenHeight - y - h, w, h);
#ifdef __ANDROID__
    glDepthRangef(minZ, maxZ);
#else
    glDepthRange(minZ, maxZ);
#endif
}

void RendererGLES::SetBlendMode(u8 mode)
{
    if (mode == this->currentBlendMode)
        return;
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_State);
    if (this->in2DPass) FlushBatch(Flush2D_State);
    this->stats.blendChanges++;
    this->currentBlendMode = mode;
    if (mode == BLEND_MODE_ADD)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void RendererGLES::SetColorOp(u8 colorOp)
{
    if (colorOp == this->currentColorOp)
        return;
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_State);
    if (this->in2DPass) FlushBatch(Flush2D_State);
    this->stats.colorOpChanges++;
    this->currentColorOp = colorOp;
    pglUniform1i(this->loc_u_ColorOp, colorOp);
}

void RendererGLES::SetTexture(u32 tex)
{
    if (tex == this->currentTexture)
        return;
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_State);
    if (this->in2DPass) FlushBatch(Flush2D_State);
    this->currentTexture = tex;
    if (tex != 0)
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, tex);
        if (!this->textureEnabled)
        {
            this->textureEnabled = 1;
            pglUniform1i(this->loc_u_TextureEnabled, 1);
        }
    }
    else
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, 0);
        if (this->textureEnabled)
        {
            this->textureEnabled = 0;
            pglUniform1i(this->loc_u_TextureEnabled, 0);
        }
    }
}

void RendererGLES::SetTextureFactor(D3DCOLOR factor)
{
    this->textureFactor = factor;
}

void RendererGLES::SetZWriteDisable(u8 disable)
{
    if (disable == this->currentZWriteDisable)
        return;
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_State);
    if (this->in2DPass) FlushBatch(Flush2D_State);
    this->stats.zwriteChanges++;
    this->currentZWriteDisable = disable;
    // Depth mask is disabled during 2D pass; restore the latest requested
    // 3D depth-write state when leaving the pass.
    if (this->in2DPass)
        this->saved3D_depthMask = disable ? GL_FALSE : GL_TRUE;
    if (!this->in2DPass)
        glDepthMask(disable ? GL_FALSE : GL_TRUE);
}

void RendererGLES::SetDepthFunc(i32 alwaysPass)
{
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_State);
    this->stats.depthFuncChanges++;
    glDepthFunc(alwaysPass ? GL_ALWAYS : GL_LEQUAL);
}

void RendererGLES::SetDestBlendInvSrcAlpha()
{
    SetBlendMode(BLEND_MODE_ALPHA);
}

void RendererGLES::SetDestBlendOne()
{
    SetBlendMode(BLEND_MODE_ADD);
}

void RendererGLES::SetTextureStageSelectDiffuse()
{
    if (this->textureEnabled)
    {
        if (this->batch3DQuadCount != 0)
            Flush3DBatch(Flush3D_State);
        if (this->in2DPass) FlushBatch(Flush2D_State);
        this->stats.textureStageChanges++;
        this->textureEnabled = 0;
        pglUniform1i(this->loc_u_TextureEnabled, 0);
    }
    this->currentTexture = 0;
}

void RendererGLES::SetTextureStageModulateTexture()
{
    if (this->textureEnabled)
        return;
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_State);
    if (this->in2DPass) FlushBatch(Flush2D_State);
    this->stats.textureStageChanges++;
    this->textureEnabled = 1;
    pglUniform1i(this->loc_u_TextureEnabled, 1);
}

void RendererGLES::SetFog(i32 enable, D3DCOLOR color, f32 start, f32 end)
{
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_State);
    this->stats.fogChanges++;
    this->fogEnabled = enable;
    this->fogColor = color;
    this->fogStart = start;
    this->fogEnd = end;
    this->fogDirty = true;
    if (this->in2DPass)
        return;
    pglUniform1i(this->loc_u_FogEnabled, enable);
    if (enable)
    {
        pglUniform1f(this->loc_u_FogStart, start);
        pglUniform1f(this->loc_u_FogEnd, end);
        pglUniform4f(this->loc_u_FogColor,
                     D3DCOLOR_R(color) / 255.0f,
                     D3DCOLOR_G(color) / 255.0f,
                     D3DCOLOR_B(color) / 255.0f,
                     D3DCOLOR_A(color) / 255.0f);
    }
}

// continued below — transforms

void RendererGLES::SetViewTransform(const D3DXMATRIX *matrix)
{
    this->viewMatrix = *matrix;
    D3DXMatrixMultiply(&this->modelviewMatrix, &this->worldMatrix, &this->viewMatrix);
    this->mvpDirty = true;
}

void RendererGLES::SetProjectionTransform(const D3DXMATRIX *matrix)
{
    this->projectionMatrix = *matrix;
    this->mvpDirty = true;
}

void RendererGLES::SetWorldTransform(const D3DXMATRIX *matrix)
{
    this->worldMatrix = *matrix;
    D3DXMatrixMultiply(&this->modelviewMatrix, matrix, &this->viewMatrix);
    this->mvpDirty = true;
}

void RendererGLES::SetTextureTransform(const D3DXMATRIX *matrix)
{
    if (memcmp(&this->textureMatrix, matrix, sizeof(*matrix)) == 0)
        return;
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_State);
    this->textureMatrix = *matrix;
    if (this->in2DPass)
        return;
    pglUniformMatrix4fv(this->loc_u_TexMatrix, 1, GL_FALSE, &matrix->m[0][0]);
}

// =========================================================================
// UploadMVP — compute MVP = projection * modelview, upload to shader
// =========================================================================
void RendererGLES::UploadMVP()
{
    if (!this->mvpDirty) return;
    D3DXMATRIX mvp;
    D3DXMatrixMultiply(&mvp, &this->modelviewMatrix, &this->projectionMatrix);
    pglUniformMatrix4fv(this->loc_u_MVP, 1, GL_FALSE, &mvp.m[0][0]);
    pglUniformMatrix4fv(this->loc_u_ModelView, 1, GL_FALSE, &this->modelviewMatrix.m[0][0]);
    this->mvpDirty = false;
}

void RendererGLES::UploadUniforms()
{
    if (!this->fogDirty) return;
    pglUniform1i(this->loc_u_FogEnabled, this->fogEnabled);
    pglUniform1f(this->loc_u_FogStart, this->fogStart);
    pglUniform1f(this->loc_u_FogEnd, this->fogEnd);
    pglUniform4f(this->loc_u_FogColor,
                 D3DCOLOR_R(this->fogColor) / 255.0f,
                 D3DCOLOR_G(this->fogColor) / 255.0f,
                 D3DCOLOR_B(this->fogColor) / 255.0f,
                 D3DCOLOR_A(this->fogColor) / 255.0f);
    this->fogDirty = false;
}

bool RendererGLES::CanBatch3DQuad() const
{
    D3DXMATRIX ident;
    D3DXMatrixIdentity(&ident);
    return memcmp(&this->textureMatrix, &ident, sizeof(ident)) == 0;
}

void RendererGLES::FinishFrameStats()
{
    const u64 frameEndCounter = SDL_GetPerformanceCounter();
    if (this->frameBeginCounter != 0)
        this->stats.renderCpuMs += (frameEndCounter - this->frameBeginCounter) * this->perfCounterToMs;
    this->stats.frames = 1;
    this->statsInterval.Accumulate(this->stats);

    const u32 nowTicks = SDL_GetTicks();
    const u32 elapsedMs = nowTicks - this->statsLastLogTicks;
    if (elapsedMs >= 1000 && this->statsInterval.frames != 0)
    {
        const double seconds = elapsedMs / 1000.0;
        const double fps = this->statsInterval.frames / seconds;
        const double avgCpuMs = this->statsInterval.renderCpuMs / this->statsInterval.frames;
        const double avgDrawCalls = (double)this->statsInterval.drawCalls / this->statsInterval.frames;
        const double avg2DQuadsPerFlush = this->statsInterval.batch2DFlushes != 0
            ? (double)this->statsInterval.batch2DQuads / this->statsInterval.batch2DFlushes
            : 0.0;
        const double avg3DQuadsPerFlush = this->statsInterval.batch3DFlushes != 0
            ? (double)this->statsInterval.batch3DQuads / this->statsInterval.batch3DFlushes
            : 0.0;
        const double avgImmediate2D = (double)this->statsInterval.immediate2DDraws / this->statsInterval.frames;
        const double avgImmediate3D = (double)this->statsInterval.immediate3DDraws / this->statsInterval.frames;
        const double avgTexBinds = (double)this->statsInterval.textureBinds / this->statsInterval.frames;
        const double avgUploadKB = (double)this->statsInterval.bufferUploadBytes / this->statsInterval.frames / 1024.0;
        const u32 stateFlush2D = this->statsInterval.flush2DByReason[Flush2D_State];
        const u32 stateFlush3D = this->statsInterval.flush3DByReason[Flush3D_State];

        SDL_Log("[render/stats] fps=%.1f cpu=%.2fms draw=%.1f 2d_flush=%u 2d_qpf=%.1f 3d_flush=%u 3d_qpf=%.1f imm2d=%u imm3d=%u texbind=%u vbo=%u(%.1fKB) texup=%u(%.1fKB) blit=%u readback=%u",
                fps, avgCpuMs, avgDrawCalls,
                this->statsInterval.batch2DFlushes, avg2DQuadsPerFlush,
                this->statsInterval.batch3DFlushes, avg3DQuadsPerFlush,
                this->statsInterval.immediate2DDraws, this->statsInterval.immediate3DDraws,
                this->statsInterval.textureBinds,
                this->statsInterval.bufferUploads, this->statsInterval.bufferUploadBytes / 1024.0,
                this->statsInterval.textureUploads, this->statsInterval.textureUploadBytes / 1024.0,
                this->statsInterval.fboBlits, this->statsInterval.readbacks);

        SDL_Log("[render/stats/detail] 2d[state=%u cap=%u leave=%u surf=%u imm=%u] 3d[state=%u cap=%u begin2d=%u begin=%u vpclr=%u fallback=%u blit=%u end=%u] state[viewport=%u blend=%u color=%u texStage=%u fog=%u zwrite=%u depth=%u] batch3d[batched=%u rejected_texm=%u] pass2d[%u/%u]",
                this->statsInterval.flush2DByReason[Flush2D_State],
                this->statsInterval.flush2DByReason[Flush2D_Capacity],
                this->statsInterval.flush2DByReason[Flush2D_LeavePass],
                this->statsInterval.flush2DByReason[Flush2D_Surface],
                this->statsInterval.flush2DByReason[Flush2D_ImmediateFallback],
                this->statsInterval.flush3DByReason[Flush3D_State],
                this->statsInterval.flush3DByReason[Flush3D_Capacity],
                this->statsInterval.flush3DByReason[Flush3D_Begin2D],
                this->statsInterval.flush3DByReason[Flush3D_BeginFrame],
                this->statsInterval.flush3DByReason[Flush3D_ViewportOrClear],
                this->statsInterval.flush3DByReason[Flush3D_FallbackImmediate],
                this->statsInterval.flush3DByReason[Flush3D_BlitOrReadback],
                this->statsInterval.flush3DByReason[Flush3D_EndFrame],
                this->statsInterval.viewportChanges,
                this->statsInterval.blendChanges,
                this->statsInterval.colorOpChanges,
                this->statsInterval.textureStageChanges,
                this->statsInterval.fogChanges,
                this->statsInterval.zwriteChanges,
                this->statsInterval.depthFuncChanges,
                this->statsInterval.batch3DQuads,
                this->statsInterval.batch3DRejectedTexMatrix,
                this->statsInterval.pass2DEnters,
                this->statsInterval.pass2DLeaves);

        char hint[256] = {};
        size_t hintLen = 0;
        auto appendHint = [&](const char *text) {
            if (hintLen != 0 && hintLen < sizeof(hint) - 1)
                hint[hintLen++] = ' ';
            const size_t remaining = sizeof(hint) - hintLen;
            if (remaining <= 1)
                return;
            const int wrote = std::snprintf(hint + hintLen, remaining, "%s", text);
            if (wrote > 0)
            {
                const size_t advance = (size_t)wrote < remaining ? (size_t)wrote : remaining - 1;
                hintLen += advance;
            }
        };

        if (avgImmediate3D > 32.0 || this->statsInterval.flush3DByReason[Flush3D_FallbackImmediate] > this->statsInterval.frames * 4)
            appendHint("3d_immediate");
        if (this->statsInterval.batch3DRejectedTexMatrix != 0)
            appendHint("3d_texmatrix");
        if (avgImmediate2D > 16.0)
            appendHint("2d_immediate");
        if (stateFlush2D + stateFlush3D > this->statsInterval.frames * 12)
            appendHint("state_churn");
        if (avgTexBinds > 64.0)
            appendHint("texture_churn");
        if (avgUploadKB > 512.0 || this->statsInterval.textureUploadBytes > (u64)this->statsInterval.frames * 128 * 1024)
            appendHint("upload_bandwidth");
        if (this->statsInterval.readbacks != 0)
            appendHint("readback");
        if (this->statsInterval.fboBlits > this->statsInterval.frames)
            appendHint("extra_blit");
        if (hintLen == 0)
            appendHint("no_obvious_hotspot");

        SDL_Log("[render/stats/hint] hot=%s avg[imm2d=%.1f imm3d=%.1f texbind=%.1f upload=%.1fKB] state_flush[2d=%u 3d=%u]",
                hint, avgImmediate2D, avgImmediate3D, avgTexBinds, avgUploadKB,
                stateFlush2D, stateFlush3D);

        this->statsInterval.Reset();
        this->statsLastLogTicks = nowTicks;
    }

    this->stats.Reset();
    this->frameBeginCounter = 0;
}

// =========================================================================
// 2D Batch System — pass-level state + append-only quad batching
// =========================================================================
void RendererGLES::Enter2DPass()
{
    if (this->in2DPass) return;
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_Begin2D);
    this->stats.pass2DEnters++;

    // Save 3D state once
    this->saved3D_depthTestEnabled = glIsEnabled(GL_DEPTH_TEST) != 0;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &this->saved3D_depthMask);
    glGetIntegerv(GL_SCISSOR_BOX, this->saved3D_scissor);

    // Set 2D ortho state
    glViewport(0, 0, this->screenWidth, this->screenHeight);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    D3DXMATRIX ortho;
    memset(&ortho, 0, sizeof(ortho));
    ortho._11 = 2.0f / this->screenWidth;
    ortho._22 = -2.0f / this->screenHeight;
    ortho._33 = -1.0f;
    ortho._44 = 1.0f;
    ortho._41 = -1.0f;
    ortho._42 = 1.0f;
    pglUniformMatrix4fv(this->loc_u_MVP, 1, GL_FALSE, &ortho.m[0][0]);

    D3DXMATRIX ident;
    D3DXMatrixIdentity(&ident);
    pglUniformMatrix4fv(this->loc_u_ModelView, 1, GL_FALSE, &ident.m[0][0]);
    pglUniformMatrix4fv(this->loc_u_TexMatrix, 1, GL_FALSE, &ident.m[0][0]);
    pglUniform1i(this->loc_u_FogEnabled, 0);

    this->in2DPass = true;
}

void RendererGLES::Leave2DPass()
{
    if (!this->in2DPass) return;
    this->stats.pass2DLeaves++;
    FlushBatch(Flush2D_LeavePass);

    // Restore 3D state
    glViewport(this->viewportX,
               this->screenHeight - this->viewportY - this->viewportH,
               this->viewportW, this->viewportH);
    glScissor(this->saved3D_scissor[0], this->saved3D_scissor[1],
              this->saved3D_scissor[2], this->saved3D_scissor[3]);
    if (this->saved3D_depthTestEnabled) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(this->saved3D_depthMask);
    pglUniform1i(this->loc_u_FogEnabled, this->fogEnabled);
    pglUniformMatrix4fv(this->loc_u_TexMatrix, 1, GL_FALSE,
                        &this->textureMatrix.m[0][0]);
#ifdef __ANDROID__
    glDepthRangef(this->viewportMinZ, this->viewportMaxZ);
#else
    glDepthRange(this->viewportMinZ, this->viewportMaxZ);
#endif
    this->in2DPass = false;
    this->mvpDirty = true;
    this->fogDirty = true;
}

void RendererGLES::FlushBatch(Flush2DReason reason)
{
    if (this->batchQuadCount == 0) return;

    const i32 vertCount = this->batchQuadCount * 4;
    const i32 stride = (i32)sizeof(BatchVertex); // 24 bytes
    const GLsizeiptr byteCount = (GLsizeiptr)(vertCount * stride);

    pglBindBuffer(GL_ARRAY_BUFFER, this->batchVBO);
    pglBufferData(GL_ARRAY_BUFFER, byteCount, nullptr, GL_STREAM_DRAW);
    pglBufferSubData(GL_ARRAY_BUFFER, 0, byteCount, this->batchBuffer.data());
    this->stats.bufferUploads++;
    this->stats.bufferUploadBytes += (u64)byteCount;

    if (this->usingVertexFog)
    {
        pglUniform1i(this->loc_u_UseVertexFog, 0);
        this->usingVertexFog = false;
        this->mvpDirty = true;
    }

    if (!this->attribsEnabled)
    {
        pglEnableVertexAttribArray(this->loc_a_Position);
        pglEnableVertexAttribArray(this->loc_a_Color);
        pglEnableVertexAttribArray(this->loc_a_TexCoord);
        this->attribsEnabled = true;
    }
    if (this->fogAttribEnabled)
    {
        pglDisableVertexAttribArray(this->loc_a_FogFactor);
        this->fogAttribEnabled = false;
    }

    // 2D sprites use GL_NEAREST filtering, matching the desktop renderer's
    // ApplySamplerFor2D().  Without this, GL_LINEAR (the default for game
    // textures) creates sub-pixel blending at sprite-atlas edges, producing
    // semi-transparent fringe pixels.  Combined with no per-frame FBO clear,
    // the previous frame's background "leaks through" those fringes via
    // alpha blending.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    pglVertexAttribPointer(this->loc_a_Position, 3, GL_FLOAT,
                           GL_FALSE, stride, (void *)0);
    pglVertexAttribPointer(this->loc_a_Color, 4, GL_UNSIGNED_BYTE,
                           GL_TRUE, stride, (void *)12);
    pglVertexAttribPointer(this->loc_a_TexCoord, 2, GL_FLOAT,
                           GL_FALSE, stride, (void *)16);

    pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->quadIBO);
    glDrawElements(GL_TRIANGLES, this->batchQuadCount * 6,
                   GL_UNSIGNED_SHORT, 0);
    pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // Restore GL_LINEAR for subsequent 3D draws / other texture usage.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    this->stats.drawCalls++;
    this->stats.batch2DFlushes++;
    this->stats.batch2DQuads += this->batchQuadCount;
    this->stats.vertexCount += vertCount;
    this->stats.flush2DByReason[reason]++;

    this->batchQuadCount = 0;
}

void RendererGLES::Flush3DBatch(Flush3DReason reason)
{
    if (this->batch3DQuadCount == 0) return;

    const i32 vertCount = this->batch3DQuadCount * 4;
    const i32 stride = (i32)sizeof(Batch3DVertex);
    const GLsizeiptr byteCount = (GLsizeiptr)(vertCount * stride);

    pglBindBuffer(GL_ARRAY_BUFFER, this->batch3DVBO);
    pglBufferData(GL_ARRAY_BUFFER, byteCount, nullptr, GL_STREAM_DRAW);
    pglBufferSubData(GL_ARRAY_BUFFER, 0, byteCount, this->batch3DBuffer.data());
    this->stats.bufferUploads++;
    this->stats.bufferUploadBytes += (u64)byteCount;

    if (!this->usingVertexFog)
    {
        pglUniform1i(this->loc_u_UseVertexFog, 1);
        this->usingVertexFog = true;
    }

    if (!this->attribsEnabled)
    {
        pglEnableVertexAttribArray(this->loc_a_Position);
        pglEnableVertexAttribArray(this->loc_a_Color);
        pglEnableVertexAttribArray(this->loc_a_TexCoord);
        this->attribsEnabled = true;
    }
    if (!this->fogAttribEnabled)
    {
        pglEnableVertexAttribArray(this->loc_a_FogFactor);
        this->fogAttribEnabled = true;
    }

    D3DXMATRIX ident;
    D3DXMatrixIdentity(&ident);
    pglUniformMatrix4fv(this->loc_u_MVP, 1, GL_FALSE, &ident.m[0][0]);
    pglUniformMatrix4fv(this->loc_u_ModelView, 1, GL_FALSE, &ident.m[0][0]);

    pglVertexAttribPointer(this->loc_a_Position, 4, GL_FLOAT,
                           GL_FALSE, stride, (void *)0);
    pglVertexAttribPointer(this->loc_a_Color, 4, GL_UNSIGNED_BYTE,
                           GL_TRUE, stride, (void *)16);
    pglVertexAttribPointer(this->loc_a_TexCoord, 2, GL_FLOAT,
                           GL_FALSE, stride, (void *)20);
    pglVertexAttribPointer(this->loc_a_FogFactor, 1, GL_FLOAT,
                           GL_FALSE, stride, (void *)28);

    pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->quadIBO);
    glDrawElements(GL_TRIANGLES, this->batch3DQuadCount * 6,
                   GL_UNSIGNED_SHORT, 0);
    pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    this->stats.drawCalls++;
    this->stats.batch3DFlushes++;
    this->stats.batch3DQuads += this->batch3DQuadCount;
    this->stats.vertexCount += vertCount;
    this->stats.flush3DByReason[reason]++;

    this->batch3DQuadCount = 0;
    this->mvpDirty = true;
}

// =========================================================================
// DrawArrays — generic vertex submission via VBO
// Interleaved layout: pos(3f) + color(4f) + texcoord(2f) = 9 floats/vert
// =========================================================================
void RendererGLES::DrawArrays(GLenum mode, const f32 *positions, const f32 *colors,
                               const f32 *texcoords, i32 vertCount)
{
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_FallbackImmediate);

    const i32 stride = 9 * sizeof(f32); // pos(3) + col(4) + tc(2)
    const size_t needed = (size_t)vertCount * 9;

    // If positions is null, caller already filled drawScratch directly
    if (positions)
    {
        if (this->drawScratch.size() < needed)
            this->drawScratch.resize(needed);
        f32 *buf = this->drawScratch.data();

        if (texcoords)
        {
            for (i32 i = 0; i < vertCount; i++)
            {
                f32 *dst = buf + i * 9;
                dst[0] = positions[i * 3 + 0];
                dst[1] = positions[i * 3 + 1];
                dst[2] = positions[i * 3 + 2];
                dst[3] = colors[i * 4 + 0];
                dst[4] = colors[i * 4 + 1];
                dst[5] = colors[i * 4 + 2];
                dst[6] = colors[i * 4 + 3];
                dst[7] = texcoords[i * 2 + 0];
                dst[8] = texcoords[i * 2 + 1];
            }
        }
        else
        {
            for (i32 i = 0; i < vertCount; i++)
            {
                f32 *dst = buf + i * 9;
                dst[0] = positions[i * 3 + 0];
                dst[1] = positions[i * 3 + 1];
                dst[2] = positions[i * 3 + 2];
                dst[3] = colors[i * 4 + 0];
                dst[4] = colors[i * 4 + 1];
                dst[5] = colors[i * 4 + 2];
                dst[6] = colors[i * 4 + 3];
                dst[7] = 0.0f;
                dst[8] = 0.0f;
            }
        }
    }

    pglBindBuffer(GL_ARRAY_BUFFER, this->vbo);
    const GLsizeiptr byteCount = (GLsizeiptr)(needed * sizeof(f32));
    pglBufferData(GL_ARRAY_BUFFER, byteCount, nullptr, GL_STREAM_DRAW);
    pglBufferSubData(GL_ARRAY_BUFFER, 0, byteCount, this->drawScratch.data());
    this->stats.bufferUploads++;
    this->stats.bufferUploadBytes += (u64)byteCount;

    if (this->usingVertexFog)
    {
        pglUniform1i(this->loc_u_UseVertexFog, 0);
        this->usingVertexFog = false;
        this->mvpDirty = true;
    }

    if (!this->attribsEnabled)
    {
        pglEnableVertexAttribArray(this->loc_a_Position);
        pglEnableVertexAttribArray(this->loc_a_Color);
        pglEnableVertexAttribArray(this->loc_a_TexCoord);
        this->attribsEnabled = true;
    }
    if (this->fogAttribEnabled)
    {
        pglDisableVertexAttribArray(this->loc_a_FogFactor);
        this->fogAttribEnabled = false;
    }

    pglVertexAttribPointer(this->loc_a_Position, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    pglVertexAttribPointer(this->loc_a_Color, 4, GL_FLOAT, GL_FALSE, stride, (void *)(3 * sizeof(f32)));
    pglVertexAttribPointer(this->loc_a_TexCoord, 2, GL_FLOAT, GL_FALSE, stride, (void *)(7 * sizeof(f32)));

    glDrawArrays(mode, 0, vertCount);
    this->stats.drawCalls++;
    this->stats.vertexCount += vertCount;
}

// continued below — draw methods

// Helper: unpack D3DCOLOR ARGB to float RGBA
static inline void ColorToFloat(D3DCOLOR c, f32 *out)
{
    out[0] = D3DCOLOR_R(c) / 255.0f;
    out[1] = D3DCOLOR_G(c) / 255.0f;
    out[2] = D3DCOLOR_B(c) / 255.0f;
    out[3] = D3DCOLOR_A(c) / 255.0f;
}

// Helper: fill one BatchVertex
static inline void FillBatchVert(RendererGLES::BatchVertex &v,
    f32 x, f32 y, f32 z, D3DCOLOR col, f32 u, f32 vv)
{
    v.x = x; v.y = y; v.z = z;
    v.r = D3DCOLOR_R(col); v.g = D3DCOLOR_G(col);
    v.b = D3DCOLOR_B(col); v.a = D3DCOLOR_A(col);
    v.u = u; v.v = vv;
}

void RendererGLES::DrawTriangleStrip(const VertexDiffuseXyzrwh *verts, i32 count)
{
    // Non-textured 2D quad — disable texture, batch
    u8 prevTexEnabled = this->textureEnabled;
    if (prevTexEnabled)
    {
        if (this->in2DPass) FlushBatch(Flush2D_State);
        this->textureEnabled = 0;
        pglUniform1i(loc_u_TextureEnabled, 0);
    }
    if (!this->in2DPass) Enter2DPass();

    if (count == 4)
    {
        const size_t off = (size_t)this->batchQuadCount * 4;
        BatchVertex *bv = this->batchBuffer.data() + off;
        FillBatchVert(bv[0], verts[0].position.x, verts[0].position.y,
                      verts[0].position.z, verts[0].diffuse, 0, 0);
        FillBatchVert(bv[1], verts[1].position.x, verts[1].position.y,
                      verts[1].position.z, verts[1].diffuse, 0, 0);
        FillBatchVert(bv[2], verts[2].position.x, verts[2].position.y,
                      verts[2].position.z, verts[2].diffuse, 0, 0);
        FillBatchVert(bv[3], verts[3].position.x, verts[3].position.y,
                      verts[3].position.z, verts[3].diffuse, 0, 0);
        this->batchQuadCount++;
        if (this->batchQuadCount >= BATCH_MAX_QUADS) FlushBatch(Flush2D_Capacity);
    }
    else
    {
        FlushBatch(Flush2D_ImmediateFallback);
        this->stats.immediate2DDraws++;
        const size_t needed = (size_t)count * 9;
        if (this->drawScratch.size() < needed)
            this->drawScratch.resize(needed);
        f32 *buf = this->drawScratch.data();
        for (i32 i = 0; i < count; i++)
        {
            f32 *dst = buf + i * 9;
            dst[0] = verts[i].position.x;
            dst[1] = verts[i].position.y;
            dst[2] = verts[i].position.z;
            ColorToFloat(verts[i].diffuse, dst + 3);
            dst[7] = 0.0f; dst[8] = 0.0f;
        }
        DrawArrays(GL_TRIANGLE_STRIP, nullptr, nullptr, nullptr, count);
    }

    if (prevTexEnabled)
    {
        if (this->in2DPass) FlushBatch(Flush2D_State);
        this->textureEnabled = 1;
        pglUniform1i(loc_u_TextureEnabled, 1);
    }
}

void RendererGLES::DrawTriangleStripTex(const VertexTex1Xyzrwh *verts, i32 count)
{
    if (!this->in2DPass) Enter2DPass();

    if (count == 4)
    {
        D3DCOLOR tf = this->textureFactor;
        const size_t off = (size_t)this->batchQuadCount * 4;
        BatchVertex *bv = this->batchBuffer.data() + off;
        FillBatchVert(bv[0], verts[0].position.x, verts[0].position.y,
                      verts[0].position.z, tf,
                      verts[0].textureUV.x, verts[0].textureUV.y);
        FillBatchVert(bv[1], verts[1].position.x, verts[1].position.y,
                      verts[1].position.z, tf,
                      verts[1].textureUV.x, verts[1].textureUV.y);
        FillBatchVert(bv[2], verts[2].position.x, verts[2].position.y,
                      verts[2].position.z, tf,
                      verts[2].textureUV.x, verts[2].textureUV.y);
        FillBatchVert(bv[3], verts[3].position.x, verts[3].position.y,
                      verts[3].position.z, tf,
                      verts[3].textureUV.x, verts[3].textureUV.y);
        this->batchQuadCount++;
        if (this->batchQuadCount >= BATCH_MAX_QUADS) FlushBatch(Flush2D_Capacity);
    }
    else
    {
        FlushBatch(Flush2D_ImmediateFallback);
        this->stats.immediate2DDraws++;
        f32 tfCol[4];
        ColorToFloat(this->textureFactor, tfCol);
        const size_t needed = (size_t)count * 9;
        if (this->drawScratch.size() < needed)
            this->drawScratch.resize(needed);
        f32 *buf = this->drawScratch.data();
        for (i32 i = 0; i < count; i++)
        {
            f32 *dst = buf + i * 9;
            dst[0] = verts[i].position.x;
            dst[1] = verts[i].position.y;
            dst[2] = verts[i].position.z;
            memcpy(dst + 3, tfCol, sizeof(tfCol));
            dst[7] = verts[i].textureUV.x;
            dst[8] = verts[i].textureUV.y;
        }
        DrawArrays(GL_TRIANGLE_STRIP, nullptr, nullptr, nullptr, count);
    }
}

void RendererGLES::DrawTriangleStripTextured(const VertexTex1DiffuseXyzrwh *verts, i32 count)
{
    if (!this->in2DPass) Enter2DPass();

    if (count == 4)
    {
        const size_t off = (size_t)this->batchQuadCount * 4;
        BatchVertex *bv = this->batchBuffer.data() + off;
        FillBatchVert(bv[0], verts[0].position.x, verts[0].position.y,
                      verts[0].position.z, verts[0].diffuse,
                      verts[0].textureUV.x, verts[0].textureUV.y);
        FillBatchVert(bv[1], verts[1].position.x, verts[1].position.y,
                      verts[1].position.z, verts[1].diffuse,
                      verts[1].textureUV.x, verts[1].textureUV.y);
        FillBatchVert(bv[2], verts[2].position.x, verts[2].position.y,
                      verts[2].position.z, verts[2].diffuse,
                      verts[2].textureUV.x, verts[2].textureUV.y);
        FillBatchVert(bv[3], verts[3].position.x, verts[3].position.y,
                      verts[3].position.z, verts[3].diffuse,
                      verts[3].textureUV.x, verts[3].textureUV.y);
        this->batchQuadCount++;
        if (this->batchQuadCount >= BATCH_MAX_QUADS) FlushBatch(Flush2D_Capacity);
    }
    else
    {
        FlushBatch(Flush2D_ImmediateFallback);
        this->stats.immediate2DDraws++;
        const size_t needed = (size_t)count * 9;
        if (this->drawScratch.size() < needed)
            this->drawScratch.resize(needed);
        f32 *buf = this->drawScratch.data();
        for (i32 i = 0; i < count; i++)
        {
            f32 *dst = buf + i * 9;
            dst[0] = verts[i].position.x;
            dst[1] = verts[i].position.y;
            dst[2] = verts[i].position.z;
            ColorToFloat(verts[i].diffuse, dst + 3);
            dst[7] = verts[i].textureUV.x;
            dst[8] = verts[i].textureUV.y;
        }
        DrawArrays(GL_TRIANGLE_STRIP, nullptr, nullptr, nullptr, count);
    }
}

void RendererGLES::DrawTriangleFanTextured(const VertexTex1DiffuseXyzrwh *verts, i32 count)
{
    if (!this->in2DPass) Enter2DPass();

    if (count == 4)
    {
        // Fan order v0,v1,v2,v3 → reorder to strip v1,v0,v2,v3
        // so the strip IBO (0,1,2, 2,1,3) produces equivalent triangles
        const size_t off = (size_t)this->batchQuadCount * 4;
        BatchVertex *bv = this->batchBuffer.data() + off;
        FillBatchVert(bv[0], verts[1].position.x, verts[1].position.y,
                      verts[1].position.z, verts[1].diffuse,
                      verts[1].textureUV.x, verts[1].textureUV.y);
        FillBatchVert(bv[1], verts[0].position.x, verts[0].position.y,
                      verts[0].position.z, verts[0].diffuse,
                      verts[0].textureUV.x, verts[0].textureUV.y);
        FillBatchVert(bv[2], verts[2].position.x, verts[2].position.y,
                      verts[2].position.z, verts[2].diffuse,
                      verts[2].textureUV.x, verts[2].textureUV.y);
        FillBatchVert(bv[3], verts[3].position.x, verts[3].position.y,
                      verts[3].position.z, verts[3].diffuse,
                      verts[3].textureUV.x, verts[3].textureUV.y);
        this->batchQuadCount++;
        if (this->batchQuadCount >= BATCH_MAX_QUADS) FlushBatch(Flush2D_Capacity);
    }
    else
    {
        FlushBatch(Flush2D_ImmediateFallback);
        this->stats.immediate2DDraws++;
        const size_t needed = (size_t)count * 9;
        if (this->drawScratch.size() < needed)
            this->drawScratch.resize(needed);
        f32 *buf = this->drawScratch.data();
        for (i32 i = 0; i < count; i++)
        {
            f32 *dst = buf + i * 9;
            dst[0] = verts[i].position.x;
            dst[1] = verts[i].position.y;
            dst[2] = verts[i].position.z;
            ColorToFloat(verts[i].diffuse, dst + 3);
            dst[7] = verts[i].textureUV.x;
            dst[8] = verts[i].textureUV.y;
        }
        DrawArrays(GL_TRIANGLE_FAN, nullptr, nullptr, nullptr, count);
    }
}

// continued below — 3D draw methods

void RendererGLES::DrawTriangleStripTextured3D(const VertexTex1DiffuseXyz *verts, i32 count)
{
    if (this->in2DPass) Leave2DPass();
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_FallbackImmediate);
    this->stats.immediate3DDraws++;
    UploadMVP();
    UploadUniforms();

    const size_t needed = (size_t)count * 9;
    if (this->drawScratch.size() < needed)
        this->drawScratch.resize(needed);
    f32 *buf = this->drawScratch.data();
    for (i32 i = 0; i < count; i++)
    {
        f32 *dst = buf + i * 9;
        dst[0] = verts[i].position.x;
        dst[1] = verts[i].position.y;
        dst[2] = verts[i].position.z;
        ColorToFloat(verts[i].diffuse, dst + 3);
        dst[7] = verts[i].textureUV.x;
        dst[8] = verts[i].textureUV.y;
    }
    DrawArrays(GL_TRIANGLE_STRIP, nullptr, nullptr, nullptr, count);
}

void RendererGLES::DrawTriangleFanTextured3D(const VertexTex1DiffuseXyz *verts, i32 count)
{
    if (this->in2DPass) Leave2DPass();
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_FallbackImmediate);
    this->stats.immediate3DDraws++;
    UploadMVP();
    UploadUniforms();

    const size_t needed = (size_t)count * 9;
    if (this->drawScratch.size() < needed)
        this->drawScratch.resize(needed);
    f32 *buf = this->drawScratch.data();
    for (i32 i = 0; i < count; i++)
    {
        f32 *dst = buf + i * 9;
        dst[0] = verts[i].position.x;
        dst[1] = verts[i].position.y;
        dst[2] = verts[i].position.z;
        ColorToFloat(verts[i].diffuse, dst + 3);
        dst[7] = verts[i].textureUV.x;
        dst[8] = verts[i].textureUV.y;
    }
    DrawArrays(GL_TRIANGLE_FAN, nullptr, nullptr, nullptr, count);
}

void RendererGLES::DrawVertexBuffer3D(const RenderVertexInfo *verts, i32 count)
{
    const bool canBatchQuad = count == 4 && CanBatch3DQuad();
    if (this->in2DPass) Leave2DPass();
    if (!canBatchQuad && this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_FallbackImmediate);
    if (count == 4 && !canBatchQuad)
        this->stats.batch3DRejectedTexMatrix++;
    if (canBatchQuad)
    {
        if (this->batch3DQuadCount >= BATCH3D_MAX_QUADS)
            Flush3DBatch(Flush3D_Capacity);

        D3DXMATRIX mvp;
        D3DXMatrixMultiply(&mvp, &this->modelviewMatrix, &this->projectionMatrix);

        const D3DCOLOR color = this->textureFactor;
        const size_t off = (size_t)this->batch3DQuadCount * 4;
        Batch3DVertex *bv = this->batch3DBuffer.data() + off;

        for (i32 i = 0; i < 4; i++)
        {
            const D3DXVECTOR3 &pos = verts[i].position;
            Batch3DVertex &dst = bv[i];

            dst.x = pos.x * mvp._11 + pos.y * mvp._21 + pos.z * mvp._31 + mvp._41;
            dst.y = pos.x * mvp._12 + pos.y * mvp._22 + pos.z * mvp._32 + mvp._42;
            dst.z = pos.x * mvp._13 + pos.y * mvp._23 + pos.z * mvp._33 + mvp._43;
            dst.w = pos.x * mvp._14 + pos.y * mvp._24 + pos.z * mvp._34 + mvp._44;
            dst.r = D3DCOLOR_R(color);
            dst.g = D3DCOLOR_G(color);
            dst.b = D3DCOLOR_B(color);
            dst.a = D3DCOLOR_A(color);
            dst.u = verts[i].textureUV.x;
            dst.v = verts[i].textureUV.y;

            if (this->fogEnabled)
            {
                const f32 eyeZ = -(pos.x * this->modelviewMatrix._13 +
                                   pos.y * this->modelviewMatrix._23 +
                                   pos.z * this->modelviewMatrix._33 +
                                   this->modelviewMatrix._43);
                const f32 denom = this->fogEnd - this->fogStart;
                f32 fogFactor = denom != 0.0f ? (this->fogEnd - eyeZ) / denom : 1.0f;
                if (fogFactor < 0.0f) fogFactor = 0.0f;
                if (fogFactor > 1.0f) fogFactor = 1.0f;
                dst.fogFactor = fogFactor;
            }
            else
            {
                dst.fogFactor = 1.0f;
            }
        }

        this->batch3DQuadCount++;
        return;
    }

    this->stats.immediate3DDraws++;
    UploadMVP();
    UploadUniforms();

    f32 tfCol[4];
    ColorToFloat(this->textureFactor, tfCol);

    const size_t needed = (size_t)count * 9;
    if (this->drawScratch.size() < needed)
        this->drawScratch.resize(needed);
    f32 *buf = this->drawScratch.data();
    for (i32 i = 0; i < count; i++)
    {
        f32 *dst = buf + i * 9;
        dst[0] = verts[i].position.x;
        dst[1] = verts[i].position.y;
        dst[2] = verts[i].position.z;
        memcpy(dst + 3, tfCol, sizeof(tfCol));
        dst[7] = verts[i].textureUV.x;
        dst[8] = verts[i].textureUV.y;
    }
    DrawArrays(GL_TRIANGLE_STRIP, nullptr, nullptr, nullptr, count);
}

// continued below — texture management

u32 RendererGLES::CreateTextureFromMemory(const u8 *data, i32 dataLen, D3DCOLOR colorKey,
                                           i32 *outWidth, i32 *outHeight)
{
    SDL_RWops *rw = SDL_RWFromConstMem(data, dataLen);
    if (!rw) return 0;
    SDL_Surface *surface = IMG_Load_RW(rw, 1);
    if (!surface) return 0;

    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surface);
    if (!rgba) return 0;

    if (colorKey != 0)
    {
        u8 ckR = D3DCOLOR_R(colorKey);
        u8 ckG = D3DCOLOR_G(colorKey);
        u8 ckB = D3DCOLOR_B(colorKey);
        SDL_LockSurface(rgba);
        u8 *pixels = (u8 *)rgba->pixels;
        i32 pixelCount = rgba->w * rgba->h;
        for (i32 i = 0; i < pixelCount; i++)
        {
            u8 *p = pixels + i * 4;
            if (p[0] == ckR && p[1] == ckG && p[2] == ckB)
                p[3] = 0;
            else
                p[3] = 255; // D3D8 forces non-key pixels to fully opaque
        }
        SDL_UnlockSurface(rgba);
    }

    if (outWidth) *outWidth = rgba->w;
    if (outHeight) *outHeight = rgba->h;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    this->stats.textureBinds++;
    this->stats.textureUploads++;
    this->stats.textureUploadBytes += (u64)rgba->w * rgba->h * 4;

#ifdef __ANDROID__
    // Diagnostic: dump alpha values at key positions for texture analysis
    {
        static i32 texLoadCount = 0;
        texLoadCount++;
        SDL_LockSurface(rgba);
        u8 *px = (u8 *)rgba->pixels;
        __android_log_print(ANDROID_LOG_INFO, "TH06_DIAG",
            "CreateTex #%d size=%dx%d colorKey=0x%08X",
            texLoadCount, rgba->w, rgba->h, (unsigned)colorKey);
        // Dump RGBA at center of each 16x16 block in first 4 rows
        for (i32 by = 0; by < 4 && by * 16 < rgba->h; by++) {
            char buf[512];
            int pos = 0;
            for (i32 bx = 0; bx < 16 && bx * 16 < rgba->w; bx++) {
                i32 cx = bx * 16 + 8;
                i32 cy = by * 16 + 8;
                if (cx < rgba->w && cy < rgba->h) {
                    u8 *p = px + cy * rgba->pitch + cx * 4;
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "[%d,%d]=%02X%02X%02X.%02X ", bx, by, p[0], p[1], p[2], p[3]);
                }
            }
            __android_log_print(ANDROID_LOG_INFO, "TH06_DIAG", "  row%d: %s", by, buf);
        }
        SDL_UnlockSurface(rgba);
    }
#endif

    UploadRgbaSurface(tex, rgba);
    SDL_FreeSurface(rgba);

    if (this->currentTexture != 0)
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, this->currentTexture);
    }
    return (u32)tex;
}

u32 RendererGLES::CreateEmptyTexture(i32 width, i32 height)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::vector<u8> zeroPixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
    this->stats.textureBinds++;
    this->stats.textureUploads++;
    this->stats.textureUploadBytes += (u64)width * height * 4;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeroPixels.data());

    if (this->currentTexture != 0)
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, this->currentTexture);
    }
    return (u32)tex;
}

void RendererGLES::DeleteTexture(u32 tex)
{
    if (tex != 0)
    {
        if (this->currentTexture == tex)
        {
            this->currentTexture = 0;
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        GLuint t = (GLuint)tex;
        glDeleteTextures(1, &t);
    }
}

void RendererGLES::CopyAlphaChannel(u32 dstTex, const u8 *srcData, i32 dataLen, i32 width, i32 height)
{
    if (dstTex == 0) return;

    SDL_RWops *rw = SDL_RWFromConstMem(srcData, dataLen);
    if (!rw) return;
    SDL_Surface *srcSurface = IMG_Load_RW(rw, 1);
    if (!srcSurface) return;
    SDL_Surface *srcRgba = SDL_ConvertSurfaceFormat(srcSurface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(srcSurface);
    if (!srcRgba) return;

    // GLES has no glGetTexImage — render texture to FBO, then readback
    GLuint tmpFbo = 0;
    pglGenFramebuffers(1, &tmpFbo);
    pglBindFramebuffer(GL_FRAMEBUFFER, tmpFbo);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)dstTex, 0);

    if (width <= 0 || height <= 0)
    {
        // Query via FBO readback — read 1x1 first to confirm attachment works.
        // In practice, callers always pass valid w/h; this is a safety fallback.
        width = srcRgba->w;
        height = srcRgba->h;
    }

    u8 *dstPixels = new u8[width * height * 4];
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, dstPixels);
    this->stats.readbacks++;

    // Copy blue channel of source as alpha channel of destination
    i32 copyW = srcRgba->w < width ? srcRgba->w : width;
    i32 copyH = srcRgba->h < height ? srcRgba->h : height;
    SDL_LockSurface(srcRgba);
    for (i32 y = 0; y < copyH; y++)
    {
        u8 *src = (u8 *)srcRgba->pixels + y * srcRgba->pitch;
        u8 *dst = dstPixels + y * width * 4;
        for (i32 x = 0; x < copyW; x++)
            dst[x * 4 + 3] = src[x * 4 + 0]; // blue -> alpha
    }
    SDL_UnlockSurface(srcRgba);

    this->stats.textureBinds++;
    this->stats.textureUploads++;
    this->stats.textureUploadBytes += (u64)width * height * 4;
    glBindTexture(GL_TEXTURE_2D, (GLuint)dstTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, dstPixels);

    // Cleanup
    pglBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
    pglDeleteFramebuffers(1, &tmpFbo);

    if (this->currentTexture != 0)
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, this->currentTexture);
    }
    delete[] dstPixels;
    SDL_FreeSurface(srcRgba);
}

void RendererGLES::UpdateTextureSubImage(u32 tex, i32 x, i32 y, i32 w, i32 h, const u8 *rgbaPixels)
{
    if (tex == 0 || !rgbaPixels) return;
    this->stats.textureBinds++;
    this->stats.textureUploads++;
    this->stats.textureUploadBytes += (u64)w * h * 4;
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
    if (this->currentTexture != 0)
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, this->currentTexture);
    }
}

// continued below — surface operations

u32 RendererGLES::LoadSurfaceFromFile(const u8 *data, i32 dataLen, i32 *outWidth, i32 *outHeight)
{
    SDL_RWops *rw = SDL_RWFromConstMem(data, dataLen);
    if (!rw) return 0;
    SDL_Surface *surface = IMG_Load_RW(rw, 1);
    if (!surface) return 0;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surface);
    if (!rgba) return 0;
    if (outWidth) *outWidth = rgba->w;
    if (outHeight) *outHeight = rgba->h;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    this->stats.textureBinds++;
    this->stats.textureUploads++;
    this->stats.textureUploadBytes += (u64)rgba->w * rgba->h * 4;
    UploadRgbaSurface(tex, rgba);
    SDL_FreeSurface(rgba);

    if (this->currentTexture != 0)
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, this->currentTexture);
    }
    return (u32)tex;
}

u32 RendererGLES::LoadSurfaceFromFile(const u8 *data, i32 dataLen, D3DXIMAGE_INFO *info)
{
    i32 w = 0, h = 0;
    u32 result = LoadSurfaceFromFile(data, dataLen, &w, &h);
    if (info) { info->Width = (u32)w; info->Height = (u32)h; }
    return result;
}

// Helper: draw a textured fullscreen-aligned quad via shader (for surface blit)
void RendererGLES::CopySurfaceToScreen(u32 surfaceTex, i32 srcX, i32 srcY,
                                        i32 dstX, i32 dstY, i32 w, i32 h,
                                        i32 texW, i32 texH)
{
    if (surfaceTex == 0) return;
    f32 u0 = (f32)srcX / texW;
    f32 v0 = (f32)srcY / texH;
    f32 u1 = (f32)(srcX + (w > 0 ? w : texW)) / texW;
    f32 v1 = (f32)(srcY + (h > 0 ? h : texH)) / texH;
    i32 drawW = w > 0 ? w : texW;
    i32 drawH = h > 0 ? h : texH;

    if (!this->in2DPass) Enter2DPass();
    SetBlendMode(BLEND_MODE_ALPHA);
    SetColorOp(0);

    u8 prevTexEnabled = this->textureEnabled;
    if (!prevTexEnabled)
    {
        if (this->in2DPass) FlushBatch(Flush2D_State);
        this->textureEnabled = 1;
        pglUniform1i(loc_u_TextureEnabled, 1);
    }
    // Force-bind surface texture (bypass cache — temp texture)
    FlushBatch(Flush2D_Surface);
    this->stats.textureBinds++;
    glBindTexture(GL_TEXTURE_2D, (GLuint)surfaceTex);

    const D3DCOLOR white = 0xFFFFFFFF;
    const size_t off = (size_t)this->batchQuadCount * 4;
    BatchVertex *bv = this->batchBuffer.data() + off;
    FillBatchVert(bv[0], (f32)dstX,         (f32)dstY,         0, white, u0, v0);
    FillBatchVert(bv[1], (f32)(dstX + drawW),(f32)dstY,         0, white, u1, v0);
    FillBatchVert(bv[2], (f32)dstX,         (f32)(dstY + drawH),0, white, u0, v1);
    FillBatchVert(bv[3], (f32)(dstX + drawW),(f32)(dstY + drawH),0, white, u1, v1);
    this->batchQuadCount++;
    FlushBatch(Flush2D_Surface);

    if (!prevTexEnabled)
    {
        this->textureEnabled = 0;
        pglUniform1i(loc_u_TextureEnabled, 0);
    }
    if (this->currentTexture != 0)
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, this->currentTexture);
    }
}

void RendererGLES::CopySurfaceToScreen(u32 surfaceTex, i32 texW, i32 texH, i32 dstX, i32 dstY)
{
    CopySurfaceToScreen(surfaceTex, 0, 0, dstX, dstY, texW, texH, texW, texH);
}

void RendererGLES::CopySurfaceRectToScreen(u32 surfaceTex, i32 srcX, i32 srcY, i32 srcW, i32 srcH,
                                             i32 dstX, i32 dstY, i32 texW, i32 texH)
{
    if (surfaceTex == 0) return;
    f32 u0 = (f32)srcX / texW;
    f32 v0 = (f32)srcY / texH;
    f32 u1 = (f32)(srcX + srcW) / texW;
    f32 v1 = (f32)(srcY + srcH) / texH;

    if (!this->in2DPass) Enter2DPass();
    SetBlendMode(BLEND_MODE_ALPHA);
    SetColorOp(0);

    u8 prevTexEnabled = this->textureEnabled;
    if (!prevTexEnabled)
    {
        if (this->in2DPass) FlushBatch(Flush2D_State);
        this->textureEnabled = 1;
        pglUniform1i(loc_u_TextureEnabled, 1);
    }
    FlushBatch(Flush2D_Surface);
    this->stats.textureBinds++;
    glBindTexture(GL_TEXTURE_2D, (GLuint)surfaceTex);

    const D3DCOLOR white = 0xFFFFFFFF;
    const size_t off = (size_t)this->batchQuadCount * 4;
    BatchVertex *bv = this->batchBuffer.data() + off;
    FillBatchVert(bv[0], (f32)dstX,          (f32)dstY,          0, white, u0, v0);
    FillBatchVert(bv[1], (f32)(dstX + srcW), (f32)dstY,          0, white, u1, v0);
    FillBatchVert(bv[2], (f32)dstX,          (f32)(dstY + srcH), 0, white, u0, v1);
    FillBatchVert(bv[3], (f32)(dstX + srcW), (f32)(dstY + srcH), 0, white, u1, v1);
    this->batchQuadCount++;
    FlushBatch(Flush2D_Surface);

    if (!prevTexEnabled)
    {
        this->textureEnabled = 0;
        pglUniform1i(loc_u_TextureEnabled, 0);
    }
    if (this->currentTexture != 0)
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, this->currentTexture);
    }
}

void RendererGLES::TakeScreenshot(u32 dstTex, i32 left, i32 top, i32 width, i32 height)
{
    if (dstTex == 0) return;
    if (this->in2DPass) Leave2DPass();
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_BlitOrReadback);

    GLint prevFbo = 0;
    if (this->fbo != 0)
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        pglBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
    }

    u8 *pixels = new u8[width * height * 4];
    glReadPixels(left, this->screenHeight - top - height, width, height,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    this->stats.readbacks++;

    // Flip vertically (GL reads bottom-up)
    u8 *flipped = new u8[width * height * 4];
    for (i32 y = 0; y < height; y++)
        memcpy(flipped + y * width * 4, pixels + (height - 1 - y) * width * 4, width * 4);

    this->stats.textureBinds++;
    this->stats.textureUploads++;
    this->stats.textureUploadBytes += (u64)width * height * 4;
    glBindTexture(GL_TEXTURE_2D, (GLuint)dstTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped);

    if (this->currentTexture != 0)
    {
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, this->currentTexture);
    }
    if (this->fbo != 0)
        pglBindFramebuffer(GL_FRAMEBUFFER, prevFbo);

    delete[] pixels;
    delete[] flipped;
}

// continued below — DrawScreenSpaceButtons / EndFrame

void RendererGLES::DrawScreenSpaceButtons()
{
    // Collect buttons from both gameplay and menu virtual button systems.
    // Gameplay has up to 7 buttons (4 left + 3 right function keys).
    // Menu has up to 2 buttons. Total max = 9.
    TouchButtonInfo buttons[12];
    int count = TouchVirtualButtons::GetButtonInfo(buttons, 7);
    count += MenuTouchButtons::GetButtonInfo(buttons + count, 5);
    if (count == 0)
        return;

    i32 rw = this->realScreenWidth;
    i32 rh = this->realScreenHeight;

    // Recompute pillarbox layout
    i32 scaledW, scaledH;
    if (rw * this->screenHeight > rh * this->screenWidth)
    {
        scaledH = rh;
        scaledW = rh * this->screenWidth / this->screenHeight;
    }
    else
    {
        scaledW = rw;
        scaledH = rw * this->screenHeight / this->screenWidth;
    }
    i32 offsetX = (rw - scaledW) / 2;
    i32 offsetY = (rh - scaledH) / 2;

    // Need at least some pillarbox to render on
    if (offsetX < 5)
        return;

    // Set up full-screen render state
    glViewport(0, 0, rw, rh);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Ortho projection: pixel coordinates [0,rw]×[0,rh], Y top-down
    D3DXMATRIX ortho;
    memset(&ortho, 0, sizeof(ortho));
    ortho._11 = 2.0f / rw;
    ortho._22 = -2.0f / rh;
    ortho._33 = 1.0f;
    ortho._44 = 1.0f;
    ortho._41 = -1.0f;
    ortho._42 = 1.0f;
    pglUniformMatrix4fv(this->loc_u_MVP, 1, GL_FALSE, &ortho.m[0][0]);

    D3DXMATRIX ident;
    D3DXMatrixIdentity(&ident);
    pglUniformMatrix4fv(this->loc_u_TexMatrix, 1, GL_FALSE, &ident.m[0][0]);
    pglUniformMatrix4fv(this->loc_u_ModelView, 1, GL_FALSE, &ident.m[0][0]);

    pglUniform1i(this->loc_u_TextureEnabled, 0);
    pglUniform1i(this->loc_u_FogEnabled, 0);
    pglUniform1f(this->loc_u_AlphaRef, 0.0f);

    constexpr int HALF_SEGS = 16;
    constexpr float BORDER_W = 2.0f;
    constexpr float PI = 3.14159265358979323846f;
    float yScale = (float)scaledH / 480.0f;

    for (int i = 0; i < count; i++)
    {
        // Map game coords → screen coords.
        //   Y: straightforward proportional mapping into the viewport.
        //   X: depends on the button's anchor (which pillarbox it lives on).
        float sy = offsetY + (buttons[i].gameY / 480.0f) * scaledH;
        float sr = buttons[i].gameRadius * yScale;

        float sx;
        if (buttons[i].anchor == ScreenAnchor::RightPillar)
        {
            // Right pillarbox: button left edge touches game viewport right edge.
            //   sx = (rw - offsetX) + sr
            // This mirrors the left-pillarbox formula across the viewport center.
            sx = (float)(rw - offsetX) + sr;
            if (sx > (float)rw - sr)
                sx = (float)rw - sr;
        }
        else
        {
            // Left pillarbox (default): button right edge touches game viewport left edge.
            sx = (float)offsetX - sr;
            if (sx < sr)
                sx = sr;
        }

        // --- Filled circle (horizontal-band triangle strip) ---
        {
            D3DCOLOR c = buttons[i].fillColor;
            float cr = D3DCOLOR_R(c) / 255.0f, cg = D3DCOLOR_G(c) / 255.0f;
            float cb = D3DCOLOR_B(c) / 255.0f, ca = D3DCOLOR_A(c) / 255.0f;

            int nv = (HALF_SEGS + 1) * 2;
            size_t needed = (size_t)nv * 9;
            if (this->drawScratch.size() < needed)
                this->drawScratch.resize(needed);
            f32 *buf = this->drawScratch.data();

            for (int j = 0; j <= HALF_SEGS; j++)
            {
                float angle = PI / 2.0f - j * PI / HALF_SEGS;
                float cosa = cosf(angle), sina = sinf(angle);
                f32 *L = buf + (j * 2) * 9;
                L[0] = sx - sr * cosa; L[1] = sy - sr * sina; L[2] = 0;
                L[3] = cr; L[4] = cg; L[5] = cb; L[6] = ca;
                L[7] = 0; L[8] = 0;
                f32 *R = buf + (j * 2 + 1) * 9;
                R[0] = sx + sr * cosa; R[1] = sy - sr * sina; R[2] = 0;
                R[3] = cr; R[4] = cg; R[5] = cb; R[6] = ca;
                R[7] = 0; R[8] = 0;
            }
            DrawArrays(GL_TRIANGLE_STRIP, nullptr, nullptr, nullptr, nv);
        }

        // --- Border ring ---
        {
            D3DCOLOR c = buttons[i].borderColor;
            float cr = D3DCOLOR_R(c) / 255.0f, cg = D3DCOLOR_G(c) / 255.0f;
            float cb = D3DCOLOR_B(c) / 255.0f, ca = D3DCOLOR_A(c) / 255.0f;
            float innerR = sr - BORDER_W;

            int segs = HALF_SEGS * 2;
            int nv = (segs + 1) * 2;
            size_t needed = (size_t)nv * 9;
            if (this->drawScratch.size() < needed)
                this->drawScratch.resize(needed);
            f32 *buf = this->drawScratch.data();

            for (int j = 0; j <= segs; j++)
            {
                float angle = j * 2.0f * PI / segs;
                float cosa = cosf(angle), sina = sinf(angle);
                f32 *O = buf + (j * 2) * 9;
                O[0] = sx + sr * cosa; O[1] = sy + sr * sina; O[2] = 0;
                O[3] = cr; O[4] = cg; O[5] = cb; O[6] = ca;
                O[7] = 0; O[8] = 0;
                f32 *I = buf + (j * 2 + 1) * 9;
                I[0] = sx + innerR * cosa; I[1] = sy + innerR * sina; I[2] = 0;
                I[3] = cr; I[4] = cg; I[5] = cb; I[6] = ca;
                I[7] = 0; I[8] = 0;
            }
            DrawArrays(GL_TRIANGLE_STRIP, nullptr, nullptr, nullptr, nv);
        }
    }

    // --- Text labels using ascii.anm texture ---
    if (g_AnmManager && g_AnmManager->textures[ANM_FILE_ASCII] != 0)
    {
        pglUniform1i(this->loc_u_TextureEnabled, 1);
        this->stats.textureBinds++;
        glBindTexture(GL_TEXTURE_2D, g_AnmManager->textures[ANM_FILE_ASCII]);

        for (int i = 0; i < count; i++)
        {
            const char *label = buttons[i].label;
            int len = (int)strlen(label);
            if (len == 0)
                continue;

            float sy = offsetY + (buttons[i].gameY / 480.0f) * scaledH;
            float sr = buttons[i].gameRadius * yScale;

            // Recompute sx for text placement, matching the circle positioning.
            float sx;
            if (buttons[i].anchor == ScreenAnchor::RightPillar)
            {
                sx = (float)(rw - offsetX) + sr;
                if (sx > (float)rw - sr) sx = (float)rw - sr;
            }
            else
            {
                sx = (float)offsetX - sr;
                if (sx < sr) sx = sr;
            }

            float charW = 14.0f * buttons[i].textScale * yScale;
            float charH = 16.0f * buttons[i].textScale * yScale;
            float totalW = len * charW;
            float startX = sx - totalW / 2.0f;
            float startY = sy - charH / 2.0f;

            for (int ci = 0; ci < len; ci++)
            {
                u8 ch = (u8)label[ci];
                if (ch < 0x21) continue;

                AnmLoadedSprite *spr = &g_AnmManager->sprites[ch - 0x15];
                float u0 = spr->uvStart.x, v0 = spr->uvStart.y;
                float u1 = spr->uvEnd.x,   v1 = spr->uvEnd.y;

                float x0 = startX + ci * charW, x1 = x0 + charW;
                float y0 = startY, y1 = y0 + charH;

                f32 pos[] = { x0,y0,0, x1,y0,0, x0,y1,0, x1,y1,0 };
                f32 col[] = { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 };
                f32 tc[]  = { u0,v0, u1,v0, u0,v1, u1,v1 };
                DrawArrays(GL_TRIANGLE_STRIP, pos, col, tc, 4);
            }
        }
    }

    // Restore alpha ref
    pglUniform1f(this->loc_u_AlphaRef, 4.0f / 255.0f);
}

void RendererGLES::BlitFBOToScreen()
{
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_BlitOrReadback);
    this->stats.fboBlits++;
    pglBindFramebuffer(GL_FRAMEBUFFER, 0);

    i32 rw = this->realScreenWidth;
    i32 rh = this->realScreenHeight;

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, rw, rh);
    glScissor(0, 0, rw, rh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // Compute centered 4:3 letterbox/pillarbox
    i32 scaledW, scaledH;
    if (rw * this->screenHeight > rh * this->screenWidth)
    {
        scaledH = rh;
        scaledW = rh * this->screenWidth / this->screenHeight;
    }
    else
    {
        scaledW = rw;
        scaledH = rw * this->screenHeight / this->screenWidth;
    }
    i32 offsetX = (rw - scaledW) / 2;
    i32 offsetY = (rh - scaledH) / 2;

    glViewport(offsetX, offsetY, scaledW, scaledH);
    glScissor(offsetX, offsetY, scaledW, scaledH);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    // Blit FBO texture via shader as a fullscreen quad
    pglUniform1i(this->loc_u_TextureEnabled, 1);
    pglUniform1i(this->loc_u_FogEnabled, 0);
    pglUniform1f(this->loc_u_AlphaRef, 0.0f); // no discard

    // Ortho [0,1]x[0,1] for normalized quad
    D3DXMATRIX ortho;
    memset(&ortho, 0, sizeof(ortho));
    ortho._11 = 2.0f; ortho._22 = 2.0f; ortho._33 = 1.0f; ortho._44 = 1.0f;
    ortho._41 = -1.0f; ortho._42 = -1.0f;
    pglUniformMatrix4fv(this->loc_u_MVP, 1, GL_FALSE, &ortho.m[0][0]);

    D3DXMATRIX ident;
    D3DXMatrixIdentity(&ident);
    pglUniformMatrix4fv(this->loc_u_TexMatrix, 1, GL_FALSE, &ident.m[0][0]);
    pglUniformMatrix4fv(this->loc_u_ModelView, 1, GL_FALSE, &ident.m[0][0]);

    this->stats.textureBinds++;
    glBindTexture(GL_TEXTURE_2D, this->fboColorTex);
    // Filter/wrap params already set at FBO texture creation time

    f32 pos[] = { 0,0,0, 1,0,0, 0,1,0, 1,1,0 };
    f32 col[] = { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 };
    f32 tc[]  = { 0,0, 1,0, 0,1, 1,1 };
    DrawArrays(GL_TRIANGLE_STRIP, pos, col, tc, 4);

    // Restore alpha ref
    pglUniform1f(this->loc_u_AlphaRef, 4.0f / 255.0f);
}

void RendererGLES::EndFrame()
{
    // Flush any pending 2D batch
    if (this->in2DPass) Leave2DPass();
    if (this->batch3DQuadCount != 0)
        Flush3DBatch(Flush3D_EndFrame);

    if (this->fbo != 0)
    {
        // Render ImGui overlay into FBO at game resolution
        glViewport(0, 0, this->screenWidth, this->screenHeight);
        glScissor(0, 0, this->screenWidth, this->screenHeight);
        glDisable(GL_DEPTH_TEST);
        pglUniform1i(this->loc_u_FogEnabled, 0);
        THPrac::THPracGuiRender();
        this->attribsEnabled = false;
        this->fogAttribEnabled = false;

        // Re-activate game shader (ImGui may have switched the active program)
        pglUseProgram(this->shaderProgram);

        // Blit FBO -> screen, draw overlay, swap
        BlitFBOToScreen();
        DrawScreenSpaceButtons();
        SDL_GL_SwapWindow(this->window);
#ifndef __ANDROID__
        // Post-swap blit for desktop capture tools (OBS, etc.)
        BlitFBOToScreen();
        DrawScreenSpaceButtons();
#endif

        // Restore state for next frame
        pglBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
        glEnable(GL_SCISSOR_TEST);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        pglUniform1i(this->loc_u_FogEnabled, this->fogEnabled);
        pglUniform1f(this->loc_u_AlphaRef, 4.0f / 255.0f);
        pglUniform1i(this->loc_u_TextureEnabled, this->textureEnabled);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(this->currentZWriteDisable ? GL_FALSE : GL_TRUE);

        // Invalidate dirty-flag caches so next frame's SetTexture /
        // SetBlendMode / SetColorOp / SetZWriteDisable calls always
        // go through to GL (BlitFBOToScreen changed GL state behind
        // the cache's back).
        this->currentTexture = (u32)-1;
        this->currentBlendMode = 0xff;
        this->currentColorOp = 0xff;
        this->currentZWriteDisable = 0xff;
        this->usingVertexFog = false;
        FinishFrameStats();
    }
    else
    {
        glDisable(GL_DEPTH_TEST);
        pglUniform1i(this->loc_u_FogEnabled, 0);
        THPrac::THPracGuiRender();
        this->attribsEnabled = false;
        this->fogAttribEnabled = false;
        SDL_GL_SwapWindow(this->window);

        // Non-FBO path: ImGui rendering changed GL state (blend, texture,
        // depth, etc.) behind the cache's back.  Invalidate renderer caches
        // so the next frame's state-setting calls always go through to GL,
        // matching the FBO path's cache invalidation.
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_SCISSOR_TEST);
        pglUniform1i(this->loc_u_FogEnabled, this->fogEnabled);
        pglUniform1f(this->loc_u_AlphaRef, 4.0f / 255.0f);
        pglUniform1i(this->loc_u_TextureEnabled, this->textureEnabled);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(this->currentZWriteDisable ? GL_FALSE : GL_TRUE);
        this->currentTexture = (u32)-1;
        this->currentBlendMode = 0xff;
        this->currentColorOp = 0xff;
        this->currentZWriteDisable = 0xff;
        this->usingVertexFog = false;
        FinishFrameStats();
    }
}

void RendererGLES::Present()
{
    // Phase 5a (ADR-008): SDL_GL_SwapWindow runs inside EndFrame() above
    // (both FBO and non-FBO branches). Present() is an explicit no-op so
    // the IRenderer abstraction is satisfied without disturbing the
    // existing GLES frame-end semantics.
}

} // namespace th06

#if defined(TH06_USE_GLES)
namespace th06 {
static RendererGLES s_RendererGLES;
IRenderer *GetRendererGLES() { return &s_RendererGLES; }
} // namespace th06
#endif
