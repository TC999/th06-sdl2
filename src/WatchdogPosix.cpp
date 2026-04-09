// =============================================================================
// WatchdogPosix.cpp — POSIX watchdog (Linux + Android)
//
// Mirrors WatchdogWin: a monitoring thread watches the main thread's heartbeat.
// If the main thread doesn't call TickHeartbeat() within the timeout, the
// watchdog sends SIGUSR1 to the main thread to capture a freeze dump.
//
// Also supports manual dump requests (RequestManualDump).
//
// Freeze dumps are written to GamePaths::GetUserPath() + "crash/".
// =============================================================================

#if !defined(_WIN32)

#include "WatchdogWin.hpp"
#include "GamePaths.hpp"

#include <SDL.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <unwind.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace th06::WatchdogWin
{
namespace
{

// ============================================================================
// Configuration
// ============================================================================

constexpr unsigned long long kHeartbeatTimeoutMs = 5000;
constexpr unsigned int kPollIntervalMs = 500;
constexpr unsigned int kManualRequestPollMs = 10;
constexpr unsigned long long kManualRequestDelayMs = 100;

// ============================================================================
// Async-signal-safe write helpers (duplicated from CrashHandlerPosix for
// self-containedness — these are trivial and avoiding a shared header keeps
// compile-time coupling to zero).
// ============================================================================

static void SafeWrite(int fd, const char *s)
{
    if (fd < 0 || !s)
        return;
    size_t len = 0;
    while (s[len])
        ++len;
    while (len > 0)
    {
        ssize_t w = write(fd, s, len);
        if (w <= 0)
            break;
        s += w;
        len -= static_cast<size_t>(w);
    }
}

static void UintToStr(char *buf, size_t sz, unsigned long long val)
{
    if (!sz)
        return;
    if (val == 0)
    {
        if (sz >= 2) { buf[0] = '0'; buf[1] = '\0'; }
        else buf[0] = '\0';
        return;
    }
    char tmp[24];
    int p = 0;
    while (val > 0 && p < 23)
    {
        tmp[p++] = '0' + static_cast<char>(val % 10);
        val /= 10;
    }
    size_t i = 0;
    while (p > 0 && i + 1 < sz)
        buf[i++] = tmp[--p];
    buf[i] = '\0';
}

static void UintToHex(char *buf, size_t sz, unsigned long long val, int minW = 0)
{
    if (!sz)
        return;
    static const char hc[] = "0123456789abcdef";
    char tmp[20];
    int p = 0;
    if (val == 0) tmp[p++] = '0';
    else while (val > 0 && p < 19) { tmp[p++] = hc[val & 0xf]; val >>= 4; }
    while (p < minW && p < 19) tmp[p++] = '0';
    size_t i = 0;
    while (p > 0 && i + 1 < sz) buf[i++] = tmp[--p];
    buf[i] = '\0';
}

static void SafeWriteHex(int fd, unsigned long long val, int w = 0)
{
    SafeWrite(fd, "0x");
    char buf[20];
    UintToHex(buf, sizeof(buf), val, w);
    SafeWrite(fd, buf);
}

static void SafeWriteUint(int fd, unsigned long long val)
{
    char buf[24];
    UintToStr(buf, sizeof(buf), val);
    SafeWrite(fd, buf);
}

// ============================================================================
// Monotonic clock (async-signal-safe via VDSO on Linux/Android)
// ============================================================================

static unsigned long long NowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1000ULL +
           static_cast<unsigned long long>(ts.tv_nsec / 1000000);
}

// ============================================================================
// Stack trace capture (for SIGUSR1 handler context)
// ============================================================================

static constexpr int kMaxFrames = 64;

struct BacktraceState
{
    uintptr_t frames[kMaxFrames];
    int count;
};

static _Unwind_Reason_Code UnwindCb(struct _Unwind_Context *ctx, void *arg)
{
    auto *st = static_cast<BacktraceState *>(arg);
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && st->count < kMaxFrames)
        st->frames[st->count++] = pc;
    return _URC_NO_REASON;
}

// ============================================================================
// State
// ============================================================================

