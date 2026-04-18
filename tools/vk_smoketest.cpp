// SPDX-License-Identifier: MIT
// Phase 1 Gate executable — 不依赖 th06.exe / 不依赖 --backend= CLI（Phase 5 才有）。
// 用法: vk_smoketest [--frames=N] [--windowed] [--width=W] [--height=H]
//
// 直接构造 RendererVulkan，跑 N 帧（每帧 Clear 成不同灰阶）后退出。
// 退出码 0 = 成功；非 0 = 初始化或运行错误。
#include "../src/RendererVulkan.hpp"
#include "../src/sdl2_renderer.hpp"   // VertexDiffuseXyzrwh
#include "../src/sdl2_compat.hpp"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using th06::RendererVulkan;
using th06::VertexDiffuseXyzrwh;

static int ParseInt(const char* s, int defv) {
    if (!s || !*s) return defv;
    int v = std::atoi(s);
    return v > 0 ? v : defv;
}

int main(int argc, char** argv) {
    int  frames   = 120;
    int  width    = 640;
    int  height   = 480;
    bool windowed = false;
    int  stressN  = 0;     // Phase 3: --stress=N runs Phase3StressTest then exits

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--frames=", 9) == 0)        frames = ParseInt(a + 9, frames);
        else if (std::strncmp(a, "--width=",  8) == 0)   width  = ParseInt(a + 8, width);
        else if (std::strncmp(a, "--height=", 9) == 0)   height = ParseInt(a + 9, height);
        else if (std::strncmp(a, "--stress=", 9) == 0)   stressN= ParseInt(a + 9, 100);
        else if (std::strcmp(a, "--windowed") == 0)      windowed = true;
        else if (std::strcmp(a, "--help") == 0) {
            std::printf("Usage: vk_smoketest [--frames=N] [--width=W] [--height=H] [--windowed] [--stress=N]\n");
            return 0;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // volk: load vulkan-1.dll early so SDL_Vulkan_GetInstanceExtensions has the loader available.
    // (SDL_Vulkan_LoadLibrary is redundant when volk handles the dlopen.)

    Uint32 winFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (!windowed) {
        // 默认 windowed；保留显式 --windowed 仅为参数占位（未来可加全屏开关）
    }
    SDL_Window* win = SDL_CreateWindow("th06_sdl vk_smoketest",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, winFlags);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 3;
    }

    RendererVulkan renderer;
    renderer.Init(win, nullptr, width, height);
    if (!renderer.window) {
        // Init 内部失败时不会 crash，但 window 仍为 win。这里靠日志。
    }

    // Phase 3: --stress=N runs the Phase3 stress test and exits (skip frame loop).
    if (stressN > 0) {
        std::fprintf(stderr, "[vk_smoketest] Running Phase 3 stress test (n=%d)...\n", stressN);
        int errors = renderer.Phase3StressTest(stressN, stderr);
        renderer.Release();
        SDL_DestroyWindow(win);
        SDL_Quit();
        std::fprintf(stderr, "[vk_smoketest] Stress test exit=%d\n", errors);
        return errors;
    }

    // Phase 3: build a 64x64 RGBA texture via CreateEmptyTexture + UpdateTextureSubImage,
    // demonstrating the new VMA-backed texture path. Pattern is a 4-color quadrant gradient.
    const int kTexW = 64, kTexH = 64;
    Uint32 texId = renderer.CreateEmptyTexture(kTexW, kTexH);
    if (texId != 0) {
        std::vector<unsigned char> pixels(kTexW * kTexH * 4);
        for (int y = 0; y < kTexH; ++y) {
            for (int x = 0; x < kTexW; ++x) {
                unsigned char* p = pixels.data() + (y * kTexW + x) * 4;
                bool right = x >= kTexW / 2;
                bool down  = y >= kTexH / 2;
                p[0] = right ? 0xFF : 0x00;          // R
                p[1] = down  ? 0xFF : 0x00;          // G
                p[2] = (right ^ down) ? 0xFF : 0x00; // B
                p[3] = 0xFF;                         // A
            }
        }
        renderer.UpdateTextureSubImage(texId, 0, 0, kTexW, kTexH, pixels.data());
        std::fprintf(stderr, "[vk_smoketest] Phase 3 textured quad enabled (id=%u, %dx%d)\n",
                     unsigned(texId), kTexW, kTexH);
    } else {
        std::fprintf(stderr, "[vk_smoketest] CreateEmptyTexture failed; textured pass skipped\n");
    }

    std::fprintf(stderr, "[vk_smoketest] Running %d frames at %dx%d...\n",
                 frames, width, height);

    int exitCode = 0;
    for (int f = 0; f < frames; ++f) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                std::fprintf(stderr, "[vk_smoketest] SDL_QUIT received at frame %d\n", f);
                f = frames;
                break;
            }
        }
        renderer.BeginFrame();
        // Clear 颜色随帧渐变，确认看到颜色循环 -> 说明 present 链路通。
        // ARGB: A=255, R=ramp, G=64, B=128
        unsigned char r = (unsigned char)((f * 4) & 0xFF);
        D3DCOLOR c = (0xFFu << 24) | (Uint32(r) << 16) | (64u << 8) | 128u;
        renderer.Clear(c, /*clearColor=*/1, /*clearDepth=*/1);

        // Phase 2: draw a colored quad as triangle strip (4 verts) using the
        // DiffuseXyzrwh layout (no texture). Position is in pixel space.
        // Quad rotates a tiny bit per frame via color ramp on each corner.
        float cx = float(width)  * 0.5f;
        float cy = float(height) * 0.5f;
        float h2 = 100.0f;
        renderer.SetBlendMode(0);   // BLEND_MODE_ALPHA
        renderer.SetColorOp(0);     // MODULATE (irrelevant for non-tex pipeline)
        renderer.SetZWriteDisable(1);
        VertexDiffuseXyzrwh quad[4];
        // ARGB packing: 0xAARRGGBB
        quad[0].position = D3DXVECTOR4(cx - h2, cy - h2, 0.5f, 1.0f);
        quad[0].diffuse  = 0xFFFF0000u;  // red
        quad[1].position = D3DXVECTOR4(cx + h2, cy - h2, 0.5f, 1.0f);
        quad[1].diffuse  = 0xFF00FF00u;  // green
        quad[2].position = D3DXVECTOR4(cx - h2, cy + h2, 0.5f, 1.0f);
        quad[2].diffuse  = 0xFF0000FFu;  // blue
        quad[3].position = D3DXVECTOR4(cx + h2, cy + h2, 0.5f, 1.0f);
        quad[3].diffuse  = 0xFFFFFFFFu;  // white
        renderer.DrawTriangleStrip(quad, 4);

        // Phase 3: textured quad in lower-right corner using DrawTriangleStripTextured.
        // Verifies real texture binding (vs Phase 2's 1x1 white default).
        if (texId != 0) {
            renderer.SetTexture(texId);
            renderer.SetColorOp(0);  // MODULATE: tex * diffuse
            float tx = float(width)  - 80.0f;
            float ty = float(height) - 80.0f;
            float ts = 64.0f;
            th06::VertexTex1DiffuseXyzrwh tquad[4];
            tquad[0].position = D3DXVECTOR4(tx,      ty,      0.5f, 1.0f);
            tquad[0].diffuse  = 0xFFFFFFFFu;
            tquad[0].textureUV= D3DXVECTOR2(0.0f, 0.0f);
            tquad[1].position = D3DXVECTOR4(tx + ts, ty,      0.5f, 1.0f);
            tquad[1].diffuse  = 0xFFFFFFFFu;
            tquad[1].textureUV= D3DXVECTOR2(1.0f, 0.0f);
            tquad[2].position = D3DXVECTOR4(tx,      ty + ts, 0.5f, 1.0f);
            tquad[2].diffuse  = 0xFFFFFFFFu;
            tquad[2].textureUV= D3DXVECTOR2(0.0f, 1.0f);
            tquad[3].position = D3DXVECTOR4(tx + ts, ty + ts, 0.5f, 1.0f);
            tquad[3].diffuse  = 0xFFFFFFFFu;
            tquad[3].textureUV= D3DXVECTOR2(1.0f, 1.0f);
            renderer.DrawTriangleStripTextured(tquad, 4);
        }

        renderer.EndFrame();
        SDL_Delay(8);  // 约 120 FPS 上限
    }

    if (texId != 0) renderer.DeleteTexture(texId);
    renderer.Release();
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::fprintf(stderr, "[vk_smoketest] Done (exitCode=%d).\n", exitCode);
    return exitCode;
}
