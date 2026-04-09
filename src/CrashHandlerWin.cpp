// =============================================================================
// CrashHandlerWin.cpp — Windows crash handler (SEH unhandled exception filter)
//
// Captures unhandled exceptions via SetUnhandledExceptionFilter and writes
// a MiniDump (.dmp) plus a text crash report alongside it.
//
// This is complementary to WatchdogWin (which detects freezes/hangs).
// CrashHandler catches actual crashes (access violations, stack overflows, etc.)
// =============================================================================

#ifdef _WIN32

#include "CrashHandler.hpp"
#include "GamePaths.hpp"

#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>

#include <cstdio>
#include <cstring>

namespace th06::CrashHandler
{
namespace
{

using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);

static LPTOP_LEVEL_EXCEPTION_FILTER s_previousFilter = nullptr;
static HMODULE s_dbghelpModule = nullptr;
static MiniDumpWriteDumpFn s_miniDumpWriteDump = nullptr;
static bool s_installed = false;

// Guard against recursive exceptions.
static volatile LONG s_inHandler = 0;

// ============================================================================
// Helpers
// ============================================================================

static bool GetExeDirectory(char *out, size_t outSize)
{
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;
    char *slash = strrchr(exePath, '\\');
    if (!slash)
        return false;
    *slash = '\0';
    std::snprintf(out, outSize, "%s", exePath);
    return true;
}

static bool EnsureCrashDirectory(char *out, size_t outSize)
{
    char exeDir[MAX_PATH];
    if (!GetExeDirectory(exeDir, sizeof(exeDir)))
        return false;

    std::snprintf(out, outSize, "%s\\Crash", exeDir);
    if (!CreateDirectoryA(out, nullptr))
    {
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    return true;
}

static void BuildTimestamp(char *out, size_t outSize)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(out, outSize, "%04u%02u%02u_%02u%02u%02u_%03u",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

// ============================================================================
// Exception code to string
// ============================================================================

static const char *ExceptionCodeName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_OVERFLOW:
        return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_UNDERFLOW:
        return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:
        return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    default:
        return "UNKNOWN_EXCEPTION";
    }
}

// ============================================================================
// Write text crash report
// ============================================================================

static void WriteTextReport(const char *path, EXCEPTION_POINTERS *ep)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;

    fprintf(f, "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n");
    fprintf(f, "th06_sdl crash report (Windows)\n");
    fprintf(f, "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n\n");

    // Timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "Timestamp: %04u-%02u-%02u %02u:%02u:%02u.%03u\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    fprintf(f, "PID: %lu\n", GetCurrentProcessId());
    fprintf(f, "TID: %lu\n\n", GetCurrentThreadId());

    if (ep && ep->ExceptionRecord)
    {
        const auto *rec = ep->ExceptionRecord;

        fprintf(f, "--- Exception ---\n");
        fprintf(f, "  Code: 0x%08lX (%s)\n", rec->ExceptionCode, ExceptionCodeName(rec->ExceptionCode));
        fprintf(f, "  Address: 0x%p\n", rec->ExceptionAddress);
        fprintf(f, "  Flags: 0x%08lX\n", rec->ExceptionFlags);

        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        {
            const char *op = rec->ExceptionInformation[0] == 0 ? "reading" : (rec->ExceptionInformation[0] == 1 ? "writing" : "executing");
            fprintf(f, "  Access violation %s address: 0x%p\n",
                    op, reinterpret_cast<void *>(rec->ExceptionInformation[1]));
        }
        fprintf(f, "\n");
    }

