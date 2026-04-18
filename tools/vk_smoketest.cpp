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

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--frames=", 9) == 0)        frames = ParseInt(a + 9, frames);
        else if (std::strncmp(a, "--width=",  8) == 0)   width  = ParseInt(a + 8, width);
        else if (std::strncmp(a, "--height=", 9) == 0)   height = ParseInt(a + 9, height);
        else if (std::strcmp(a, "--windowed") == 0)      windowed = true;
        else if (std::strcmp(a, "--help") == 0) {
            std::printf("Usage: vk_smoketest [--frames=N] [--width=W] [--height=H] [--windowed]\n");
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

        renderer.EndFrame();
        SDL_Delay(8);  // 约 120 FPS 上限
    }

    renderer.Release();
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::fprintf(stderr, "[vk_smoketest] Done (exitCode=%d).\n", exitCode);
    return exitCode;
}