struct WatchdogState
{
    std::atomic<bool> running{false};
    std::atomic<unsigned long long> lastHeartbeatMs{0};
    std::atomic<unsigned long long> heartbeatSerial{0};
    std::atomic<unsigned long long> dumpedSerial{0};
    std::atomic<unsigned long long> manualRequestSerial{0};
    std::atomic<unsigned long long> manualDumpedSerial{0};
    std::atomic<unsigned long long> manualRequestHeartbeatSerial{0};
    std::atomic<unsigned long long> manualRequestMs{0};
    pthread_t workerThread;
    bool workerCreated = false;
    pid_t mainTid = 0;
    int pipeFd[2] = {-1, -1}; // pipe for wake-up
};

static WatchdogState g_State;

// Freeze dump state shared between watcher thread and SIGUSR1 handler.
// The watcher sets the reason, sends SIGUSR1, and the handler writes the dump.
enum class DumpReason
{
    None,
    Watchdog,
    Manual,
};

static volatile sig_atomic_t s_pendingDumpReason = 0; // DumpReason cast
static volatile unsigned long long s_pendingSerial = 0;
static volatile unsigned long long s_pendingStalledMs = 0;

// ============================================================================
// Crash directory (pre-computed at Init)
// ============================================================================

static char s_crashDir[512] = "";