    // Registers
    if (ep && ep->ContextRecord)
    {
        const auto *ctx = ep->ContextRecord;
        fprintf(f, "--- Registers ---\n");

#ifdef _M_IX86
        fprintf(f, "  EAX: 0x%08lX  EBX: 0x%08lX  ECX: 0x%08lX  EDX: 0x%08lX\n",
                ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
        fprintf(f, "  ESI: 0x%08lX  EDI: 0x%08lX  EBP: 0x%08lX  ESP: 0x%08lX\n",
                ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
        fprintf(f, "  EIP: 0x%08lX  EFLAGS: 0x%08lX\n", ctx->Eip, ctx->EFlags);
        fprintf(f, "  CS:  0x%04lX  DS: 0x%04lX  SS: 0x%04lX\n", ctx->SegCs, ctx->SegDs, ctx->SegSs);
#elif defined(_M_X64)
        fprintf(f, "  RAX: 0x%016llX  RBX: 0x%016llX\n", ctx->Rax, ctx->Rbx);
        fprintf(f, "  RCX: 0x%016llX  RDX: 0x%016llX\n", ctx->Rcx, ctx->Rdx);
        fprintf(f, "  RSI: 0x%016llX  RDI: 0x%016llX\n", ctx->Rsi, ctx->Rdi);
        fprintf(f, "  RBP: 0x%016llX  RSP: 0x%016llX\n", ctx->Rbp, ctx->Rsp);
        fprintf(f, "  R8:  0x%016llX  R9:  0x%016llX\n", ctx->R8, ctx->R9);
        fprintf(f, "  R10: 0x%016llX  R11: 0x%016llX\n", ctx->R10, ctx->R11);
        fprintf(f, "  R12: 0x%016llX  R13: 0x%016llX\n", ctx->R12, ctx->R13);
        fprintf(f, "  R14: 0x%016llX  R15: 0x%016llX\n", ctx->R14, ctx->R15);
        fprintf(f, "  RIP: 0x%016llX  EFLAGS: 0x%08lX\n", ctx->Rip, ctx->EFlags);
#endif
        fprintf(f, "\n");
    }

    // Stack trace via CaptureStackBackTrace
    fprintf(f, "--- Backtrace ---\n");
    void *frames[64];
    USHORT frameCount = CaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i)
    {
        fprintf(f, "  #%02u  0x%p\n", i, frames[i]);
    }
    fprintf(f, "\n");

    // System info
    fprintf(f, "--- System Info ---\n");
    OSVERSIONINFOEXA ovi;
    memset(&ovi, 0, sizeof(ovi));
    ovi.dwOSVersionInfoSize = sizeof(ovi);
#pragma warning(push)
#pragma warning(disable : 4996) // GetVersionEx deprecated
    GetVersionExA(reinterpret_cast<OSVERSIONINFOA *>(&ovi));
#pragma warning(pop)
    fprintf(f, "  Windows version: %lu.%lu.%lu\n",
            ovi.dwMajorVersion, ovi.dwMinorVersion, ovi.dwBuildNumber);

#ifdef _M_IX86
    fprintf(f, "  Architecture: x86\n");
#elif defined(_M_X64)
    fprintf(f, "  Architecture: x64\n");
#endif
    fprintf(f, "\n");

    fclose(f);
}

// ============================================================================
// Write MiniDump
// ============================================================================

static bool WriteMiniDump(const char *path, EXCEPTION_POINTERS *ep)
{
    if (!s_miniDumpWriteDump)
        return false;

    HANDLE dumpFile = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpFile == INVALID_HANDLE_VALUE)
        return false;

    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithHandleData |
        MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpWithThreadInfo | MiniDumpWithFullMemoryInfo |
        MiniDumpScanMemory);

    BOOL ok = s_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                  dumpFile, dumpType,
                                  ep ? &mei : nullptr, nullptr, nullptr);

    CloseHandle(dumpFile);

    if (!ok)
    {
        DeleteFileA(path);
        return false;
    }

    return true;
}

// ============================================================================
// Unhandled exception filter
// ============================================================================

static LONG WINAPI CrashExceptionFilter(EXCEPTION_POINTERS *ep)
{
    // Guard against recursive crashes.
    if (InterlockedCompareExchange(&s_inHandler, 1, 0) != 0)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    char crashDir[MAX_PATH];
    if (!EnsureCrashDirectory(crashDir, sizeof(crashDir)))
    {
        InterlockedExchange(&s_inHandler, 0);
        if (s_previousFilter)
            return s_previousFilter(ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    char timestamp[64];
    BuildTimestamp(timestamp, sizeof(timestamp));

    // Write MiniDump
    char dmpPath[MAX_PATH];
    std::snprintf(dmpPath, sizeof(dmpPath), "%s\\crash_%s_pid%lu.dmp",
                  crashDir, timestamp, GetCurrentProcessId());
    WriteMiniDump(dmpPath, ep);

    // Write text report
    char txtPath[MAX_PATH];
    std::snprintf(txtPath, sizeof(txtPath), "%s\\crash_%s_pid%lu.txt",
                  crashDir, timestamp, GetCurrentProcessId());
    WriteTextReport(txtPath, ep);

    // Log to debug output
    char msg[512];
    std::snprintf(msg, sizeof(msg),
                  "[CrashHandler] Crash report written to:\n  %s\n  %s\n",
                  dmpPath, txtPath);
    OutputDebugStringA(msg);

    // Also write to log file
    char logPath[512];
    GamePaths::Resolve(logPath, sizeof(logPath), "./log.txt");
    FILE *logFile = fopen(logPath, "a");
    if (logFile)
    {
        fprintf(logFile, "%s", msg);
        fclose(logFile);
    }

    InterlockedExchange(&s_inHandler, 0);

    // Chain to previous filter (e.g. WatchdogWin might have its own).
    if (s_previousFilter)
        return s_previousFilter(ep);

    return EXCEPTION_CONTINUE_SEARCH;
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

void Init()
{
    if (s_installed)
        return;

    // Load DbgHelp.dll for minidump writing.
    if (!s_dbghelpModule)
    {
        s_dbghelpModule = LoadLibraryA("DbgHelp.dll");
        if (s_dbghelpModule)
        {
            s_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
                GetProcAddress(s_dbghelpModule, "MiniDumpWriteDump"));
        }
    }

    s_previousFilter = SetUnhandledExceptionFilter(CrashExceptionFilter);
    s_installed = true;

    // Ensure crash directory exists early.
    char crashDir[MAX_PATH];
    if (EnsureCrashDirectory(crashDir, sizeof(crashDir)))
    {
        SDL_Log("CrashHandler: initialized (crash reports -> %s)", crashDir);
    }
}

void Shutdown()
{
    if (!s_installed)
        return;

    SetUnhandledExceptionFilter(s_previousFilter);
    s_previousFilter = nullptr;
    s_installed = false;
}

} // namespace th06::CrashHandler

#endif // _WIN32
