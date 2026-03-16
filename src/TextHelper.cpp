#include "TextHelper.hpp"
#include "GameWindow.hpp"
#include "Supervisor.hpp"
#include "sdl2_renderer.hpp"
#include "i18n.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <SDL.h>

// Compile our own static copy of stb_truetype to avoid linkage issues
// with imgui's copy (which may have different linkage settings).
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>

#include "encoding_tables.h"

namespace th06
{

// =========================================================================
// Cross-platform font cache: load a system TTF/TTC font once via stb_truetype
// =========================================================================
static u8 *s_fontFileData = NULL;
static size_t s_fontFileSize = 0;
static stbtt_fontinfo s_fontInfo;
static bool s_fontLoaded = false;
static bool s_fontInitAttempted = false;

// Fallback font for glyphs missing from primary (e.g. ♪, ▶)
static u8 *s_fallbackFontData = NULL;
static stbtt_fontinfo s_fallbackFontInfo;
static bool s_fallbackFontLoaded = false;

static bool TryLoadFontInto(const char *path, stbtt_fontinfo *info, u8 **outData)
{
    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
    if (!rw) return false;
    Sint64 sz = SDL_RWsize(rw);
    if (sz <= 0) { SDL_RWclose(rw); return false; }
    u8 *data = (u8 *)malloc((size_t)sz);
    if (!data) { SDL_RWclose(rw); return false; }
    if (SDL_RWread(rw, data, 1, (size_t)sz) != (size_t)sz)
    {
        free(data);
        SDL_RWclose(rw);
        return false;
    }
    SDL_RWclose(rw);

    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0) { free(data); return false; }
    if (!stbtt_InitFont(info, data, offset)) { free(data); return false; }

    *outData = data;
    return true;
}

static bool TryLoadFontFile(const char *path)
{
    if (!TryLoadFontInto(path, &s_fontInfo, &s_fontFileData)) return false;
    s_fontLoaded = true;
    return true;
}

// =========================================================================
// Cross-platform encoding conversion: GBK / Shift-JIS → UTF-8
// Uses embedded lookup tables (encoding_tables.h) — no platform API needed.
// =========================================================================
static int EncodeUtf8(u32 cp, char *out)
{
    if (cp < 0x80)       { out[0] = (char)cp;                                                           return 1; }
    if (cp < 0x800)      { out[0] = (char)(0xC0|(cp>>6));      out[1] = (char)(0x80|(cp&0x3F));          return 2; }
    if (cp < 0x10000)    { out[0] = (char)(0xE0|(cp>>12));     out[1] = (char)(0x80|((cp>>6)&0x3F)); out[2] = (char)(0x80|(cp&0x3F)); return 3; }
    out[0] = (char)(0xF0|(cp>>18)); out[1] = (char)(0x80|((cp>>12)&0x3F)); out[2] = (char)(0x80|((cp>>6)&0x3F)); out[3] = (char)(0x80|(cp&0x3F)); return 4;
}

static u16 GbkLookup(u8 lead, u8 trail)
{
    if (lead < GBK_LEAD_MIN || lead > GBK_LEAD_MAX) return 0;
    if (trail < GBK_TRAIL_MIN || trail > GBK_TRAIL_MAX) return 0;
    return g_gbkToUnicode[(lead - GBK_LEAD_MIN) * GBK_TRAIL_COUNT + (trail - GBK_TRAIL_MIN)];
}

