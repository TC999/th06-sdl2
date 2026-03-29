#include "WatchdogWin.hpp"

#ifdef _WIN32

#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>

#include "GamePaths.hpp"
#include "thprac_th06.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

namespace th06::WatchdogWin
{
namespace
{
using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);

struct WatchdogState
{
    std::atomic<bool> running{false};
    std::atomic<ULONGLONG> lastHeartbeatMs{0};
    std::atomic<unsigned long long> heartbeatSerial{0};
    std::atomic<unsigned long long> dumpedSerial{0};
    std::atomic<unsigned long long> manualRequestSerial{0};
    std::atomic<unsigned long long> manualDumpedSerial{0};
    std::atomic<unsigned long long> manualRequestHeartbeatSerial{0};
    std::atomic<ULONGLONG> manualRequestMs{0};
    std::thread worker;
    HANDLE mainThreadHandle = NULL;
    DWORD mainThreadId = 0;
    HMODULE dbghelpModule = NULL;
    MiniDumpWriteDumpFn miniDumpWriteDump = nullptr;
    HANDLE wakeEvent = NULL;
};

WatchdogState g_WatchdogState;

constexpr ULONGLONG kHeartbeatTimeoutMs = 5000;
constexpr DWORD kPollIntervalMs = 500;
constexpr DWORD kManualRequestPollMs = 10;
constexpr ULONGLONG kManualRequestDelayMs = 100;

bool GetExecutableDirectory(char *outBuf, size_t outBufSize)
{
    char exePath[MAX_PATH];
    const DWORD exePathLen = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (exePathLen == 0 || exePathLen >= MAX_PATH)
    {
        return false;
    }

    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash == nullptr)
    {
        return false;
    }

    *lastSlash = '\0';
    std::snprintf(outBuf, outBufSize, "%s", exePath);
    return true;
}

bool EnsureCrashDirectory(char *outBuf, size_t outBufSize)
{
    char exeDir[MAX_PATH];
    if (!GetExecutableDirectory(exeDir, sizeof(exeDir)))
    {
        return false;
    }

    std::snprintf(outBuf, outBufSize, "%s\\Crash", exeDir);
    if (CreateDirectoryA(outBuf, NULL) == 0)
    {
        const DWORD lastError = GetLastError();
        if (lastError != ERROR_ALREADY_EXISTS)
        {
            return false;
        }
    }

    return true;
}