static bool EnsureCrashDir()
{
    const char *userPath = GamePaths::GetUserPath();
    int len = 0;
    while (userPath[len] && len < 480)
    {
        s_crashDir[len] = userPath[len];
        ++len;
    }
    const char *suffix = "crash/";
    int si = 0;
    while (suffix[si] && len < 500)
        s_crashDir[len++] = suffix[si++];
    s_crashDir[len] = '\0';

    if (mkdir(s_crashDir, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

// ============================================================================
// SIGUSR1 handler — runs in main thread context to capture its stack
// ============================================================================

// Saved previous SIGUSR1 action.
static struct sigaction s_oldSigusr1Action;
static bool s_sigusr1Installed = false;

static void FreezeDumpHandler(int /*signo*/, siginfo_t * /*info*/, void * /*ucontext*/)
{
    DumpReason reason = static_cast<DumpReason>(s_pendingDumpReason);
    if (reason == DumpReason::None)
        return;

    unsigned long long serial = s_pendingSerial;
    unsigned long long stalledMs = s_pendingStalledMs;

    // Build output path
    char path[512];
    {
        const char *prefix = (reason == DumpReason::Manual) ? "freeze_manual_" : "freeze_watchdog_";
        // Read uptime for unique name (async-signal-safe via /proc/uptime)
        unsigned long long uptime = 0;
        {
            int ufd = open("/proc/uptime", O_RDONLY);
            if (ufd >= 0)
            {
                char ubuf[64];
                ssize_t n = read(ufd, ubuf, sizeof(ubuf) - 1);
                close(ufd);
                if (n > 0)
                {
                    ubuf[n] = '\0';
                    for (int i = 0; ubuf[i] >= '0' && ubuf[i] <= '9'; ++i)
                        uptime = uptime * 10 + static_cast<unsigned long long>(ubuf[i] - '0');
                }
            }
        }
        char uptimeStr[24], pidStr[16];
        UintToStr(uptimeStr, sizeof(uptimeStr), uptime);
        UintToStr(pidStr, sizeof(pidStr), static_cast<unsigned long long>(getpid()));

        int pos = 0;
        const char *parts[] = {s_crashDir, prefix, uptimeStr, "s_pid", pidStr, ".txt", nullptr};
        for (int pi = 0; parts[pi]; ++pi)
        {
            const char *p = parts[pi];
            while (*p && pos + 1 < (int)sizeof(path))
                path[pos++] = *p++;
        }
        path[pos] = '\0';
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        fd = STDERR_FILENO;

    SafeWrite(fd, "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n");
    SafeWrite(fd, "th06_sdl freeze report\n");
    SafeWrite(fd, "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n\n");

    SafeWrite(fd, "Reason: ");
    SafeWrite(fd, (reason == DumpReason::Manual) ? "manual dump request" : "watchdog heartbeat timeout");
    SafeWrite(fd, "\n");

    SafeWrite(fd, "Stalled ms: ");
    SafeWriteUint(fd, stalledMs);
    SafeWrite(fd, "\nSerial: ");
    SafeWriteUint(fd, serial);
    SafeWrite(fd, "\nPID: ");
    SafeWriteUint(fd, static_cast<unsigned long long>(getpid()));
    SafeWrite(fd, "\nTID: ");
    SafeWriteUint(fd, static_cast<unsigned long long>(static_cast<pid_t>(syscall(SYS_gettid))));
    SafeWrite(fd, "\n\n");

    // Backtrace of the main thread (this handler runs in main thread context)
    SafeWrite(fd, "--- Main thread backtrace ---\n");
    SafeWrite(fd, "Note: Use addr2line or ndk-stack to resolve addresses.\n\n");

    BacktraceState bt;
    bt.count = 0;
    _Unwind_Backtrace(UnwindCb, &bt);

    for (int i = 0; i < bt.count; ++i)
    {
        SafeWrite(fd, "  #");
        char idx[8];
        UintToStr(idx, sizeof(idx), static_cast<unsigned long long>(i));
        if (i < 10) SafeWrite(fd, "0");
        SafeWrite(fd, idx);
        SafeWrite(fd, "  pc ");
        SafeWriteHex(fd, bt.frames[i], sizeof(void *) * 2);
        SafeWrite(fd, "\n");
    }

    if (fd != STDERR_FILENO)
    {
        close(fd);

#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_WARN, "th06_watchdog",
                            "Freeze dump written to: %s (stalled %llums)", path, stalledMs);
#endif
    }

    // Clear pending state.
    s_pendingDumpReason = 0;
}

// ============================================================================
// Watchdog thread
// ============================================================================

static void WakeWorker()
{
    if (g_State.pipeFd[1] >= 0)
    {
        char c = 'w';
        (void)write(g_State.pipeFd[1], &c, 1);
    }
}

static void WatchdogSleep(unsigned int ms)
{
    if (g_State.pipeFd[0] >= 0)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_State.pipeFd[0], &rfds);
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        if (select(g_State.pipeFd[0] + 1, &rfds, nullptr, nullptr, &tv) > 0)
        {
            // Drain pipe
            char drain[16];
            (void)read(g_State.pipeFd[0], drain, sizeof(drain));
        }
    }
    else
    {
        usleep(static_cast<useconds_t>(ms) * 1000);
    }
}

static bool HasPendingManualRequest()
{
    return g_State.manualRequestSerial.load(std::memory_order_acquire) !=
           g_State.manualDumpedSerial.load(std::memory_order_acquire);
}

static void SendDumpSignal(DumpReason reason, unsigned long long serial, unsigned long long stalledMs)
{
    s_pendingDumpReason = static_cast<int>(reason);
    s_pendingSerial = serial;
    s_pendingStalledMs = stalledMs;

    // Send SIGUSR1 to the main thread. The handler runs in main thread context
    // and captures its stack. tgkill is async-signal-safe.
    syscall(SYS_tgkill, getpid(), g_State.mainTid, SIGUSR1);

    // Give the handler time to complete (best-effort wait).
    usleep(50000); // 50ms
}

static void ServiceManualDump()
{
    unsigned long long reqSerial = g_State.manualRequestSerial.load(std::memory_order_acquire);
    unsigned long long dumpedSerial = g_State.manualDumpedSerial.load(std::memory_order_acquire);
    if (reqSerial == 0 || reqSerial == dumpedSerial)
        return;

    unsigned long long reqHbSerial = g_State.manualRequestHeartbeatSerial.load(std::memory_order_acquire);
    unsigned long long reqMs = g_State.manualRequestMs.load(std::memory_order_acquire);
    unsigned long long curHbSerial = g_State.heartbeatSerial.load(std::memory_order_acquire);
    unsigned long long nowMs = NowMs();

    if (curHbSerial == reqHbSerial && nowMs >= reqMs && nowMs - reqMs < kManualRequestDelayMs)
        return;

    if (!g_State.manualDumpedSerial.compare_exchange_strong(dumpedSerial, reqSerial,
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_acquire))
        return;

    SendDumpSignal(DumpReason::Manual, reqSerial, 0);
}

static void *WatchdogLoop(void * /*arg*/)
{
    SDL_Log("[Watchdog] started (timeout=%llums, poll=%ums, mainTid=%d)",
            kHeartbeatTimeoutMs, kPollIntervalMs, g_State.mainTid);

    while (g_State.running.load(std::memory_order_acquire))
    {
        unsigned int sleepMs = HasPendingManualRequest() ? kManualRequestPollMs : kPollIntervalMs;
        WatchdogSleep(sleepMs);

        if (!g_State.running.load(std::memory_order_acquire))
            break;

        ServiceManualDump();

        unsigned long long lastHb = g_State.lastHeartbeatMs.load(std::memory_order_acquire);
        if (lastHb == 0)
            continue;

        unsigned long long now = NowMs();
        if (now < lastHb)
        {
            g_State.lastHeartbeatMs.store(now, std::memory_order_release);
            continue;
        }

        unsigned long long stalled = now - lastHb;
        if (stalled < kHeartbeatTimeoutMs)
            continue;

        unsigned long long serial = g_State.heartbeatSerial.load(std::memory_order_acquire);
        if (serial == 0)
            continue;

        unsigned long long dumpedSerial = g_State.dumpedSerial.load(std::memory_order_acquire);
        if (dumpedSerial == serial)
            continue;

        if (!g_State.dumpedSerial.compare_exchange_strong(dumpedSerial, serial,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
            continue;

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[Watchdog] heartbeat timeout! serial=%llu stalled=%llums",
                    serial, stalled);

        SendDumpSignal(DumpReason::Watchdog, serial, stalled);
    }

    SDL_Log("[Watchdog] stopped");
    return nullptr;
}

} // anonymous namespace

// ============================================================================
// Public API (same interface as WatchdogWin)
// ============================================================================

void Init()
{
    if (g_State.running.load(std::memory_order_acquire))
        return;

    g_State.mainTid = static_cast<pid_t>(syscall(SYS_gettid));

    // Pre-compute crash directory.
    EnsureCrashDir();

    // Install SIGUSR1 handler for freeze dumps.
    struct sigaction sa = {};
    sa.sa_sigaction = FreezeDumpHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, &s_oldSigusr1Action) == 0)
    {
        s_sigusr1Installed = true;
    }
    else
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[Watchdog] failed to install SIGUSR1 handler: %s", strerror(errno));
    }

    // Create wake-up pipe (non-blocking write end).
    if (pipe(g_State.pipeFd) == 0)
    {
        int flags = fcntl(g_State.pipeFd[1], F_GETFL);
        if (flags >= 0)
            fcntl(g_State.pipeFd[1], F_SETFL, flags | O_NONBLOCK);
    }

    g_State.lastHeartbeatMs.store(NowMs(), std::memory_order_release);
    g_State.heartbeatSerial.store(1, std::memory_order_release);
    g_State.dumpedSerial.store(0, std::memory_order_release);
    g_State.manualRequestSerial.store(0, std::memory_order_release);
    g_State.manualDumpedSerial.store(0, std::memory_order_release);
    g_State.running.store(true, std::memory_order_release);

    int err = pthread_create(&g_State.workerThread, nullptr, WatchdogLoop, nullptr);
    if (err != 0)
    {
        g_State.running.store(false, std::memory_order_release);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[Watchdog] failed to create thread: %s", strerror(err));
        return;
    }

    g_State.workerCreated = true;
    SDL_Log("[Watchdog] initialized (crash dir: %s)", s_crashDir);
}