// GB18030 4-byte ranges: (linear_offset, unicode_codepoint) pairs
// marking the start of each contiguous mapping segment.
struct Gb18030Range { u32 gbOff; u32 uni; };
static const Gb18030Range g_gb18030Ranges[] = {
    {0, 0x0080}, {36, 0x00A5}, {38, 0x00A9}, {45, 0x00B2}, {50, 0x00B8},
    {81, 0x00D8}, {89, 0x00E2}, {95, 0x00EB}, {96, 0x00EE}, {100, 0x00F4},
    {103, 0x00F8}, {104, 0x00FB}, {105, 0x00FD}, {109, 0x0102}, {126, 0x0114},
    {133, 0x011C}, {148, 0x012C}, {172, 0x0145}, {175, 0x0149}, {179, 0x014E},
    {208, 0x016C}, {306, 0x01CF}, {307, 0x01D1}, {308, 0x01D3}, {309, 0x01D5},
    {310, 0x01D7}, {311, 0x01D9}, {312, 0x01DB}, {313, 0x01DD}, {341, 0x01FA},
    {428, 0x0252}, {443, 0x0262}, {544, 0x02C8}, {545, 0x02CC}, {558, 0x02DA},
    {741, 0x03A2}, {742, 0x03AA}, {749, 0x03C2}, {750, 0x03CA}, {805, 0x0402},
    {819, 0x0450}, {820, 0x0452}, {7922, 0x2011}, {7924, 0x2017}, {7925, 0x201A},
    {7927, 0x201E}, {7934, 0x2027}, {7943, 0x2031}, {7944, 0x2034}, {7945, 0x2036},
    {7950, 0x203C}, {8062, 0x20AD}, {8148, 0x2104}, {8149, 0x2106}, {8152, 0x210A},
    {8164, 0x2117}, {8174, 0x2122}, {8236, 0x216C}, {8240, 0x217A}, {8262, 0x2194},
    {8264, 0x219A}, {8374, 0x2209}, {8380, 0x2210}, {8381, 0x2212}, {8384, 0x2216},
    {8388, 0x221B}, {8390, 0x2221}, {8392, 0x2224}, {8393, 0x2226}, {8394, 0x222C},
    {8396, 0x222F}, {8401, 0x2238}, {8406, 0x223E}, {8416, 0x2249}, {8419, 0x224D},
    {8424, 0x2253}, {8437, 0x2262}, {8439, 0x2268}, {8445, 0x2270}, {8482, 0x2296},
    {8485, 0x229A}, {8496, 0x22A6}, {8521, 0x22C0}, {8603, 0x2313}, {8936, 0x246A},
    {8946, 0x249C}, {9046, 0x254C}, {9050, 0x2574}, {9063, 0x2590}, {9066, 0x2596},
    {9076, 0x25A2}, {9092, 0x25B4}, {9100, 0x25BE}, {9108, 0x25C8}, {9111, 0x25CC},
    {9113, 0x25D0}, {9131, 0x25E6}, {9162, 0x2607}, {9164, 0x260A}, {9218, 0x2641},
    {9219, 0x2643}, {11329, 0x2E82}, {11331, 0x2E85}, {11334, 0x2E89}, {11336, 0x2E8D},
    {11346, 0x2E98}, {11361, 0x2EA8}, {11363, 0x2EAB}, {11366, 0x2EAF}, {11370, 0x2EB4},
    {11372, 0x2EB8}, {11375, 0x2EBC}, {11389, 0x2ECB}, {11682, 0x2FFC}, {11686, 0x3004},
    {11687, 0x3018}, {11692, 0x301F}, {11694, 0x302A}, {11714, 0x303F}, {11716, 0x3094},
    {11723, 0x309F}, {11725, 0x30F7}, {11730, 0x30FF}, {11736, 0x312A}, {11982, 0x322A},
    {11989, 0x3232}, {12102, 0x32A4}, {12336, 0x3390}, {12348, 0x339F}, {12350, 0x33A2},
    {12384, 0x33C5}, {12393, 0x33CF}, {12395, 0x33D3}, {12397, 0x33D6}, {12510, 0x3448},
    {12553, 0x3474}, {12851, 0x359F}, {12962, 0x360F}, {12973, 0x361B}, {13738, 0x3919},
    {13823, 0x396F}, {13919, 0x39D1}, {13933, 0x39E0}, {14080, 0x3A74}, {14298, 0x3B4F},
    {14585, 0x3C6F}, {14698, 0x3CE1}, {15583, 0x4057}, {15847, 0x4160}, {16318, 0x4338},
    {16434, 0x43AD}, {16438, 0x43B2}, {16481, 0x43DE}, {16729, 0x44D7}, {17102, 0x464D},
    {17122, 0x4662}, {17315, 0x4724}, {17320, 0x472A}, {17402, 0x477D}, {17418, 0x478E},
    {17859, 0x4948}, {17909, 0x497B}, {17911, 0x497E}, {17915, 0x4984}, {17916, 0x4987},
    {17936, 0x499C}, {17939, 0x49A0}, {17961, 0x49B8}, {18664, 0x4C78}, {18703, 0x4CA4},
    {18814, 0x4D1A}, {18962, 0x4DAF}, {19043, 0x9FA6}, {33469, 0xE76C}, {33470, 0xE7C8},
    {33471, 0xE7E7}, {33484, 0xE815}, {33485, 0xE819}, {33490, 0xE81F}, {33497, 0xE827},
    {33501, 0xE82D}, {33505, 0xE833}, {33513, 0xE83C}, {33520, 0xE844}, {33536, 0xE856},
    {33550, 0xE865}, {37845, 0xF92D}, {37921, 0xF97A}, {37948, 0xF996}, {38029, 0xF9E8},
    {38038, 0xF9F2}, {38064, 0xFA10}, {38065, 0xFA12}, {38066, 0xFA15}, {38069, 0xFA19},
    {38075, 0xFA22}, {38076, 0xFA25}, {38078, 0xFA2A}, {39108, 0xFE32}, {39109, 0xFE45},
    {39113, 0xFE53}, {39114, 0xFE58}, {39115, 0xFE67}, {39116, 0xFE6C}, {39265, 0xFF5F},
    {39394, 0xFFE6},
};
static const int g_gb18030RangeCount = sizeof(g_gb18030Ranges) / sizeof(g_gb18030Ranges[0]);