void AppendLogLine(const char *line)
{
    char resolvedLogPath[512];
    GamePaths::Resolve(resolvedLogPath, sizeof(resolvedLogPath), "./log.txt");
    GamePaths::EnsureParentDir(resolvedLogPath);

    HANDLE logFile = CreateFileA(resolvedLogPath, FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
    if (logFile == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD bytesWritten = 0;
    WriteFile(logFile, line, (DWORD)std::strlen(line), &bytesWritten, NULL);
    WriteFile(logFile, "\r\n", 2, &bytesWritten, NULL);
    CloseHandle(logFile);
}

void LogWatchdog(const char *fmt, ...)
{
    if (!THPrac::TH06::THPracIsDebugLogEnabled())
    {
        return;
    }

    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    AppendLogLine(line);
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}

bool BuildDumpPath(char *outBuf, size_t outBufSize, const char *prefix)
{
    char crashDir[MAX_PATH];
    if (!EnsureCrashDirectory(crashDir, sizeof(crashDir)))
    {
        return false;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(outBuf, outBufSize, "%s\\%s_%04u%02u%02u_%02u%02u%02u_%03u_pid%lu.dmp", crashDir, prefix,
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  GetCurrentProcessId());
    return true;
}

enum class DumpReason
{
    Watchdog,
    Manual,
};

const char *DumpReasonName(DumpReason reason)
{
    return reason == DumpReason::Manual ? "manual" : "watchdog";
}

const char *DumpFilePrefix(DumpReason reason)
{
    return reason == DumpReason::Manual ? "th06_manual" : "th06_watchdog";
}

void WriteDump(DumpReason reason, unsigned long long serial, ULONGLONG stalledMs)
{
    if (g_WatchdogState.miniDumpWriteDump == nullptr)
    {
        LogWatchdog("[Watchdog] event=dump-skipped trigger=%s reason=dbghelp-unavailable serial=%llu stalled_ms=%llu",
                    DumpReasonName(reason), serial, stalledMs);
        return;
    }

    char dumpPath[MAX_PATH];
    if (!BuildDumpPath(dumpPath, sizeof(dumpPath), DumpFilePrefix(reason)))
    {
        LogWatchdog(
            "[Watchdog] event=dump-skipped trigger=%s reason=crash-dir-create-failed serial=%llu stalled_ms=%llu",
            DumpReasonName(reason), serial, stalledMs);
        return;
    }

    HANDLE dumpFile = CreateFileA(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  NULL);
    if (dumpFile == INVALID_HANDLE_VALUE)
    {
        LogWatchdog("[Watchdog] event=dump-failed trigger=%s reason=create-file error=%lu serial=%llu stalled_ms=%llu",
                    DumpReasonName(reason), GetLastError(), serial, stalledMs);
        return;
    }

    bool mainThreadSuspended = false;
    if (g_WatchdogState.mainThreadHandle != NULL)
    {
        const DWORD suspendResult = SuspendThread(g_WatchdogState.mainThreadHandle);
        mainThreadSuspended = suspendResult != DWORD(-1);
    }

    const MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                                   MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory |
                                                   MiniDumpWithThreadInfo | MiniDumpWithFullMemoryInfo |
                                                   MiniDumpScanMemory);

    const BOOL dumpOk =
        g_WatchdogState.miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile, dumpType, NULL, NULL,
                                          NULL);
    const DWORD dumpError = dumpOk ? ERROR_SUCCESS : GetLastError();

    if (mainThreadSuspended)
    {
        ResumeThread(g_WatchdogState.mainThreadHandle);
    }

    CloseHandle(dumpFile);

    if (!dumpOk)
    {
        DeleteFileA(dumpPath);
        LogWatchdog("[Watchdog] event=dump-failed trigger=%s reason=minidump error=%lu serial=%llu stalled_ms=%llu",
                    DumpReasonName(reason), dumpError, serial, stalledMs);
        return;
    }

    LogWatchdog("[Watchdog] event=dump-written trigger=%s serial=%llu stalled_ms=%llu path=%s",
                DumpReasonName(reason), serial, stalledMs, dumpPath);
}

bool HasPendingManualRequest()
{
    return g_WatchdogState.manualRequestSerial.load(std::memory_order_acquire) !=
           g_WatchdogState.manualDumpedSerial.load(std::memory_order_acquire);
}

void ServicePendingManualDump()
{
    const unsigned long long requestSerial = g_WatchdogState.manualRequestSerial.load(std::memory_order_acquire);
    unsigned long long dumpedSerial = g_WatchdogState.manualDumpedSerial.load(std::memory_order_acquire);
    if (requestSerial == 0 || requestSerial == dumpedSerial)
    {
        return;
    }

    const unsigned long long requestHeartbeatSerial =
        g_WatchdogState.manualRequestHeartbeatSerial.load(std::memory_order_acquire);
    const ULONGLONG requestMs = g_WatchdogState.manualRequestMs.load(std::memory_order_acquire);
    const unsigned long long currentHeartbeatSerial =
        g_WatchdogState.heartbeatSerial.load(std::memory_order_acquire);
    const ULONGLONG nowMs = GetTickCount64();

    if (currentHeartbeatSerial == requestHeartbeatSerial && nowMs >= requestMs &&
        nowMs - requestMs < kManualRequestDelayMs)
    {
        return;
    }

    if (!g_WatchdogState.manualDumpedSerial.compare_exchange_strong(dumpedSerial, requestSerial,
                                                                    std::memory_order_acq_rel,
                                                                    std::memory_order_acquire))
    {
        return;
    }

    WriteDump(DumpReason::Manual, requestSerial, 0);
}

void WatchdogLoop()
{
    LogWatchdog("[Watchdog] event=started timeout_ms=%llu poll_ms=%lu main_thread_id=%lu", kHeartbeatTimeoutMs,
                kPollIntervalMs, g_WatchdogState.mainThreadId);

    while (g_WatchdogState.running.load(std::memory_order_acquire))
    {
        const DWORD waitMs = HasPendingManualRequest() ? kManualRequestPollMs : kPollIntervalMs;
        if (g_WatchdogState.wakeEvent != NULL)
        {
            WaitForSingleObject(g_WatchdogState.wakeEvent, waitMs);
        }
        else
        {
            Sleep(waitMs);
        }
        if (!g_WatchdogState.running.load(std::memory_order_acquire))
        {
            break;
        }

        ServicePendingManualDump();

        const ULONGLONG lastHeartbeatMs = g_WatchdogState.lastHeartbeatMs.load(std::memory_order_acquire);
        if (lastHeartbeatMs == 0)
        {
            continue;
        }

        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs < lastHeartbeatMs)
        {
            g_WatchdogState.lastHeartbeatMs.store(nowMs, std::memory_order_release);
            continue;
        }

        const ULONGLONG stalledMs = nowMs - lastHeartbeatMs;
        if (stalledMs < kHeartbeatTimeoutMs)
        {
            continue;
        }

        const unsigned long long serial = g_WatchdogState.heartbeatSerial.load(std::memory_order_acquire);
        if (serial == 0)
        {
            continue;
        }

        unsigned long long dumpedSerial = g_WatchdogState.dumpedSerial.load(std::memory_order_acquire);
        if (dumpedSerial == serial)
        {
            continue;
        }

        if (!g_WatchdogState.dumpedSerial.compare_exchange_strong(dumpedSerial, serial, std::memory_order_acq_rel,
                                                                  std::memory_order_acquire))
        {
            continue;
        }

        WriteDump(DumpReason::Watchdog, serial, stalledMs);
    }

    LogWatchdog("[Watchdog] event=stopped");
}
} // namespace