void Shutdown()
{
    if (!g_State.running.load(std::memory_order_acquire))
        return;

    g_State.running.store(false, std::memory_order_release);
    WakeWorker();

    if (g_State.workerCreated)
    {
        pthread_join(g_State.workerThread, nullptr);
        g_State.workerCreated = false;
    }

    if (g_State.pipeFd[0] >= 0)
    {
        close(g_State.pipeFd[0]);
        g_State.pipeFd[0] = -1;
    }
    if (g_State.pipeFd[1] >= 0)
    {
        close(g_State.pipeFd[1]);
        g_State.pipeFd[1] = -1;
    }

    // Restore original SIGUSR1 handler.
    if (s_sigusr1Installed)
    {
        sigaction(SIGUSR1, &s_oldSigusr1Action, nullptr);
        s_sigusr1Installed = false;
    }
}

void TickHeartbeat()
{
    g_State.lastHeartbeatMs.store(NowMs(), std::memory_order_release);
    g_State.heartbeatSerial.fetch_add(1, std::memory_order_release);
}

bool RequestManualDump()
{
    if (!g_State.running.load(std::memory_order_acquire))
        return false;

    unsigned long long serial = g_State.manualRequestSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_State.manualRequestHeartbeatSerial.store(
        g_State.heartbeatSerial.load(std::memory_order_acquire),
        std::memory_order_release);
    g_State.manualRequestMs.store(NowMs(), std::memory_order_release);

    WakeWorker();
    return true;
}

} // namespace th06::WatchdogWin

#endif // !defined(_WIN32)