static u32 Gb18030FourByteLookup(u8 b1, u8 b2, u8 b3, u8 b4)
{
    u32 linear = (u32)(b1 - 0x81) * 12600 + (u32)(b2 - 0x30) * 1260 +
                 (u32)(b3 - 0x81) * 10 + (u32)(b4 - 0x30);
    // Binary search for the range containing this linear offset
    int lo = 0, hi = g_gb18030RangeCount - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (linear < g_gb18030Ranges[mid].gbOff)
            hi = mid - 1;
        else if (mid + 1 < g_gb18030RangeCount && linear >= g_gb18030Ranges[mid + 1].gbOff)
            lo = mid + 1;
        else
        {
            return g_gb18030Ranges[mid].uni + (linear - g_gb18030Ranges[mid].gbOff);
        }
    }
    return 0;
}

static u16 SjisLookup(u8 lead, u8 trail)
{
    int leadIdx;
    if (lead >= SJIS_LEAD1_MIN && lead <= SJIS_LEAD1_MAX)
        leadIdx = lead - SJIS_LEAD1_MIN;
    else if (lead >= SJIS_LEAD2_MIN && lead <= SJIS_LEAD2_MAX)
        leadIdx = lead - SJIS_LEAD2_MIN + (SJIS_LEAD1_MAX - SJIS_LEAD1_MIN + 1);
    else
        return 0;
    if (trail < SJIS_TRAIL_MIN || trail > SJIS_TRAIL_MAX) return 0;
    return g_sjisToUnicode[leadIdx * SJIS_TRAIL_COUNT + (trail - SJIS_TRAIL_MIN)];
}

