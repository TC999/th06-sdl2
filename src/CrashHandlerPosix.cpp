// =============================================================================
// CrashHandlerPosix.cpp — POSIX crash handler (Linux + Android)
//
// Captures SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL, SIGTRAP via sigaction().
// Writes a structured crash report using only async-signal-safe functions.
// On Android, also collects device info from system properties.
// =============================================================================

#if !defined(_WIN32)

#include "CrashHandler.hpp"
#include "GamePaths.hpp"

#include <SDL.h>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

// _Unwind_Backtrace is available in both glibc and Android NDK (via libgcc/libunwind).
#include <unwind.h>

namespace th06::CrashHandler
{
namespace
{

// ============================================================================
// Async-signal-safe utilities
// ============================================================================

// All write helpers here avoid heap allocation and only use write(2).

static void SafeWrite(int fd, const char *s)
{
    if (fd < 0 || s == nullptr)
        return;
    size_t len = 0;
    while (s[len] != '\0')
        ++len;
    // write() is async-signal-safe per POSIX.
    while (len > 0)
    {
        ssize_t written = write(fd, s, len);
        if (written <= 0)
            break;
        s += written;
        len -= static_cast<size_t>(written);
    }
}

static void SafeWriteChar(int fd, char c)
{
    char buf[2] = {c, '\0'};
    SafeWrite(fd, buf);
}

// Convert an unsigned 64-bit value to decimal string.
static void UintToStr(char *buf, size_t bufSize, unsigned long long val)
{
    if (bufSize == 0)
        return;
    if (val == 0)
    {
        if (bufSize >= 2)
        {
            buf[0] = '0';
            buf[1] = '\0';
        }
        else
        {
            buf[0] = '\0';
        }
        return;
    }
    char tmp[24];
    int pos = 0;
    while (val > 0 && pos < 23)
    {
        tmp[pos++] = '0' + static_cast<char>(val % 10);
        val /= 10;
    }
    size_t i = 0;
    while (pos > 0 && i + 1 < bufSize)
    {
        buf[i++] = tmp[--pos];
    }
    buf[i] = '\0';
}

// Convert an unsigned 64-bit value to hex string (no "0x" prefix).
static void UintToHex(char *buf, size_t bufSize, unsigned long long val, int minWidth = 0)
{
    if (bufSize == 0)
        return;
    static const char hexChars[] = "0123456789abcdef";
    char tmp[20];
    int pos = 0;
    if (val == 0)
    {
        tmp[pos++] = '0';
    }
    else
    {
        while (val > 0 && pos < 19)
        {
            tmp[pos++] = hexChars[val & 0xf];
            val >>= 4;
        }
    }
    // Pad with zeros if minWidth requested.
    while (pos < minWidth && pos < 19)
    {
        tmp[pos++] = '0';
    }
    size_t i = 0;
    while (pos > 0 && i + 1 < bufSize)
    {
        buf[i++] = tmp[--pos];
    }
    buf[i] = '\0';
}

static void SafeWriteUint(int fd, unsigned long long val)
{
    char buf[24];
    UintToStr(buf, sizeof(buf), val);
    SafeWrite(fd, buf);
}

static void SafeWriteHex(int fd, unsigned long long val, int minWidth = 0)
{
    SafeWrite(fd, "0x");
    char buf[20];
    UintToHex(buf, sizeof(buf), val, minWidth);
    SafeWrite(fd, buf);
}

static void SafeWriteKV(int fd, const char *key, const char *value)
{
    SafeWrite(fd, key);
    SafeWrite(fd, ": ");
    SafeWrite(fd, value ? value : "(null)");
    SafeWriteChar(fd, '\n');
}

static void SafeWriteKVHex(int fd, const char *key, unsigned long long value, int width = 8)
{
    SafeWrite(fd, key);
    SafeWrite(fd, ": ");
    SafeWriteHex(fd, value, width);
    SafeWriteChar(fd, '\n');
}

static void SafeWriteKVUint(int fd, const char *key, unsigned long long value)
{
    SafeWrite(fd, key);
    SafeWrite(fd, ": ");
    SafeWriteUint(fd, value);
    SafeWriteChar(fd, '\n');
}

// ============================================================================
// Signal info tables
// ============================================================================

struct SignalEntry
{
    int signo;
    const char *name;
    const char *description;
};

static const SignalEntry kSignals[] = {
    {SIGSEGV, "SIGSEGV", "Segmentation fault (invalid memory access)"},
    {SIGABRT, "SIGABRT", "Abort signal"},
    {SIGFPE, "SIGFPE", "Floating-point exception"},
    {SIGBUS, "SIGBUS", "Bus error (bad memory alignment)"},
    {SIGILL, "SIGILL", "Illegal instruction"},
    {SIGTRAP, "SIGTRAP", "Trace/breakpoint trap"},
};
static constexpr int kNumSignals = sizeof(kSignals) / sizeof(kSignals[0]);

static const char *SignalName(int signo)
{
    for (int i = 0; i < kNumSignals; ++i)
    {
        if (kSignals[i].signo == signo)
            return kSignals[i].name;
    }
    return "UNKNOWN";
}

static const char *SignalDescription(int signo)
{
    for (int i = 0; i < kNumSignals; ++i)
    {
        if (kSignals[i].signo == signo)
            return kSignals[i].description;
    }
    return "Unknown signal";
}

static const char *SiCodeName(int signo, int code)
{
    // Common si_code values
    switch (code)
    {
    case SI_USER:
        return "SI_USER (kill/raise)";
    case SI_QUEUE:
        return "SI_QUEUE (sigqueue)";
    case SI_TKILL:
        return "SI_TKILL (tkill/tgkill)";
    default:
        break;
    }

    if (signo == SIGSEGV)
    {
        switch (code)
        {
        case SEGV_MAPERR:
            return "SEGV_MAPERR (address not mapped)";
        case SEGV_ACCERR:
            return "SEGV_ACCERR (invalid permissions)";
        default:
            break;
        }
    }
    else if (signo == SIGFPE)
    {
        switch (code)
        {
        case FPE_INTDIV:
            return "FPE_INTDIV (integer divide by zero)";
        case FPE_FLTDIV:
            return "FPE_FLTDIV (float divide by zero)";
        case FPE_FLTOVF:
            return "FPE_FLTOVF (float overflow)";
        case FPE_FLTUND:
            return "FPE_FLTUND (float underflow)";
        default:
            break;
        }
    }
    else if (signo == SIGBUS)
    {
        switch (code)
        {
        case BUS_ADRALN:
            return "BUS_ADRALN (invalid address alignment)";
        case BUS_ADRERR:
            return "BUS_ADRERR (nonexistent physical address)";
        default:
            break;
        }
    }

    return nullptr; // caller will print numeric code
}

// ============================================================================
// Stack unwinding via _Unwind_Backtrace
// ============================================================================

static constexpr int kMaxFrames = 64;

struct BacktraceState
{
    uintptr_t frames[kMaxFrames];
    int count;
};

static _Unwind_Reason_Code UnwindCallback(struct _Unwind_Context *context, void *arg)
{
    auto *state = static_cast<BacktraceState *>(arg);
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc != 0 && state->count < kMaxFrames)
    {
        state->frames[state->count++] = pc;
    }
    return _URC_NO_REASON;
}

static void CaptureBacktrace(BacktraceState *state)
{
    state->count = 0;
    // Note: _Unwind_Backtrace is NOT strictly async-signal-safe per POSIX, but
    // it is widely used in signal handlers in practice. Android's debuggerd,
    // Google Breakpad, and numerous production crash reporters all use it from
    // signal context. The implementation only reads stack memory and unwind
    // tables (.ARM.exidx / .eh_frame) and does not allocate.
    _Unwind_Backtrace(UnwindCallback, state);
}

static void WriteBacktrace(int fd, const BacktraceState *state)
{
    SafeWrite(fd, "\n--- Backtrace ---\n");
    SafeWrite(fd, "Note: Use addr2line or ndk-stack to resolve addresses to source locations.\n");
    SafeWrite(fd, "  ndk-stack -sym <path-to-unstripped-libmain.so> -dump <this-file>\n\n");

    for (int i = 0; i < state->count; ++i)
    {
        SafeWrite(fd, "  #");
        char idxBuf[8];
        UintToStr(idxBuf, sizeof(idxBuf), static_cast<unsigned long long>(i));
        // Pad frame number
        if (i < 10)
            SafeWrite(fd, "0");
        SafeWrite(fd, idxBuf);
        SafeWrite(fd, "  pc ");
        SafeWriteHex(fd, state->frames[i], sizeof(void *) * 2);
        SafeWriteChar(fd, '\n');
    }
    SafeWriteChar(fd, '\n');
}

// ============================================================================
// Register dump (architecture-specific)
// ============================================================================

static void WriteRegisters(int fd, const ucontext_t *uc)
{
    if (uc == nullptr)
    {
        SafeWrite(fd, "\n--- Registers: unavailable ---\n\n");
        return;
    }

    SafeWrite(fd, "\n--- Registers ---\n");

#if defined(__arm__)
    // ARM32 (armeabi-v7a)
    const auto &regs = uc->uc_mcontext;
    SafeWriteKVHex(fd, "  r0 ", regs.arm_r0, 8);
    SafeWriteKVHex(fd, "  r1 ", regs.arm_r1, 8);
    SafeWriteKVHex(fd, "  r2 ", regs.arm_r2, 8);
    SafeWriteKVHex(fd, "  r3 ", regs.arm_r3, 8);
    SafeWriteKVHex(fd, "  r4 ", regs.arm_r4, 8);
    SafeWriteKVHex(fd, "  r5 ", regs.arm_r5, 8);
    SafeWriteKVHex(fd, "  r6 ", regs.arm_r6, 8);
    SafeWriteKVHex(fd, "  r7 ", regs.arm_r7, 8);
    SafeWriteKVHex(fd, "  r8 ", regs.arm_r8, 8);
    SafeWriteKVHex(fd, "  r9 ", regs.arm_r9, 8);
    SafeWriteKVHex(fd, "  r10", regs.arm_r10, 8);
    SafeWriteKVHex(fd, "  fp ", regs.arm_fp, 8);
    SafeWriteKVHex(fd, "  ip ", regs.arm_ip, 8);
    SafeWriteKVHex(fd, "  sp ", regs.arm_sp, 8);
    SafeWriteKVHex(fd, "  lr ", regs.arm_lr, 8);
    SafeWriteKVHex(fd, "  pc ", regs.arm_pc, 8);
    SafeWriteKVHex(fd, "  cpsr", regs.arm_cpsr, 8);
#elif defined(__aarch64__)
    // ARM64
    const auto &mc = uc->uc_mcontext;
    for (int i = 0; i < 31; ++i)
    {
        char name[8];
        name[0] = ' ';
        name[1] = ' ';
        name[2] = 'x';
        if (i < 10)
        {
            name[3] = '0' + static_cast<char>(i);
            name[4] = '\0';
        }
        else
        {
            name[3] = '0' + static_cast<char>(i / 10);
            name[4] = '0' + static_cast<char>(i % 10);
            name[5] = '\0';
        }
        SafeWriteKVHex(fd, name, mc.regs[i], 16);
    }
    SafeWriteKVHex(fd, "  sp ", mc.sp, 16);
    SafeWriteKVHex(fd, "  pc ", mc.pc, 16);
#elif defined(__x86_64__)
    // x86-64
    const auto &mc = uc->uc_mcontext;
    SafeWriteKVHex(fd, "  rax", mc.gregs[REG_RAX], 16);
    SafeWriteKVHex(fd, "  rbx", mc.gregs[REG_RBX], 16);
    SafeWriteKVHex(fd, "  rcx", mc.gregs[REG_RCX], 16);
    SafeWriteKVHex(fd, "  rdx", mc.gregs[REG_RDX], 16);
    SafeWriteKVHex(fd, "  rsi", mc.gregs[REG_RSI], 16);
    SafeWriteKVHex(fd, "  rdi", mc.gregs[REG_RDI], 16);
    SafeWriteKVHex(fd, "  rbp", mc.gregs[REG_RBP], 16);
    SafeWriteKVHex(fd, "  rsp", mc.gregs[REG_RSP], 16);
    SafeWriteKVHex(fd, "  r8 ", mc.gregs[REG_R8], 16);
    SafeWriteKVHex(fd, "  r9 ", mc.gregs[REG_R9], 16);
    SafeWriteKVHex(fd, "  r10", mc.gregs[REG_R10], 16);
    SafeWriteKVHex(fd, "  r11", mc.gregs[REG_R11], 16);
    SafeWriteKVHex(fd, "  r12", mc.gregs[REG_R12], 16);
    SafeWriteKVHex(fd, "  r13", mc.gregs[REG_R13], 16);
    SafeWriteKVHex(fd, "  r14", mc.gregs[REG_R14], 16);
    SafeWriteKVHex(fd, "  r15", mc.gregs[REG_R15], 16);
    SafeWriteKVHex(fd, "  rip", mc.gregs[REG_RIP], 16);
#elif defined(__i386__)
    // x86-32
    const auto &mc = uc->uc_mcontext;
    SafeWriteKVHex(fd, "  eax", mc.gregs[REG_EAX], 8);
    SafeWriteKVHex(fd, "  ebx", mc.gregs[REG_EBX], 8);
    SafeWriteKVHex(fd, "  ecx", mc.gregs[REG_ECX], 8);
    SafeWriteKVHex(fd, "  edx", mc.gregs[REG_EDX], 8);
    SafeWriteKVHex(fd, "  esi", mc.gregs[REG_ESI], 8);
    SafeWriteKVHex(fd, "  edi", mc.gregs[REG_EDI], 8);
    SafeWriteKVHex(fd, "  ebp", mc.gregs[REG_EBP], 8);
    SafeWriteKVHex(fd, "  esp", mc.gregs[REG_ESP], 8);
    SafeWriteKVHex(fd, "  eip", mc.gregs[REG_EIP], 8);
#else
    SafeWrite(fd, "  (register dump not implemented for this architecture)\n");
#endif

    SafeWriteChar(fd, '\n');
}

// ============================================================================
// Device/system info
// ============================================================================

#ifdef __ANDROID__
static void AndroidGetProp(const char *name, char *buf, size_t bufSize)
{
    buf[0] = '\0';
    __system_property_get(name, buf);
}

static void WriteAndroidDeviceInfo(int fd)
{
    char buf[PROP_VALUE_MAX + 1];

    SafeWrite(fd, "\n--- Device Info (Android) ---\n");

    AndroidGetProp("ro.build.fingerprint", buf, sizeof(buf));
    SafeWriteKV(fd, "  Build fingerprint", buf);

    AndroidGetProp("ro.product.model", buf, sizeof(buf));
    SafeWriteKV(fd, "  Model", buf);

    AndroidGetProp("ro.product.brand", buf, sizeof(buf));
    SafeWriteKV(fd, "  Brand", buf);

    AndroidGetProp("ro.product.device", buf, sizeof(buf));
    SafeWriteKV(fd, "  Device", buf);

    AndroidGetProp("ro.build.version.sdk", buf, sizeof(buf));
    SafeWriteKV(fd, "  SDK version", buf);

    AndroidGetProp("ro.build.version.release", buf, sizeof(buf));
    SafeWriteKV(fd, "  Android version", buf);

#if defined(__arm__)
    SafeWriteKV(fd, "  ABI", "armeabi-v7a");
#elif defined(__aarch64__)
    SafeWriteKV(fd, "  ABI", "arm64-v8a");
#elif defined(__i386__)
    SafeWriteKV(fd, "  ABI", "x86");
#elif defined(__x86_64__)
    SafeWriteKV(fd, "  ABI", "x86_64");
#else
    SafeWriteKV(fd, "  ABI", "unknown");
#endif

    SafeWriteChar(fd, '\n');
}
#endif // __ANDROID__

static void WriteLinuxSystemInfo(int fd)
{
#ifndef __ANDROID__
    SafeWrite(fd, "\n--- System Info (Linux) ---\n");

    // Read /proc/version for kernel info (async-signal-safe: open/read/close)
    int procFd = open("/proc/version", O_RDONLY);
    if (procFd >= 0)
    {
        char verbuf[256];
        ssize_t n = read(procFd, verbuf, sizeof(verbuf) - 1);
        close(procFd);
        if (n > 0)
        {
            verbuf[n] = '\0';
            // Trim trailing newline
            if (n > 0 && verbuf[n - 1] == '\n')
                verbuf[n - 1] = '\0';
            SafeWriteKV(fd, "  Kernel", verbuf);
        }
    }

#if defined(__x86_64__)
    SafeWriteKV(fd, "  Arch", "x86_64");
#elif defined(__i386__)
    SafeWriteKV(fd, "  Arch", "x86");
#elif defined(__aarch64__)
    SafeWriteKV(fd, "  Arch", "aarch64");
#elif defined(__arm__)
    SafeWriteKV(fd, "  Arch", "arm");
#else
    SafeWriteKV(fd, "  Arch", "unknown");
#endif

    SafeWriteChar(fd, '\n');
#endif
}

// ============================================================================
// /proc/self/maps snapshot (for addr2line offset calculation)
// ============================================================================

static void WriteMaps(int fd)
{
    SafeWrite(fd, "\n--- Memory Maps (/proc/self/maps, filtered) ---\n");
    SafeWrite(fd, "Note: Only executable and game-related mappings are shown.\n\n");

    int mapsFd = open("/proc/self/maps", O_RDONLY);
    if (mapsFd < 0)
    {
        SafeWrite(fd, "  (unable to read /proc/self/maps)\n\n");
        return;
    }

    // Read and filter maps line by line.
    // We only output lines containing "libmain.so" or lines with executable permissions.
    // Buffer-based line reading (async-signal-safe).
    char buf[4096];
    char line[512];
    int linePos = 0;
    ssize_t n;

    while ((n = read(mapsFd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; ++i)
        {
            if (buf[i] == '\n' || linePos >= (int)sizeof(line) - 1)
            {
                line[linePos] = '\0';

                // Filter: show lines containing "libmain.so" (game), or
                // lines with 'x' in permissions (column ~20, after addr range + space)
                bool show = false;

                // Check for "libmain.so" or "th06" anywhere in line
                for (int j = 0; line[j] != '\0'; ++j)
                {
                    if (line[j] == 'l' && line[j + 1] == 'i' && line[j + 2] == 'b' && line[j + 3] == 'm' &&
                        line[j + 4] == 'a' && line[j + 5] == 'i' && line[j + 6] == 'n')
                    {
                        show = true;
                        break;
                    }
                    if (line[j] == 't' && line[j + 1] == 'h' && line[j + 2] == '0' && line[j + 3] == '6')
                    {
                        show = true;
                        break;
                    }
                }

                // Also show lines with executable permission (r-xp or r-xs)
                if (!show)
                {
                    // Format: "addr1-addr2 perms ..."
                    // Find the permissions field (after first space)
                    int spaceIdx = -1;
                    for (int j = 0; line[j] != '\0'; ++j)
                    {
                        if (line[j] == ' ')
                        {
                            spaceIdx = j;
                            break;
                        }
                    }
                    if (spaceIdx >= 0 && line[spaceIdx + 1] != '\0' && line[spaceIdx + 2] != '\0' &&
                        line[spaceIdx + 3] == 'x')
                    {
                        show = true;
                    }
                }

                if (show && linePos > 0)
                {
                    SafeWrite(fd, "  ");
                    SafeWrite(fd, line);
                    SafeWriteChar(fd, '\n');
                }

                linePos = 0;
            }
            else
            {
                line[linePos++] = buf[i];
            }
        }
    }

    close(mapsFd);
    SafeWriteChar(fd, '\n');
}

// ============================================================================
// Crash file path generation
// ============================================================================

static char s_crashDir[512] = "";

static bool EnsureCrashDir()
{
    const char *userPath = GamePaths::GetUserPath();
    int len = 0;
    while (userPath[len] != '\0' && len < 480)
    {
        s_crashDir[len] = userPath[len];
        ++len;
    }
    // Append "crash/"
    const char *suffix = "crash/";
    int si = 0;
    while (suffix[si] != '\0' && len < 500)
    {
        s_crashDir[len++] = suffix[si++];
    }
    s_crashDir[len] = '\0';

    // mkdir (ignore EEXIST)
    if (mkdir(s_crashDir, 0755) != 0 && errno != EEXIST)
    {
        return false;
    }
    return true;
}

// Read a monotonic-ish timestamp from /proc/uptime (async-signal-safe).
// Returns uptime in seconds; 0 on failure.
static unsigned long long ReadUptimeSeconds()
{
    int fd = open("/proc/uptime", O_RDONLY);
    if (fd < 0)
        return 0;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    // Parse integer part of first field (seconds before decimal point)
    unsigned long long val = 0;
    for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; ++i)
        val = val * 10 + static_cast<unsigned long long>(buf[i] - '0');
    return val;
}

static int BuildCrashFilePath(char *outBuf, size_t outBufSize)
{
    // Use PID + uptime as unique identifiers (all async-signal-safe).
    // Format: crash/crash_<uptime>s_pid<pid>.txt
    char uptimeStr[24];
    UintToStr(uptimeStr, sizeof(uptimeStr), ReadUptimeSeconds());
    char pidStr[16];
    UintToStr(pidStr, sizeof(pidStr), static_cast<unsigned long long>(getpid()));

    int pos = 0;
    const char *parts[] = {s_crashDir, "crash_", uptimeStr, "s_pid", pidStr, ".txt", nullptr};
    for (int pi = 0; parts[pi] != nullptr; ++pi)
    {
        const char *p = parts[pi];
        while (*p && pos + 1 < (int)outBufSize)
            outBuf[pos++] = *p++;
    }
    outBuf[pos] = '\0';
    return pos;
}

// ============================================================================
// Signal handler state
// ============================================================================

// Guard against recursive signals (crash inside crash handler).
static volatile sig_atomic_t s_inHandler = 0;

// Saved previous signal actions for restoration.
static struct sigaction s_oldActions[kNumSignals];
static bool s_installed = false;

// ============================================================================
// The signal handler (async-signal-safe)
// ============================================================================

static void CrashSignalHandler(int signo, siginfo_t *info, void *ucontextRaw)
{
    // Guard against recursion: if we crash inside the handler, just die.
    if (s_inHandler)
    {
        // Reset to default and re-raise.
        signal(signo, SIG_DFL);
        raise(signo);
        _exit(128 + signo);
    }
    s_inHandler = 1;

    const ucontext_t *uc = static_cast<const ucontext_t *>(ucontextRaw);

    // --- Attempt to write crash file ---

    char crashPath[512];
    int fd = -1;

    if (EnsureCrashDir())
    {
        BuildCrashFilePath(crashPath, sizeof(crashPath));
        fd = open(crashPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }

    // If we can't write to file, try stderr as fallback.
    if (fd < 0)
    {
        fd = STDERR_FILENO;
    }

    // --- Header ---
    SafeWrite(fd, "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n");
    SafeWrite(fd, "th06_sdl crash report\n");
    SafeWrite(fd, "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n\n");

    // --- Signal info ---
    SafeWrite(fd, "--- Signal ---\n");
    SafeWriteKV(fd, "  Signal", SignalName(signo));
    SafeWriteKVUint(fd, "  Signal number", static_cast<unsigned long long>(signo));
    SafeWriteKV(fd, "  Description", SignalDescription(signo));

    if (info != nullptr)
    {
        const char *codeName = SiCodeName(signo, info->si_code);
        if (codeName)
        {
            SafeWriteKV(fd, "  Code", codeName);
        }
        else
        {
            SafeWriteKVUint(fd, "  Code", static_cast<unsigned long long>(info->si_code));
        }
        SafeWriteKVHex(fd, "  Fault address", reinterpret_cast<uintptr_t>(info->si_addr), sizeof(void *) * 2);
    }

    SafeWriteKVUint(fd, "  PID", static_cast<unsigned long long>(getpid()));
    SafeWriteKVUint(fd, "  TID", static_cast<unsigned long long>(static_cast<pid_t>(syscall(SYS_gettid))));
    SafeWriteChar(fd, '\n');

    // --- Registers ---
    WriteRegisters(fd, uc);

    // --- Backtrace ---
    BacktraceState btState;
    CaptureBacktrace(&btState);
    WriteBacktrace(fd, &btState);

    // --- Platform info ---
#ifdef __ANDROID__
    WriteAndroidDeviceInfo(fd);
#else
    WriteLinuxSystemInfo(fd);
#endif

    // --- Memory maps ---
    WriteMaps(fd);

    // --- Log crash file location to logcat/stderr ---
    if (fd != STDERR_FILENO)
    {
        close(fd);

#ifdef __ANDROID__
        // Note: __android_log_print is NOT strictly async-signal-safe, but it is
        // widely used in signal handlers in practice (Android's own debuggerd does
        // this). We call it after the crash file is already written and closed,
        // so even if it crashes, the report is already persisted.
        __android_log_print(ANDROID_LOG_FATAL, "th06_crash",
                            "Native crash caught! Report written to: %s", crashPath);
        __android_log_print(ANDROID_LOG_FATAL, "th06_crash",
                            "Signal: %s (%d), fault addr: %p",
                            SignalName(signo), signo,
                            info ? info->si_addr : nullptr);
#else
        // Write to stderr
        SafeWrite(STDERR_FILENO, "[th06_crash] Crash report written to: ");
        SafeWrite(STDERR_FILENO, crashPath);
        SafeWriteChar(STDERR_FILENO, '\n');
#endif
    }

    // --- Restore default handler and re-raise ---
    // This allows the system to generate its own tombstone (Android) or core dump (Linux).
    struct sigaction defaultAction = {}; // value-init to zero (no memset needed)
    defaultAction.sa_handler = SIG_DFL;
    sigemptyset(&defaultAction.sa_mask);
    sigaction(signo, &defaultAction, nullptr);

    raise(signo);

    // Should not reach here, but just in case:
    _exit(128 + signo);
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

void Init()
{
    if (s_installed)
        return;

    // Pre-compute crash directory path so it's ready when signal fires.
    EnsureCrashDir();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = CrashSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    // Block all caught signals during handler execution to prevent nesting.
    for (int i = 0; i < kNumSignals; ++i)
    {
        sigaddset(&sa.sa_mask, kSignals[i].signo);
    }

    // Set up alternate signal stack for stack overflow scenarios.
    static char altStack[SIGSTKSZ + 16384];
    stack_t ss;
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    for (int i = 0; i < kNumSignals; ++i)
    {
        if (sigaction(kSignals[i].signo, &sa, &s_oldActions[i]) != 0)
        {
            // Failed to install handler for this signal — not fatal, log and continue.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "CrashHandler: failed to install handler for %s: %s",
                        kSignals[i].name, strerror(errno));
        }
    }

    s_installed = true;

    SDL_Log("CrashHandler: initialized (crash reports → %s)", s_crashDir);
}

void Shutdown()
{
    if (!s_installed)
        return;

    // Restore original signal handlers.
    for (int i = 0; i < kNumSignals; ++i)
    {
        sigaction(kSignals[i].signo, &s_oldActions[i], nullptr);
    }

    s_installed = false;
}

} // namespace th06::CrashHandler

#endif // !defined(_WIN32)
