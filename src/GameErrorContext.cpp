#include "sdl2_compat.hpp"

#include "CMyFont.hpp"
#include "GameErrorContext.hpp"
#include "GamePaths.hpp"
#include "thprac_th06.h"
#include <string>
#include <vector>
#include <stdio.h>

namespace th06
{
DIFFABLE_STATIC(GameErrorContext, g_GameErrorContext)
DIFFABLE_STATIC(CMyFont, g_CMyFont)

namespace
{
bool IsPreservedRuntimeLogLine(const char *line)
{
    if (!THPrac::TH06::THPracIsDebugLogEnabled())
    {
        return false;
    }

    return strncmp(line, "[PortableValidationTrace]", 25) == 0 || strncmp(line, "[PortableValidation]", 20) == 0 ||
           strncmp(line, "[PortableValidationDump]", 24) == 0 || strncmp(line, "[PortableLoadTrial]", 19) == 0 ||
           strncmp(line, "[PortableSaveTrial]", 19) == 0 || strncmp(line, "[PortableRestore]", 17) == 0 ||
           strncmp(line, "[PortablePostVerifyDiff]", 24) == 0 ||
           strncmp(line, "[QuickSnapshot]", 16) == 0 || strncmp(line, "[Watchdog]", 10) == 0;
}

std::vector<std::string> ReadPreservedRuntimeLogLines(const char *path)
{
    std::vector<std::string> lines;
    FILE *file = fopen(path, "rt");
    if (file == NULL)
    {
        return lines;
    }

    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL)
    {
        if (IsPreservedRuntimeLogLine(line))
        {
            lines.emplace_back(line);
        }
    }

    fclose(file);
    return lines;
}
} // namespace

const char *GameErrorContext::Log(GameErrorContext *ctx, const char *fmt, ...)
{
    char tmpBuffer[2048];
    size_t tmpBufferSize;
    va_list args;

    va_start(args, fmt);
    _vsnprintf(tmpBuffer, sizeof(tmpBuffer) - 1, fmt, args);
    tmpBuffer[sizeof(tmpBuffer) - 1] = '\0';

    tmpBufferSize = strlen(tmpBuffer);

    if (ctx->m_BufferEnd + tmpBufferSize < &ctx->m_Buffer[sizeof(ctx->m_Buffer) - 1])
    {
        strcpy(ctx->m_BufferEnd, tmpBuffer);

        ctx->m_BufferEnd += tmpBufferSize;
        *ctx->m_BufferEnd = '\0';
    }

    va_end(args);

    return fmt;
}

const char *GameErrorContext::Fatal(GameErrorContext *ctx, const char *fmt, ...)
{
    char tmpBuffer[2048];
    size_t tmpBufferSize;
    va_list args;

    va_start(args, fmt);
    _vsnprintf(tmpBuffer, sizeof(tmpBuffer) - 1, fmt, args);
    tmpBuffer[sizeof(tmpBuffer) - 1] = '\0';

    tmpBufferSize = strlen(tmpBuffer);

    if (ctx->m_BufferEnd + tmpBufferSize < &ctx->m_Buffer[sizeof(ctx->m_Buffer) - 1])
    {
        strcpy(ctx->m_BufferEnd, tmpBuffer);

        ctx->m_BufferEnd += tmpBufferSize;
        *ctx->m_BufferEnd = '\0';
    }

    va_end(args);

    ctx->m_ShowMessageBox = true;

    return fmt;
}

void GameErrorContext::Flush()
{
    FILE *logFile;

    if (m_BufferEnd != m_Buffer)
    {
        GameErrorContext::Log(this, TH_ERR_LOGGER_END);

        if (m_ShowMessageBox)
        {
#ifdef _WIN32
            MessageBoxA(NULL, m_Buffer, "log", MB_ICONERROR);
#else
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "log", m_Buffer, NULL);
#endif
        }

        char resolvedLogPath[512];
        GamePaths::Resolve(resolvedLogPath, sizeof(resolvedLogPath), "./log.txt");
        GamePaths::EnsureParentDir(resolvedLogPath);
        const std::vector<std::string> preservedRuntimeLines = ReadPreservedRuntimeLogLines(resolvedLogPath);
        logFile = fopen(resolvedLogPath, "wt");
        if (logFile != NULL)
        {
            fprintf(logFile, "%s", m_Buffer);
            for (const std::string &line : preservedRuntimeLines)
            {
                if (strstr(m_Buffer, line.c_str()) == NULL)
                {
                    fprintf(logFile, "%s", line.c_str());
                }
            }
            fclose(logFile);
        }
    }
}
}; // namespace th06