static char *ConvertToUtf8(const char *str, size_t byteLen)
{
    // Worst case: each input byte → 3 UTF-8 bytes
    char *out = (char *)malloc(byteLen * 3 + 1);
    if (!out) return NULL;
    char *p = out;
    const u8 *s = (const u8 *)str;
    const u8 *end = s + byteLen;

    while (s < end && *s)
    {
        u8 b = *s;

        // 0x7F = TEXT_RIGHT_ARROW in game code → render as ▶ (U+25B6)
        if (b == 0x7F)
        {
            p += EncodeUtf8(0x25B6, p);
            s++;
            continue;
        }

        // ASCII pass-through
        if (b < 0x80)
        {
            *p++ = (char)b;
            s++;
            continue;
        }

#if defined(TH_CHARSET_GBK)
        // GB18030 4-byte sequence: byte1 0x81-0xFE, byte2 0x30-0x39, byte3 0x81-0xFE, byte4 0x30-0x39
        if (s + 3 < end && b >= 0x81 && b <= 0xFE &&
            s[1] >= 0x30 && s[1] <= 0x39 &&
            s[2] >= 0x81 && s[2] <= 0xFE &&
            s[3] >= 0x30 && s[3] <= 0x39)
        {
            u32 cp = Gb18030FourByteLookup(b, s[1], s[2], s[3]);
            if (cp) { p += EncodeUtf8(cp, p); s += 4; continue; }
            s += 4;
            continue;
        }
        // GBK double-byte
        if (s + 1 < end)
        {
            u16 cp = GbkLookup(b, s[1]);
            if (cp) { p += EncodeUtf8(cp, p); s += 2; continue; }
        }
#else
        // Shift-JIS: half-width katakana single-byte (0xA1-0xDF)
        if (b >= SJIS_KANA_MIN && b <= SJIS_KANA_MAX)
        {
            u16 cp = g_sjisKanaToUnicode[b - SJIS_KANA_MIN];
            if (cp) { p += EncodeUtf8(cp, p); s++; continue; }
        }
        // Shift-JIS double-byte
        if (s + 1 < end)
        {
            u16 cp = SjisLookup(b, s[1]);
            if (cp) { p += EncodeUtf8(cp, p); s += 2; continue; }
        }
#endif
        // Unmapped byte: skip
        s++;
    }
    *p = '\0';
    return out;
}

static void EnsureFontLoaded()
{
    if (s_fontInitAttempted) return;
    s_fontInitAttempted = true;

    // Try system font paths in order of preference.
    // Font order depends on the build's text encoding:
    // GBK builds need Chinese fonts first, Shift-JIS builds need Japanese fonts first.
    static const char *fontPaths[] = {
#ifdef _WIN32
#if defined(TH_CHARSET_GBK)
        // Fullwidth CJK fonts first - consistent character widths matching original game
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simkai.ttf",
        "C:/Windows/Fonts/msgothic.ttc",
        "C:/Windows/Fonts/YuGothR.ttc",
#else
        // Japanese fonts first for Shift-JIS builds
        "C:/Windows/Fonts/msgothic.ttc",
        "C:/Windows/Fonts/YuGothR.ttc",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/msmincho.ttc",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/simhei.ttf",
#endif
        "C:/Windows/Fonts/arial.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
        "/Library/Fonts/Arial Unicode.ttf",
#else
        // Linux: Noto CJK, Google Noto, WenQuanYi, etc.
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
        NULL
    };
    for (int i = 0; fontPaths[i]; i++)
    {
        if (TryLoadFontFile(fontPaths[i]))
        {
            SDL_Log("TextHelper: loaded font from %s", fontPaths[i]);
            break;
        }
    }
    if (!s_fontLoaded)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "TextHelper: no system font found, text rendering will be degraded");
        return;
    }

    // Load fallback fonts for missing glyphs (symbols like ♪ ▶).
    // Try fonts that weren't selected as primary.
    static const char *fallbackPaths[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/msgothic.ttc",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/seguisym.ttf",
        "C:/Windows/Fonts/symbola.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
#endif
        NULL
    };
    for (int i = 0; fallbackPaths[i]; i++)
    {
        if (TryLoadFontInto(fallbackPaths[i], &s_fallbackFontInfo, &s_fallbackFontData))
        {
            s_fallbackFontLoaded = true;
            SDL_Log("TextHelper: loaded fallback font from %s", fallbackPaths[i]);
            break;
        }
    }
}