void Init()
{
    if (g_WatchdogState.running.load(std::memory_order_acquire))
    {
        return;
    }

    g_WatchdogState.mainThreadId = GetCurrentThreadId();
    if (g_WatchdogState.mainThreadHandle != NULL)
    {
        CloseHandle(g_WatchdogState.mainThreadHandle);
        g_WatchdogState.mainThreadHandle = NULL;
    }

    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_WatchdogState.mainThreadHandle, 0,
                    FALSE, DUPLICATE_SAME_ACCESS);

    if (g_WatchdogState.dbghelpModule == NULL)
    {
        g_WatchdogState.dbghelpModule = LoadLibraryA("DbgHelp.dll");
        if (g_WatchdogState.dbghelpModule != NULL)
        {
            g_WatchdogState.miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
                GetProcAddress(g_WatchdogState.dbghelpModule, "MiniDumpWriteDump"));
        }
    }

    if (g_WatchdogState.wakeEvent == NULL)
    {
        g_WatchdogState.wakeEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    }

    g_WatchdogState.lastHeartbeatMs.store(GetTickCount64(), std::memory_order_release);
    g_WatchdogState.heartbeatSerial.store(1, std::memory_order_release);
    g_WatchdogState.dumpedSerial.store(0, std::memory_order_release);
    g_WatchdogState.manualRequestSerial.store(0, std::memory_order_release);
    g_WatchdogState.manualDumpedSerial.store(0, std::memory_order_release);
    g_WatchdogState.manualRequestHeartbeatSerial.store(0, std::memory_order_release);
    g_WatchdogState.manualRequestMs.store(0, std::memory_order_release);
    g_WatchdogState.running.store(true, std::memory_order_release);

    try
    {
        g_WatchdogState.worker = std::thread(WatchdogLoop);
    }
    catch (...)
    {
        g_WatchdogState.running.store(false, std::memory_order_release);
        LogWatchdog("[Watchdog] event=start-failed reason=thread-create");
    }
}

void Shutdown()
{
    if (!g_WatchdogState.running.load(std::memory_order_acquire))
    {
        return;
    }

    g_WatchdogState.running.store(false, std::memory_order_release);
    if (g_WatchdogState.wakeEvent != NULL)
    {
        SetEvent(g_WatchdogState.wakeEvent);
    }
    if (g_WatchdogState.worker.joinable())
    {
        g_WatchdogState.worker.join();
    }

    if (g_WatchdogState.mainThreadHandle != NULL)
    {
        CloseHandle(g_WatchdogState.mainThreadHandle);
        g_WatchdogState.mainThreadHandle = NULL;
    }

    if (g_WatchdogState.wakeEvent != NULL)
    {
        CloseHandle(g_WatchdogState.wakeEvent);
        g_WatchdogState.wakeEvent = NULL;
    }

    if (g_WatchdogState.dbghelpModule != NULL)
    {
        FreeLibrary(g_WatchdogState.dbghelpModule);
        g_WatchdogState.dbghelpModule = NULL;
        g_WatchdogState.miniDumpWriteDump = nullptr;
    }
}

void TickHeartbeat()
{
    if (!g_WatchdogState.running.load(std::memory_order_acquire))
    {
        return;
    }

    g_WatchdogState.lastHeartbeatMs.store(GetTickCount64(), std::memory_order_release);
    g_WatchdogState.heartbeatSerial.fetch_add(1, std::memory_order_acq_rel);
}

bool RequestManualDump()
{
    if (!g_WatchdogState.running.load(std::memory_order_acquire))
    {
        return false;
    }

    const unsigned long long queuedSerial = g_WatchdogState.manualRequestSerial.load(std::memory_order_acquire);
    const unsigned long long dumpedSerial = g_WatchdogState.manualDumpedSerial.load(std::memory_order_acquire);
    if (queuedSerial != dumpedSerial)
    {
        return false;
    }

    const unsigned long long nextSerial = dumpedSerial + 1;
    g_WatchdogState.manualRequestHeartbeatSerial.store(g_WatchdogState.heartbeatSerial.load(std::memory_order_acquire),
                                                       std::memory_order_release);
    g_WatchdogState.manualRequestMs.store(GetTickCount64(), std::memory_order_release);
    g_WatchdogState.manualRequestSerial.store(nextSerial, std::memory_order_release);
    if (g_WatchdogState.wakeEvent != NULL)
    {
        SetEvent(g_WatchdogState.wakeEvent);
    }

    LogWatchdog("[Watchdog] event=manual-requested serial=%llu", nextSerial);
    return true;
}
} // namespace th06::WatchdogWin

#else

namespace th06::WatchdogWin
{
void Init()
{
}

void Shutdown()
{
}

void TickHeartbeat()
{
}

bool RequestManualDump()
{
    return false;
}
} // namespace th06::WatchdogWin

#endif
