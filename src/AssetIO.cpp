// =============================================================================
// AssetIO.cpp — see AssetIO.hpp for design notes.
// =============================================================================
#include "AssetIO.hpp"

#include <SDL.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>

#ifdef _WIN32
#include <windows.h>
#elif !defined(__ANDROID__)
#include <unistd.h>
#endif

namespace th06
{
namespace AssetIO
{

namespace
{
char s_baseDir[1024] = "";
bool s_inited = false;

#ifndef __ANDROID__
SDL_RWops *OpenAtRoot(const char *root, const char *relPath)
{
    char full[2048];
    if (root != NULL && root[0] != '\0')
    {
        std::snprintf(full, sizeof(full), "%s%s", root, relPath);
    }
    else
    {
        std::snprintf(full, sizeof(full), "%s", relPath);
    }

    // SDL_RWFromFile correctly opens via Win32 CreateFile on Windows;
    // SDL_RWFromFP is unavailable in SDL2 default Windows builds (HAVE_STDIO_H
    // disabled), causing every read to silently fail even when fopen succeeds.
    SDL_RWops *rw = SDL_RWFromFile(full, "rb");
    if (rw == NULL)
    {
        const char *bn = std::strrchr(relPath, '/');
        const char *bn2 = std::strrchr(relPath, '\\');
        if (bn2 > bn) bn = bn2;
        bn = (bn != NULL) ? bn + 1 : relPath;
        if (std::strstr(bn, ".cfg") != NULL || std::strstr(bn, ".save") != NULL || std::strstr(bn, ".dat") != NULL)
        {
            ::th06::AssetIO::DiagLog("AssetIO", "OpenAtRoot SDL_RWFromFile FAIL full=[%s] err=%s", full, SDL_GetError());
        }
        return NULL;
    }
    return rw;
}

// On case-sensitive filesystems (Linux/macOS), assets shipped with mixed-case
// directory names (e.g. "BGM/" instead of "bgm/") would otherwise fail to
// resolve. We retry with the first path component upper-cased as a best-effort
// fallback. Cheap and only invoked when the lowercase lookup already missed.
#if !defined(_WIN32)
SDL_RWops *OpenAtRootCaseFallback(const char *root, const char *relPath)
{
    const char *slash = std::strchr(relPath, '/');
    if (slash == NULL)
    {
        return NULL;
    }
    size_t prefixLen = (size_t)(slash - relPath);
    if (prefixLen == 0 || prefixLen >= 64)
    {
        return NULL;
    }

    char alt[2048];
    if (prefixLen + std::strlen(slash) + 1 > sizeof(alt))
    {
        return NULL;
    }
    for (size_t i = 0; i < prefixLen; ++i)
    {
        char c = relPath[i];
        if (c >= 'a' && c <= 'z')
        {
            c = (char)(c - 'a' + 'A');
        }
        alt[i] = c;
    }
    std::strcpy(alt + prefixLen, slash);

    // Avoid a redundant call if upper-casing produced the same string.
    if (std::strcmp(alt, relPath) == 0)
    {
        return NULL;
    }
    return OpenAtRoot(root, alt);
}
#endif
#endif
} // namespace

void Init(int /*argc*/, char ** /*argv*/)
{
    if (s_inited)
    {
        return;
    }
    s_inited = true;

#ifdef __ANDROID__
    s_baseDir[0] = '\0';
#else
    char cwdBefore[1024] = "";
#ifdef _WIN32
    GetCurrentDirectoryA((DWORD)sizeof(cwdBefore), cwdBefore);
#else
    if (getcwd(cwdBefore, sizeof(cwdBefore)) == NULL) cwdBefore[0] = '\0';
#endif

    char *base = SDL_GetBasePath();
    if (base != NULL)
    {
        std::snprintf(s_baseDir, sizeof(s_baseDir), "%s", base);
        SDL_free(base);
    }

    if (s_baseDir[0] != '\0')
    {
#ifdef _WIN32
        SetCurrentDirectoryA(s_baseDir);
#else
        if (chdir(s_baseDir) != 0)
        {
            // Non-fatal: fall back to caller's cwd.
        }
#endif
    }

    char cwdAfter[1024] = "";
#ifdef _WIN32
    GetCurrentDirectoryA((DWORD)sizeof(cwdAfter), cwdAfter);
#else
    if (getcwd(cwdAfter, sizeof(cwdAfter)) == NULL) cwdAfter[0] = '\0';
#endif
    DiagLog("AssetIO", "Init: cwdBefore=%s baseDir=%s cwdAfter=%s",
            cwdBefore, s_baseDir, cwdAfter);

    // ===========================================================================
    //  吐槽时间 / Rant Banner
    // ---------------------------------------------------------------------------
    //  亲爱的未来维护者：
    //    如果你在调"为什么游戏每次启动都像是新装的、所有配置丢失、cfg 文件
    //    明明在磁盘上就是读不出来"——恭喜，你和 2026/04/25 的我踩了同一个坑。
    //
    //    元凶是 SDL_RWFromFP(fopen的FILE*, SDL_TRUE)。SDL2 在 Windows 默认
    //    构建里 HAVE_STDIO_H = 0（VC 的 FILE 内部布局非公开），于是 RWFromFP
    //    **永远返回 NULL**，但调用方拿到 NULL 当成"文件不存在"处理，于是每次
    //    都走默认值路径再立即写覆盖磁盘上的好 cfg。日志里看着 fopen 全成功、
    //    write 全 OK，但 read 全 MISS——一脸懵。
    //
    //    解决方案：所有 RW 路径统一走 SDL_RWFromFile(path, "rb")，它在 Windows
    //    内部用 CreateFileA，是真正能用的。Linux/Android 也一致。
    //
    //    教训：跨平台 IO 不要在 SDL 之上再裹一层 stdio——要么全 stdio，要么
    //    全 SDL_RW。混用就是地狱。
    //
    //  Dear future maintainer:
    //    SDL_RWFromFP is a trap on Windows (HAVE_STDIO_H=0 in default builds).
    //    Always use SDL_RWFromFile. Don't mix stdio and SDL_RW.
    // ===========================================================================
    DiagLog("AssetIO", "NOTE: using SDL_RWFromFile (not SDL_RWFromFP) — see source for the rant about why.");
#endif
}

const char *BaseDir()
{
    return s_baseDir;
}

SDL_RWops *OpenRW(const char *relPath)
{
    if (relPath == NULL || relPath[0] == '\0')
    {
        return NULL;
    }

    // Trace only paths the user reports as broken (cfg / save / bgm) to
    // keep diag_log.txt from drowning in shader/anm noise.
    bool trace = false;
    if (relPath[0] != '\0')
    {
        const char *bn = std::strrchr(relPath, '/');
        const char *bn2 = std::strrchr(relPath, '\\');
        if (bn2 > bn) bn = bn2;
        bn = (bn != NULL) ? bn + 1 : relPath;
        if (std::strstr(bn, ".cfg") != NULL ||
            std::strstr(bn, ".dat") != NULL ||
            std::strstr(bn, ".save") != NULL ||
            std::strncmp(relPath, "bgm/", 4) == 0 ||
            std::strncmp(relPath, "bgm\\", 4) == 0)
        {
            trace = true;
        }
    }

#ifdef __ANDROID__
    SDL_RWops *r = SDL_RWFromFile(relPath, "rb");
    if (trace) DiagLog("AssetIO", "OpenRW(%s) android -> %s", relPath, r ? "OK" : "MISS");
    return r;
#else
    SDL_RWops *rw = OpenAtRoot("", relPath);
    if (rw != NULL)
    {
        if (trace) DiagLog("AssetIO", "OpenRW(%s) -> cwd OK", relPath);
        return rw;
    }
    if (s_baseDir[0] != '\0')
    {
        rw = OpenAtRoot(s_baseDir, relPath);
        if (rw != NULL)
        {
            if (trace) DiagLog("AssetIO", "OpenRW(%s) -> baseDir OK", relPath);
            return rw;
        }
    }
#if !defined(_WIN32)
    rw = OpenAtRootCaseFallback("", relPath);
    if (rw != NULL)
    {
        if (trace) DiagLog("AssetIO", "OpenRW(%s) -> cwd casefallback OK", relPath);
        return rw;
    }
    if (s_baseDir[0] != '\0')
    {
        rw = OpenAtRootCaseFallback(s_baseDir, relPath);
        if (rw != NULL)
        {
            if (trace) DiagLog("AssetIO", "OpenRW(%s) -> baseDir casefallback OK", relPath);
            return rw;
        }
    }
#endif
    if (trace) DiagLog("AssetIO", "OpenRW(%s) -> MISS (cwd+base+casefallback)", relPath);
    return NULL;
#endif
}

unsigned char *ReadAll(const char *relPath, size_t *outSize)
{
    if (outSize != NULL)
    {
        *outSize = 0;
    }

    SDL_RWops *rw = OpenRW(relPath);
    if (rw == NULL)
    {
        return NULL;
    }

    Sint64 sz = SDL_RWsize(rw);
    if (sz <= 0)
    {
        SDL_RWclose(rw);
        return NULL;
    }

    unsigned char *buf = (unsigned char *)std::malloc((size_t)sz);
    if (buf == NULL)
    {
        SDL_RWclose(rw);
        return NULL;
    }

    size_t got = SDL_RWread(rw, buf, 1, (size_t)sz);
    SDL_RWclose(rw);
    if (got != (size_t)sz)
    {
        std::free(buf);
        return NULL;
    }

    if (outSize != NULL)
    {
        *outSize = (size_t)sz;
    }
    return buf;
}

void DiagLog(const char *tag, const char *fmt, ...)
{
    static FILE *s_f = NULL;
    if (s_f == NULL)
    {
        s_f = std::fopen("diag_log.txt", "a");
        if (s_f == NULL)
        {
            return;
        }
    }

    char line[1024];
    std::va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }

    Uint32 ms = SDL_GetTicks();
    std::fprintf(s_f, "[%02u:%02u.%03u %s] %s\n",
                 (ms / 60000u) % 60u, (ms / 1000u) % 60u, ms % 1000u,
                 tag != NULL ? tag : "", line);
    std::fflush(s_f);
}

} // namespace AssetIO
} // namespace th06