// =========================================================================
// UTF-8 decoder: returns codepoint and advances pointer
// =========================================================================
static u32 DecodeUtf8(const char *&p)
{
    u8 c = (u8)*p++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0)
    {
        u32 cp = (c & 0x1F) << 6;
        cp |= ((u8)*p++) & 0x3F;
        return cp;
    }
    if ((c & 0xF0) == 0xE0)
    {
        u32 cp = (c & 0x0F) << 12;
        cp |= (((u8)*p++) & 0x3F) << 6;
        cp |= ((u8)*p++) & 0x3F;
        return cp;
    }
    if ((c & 0xF8) == 0xF0)
    {
        u32 cp = (c & 0x07) << 18;
        cp |= (((u8)*p++) & 0x3F) << 12;
        cp |= (((u8)*p++) & 0x3F) << 6;
        cp |= ((u8)*p++) & 0x3F;
        return cp;
    }
    return 0xFFFD; // replacement character
}

// =========================================================================
// Render a single glyph into an A8R8G8B8 buffer using stb_truetype
// =========================================================================
static void RenderGlyphToARGB(u8 *dst, i32 dstW, i32 dstH, i32 dstPitch,
                               i32 px, i32 py, float scale,
                               int glyphIndex, u32 argbColor,
                               const stbtt_fontinfo *font)
{
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(font, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

    int gw = x1 - x0;
    int gh = y1 - y0;
    if (gw <= 0 || gh <= 0) return;

    u8 *glyphBitmap = (u8 *)calloc(gw * gh, 1);
    if (!glyphBitmap) return;
    stbtt_MakeGlyphBitmap(font, glyphBitmap, gw, gh, gw, scale, scale, glyphIndex);

    u8 cr = (argbColor >> 16) & 0xFF;
    u8 cg = (argbColor >> 8) & 0xFF;
    u8 cb = argbColor & 0xFF;

    for (int row = 0; row < gh; row++)
    {
        int dy = py + y0 + row;
        if (dy < 0 || dy >= dstH) continue;
        for (int col = 0; col < gw; col++)
        {
            int dx = px + x0 + col;
            if (dx < 0 || dx >= dstW) continue;
            u8 alpha = glyphBitmap[row * gw + col];
            if (alpha == 0) continue;

            // Composite: src-over onto the destination A8R8G8B8 pixel
            u32 *pixel = (u32 *)(dst + dy * dstPitch + dx * 4);
            u8 dstA = (*pixel >> 24) & 0xFF;
            u8 dstR = (*pixel >> 16) & 0xFF;
            u8 dstG = (*pixel >> 8) & 0xFF;
            u8 dstB = *pixel & 0xFF;

            u8 srcA = alpha;
            u8 invA = 255 - srcA;
            u8 outA = srcA + (u8)((dstA * invA) / 255);
            u8 outR = (u8)((cr * srcA + dstR * invA) / 255);
            u8 outG = (u8)((cg * srcA + dstG * invA) / 255);
            u8 outB = (u8)((cb * srcA + dstB * invA) / 255);

            *pixel = ((u32)outA << 24) | ((u32)outR << 16) | ((u32)outG << 8) | outB;
        }
    }
    free(glyphBitmap);
}

// =========================================================================
// Render a string (as UTF-8) onto an A8R8G8B8 pixel buffer at given position
// =========================================================================
static void RenderUtf8StringToARGB(u8 *dst, i32 dstW, i32 dstH, i32 dstPitch,
                                    const char *utf8, float pixelHeight,
                                    i32 startX, i32 startY, u32 argbColor)
{
    if (!s_fontLoaded || !utf8 || !*utf8) return;

    float scale = stbtt_ScaleForPixelHeight(&s_fontInfo, pixelHeight);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&s_fontInfo, &ascent, &descent, &lineGap);
    int baseline = startY + (int)(ascent * scale);

    float fallbackScale = 0;
    int fallbackAscent = 0;
    if (s_fallbackFontLoaded)
    {
        fallbackScale = stbtt_ScaleForPixelHeight(&s_fallbackFontInfo, pixelHeight);
        int fd, fl;
        stbtt_GetFontVMetrics(&s_fallbackFontInfo, &fallbackAscent, &fd, &fl);
    }

    float xpos = (float)startX;
    const char *p = utf8;
    u32 prevCp = 0;
    while (*p)
    {
        u32 cp = DecodeUtf8(p);
        if (cp == 0 || cp == '\n') break;

        const stbtt_fontinfo *useFont = &s_fontInfo;
        float useScale = scale;
        int glyphIndex = stbtt_FindGlyphIndex(&s_fontInfo, cp);
        if (glyphIndex == 0 && cp != ' ' && s_fallbackFontLoaded)
        {
            int fbGlyph = stbtt_FindGlyphIndex(&s_fallbackFontInfo, cp);
            if (fbGlyph != 0)
            {
                glyphIndex = fbGlyph;
                useFont = &s_fallbackFontInfo;
                useScale = fallbackScale;
            }
        }
        if (glyphIndex == 0 && cp != ' ')
        {
            glyphIndex = stbtt_FindGlyphIndex(&s_fontInfo, 0xFFFD);
        }

        if (prevCp)
        {
            int kern = stbtt_GetCodepointKernAdvance(&s_fontInfo, prevCp, cp);
            xpos += kern * scale;
        }

        int renderBaseline = (useFont == &s_fallbackFontInfo)
            ? startY + (int)(fallbackAscent * fallbackScale)
            : baseline;

        RenderGlyphToARGB(dst, dstW, dstH, dstPitch,
                          (int)xpos, renderBaseline, useScale, glyphIndex, argbColor, useFont);

        int advanceWidth, leftSideBearing;
        stbtt_GetGlyphHMetrics(useFont, glyphIndex, &advanceWidth, &leftSideBearing);
        xpos += advanceWidth * useScale;
        prevCp = cp;
    }
}

