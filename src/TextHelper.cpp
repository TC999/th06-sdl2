#include "TextHelper.hpp"
#include "GameWindow.hpp"
#include "Supervisor.hpp"
#include "sdl2_renderer.hpp"
#include "i18n.hpp"
#include <cstring>

namespace th06
{

DIFFABLE_STATIC_ARRAY_ASSIGN(FormatInfo, 7, g_FormatInfoArray) = {
    {D3DFMT_X8R8G8B8, 32, 0x00000000, 0x00FF0000, 0x0000FF00, 0x000000FF},
    {D3DFMT_A8R8G8B8, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF},
    {D3DFMT_X1R5G5B5, 16, 0x00000000, 0x00007C00, 0x000003E0, 0x0000001F},
    {D3DFMT_R5G6B5, 16, 0x00000000, 0x0000F800, 0x000007E0, 0x0000001F},
    {D3DFMT_A1R5G5B5, 16, 0x0000F000, 0x00007C00, 0x000003E0, 0x0000001F},
    {D3DFMT_A4R4G4B4, 16, 0x0000F000, 0x00000F00, 0x000000F0, 0x0000000F},
    {(D3DFORMAT)-1, 0, 0, 0, 0, 0},
};

TextHelper::TextHelper()
{
    this->format = (D3DFORMAT)-1;
    this->width = 0;
    this->height = 0;
    this->hdc = 0;
    this->gdiObj2 = 0;
    this->gdiObj = 0;
    this->buffer = NULL;
}

TextHelper::~TextHelper()
{
    this->ReleaseBuffer();
}

bool TextHelper::ReleaseBuffer()
{
    if (this->hdc)
    {
#ifdef _WIN32
        SelectObject((HDC)this->hdc, (HGDIOBJ)this->gdiObj);
        DeleteDC((HDC)this->hdc);
        DeleteObject((HGDIOBJ)this->gdiObj2);
#else
        if (this->buffer)
        {
            free(this->buffer);
        }
#endif
        this->format = (D3DFORMAT)-1;
        this->width = 0;
        this->height = 0;
        this->hdc = 0;
        this->gdiObj2 = 0;
        this->gdiObj = 0;
        this->buffer = NULL;
        return true;
    }
    else
    {
        return false;
    }
}

#define TEXT_BUFFER_HEIGHT 64
void TextHelper::CreateTextBuffer()
{
    SoftSurface *surf = new SoftSurface();
    surf->width = GAME_WINDOW_WIDTH;
    surf->height = TEXT_BUFFER_HEIGHT;
    surf->format = D3DFMT_A8R8G8B8;
    surf->pitch = GAME_WINDOW_WIDTH * 4;
    surf->pixels = (u8 *)calloc(surf->pitch * surf->height, 1);
    g_TextBufferSurface = surf;
}

bool TextHelper::AllocateBufferWithFallback(i32 width, i32 height, D3DFORMAT format)
{
    if (this->TryAllocateBuffer(width, height, format))
    {
        return true;
    }

    if (format == D3DFMT_A1R5G5B5 || format == D3DFMT_A4R4G4B4)
    {
        return this->TryAllocateBuffer(width, height, D3DFMT_A8R8G8B8);
    }
    if (format == D3DFMT_R5G6B5)
    {
        return this->TryAllocateBuffer(width, height, D3DFMT_X8R8G8B8);
    }
    return false;
}

#ifdef _WIN32
struct THBITMAPINFO
{
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[17];
};
#endif

#pragma function(memset)
bool TextHelper::TryAllocateBuffer(i32 width, i32 height, D3DFORMAT format)
{
    FormatInfo *formatInfo;
    i32 imageWidthInBytes;

    this->ReleaseBuffer();
    formatInfo = this->GetFormatInfo(format);
    if (formatInfo == NULL)
    {
        return false;
    }
    imageWidthInBytes = ((((width * formatInfo->bitCount) / 8) + 3) / 4) * 4;

#ifdef _WIN32
    THBITMAPINFO bitmapInfo;
    u8 *bitmapData;
    HBITMAP bitmapObj;
    HDC deviceContext;
    HGDIOBJ originalBitmapObj;

    memset(&bitmapInfo, 0, sizeof(THBITMAPINFO));
    bitmapInfo.bmiHeader.biSize = sizeof(THBITMAPINFO);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -(height + 1);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = formatInfo->bitCount;
    bitmapInfo.bmiHeader.biSizeImage = height * imageWidthInBytes;
    if (format != D3DFMT_X1R5G5B5 && format != D3DFMT_X8R8G8B8)
    {
        bitmapInfo.bmiHeader.biCompression = 3;
        ((u32 *)bitmapInfo.bmiColors)[0] = formatInfo->redMask;
        ((u32 *)bitmapInfo.bmiColors)[1] = formatInfo->greenMask;
        ((u32 *)bitmapInfo.bmiColors)[2] = formatInfo->blueMask;
        ((u32 *)bitmapInfo.bmiColors)[3] = formatInfo->alphaMask;
    }
    bitmapObj = CreateDIBSection(NULL, (BITMAPINFO *)&bitmapInfo, 0, (void **)&bitmapData, NULL, 0);
    if (bitmapObj == NULL)
    {
        return false;
    }
    memset(bitmapData, 0, bitmapInfo.bmiHeader.biSizeImage);
    deviceContext = CreateCompatibleDC(NULL);
    originalBitmapObj = SelectObject(deviceContext, bitmapObj);
    this->hdc = deviceContext;
    this->gdiObj2 = bitmapObj;
    this->buffer = bitmapData;
    this->imageSizeInBytes = bitmapInfo.bmiHeader.biSizeImage;
    this->gdiObj = originalBitmapObj;
#else
    u32 bufSize = height * imageWidthInBytes;
    u8 *buf = (u8 *)calloc(bufSize, 1);
    if (buf == NULL)
    {
        return false;
    }
    this->hdc = (void *)1;
    this->gdiObj2 = (void *)1;
    this->buffer = buf;
    this->imageSizeInBytes = bufSize;
    this->gdiObj = NULL;
#endif
    this->width = width;
    this->height = height;
    this->format = format;
    this->imageWidthInBytes = imageWidthInBytes;
    return true;
}

FormatInfo *TextHelper::GetFormatInfo(D3DFORMAT format)
{
    i32 local_8;

    for (local_8 = 0; g_FormatInfoArray[local_8].format != -1 && g_FormatInfoArray[local_8].format != format; local_8++)
    {
    }
    if (format == -1)
    {
        return NULL;
    }
    return &g_FormatInfoArray[local_8];
}

struct A1R5G5B5
{
    u16 blue : 5;
    u16 green : 5;
    u16 red : 5;
    u16 alpha : 1;
};

#pragma var_order(bufferRegion, idx, doubleArea, bufferCursor, bufferStart)
bool TextHelper::InvertAlpha(i32 x, i32 y, i32 spriteWidth, i32 fontHeight)
{
    i32 doubleArea;
    u8 *bufferRegion;
    i32 idx;
    u8 *bufferStart;
    A1R5G5B5 *bufferCursor;

    doubleArea = spriteWidth * fontHeight * 2;
    bufferStart = &this->buffer[0];
    bufferRegion = &bufferStart[y * spriteWidth * 2];
    switch (this->format)
    {
    case D3DFMT_A8R8G8B8:
    {
        // doubleArea is byte count for 16-bit (2 bytes/pixel) formats.
        // For 32-bit A8R8G8B8, we need twice the byte range.
        i32 byteCount = spriteWidth * fontHeight * 4;
        for (idx = 3; idx < byteCount; idx += 4)
        {
            bufferRegion[idx] = bufferRegion[idx] ^ 0xff;
        }
        break;
    }
    case D3DFMT_A1R5G5B5:
        for (bufferCursor = (A1R5G5B5 *)bufferRegion, idx = 0; idx < doubleArea; idx += 2, bufferCursor += 1)
        {
            bufferCursor->alpha ^= 1;
            if (bufferCursor->alpha)
            {
                bufferCursor->red = bufferCursor->red - bufferCursor->red * idx / doubleArea / 2;
                bufferCursor->green = bufferCursor->green - bufferCursor->green * idx / doubleArea / 2;
                bufferCursor->blue = bufferCursor->blue - bufferCursor->blue * idx / doubleArea / 4;
            }
            else
            {
                bufferCursor->red = 31 - idx * 31 / doubleArea / 2;
                bufferCursor->green = 31 - idx * 31 / doubleArea / 2;
                bufferCursor->blue = 31 - idx * 31 / doubleArea / 4;
            }
        }
        break;
    case D3DFMT_A4R4G4B4:
        for (idx = 1; idx < doubleArea; idx = idx + 2)
        {
            bufferRegion[idx] = bufferRegion[idx] ^ 0xf0;
        }
        break;
    default:
        return false;
    }
    return true;
}

#pragma function(memcpy)
bool TextHelper::CopyTextToSurface(SoftSurface *outSurface)
{
    u8 *srcBuf;
    size_t srcWidthBytes;
    int curHeight;
    int dstWidthBytes;
    u8 *dstBuf;
    i32 thisHeight;

    if (this->gdiObj2 == NULL)
    {
        return false;
    }
    if (outSurface == NULL || outSurface->pixels == NULL)
    {
        return false;
    }
    dstWidthBytes = outSurface->pitch;
    srcWidthBytes = this->imageWidthInBytes;
    srcBuf = this->buffer;
    dstBuf = outSurface->pixels;
    if (outSurface->format == this->format)
    {
        for (curHeight = 0; thisHeight = this->height, curHeight < thisHeight; curHeight++)
        {
            memcpy(dstBuf, srcBuf, srcWidthBytes);
            srcBuf += srcWidthBytes;
            dstBuf += dstWidthBytes;
        }
    }
    return true;
}

static void ConvertSurfaceToRGBA(SoftSurface *surf, u8 *rgbaOut, i32 srcX, i32 srcY, i32 srcW, i32 srcH)
{
    for (i32 row = 0; row < srcH; row++)
    {
        i32 sy = srcY + row;
        if (sy < 0 || sy >= surf->height) continue;
        u8 *srcRow = surf->pixels + sy * surf->pitch;
        u8 *dstRow = rgbaOut + row * srcW * 4;
        for (i32 col = 0; col < srcW; col++)
        {
            i32 sx = srcX + col;
            if (sx < 0 || sx >= surf->width)
            {
                dstRow[col * 4 + 0] = 0;
                dstRow[col * 4 + 1] = 0;
                dstRow[col * 4 + 2] = 0;
                dstRow[col * 4 + 3] = 0;
                continue;
            }
            if (surf->format == D3DFMT_A8R8G8B8)
            {
                u32 pixel = ((u32 *)srcRow)[sx];
                dstRow[col * 4 + 0] = (pixel >> 16) & 0xFF;
                dstRow[col * 4 + 1] = (pixel >> 8) & 0xFF;
                dstRow[col * 4 + 2] = pixel & 0xFF;
                dstRow[col * 4 + 3] = (pixel >> 24) & 0xFF;
            }
            else if (surf->format == D3DFMT_A1R5G5B5)
            {
                u16 pixel = ((u16 *)srcRow)[sx];
                u8 r = (pixel >> 10) & 0x1F;
                u8 g = (pixel >> 5) & 0x1F;
                u8 b = pixel & 0x1F;
                u8 a = (pixel >> 15) & 1;
                dstRow[col * 4 + 0] = (r << 3) | (r >> 2);
                dstRow[col * 4 + 1] = (g << 3) | (g >> 2);
                dstRow[col * 4 + 2] = (b << 3) | (b >> 2);
                dstRow[col * 4 + 3] = a ? 255 : 0;
            }
            else
            {
                dstRow[col * 4 + 0] = 0;
                dstRow[col * 4 + 1] = 0;
                dstRow[col * 4 + 2] = 0;
                dstRow[col * 4 + 3] = 0;
            }
        }
    }
}

#pragma function(strlen)
void TextHelper::RenderTextToTexture(i32 xPos, i32 yPos, i32 spriteWidth, i32 spriteHeight, i32 fontHeight,
                                     i32 fontWidth, ZunColor textColor, ZunColor shadowColor, char *string,
                                     u32 outTexture)
{
    RECT destRect;
    RECT srcRect;

#ifdef _WIN32
    memset(g_TextBufferSurface->pixels, 0, g_TextBufferSurface->pitch * g_TextBufferSurface->height);

    // Runtime charset detection based on system ANSI code page.
    // This allows a single binary to work with both Chinese (GBK) and Japanese (Shift-JIS) DAT files.
    UINT acp = GetACP();
    BYTE charset;
    const wchar_t *fontNameW;
    UINT mbCodePage;
    switch (acp)
    {
    case 936:
        charset = GB2312_CHARSET;
        fontNameW = L"SimSun";
        // Use GB18030 (CP 54936) which is a superset of GBK; correctly handles both
        // GBK text from Chinese DATs and GB18030-encoded compiled-in strings (e.g. ♪).
        mbCodePage = 54936;
        break;
    case 950:
        charset = CHINESEBIG5_CHARSET;
        fontNameW = L"MingLiU";
        mbCodePage = 950;
        break;
    default:
        charset = SHIFTJIS_CHARSET;
        fontNameW = L"\xFF2D\xFF33 \x30B4\x30B7\x30C3\x30AF"; // ＭＳ ゴシック
        mbCodePage = 932;
        break;
    }

    HFONT font = CreateFontW(fontHeight * 2, 0, 0, 0, FW_BOLD, false, false, false, charset,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FF_ROMAN | FIXED_PITCH, fontNameW);
    TextHelper textHelper;
    D3DSURFACE_DESC textSurfaceDesc;
    textSurfaceDesc.Width = g_TextBufferSurface->width;
    textSurfaceDesc.Height = g_TextBufferSurface->height;
    textSurfaceDesc.Format = g_TextBufferSurface->format;
    textHelper.AllocateBufferWithFallback(textSurfaceDesc.Width, textSurfaceDesc.Height, textSurfaceDesc.Format);
    HDC hdc = (HDC)textHelper.hdc;
    HGDIOBJ h = SelectObject(hdc, font);
    textHelper.InvertAlpha(0, 0, spriteWidth * 2, fontHeight * 2 + 6);
    SetBkMode(hdc, TRANSPARENT);

    // Convert multi-byte string to wide chars using the system code page,
    // so GBK text from Chinese DATs or Shift-JIS from Japanese DATs is handled correctly.
    int slen = (int)strlen(string);
    int wlen = MultiByteToWideChar(mbCodePage, 0, string, slen, NULL, 0);
    wchar_t wbuf[1024];
    if (wlen >= 1024)
        wlen = 1023;
    MultiByteToWideChar(mbCodePage, 0, string, slen, wbuf, wlen);
    wbuf[wlen] = L'\0';

    if (shadowColor != COLOR_WHITE)
    {
        SetTextColor(hdc, shadowColor);
        TextOutW(hdc, xPos * 2 + 3, 2, wbuf, wlen);
    }
    SetTextColor(hdc, textColor);
    TextOutW(hdc, xPos * 2, 0, wbuf, wlen);

    SelectObject(hdc, h);
    textHelper.InvertAlpha(0, 0, spriteWidth * 2, fontHeight * 2 + 6);
    textHelper.CopyTextToSurface(g_TextBufferSurface);
    SelectObject(hdc, h);
    DeleteObject(font);
#endif

    i32 textLen = (i32)strlen(string);
    if (textLen <= 0)
    {
        return;
    }

    // Original D3D8 rects:
    //   destRect = {0, yPos, spriteWidth, yPos+16}
    //   srcRect  = {0, 0, spriteWidth*2-2, fontHeight*2-2}
    // D3DXLoadSurfaceFromSurface does a point-sampled blit/scale from src to dest.
    // We replicate this while clamping to the staging buffer dimensions.
    srcRect.left = 0;
    srcRect.top = 0;
    srcRect.right = spriteWidth * 2 - 2;
    srcRect.bottom = fontHeight * 2 - 2;

    // Clamp source to staging buffer bounds
    if (srcRect.right > (LONG)g_TextBufferSurface->width)
        srcRect.right = (LONG)g_TextBufferSurface->width;
    if (srcRect.bottom > (LONG)g_TextBufferSurface->height)
        srcRect.bottom = (LONG)g_TextBufferSurface->height;

    destRect.left = 0;
    destRect.top = yPos;
    destRect.right = spriteWidth;
    destRect.bottom = yPos + 16;

    i32 srcW = srcRect.right - srcRect.left;
    i32 srcH = srcRect.bottom - srcRect.top;
    if (srcW <= 0 || srcH <= 0) return;

    i32 dstW = destRect.right - destRect.left;
    i32 dstH = destRect.bottom - destRect.top;
    if (dstW <= 0 || dstH <= 0) return;

    u8 *srcRGBA = (u8 *)calloc(srcW * srcH * 4, 1);
    ConvertSurfaceToRGBA(g_TextBufferSurface, srcRGBA, srcRect.left, srcRect.top, srcW, srcH);

    u8 *dstRGBA = (u8 *)calloc(dstW * dstH * 4, 1);
    for (i32 row = 0; row < dstH; row++)
    {
        i32 sy = row * srcH / dstH;
        for (i32 col = 0; col < dstW; col++)
        {
            i32 sx = col * srcW / dstW;
            u8 *sp = srcRGBA + (sy * srcW + sx) * 4;
            u8 *dp = dstRGBA + (row * dstW + col) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }

    g_Renderer.UpdateTextureSubImage(outTexture, destRect.left, destRect.top, dstW, dstH, dstRGBA);
    free(dstRGBA);
    free(srcRGBA);
    return;
}

void th06::TextHelper::ReleaseTextBuffer()
{
    if (g_TextBufferSurface != NULL)
    {
        g_TextBufferSurface->Release();
        g_TextBufferSurface = NULL;
    }
    return;
}
}; // namespace th06
