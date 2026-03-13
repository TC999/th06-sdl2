#pragma once

#include "ZunColor.hpp"
#include "inttypes.hpp"
#include "sdl2_compat.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace th06
{
struct FormatInfo
{
    D3DFORMAT format;
    i32 bitCount;
    u32 alphaMask;
    u32 redMask;
    u32 greenMask;
    u32 blueMask;
};
struct TextHelper
{
    static void CreateTextBuffer();
    static void ReleaseTextBuffer();
    static void RenderTextToTexture(i32 xPos, i32 yPos, i32 spriteWidth, i32 spriteHeight, i32 fontHeight,
                                    i32 fontWidth, ZunColor textColor, ZunColor shadowColor, char *string,
                                    u32 outTexture);

    TextHelper();
    ~TextHelper();
    bool AllocateBufferWithFallback(i32 width, i32 height, D3DFORMAT format);
    bool TryAllocateBuffer(i32 width, i32 height, D3DFORMAT format);
    FormatInfo *GetFormatInfo(D3DFORMAT format);
    bool ReleaseBuffer();
    bool InvertAlpha(i32 x, i32 y, i32 spriteWidth, i32 fontHeight);
    bool CopyTextToSurface(SoftSurface *outSurface);

    D3DFORMAT format;
    i32 width;
    i32 height;
    u32 imageSizeInBytes;
    i32 imageWidthInBytes;
#ifdef _WIN32
    HDC hdc;
    HGDIOBJ gdiObj;
    HGDIOBJ gdiObj2;
#else
    void *hdc;
    void *gdiObj;
    void *gdiObj2;
#endif
    u8 *buffer;
};
}; // namespace th06