// =========================================================================
// Static data
// =========================================================================
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
    this->allocated = false;
    this->buffer = NULL;
}

TextHelper::~TextHelper()
{
    this->ReleaseBuffer();
}

bool TextHelper::ReleaseBuffer()
{
    if (this->allocated)
    {
        if (this->buffer)
        {
            free(this->buffer);
        }
        this->format = (D3DFORMAT)-1;
        this->width = 0;
        this->height = 0;
        this->allocated = false;
        this->buffer = NULL;
        return true;
    }
    return false;
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

    u32 bufSize = height * imageWidthInBytes;
    u8 *buf = (u8 *)calloc(bufSize, 1);
    if (buf == NULL)
    {
        return false;
    }
    this->allocated = true;
    this->buffer = buf;
    this->imageSizeInBytes = bufSize;
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

    if (!this->allocated)
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

    i32 textLen = (i32)strlen(string);
    if (textLen <= 0)
    {
        return;
    }

    // Ensure the stb_truetype font is loaded
    EnsureFontLoaded();

    // Clear the staging surface
    memset(g_TextBufferSurface->pixels, 0, g_TextBufferSurface->pitch * g_TextBufferSurface->height);

    if (s_fontLoaded)
    {
        // Convert multi-byte string (Shift-JIS / GBK / UTF-8) to UTF-8
        char *utf8str = ConvertToUtf8(string, textLen + 1);

        // Render at 2x scale (matching original GDI behavior that used fontHeight*2)
        float pixelHeight = (float)(fontHeight * 2);

        // ZunColor is ARGB u32 (same as D3DCOLOR)
        // stb_truetype renders alpha bitmaps; we composite with the requested color.
        // Shadow: render offset text first (shadow behind main text)
        u32 shadowARGB = shadowColor;
        u32 textARGB = textColor;

        // The staging buffer is A8R8G8B8 (g_TextBufferSurface->format)
        u8 *pixels = g_TextBufferSurface->pixels;
        i32 bufW = g_TextBufferSurface->width;
        i32 bufH = g_TextBufferSurface->height;
        i32 pitch = g_TextBufferSurface->pitch;

        if (shadowColor != COLOR_WHITE)
        {
            RenderUtf8StringToARGB(pixels, bufW, bufH, pitch,
                                   utf8str, pixelHeight,
                                   xPos * 2 + 3, 2, shadowARGB);
        }
        RenderUtf8StringToARGB(pixels, bufW, bufH, pitch,
                               utf8str, pixelHeight,
                               xPos * 2, 0, textARGB);

        free(utf8str);
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
