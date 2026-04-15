#include "thprac_games.h"
#include "thprac_games_dx8.h"

// __declspec(noinline) is MSVC-only; map to GCC attribute on other compilers
#if !defined(_MSC_VER) && !defined(__declspec)
#define __declspec(x) __attribute__((x))
#endif

// MSVC-only safe functions; map to standard equivalents on other compilers
#ifndef _MSC_VER
#define sscanf_s sscanf
#define sprintf_s sprintf
#endif

#include "thprac_utils.h"
#include <algorithm>
#include <fstream>
#include <random>
#include <string>
#include <cstdio>
#include <cmath>


#include "thprac_res.h"
#include "thprac_th06.h"
#include "thprac_bridge.h"
#include "Gui.hpp"
#include "EclManager.hpp"
#include "AnmManager.hpp"
#include "AstroBot.hpp"
#include "MainMenu.hpp"
#include "NetplaySession.hpp"
#include "OnlineMenu.hpp"
#include "Session.hpp"
#include "BulletManager.hpp"
#include "Rng.hpp"
#include "Stage.hpp"
#include "SinglePlayerSnapshot.hpp"
#include "sdl2_renderer.hpp"
#include "IRenderer.hpp"
#include "Supervisor.hpp"
#include "FileSystem.hpp"

// extern "C" globals from other TUs (DIFFABLE_STATIC)
extern "C" { extern i32 g_PlayerShot; }

// D3DSURFACE_DESC provided by sdl2_compat.hpp via thprac_bridge.h
// IDirect3DTexture8 stub for hitbox texture operations (no-op in SDL2 build)
struct IDirect3DTexture8 {
    void Release() {}
    void GetLevelDesc(int, D3DSURFACE_DESC* desc) {
        if (desc) { desc->Width = 0; desc->Height = 0; }
    }
};
typedef IDirect3DTexture8* LPDIRECT3DTEXTURE8;

// DirectSound stub
struct IDirectSoundBuffer {
    void Stop() {}
    void Play(uint32_t, uint32_t, uint32_t) {}
};

// Win32 type and constant compatibility for SDL2 build
#ifndef WORD
typedef unsigned short WORD;
#endif
#ifndef BYTE
typedef unsigned char BYTE;
#endif
#ifndef LPCSTR
typedef const char* LPCSTR;
#endif
#ifndef byte
typedef unsigned char byte;
#endif
#ifndef VK_F1
#define VK_ESCAPE 0x1B
#define VK_RETURN 0x0D
#define VK_F1     0x70
#define VK_F2     0x71
#define VK_F3     0x72
#define VK_F4     0x73
#define VK_F5     0x74
#define VK_F6     0x75
#define VK_F7     0x76
#define VK_F8     0x77
#define VK_F9     0x78
#define VK_F10    0x79
#define VK_F11    0x7A
#define VK_F12    0x7B
#define VK_UP     0x26
#define VK_DOWN   0x28
#define VK_LEFT   0x25
#define VK_RIGHT  0x27
#define VK_LSHIFT 0xA0
#define VK_OEM_3  0xC0
#endif

// timeGetTime and SDL provided by sdl2_compat.hpp via thprac_bridge.h

// Platform-specific high-resolution timer
#ifdef _WIN32
static double g_performance_freq = []() -> double {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
}();
#else
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
static double g_performance_freq = 1000000000.0; // nanoseconds
struct LARGE_INTEGER { int64_t QuadPart; };
static inline void QueryPerformanceCounter(LARGE_INTEGER* li) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    li->QuadPart = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static inline void QueryPerformanceFrequency(LARGE_INTEGER* li) {
    li->QuadPart = 1000000000LL;
}
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#endif
static std::vector<int64_t> g_clocks_start;

inline int SetUpClock() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_clocks_start.push_back(t.QuadPart);
    return (int)g_clocks_start.size() - 1;
}

inline double ResetClock(int id) {
    if (id >= 0 && id < (int)g_clocks_start.size()) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        double time_passed = (double)(t.QuadPart - g_clocks_start[id]) / g_performance_freq;
        g_clocks_start[id] = t.QuadPart;
        return time_passed > 0 ? time_passed : 0;
    }
    return 0;
}

static int g_count_directory = 0;

#ifdef _WIN32
static std::vector<std::wstring> g_directory;

inline void PushCurrentDirectory(const wchar_t* new_dir) {
    WCHAR buffer[MAX_PATH] = { 0 };
    GetCurrentDirectoryW(MAX_PATH, buffer);
    if ((int)g_directory.size() <= g_count_directory)
        g_directory.push_back(std::wstring(buffer));
    else
        g_directory[g_count_directory] = std::wstring(buffer);
    g_count_directory++;
    ExpandEnvironmentStringsW(new_dir, buffer, MAX_PATH);
    if (GetFileAttributesW(buffer) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryW(buffer, NULL);
    SetCurrentDirectoryW(buffer);
}

inline void PopCurrentDirectory() {
    if (!g_directory.empty() && g_count_directory > 0) {
        SetCurrentDirectoryW(g_directory[g_count_directory - 1].c_str());
        g_count_directory--;
    }
}
#else // !_WIN32
static std::vector<std::string> g_directory;

inline void PushCurrentDirectory(const wchar_t*) {
    char buf[MAX_PATH];
    if (getcwd(buf, sizeof(buf))) {
        if ((int)g_directory.size() <= g_count_directory)
            g_directory.push_back(std::string(buf));
        else
            g_directory[g_count_directory] = std::string(buf);
        g_count_directory++;
    }
    // On Linux the game runs from its own directory; no appdata relocation needed.
}

inline void PopCurrentDirectory() {
    if (!g_directory.empty() && g_count_directory > 0) {
        chdir(g_directory[g_count_directory - 1].c_str());
        g_count_directory--;
    }
}
#endif // _WIN32
inline const char* GetVersionStr() { return "1.0.0"; }
// GetModuleHandleW: use 0 directly where needed (Windows API not applicable)
inline void CalcFileHash(const wchar_t*, uint64_t*) {}

// Time formatting utilities
inline std::string GetTime_HHMMSS(int64_t ns) {
    int64_t sec = ns / 1000000000ll;
    int h = (int)(sec / 3600); int m = (int)((sec % 3600) / 60); int s = (int)(sec % 60);
    char buf[64]; snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return buf;
}
inline std::string GetTime_YYMMDD_HHMMSS(int64_t ns) {
    int64_t sec = ns / 1000000000ll;
    int d = (int)(sec / 86400); int h = (int)((sec % 86400) / 3600);
    int m = (int)((sec % 3600) / 60); int s = (int)(sec % 60);
    char buf[64]; snprintf(buf, sizeof(buf), "%dd %02d:%02d:%02d", d, h, m, s);
    return buf;
}

// ECLHelper — patches ECL bytecode in-place via g_EclManager.eclFile pointer.
// SetBaseAddr receives &g_EclManager; first field is eclFile (EclRawHeader*).
struct ECLHelper {
    uint8_t* mBuffer = nullptr;
    size_t mPos = 0;

    void SetBaseAddr(void* addr) {
        // addr = &g_EclManager; *(void**)addr = g_EclManager.eclFile
        mBuffer = (uint8_t*)(*(uintptr_t*)addr);
        mPos = 0;
    }
    void SetPos(int pos) { mPos = (size_t)pos; }

    // Write at current position and advance
    template<typename T>
    ECLHelper& operator<<(const T& data) {
        if (mBuffer) {
            memcpy(mBuffer + mPos, &data, sizeof(T));
            mPos += sizeof(T);
        }
        return *this;
    }

    // Write at specific offset (pair form)
    template<typename T>
    ECLHelper& operator<<(const std::pair<size_t, T>& data) {
        if (mBuffer) {
            size_t oldPos = mPos;
            mPos = data.first;
            memcpy(mBuffer + mPos, &data.second, sizeof(T));
            mPos += sizeof(T);
        }
        return *this;
    }
    template<typename T>
    ECLHelper& operator<<(const std::pair<int, T>& data) {
        if (mBuffer) {
            mPos = (size_t)data.first;
            memcpy(mBuffer + mPos, &data.second, sizeof(T));
            mPos += sizeof(T);
        }
        return *this;
    }
};

// asm_call stub - assembly-level game function calls are no-ops in source build
enum { Thiscall = 0 };
template<uintptr_t addr, int convention, typename... Args>
inline void asm_call(Args...) {}

namespace THPrac {
// g_forceRenderCursor and FastRetryOpt provided by thprac_games.h

namespace TH06 {
    using namespace Bridge;

    const char* TrLocal(const char* zh, const char* en, const char* ja)
    {
        switch (Gui::LocaleGet()) {
        case Gui::LOCALE_ZH_CN:
            return zh;
        case Gui::LOCALE_JA_JP:
            return ja;
        case Gui::LOCALE_EN_US:
        default:
            return en;
        }
    }

    const char* NoFreezeOnFocusLossLabel()
    {
        return TrLocal("窗口失焦时不中止游戏", "Do not freeze on focus loss", "フォーカスを失っても停止しない");
    }

    const char* NoFreezeOnFocusLossDesc()
    {
        return TrLocal("启用后，窗口失去焦点或进入后台时仍继续更新和渲染。联机双开同机测试时需要打开。",
            "Keep updating and rendering when the window loses focus or enters the background. Enable this for same-machine netplay testing.",
            "有効にすると、ウィンドウがフォーカスを失っても更新と描画を続行します。同一PCでのネット対戦テストでは有効化が必要です。");
    }

    const char* ManualDumpHotkeyLabel()
    {
        return TrLocal("启用手动转储热键 (Ctrl+D)", "Enable Manual Dump Hotkey (Ctrl+D)",
                       "手動ダンプホットキーを有効化 (Ctrl+D)");
    }

    const char* ManualDumpHotkeyDesc()
    {
        return TrLocal("开启后，开发者模式下按 Ctrl+D 会让 watchdog 辅助线程立即输出一个主线程现场转储到 Crash 目录，适合抓“画面卡住但还有心跳”的现场。",
                       "When enabled, pressing Ctrl+D in developer mode asks the watchdog helper thread to immediately dump the current main-thread state into the Crash directory. Use this for hangs that still keep heartbeating.",
                       "有効にすると、開発者モード中に Ctrl+D を押した時、watchdog 補助スレッドが Crash ディレクトリへ主スレッドの現況ダンプを即座に出力します。ハートビートは生きているのに止まって見える症状の採取に使います。");
    }

    const char* RecoveryAutoDumpLabel()
    {
        return TrLocal("恢复快照时自动转储", "Auto Dump During Snapshot Recovery",
                       "スナップショット復旧時に自動ダンプ");
    }

    const char* RecoveryAutoDumpDesc()
    {
        return TrLocal("开启后，联机权威快照恢复时会自动请求 watchdog 辅助线程输出转储。主机在开始发送权威快照时转储一次，客机在开始应用收到的快照时转储一次。",
                       "When enabled, authoritative netplay snapshot recovery automatically asks the watchdog helper thread to write a dump. The host dumps once when it starts sending the authoritative snapshot, and the guest dumps once when it starts applying the received snapshot.",
                       "有効にすると、ネット対戦の権威スナップショット復旧中に watchdog 補助スレッドへ自動ダンプを要求します。ホストは権威スナップショット送信開始時に一度、ゲストは受信したスナップショットの適用開始時に一度ダンプします。");
    }

    const char* ManualDumpHotkeyUnavailableHint()
    {
        return TrLocal("（仅 Windows 可用）", "(Windows only)", "（Windows 専用）");
    }

    const char* NoFreezeOnFocusLossLockedHint()
    {
        return TrLocal("（联机中锁定）", "(locked by netplay)", "（ネット対戦中は固定）");
    }

    const char* DeveloperModeLabel()
    {
        return TrLocal("开发者模式", "Developer Mode", "開発者モード");
    }

    const char* DeveloperModeDesc()
    {
        return TrLocal("开启后，F11 面板会切换成多标签页，并显示联机调试工具。",
                       "When enabled, the F11 panel switches to tabs and shows netplay debugging tools.",
                       "有効にすると、F11 パネルがタブ表示に切り替わり、ネット対戦デバッグ機能を表示します。");
    }

    const char* AdvancedFeaturesTabLabel()
    {
        return TrLocal("高级功能", "Advanced Features", "高度な機能");
    }

    const char* DeveloperFeaturesTabLabel()
    {
        return TrLocal("开发者功能", "Developer Tools", "開発者機能");
    }

    const char* NetworkDebuggerTabLabel()
    {
        return TrLocal("网络调试器", "Network Debugger", "ネットワークデバッガ");
    }

    const char* PortableRestoreTabLabel()
    {
        return TrLocal("定态恢复", "Portable Restore", "ポータブル復元");
    }

    const char* DebugLogOutputLabel()
    {
        return TrLocal("输出调试日志", "Emit Debug Logs", "デバッグログを出力する");
    }

    const char* DebugLogOutputDesc()
    {
        return TrLocal("开启后，replay / portable restore / watchdog 等开发诊断日志会额外写入日志文件。关闭时只保留游戏原本的日志，不再输出这些附加调试记录。",
                       "When enabled, developer diagnostics such as replay / portable restore / watchdog tracing are written into the log files. When disabled, only the game's original log output remains and these extra debug records stay silent.",
                       "有効にすると、replay / portable restore / watchdog などの開発診断ログを追加でログファイルへ出力します。無効時はゲーム本来のログだけを残し、追加のデバッグ記録は出力しません。");
    }

    const char* AstroBotSectionLabel()
    {
        return TrLocal("AstroBot", "AstroBot", "AstroBot");
    }

    const char* AstroBotEnableLabel()
    {
        return TrLocal("启用 AstroBot", "Enable AstroBot", "AstroBot を有効化");
    }

    const char* AstroBotTargetLabel()
    {
        return TrLocal("控制对象", "AstroBot Target", "制御対象");
    }

    const char* AstroBotTargetHint()
    {
        if (th06::Session::IsRemoteNetplaySession())
        {
            return TrLocal("远程联机中仅允许 AstroBot 接管本机这一侧。", "In remote netplay AstroBot can only control the local side.",
                           "リモート対戦中は AstroBot はローカル側のみ操作できます。");
        }
        return TrLocal("选择 AstroBot 接管的玩家侧。", "Choose which player AstroBot controls.",
                       "AstroBot が操作するプレイヤー側を選択します。");
    }

    const char* AstroBotAutoShootLabel()
    {
        return TrLocal("自动射击", "Auto Shoot", "自動ショット");
    }

    const char* AstroBotAutoBombLabel()
    {
        return TrLocal("自动 Bomb", "Auto Bomb", "自動ボム");
    }

    const char* AstroBotBossOnlyLabel()
    {
        return TrLocal("仅 Boss 战追踪", "Boss Only", "ボス戦のみ追尾");
    }

    const char* AstroBotStatusLabel()
    {
        return TrLocal("AstroBot 状态", "AstroBot Status", "AstroBot 状態");
    }

    const char* AstroBotTargetName(int target)
    {
        if (th06::Session::IsRemoteNetplaySession())
        {
            return target == 0 ? TrLocal("关闭", "Off", "オフ") : TrLocal("本机侧", "LocalSide", "ローカル側");
        }
        switch (target)
        {
        case 1: return "P1";
        case 2: return "P2";
        default: return TrLocal("关闭", "Off", "オフ");
        }
    }

    const char* AstroBotOwnershipName(const th06::AstroBot::StatusSnapshot& status)
    {
        if (!status.remoteSession)
        {
            return AstroBotTargetName(static_cast<int>(status.target));
        }
        switch (status.target)
        {
        case th06::AstroBot::Target::P1: return "local-p1";
        case th06::AstroBot::Target::P2: return "local-p2";
        case th06::AstroBot::Target::Off:
        default: return TrLocal("本机侧", "local-side", "ローカル側");
        }
    }

    const char* AstroBotActionName(th06::AstroBot::Action action)
    {
        using Action = th06::AstroBot::Action;
        switch (action)
        {
        case Action::Up: return TrLocal("上", "Up", "上");
        case Action::Down: return TrLocal("下", "Down", "下");
        case Action::Left: return TrLocal("左", "Left", "左");
        case Action::Right: return TrLocal("右", "Right", "右");
        case Action::UpLeft: return TrLocal("左上", "Up-Left", "左上");
        case Action::UpRight: return TrLocal("右上", "Up-Right", "右上");
        case Action::DownLeft: return TrLocal("左下", "Down-Left", "左下");
        case Action::DownRight: return TrLocal("右下", "Down-Right", "右下");
        case Action::Idle:
        default: return TrLocal("停", "Idle", "停止");
        }
    }

    const char* AstroBotBypassReasonName(th06::AstroBot::BypassReason reason)
    {
        using BypassReason = th06::AstroBot::BypassReason;
        switch (reason)
        {
        case BypassReason::Disabled: return TrLocal("未启用", "disabled", "無効");
        case BypassReason::NoTarget: return TrLocal("未选择目标", "no target", "対象未選択");
        case BypassReason::NotGameplay: return TrLocal("不在 gameplay", "not gameplay", "ゲームプレイ外");
        case BypassReason::NetplayInactive: return TrLocal("联机会话未激活", "netplay inactive", "対戦セッション未開始");
        case BypassReason::ReplayOrDemo: return TrLocal("Replay/Demo 禁用", "replay/demo", "リプレイ/デモ中");
        case BypassReason::SharedShell: return TrLocal("菜单/续关中", "menu/retry shell", "メニュー/コンティニュー中");
        case BypassReason::PortableRestoreBusy: return TrLocal("恢复流程忙", "portable restore busy", "復元処理中");
        case BypassReason::NoPlayer: return TrLocal("玩家侧不存在", "missing player", "対象プレイヤーなし");
        case BypassReason::PlayerInactive: return TrLocal("玩家不可操作", "player inactive", "操作不能");
        case BypassReason::None:
        default: return TrLocal("旁路", "bypassed", "無効");
        }
    }

    const char* AstroBotModeName(th06::AstroBot::ModeHint mode)
    {
        using ModeHint = th06::AstroBot::ModeHint;
        switch (mode)
        {
        case ModeHint::Force: return TrLocal("势场", "force", "勢場");
        case ModeHint::Dest: return TrLocal("安全点", "dest", "安全点");
        case ModeHint::Escape: return TrLocal("逃生", "escape", "離脱");
        case ModeHint::Thread: return TrLocal("钻缝", "thread", "隙間");
        case ModeHint::MoveCheck: return TrLocal("纠错", "movecheck", "補正");
        case ModeHint::None:
        default: return TrLocal("无", "none", "なし");
        }
    }

    const char* PausePresentationHoldLabel()
    {
        return TrLocal("暂停同步中...", "Pausing...", "一時停止を同期中...");
    }

    const char* PortableRestoreTrialLabel()
    {
        return TrLocal("单机 L 使用 portable 试运行", "Use portable trial for single-player L",
                       "シングルLでポータブル試行を使う");
    }

    const char* PortableRestoreTrialDesc()
    {
        return TrLocal("启用后，单机 Load(L)/Ctrl+O 会走纯 portable 内存恢复，Ctrl+L 会从磁盘的 portable_state.bin 启动并恢复。关闭时普通 L 仍走原来的 DGS 路径。",
                       "When enabled, single-player Load(L)/Ctrl+O use pure portable in-memory restore, and Ctrl+L boots/restores from portable_state.bin on disk. When disabled, normal L keeps using the original DGS path.",
                       "有効にすると、シングルの Load(L)/Ctrl+O は純粋なポータブル復元を使い、Ctrl+L はディスク上の portable_state.bin から起動して復元します。無効時は通常の L が従来の DGS 経路のままです。");
    }

    const char* PortableRestoreRuntimeHint()
    {
        return TrLocal("当前仅在开发者模式下可用，目标是同版本、同资源内容下的纯 portable 单机恢复。暂不支持 Netplay / Replay / 2P portable restore。",
                       "This is currently developer-mode only and targets pure portable single-player restore on the same build and resource set. Netplay / replay / 2P portable restore are still unsupported.",
                       "現在は開発者モード専用で、同じビルドとリソース内容での純粋なポータブルシングル復元を対象としています。Netplay / Replay / 2P のポータブル復元は未対応です。");
    }

    const char* NetworkDebuggerUnavailableHint()
    {
        return TrLocal("仅远程联机时可用。", "Only available during remote netplay.", "リモートネット対戦中のみ利用できます。");
    }

    const char* NetworkDebuggerSummary()
    {
        return TrLocal("这些设置只作用于当前进程，用于压测联机收发链路，不会影响单机逻辑。",
                       "These settings affect only the current process and are meant for stressing the netplay packet path without touching single-player logic.",
                       "これらの設定は現在のプロセスにのみ作用し、シングルプレイのロジックには影響せず、ネット対戦の送受信経路の負荷試験に使います。");
    }

    const char* NetDebugEnableLabel()
    {
        return TrLocal("启用网络仿真", "Enable Network Simulation", "ネットワークシミュレーションを有効化");
    }

    const char* NetDebugLatencyLabel()
    {
        return TrLocal("基础延迟", "Base Latency", "基本遅延");
    }

    const char* NetDebugJitterLabel()
    {
        return TrLocal("抖动", "Jitter", "ジッタ");
    }

    const char* NetDebugPacketLossLabel()
    {
        return TrLocal("丢包率", "Packet Loss", "パケットロス");
    }

    const char* NetDebugDuplicateLabel()
    {
        return TrLocal("重复包率", "Packet Duplication", "重複パケット率");
    }

    const char* NetDebugResetLabel()
    {
        return TrLocal("重置网络调试设置", "Reset Network Debugger", "ネットワークデバッガをリセット");
    }

    const char* NetDebugSessionStateLabel()
    {
        return TrLocal("当前联机状态", "Current Netplay State", "現在のネット対戦状態");
    }

    const char* RecoveryChunksLabel()
    {
        return TrLocal("快照分片", "Snapshot Chunks", "スナップショット分割");
    }

    bool IsNetplayDebuggerAvailable()
    {
        return th06::Session::IsRemoteNetplaySession() || th06::Netplay::IsSessionActive() ||
               th06::Netplay::IsWaitingForStartup();
    }

    void ClampDebugNetworkConfig(th06::Netplay::DebugNetworkConfig& config)
    {
        config.latencyMs = std::clamp(config.latencyMs, 0, 1000);
        config.jitterMs = std::clamp(config.jitterMs, 0, 500);
        config.packetLossPercent = std::clamp(config.packetLossPercent, 0, 100);
        config.duplicatePercent = std::clamp(config.duplicatePercent, 0, 100);
    }

    // Legacy address constants removed - use Bridge:: accessors instead
    // e.g., Bridge::GM_Lives() instead of *(int8_t*)(0x69d4ba)

    ImTextureID g_hitbox_textureID = NULL;
    ImVec2 g_hitbox_sz = { 32.0f, 32.0f };

    ImVec2 g_books_pos[6] = { { 0.0f, 0.0f } };

    int32_t g_last_rep_seed = 0;
    bool g_show_bullet_hitbox = false;
    float g_last_boss_x, g_last_boss_y;

    int g_lock_timer = 0;
    float g_speed_multiplier = 1.0f;  // CE-style speed: affects frame timing, not game logic
    bool g_developer_mode_enabled = false;
    bool g_new_touch_enabled = false;
    bool g_mouse_follow_enabled = false;
    bool g_mouse_touch_drag_enabled = false;
    int g_portable_current_bgm_track_index = -1;
    int g_portable_current_boss_asset_profile = 0;
    bool g_debug_ending_jump_active = false;
    int g_ending_shortcut_progress = 0;
    u32 g_ending_shortcut_last_tick = 0;
    bool g_ending_shortcut_prev_e = false;
    bool g_ending_shortcut_prev_n = false;
    bool g_ending_shortcut_prev_d = false;

    constexpr u32 ENDING_SHORTCUT_TIMEOUT_MS = 1500;
    constexpr int PORTABLE_BOSS_ASSET_PROFILE_NONE = 0;
    constexpr int PORTABLE_BOSS_ASSET_PROFILE_STAGE6_BOSS_EFFECTS = 1;
    constexpr int PORTABLE_BOSS_ASSET_PROFILE_STAGE7_END_EFFECTS = 2;

    bool THBGMTest();
    using std::pair;

    class TH06Save {
        SINGLETON(TH06Save)
    public:
        enum TH06SpellState {
            Capture = 0,
            Attempt = 1,
            Timeout = 2
        };
    private:
        struct Save {
            int32_t SC_History[65][5][4][3] = { 0 }; // spellid, diff, playertype, [capture/attempt/timeout]
            int64_t timePlayer[5][4] = { 0 }; // diff/type, precision:ns
        };
        Save save_current;
        Save save_total;
        bool is_save_loaded;
        TH06Save()
        {
            save_current = { 0 };
            save_total = { 0 };
            is_save_loaded = false;
            LoadSave();
        }
    public:
        const static int spellid_books = 64;
        void LoadSave()
        {
            if (is_save_loaded)
                return;
            is_save_loaded = true;
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th06");
            auto fs_new = ::std::fstream("score06.dat", ::std::ios::in | ::std::ios::binary);
            if (fs_new.is_open()) {
                int version = 1;
                fs_new.read((char*)&version, sizeof(version));
                switch (version) {
                    default:
                    case 1: {
                        fs_new.read((char*)save_total.SC_History, sizeof(save_total.SC_History));
                        fs_new.read((char*)save_total.timePlayer, sizeof(save_total.timePlayer));
                    } break;
                }
                fs_new.close();
            } else {
                // compatible
                auto fs = ::std::fstream("spell_capture.dat", ::std::ios::in | ::std::ios::binary);
                if (fs.is_open()) {
                    is_save_loaded = true;
                    fs.read((char*)save_total.SC_History, sizeof(save_total.SC_History));
                    if (!fs.eof())
                        fs.read((char*)save_total.timePlayer, sizeof(save_total.timePlayer)); // compatible , avoid eof

                    for (int i = 0; i < 5; i++)
                        for (int j = 0; j < 4; j++)
                            save_total.timePlayer[i][j] *= 1000000; // make precision to ns
                }
                fs.close();
            }
            PopCurrentDirectory();
        }

        void SaveSave()
        {
            int version = 1;
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th06");
            auto fs = ::std::fstream("score06.dat", ::std::ios::out | ::std::ios::binary);
            if (fs.is_open()) {
                fs.write((char*)(&version), sizeof(version));
                fs.write((char*)(&save_total), sizeof(save_total));
                fs.close();
            }
            PopCurrentDirectory();
        }

        void AddAttempt(int spell_id, byte diff, byte player_type)
        {
            LoadSave();
            save_total.SC_History[spell_id][diff][player_type][1]++;
            save_current.SC_History[spell_id][diff][player_type][1]++;
            SaveSave();
        }

        void AddCapture(int spell_id, byte diff, byte player_type)
        {
            LoadSave();
            save_total.SC_History[spell_id][diff][player_type][0]++;
            save_current.SC_History[spell_id][diff][player_type][0]++;
            SaveSave();
        }

        void AddTimeOut(int spell_id, byte diff, byte player_type)
        {
            LoadSave();
            save_total.SC_History[spell_id][diff][player_type][2]++;
            save_current.SC_History[spell_id][diff][player_type][2]++;
            SaveSave();
        }

        void IncreaseGameTime()
        {
            static int clockid = -1;
            static int64_t timePlayedns;
            DWORD gameState = SV_CurState();
            BYTE pauseMenuState = GM_IsInGameMenu();
            byte is_rep = GM_IsInReplay();
            if ((!is_rep) && gameState == 2 && pauseMenuState == 0) {
                if (clockid == -1)
                    clockid = SetUpClock();
                // SetThreadAffinityMask(GetCurrentThread(), 1);
                // win7 has some problem with static performance counter when use full screen
                byte cur_diff = (byte)GM_Difficulty();
                byte cur_player_typea = GM_Character();
                byte cur_player_typeb = GM_ShotType();
                byte cur_player_type = (cur_player_typea << 1) | cur_player_typeb;

                int64_t time_ns = static_cast<int64_t>(ResetClock(clockid) * 1e9);
                save_total.timePlayer[cur_diff][cur_player_type] += time_ns;
                save_current.timePlayer[cur_diff][cur_player_type] += time_ns;
                timePlayedns += time_ns;
                if (timePlayedns >= (1000000000ll * 60 * 3)) { // save every 3 minutes automatically
                    LoadSave();// if load have failed, it will eat the time, but anyway
                    timePlayedns = 0;
                    SaveSave();
                }
            } else {
                ResetClock(clockid);
            }
        }
        
        
        int GetTotalSpellCardCount(int spell_id, int diff, int player_type, TH06SpellState state)
        {
            return save_total.SC_History[spell_id][diff][player_type][state];
        }
        int GetCurrentSpellCardCount(int spell_id, int diff, int player_type, TH06SpellState state)
        {
            return save_current.SC_History[spell_id][diff][player_type][state];
        }
        int64_t GetTotalGameTime(int diff, int player_type)
        {
            return save_total.timePlayer[diff][player_type];
        }
        int64_t GetCurrentGameTime(int diff, int player_type)
        {
            return save_current.timePlayer[diff][player_type];
        }
    };


    struct THPracParam {
        int32_t mode;

        int32_t stage;
        int32_t section;
        int32_t phase;
        int32_t frame;

        int64_t score;
        float life;
        float bomb;
        float power;
        int32_t graze;
        int32_t point;

        int32_t rank;
        bool rankLock;
        int32_t fakeType;

        int32_t delay_st6bs9;
        bool wall_prac_st6;
        bool dlg;

        int bF;//book_fix
        int bX1;//book_x
        int bX2;
        int bX3;
        int bX4;
        int bX5;
        int bX6;

        int bY1;//book_y
        int bY2;
        int bY3;
        int bY4;
        int bY5;
        int bY6;

        int snipeN;
        int snipeF;

        bool _playLock;
        void Reset()
        {
            mode = 0;
            stage = 0;
            section = 0;
            phase = 0;
            score = 0ll;
            life = 0.0f;
            bomb = 0.0f;
            power = 0.0f;
            graze = 0;
            point = 0;
            rank = 0;
            rankLock = false;
            fakeType = 0;

            bY1 = 32,
            bY2 = 128,
            bY3 = 144,
            bY4 = 64,
            bY5 = 80,
            bY6 = 96;
            bX1 = 0;
            bX2 = 0;
            bX3 = 0;
            bX4 = 0;
            bX5 = 0;
            bX6 = 0;
            bF = 0;
            snipeN = 0;
            snipeF = 0;
            
        }
        bool ReadJson(std::string& json)
        {
            ParseJson();

            ForceJsonValue(game, "th06");
            GetJsonValue(mode);
            GetJsonValue(stage);
            GetJsonValue(section);
            GetJsonValue(phase);
            GetJsonValueEx(dlg, Bool);
            GetJsonValue(frame);
            GetJsonValue(score);
            GetJsonValue(life);
            GetJsonValue(bomb);
            GetJsonValue(power);
            GetJsonValue(graze);
            GetJsonValue(point);
            GetJsonValue(rank);
            GetJsonValueEx(rankLock, Bool);
            GetJsonValue(fakeType);
            GetJsonValue(delay_st6bs9);
            GetJsonValueEx(wall_prac_st6, Bool);

            GetJsonValue(bF);
            GetJsonValue(bX1);
            GetJsonValue(bX2);
            GetJsonValue(bX3);
            GetJsonValue(bX4);
            GetJsonValue(bX5);
            GetJsonValue(bX6);
            GetJsonValue(bY1);
            GetJsonValue(bY2);
            GetJsonValue(bY3);
            GetJsonValue(bY4);
            GetJsonValue(bY5);
            GetJsonValue(bY6);

            GetJsonValue(snipeN);
            GetJsonValue(snipeF);

            return true;
        }
        std::string GetJson()
        {
            CreateJson();

            AddJsonValueEx(version, GetVersionStr(), jalloc);
            AddJsonValueEx(game, "th06", jalloc);
            AddJsonValue(mode);
            AddJsonValue(stage);
            if (section)
                AddJsonValue(section);
            if (phase)
                AddJsonValue(phase);
            if (frame)
                AddJsonValue(frame);
            if (dlg)
                AddJsonValue(dlg);

            AddJsonValue(score);
            AddJsonValueEx(life, (int)life);
            AddJsonValueEx(bomb, (int)bomb);
            AddJsonValueEx(power, (int)power);
            AddJsonValue(graze);
            AddJsonValue(point);
            AddJsonValue(rank);
            AddJsonValue(rankLock);
            AddJsonValue(fakeType);
            AddJsonValue(delay_st6bs9);
            AddJsonValue(wall_prac_st6);
            
            AddJsonValue(bF);
            AddJsonValue(bX1);
            AddJsonValue(bX2);
            AddJsonValue(bX3);
            AddJsonValue(bX4);
            AddJsonValue(bX5);
            AddJsonValue(bX6);
            AddJsonValue(bY1);
            AddJsonValue(bY2);
            AddJsonValue(bY3);
            AddJsonValue(bY4);
            AddJsonValue(bY5);
            AddJsonValue(bY6);

            AddJsonValue(snipeN);
            AddJsonValue(snipeF);

            ReturnJson();
        }
    };
    bool thRestartFlag = false;
    bool threstartflag_normalgame = false;
    

    THPracParam thPracParam {};

    class THOverlay : public Gui::GameGuiWnd {
        THOverlay() noexcept
        {
            SetTitle("Mod Menu");
            SetFade(0.5f, 0.5f);
            SetPos(10.0f, 10.0f);
            SetSize(0.0f, 0.0f);
            SetWndFlag(
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | 0);
            OnLocaleChange();
        }
        SINGLETON(THOverlay)
    public:

    protected:
        virtual void OnLocaleChange() override
        {
            float x_offset_1 = 0.0f;
            float x_offset_2 = 0.0f;
            switch (Gui::LocaleGet()) {
            case Gui::LOCALE_ZH_CN:
                x_offset_1 = 0.12f;
                x_offset_2 = 0.172f;
                break;
            case Gui::LOCALE_EN_US:
                x_offset_1 = 0.12f;
                x_offset_2 = 0.16f;
                break;
            case Gui::LOCALE_JA_JP:
                x_offset_1 = 0.18f;
                x_offset_2 = 0.235f;
                break;
            default:
                break;
            }

            mMenu.SetTextOffsetRel(x_offset_1, x_offset_2);
            mMuteki.SetTextOffsetRel(x_offset_1, x_offset_2);
            mInfLives.SetTextOffsetRel(x_offset_1, x_offset_2);
            mInfBombs.SetTextOffsetRel(x_offset_1, x_offset_2);
            mInfPower.SetTextOffsetRel(x_offset_1, x_offset_2);
            mTimeLock.SetTextOffsetRel(x_offset_1, x_offset_2);
            mAutoBomb.SetTextOffsetRel(x_offset_1, x_offset_2);
            mElBgm.SetTextOffsetRel(x_offset_1, x_offset_2);
            mShowSpellCapture.SetTextOffsetRel(x_offset_1, x_offset_2);
        }
        virtual void OnContentUpdate() override
        {
            mMuteki();
            mInfLives();
            mInfBombs();
            mInfPower();
            mTimeLock();
            mAutoBomb();
            mElBgm();
            mShowSpellCapture();
        }
        virtual void OnPreUpdate() override
        {
            if (mMenu(false) && (!ImGui::IsAnyItemActive() || *mShowSpellCapture)) {
                if (*mMenu) {
                    Open();
                } else {
                    Close();
                    SV_Unk198() = 3;
                }
            }
        }

        Gui::GuiHotKeyChord mMenu { "ModMenuToggle", "BACKSPACE", Gui::GetBackspaceMenuChord() };
        
    public:
        HOTKEY_DEFINE(mMuteki, TH_MUTEKI, "F1", VK_F1)
        PATCH_HK(0x4277c2, "03"),
        PATCH_HK(0x42779a, "83c4109090")
        HOTKEY_ENDDEF();

        HOTKEY_DEFINE(mInfBombs, TH_INFBOMBS, "F3", VK_F3)
        PATCH_HK(0x4289e3, "00")
        HOTKEY_ENDDEF();
        
        HOTKEY_DEFINE(mInfPower, TH_INFPOWER, "F4", VK_F4)
        PATCH_HK(0x428B7D, "00"),
        PATCH_HK(0x428B67, "909090909090909090")
        HOTKEY_ENDDEF();
        
        HOTKEY_DEFINE(mAutoBomb, TH_AUTOBOMB, "F6", VK_F6)
        PATCH_HK(0x428989, "EB1D"),
        PATCH_HK(0x4289B4, "85D2"),
        PATCH_HK(0x428A94, "FF89"),
        PATCH_HK(0x428A9D, "66C70504D9690002")
        HOTKEY_ENDDEF();

        HOTKEY_DEFINE(mInfLives, TH_INFLIVES2, "F2", VK_F2)
        PATCH_HK(0x428DDB, "a0bad469009090909090909090909090"),
        PATCH_HK(0x428AC6, "909090909090")
        HOTKEY_ENDDEF();
        
        HOTKEY_DEFINE(mTimeLock, TH_TIMELOCK, "F5", VK_F5)
        PATCH_HK(0x412DD1, "eb")
        HOTKEY_ENDDEF();

        Gui::GuiHotKey mElBgm { TH_EL_BGM, "F7", VK_F7 };
        Gui::GuiHotKey mShowSpellCapture { THPRAC_INGAMEINFO, "F8", VK_F8 };
    };

    class THGuiPrac : public Gui::GameGuiWnd {
        THGuiPrac() noexcept
        {
            *mLife = 8;
            *mBomb = 8;
            *mPower = 128;
            *mMode = 1;
            *mScore = 0;
            *mGraze = 0;
            *mRank = 32;

            SetFade(0.8f, 0.1f);
            SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
            SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
            OnLocaleChange();
        }
        SINGLETON(THGuiPrac)
    public:

        __declspec(noinline) void State(int state)
        {
            switch (state) {
            case 0:
                break;
            case 1:
                SetFade(0.8f, 0.1f);
                Open();
                mDiffculty = (int)GM_Difficulty();
                mShotType = (int)(GM_Character() * 2) + GM_ShotType();
                break;
            case 2:
                break;
            case 3:
                SetFade(0.8f, 0.8f);
                Close();
                *mNavFocus = 0;

                // Fill Param
                thPracParam.mode = *mMode;

                thPracParam.stage = *mStage;
                thPracParam.section = CalcSection();
                thPracParam.phase = *mPhase;
                thPracParam.frame = *mFrame;
                if (SectionHasDlg(thPracParam.section))
                    thPracParam.dlg = *mDlg;
                if (thPracParam.section == TH06_ST6_BOSS9)
                    thPracParam.delay_st6bs9 = *mDelaySt6Bs9;
                if (thPracParam.section == TH06_ST6_BOSS9 || thPracParam.section == TH06_ST6_BOSS6)
                {
                    thPracParam.wall_prac_st6 = *mWallPrac;
                    thPracParam.snipeF = *mWallPracSnipeF;
                    thPracParam.snipeN = *mWallPracSnipeN;
                }
                if (thPracParam.section == TH06_ST4_BOOKS) {
                    thPracParam.bF = 0;
                    thPracParam.bF |= ((int)*mBookC1)<<0;thPracParam.bX1 = *mBookX1;thPracParam.bY1 = *mBookY1;
                    thPracParam.bF |= ((int)*mBookC2)<<1;thPracParam.bX2 = *mBookX2;thPracParam.bY2 = *mBookY2;
                    thPracParam.bF |= ((int)*mBookC3)<<2;thPracParam.bX3 = *mBookX3;thPracParam.bY3 = *mBookY3;
                    thPracParam.bF |= ((int)*mBookC4)<<3;thPracParam.bX4 = *mBookX4;thPracParam.bY4 = *mBookY4;
                    thPracParam.bF |= ((int)*mBookC5)<<4;thPracParam.bX5 = *mBookX5;thPracParam.bY5 = *mBookY5;
                    thPracParam.bF |= ((int)*mBookC6)<<5;thPracParam.bX6 = *mBookX6;thPracParam.bY6 = *mBookY6;
                }

                thPracParam.score = *mScore;
                thPracParam.life = (float)*mLife;
                thPracParam.bomb = (float)*mBomb;
                thPracParam.power = (float)*mPower;
                thPracParam.graze = *mGraze;
                thPracParam.point = *mPoint;

                thPracParam.rank = *mRank;
                thPracParam.rankLock = *mRankLock;
                if (thPracParam.section >= TH06_ST4_BOSS1 && thPracParam.section <= TH06_ST4_BOSS7)
                    thPracParam.fakeType = *mFakeShot;
                break;
            case 4:
                Close();
                *mNavFocus = 0;
                break;
            case 5:
                // Fill Param
                thPracParam.mode = *mMode;

                thPracParam.stage = *mStage;
                thPracParam.section = CalcSection();
                thPracParam.phase = *mPhase;
                thPracParam.frame = *mFrame;
                if (SectionHasDlg(thPracParam.section))
                    thPracParam.dlg = *mDlg;
                if (thPracParam.section == TH06_ST6_BOSS9)
                    thPracParam.delay_st6bs9 = *mDelaySt6Bs9;
                if (thPracParam.section == TH06_ST6_BOSS9 || thPracParam.section == TH06_ST6_BOSS6) {
                    thPracParam.wall_prac_st6 = *mWallPrac;
                    thPracParam.snipeF = *mWallPracSnipeF;
                    thPracParam.snipeN = *mWallPracSnipeN;
                }
                if (thPracParam.section == TH06_ST4_BOOKS) {
                    thPracParam.bF = 0;
                    thPracParam.bF |= ((int)*mBookC1)<<0;thPracParam.bX1 = *mBookX1;thPracParam.bY1 = *mBookY1;
                    thPracParam.bF |= ((int)*mBookC2)<<1;thPracParam.bX2 = *mBookX2;thPracParam.bY2 = *mBookY2;
                    thPracParam.bF |= ((int)*mBookC3)<<2;thPracParam.bX3 = *mBookX3;thPracParam.bY3 = *mBookY3;
                    thPracParam.bF |= ((int)*mBookC4)<<3;thPracParam.bX4 = *mBookX4;thPracParam.bY4 = *mBookY4;
                    thPracParam.bF |= ((int)*mBookC5)<<4;thPracParam.bX5 = *mBookX5;thPracParam.bY5 = *mBookY5;
                    thPracParam.bF |= ((int)*mBookC6)<<5;thPracParam.bX6 = *mBookX6;thPracParam.bY6 = *mBookY6;
                }
                thPracParam.score = *mScore;
                thPracParam.life = (float)*mLife;
                thPracParam.bomb = (float)*mBomb;
                thPracParam.power = (float)*mPower;
                thPracParam.graze = *mGraze;
                thPracParam.point = *mPoint;

                thPracParam.rank = *mRank;
                thPracParam.rankLock = *mRankLock;
                if (thPracParam.section >= TH06_ST4_BOSS1 && thPracParam.section <= TH06_ST4_BOSS7)
                    thPracParam.fakeType = *mFakeShot;
                break;
            default:
                break;
            }
        }

        void SpellPhase()
        {
            auto section = CalcSection();
            if (section == TH06_ST7_END_S10) {
                mPhase(TH_PHASE, TH_SPELL_PHASE1);
            }else if (section == TH06_ST4_BOOKS) {
                mPhase(TH_PHASE, TH_BOOKS_PHASE_INF_TIME);
                if (*mPhase == 4){
                    float w = ImGui::GetColumnWidth();
                    ImGui::Columns(3);
                    ImGui::SetColumnWidth(0, 0.2f*w);
                    ImGui::SetColumnWidth(1, 0.4f*w);
                    ImGui::SetColumnWidth(2, 0.4f*w);

                    mBookC1();ImGui::NextColumn(); mBookX1();ImGui::NextColumn();mBookY1();ImGui::NextColumn();
                    mBookC2();ImGui::NextColumn(); mBookX2();ImGui::NextColumn();mBookY2();ImGui::NextColumn();
                    mBookC3();ImGui::NextColumn(); mBookX3();ImGui::NextColumn();mBookY3();ImGui::NextColumn();
                    mBookC4();ImGui::NextColumn(); mBookX4();ImGui::NextColumn();mBookY4();ImGui::NextColumn();
                    mBookC5();ImGui::NextColumn(); mBookX5();ImGui::NextColumn();mBookY5();ImGui::NextColumn();
                    mBookC6();ImGui::NextColumn(); mBookX6();ImGui::NextColumn();mBookY6();ImGui::NextColumn();
                    ImGui::Columns(1);
                    if (ImGui::Button(S(TH_BOOK_MIRROR))) {
                        *mBookX4 = -*mBookX3;
                        *mBookX5 = -*mBookX2;
                        *mBookX6 = -*mBookX1;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(S(TH_BOOK_MIRROR2))) {
                        *mBookX1 = -*mBookX1;
                        *mBookX2 = -*mBookX2;
                        *mBookX3 = -*mBookX3;
                        *mBookX4 = -*mBookX4;
                        *mBookX5 = -*mBookX5;
                        *mBookX6 = -*mBookX6;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(S(TH_BOOK_ROLL))) {
                        int temp = *mBookX6;
                        *mBookX6 = *mBookX5;
                        *mBookX5 = *mBookX4;
                        *mBookX4 = *mBookX3;
                        *mBookX3 = *mBookX2;
                        *mBookX2 = *mBookX1;
                        *mBookX1 = temp;
                    }
                    if (ImGui::Button(S(TH_BOOK_RANDX))) {
                        static std::default_random_engine rand_engine(timeGetTime());
                        static std::uniform_int_distribution<int32_t> rand_value(-192, 192);
                        *mBookX1 = rand_value(rand_engine);
                        *mBookX2 = rand_value(rand_engine);
                        *mBookX3 = rand_value(rand_engine);
                        *mBookX4 = rand_value(rand_engine);
                        *mBookX5 = rand_value(rand_engine);
                        *mBookX6 = rand_value(rand_engine);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(S(TH_BOOK_RESETY))) {
                        *mBookY1 = 32;
                        *mBookY2 = 128;
                        *mBookY3 = 144;
                        *mBookY4 = 64;
                        *mBookY5 = 80;
                        *mBookY6 = 96;
                    }
                    if (ImGui::Button(S(TH_BOOK_COPY_SETTING)))
                    {
                        { char _buf[256]; snprintf(_buf, sizeof(_buf), "(%d,%d),(%d,%d),(%d,%d),(%d,%d),(%d,%d),(%d,%d)",
                            (int)*mBookX1, (int)*mBookY1, (int)*mBookX2, (int)*mBookY2, (int)*mBookX3, (int)*mBookY3,
                            (int)*mBookX4, (int)*mBookY4, (int)*mBookX5, (int)*mBookY5, (int)*mBookX6, (int)*mBookY6);
                        ImGui::SetClipboardText(_buf); }
                    }
                    if (ImGui::Button(S(TH_BOOK_PASTE_SETTING))){
                        int xs[6] = { 0 }, ys[6] = { 32,128,144,64,80,96 };
                        auto text = ImGui::GetClipboardText();
                        if (text){
                            int n = 0;
                            while (isspace(text[n]) && text[n+1] != 0)
                                n++;
                            // trim
                            if (text && sscanf_s(text + n, "(%d,%d),(%d,%d),(%d,%d),(%d,%d),(%d,%d),(%d,%d)", &xs[0], &ys[0], &xs[1], &ys[1], &xs[2], &ys[2], &xs[3], &ys[3], &xs[4], &ys[4], &xs[5], &ys[5])) {
                                *mBookX1 = xs[0], *mBookY1 = ys[0],
                                *mBookX2 = xs[1], *mBookY2 = ys[1],
                                *mBookX3 = xs[2], *mBookY3 = ys[2],
                                *mBookX4 = xs[3], *mBookY4 = ys[3],
                                *mBookX5 = xs[4], *mBookY5 = ys[4],
                                *mBookX6 = xs[5], *mBookY6 = ys[5];
                            }
                        }
                       
                    }
                }
            }else if (section == TH06_ST5_BOSS6) {
                mPhase(TH_PHASE, TH_EOSD_SAKUYA_DOLLS);
            }else if (section == TH06_ST6_BOSS9){
                mPhase(TH_PHASE, TH06_FINAL_SPELL);
                if (*mPhase == 1 || *mPhase == 2 || *mPhase == 3)
                    mDelaySt6Bs9();
                mWallPrac();
                if (*mWallPrac) {
                    mWallPracSnipeN("%d%%");
                    mWallPracSnipeF("%d%%");
                }
            } else if (section == TH06_ST6_BOSS6) {
                mWallPrac();
                if (*mWallPrac){
                    mWallPracSnipeN("%d%%");
                    mWallPracSnipeF("%d%%");
                }
            }
        }
        void PracticeMenu(Gui::GuiNavFocus& nav_focus)
        {
            mMode();
            if (mStage())
                *mSection = *mChapter = 0;
            if (*mMode == 1) {
                if (mWarp()) {
                    *mSection = *mChapter = *mPhase = *mFrame = 0, *mDelaySt6Bs9 = 120,*mWallPrac = false,*mWallPracSnipeF=30,*mWallPracSnipeN=0;
                    //(-180,32),(-116,128),(-61,144),(41,64),(112,80),(180,96)
                    //(-180,32),(-12,128),(-72,144),(68,64),(130,80),(180,96)
                    *mBookC1 = true,*mBookX1 = -180, *mBookY1 = 32;
                    *mBookC2 = true,*mBookX2 = -116, *mBookY2 = 128;
                    *mBookC3 = true, *mBookX3 = -61, *mBookY3 = 144;
                    *mBookC4 = true,*mBookX4 = 41, *mBookY4 = 64;
                    *mBookC5 = true,*mBookX5 = 112, *mBookY5 = 80;
                    *mBookC6 = true,*mBookX6 = 180, *mBookY6 = 96;
                }
                if (*mWarp) {
                    int st = 0;
                    if (*mStage == 3) {
                        mFakeShot();
                        st = (*mFakeShot ? *mFakeShot - 1 : mShotType) + 4;
                    }
                    SectionWidget();
                    SpellPhase();
                }

                mLife();
                mBomb();
                mScore();
                mScore.RoundDown(10);
                mPower();
                mGraze();
                mPoint();
                mRank();
                if (mRankLock()) {
                    if (*mRankLock)
                        mRank.SetBound(0, 99);
                    else
                        mRank.SetBound(0, 32);
                }
            }

            nav_focus();
        }

    protected:
        virtual void OnLocaleChange() override
        {
            SetTitle(S(TH_MENU));
            switch (Gui::LocaleGet()) {
            case Gui::LOCALE_ZH_CN:
                SetSize(330.f, 390.f);
                SetPos(260.f, 65.f);
                SetItemWidth(-60.0f);
                break;
            case Gui::LOCALE_EN_US:
                SetSize(370.f, 375.f);
                SetPos(240.f, 75.f);
                SetItemWidth(-60.0f);
                break;
            case Gui::LOCALE_JA_JP:
                SetSize(330.f, 390.f);
                SetPos(260.f, 65.f);
                SetItemWidth(-65.0f);
                break;
            default:
                break;
            }
        }
        virtual void OnContentUpdate() override
        {
            ImGui::TextUnformatted(S(TH_MENU));
            ImGui::Separator();

            PracticeMenu(mNavFocus);
        }
        int CalcSection()
        {
            int chapterId = 0;
            switch (*mWarp) {
            case 1: // Chapter
                // Chapter Id = 10000 + Stage * 100 + Section
                chapterId += (*mStage + 1) * 100;
                chapterId += *mChapter;
                chapterId += 10000; // Base of chapter ID is 1000.
                return chapterId;
                break;
            case 2:
            case 3: // Mid boss & End boss
                return th_sections_cba[*mStage][*mWarp - 2][*mSection];
                break;
            case 4:
            case 5: // Non-spell & Spellcard
                return th_sections_cbt[*mStage][*mWarp - 4][*mSection];
                break;
            default:
                return 0;
                break;
            }
        }
        bool SectionHasDlg(int32_t section)
        {
            switch (section) {
            case TH06_ST1_BOSS1:
            case TH06_ST2_BOSS1:
            case TH06_ST3_BOSS1:
            case TH06_ST4_BOSS1:
            case TH06_ST5_BOSS1:
            case TH06_ST5_MID1:
            case TH06_ST6_BOSS1:
            case TH06_ST6_MID1:
            case TH06_ST7_END_NS1:
            case TH06_ST7_MID1:
                return true;
            default:
                return false;
            }
        }
        void SectionWidget()
        {
            static char chapterStr[256] {};
            auto& chapterCounts = mChapterSetup[*mStage];

            int st = 0;
            if (*mStage == 3) {
                st = (*mFakeShot ? *mFakeShot - 1 : mShotType) + 4;
            }

            switch (*mWarp) {
            case 1: // Chapter
                mChapter.SetBound(1, chapterCounts[0] + chapterCounts[1]);

                if (chapterCounts[1] == 0) {
                    sprintf_s(chapterStr, S(TH_STAGE_PORTION_N), *mChapter);
                } else if (*mChapter <= chapterCounts[0]) {
                    sprintf_s(chapterStr, S(TH_STAGE_PORTION_1), *mChapter);
                } else {
                    sprintf_s(chapterStr, S(TH_STAGE_PORTION_2), *mChapter - chapterCounts[0]);
                };

                mChapter(chapterStr);
                break;
            case 2:
            case 3: // Mid boss & End boss
                if (mSection(TH_WARP_SELECT_FRAME[*mWarp],
                    th_sections_cba[*mStage + st][*mWarp - 2],
                    th_sections_str[::THPrac::Gui::LocaleGet()][mDiffculty]))
                    *mPhase = 0;
                if (SectionHasDlg(th_sections_cba[*mStage][*mWarp - 2][*mSection]))
                    mDlg();
                break;
            case 4:
            case 5: // Non-spell & Spellcard
                if (mSection(TH_WARP_SELECT_FRAME[*mWarp],
                    th_sections_cbt[*mStage + st][*mWarp - 4],
                    th_sections_str[::THPrac::Gui::LocaleGet()][mDiffculty]))
                    *mPhase = 0;
                if (SectionHasDlg(th_sections_cbt[*mStage][*mWarp - 4][*mSection]))
                    mDlg();
                break;
            case 6:
                mFrame();
                break;
            }
        }


        // Data
        Gui::GuiCombo mMode { TH_MODE, TH_MODE_SELECT };
        Gui::GuiCombo mStage { TH_STAGE, TH_STAGE_SELECT };
        Gui::GuiCombo mWarp { TH_WARP, TH_WARP_SELECT_FRAME };
        Gui::GuiCombo mSection { TH_MODE };
        Gui::GuiCombo mPhase { TH_PHASE };
        Gui::GuiCheckBox mDlg { TH_DLG };

        Gui::GuiSlider<int, ImGuiDataType_S32> mChapter { TH_CHAPTER, 0, 0 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mFrame { TH_FRAME, 0, INT_MAX };
        Gui::GuiSlider<int, ImGuiDataType_S32> mLife { TH_LIFE, 0, 8 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mBomb { TH_BOMB, 0, 8 };
        Gui::GuiDrag<int64_t, ImGuiDataType_S64> mScore { TH_SCORE, 0, 9999999990, 10, 100000000 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mPower { TH_POWER, 0, 128 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mGraze { TH_GRAZE, 0, 99999, 1, 10000 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mPoint { TH_POINT, 0, 9999, 1, 1000 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mDelaySt6Bs9 { TH_DELAY, 0, 600, 1, 10,10};
        Gui::GuiCheckBox mWallPrac { TH06_ST6_WALL_PRAC };


        Gui::GuiDrag<int, ImGuiDataType_S32> mBookX1 { TH_BOOK_X1, -192, 192, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookY1 { TH_BOOK_Y1, -50, 448, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookX2 { TH_BOOK_X2, -192, 192, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookY2 { TH_BOOK_Y2, -50, 448, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookX3 { TH_BOOK_X3, -192, 192, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookY3 { TH_BOOK_Y3, -50, 448, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookX4 { TH_BOOK_X4, -192, 192, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookY4 { TH_BOOK_Y4, -50, 448, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookX5 { TH_BOOK_X5, -192, 192, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookY5 { TH_BOOK_Y5, -50, 448, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookX6 { TH_BOOK_X6, -192, 192, 1, 10, 10 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mBookY6 { TH_BOOK_Y6, -50, 448, 1, 10, 10 };

        Gui::GuiSlider<int, ImGuiDataType_S32> mWallPracSnipeF { TH06_ST6_WALL_PRAC_SNIPE_F, 0, 100};
        Gui::GuiSlider<int, ImGuiDataType_S32> mWallPracSnipeN { TH06_ST6_WALL_PRAC_SNIPE_N, 0, 100};

        Gui::GuiCheckBox                           mBookC1 { TH_BOOK_C1};
        Gui::GuiCheckBox                           mBookC2 { TH_BOOK_C2};
        Gui::GuiCheckBox                           mBookC3 { TH_BOOK_C3};
        Gui::GuiCheckBox                           mBookC4 { TH_BOOK_C4};
        Gui::GuiCheckBox                           mBookC5 { TH_BOOK_C5};
        Gui::GuiCheckBox                           mBookC6 { TH_BOOK_C6};


        Gui::GuiSlider<int, ImGuiDataType_S32> mRank { TH06_RANK, 0, 32, 1, 10, 10 };
        Gui::GuiCheckBox mRankLock { TH06_RANKLOCK };
        Gui::GuiCombo mFakeShot { TH06_FS, TH06_TYPE_SELECT };

        Gui::GuiNavFocus mNavFocus { TH_STAGE, TH_MODE, TH_WARP, TH_FRAME,
            TH_MID_STAGE, TH_END_STAGE, TH_NONSPELL, TH_SPELL, TH_PHASE, TH_CHAPTER,
            TH_LIFE, TH_BOMB, TH_SCORE, TH_POWER, TH_GRAZE, TH_POINT,
            TH06_RANK, TH06_RANKLOCK, TH06_FS,
            TH_BOOK_X1,TH_BOOK_Y1,
            TH_BOOK_X2,TH_BOOK_Y2,
            TH_BOOK_X3,TH_BOOK_Y3,
            TH_BOOK_X4,TH_BOOK_Y4,
            TH_BOOK_X5,TH_BOOK_Y5,
            TH_BOOK_X6,TH_BOOK_Y6,
        };

        int mChapterSetup[7][2] {
            { 4, 2 },
            { 2, 2 },
            { 4, 3 },
            { 4, 5 },
            { 3, 2 },
            { 2, 0 },
            { 4, 3 }
        };

        float mStep = 10.0;
        int mDiffculty = 0;
        int mShotType = 0;
    };
    
    class THPauseMenu : public Gui::GameGuiWnd {
        THPauseMenu() noexcept
        {
            SetTitle("Pause Menu");
            SetFade(0.8f, 0.1f);
            SetSize(384.f, 448.f);
            SetPos(32.f, 16.f);
            SetItemWidth(-60);
            SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
            SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
        }
        SINGLETON(THPauseMenu)
    public:

        bool el_bgm_signal { false };
        bool el_bgm_changed { false };

        enum state {
            STATE_CLOSE = 0,
            STATE_RESUME = 1,
            STATE_EXIT = 2,
            STATE_RESTART = 3,
            STATE_OPEN = 4,
            STATE_EXIT2 = 5,
        };
        enum signal {
            SIGNAL_NONE = 0,
            SIGNAL_RESUME = 1,
            SIGNAL_EXIT = 2,
            SIGNAL_RESTART = 3,
            SIGNAL_EXIT2 = 4,
        };
        signal PMState()
        {
            switch (mState) {
            case THPrac::TH06::THPauseMenu::STATE_CLOSE:
                return StateClose();
            case THPrac::TH06::THPauseMenu::STATE_RESUME:
                return StateResume();
            case THPrac::TH06::THPauseMenu::STATE_EXIT:
                return StateExit();
            case THPrac::TH06::THPauseMenu::STATE_EXIT2:
                return StateExit2();
            case THPrac::TH06::THPauseMenu::STATE_RESTART:
                return StateRestart();
            case THPrac::TH06::THPauseMenu::STATE_OPEN:
                return StateOpen();
            default:
                return SIGNAL_NONE;
            }
        }

    protected:
        signal StateRestart()
        {
            if (mState != STATE_RESTART) {
                mState = STATE_RESTART;
                mFrameCounter = 0;
            }

            if (mFrameCounter == 1) {
                *mNavFocus = 0;
                inSettings = false;

                auto oldMode = thPracParam.mode;
                auto oldStage = thPracParam.stage;
                auto oldBgmFlag = THBGMTest();
                thRestartFlag = true;
                THGuiPrac::singleton().State(5);
                if (*(THOverlay::singleton().mElBgm) && !el_bgm_changed && oldMode == thPracParam.mode && oldStage == thPracParam.stage && oldBgmFlag == THBGMTest()) {
                    el_bgm_signal = true;
                }

                Close();
            } else if (mFrameCounter == 10) {
                StateClose();
                return SIGNAL_RESTART;
            }

            return SIGNAL_NONE;
        }
        signal StateExit()
        {
            if (mState != STATE_EXIT) {
                mState = STATE_EXIT;
                mFrameCounter = 0;
            }

            if (mFrameCounter == 1) {
                *mNavFocus = 0;
                inSettings = false;
                Close();
            } else if (mFrameCounter == 10) {
                StateClose();
                return SIGNAL_EXIT;
            }

            return SIGNAL_NONE;
        }
        signal StateExit2()
        {
            if (mState != STATE_EXIT2) {
                mState = STATE_EXIT2;
                mFrameCounter = 0;
            }
            if (mFrameCounter == 1) {
                *mNavFocus = 0;
                inSettings = false;
                Close();
            } else if (mFrameCounter == 10) {
                StateClose();
                return SIGNAL_EXIT2;
            }
            return SIGNAL_NONE;
        }
        signal StateResume()
        {
            if (mState != STATE_RESUME) {
                mState = STATE_RESUME;
                mFrameCounter = 0;
            }

            if (mFrameCounter == 1) {
                *mNavFocus = 0;
                inSettings = false;
                Close();
            } else if (mFrameCounter == 10) {
                StateClose();
                return SIGNAL_RESUME;
            }

            return SIGNAL_NONE;
        }
        signal StateClose()
        {
            if (mState != STATE_CLOSE) {
                mState = STATE_CLOSE;
                mFrameCounter = 0;
            }

            if (mFrameCounter > 5) {
                return StateOpen();
            }

            return SIGNAL_NONE;
        }
        signal StateOpen()
        {
            if (mState != STATE_OPEN) {
                mState = STATE_OPEN;
                mFrameCounter = 0;
            }

            if (mFrameCounter == 1) {
                Open();
            }
            if (mFrameCounter > 10) {
                if (Gui::KeyboardInputGetSingle(VK_ESCAPE))
                    StateResume();
                else if (Gui::KeyboardInputGetRaw('Q'))
                    StateExit2();
                else if (Gui::KeyboardInputGetRaw('R'))
                    StateRestart();
            }
            if (g_fast_re_opt.fast_retry_count_down && g_fast_re_opt.fast_retry_count_down <= 1)
                StateRestart();

            return SIGNAL_NONE;
        }

        virtual void OnPreUpdate() override
        {
            if (mFrameCounter < UINT_MAX)
                mFrameCounter++;
        }
        virtual void OnLocaleChange() override
        {
            switch (Gui::LocaleGet()) {
            case Gui::LOCALE_ZH_CN:
                SetItemWidth(-60.0f);
                break;
            case Gui::LOCALE_EN_US:
                SetItemWidth(-60.0f);
                break;
            case Gui::LOCALE_JA_JP:
                SetItemWidth(-65.0f);
                break;
            default:
                break;
            }
        }
        virtual void OnContentUpdate() override
        {
            if (!inSettings) {
                ImGui::Dummy(ImVec2(10.0f, 140.0f));
                ImGui::Indent(119.0f);
                if (mResume())
                    StateResume();
                if (ImGui::IsItemFocused()) {
                    WORD key = GM_CurInput();
                    WORD key_last = GM_LastInput();
                    if (((key & (16)) == 16 && (key & (16)) != (key_last & (16)))) { // up
                        mNavFocus.ForceFocus(TH_TWEAK);
                    }
                }
                ImGui::Spacing();
                if (mExit())
                    StateExit();
                ImGui::Spacing();
                if (mExit2())
                    StateExit2();
                ImGui::Spacing();
                if (mRestart())
                    StateRestart();
                ImGui::Spacing();
                if (mSettings())
                    inSettings = !inSettings;
                if (ImGui::IsItemFocused()) {
                    WORD key = GM_CurInput();
                    WORD key_last = GM_LastInput();
                    if (((key & (32)) == 32 && (key & (32)) != (key_last & (32)))) { // down
                        mNavFocus.ForceFocus(TH_RESUME);
                    }
                }
                ImGui::Spacing();
                ImGui::Unindent();
                mNavFocus();
            } else {
                ImGui::Dummy(ImVec2(10.0f, 10.0f));
                ImGui::Indent(119.0f);
                if (mResume())
                    StateResume();
                ImGui::Spacing();
                if (mExit())
                    StateExit();
                ImGui::Spacing();
                if (mExit2())
                    StateExit2();
                ImGui::Spacing();
                if (mRestart())
                    StateRestart();
                ImGui::Spacing();
                if (mSettings())
                    inSettings = !inSettings;
                ImGui::Spacing();
                ImGui::Unindent(67.0f);
                THGuiPrac::singleton().PracticeMenu(mNavFocus);
            }
        }

        // Var
        state mState = STATE_CLOSE;
        unsigned int mFrameCounter = 0;
        bool inSettings = false;

        Gui::GuiButton mResume { TH_RESUME, 130.0f, 25.0f };
        Gui::GuiButton mExit { TH_EXIT, 130.0f, 25.0f };
        Gui::GuiButton mExit2 { TH_EXIT2, 130.0f, 25.0f };
        Gui::GuiButton mRestart { TH_RESTART, 130.0f, 25.0f };
        Gui::GuiButton mSettings { TH_TWEAK, 130.0f, 25.0f };

        Gui::GuiNavFocus mNavFocus { TH_RESUME, TH_EXIT, TH_EXIT2, TH_RESTART, TH_TWEAK,
            TH_STAGE, TH_MODE, TH_WARP,
            TH_MID_STAGE, TH_END_STAGE, TH_NONSPELL, TH_SPELL, TH_PHASE,
            TH_LIFE, TH_BOMB, TH_SCORE, TH_POWER, TH_GRAZE, TH_POINT,
            TH06_RANK, TH06_RANKLOCK, TH06_FS };
    };

    class THGuiRep : public Gui::GameGuiWnd {
        THGuiRep() noexcept
        {
        }
        SINGLETON(THGuiRep)
    public:

        void CheckReplay()
        {
            uint32_t index = th06::g_MainMenu.chosenReplay;
            char* raw = th06::g_MainMenu.replayFilePaths[index];

            std::string param;
            if (ReplayLoadParam(mb_to_utf16(raw, 932).c_str(), param) && mRepParam.ReadJson(param))
                mParamStatus = true;
            else
                mRepParam.Reset();
        }

        bool mRepStatus = false;
        void State(int state)
        {
            switch (state) {
            case 1:
                mRepStatus = false;
                mParamStatus = false;
                thPracParam.Reset();
                break;
            case 2:
                CheckReplay();
                break;
            case 3:
                mRepStatus = true;
                if (mParamStatus)
                    memcpy(&thPracParam, &mRepParam, sizeof(THPracParam));
                break;
            default:
                break;
            }
        }

    protected:
        bool mParamStatus = false;
        THPracParam mRepParam;
    };

    
    class TH06InGameInfo : public Gui::GameGuiWnd {
        TH06InGameInfo() noexcept
        {
            SetTitle("igi");
            SetFade(0.9f, 0.9f);
            SetSizeRel(180.0f / 640.0f, 0.0f);
            SetPosRel(433.0f / 640.0f, 245.0f / 480.0f);
            SetWndFlag(ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | 0);
            OnLocaleChange();
        }
        SINGLETON(TH06InGameInfo)

    public:
        int32_t mMissCount = 0;
        struct BooksInfo {
            bool is_books;
            int32_t time_books;
            bool is_died;
            int32_t miss_count;
            int32_t bomb_count;
        } booksInfo;

        void GameStartInit()
        {
            booksInfo.is_books = false;
            booksInfo.time_books = 0;
            booksInfo.is_died = false;
            booksInfo.miss_count = 0;
            booksInfo.bomb_count = 0;
        }
        void GameRunInit()
        {
            mMissCount = 0;
        }

    protected:
        bool detail_open = false;
        virtual void OnLocaleChange() override
        {
            float x_offset_1 = 0.0f;
            float x_offset_2 = 0.0f;
            switch (Gui::LocaleGet()) {
            case Gui::LOCALE_ZH_CN:
                x_offset_1 = 0.12f;
                x_offset_2 = 0.172f;
                break;
            case Gui::LOCALE_EN_US:
                x_offset_1 = 0.12f;
                x_offset_2 = 0.16f;
                break;
            case Gui::LOCALE_JA_JP:
                x_offset_1 = 0.18f;
                x_offset_2 = 0.235f;
                break;
            default:
                break;
            }
        }

        virtual void OnContentUpdate() override
        {
            int32_t mBombCount = GM_BombsUsed();

            byte cur_player_typea = GM_Character();
            byte cur_player_typeb = GM_ShotType();
            byte cur_player_type = (cur_player_typea << 1) | cur_player_typeb;
            int8_t diff = (int8_t)GM_Difficulty();
            auto diff_pl = std::string(S(IGI_DIFF[diff])) + " (" + S(IGI_PL_06[cur_player_type]) + ")";
            auto diff_pl_sz = ImGui::CalcTextSize(diff_pl.c_str());

            ImGui::SetCursorPosX(ImGui::GetWindowSize().x * 0.5f - diff_pl_sz.x * 0.5f);
            ImGui::Text(diff_pl.c_str());

            ImGui::Columns(2);
            ImGui::Text(S(THPRAC_INGAMEINFO_MISS_COUNT));
            ImGui::NextColumn();
            ImGui::Text("%8d", mMissCount);
            ImGui::NextColumn();
            ImGui::Text(S(THPRAC_INGAMEINFO_BOMB_COUNT));
            ImGui::NextColumn();
            ImGui::Text("%8d", mBombCount);

            if (g_adv_igi_options.th06_showRank) {
                ImGui::NextColumn();
                ImGui::Text(S(THPRAC_INGAMEINFO_TH06_RANK));
                ImGui::NextColumn();
                ImGui::Text("%8d.%02d", GM_Rank(), GM_SubRank());
            }

            ImGui::Columns(1);

            bool cur_is_in_spell = EM_SpellActive();

            if (booksInfo.is_books || cur_is_in_spell) {
                byte cur_spell_id = booksInfo.is_books ? TH06Save::spellid_books : (byte)EM_SpellIdx();

                ImGui::Text("%s", th06_spells_str[Gui::LocaleGet()][cur_spell_id]);
                int tot_sc_caped = TH06Save::singleton().GetTotalSpellCardCount(cur_spell_id, diff, cur_player_type, TH06Save::Capture);
                int tot_sc_tot = TH06Save::singleton().GetTotalSpellCardCount(cur_spell_id, diff, cur_player_type, TH06Save::Attempt);
                int tot_sc_to = TH06Save::singleton().GetTotalSpellCardCount(cur_spell_id, diff, cur_player_type, TH06Save::Timeout);
                ImGui::Text("%d/%d(%.1f%%); %d", tot_sc_caped, tot_sc_tot, (float)(tot_sc_caped) / std::fmax(1.0f, tot_sc_tot) * 100.0f, tot_sc_to);
                int cur_sc_caped = TH06Save::singleton().GetCurrentSpellCardCount(cur_spell_id, diff, cur_player_type, TH06Save::Capture);
                int cur_sc_tot = TH06Save::singleton().GetCurrentSpellCardCount(cur_spell_id, diff, cur_player_type, TH06Save::Attempt);
                int cur_sc_to = TH06Save::singleton().GetCurrentSpellCardCount(cur_spell_id, diff, cur_player_type, TH06Save::Timeout);
                ImGui::Text("%d/%d(%.1f%%); %d", cur_sc_caped, cur_sc_tot, (float)(cur_sc_caped) / std::fmax(1.0f, cur_sc_tot) * 100.0f, cur_sc_to);

                if (booksInfo.is_books && thPracParam.mode && (thPracParam.phase != 0)) { // books
                    ImGui::Text("%.1f", (float)booksInfo.time_books / 60.0f);
                }
            }
        }
        virtual void OnPreUpdate() override
        {
            DWORD gameState = SV_CurState();
            if (gameState == 2)
            {
                GameUpdateInner(6);
            }
            if (*THOverlay::singleton().mShowSpellCapture && (gameState == 2)) {
                SetPosRel(433.0f / 640.0f, 245.0f / 480.0f);
                SetSizeRel(180.0f / 640.0f, 0.0f);
                Open();
            } else {
                Close();
                SV_Unk198() = 3;
            }
        }
    };


    float g_bossMoveDownRange = BOSS_MOVE_DOWN_RANGE_INIT;
    EHOOK_ST(th06_bossmovedown, 0x0040917F, 5, {
        //float* left = (float*)(pCtx->Ecx + 0xE60);
        float* top = (float*)(pCtx->Ecx + 0xE64);
        //float* right = (float*)(pCtx->Ecx + 0xE68);
        float* bottom = (float*)(pCtx->Ecx + 0xE6C);
        float range = *bottom - *top;
        *top = *bottom - range * (1.0f - g_bossMoveDownRange);
    });

    HOOKSET_DEFINE(th06_rankdown_disable)
    PATCH_DY(th06_rankdown_disable1, 0x428C34, "909090909090909090909090909090")
    PATCH_DY(th06_rankdown_disable2, 0x428A55, "909090909090909090909090909090")
    HOOKSET_ENDDEF()

    HOOKSET_DEFINE(TH06BgFix)
    // fix igi render problem
    PATCH_DY(th06_background_fix_1, 0x42073B, "909090909090")
    PATCH_DY(th06_background_fix_2, 0x419F4B, "909090909090")
    PATCH_DY(th06_background_fix_3, 0x419F81, "909090909090")
    EHOOK_DY(th06_stage_color_fix, 0x4039E5, 3,
        {
            pCtx->Edx = 0x00000000;
        })
    HOOKSET_ENDDEF()


    class THAdvOptWnd : public Gui::PPGuiWnd {
        SINGLETON(THAdvOptWnd)
        // Option Related Functions

    private:
        bool mDeveloperMode = false;

        void FpsInit()
        {
            // SDL2 build: native game speed control via framerateMultiplier
            mOptCtx.fps_status = 1; // enable FPS UI
            mOptCtx.fps = 60;
        }
        void FpsSet()
        {
            // CE-style: change frame timing, keep game logic at 1.0
            g_speed_multiplier = (float)mOptCtx.fps / 60.0f;
        }
        void GameplayInit()
        {
        }
        void GameplaySet()
        {
        }

        THAdvOptWnd() noexcept
        {
            SetWndFlag(ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
            SetFade(0.8f, 0.8f);
            SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
            SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);

            InitUpdFunc([&]() { ContentUpdate(); },
                [&]() { LocaleUpdate(); },
                [&]() {},
                []() {});

            OnLocaleChange();
            FpsInit();

            for (int i = 0; i < 2; i++)
                th06_rankdown_disable[i].Setup();
            for (int i = 0; i < 2; i++)
                th06_rankdown_disable[i].Toggle(g_adv_igi_options.th06_disable_drop_rank);
            th06_bossmovedown.Setup();
            th06_bossmovedown.Toggle(false);
            GameplayInit();
        }

    public:
        bool forceBossMoveDown = false;
        __declspec(noinline) static bool StaticUpdate()
        {
            auto& advOptWnd = THAdvOptWnd::singleton();

            if (Gui::GetChordPressed(Gui::GetAdvancedMenuChord())) {
                if (advOptWnd.IsOpen())
                    advOptWnd.Close();
                else
                    advOptWnd.Open();
            }
            advOptWnd.Update();

            return advOptWnd.IsOpen();
        }

    protected:
        void LocaleUpdate()
        {
            SetTitle(S(TH_SPELL_PRAC));
            switch (Gui::LocaleGet()) {
            case Gui::LOCALE_ZH_CN:
                SetSizeRel(1.0f, 1.0f);
                SetPosRel(0.0f, 0.0f);
                SetItemWidthRel(-0.0f);
                SetAutoSpacing(true);
                break;
            case Gui::LOCALE_EN_US:
                SetSizeRel(1.0f, 1.0f);
                SetPosRel(0.0f, 0.0f);
                SetItemWidthRel(-0.0f);
                SetAutoSpacing(true);
                break;
            case Gui::LOCALE_JA_JP:
                SetSizeRel(1.0f, 1.0f);
                SetPosRel(0.0f, 0.0f);
                SetItemWidthRel(-0.0f);
                SetAutoSpacing(true);
                break;
            default:
                break;
            }
        }

        bool RenderDebugValueRow(const char* label, const char* sliderId, const char* inputId, int* value, int minValue,
                                 int maxValue, const char* displayFormat)
        {
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(260.0f);
            const bool sliderChanged = ImGui::SliderInt(sliderId, value, minValue, maxValue, displayFormat);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            const bool inputChanged = ImGui::InputInt(inputId, value);
            return sliderChanged || inputChanged;
        }

        void RenderNetworkDebuggerContent()
        {
            th06::Netplay::DebugNetworkConfig config = th06::Netplay::GetDebugNetworkConfig();
            const bool available = IsNetplayDebuggerAvailable();
            const th06::Netplay::Snapshot snapshot = th06::Netplay::GetSnapshot();

            ImGui::TextWrapped("%s", NetworkDebuggerSummary());
            ImGui::Spacing();

            if (!available)
            {
                ImGui::TextDisabled("%s", NetworkDebuggerUnavailableHint());
                return;
            }

            bool dirty = false;
            if (ImGui::Checkbox(NetDebugEnableLabel(), &config.enabled))
            {
                dirty = true;
            }

            dirty |= RenderDebugValueRow(NetDebugLatencyLabel(), "##netdebug_latency_slider", "##netdebug_latency_input",
                                         &config.latencyMs, 0, 1000, "%d ms");
            dirty |= RenderDebugValueRow(NetDebugJitterLabel(), "##netdebug_jitter_slider", "##netdebug_jitter_input",
                                         &config.jitterMs, 0, 500, "%d ms");
            dirty |= RenderDebugValueRow(NetDebugPacketLossLabel(), "##netdebug_loss_slider", "##netdebug_loss_input",
                                         &config.packetLossPercent, 0, 100, "%d%%");
            dirty |= RenderDebugValueRow(NetDebugDuplicateLabel(), "##netdebug_dup_slider", "##netdebug_dup_input",
                                         &config.duplicatePercent, 0, 100, "%d%%");

            if (ImGui::Button(NetDebugResetLabel()))
            {
                config = {};
                dirty = true;
            }

            ClampDebugNetworkConfig(config);
            if (dirty)
            {
                th06::Netplay::SetDebugNetworkConfig(config);
            }

            ImGui::Separator();
            const std::string localizedStatusText = th06::OnlineMenu::LocalizeNetplayStatusText(snapshot.statusText);
            ImGui::Text("%s: %s", NetDebugSessionStateLabel(), localizedStatusText.c_str());
        }

        void RenderAdvancedFeaturesContent()
        {
            if (BeginOptGroup<TH_GAME_SPEED>()) {
                if (GameFPSOpt(mOptCtx))
                    FpsSet();
                EndOptGroup();
            }
            {
                if (ImGui::Checkbox(S(TH06_RANKLOCK_DOWN), &g_adv_igi_options.th06_disable_drop_rank)){

                    for (int i = 0; i < 2; i++)
                        th06_rankdown_disable[i].Toggle(g_adv_igi_options.th06_disable_drop_rank);
                }
                ImGui::SameLine();
                HelpMarker(S(TH06_RANKLOCK_DOWN_DESC));
                if (ImGui::IsKeyDown(0x10)) // shift
                {
                    if (ImGui::IsKeyPressed('C'))
                    {
                        g_adv_igi_options.th06_disable_drop_rank = !g_adv_igi_options.th06_disable_drop_rank;
                        for (int i = 0; i < 2; i++)
                            th06_rankdown_disable[i].Toggle(g_adv_igi_options.th06_disable_drop_rank);
                    }
                }
            }
            DisableKeyOpt();
            KeyHUDOpt();
            InfLifeOpt();
            if (ImGui::Checkbox(S(TH_BOSS_FORCE_MOVE_DOWN), &forceBossMoveDown)) {
                th06_bossmovedown.Toggle(forceBossMoveDown);
            }
            ImGui::SameLine();
            HelpMarker(S(TH_BOSS_FORCE_MOVE_DOWN_DESC));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::DragFloat(S(TH_BOSS_FORCE_MOVE_DOWN_RANGE), &g_bossMoveDownRange, 0.002f, 0.0f, 1.0f))
                g_bossMoveDownRange = std::clamp(g_bossMoveDownRange, 0.0f, 1.0f);

            ImGui::Checkbox(S(TH_ENABLE_LOCK_TIMER), &g_adv_igi_options.enable_lock_timer_autoly);
            const bool forceRunInBackground = th06::OnlineMenu::ShouldForceRunInBackground();
            bool effectiveRunInBackground = g_adv_igi_options.th06_run_in_background || forceRunInBackground;
            if (forceRunInBackground) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Checkbox(NoFreezeOnFocusLossLabel(), &effectiveRunInBackground) && !forceRunInBackground) {
                g_adv_igi_options.th06_run_in_background = effectiveRunInBackground;
            }
            if (forceRunInBackground) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", NoFreezeOnFocusLossLockedHint());
            }
            ImGui::SameLine();
            HelpMarker(NoFreezeOnFocusLossDesc());

            ImGui::Checkbox(S(THPRAC_SHOW_BULLET_HITBOX), &g_show_bullet_hitbox);

            ImGui::Checkbox(S(THPRAC_INGAMEINFO_TH06_SHOW_RANK), &g_adv_igi_options.th06_showRank);
            ImGui::Checkbox(S(THPRAC_INGAMEINFO_TH06_SHOW_HITBOX), &g_adv_igi_options.th06_show_hitbox);
            ImGui::SameLine();
            HelpMarker(S(THPRAC_INGAMEINFO_TH06_SHOW_HITBOX_DESC));
            ImGui::SameLine();
            if (ImGui::Button(S(THPRAC_INGAMEINFO_TH06_SHOW_HITBOX_RELOAD))) {
                if (g_hitbox_textureID)
                {
                    ((LPDIRECT3DTEXTURE8)g_hitbox_textureID)->Release();
                    g_hitbox_textureID = nullptr;
                }
                g_hitbox_textureID = ReadImage(8, 0, "hitbox.png", hitbox_file, sizeof(hitbox_file)); // SDL2: device unused
                D3DSURFACE_DESC desc;
                ((LPDIRECT3DTEXTURE8)g_hitbox_textureID)->GetLevelDesc(0, &desc);
                g_hitbox_sz.x = static_cast<float>(desc.Width),g_hitbox_sz.y = static_cast<float>(desc.Height);
            }
            ImGui::Checkbox(S(THPRAC_TH06_SHOW_REP_MARKER), &g_adv_igi_options.th06_showRepMarker);
            ImGui::Checkbox(S(THPRAC_TH06_FIX_RAND_SEED), &g_adv_igi_options.th06_fix_seed);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputInt(S(THPRAC_TH06_RAND_SEED), &g_adv_igi_options.th06_seed)){
                g_adv_igi_options.th06_seed = std::clamp(g_adv_igi_options.th06_seed, 0, 65535);
            }
            ImGui::Text("%s: %d",S(THPRAC_TH06_REP_RAND_SEED),g_last_rep_seed);

            if (ImGui::Checkbox(S(THPRAC_TH06_BACKGROUND_FIX), &g_adv_igi_options.th06_bg_fix))
            {
                if (g_adv_igi_options.th06_bg_fix)
                    EnableAllHooks(TH06BgFix);
                else
                    DisableAllHooks(TH06BgFix);
            }
            HelpMarker(S(THPRAC_INGAMEINFO_ADV_DESC1));
            ImGui::SameLine();
            HelpMarker(S(THPRAC_INGAMEINFO_ADV_DESC2));

            SSS::SSS_UI(6);

            {
                ImGui::SetNextWindowCollapsed(false);
                if (ImGui::CollapsingHeader(S(THPRAC_INGAMEINFO_06_SHOWDETAIL_COLLAPSE)))
                {
                    ShowDetail(nullptr);
                    ImGui::NewLine();
                    ImGui::Separator();
                    ImGui::Separator();
                    ImGui::Separator();
                    ImGui::NewLine();
                }
                ImGui::Text("%s:(%d,%d),(%d,%d),(%d,%d),(%d,%d),(%d,%d),(%d,%d)", S(TH_BOOK_LAST), 
                    (int)(g_books_pos[0].x - 192.0f), (int)g_books_pos[0].y,
                    (int)(g_books_pos[1].x - 192.0f), (int)g_books_pos[1].y,
                    (int)(g_books_pos[2].x - 192.0f), (int)g_books_pos[2].y,
                    (int)(g_books_pos[3].x - 192.0f), (int)g_books_pos[3].y,
                    (int)(g_books_pos[4].x - 192.0f), (int)g_books_pos[4].y,
                    (int)(g_books_pos[5].x - 192.0f), (int)g_books_pos[5].y
                );
                if (ImGui::Button(S(TH_BOOK_COPY_SETTING))) {
                    { char _buf[256]; snprintf(_buf, sizeof(_buf), "(%d,%d),(%d,%d),(%d,%d),(%d,%d),(%d,%d),(%d,%d)",
                        (int)(g_books_pos[0].x - 192.0f), (int)g_books_pos[0].y,
                        (int)(g_books_pos[1].x - 192.0f), (int)g_books_pos[1].y,
                        (int)(g_books_pos[2].x - 192.0f), (int)g_books_pos[2].y,
                        (int)(g_books_pos[3].x - 192.0f), (int)g_books_pos[3].y,
                        (int)(g_books_pos[4].x - 192.0f), (int)g_books_pos[4].y,
                        (int)(g_books_pos[5].x - 192.0f), (int)g_books_pos[5].y);
                    ImGui::SetClipboardText(_buf); }
                }
            }
            InGameReactionTestOpt();
            AboutOpt();
        }

    void RenderDeveloperFeaturesContent()
    {
        ImGui::Checkbox(DebugLogOutputLabel(), &g_adv_igi_options.th06_enable_debug_logs);
            ImGui::SameLine();
            HelpMarker(DebugLogOutputDesc());

#ifndef _WIN32
            ImGui::BeginDisabled();
#endif
            ImGui::Checkbox(ManualDumpHotkeyLabel(), &g_adv_igi_options.th06_enable_manual_dump_hotkey);
#ifndef _WIN32
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", ManualDumpHotkeyUnavailableHint());
#endif
            ImGui::SameLine();
            HelpMarker(ManualDumpHotkeyDesc());

#ifndef _WIN32
            ImGui::BeginDisabled();
#endif
        ImGui::Checkbox(RecoveryAutoDumpLabel(), &g_adv_igi_options.th06_enable_recovery_auto_dump);
#ifndef _WIN32
        ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", ManualDumpHotkeyUnavailableHint());
#endif
        ImGui::SameLine();
        HelpMarker(RecoveryAutoDumpDesc());

        ImGui::Separator();
        ImGui::Checkbox(
            TrLocal("启用新触控", "Enable New Touch", "新タッチ入力を有効にする"),
            &g_new_touch_enabled);
        ImGui::SameLine();
        HelpMarker(TrLocal(
            "启用后，桌面端鼠标点击将模拟触控输入（左键=点击，右键=返回）。安卓端始终启用触控。",
            "When enabled, desktop mouse clicks simulate touch input (left click = tap, right click = back). Touch is always enabled on Android.",
            "有効にすると、デスクトップでマウスクリックがタッチ入力をシミュレートします（左クリック＝タップ、右クリック＝戻る）。Android では常に有効です。"));

        if (g_new_touch_enabled)
        {
            ImGui::Checkbox(
                TrLocal("自机跟随鼠标", "Player Follows Mouse", "自機がマウスに追従"),
                &g_mouse_follow_enabled);
            ImGui::SameLine();
            HelpMarker(TrLocal(
                "启用后，游戏中自机的位置直接由鼠标位置决定。支持录像录制和回放。",
                "When enabled, the player character position follows the mouse cursor during gameplay. Supports replay recording and playback.",
                "有効にすると、ゲーム中の自機がマウスカーソルの位置に直接移動します。リプレイの録画と再生に対応しています。"));
            if (g_mouse_follow_enabled)
                g_mouse_touch_drag_enabled = false;

            ImGui::Checkbox(
                TrLocal("使用鼠标模拟手指拖拽", "Mouse Simulates Finger Drag", "マウスで指ドラッグをシミュレート"),
                &g_mouse_touch_drag_enabled);
            ImGui::SameLine();
            HelpMarker(TrLocal(
                "启用后，鼠标拖拽将模拟触控拖拽移动自机（相对位移，非跟随）。与「自机跟随鼠标」互斥。",
                "When enabled, mouse drag simulates touch-drag to move the player character (relative displacement, not following). Mutually exclusive with Player Follows Mouse.",
                "有効にすると、マウスドラッグがタッチドラッグをシミュレートして自機を移動します。「自機がマウスに追従」とは排他的です。"));
            if (g_mouse_touch_drag_enabled)
                g_mouse_follow_enabled = false;
        }
        else
        {
            g_mouse_follow_enabled = false;
            g_mouse_touch_drag_enabled = false;
        }

        ImGui::Separator();
        ImGui::Text("%s", AstroBotSectionLabel());
        const bool enabledChanged =
            ImGui::Checkbox(AstroBotEnableLabel(), &g_adv_igi_options.th06_enable_astrobot);
        if (enabledChanged && g_adv_igi_options.th06_enable_astrobot && g_adv_igi_options.th06_astrobot_target == 0)
        {
            g_adv_igi_options.th06_astrobot_target = 1;
        }
        ImGui::Text("%s", AstroBotTargetLabel());
        const bool isRemoteNetplay = th06::Session::IsRemoteNetplaySession();
        int target = std::clamp<int>(g_adv_igi_options.th06_astrobot_target, 0, 2);
        if (isRemoteNetplay && target != 0)
        {
            target = 1;
        }
        ImGui::RadioButton(AstroBotTargetName(0), &target, 0);
        ImGui::SameLine();
        ImGui::RadioButton(AstroBotTargetName(1), &target, 1);
        if (!isRemoteNetplay)
        {
            ImGui::SameLine();
            ImGui::RadioButton(AstroBotTargetName(2), &target, 2);
        }
        g_adv_igi_options.th06_astrobot_target = static_cast<uint8_t>(target);
        ImGui::SameLine();
        HelpMarker(AstroBotTargetHint());
        ImGui::Checkbox(AstroBotAutoShootLabel(), &g_adv_igi_options.th06_astrobot_auto_shoot);
        ImGui::Checkbox(AstroBotAutoBombLabel(), &g_adv_igi_options.th06_astrobot_auto_bomb);
        ImGui::Checkbox(AstroBotBossOnlyLabel(), &g_adv_igi_options.th06_astrobot_boss_only);

        const th06::AstroBot::StatusSnapshot botStatus = th06::AstroBot::GetStatusSnapshot();
        const char* stateText = botStatus.eligible
                                    ? (botStatus.active ? TrLocal("运行中", "running", "稼働中")
                                                        : TrLocal("待机", "idle", "待機"))
                                    : AstroBotBypassReasonName(botStatus.bypassReason);
        ImGui::Text("%s: %s | %s | ownership=%s | danger=%.1f | forecast=%d | mode=%s | focus=%s | bomb=%s | bombCD=%s",
                    AstroBotStatusLabel(),
                    stateText,
                    AstroBotActionName(botStatus.action), AstroBotOwnershipName(botStatus), botStatus.danger, botStatus.forecastThreatCount,
                    AstroBotModeName(botStatus.modeHint),
                    botStatus.focus ? TrLocal("是", "yes", "はい") : TrLocal("否", "no", "いいえ"),
                    botStatus.willBomb ? TrLocal("是", "yes", "はい") : TrLocal("否", "no", "いいえ"),
                    botStatus.bombCoolingDown ? TrLocal("是", "yes", "はい") : TrLocal("否", "no", "いいえ"));
    }

        void RenderPortableRestoreContent()
        {
            bool enabled = th06::SinglePlayerSnapshot::IsPortableRestoreTrialEnabled();
            if (ImGui::Checkbox(PortableRestoreTrialLabel(), &enabled))
            {
                th06::SinglePlayerSnapshot::SetPortableRestoreTrialEnabled(enabled);
            }
            ImGui::SameLine();
            HelpMarker(PortableRestoreTrialDesc());
            ImGui::Spacing();
            ImGui::TextWrapped("%s", PortableRestoreRuntimeHint());
        }

        void ShowDetail(bool* isOpen)
        {
            ImGui::BeginTabBar("Detail Spell");
            {
                const char* tabs_diff[5] = { S(THPRAC_IGI_DIFF_E), S(THPRAC_IGI_DIFF_N), S(THPRAC_IGI_DIFF_H), S(THPRAC_IGI_DIFF_L), S(THPRAC_IGI_DIFF_EX) };
                for (int diff = 0; diff < 5; diff++) {
                    if (ImGui::BeginTabItem(tabs_diff[diff])) {
                        ImGui::BeginTabBar("Player Type");
                        const char* tabs_player[4] = { S(THPRAC_IGI_PL_ReimuA), S(THPRAC_IGI_PL_ReimuB), S(THPRAC_IGI_PL_MarisaA), S(THPRAC_IGI_PL_MarisaB) };
                        for (int pl = 0; pl < 4; pl++) {
                            if (ImGui::BeginTabItem(tabs_player[pl])) {
                                // time
                                { char _tid[128]; snprintf(_tid, sizeof(_tid), "%s%stimetable", tabs_diff[diff], tabs_player[pl]);
                                if (ImGui::BeginTable(_tid, 4,
                                    ImGuiTableFlags_::ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_::ImGuiTableFlags_SizingStretchSame)) {
                                ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_GAMTIME_TOT));
                                ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_GAMTIME_CUR));
                                ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_GAMTIME_CHARACTER_TOT));
                                ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_GAMTIME_ALL));
                                ImGui::TableHeadersRow();

                                int64_t gametime_tot = TH06Save::singleton().GetTotalGameTime(diff,pl);
                                int64_t gametime_cur = TH06Save::singleton().GetCurrentGameTime(diff, pl);
                                int64_t gametime_chartot = 0;
                                for (int i = 0; i < 5; i++)
                                    gametime_chartot += TH06Save::singleton().GetTotalGameTime(i, pl);
                                int64_t gametime_all = 0;
                                for (int i = 0; i < 5; i++)
                                    for (int j = 0; j < 4; j++)
                                        gametime_all += TH06Save::singleton().GetTotalGameTime(i, j);

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped(GetTime_HHMMSS(gametime_tot).c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped(GetTime_HHMMSS(gametime_cur).c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped(GetTime_HHMMSS(gametime_chartot).c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped(GetTime_HHMMSS(gametime_all).c_str());

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped(GetTime_YYMMDD_HHMMSS(gametime_tot).c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped(GetTime_YYMMDD_HHMMSS(gametime_cur).c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped(GetTime_YYMMDD_HHMMSS(gametime_chartot).c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped(GetTime_YYMMDD_HHMMSS(gametime_all).c_str());

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped("(%lld %s)", gametime_tot, S(THPRAC_INGAMEINFO_06_GAMTIME_NANOSECOND));
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped("(%lld %s)", gametime_cur, S(THPRAC_INGAMEINFO_06_GAMTIME_NANOSECOND));
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped("(%lld %s)", gametime_chartot, S(THPRAC_INGAMEINFO_06_GAMTIME_NANOSECOND));
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped("(%lld %s)", gametime_all, S(THPRAC_INGAMEINFO_06_GAMTIME_NANOSECOND));

                                ImGui::EndTable();
                                } }
                                // spell capture
                                { char _sid[128]; snprintf(_sid, sizeof(_sid), "%s%ssptable", tabs_diff[diff], tabs_player[pl]);
                                if (ImGui::BeginTable(_sid, 5,
                                    ImGuiTableFlags_::ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_::ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_SPELL_NAME), ImGuiTableColumnFlags_::ImGuiTableColumnFlags_WidthFixed, 100.0f);
                                ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_CAPTURE_TOT), ImGuiTableColumnFlags_::ImGuiTableColumnFlags_WidthFixed, 50.0f);
                                ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_TIMEOUT_TOT), ImGuiTableColumnFlags_::ImGuiTableColumnFlags_WidthFixed, 30.0f);
                                ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_CAPTURE_CUR), ImGuiTableColumnFlags_::ImGuiTableColumnFlags_WidthFixed, 50.0f);
                                ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_TIMEOUT_CUR), ImGuiTableColumnFlags_::ImGuiTableColumnFlags_WidthFixed, 30.0f);
                                ImGui::TableHeadersRow();

                                for (int spell = 0; spell < 65; spell++) {
                                    if (is_spell_used[diff][spell]) {
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();
                                        ImGui::Text("%s", th06_spells_str[Gui::LocaleGet()][spell]);
                                        ImGui::TableNextColumn();

                                        ImGui::Text("%d/%d(%.1f%%)",
                                                    TH06Save::singleton().GetTotalSpellCardCount(spell, diff, pl, TH06Save::Capture),
                                                    TH06Save::singleton().GetTotalSpellCardCount(spell, diff, pl, TH06Save::Attempt),
                                            ((float)TH06Save::singleton().GetTotalSpellCardCount(spell, diff, pl, TH06Save::Capture)) /
                                           std::fmax(1.0f,((float)TH06Save::singleton().GetTotalSpellCardCount(spell, diff, pl, TH06Save::Attempt))) * 100.0f);
                                        ImGui::TableNextColumn();

                                        ImGui::Text("%d", TH06Save::singleton().GetTotalSpellCardCount(spell, diff, pl, TH06Save::Timeout));
                                        ImGui::TableNextColumn();

                                        ImGui::Text("%d/%d(%.1f%%)",
                                            TH06Save::singleton().GetCurrentSpellCardCount(spell, diff, pl, TH06Save::Capture),
                                            TH06Save::singleton().GetCurrentSpellCardCount(spell, diff, pl, TH06Save::Attempt),
                                            ((float)TH06Save::singleton().GetCurrentSpellCardCount(spell, diff, pl, TH06Save::Capture)) / 
                                            std::fmax(1.0f, ((float)TH06Save::singleton().GetCurrentSpellCardCount(spell, diff, pl, TH06Save::Attempt))) * 100.0f);
                                        ImGui::TableNextColumn();

                                        ImGui::Text("%d", TH06Save::singleton().GetCurrentSpellCardCount(spell, diff, pl, TH06Save::Timeout));
                                    }
                                }
                                ImGui::EndTable();
                                } }
                                ImGui::EndTabItem();
                            }
                        }
                        ImGui::EndTabBar();
                        ImGui::EndTabItem();
                    }
                }
            }
            ImGui::EndTabBar();
            if (isOpen != nullptr && ImGui::Button(S(TH_BACK))) {
                *isOpen = false;
            }
        }
        void ContentUpdate()
        {
            SV_Unk198() = 3;
            ImGui::TextUnformatted(S(TH_ADV_OPT));
            ImGui::SameLine();
            ImGui::Checkbox(DeveloperModeLabel(), &mDeveloperMode);
            g_developer_mode_enabled = mDeveloperMode;
            ImGui::SameLine();
            HelpMarker(DeveloperModeDesc());
            ImGui::Separator();
            ImGui::BeginChild("Adv. Options", ImVec2(0.0f, 0.0f));
            if (mDeveloperMode && ImGui::BeginTabBar("TH06DeveloperTabs"))
            {
                if (ImGui::BeginTabItem(AdvancedFeaturesTabLabel()))
                {
                    RenderAdvancedFeaturesContent();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(DeveloperFeaturesTabLabel()))
                {
                    RenderDeveloperFeaturesContent();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(NetworkDebuggerTabLabel()))
                {
                    RenderNetworkDebuggerContent();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(PortableRestoreTabLabel()))
                {
                    RenderPortableRestoreContent();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            else
            {
                RenderAdvancedFeaturesContent();
            }
            ImGui::EndChild();
            ImGui::SetWindowFocus();
        }

        adv_opt_ctx mOptCtx;


    };

    class THFirstRunWnd : public Gui::GameGuiWnd {
        SINGLETON(THFirstRunWnd)
        bool mChecked;

        THFirstRunWnd() noexcept
        {
            mChecked = false;
            SetWndFlag(ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
            SetFade(0.95f, 0.8f);
            OnLocaleChange();
        }

    public:
        __declspec(noinline) static bool StaticUpdate()
        {
            auto& wnd = THFirstRunWnd::singleton();
            if (!wnd.mChecked) {
                wnd.mChecked = true;
                // unk[1] == 0 means first-run prompt not yet shown.
                // LoadConfig writes defaults with unk[1]=0 on first run.
                if (th06::g_Supervisor.cfg.unk[1] == 0) {
                    wnd.Open();
                }
            }
            wnd.Update();
            return wnd.IsOpen();
        }

    protected:
        void OnLocaleChange() override
        {
            SetTitle(S(TH06_FIRSTRUN_TITLE));
            SetSizeRel(0.5f, 0.3f);
            AutoPos(0.5f, 0.5f);
            SetItemWidthRel(0.0f);
        }

        void OnContentUpdate() override
        {
            ImGui::TextWrapped("%s", S(TH06_FIRSTRUN_MSG));
            ImGui::Separator();

            float btnW = ImGui::GetContentRegionAvail().x * 0.45f;
            float spacing = ImGui::GetContentRegionAvail().x - btnW * 2.0f;

            if (ImGui::Button(S(TH06_FIRSTRUN_FULLSCREEN), ImVec2(btnW, 0.f))) {
                th06::g_Supervisor.cfg.windowed = false;
                th06::g_Supervisor.cfg.unk[1] = 1; // mark first-run prompt done
                th06::FileSystem::WriteDataToFile((char*)"th06.cfg", &th06::g_Supervisor.cfg, sizeof(th06::g_Supervisor.cfg));
                th06::SetWindowMode(false);
                Close();
            }
            ImGui::SameLine(0.0f, spacing);
            if (ImGui::Button(S(TH06_FIRSTRUN_WINDOWED), ImVec2(btnW, 0.f))) {
                th06::g_Supervisor.cfg.windowed = true;
                th06::g_Supervisor.cfg.unk[1] = 1; // mark first-run prompt done
                th06::FileSystem::WriteDataToFile((char*)"th06.cfg", &th06::g_Supervisor.cfg, sizeof(th06::g_Supervisor.cfg));
                th06::SetWindowMode(true);
                Close();
            }
        }
    };

    class THConfigWnd : public Gui::GameGuiWnd {
        SINGLETON(THConfigWnd)

        // Working copy of config, applied on OK
        bool mOptNoVertexBuf;
        bool mOptNoFog;
        bool mOptForce16BitTex;
        bool mOptNoGouraud;
        bool mOptNoColorComp;
        bool mOptRefRasterizer;
        bool mOptClearBackbuf;
        bool mOptMinGraphics;
        bool mOptNoDepthTest;
        bool mOptForce60FPS;
        int  mWindowed;       // 0=fullscreen, 1=windowed
        int  mFrameskip;      // 0,1,2
        int  mColorMode;      // 0=32bit, 1=16bit
        int  mPadXAxis;
        int  mPadYAxis;
        int  mBackend;        // 0=GLES (shader), 1=GL (fixed-function)
        int  mOrigBackend;    // backend value at window open time
        bool mShowSaveMsg;
        int  mSaveMsgTimer;
        bool mShowRestartPopup;

        THConfigWnd() noexcept
        {
            SetWndFlag(ImGuiWindowFlags_NoCollapse);
            SetFade(0.95f, 0.8f);
            OnLocaleChange();
            LoadFromGame();
            mShowSaveMsg = false;
            mSaveMsgTimer = 0;
            mShowRestartPopup = false;
            mOrigBackend = mBackend;
        }

        void LoadFromGame()
        {
            auto& cfg = th06::g_Supervisor.cfg;
            mOptNoVertexBuf   = (cfg.opts >> th06::GCOS_DONT_USE_VERTEX_BUF) & 1;
            mOptNoFog         = (cfg.opts >> th06::GCOS_DONT_USE_FOG) & 1;
            mOptForce16BitTex = (cfg.opts >> th06::GCOS_FORCE_16BIT_COLOR_MODE) & 1;
            mOptNoGouraud     = (cfg.opts >> th06::GCOS_SUPPRESS_USE_OF_GOROUD_SHADING) & 1;
            mOptNoColorComp   = (cfg.opts >> th06::GCOS_NO_COLOR_COMP) & 1;
            mOptRefRasterizer = (cfg.opts >> th06::GCOS_REFERENCE_RASTERIZER_MODE) & 1;
            mOptClearBackbuf  = (cfg.opts >> th06::GCOS_CLEAR_BACKBUFFER_ON_REFRESH) & 1;
            mOptMinGraphics   = (cfg.opts >> th06::GCOS_DISPLAY_MINIMUM_GRAPHICS) & 1;
            mOptNoDepthTest   = (cfg.opts >> th06::GCOS_TURN_OFF_DEPTH_TEST) & 1;
            mOptForce60FPS    = (cfg.opts >> th06::GCOS_FORCE_60FPS) & 1;
            mWindowed         = cfg.windowed ? 1 : 0;
            mFrameskip        = cfg.frameskipConfig;
            if (mFrameskip > 2) mFrameskip = 0;
            mColorMode        = (cfg.colorMode16bit == 0xFF || cfg.colorMode16bit == 0) ? 0 : 1;
            mPadXAxis         = cfg.padXAxis;
            mPadYAxis         = cfg.padYAxis;
            mBackend          = (cfg.unk[0] == 1) ? 1 : 0;
            mOrigBackend      = mBackend;
        }

        void ApplyToGame()
        {
            auto& cfg = th06::g_Supervisor.cfg;
            auto setBit = [&](int shift, bool val) {
                if (val) cfg.opts |=  (1u << shift);
                else     cfg.opts &= ~(1u << shift);
            };
            setBit(th06::GCOS_DONT_USE_VERTEX_BUF, mOptNoVertexBuf);
            setBit(th06::GCOS_DONT_USE_FOG, mOptNoFog);
            setBit(th06::GCOS_FORCE_16BIT_COLOR_MODE, mOptForce16BitTex);
            setBit(th06::GCOS_SUPPRESS_USE_OF_GOROUD_SHADING, mOptNoGouraud);
            setBit(th06::GCOS_NO_COLOR_COMP, mOptNoColorComp);
            setBit(th06::GCOS_REFERENCE_RASTERIZER_MODE, mOptRefRasterizer);
            setBit(th06::GCOS_CLEAR_BACKBUFFER_ON_REFRESH, mOptClearBackbuf);
            setBit(th06::GCOS_DISPLAY_MINIMUM_GRAPHICS, mOptMinGraphics);
            setBit(th06::GCOS_TURN_OFF_DEPTH_TEST, mOptNoDepthTest);
            setBit(th06::GCOS_FORCE_60FPS, mOptForce60FPS);
            cfg.windowed = mWindowed ? 1 : 0;
            cfg.frameskipConfig = (u8)mFrameskip;
            cfg.colorMode16bit = mColorMode ? 1 : 0;
            cfg.padXAxis = (i16)std::clamp(mPadXAxis, 1, 1000);
            cfg.padYAxis = (i16)std::clamp(mPadYAxis, 1, 1000);
            cfg.unk[0] = (i8)mBackend;
            // Save to disk
            th06::FileSystem::WriteDataToFile((char*)"th06.cfg", &cfg, sizeof(cfg));
        }

    public:
        __declspec(noinline) static bool StaticUpdate()
        {
            auto& wnd = THConfigWnd::singleton();
            if (Gui::KeyboardInputGetSingle(VK_OEM_3)) {
                if (wnd.IsOpen()) {
                    wnd.Close();
                } else {
                    wnd.LoadFromGame();
                    wnd.mShowSaveMsg = false;
                    wnd.Open();
                }
            }
            wnd.Update();
            return wnd.IsOpen();
        }

    protected:
        void OnLocaleChange() override
        {
            SetTitle(S(TH06_CFG_TITLE));
            SetSizeRel(0.65f, 0.85f);
            AutoPos(0.5f, 0.5f);
        }

        void OnContentUpdate() override
        {
            ImGui::TextUnformatted(S(TH06_CFG_TITLE));
            ImGui::Separator();

            // Scrollable content region (leave space for footer buttons)
            float footerH = ImGui::GetFrameHeightWithSpacing() * 2.f + 8.f;
            ImGui::BeginChild("cfgScroll", ImVec2(0, -footerH), false);

            // Checkboxes for opts flags
            ImGui::Checkbox(S(TH06_CFG_NO_VERTEX_BUF), &mOptNoVertexBuf);
            ImGui::Checkbox(S(TH06_CFG_NO_FOG), &mOptNoFog);
            ImGui::Checkbox(S(TH06_CFG_FORCE_16BIT_TEX), &mOptForce16BitTex);
            ImGui::Checkbox(S(TH06_CFG_NO_GOURAUD), &mOptNoGouraud);
            ImGui::Checkbox(S(TH06_CFG_NO_COLOR_COMP), &mOptNoColorComp);
            ImGui::Checkbox(S(TH06_CFG_REF_RASTERIZER), &mOptRefRasterizer);
            ImGui::Checkbox(S(TH06_CFG_CLEAR_BACKBUF), &mOptClearBackbuf);
            ImGui::Checkbox(S(TH06_CFG_MIN_GRAPHICS), &mOptMinGraphics);
            ImGui::Checkbox(S(TH06_CFG_NO_DEPTH_TEST), &mOptNoDepthTest);
            ImGui::Checkbox(S(TH06_CFG_FORCE_60FPS), &mOptForce60FPS);

            ImGui::Separator();

            // Window style
            ImGui::TextUnformatted(S(TH06_CFG_WINDOW_STYLE));
            ImGui::SameLine();
            int prevWindowed = mWindowed;
            ImGui::RadioButton(S(TH06_CFG_FULLSCREEN), &mWindowed, 0);
            ImGui::SameLine();
            ImGui::RadioButton(S(TH06_CFG_WINDOWED), &mWindowed, 1);
            if (mWindowed != prevWindowed) {
                th06::g_Supervisor.cfg.windowed = mWindowed ? 1 : 0;
                th06::SetWindowMode(mWindowed != 0);
            }

            // Frame skip
            ImGui::TextUnformatted(S(TH06_CFG_FRAMESKIP));
            ImGui::SameLine();
            ImGui::RadioButton(S(TH06_CFG_FRAMESKIP_FULL), &mFrameskip, 0);
            ImGui::SameLine();
            ImGui::RadioButton(S(TH06_CFG_FRAMESKIP_HALF), &mFrameskip, 1);
            ImGui::SameLine();
            ImGui::RadioButton(S(TH06_CFG_FRAMESKIP_THIRD), &mFrameskip, 2);

            // Color mode
            ImGui::TextUnformatted(S(TH06_CFG_COLOR_MODE));
            ImGui::SameLine();
            ImGui::RadioButton(S(TH06_CFG_32BIT), &mColorMode, 0);
            ImGui::SameLine();
            ImGui::RadioButton(S(TH06_CFG_16BIT), &mColorMode, 1);

            ImGui::Separator();

            // Pad axis
            ImGui::SetNextItemWidth(120.f);
            ImGui::InputInt(S(TH06_CFG_PAD_X_SPEED), &mPadXAxis);
            mPadXAxis = std::clamp(mPadXAxis, 1, 1000);
            ImGui::SetNextItemWidth(120.f);
            ImGui::InputInt(S(TH06_CFG_PAD_Y_SPEED), &mPadYAxis);
            mPadYAxis = std::clamp(mPadYAxis, 1, 1000);

            // Renderer backend
            ImGui::Separator();
            ImGui::TextUnformatted(S(TH06_CFG_RENDERER_BACKEND));
            ImGui::SameLine();
            ImGui::RadioButton(S(TH06_CFG_RENDERER_GLES), &mBackend, 0);
            ImGui::SameLine();
            ImGui::RadioButton(S(TH06_CFG_RENDERER_GL), &mBackend, 1);

            ImGui::EndChild(); // end scrollable region

            ImGui::Separator();

            // OK / Cancel
            if (ImGui::Button(S(TH06_CFG_OK), ImVec2(120.f, 0.f))) {
                ApplyToGame();
                mShowSaveMsg = true;
                mSaveMsgTimer = 180;
                if (mBackend != mOrigBackend) {
                    mShowRestartPopup = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(S(TH06_CFG_CANCEL), ImVec2(120.f, 0.f))) {
                Close();
            }

            // Restart confirmation popup — ID must match between OpenPopup and BeginPopupModal
            if (mShowRestartPopup) {
                ImGui::OpenPopup(S(TH06_CFG_RESTART_TITLE));
            }
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal(S(TH06_CFG_RESTART_TITLE), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(S(TH06_CFG_RESTART_MSG));
                ImGui::Separator();
                if (ImGui::Button(S(TH06_CFG_RESTART_YES), ImVec2(140.f, 0.f))) {
                    // In-process restart: re-creates window and renderer from current config
                    mShowRestartPopup = false;
                    ImGui::CloseCurrentPopup();
                    Close();
                    th06::RequestRestart();
                }
                ImGui::SameLine();
                if (ImGui::Button(S(TH06_CFG_RESTART_NO), ImVec2(140.f, 0.f))) {
                    mShowRestartPopup = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Save confirmation
            if (mShowSaveMsg && mSaveMsgTimer > 0) {
                mSaveMsgTimer--;
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", S(TH06_CFG_SAVE_SUCCESS));
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", S(TH06_CFG_NEED_RESTART));
            }
            if (mSaveMsgTimer <= 0) {
                mShowSaveMsg = false;
            }
        }
    };

    // ECL Patch Helper

    void ECLJump(ECLHelper& ecl,int32_t time,int32_t pos, int32_t time_jmp,int32_t target)
    {
        ecl << pair { pos, (int32_t)time } << pair { pos + 4, (int16_t)0x2 } << pair { pos + 6, (int16_t)0x14 } << pair { pos + 8, (int32_t)0x00FFFF00 };
        ecl << pair { pos + 0xC, (int32_t)time_jmp } << pair { pos + 0xC + 4, (int32_t)target - pos };
    }

    void ECLWarp(int32_t time)
    {
        EM_TimelineCurrent() = time;
    }
    void ECLSetHealth(ECLHelper& ecl, int offset, int32_t ecl_time, int32_t time)
    {
        ecl.SetPos(offset);
        ecl << ecl_time << 0x0010006f << 0x00ffff00 << time;
    }
    void ECLSetTime(ECLHelper& ecl, int offset, int32_t ecl_time, int32_t health)
    {
        ecl.SetPos(offset);
        ecl << ecl_time << 0x00100073 << 0x00ffff00 << health;
    }
    void ECLStall(ECLHelper& ecl, int offset)
    {
        ecl.SetPos(offset);
        ecl << 0x99999 << 0x000c0000 << 0x0000ff00;
    }
    void ECLNameFix()
    {
        if (thPracParam.stage == 5) {
            th06::g_AnmManager->LoadAnm(11, "data/eff06.anm", 691);
        } else if (thPracParam.stage == 6) {
            th06::g_AnmManager->LoadAnm(11, "data/eff07.anm", 691);
            th06::g_AnmManager->LoadAnm(18, "data/face12c.anm", 1192);
        }
    }
    __declspec(noinline) void THPatch(ECLHelper& ecl, th_sections_t section)
    {
        int shot;
        auto s2b_nd = [&]() {
            ECLWarp(0x1760);
            ecl << pair{0x18fc, 0x0};
            ecl << pair{0x191c, 0x0};
            ecl << pair{0x192c, 0x0};
            ecl << pair{0x194c, 0x0};
            ecl << pair{0x196c, 0x0};
            ecl << pair{0x198c, 0x0};
            ecl << pair{0x19a0, 0x0};
        };
        auto s3b_n1 = [&]() {
            ecl << pair{0x1274, (int16_t)0x0};
            ecl << pair{0x12f0, (int16_t)0x0};
            ECLWarp(0x16d4);
            ecl << pair{0x80d6, (int16_t)0x16d4};
            ecl << pair{0x1f70, 0x1};
            ecl << pair{0x1f90, 0x0};
            ecl << pair{0x80dc, (int16_t)0x24};
            ecl << pair{0x20ec, 0x0};
            ecl << pair{0x210c, 0x0};
            ecl << pair{0x212c, 0x0};
            ecl << pair{0x214c, 0x1e};
            ecl << pair{0x2160, 0x1e};
            ecl << pair{0x2194, 0x1e};
            ecl << pair{0x2188, 150};
        };
        auto s4b_time = [&]() {
            ecl << pair{0x2790, 0x0};
            ecl << pair{0x27b0, 0x0};
            ecl << pair{0x27d0, 0x0};
            ecl << pair{0x27f0, 0x0};
            ecl << pair{0x2810, 0x0};
            ecl << pair{0x2824, 0x0};
            ecl << pair{0x283c, 0x0};
            ecl << pair{0x2850, 0x0};
        };
        auto s7b_call = [&]() {
            ecl << pair{0x344e, 0x0};
            ecl << pair{0x3452, 0x00180023};
            ecl << pair{0x3456, 0x00ffff00};
            ecl << pair{0x345a, 0x0};
            ecl << pair{0x345e, 0x0};
            ecl << pair{0x3462, 0x0};
            ECLStall(ecl, 0x3466);
        };
        auto s7b_n1 = [&]() {
            ECLWarp(0x2192);
            ecl << pair{0x339e, 0x0};
            ecl << pair{0x33ae, 0x0};
            ecl << pair{0x33ce, 0x0};
            ecl << pair{0x33ee, 0x0};
            ecl << pair{0x340e, 0x0};
            ecl << pair{0x342e, 0x0};
        };

        switch (section) {
        case THPrac::TH06::TH06_ST1_MID1:
            ECLWarp(0x7d8);
            ecl << pair{0x0ab0, 0x3c} << pair{0x0ad0, 0x3c};
            break;
        case THPrac::TH06::TH06_ST1_MID2:
            ECLWarp(0x7d8);
            ecl << pair{0x0ab0, 0x3c} << pair{0x0ad0, 0x3c};
            ECLSetHealth(ecl, 0x0af0, 0x3c, 0x1f3);
            break;
        case THPrac::TH06::TH06_ST1_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x149e);
            else {
                ECLWarp(0x149f);
                ecl << pair{0x16a6, 0} << pair{0x16c6, 0} << pair{0x16e6, 0x50};
            }
            break;
        case THPrac::TH06::TH06_ST1_BOSS2:
            ECLWarp(0x149f);
            ecl << pair{0x16a6, 0} << pair{0x16c6, 0} << pair{0x16e6, 0x50};
            ECLSetTime(ecl, 0x16e6, 0, 0);
            ECLStall(ecl, 0x16f6);
            break;
        case THPrac::TH06::TH06_ST1_BOSS3:
            ECLWarp(0x149f);
            ecl << pair{0x16a6, 0} << pair{0x16c6, 0} << pair{0x16e6, 0x50}
                << pair{0x16f2, 0x10} << pair{0x293a, 0} << pair{0x294a, 0}
                << pair{0x291e, (int16_t)0};
            break;
        case THPrac::TH06::TH06_ST1_BOSS4:
            ECLWarp(0x149f);
            ecl << pair{0x16a6, 0} << pair{0x16c6, 0} << pair{0x16e6, 0x50}
                << pair{0x16f2, 0x10} << pair{0x293a, 0} << pair{0x294a, 0}
                << pair{0x291e, (int16_t)0};
            ECLSetTime(ecl, 0x294a, 0, 0);
            ECLStall(ecl, 0x295a);
            break;
        case THPrac::TH06::TH06_ST2_MID1:
            ECLWarp(0xa1c);
            break;
        case THPrac::TH06::TH06_ST2_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x175f);
            else
                ECLWarp(0x1760);
            break;
        case THPrac::TH06::TH06_ST2_BOSS2:
            s2b_nd();
            ECLSetTime(ecl, 0x19a0, 0x0, 0x0);
            ECLStall(ecl, 0x19b0);
            break;
        case THPrac::TH06::TH06_ST2_BOSS3:
            s2b_nd();
            ecl << pair{0x19ac, 0x19};
            ecl << pair{0x2138, 0x0};
            ecl << pair{0x2148, 0x60};
            ecl << pair{0x2110, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST2_BOSS4:
            s2b_nd();
            ecl << pair{0x19ac, 0x19};
            ecl << pair{0x2138, 0x0};
            ecl << pair{0x2148, 0x60};
            ecl << pair{0x2110, (int16_t)0x0};
            ECLSetTime(ecl, 0x2148, 0x30, 0x0);
            ECLStall(ecl, 0x2158);
            break;
        case THPrac::TH06::TH06_ST2_BOSS5:
            s2b_nd();
            ecl << pair{0x19ac, 0x19};
            ecl << pair{0x2138, 0x0};
            ecl << pair{0x2148, 0x60};
            ecl << pair{0x2110, (int16_t)0x0};
            ecl << pair{0x2148, 0x0};
            ecl << pair{0x2154, 0x20};
            ecl << pair{0x33a2, (int16_t)0x0};
            ecl << pair{0x337a, (int16_t)0x0};
            ecl << pair{0x3392, (int16_t)0x0};
            ecl << pair{0x2090, 0x578};
            ecl << pair{0x20b0, 0xffffffff};
            ecl << pair{0x20c0, 0xffffffff};
            ecl << pair{0x20f0, 0x1c};
            break;
        case THPrac::TH06::TH06_ST3_MID1:
            ECLWarp(0x0edc);
            break;
        case THPrac::TH06::TH06_ST3_MID2:
            ECLWarp(0x0edc);
            ecl << pair{0x1274, (int16_t)0x0};
            ecl << pair{0x12f0, (int16_t)0x0};
            ecl << pair{0x1018, 0x0};
            ECLSetHealth(ecl, 0x10dc, 0x1e, 0x513);
            ECLStall(ecl, 0x10ec);
            break;
        case THPrac::TH06::TH06_ST3_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x16d4);
            else
                s3b_n1();
            break;
        case THPrac::TH06::TH06_ST3_BOSS2:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ECLSetTime(ecl, 0x2160, 0x0, 0x0);
            ECLStall(ecl, 0x2170);
            break;
        case THPrac::TH06::TH06_ST3_BOSS3:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x14};
            ecl << pair{0x25f4, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST3_BOSS4:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x14};
            ecl << pair{0x25f4, (int16_t)0x0};
            ECLSetTime(ecl, 0x267c, 0x0, 0x0);
            ECLStall(ecl, 0x268c);
            break;
        case THPrac::TH06::TH06_ST3_BOSS5:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x1a};
            ecl << pair{0x31d0, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST3_BOSS6:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x1a};
            ecl << pair{0x31d0, (int16_t)0x0};
            ECLSetTime(ecl, 0x3254, 0x0, 0x0);
            ECLStall(ecl, 0x3264);
            break;
        case THPrac::TH06::TH06_ST3_BOSS7:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x1a};
            ecl << pair{0x31d0, (int16_t)0x0};
            ECLSetTime(ecl, 0x3254, 0x0, 0x0);
            ECLStall(ecl, 0x3264);
            ecl << pair{0x3168, 0x7d0};
            ecl << pair{0x31a8, 0x21};
            ecl << pair{0x31b8, 0x21};
            ecl << pair{0x4b64, (int16_t)0x0};
            ecl << pair{0x4bec, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST4_BOOKS:
            switch (thPracParam.phase)
            {
            case 0:
            default:
                break;
            case 1:
                ecl << pair { 0xCE7C, (int16_t)-1 }; // disable timeline after books
                ecl << pair { 0x1324, (int32_t)99999999 }; // loop forever
                break;
            case 2:
                ecl << pair { 0xCE60, (int16_t)-1 }; // disable timeline after books
                ecl << pair { 0x1324, (int32_t)99999999 }; // loop forever
                break;
            case 3:
                ecl << pair { 0xCDFC, (float)12345.0f }; // disable book 2
                ecl << pair { 0xCE7C, (int16_t)-1 }; // disable timeline after books
                ecl << pair { 0x1324, (int32_t)99999999 }; // loop forever
                break;
            case 4:{
                int32_t ofs = 0xCDD8;
                if (thPracParam.bF & (1<<0)) ecl << pair { ofs, (int16_t)0 } << pair { ofs + 0x4, (float)thPracParam.bX1 + 192.0f } << pair { ofs + 0x8, (float)thPracParam.bY1 }; ofs += 0x1C;
                if (thPracParam.bF & (1<<1)) ecl << pair { ofs, (int16_t)0 } << pair { ofs + 0x4, (float)thPracParam.bX2 + 192.0f } << pair { ofs + 0x8, (float)thPracParam.bY2 }; ofs += 0x1C;
                if (thPracParam.bF & (1<<2)) ecl << pair { ofs, (int16_t)0 } << pair { ofs + 0x4, (float)thPracParam.bX3 + 192.0f } << pair { ofs + 0x8, (float)thPracParam.bY3 }; ofs += 0x1C;
                if (thPracParam.bF & (1<<3)) ecl << pair { ofs, (int16_t)0 } << pair { ofs + 0x4, (float)thPracParam.bX4 + 192.0f } << pair { ofs + 0x8, (float)thPracParam.bY4 }; ofs += 0x1C;
                if (thPracParam.bF & (1<<4)) ecl << pair { ofs, (int16_t)0 } << pair { ofs + 0x4, (float)thPracParam.bX5 + 192.0f } << pair { ofs + 0x8, (float)thPracParam.bY5 }; ofs += 0x1C;
                if (thPracParam.bF & (1<<5)) ecl << pair { ofs, (int16_t)0 } << pair { ofs + 0x4, (float)thPracParam.bX6 + 192.0f } << pair { ofs + 0x8, (float)thPracParam.bY6 };
                ecl << pair { 0xCE7C, (int16_t)-1 }; // disable timeline after books
                ecl << pair { 0x1324, (int32_t)99999999 }; // loop forever
            }
                break;

            }
            ECLWarp(0x0d40);
            break;
        case THPrac::TH06::TH06_ST4_MID1:
            ECLWarp(0x1024);
            break;
        case THPrac::TH06::TH06_ST4_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x29c6);
            else
                ECLWarp(0x29c7);
            break;
        case THPrac::TH06::TH06_ST4_BOSS2:
            ECLWarp(0x29c7);
            s4b_time();
            ECLSetTime(ecl, 0x2810, 0x0, 0x0);
            ECLStall(ecl, 0x2820);
            break;
        case THPrac::TH06::TH06_ST4_BOSS3:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x25};
            ecl << pair{0x6da0, (int16_t)0x0};
            ECLSetTime(ecl, 0x7440, 0x0, 0x0);
            ECLStall(ecl, 0x7450);
            break;
        case THPrac::TH06::TH06_ST4_BOSS4:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x25};
            ecl << pair{0x6da0, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST4_BOSS5:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x27};
            ecl << pair{0x7950, (int16_t)0x0};
            ecl << pair{0x7afc, 0x0};
            ecl << pair{0x7b0c, 0x0};
            ecl << pair{0x7b2c, 0x0};
            ecl << pair{0x7b4c, 0x0};
            ecl << pair{0x7b6c, 0x0};
            ecl << pair{0x7b8c, 0x0};
            break;
        case THPrac::TH06::TH06_ST4_BOSS6:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x27};
            ecl << pair{0x7950, (int16_t)0x0};

            ECLSetHealth(ecl, 0x7afc, 0, 1699);
            ECLSetHealth(ecl, 0x7b0c, 0, 3399);
            ECLStall(ecl, 0x7b1c);
            ecl << pair{0x7b04, (int16_t)0x0200} << pair{0x7b14, (int16_t)0x0c00};
            ecl << pair{0x7bc8, (int16_t)0}
                << pair{0x7cf4, (int16_t)0} << pair{0x7d0c, (int16_t)0};
            ecl << pair{0x7d18, 0x0} << pair{0x7d28, 0x0}
                << pair{0x7d48, 0x0} << pair{0x7d68, 0x0}
                << pair{0x7d88, 0x0} << pair{0x7da8, 0x0};
            break;
        case THPrac::TH06::TH06_ST4_BOSS7:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x27};
            ecl << pair{0x7950, (int16_t)0x0};

            ECLSetHealth(ecl, 0x7afc, 0, 1699);
            ECLStall(ecl, 0x7b0c);
            ecl << pair{0x7a74, (int16_t)41} << pair{0x7a94, (int16_t)41};
            ecl << pair{0x7a54, (int16_t)1700} << pair{0x7a64, (int16_t)1700};
            ecl << pair{0x7de4, (int16_t)0}
                << pair{0x7ed0, (int16_t)0} << pair{0x7ee8, (int16_t)0};
            ecl << pair{0x7ef4, 0x0} << pair{0x7f04, 0x0}
                << pair{0x7f24, 0x0} << pair{0x7f44, 0x0}
                << pair{0x7f64, 0x0} << pair{0x7f84, 0x0};
            break;
        case THPrac::TH06::TH06_ST5_MID1:
            ECLWarp(0x0d2c);
            if (!thPracParam.dlg)
                ecl << pair{0x64a8, (uint16_t)13};
            break;
        case THPrac::TH06::TH06_ST5_MID2:
            ECLWarp(0x0d2c);
            ecl << pair{0x64a4, (int16_t)0x0};
            ECLSetHealth(ecl, 0x14d8, 0x1e, 0x2c5);
            ECLStall(ecl, 0x14e8);
            break;
        case THPrac::TH06::TH06_ST5_BOSS1:
            ECLWarp(0x1e18);
            if (!thPracParam.dlg) {
                ecl << pair{0x767c, (int16_t)0x0};
                ecl << pair{0x22c8, 0x0};
                ecl << pair{0x22e8, 0x0};
                ecl << pair{0x2308, 0x0};
                ecl << pair{0x2328, 0x0};
                ecl << pair{0x2348, 0x0};
                ecl << pair{0x2218, (int16_t)0x0};
            }
            break;
        case THPrac::TH06::TH06_ST5_BOSS2:
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ECLSetTime(ecl, 0x2348, 0x0, 0x0);
            ECLStall(ecl, 0x2358);
            break;
        case THPrac::TH06::TH06_ST5_BOSS3:
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ecl << pair{0x235c, 0x0};
            ecl << pair{0x2368, 0x22};
            ecl << pair{0x3778, (int16_t)0x0};
            ecl << pair{0x3828, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST5_BOSS4:
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ecl << pair{0x235c, 0x0};
            ecl << pair{0x2368, 0x22};
            ecl << pair{0x3778, (int16_t)0x0};
            ecl << pair{0x3828, (int16_t)0x0};
            ECLSetTime(ecl, 0x38b8, 0x0, 0x0);
            ECLStall(ecl, 0x38c8);
            break;
        case THPrac::TH06::TH06_ST5_BOSS5:
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ecl << pair{0x235c, 0x1e};
            ecl << pair{0x2368, 0x29};
            ecl << pair{0x4638, (int16_t)0x0};
            ecl << pair{0x46e8, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST5_BOSS6:
        {
            std::default_random_engine rand_engine(GM_RngSeed());
            std::uniform_int_distribution<int32_t> rand_value(0, 640);
            i32* pBulletInsertCount = &th06::g_BulletManager.nextBulletIndex;
            switch (thPracParam.phase) {
            case 0:
                break;
            case 1:
                *pBulletInsertCount = 640 - 360;
                break;
            case 2:
                *pBulletInsertCount = 550;
                break;
            case 3:
                *pBulletInsertCount = rand_value(rand_engine);
                break;
            }
        }
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ecl << pair{0x235c, 0x1e};
            ecl << pair{0x2368, 0x29};
            ecl << pair{0x4638, (int16_t)0x0};
            ecl << pair{0x46e8, (int16_t)0x0};
            ecl << pair{0x235c, 0x0};
            ECLSetTime(ecl, 0x4758, 0x0, 0x0);
            ECLStall(ecl, 0x4768);
            break;
        case THPrac::TH06::TH06_ST6_MID1:
            ECLWarp(0x0a04);
            if (!thPracParam.dlg) {
                ecl << pair{0x77f2, (int16_t)0x0};
                ecl << pair{0x9e8, 0x1};
            }
            break;
        case THPrac::TH06::TH06_ST6_MID2:
            shot = (int)(GM_Character() * 2) + GM_ShotType();
            if (shot > 1)
                shot = 1099;
            else if (!shot)
                shot = 749;
            else
                shot = 999;
            ECLWarp(0x0a04);
            ecl << pair{0x77f2, (int16_t)0x0};
            ecl << pair{0x0d2c, 0x0};
            ECLSetHealth(ecl, 0x0d3c, 0x0, shot);
            ECLStall(ecl, 0x0d4c);
            break;
        case THPrac::TH06::TH06_ST6_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x0c5f);
            else {
                ECLWarp(0x0c61);
                ECLNameFix();
                ecl << pair{0x1686, 0x0};
                ecl << pair{0x16a6, 0x0};
                ecl << pair{0x16c6, 0x0};
                ecl << pair{0x16e6, 0x0};
                ecl << pair{0x15d6, (int16_t)0x0};
            }
            break;
        case THPrac::TH06::TH06_ST6_BOSS2:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ECLSetTime(ecl, 0x1706, 0x0, 0x0);
            ECLStall(ecl, 0x1716);
            break;
        case THPrac::TH06::TH06_ST6_BOSS3:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1726, 0x13};
            ecl << pair{0x1b8e, (int16_t)0x0};
            ecl << pair{0x1c3e, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST6_BOSS4:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1726, 0x13};
            ecl << pair{0x1b8e, (int16_t)0x0};
            ecl << pair{0x1c3e, (int16_t)0x0};
            ECLSetTime(ecl, 0x1cf2, 0x0, 0x0);
            ECLStall(ecl, 0x1d02);
            break;
        case THPrac::TH06::TH06_ST6_BOSS5:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x1e};
            ecl << pair{0x1726, 0x17};
            ecl << pair{0x28e2, (int16_t)0x0};
            ecl << pair{0x2992, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST6_BOSS6:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x1e};
            ecl << pair{0x1726, 0x17};
            ecl << pair{0x28e2, (int16_t)0x0};
            ecl << pair{0x2992, (int16_t)0x0};
            ecl << pair{0x171a, 0x0};
            ECLSetTime(ecl, 0x2a22, 0x0, 0x0);
            ECLStall(ecl, 0x2a32);
            break;
        case THPrac::TH06::TH06_ST6_BOSS7:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1726, 0x1a};
            ecl << pair{0x2d8e, (int16_t)0x0};
            ecl << pair{0x2e3e, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST6_BOSS8:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1726, 0x1a};
            ecl << pair{0x2d8e, (int16_t)0x0};
            ecl << pair{0x2e3e, (int16_t)0x0};
            ECLSetTime(ecl, 0x2ee6, 0x0, 0x0);
            ECLStall(ecl, 0x2ef6);
            break;
        case THPrac::TH06::TH06_ST6_BOSS9:
            ECLNameFix();
            ECLWarp(0x0c61);
            
            switch (thPracParam.phase) {
            case 0:
                break;
            case 1:
                // full 56 way
                ECLJump(ecl, 180 + thPracParam.delay_st6bs9, 0x6592, 180, 0x6486);
                break;
            case 2:
                // super full 56 way
                ECLJump(ecl, 180 + thPracParam.delay_st6bs9, 0x6592, 180, 0x6486);
                ecl << pair { 0x658A, (int32_t)9 };
                break;
            case 3:
            {// half full 56 way
                const uint8_t data[132] = {
                    0x04, 0x01, 0x00, 0x00, 0x52, 0x00, 0x2C, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x78, 0x00, 0x00, 0x00,
                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x6A, 0xBC, 0x3C,
                    0xDB, 0x0F, 0xC9, 0x3C, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x80, 0xBF, 0x04, 0x01, 0x00, 0x00,
                    0x46, 0x00, 0x2C, 0x00, 0x00, 0x04, 0xFF, 0x00, 0x09, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x00, 0x00,
                    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x40, 0x66, 0x66, 0xE6, 0x3F, 0x00, 0x54, 0x1C, 0xC6,
                    0x7C, 0xD9, 0xA0, 0xBE, 0x00, 0x02, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x46, 0x00, 0x2C, 0x00,
                    0x00, 0x08, 0xFF, 0x00, 0x09, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x80, 0x40, 0x66, 0x66, 0xE6, 0x3F, 0x00, 0x54, 0x1C, 0xC6, 0x7C, 0xD9, 0xA0, 0xBE,
                    0x00, 0x02, 0x00, 0x00
                };
                for (int i=0;i < 132;i++)
                    ecl << pair { 0x67A2 + i,data[i] };
                ECLJump(ecl, 180 + thPracParam.delay_st6bs9, 0x6592, 260, 0x6756);
            }
                break;
            case 4:
                // + wave 3
                ECLJump(ecl, 180, 0x6592, 180, 0x6756);
                break;
            case 5:
                // + wave 2,3
                ECLJump(ecl, 180, 0x6592, 180, 0x668E);
                break;
            }

            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1732, 0x0};
            ecl << pair{0x1726, 0x2b};
            ecl << pair{0x1722, (int16_t)0x0300};
            ecl << pair{0x1736, (int16_t)0x23};
            ecl << pair{0x173a, (int16_t)0x0c00};
            ecl << pair{0x173e, 0x2c};
            ecl << pair{0x5c8e, (int16_t)0x0};
            ecl << pair{0x6290, (int16_t)0x0};
            ecl << pair{0x1622, 0xffffffff};
            break;
        case THPrac::TH06::TH06_ST7_MID1:
            ECLWarp(0x1284);
            if (!thPracParam.dlg)
                ecl << pair{0x0d2e2, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_MID2:
            ECLWarp(0x1284);
            ecl << pair{0x0d2e2, (int16_t)0x0};
            ecl << pair{0x1b14, 0x12};
            ecl << pair{0x1c2c, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_MID3:
            ECLWarp(0x1284);
            ecl << pair{0x0d2e2, (int16_t)0x0};
            ecl << pair{0x1b14, 0x13};
            ecl << pair{0x1d7c, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_NS1:
            if (thPracParam.dlg)
                ECLWarp(0x2191);
            else {
                ECLNameFix();
                s7b_n1();
            }
            break;
        case THPrac::TH06::TH06_ST7_END_S1:
            ECLNameFix();
            s7b_n1();
            ECLSetTime(ecl, 0x344e, 0x0, 0x0);
            break;
        case THPrac::TH06::TH06_ST7_END_NS2:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x23};
            ecl << pair{0x4210, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S2:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x23};
            ecl << pair{0x4210, (int16_t)0x0};
            ecl << pair{0x41fc, 0x0};
            ecl << pair{0x420c, 0x0};
            ECLSetTime(ecl, 0x421c, 0x0, 0x0);
            ECLStall(ecl, 0x422c);
            break;
        case THPrac::TH06::TH06_ST7_END_NS3:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x26};
            ecl << pair{0x4c62, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S3:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x26};
            ecl << pair{0x4c62, (int16_t)0x0};
            ecl << pair{0x4c4e, 0x0};
            ecl << pair{0x4c5e, 0x0};
            ECLSetTime(ecl, 0x4c6e, 0x0, 0x0);
            ECLStall(ecl, 0x4c7e);
            break;
        case THPrac::TH06::TH06_ST7_END_NS4:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x2b};
            ecl << pair{0x59cc, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S4:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x2b};
            ecl << pair{0x59cc, (int16_t)0x0};
            ecl << pair{0x59b8, 0x0};
            ecl << pair{0x59c8, 0x0};
            ECLSetTime(ecl, 0x59d8, 0x0, 0x0);
            ECLStall(ecl, 0x59e8);
            break;
        case THPrac::TH06::TH06_ST7_END_NS5:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x2f};
            ecl << pair{0x63a2, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S5:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x2f};
            ecl << pair{0x63a2, (int16_t)0x0};
            ecl << pair{0x638e, 0x0};
            ecl << pair{0x639e, 0x0};
            ECLSetTime(ecl, 0x63ae, 0x0, 0x0);
            ECLStall(ecl, 0x63be);
            break;
        case THPrac::TH06::TH06_ST7_END_NS6:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x31};
            ecl << pair{0x6b1c, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S6:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x31};
            ecl << pair{0x6b1c, (int16_t)0x0};
            ecl << pair{0x6b08, 0x0};
            ecl << pair{0x6b18, 0x0};
            ECLSetTime(ecl, 0x6b28, 0x0, 0x0);
            ECLStall(ecl, 0x6b38);
            break;
        case THPrac::TH06::TH06_ST7_END_NS7:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x35};
            ecl << pair{0x78aa, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S7:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x35};
            ecl << pair{0x78aa, (int16_t)0x0};
            ecl << pair{0x7896, 0x0};
            ecl << pair{0x78a6, 0x0};
            ECLSetTime(ecl, 0x78b6, 0x0, 0x0);
            ECLStall(ecl, 0x78c6);
            break;
        case THPrac::TH06::TH06_ST7_END_NS8:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x38};
            ecl << pair{0x8508, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S8:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x38};
            ecl << pair{0x8508, (int16_t)0x0};
            ecl << pair{0x84f4, 0x0};
            ecl << pair{0x8504, 0x0};
            ECLSetTime(ecl, 0x8514, 0x0, 0x0);
            ECLStall(ecl, 0x8524);
            break;
        case THPrac::TH06::TH06_ST7_END_S9:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x3b};
            ecl << pair{0x940a, (int16_t)0x0};
            ecl << pair{0x93f6, 0x0};
            ecl << pair{0x9406, 0x0};
            ecl << pair{0x9416, 0x0};
            ecl << pair{0x9422, 0x0};
            ecl << pair{0x943a, 0x0};
            ecl << pair{0x9466, 0x0};
            ecl << pair{0x9472, 0x0};
            ecl << pair{0x9482, 0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S10:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x43};
            ecl << pair{0x0bea4, (int16_t)0x0};
            ecl << pair{0x0be90, 0x0};
            ecl << pair{0x0bea0, 0x0};
            ecl << pair{0x0beb0, 0x0};
            ecl << pair{0x0bed0, 0x0};
            ecl << pair{0x0bef0, 0x0};
            ecl << pair{0x0bf10, 0x0};
            ecl << pair{0x0bf30, 0x0};
            ecl << pair{0x0bf50, 0x0};
            ecl << pair{0x0bf5c, 0x0};
            ecl << pair{0x0bf74, 0x0};
            ecl << pair{0x0bfa0, 0x0};
            ecl << pair{0x0bfac, 0x0};
            ecl << pair{0x0bfbc, 0x0};
            break;
        default:
            break;
        }
    }
    __declspec(noinline) void THStageWarp([[maybe_unused]] ECLHelper& ecl, int stage, int portion)
    {
        if (stage == 1) {
            switch (portion) {
            case 1:
                ECLWarp(68);
                break;
            case 2:
                ECLWarp(580);
                break;
            case 3:
                ECLWarp(1160);
                break;
            case 4:
                ECLWarp(1540);
                break;
            case 5:
                ECLWarp(2348);
                break;
            case 6:
                ECLWarp(4438);
                break;
            default:
                break;
            }
        } else if (stage == 2) {
            switch (portion) {
            case 1:
                ECLWarp(270);
                break;
            case 2:
                ECLWarp(924);
                break;
            case 3:
                ECLWarp(3528);
                break;
            case 4:
                ECLWarp(4563);
                break;
            default:
                break;
            }
        } else if (stage == 3) {
            switch (portion) {
            case 1:
                ECLWarp(340);
                break;
            case 2:
                ECLWarp(1050);
                break;
            case 3:
                ECLWarp(1670);
                break;
            case 4:
                ECLWarp(2762);
                break;
            case 5:
                ECLWarp(3807);
                break;
            case 6:
                ECLWarp(4118);
                break;
            case 7:
                ECLWarp(5274);
                break;
            default:
                break;
            }
        } else if (stage == 4) {
            switch (portion) {
            case 1:
                ECLWarp(380);
                break;
            case 2:
                ECLWarp(1454);
                break;
            case 3:
                ECLWarp(2328);
                break;
            case 4:
                ECLWarp(0x0d40);
                break;
            case 5:
                ECLWarp(4872);
                break;
            case 6:
                ECLWarp(5712);
                break;
            case 7:
                ECLWarp(7434);
                break;
            case 8:
                ECLWarp(8354);
                break;
            case 9:
                ECLWarp(9784);
                break;
            default:
                break;
            }
        } else if (stage == 5) {
            switch (portion) {
            case 1:
                ECLWarp(350);
                break;
            case 2:
                ECLWarp(1352);
                break;
            case 3:
                ECLWarp(2292);
                break;
            case 4:
                ECLWarp(3814);
                break;
            case 5:
                ECLWarp(6774);
                break;
            default:
                break;
            }
        } else if (stage == 6) {
            switch (portion) {
            case 1:
                ECLWarp(380);
                break;
            case 2:
                ECLWarp(1484);
                break;
            default:
                break;
            }
        } else if (stage == 7) {
            switch (portion) {
            case 1:
                ECLWarp(380);
                break;
            case 2:
                ECLWarp(1300);
                break;
            case 3:
                ECLWarp(2600);
                break;
            case 4:
                ECLWarp(3680);
                break;
            case 5:
                ECLWarp(4803);
                break;
            case 6:
                ECLWarp(5933);
                break;
            case 7:
                ECLWarp(7733);
                break;
            default:
                break;
            }
        }
    }
    __declspec(noinline) void THSectionPatch()
    {
        ECLHelper ecl;
        ecl.SetBaseAddr((void*)&th06::g_EclManager);

        auto section = thPracParam.section;
        if (section >= 10000 && section < 20000) {
            int stage = (section - 10000) / 100;
            int portionId = (section - 10000) % 100;
            THStageWarp(ecl, stage, portionId);
        } else {
            THPatch(ecl, (th_sections_t)section);
        }
    }

    // Hook Helper
    bool THBGMTest()
    {
        if (!thPracParam.mode)
            return 0;
        else if (thPracParam.section >= 10000)
            return 0;
        else
            return th_sections_bgm[thPracParam.section];
    }
    void THSaveReplay(char* rep_name)
    {
        ReplaySaveParam(mb_to_utf16(rep_name, 932).c_str(), thPracParam.GetJson());
    }

    static float MInterpolation(float t, float a, float b)
    {
        if (t < 0.0f) {
            return a;
        } else if (t < 0.5) {
            float k = (b - a) * 2.0f;
            return k * t * t + a;
        } else if (t < 1.0f) {
            float k = (b - a) * 2.0f;
            t = t - 1.0f;
            return -k * t * t + b;
        }
        return b;
    }
    static void RenderRepMarker(ImDrawList* p)
    {
        if (g_adv_igi_options.th06_showRepMarker) {
            DWORD is_rep = GM_IsInReplay();
            DWORD gameState = SV_CurState();
            if (is_rep && gameState == 2) {
                auto f = ImGui::GetFont();
                auto sz = f->CalcTextSizeA(20, 100, 100, "ＲＥＰ");
                ImVec2 p1 = { 416.0f, 464.0f };
                p->AddRectFilled({ p1.x - sz.x, p1.y - sz.y }, p1, 0x77000000);
                p->AddText(f, 20, { p1.x - sz.x, p1.y - sz.y }, 0xFFFFFFFF, "ＲＥＰ");
            }
        }
    }
    static void RenderPlHitbox(ImDrawList* p)
    {
        { // player hitbox
            static float t = 0.0f;
            static bool is_shift_pressed = false;
            if (g_adv_igi_options.th06_show_hitbox && g_hitbox_textureID != NULL) {
                bool is_time_stopped = GM_IsTimeStopped();
                DWORD gameState = SV_CurState();
                BYTE pauseMenuState = GM_IsInGameMenu();
                WORD keyState = GM_CurInput();
                if (!is_time_stopped)
                    is_shift_pressed = keyState & 0x4;
                if (gameState == 2 && is_shift_pressed) {
                    if (pauseMenuState == 0) {
                        if (!is_time_stopped)
                            t += 1.0f;
                        float scale = MInterpolation(t / 18.0f, 1.5f, 1.0f),
                              scale2 = MInterpolation(t / 12.0f, 0.3f, 1.0f),
                              angle = 3.14159f,
                              angle2 = 0.0f,
                              alpha = t < 6.0f ? t / 6.0f : 1.0f;
                        if (t < 18.0f) {
                            angle = MInterpolation(t / 18.0f, 3.14159f, -3.14159f);
                            angle2 = -angle;
                        } else {
                            angle = -3.14159f + t * 0.05235988f;
                            angle2 = 3.14159f - t * 0.05235988f;
                        }
                        scale *= 0.75f;
                        scale2 *= 0.75f; // 32->24
                        p->PushClipRect({ 32.0f, 16.0f }, { 416.0f, 464.0f });
                        ImVec2 p1 = { PL_PosX() + 32.0f, PL_PosY() + 16.0f };
                        float c, s;
                        c = cosf(angle) * scale * g_hitbox_sz.x, s = sinf(angle) * scale * g_hitbox_sz.y;
                        p->AddImageQuad(g_hitbox_textureID, { p1.x + c, p1.y + s }, { p1.x - s, p1.y + c }, { p1.x - c, p1.y - s }, { p1.x + s, p1.y - c }, { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, ImGui::ColorConvertFloat4ToU32({ 1, 1, 1, alpha }));
                        c = cosf(angle2) * scale2 * g_hitbox_sz.x, s = sinf(angle2) * scale2 * g_hitbox_sz.y;
                        p->AddImageQuad(g_hitbox_textureID, { p1.x + c, p1.y + s }, { p1.x - s, p1.y + c }, { p1.x - c, p1.y - s }, { p1.x + s, p1.y - c });
                        p->PopClipRect();
                    }
                } else {
                    t = 0.0f;
                }
            }
        }
    }
    static void RenderBtHitbox(ImDrawList* p)
    {
        // show bullet hitbox
        if (g_show_bullet_hitbox) {
            DWORD is_practice = GM_IsPracticeMode();
            DWORD gameState = SV_CurState();
            DWORD is_rep = GM_IsInReplay();
            if (((is_practice) || (is_rep) || (thPracParam.mode)) && gameState == 2) {
                p->PushClipRect({ 32.0f, 16.0f }, { 416.0f, 464.0f });
                ImVec2 stage_pos = { 32.0f, 16.0f };
                ImVec2 plpos1 = {PL_HitboxTL().x, PL_HitboxTL().y};
                ImVec2 plpos2 = {PL_HitboxBR().x, PL_HitboxBR().y};
                float plhit = plpos2.x - plpos1.x;

                // bullet hitbox
                for (int i = 0; i < 640; i++) {
                    th06::Bullet& bt = th06::g_BulletManager.bullets[i];
                    if (bt.state != 1) {
                        continue;
                    }
                    ImVec2 pos = { bt.pos.x, bt.pos.y };
                    ImVec2 hit = { bt.sprites.grazeSize.x, bt.sprites.grazeSize.y };

                    ImVec2 p1 = { pos.x - hit.x * 0.5f - plhit * 0.5f + stage_pos.x, pos.y - hit.y * 0.5f - plhit * 0.5f + stage_pos.y };
                    ImVec2 p2 = { pos.x + hit.x * 0.5f + plhit * 0.5f + stage_pos.x, pos.y + hit.y * 0.5f + plhit * 0.5f + stage_pos.y };
                    p->AddRectFilled(p1, p2, 0x88002288);
                    p->AddRect(p1, p2, 0xFFFFFFFF, 0.0f);
                }

                // laser hitbox
                for (int i = 0; i < 64; i++) {
                    th06::Laser& ls = th06::g_BulletManager.lasers[i];
                    if (ls.inUse) {
                        ImVec2 pos = { ls.pos.x, ls.pos.y };
                        float angle = ls.angle;
                        float quat_width = ls.width * 0.5f * 0.5f;
                        float half_width_pl = plhit * 0.5f;
                        float start_ofs = ls.startOffset;
                        float end_ofs = ls.endOffset;
                        int state = ls.state;

                        int start_time_graze = ls.hitboxStartTime;
                        int end_time_graze = ls.hitboxEndDelay;
                        int time_cur_state = ls.timer.current;
                        float sub_frame = ls.timer.subFrame;
                        if (state == 0) {
                            int state_change_time_hit = ls.startTime;
                            float l2 = 0.0f;
                            if (time_cur_state <= state_change_time_hit - std::max(30, state_change_time_hit)) {
                                l2 = 1.2f * 0.5f;
                            } else {
                                l2 = quat_width * ((float)time_cur_state + sub_frame) / (float)state_change_time_hit;
                            }
                            float mid = (start_ofs + end_ofs) * 0.5f;
                            start_ofs = mid - l2;
                            end_ofs = mid + l2;
                        }
                        if (state == 2) {
                            int state_change_time_disappear = ls.despawnDuration;
                            float l2 = 0.0f;
                            if (state_change_time_disappear > 0) {
                                l2 = quat_width - quat_width * ((float)time_cur_state + sub_frame) / (float)state_change_time_disappear;
                            }
                            float mid = (start_ofs + end_ofs) * 0.5f;
                            start_ofs = mid - l2;
                            end_ofs = mid + l2;
                        }
                        if (state == 1 || (state == 0 && time_cur_state >= start_time_graze) || (state == 2 && time_cur_state < end_time_graze)) {
                            float c = cosf(angle);
                            float s = sinf(angle);
                            ImVec2 hitpos[4] = {
                                { start_ofs - half_width_pl, -quat_width - half_width_pl },
                                { end_ofs + half_width_pl, -quat_width - half_width_pl },
                                { end_ofs + half_width_pl, quat_width + half_width_pl },
                                { start_ofs - half_width_pl, quat_width + half_width_pl }
                            };
                            auto RotPos = [](ImVec2 p, float c, float s) -> ImVec2 {
                                return { p.x * c - p.y * s, p.x * s + p.y * c };
                            };
                            for (int j = 0; j < 4; j++) {
                                hitpos[j] = RotPos(hitpos[j], c, s);
                                hitpos[j].x += pos.x + stage_pos.x;
                                hitpos[j].y += pos.y + stage_pos.y;
                            }
                            p->AddQuad(hitpos[0], hitpos[1], hitpos[2], hitpos[3], 0xFFFF0000);
                            p->AddQuadFilled(hitpos[0], hitpos[1], hitpos[2], hitpos[3], 0x88882200);
                        }
                    }
                }

                // enemy hitbox
                for (int i = 0; i < 0x100; i++) {
                    th06::Enemy& enemy = th06::g_EnemyManager.enemies[i];
                    if (enemy.flags.unk5) {
                        if (enemy.flags.unk8 && enemy.flags.unk7 && enemy.flags.unk6) {
                            ImVec2 pos = { enemy.position.x, enemy.position.y };
                            ImVec2 hit = { enemy.hitboxDimensions.x, enemy.hitboxDimensions.y };
                            hit.x *= 0.666666f;
                            hit.y *= 0.666666f;
                            ImVec2 p1 = { pos.x - hit.x * 0.5f - plhit * 0.5f + stage_pos.x, pos.y - hit.y * 0.5f - plhit * 0.5f + stage_pos.y };
                            ImVec2 p2 = { pos.x + hit.x * 0.5f + plhit * 0.5f + stage_pos.x, pos.y + hit.y * 0.5f + plhit * 0.5f + stage_pos.y };
                            p->AddRectFilled(p1, p2, 0xAAFF5500);
                            p->AddRect(p1, p2, 0xFFFFFFFF);
                        }
                    }
                }
                 p->PopClipRect();
            }
        }
    }
    static void RenderLockTimer(ImDrawList* p)
    {
        if (g_adv_igi_options.enable_lock_timer_autoly && *THOverlay::singleton().mTimeLock) {
            char _tbuf[32]; snprintf(_tbuf, sizeof(_tbuf), "%.2f", (float)g_lock_timer / 60.0f); std::string time_text(_tbuf);
            auto sz = ImGui::CalcTextSize(time_text.c_str());
            p->AddRectFilled({ 32.0f, 0.0f }, { 110.0f, sz.y }, 0xFFFFFFFF);
            p->AddText({ 110.0f - sz.x, 0.0f }, 0xFF000000, time_text.c_str());
        }
    }

    EHOOK_ST(th06_result_screen_create, 0x42d812, 4, {
        self->Disable();
        *(uint32_t*)(*(uint32_t*)(pCtx->Ebp - 0x10) + 0x8) = 0xA;
        pCtx->Eip = 0x42d839;
    });

    // It would be good practice to run Setup() on this
    // But due to the way this new hooking system works
    // running Setup is only needed for Hooks, not patches
    PATCH_ST(th06_white_screen, 0x42fee0, "c3");

    HOOKSET_DEFINE(THMainHook)
    PATCH_DY(th06_reacquire_input, 0x41dc58, "0000000074")
    EHOOK_DY(th06_activateapp, 0x420D96, 3, {
        // Wacky hack to disable rendering for one frame to prevent the game from crasing when alt tabbing into it if the pause menu is open and the game is in fullscreen mode
        GameGuiProgress = 1;
    })
    EHOOK_DY(th06_bgm_play, 0x424b5d, 1, {
        int32_t retn_addr = ((int32_t*)pCtx->Esp)[0];

        if (THPauseMenu::singleton().el_bgm_signal) {
            pCtx->Eip = 0x424d35;
        }
        if (retn_addr == 0x418db4) {
            THPauseMenu::singleton().el_bgm_changed = true;
        }
    })
    EHOOK_DY(th06_bgm_stop, 0x430f80, 1, {
        if (THPauseMenu::singleton().el_bgm_signal) {
            pCtx->Eip = 0x43107b;
        }
    })
    EHOOK_DY(th06_prac_menu_1, 0x437179, 7, {
        THGuiPrac::singleton().State(1);
    })
    EHOOK_DY(th06_prac_menu_3, 0x43738c, 3, {
        THGuiPrac::singleton().State(3);
    })
    EHOOK_DY(th06_prac_menu_4, 0x43723f, 3, {
        THGuiPrac::singleton().State(4);
    })
    EHOOK_DY(th06_prac_menu_enter, 0x4373a3, 5, {
        GM_Stage() = GM_MenuCursor() = thPracParam.stage;
        if (thPracParam.stage == 6)
            (int8_t&)GM_Difficulty() = 4;
        else
            (int8_t&)GM_Difficulty() = (int8_t)th06::g_Supervisor.cfg.difficulty;
    })
    EHOOK_DY(th06_pause_menu, 0x401b8f, 2, {
        if (thPracParam.mode && (GM_IsInReplay() == 0)) {
            auto sig = THPauseMenu::singleton().PMState();
            if (sig == THPauseMenu::SIGNAL_RESUME) {
                pCtx->Eip = 0x40223d;
            } else if (sig == THPauseMenu::SIGNAL_EXIT) {
                SV_CurState() = 7; // Set gamemode to result screen
                GM_IsInGameMenu() = 0; // Close pause menu
                th06_result_screen_create.Enable();
            } else if (sig == THPauseMenu::SIGNAL_RESTART) {
                pCtx->Eip = 0x40263c;
            } else if (sig == THPauseMenu::SIGNAL_EXIT2) {
                SV_CurState() = 1; 
                GM_IsInGameMenu() = 0;
                th06_result_screen_create.Enable();
            } else {
                pCtx->Eip = 0x4026a6;
            }
        }
        // escR patch
        DWORD thiz = pCtx->Ecx;
        if (!thPracParam.mode && (GM_IsInReplay() == 0))
        {
            if (*(DWORD*)(thiz) != 7) {
                WORD key = GM_CurInput();
                //WORD key_last = GM_LastInput();
                if (Gui::KeyboardInputGetRaw('R') || (key & 0x124) == 0x124) { // ctrl+shift+down or R
                    *(DWORD*)(thiz) = 7;
                    threstartflag_normalgame = true;
                }
            }
        }
        if (*(DWORD*)(thiz) == 7) {
            pCtx->Eip = 0x40263c;
        }
    })
    
    EHOOK_DY(th06_inf_lives,0x00428DEB,2,{
        if ((*(THOverlay::singleton().mInfLives)))
        {
            if (!g_adv_igi_options.map_inf_life_to_no_continue){
                pCtx->Eax += 1;
            }else{
                if ((pCtx->Eax & 0xFF) == 0)
                    pCtx->Eax += 1;
            }
        }  
    })

    EHOOK_DY(th06_pause_menu_pauseBGM, 0x402714,3,{
        if (g_adv_igi_options.th06_pauseBGM) {
            // TODO: Reimplement BGM pause/resume using SDL_mixer instead of DirectSound
            // Original code accessed g_SoundPlayer.backgroundMusic (CStreamingSound*)
            // which used IDirectSoundBuffer - not available in SDL2 build
            if (GM_IsInGameMenu() == 0 || (*(THOverlay::singleton().mElBgm) && thPracParam.mode)) // show menu==0
            {
                // Resume BGM - SDL2 equivalent needed
            } else {
                // Pause BGM - SDL2 equivalent needed
            }
        }
    })
    EHOOK_DY(th06_unpause_prevent_desyncs, 0x40223d, 6, {
        if (!GM_IsInReplay()) {
            GM_CurInput() &= ~0x2; // clear bomb bit from input

            if (th06::g_Gui.impl->msg.currentMsgIdx >= 0) // in dialogue
                GM_CurInput() &= ~0x1; // clear shoot bit from input
        }
    })
    EHOOK_DY(th06_patch_main, 0x41c17a,5, {
        THPauseMenu::singleton().el_bgm_changed = false;
        if (thPracParam.mode == 1) {
            // TODO: Probably remove this ASM comment?
            /*
                    mov eax,dword ptr [@MENU_RANK]
                    mov dword ptr [69d710],eax
                    cmp dword ptr [@MENU_RANKLOCK],@MENU_ON_STR
                    jnz @f
                    mov dword ptr [69d714],eax
                    mov dword ptr [69d718],eax
                */
            GM_Lives() = (int8_t)thPracParam.life;
            GM_Bombs() = (int8_t)thPracParam.bomb;
            GM_Power() = (int16_t)thPracParam.power;
            GM_GuiScore() = GM_Score() = (int32_t)thPracParam.score;
            GM_GrazeInStage() = GM_GrazeInTotal() = (int32_t)thPracParam.graze;
            GM_PointInStage() = GM_PointTotal() = (int16_t)thPracParam.point;
            *(uint32_t*)&EM_TimelineCurrent() = thPracParam.frame;

            if ((int8_t)GM_Difficulty() != 4) {
                if (thPracParam.score >= 60000000)
                    GM_ExtraLives() = 4;
                else if (thPracParam.score >= 40000000)
                    GM_ExtraLives() = 3;
                else if (thPracParam.score >= 20000000)
                    GM_ExtraLives() = 2;
                else if (thPracParam.score >= 10000000)
                    GM_ExtraLives() = 1;
            }

            GM_Rank() = (int32_t)thPracParam.rank;
            if (thPracParam.rankLock) {
                GM_MaxRank() = (int32_t)thPracParam.rank;
                GM_MinRank() = (int32_t)thPracParam.rank;
            }

            THSectionPatch();
        }
        thPracParam._playLock = true;

        if (THPauseMenu::singleton().el_bgm_signal) {
            THPauseMenu::singleton().el_bgm_signal = false;
            pCtx->Eip = 0x41c18a;
        } else if (THBGMTest()) {
            pCtx->Eax += 0x310;
            pCtx->Eip = 0x41c17f;
        }
    })
    EHOOK_DY(th06_restart, 0x435901, 5, {
        if (!threstartflag_normalgame && !thRestartFlag) {
            th06_white_screen.Disable();
        }
        if (threstartflag_normalgame)
        {
            th06_white_screen.Enable();
            threstartflag_normalgame = false;
            pCtx->Eip = 0x436DCB;
        }
        if (thRestartFlag) {
            th06_white_screen.Enable();
            thRestartFlag = false;
            pCtx->Eip = 0x43738c;
        } else {
            thPracParam.Reset();
        }
    })
    EHOOK_DY(th06_title, 0x41ae2c, 7, {
        if (thPracParam.mode != 0 && thPracParam.section) {
            pCtx->Eip = 0x41af35;
        }
    })
    PATCH_DY(th06_preplay_1, 0x42d835, "09")
    EHOOK_DY(th06_preplay_2, 0x418ef9, 5, {
        if (thPracParam.mode && !THGuiRep::singleton().mRepStatus) {
            GM_GuiScore() = GM_Score();
            pCtx->Eip = 0x418f0e;
        }
    })
    EHOOK_DY(th06_save_replay, 0x42b03b, 3, {
        char* rep_name = *(char**)(pCtx->Ebp + 0x8);
        if (thPracParam.mode)
            THSaveReplay(rep_name);
    })
    EHOOK_DY(th06_rep_menu_1, 0x438262, 6, {
        THGuiRep::singleton().State(1);
    })
    EHOOK_DY(th06_rep_menu_2, 0x4385d5, 6, {
        THGuiRep::singleton().State(2);
    })
    EHOOK_DY(th06_rep_menu_3, 0x438974, 10, {
        THGuiRep::singleton().State(3);
    })
    EHOOK_DY(th06_fake_shot_type, 0x40b2f9, 6, {
        if (thPracParam.fakeType) {
            g_PlayerShot = thPracParam.fakeType - 1;
            pCtx->Eip = 0x40b2ff;
        }
    })
    EHOOK_DY(th06_patchouli, 0x40c100, 1, {
        int32_t* var = *(int32_t**)(pCtx->Esp + 4);
        if (thPracParam.fakeType) {
            var[618] = ((int32_t*)0x476264)[3 * (thPracParam.fakeType - 1)];
            var[619] = ((int32_t*)0x476268)[3 * (thPracParam.fakeType - 1)];
            var[620] = ((int32_t*)0x47626c)[3 * (thPracParam.fakeType - 1)];
            pCtx->Eip = 0x40c174;
        }
    })
    EHOOK_DY(th06_cancel_muteki, 0x429ec4, 7, {
        if (thPracParam.mode) {
            *(uint8_t*)(pCtx->Eax + 0x9e0) = 0;
            pCtx->Eip = 0x429ecb;
        }
    })
    EHOOK_DY(th06_set_deathbomb_timer, 0x42a09c, 10, {
        if (thPracParam.mode) {
            *(uint32_t*)(pCtx->Eax + 0x9d8) = 6;
            pCtx->Eip = 0x42a0a6;
        }
    })
    EHOOK_DY(th06_hamon_rage, 0x40e1c7, 10, {
        if (thPracParam.mode && thPracParam.stage == 6 && thPracParam.section == TH06_ST7_END_S10 && thPracParam.phase == 1) {
            pCtx->Eip = 0x40e1d8;
        }
    })
    EHOOK_DY(th06_wall_prac_boss_pos, 0x40907F,3,{
        if (thPracParam.mode && thPracParam.stage == 5 && thPracParam.wall_prac_st6 && thPracParam.section == TH06_ST6_BOSS9) {
            DWORD penm = *(DWORD*)(pCtx->Ebp + 0x8);
            g_last_boss_x = *(float*)(penm + 0xC6C);
            g_last_boss_y = *(float*)(penm + 0xC70);
        }
    })
    EHOOK_DY(th06_wall_prac, 0x40D57C,7,{
        if (thPracParam.mode && thPracParam.stage == 5  && thPracParam.wall_prac_st6) {
            auto GetRandF = []() -> float {
                unsigned int(__fastcall * sb_41E7F0_rand_int)(DWORD thiz);
                sb_41E7F0_rand_int = (decltype(sb_41E7F0_rand_int))0x41E7F0;
                unsigned int randi = sb_41E7F0_rand_int(0x69D8F8);
                return static_cast<float>((double)randi / 4294967296.0);
            }; // rand from 0 to 1
            if (thPracParam.section == TH06_ST6_BOSS6)
            {
                float posb1 = 0.4;
                float posb2 = 0.9;
                float posb3 = 0.2;
               
                float* wall_angle = (float*)(pCtx->Ebp - 0x68);
                DWORD penm = *(DWORD*)(pCtx->Ebp + 0x8);
                float bossx = *(float*)(penm + 0xC6C);
                float bossy = *(float*)(penm + 0xC70);
                float plx = PL_PosX();
                float ply = PL_PosY();
                float angle_pl = atan2f(ply - bossy, plx - bossx);
                float dist_pl = hypotf(ply - bossy, plx - bossx);
                
                float decision = GetRandF();
                
                // - 1.570796f + GetRandF() * 1.745329f
                if (decision < posb1) {
                    float min_dist_bt = 99999.0f;
                    for (int i = 0; i < 640; i++) {
                        DWORD pbt = 0x005AB5F8 + i * 0x5C4;
                        if (*(WORD*)(pbt + 0x5BE)
                            && *(WORD*)(pbt + 0x5BE) != 5
                            && *(DWORD*)(pbt + 0xC0)
                            && *(float*)(*(DWORD*)(pbt + 0xC0) + 0x2C) < 30.0
                            && *(float*)(pbt + 0x584) == 0.0)
                        {
                            ImVec2 pos = *(ImVec2*)(pbt + 0x560);
                            min_dist_bt = std::min(min_dist_bt, hypotf(pos.x - bossx, pos.y - bossy));
                        }
                    }
                    if (decision < posb3)
                        *wall_angle = angle_pl - min_dist_bt * 3.14159f / 256.0f - 0.5235988f + GetRandF() * 0.5235988f; // -30 deg ~ 0deg
                    else
                        *wall_angle = angle_pl - min_dist_bt * 3.14159f / 256.0f - 1.570796f + GetRandF() * 1.745329f; // -90 deg ~ 10deg
                } else if (decision < posb2) {
                    // angle = randA + dist*pi/256 = pi
                    // => randA = pi - dist*pi/256
                    *wall_angle = 3.14159f - dist_pl * 3.14159f / 256.0f - 0.2617f + GetRandF() * 0.5235988f; //
                }else {
                    *wall_angle = GetRandF() * 6.28318f - 3.1415926f;
                }
            } else if (thPracParam.section == TH06_ST6_BOSS9) {
                float* wall_angle = (float*)(pCtx->Ebp - 0x68);
                DWORD penm = *(DWORD*)(pCtx->Ebp + 0x8);
                float bossx = *(float*)(penm + 0xC6C);
                float bossy = *(float*)(penm + 0xC70);
                float plx = PL_PosX();
                float ply = PL_PosY();
                float angle_pl = atan2f(ply - bossy, plx - bossx);

                float decision = GetRandF();
                if (decision < 0.8) {
                    float min_dist_bt = 99999.0f;
                    ImVec2 bt_pos = { 0.0f, 0.0f };
                    for (int i = 0; i < 640; i++) {
                        DWORD pbt = 0x005AB5F8 + i * 0x5C4;
                        if (*(WORD*)(pbt + 0x5BE)
                            && *(WORD*)(pbt + 0x5BE) != 5
                            && *(DWORD*)(pbt + 0xC0)
                            && *(float*)(*(DWORD*)(pbt + 0xC0) + 0x2C) < 30.0
                            && *(float*)(pbt + 0x584) == 0.0) {
                            ImVec2 pos = *(ImVec2*)(pbt + 0x560);
                            auto dist = hypotf(pos.x - g_last_boss_x, pos.y - g_last_boss_y);
                            if (min_dist_bt > dist) {
                                min_dist_bt = dist;
                                bt_pos = pos;
                            }
                        }
                    }
                    *wall_angle = angle_pl - hypotf(bt_pos.x - bossx, bt_pos.y - bossy) * 3.14159f / 256.0f + (GetRandF() - 0.5f) * 2.0f * 0.34f;
                }else {
                    *wall_angle = GetRandF() * 6.28318f - 3.1415926f;
                }
            }
        }
    })
    EHOOK_DY(th06_wall_prac2, 0x0040D900,6,{
        if (thPracParam.mode && thPracParam.stage == 5 && thPracParam.wall_prac_st6 && thPracParam.section == TH06_ST6_BOSS9 
            && (thPracParam.snipeF > 0 || thPracParam.snipeN > 0)
            ) {
                float* angle = (float*)(pCtx->Ebp - 0x70);
                DWORD pbt = *(DWORD*)(pCtx->Ebp - 0x60);
                ImVec2 pos = *(ImVec2*)(pbt + 0x560);
                float plx = PL_PosX();
                float ply = PL_PosY();
                float dist_pl = hypotf(plx - pos.x, ply - pos.y);
                float angle_pl = atan2f(ply - pos.y, plx - pos.x);
                float random_near = 1.0f - thPracParam.snipeN/100.0f;
                float random_far = 1.0f - thPracParam.snipeF / 100.0f;
                if (dist_pl > 400.0f) {
                    dist_pl = 400.0f;
                }
                *angle = *angle * (dist_pl * (random_far - random_near) / 400.0f + random_near) + angle_pl;

        }
    })
    PATCH_DY(th06_disable_menu, 0x439ab2, "9090909090")
    EHOOK_DY(th06_update, 0x41caac, 1, {
        GameGuiBegin(IMPL_WIN32_DX8, !THAdvOptWnd::singleton().IsOpen() && !THConfigWnd::singleton().IsOpen() &&
                                         !THFirstRunWnd::singleton().IsOpen() && !th06::OnlineMenu::IsOpen());
        
        // Gui components update
        Gui::KeyboardInputUpdate(VK_ESCAPE);
        THPauseMenu::singleton().Update();
        THGuiPrac::singleton().Update();
        THGuiRep::singleton().Update();
        THOverlay::singleton().Update();
        TH06InGameInfo::singleton().Update();
        if (TH06InGameInfo::singleton().booksInfo.is_books) {
            TH06InGameInfo::singleton().booksInfo.time_books++;
        }
        TH06Save::singleton().IncreaseGameTime();

        auto p = ImGui::GetOverlayDrawList();
        RenderPlHitbox(ImGui::GetBackgroundDrawList());
        SSS::SSS_Update(6);

        RenderRepMarker(p);
        RenderBtHitbox(p);
        RenderLockTimer(p);
        GameUpdateOuter(p, 6);
        if (g_adv_igi_options.show_keyboard_monitor && (SV_CurState() == 2)) {
            g_adv_igi_options.keyboard_style.size = { 48.0f, 48.0f };
            KeysHUD(6, { 1280.0f, 0.0f }, { 833.0f, 0.0f }, g_adv_igi_options.keyboard_style);
        }
        {
            if (THAdvOptWnd::singleton().forceBossMoveDown) {
                auto sz = ImGui::CalcTextSize(S(TH_BOSS_FORCE_MOVE_DOWN));
                p->AddRectFilled({ 120.0f, 0.0f }, { sz.x + 120.0f, sz.y }, 0xFFCCCCCC);
                p->AddText({ 120.0f, 0.0f }, 0xFFFF0000, S(TH_BOSS_FORCE_MOVE_DOWN));
            }
        }
        if (th06::Netplay::IsPausePresentationHoldActive()) {
            const char* label = PausePresentationHoldLabel();
            ImVec2 textSize = ImGui::CalcTextSize(label);
            ImVec2 textPos((1280.0f - textSize.x) * 0.5f, 28.0f);
            ImVec2 boxMin(textPos.x - 14.0f, textPos.y - 8.0f);
            ImVec2 boxMax(textPos.x + textSize.x + 14.0f, textPos.y + textSize.y + 8.0f);
            p->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 176), 6.0f);
            p->AddRect(boxMin, boxMax, IM_COL32(255, 255, 255, 90), 6.0f);
            p->AddText(textPos, IM_COL32(255, 235, 160, 255), label);
        }

        th06::OnlineMenu::UpdateImGui();
        GameGuiEnd(THAdvOptWnd::StaticUpdate() || THConfigWnd::StaticUpdate() || THFirstRunWnd::StaticUpdate() ||
                   THGuiPrac::singleton().IsOpen() || THPauseMenu::singleton().IsOpen() || th06::OnlineMenu::IsOpen());
    })
    EHOOK_DY(th06_render, 0x41cb6d, 1, {
        GameGuiRender(IMPL_WIN32_DX8);

        if (Gui::GetChordPressed(Gui::GetScreenshotChord()))
            THSnapshot::Snapshot(nullptr); // SDL2: screenshot stub
    })
    EHOOK_DY(th06_player_state, 0x4288C0,1,
    {
        if (g_adv_igi_options.show_keyboard_monitor)
            RecordKey(6, GM_CurInput());
    })
    EHOOK_DY(th06_rep_seed,0x42A97E,10,{
       g_last_rep_seed = GM_RngSeed();
    })
    EHOOK_DY(th06_fix_seed, 0x41BE47,7,{
        if (g_adv_igi_options.th06_fix_seed)
        {
            if ((GM_Stage() == 1 || GM_Stage() == 7)) {
                if ((!thPracParam.mode) || thPracParam.section == 10000 + 1 * 100 + 1 || thPracParam.section == 0)
                {
                    GM_RngSeed() = (uint16_t)((int32_t)g_adv_igi_options.th06_seed & 0xFFFF);
                }
            }
        }
    })
    HOOKSET_ENDDEF()

    HOOKSET_DEFINE(THInGameInfo)
    EHOOK_DY(th06_enter_game, 0x41BDE8,4, // set inner misscount to 0
    {
        TH06InGameInfo::singleton().GameStartInit();
        // TH06InGameInfo::singleton().Retry();
    })
    // EHOOK_DY(miss_spellcard_get_failed, 0x4277C3,3,
    // {
    //     is_died = true;
    // })
    EHOOK_DY(th06_miss, 0x428DD9,2,// dec life
    {
        TH06InGameInfo::singleton().mMissCount++;
        FastRetry(thPracParam.mode);
    })
    EHOOK_DY(th06_bomb, 0x4289CC, 6, {
    })
    EHOOK_DY(th06_lock_timer1, 0x41B27C,3, // initialize
    {
        g_lock_timer = 0;
    })
    EHOOK_DY(th06_lock_timer2, 0x409A10,6, // set timeout case 115
    {
        g_lock_timer = 0;
    })
    EHOOK_DY(th06_lock_timer3, 0x408DDA,2, // set boss mode case 101
    {
        g_lock_timer = 0;
    })
    EHOOK_DY(th06_lock_timer4, 0x411F88,6, // decrease time (update)
    {
        g_lock_timer++;
    })

    EHOOK_DY(th06_autoName_score,0x42BE49,5,{
        if (g_adv_igi_options.th06_autoname){
            int size = strlen(g_adv_igi_options.th06_autoname_name);
            if (size > 8)
                size = 8;
            if (size >= 1){
                *(DWORD*)(pCtx->Eax + 0x10) = size - 1;
                for (int i = 0; i < size; i++) {
                    *(char*)(pCtx->Eax + 0x5193 + i) = (g_adv_igi_options.th06_autoname_name[i] == '~') ? '\xA5' : g_adv_igi_options.th06_autoname_name[i]; // red slash = 0xA5
                }
            }
        }
    })
    EHOOK_DY(th06_autoName_rep_overwrite, 0x42D085,7,
    {
        if (g_adv_igi_options.th06_autoname) {
            bool set_name = true;
            int name_len = 0;
            for (int i = 0; i < 8; i++) {
                char ch = *(char*)(pCtx->Eax + 0x34 + i);
                if (ch != ' ' && ch!=0) {
                    set_name = false;
                    name_len = i;
                }
            }
            if (set_name) {
                int size = strlen(g_adv_igi_options.th06_autoname_name);
                if (size > 8)
                    size = 8;
                if (size >= 1) {
                    *(DWORD*)(pCtx->Eax + 0x10) = size - 1;
                    for (int i = 0; i < 8; i++) {
                        if (i < size)
                            *(char*)(pCtx->Eax + 0x34 + i) = (g_adv_igi_options.th06_autoname_name[i] == '~') ? '\xA5' : g_adv_igi_options.th06_autoname_name[i]; // red slash = 0xA5
                        else
                            *(char*)(pCtx->Eax + 0x34 + i) = ' ';
                    }
                }
            }else{
                *(DWORD*)(pCtx->Eax + 0x10) = name_len;
            }
        }
    })
    EHOOK_DY(th06_autoName_rep, 0x42C8A0,2,
    {
        if (g_adv_igi_options.th06_autoname) {
            bool set_name = true;
            int name_len = 0;
            for (int i = 0; i < 8; i++){
                char ch = *(char*)(pCtx->Eax + 0x34 + i);
                if (ch != ' ' && ch != 0) {
                    set_name = false;
                    name_len = i;
                }
            }
            if (set_name){
                int size = strlen(g_adv_igi_options.th06_autoname_name);
                if (size > 8)
                    size = 8;
                if (size >= 1) {
                    *(DWORD*)(pCtx->Eax + 0x10) = size - 1;
                    for (int i = 0; i < 8; i++) {
                        if (i < size)
                            *(char*)(pCtx->Eax + 0x34 + i) = (g_adv_igi_options.th06_autoname_name[i] == '~') ? '\xA5' : g_adv_igi_options.th06_autoname_name[i]; // red slash = 0xA5
                        else
                            *(char*)(pCtx->Eax + 0x34 + i) = ' ';
                    }
                    pCtx->Eip = 0x42C91C;
                }
            }else{
                *(DWORD*)(pCtx->Eax + 0x10) = name_len;
                pCtx->Eip = 0x42C91C;
            }
        }
       
    })
    HOOKSET_ENDDEF()

    HOOKSET_DEFINE(TH06_SavesHook)
    EHOOK_DY(th06BooksStart, 0x41188A, 3, {
        // timeline ins_4
        // also saves books' pos
        if (GM_Stage() == 4) { // stage 4
            DWORD pCode = (DWORD)ECL_File();
            DWORD pCodeOfs = *(DWORD*)(pCtx->Ebp - 0x14) - 8;
            float posx = *(float*)(pCtx->Ebp - 0x20);
            float posy = *(float*)(pCtx->Ebp - 0x1C);
            int n = (pCodeOfs - pCode - 0xCDD4) / 0x1C;
            g_books_pos[n] = { posx, posy };

            if (n == 0) {//1st book
                TH06InGameInfo::singleton().booksInfo.is_books = true;
                TH06InGameInfo::singleton().booksInfo.time_books = 0;
                TH06InGameInfo::singleton().booksInfo.is_died = false;
                TH06InGameInfo::singleton().booksInfo.miss_count = TH06InGameInfo::singleton().mMissCount;
                TH06InGameInfo::singleton().booksInfo.bomb_count = GM_BombsUsed();
                bool is_rep = GM_IsInReplay();
                if (!is_rep) {
                    if ( !(thPracParam.mode && thPracParam.stage == 3 && thPracParam.section == TH06_ST4_BOOKS && thPracParam.phase != 0)) {// not in other Tests
                        byte cur_player_typea = GM_Character();
                        byte cur_player_typeb = GM_ShotType();
                        byte cur_player_type = (cur_player_typea << 1) | cur_player_typeb;
                        int8_t diff = (int8_t)GM_Difficulty();
                        int32_t spell_id = TH06Save::spellid_books;
                        TH06Save::singleton().AddAttempt(spell_id, diff, cur_player_type);
                    }
                }
            }
        }
    })
    EHOOK_DY(th06BooksEnd, 0x411669, 3, {
        // timeline ins_0
        // also saves books' pos
        if (GM_Stage() == 4 && TH06InGameInfo::singleton().booksInfo.is_books == true) { // stage 4
            DWORD pCode = (DWORD)ECL_File();
            DWORD pCodeOfs = *(DWORD*)(pCtx->Ebp - 0xC) - 8;
            if (pCodeOfs - pCode == 0xCE7C) { // mid boss

                TH06InGameInfo::singleton().booksInfo.is_books = false;
                TH06InGameInfo::singleton().booksInfo.time_books = 0;
                bool is_rep = GM_IsInReplay();
                if (!is_rep) {
                    if (!(thPracParam.mode && thPracParam.stage == 3 && thPracParam.section == TH06_ST4_BOOKS && thPracParam.phase != 0)) { // not in other Tests

                        if (TH06InGameInfo::singleton().booksInfo.is_died == false
                        && TH06InGameInfo::singleton().booksInfo.miss_count == TH06InGameInfo::singleton().mMissCount
                        && TH06InGameInfo::singleton().booksInfo.bomb_count == GM_BombsUsed()) {
                            byte cur_player_typea = GM_Character();
                            byte cur_player_typeb = GM_ShotType();
                            byte cur_player_type = (cur_player_typea << 1) | cur_player_typeb;
                            int8_t diff = (int8_t)GM_Difficulty();
                            int32_t spell_id = TH06Save::spellid_books;
                            TH06Save::singleton().AddCapture(spell_id, diff, cur_player_type);
                        }
                        TH06InGameInfo::singleton().booksInfo.is_died = false;
                    }
                }
            }
        }
    })
    EHOOK_DY(miss_spellcard_get_failed, 0x4277C3, 3,
    {
        if (TH06InGameInfo::singleton().booksInfo.is_books == true)
        {
            TH06InGameInfo::singleton().booksInfo.is_died = true;
        }
    })
    EHOOK_DY(th06_close, 0x420669, 2, {
        TH06Save::singleton().SaveSave();
    })
    EHOOK_DY(th06_spellStart, 0x4096e8, 7,
        {
            bool is_rep = GM_IsInReplay();
            if (!is_rep) {
                byte cur_player_typea = GM_Character();
                byte cur_player_typeb = GM_ShotType();
                byte cur_player_type = (cur_player_typea << 1) | cur_player_typeb;
                int8_t diff = (int8_t)GM_Difficulty();
                int32_t spell_id = (int8_t)EM_SpellIdx();
                TH06Save::singleton().AddAttempt(spell_id, diff, cur_player_type);
            }
        })
    EHOOK_DY(th06_spellCapture, 0x409978, 6,
        {
            bool is_rep = GM_IsInReplay();
            if (!is_rep) {
                byte cur_player_typea = GM_Character();
                byte cur_player_typeb = GM_ShotType();
                byte cur_player_type = (cur_player_typea << 1) | cur_player_typeb;
                int8_t diff = (int8_t)GM_Difficulty();
                int32_t spell_id = (int8_t)EM_SpellIdx();
                TH06Save::singleton().AddCapture(spell_id, diff, cur_player_type);
            }
        })
    EHOOK_DY(th06_spellTimeout, 0x412056, 10,
        {
            bool is_rep = GM_IsInReplay();
            if (!is_rep) {
                DWORD is_capture = EM_IsCapturing();
                if (is_capture)
                {
                    byte cur_player_typea = GM_Character();
                    byte cur_player_typeb = GM_ShotType();
                    byte cur_player_type = (cur_player_typea << 1) | cur_player_typeb;
                    int8_t diff = (int8_t)GM_Difficulty();
                    int32_t spell_id = (int8_t)EM_SpellIdx();
                    int32_t stage = GM_Stage();
                    if (! (spell_id == 1 && stage != 1))// to avoid spell record bug
                    {
                        TH06Save::singleton().AddTimeOut(spell_id, diff, cur_player_type);
                    }
                }
            }
        })
    HOOKSET_ENDDEF()

    static bool s_guiCreated = false;

     __declspec(noinline) void THGuiCreate()
     {
         if (s_guiCreated) return;
         s_guiCreated = true;

        // GameGuiInit is a no-op — ImGui context initialized by THPracGuiInit
        GameGuiInit(IMPL_WIN32_DX8, 0, 0, // SDL2: device/window addresses unused
            Gui::INGAGME_INPUT_GEN1, 0, 0, 0,
            -1);
         SetDpadHook(0x41D330, 3);
        // g_adv_igi_options.th06_showHitbox
        g_hitbox_textureID = ReadImage(8, 0, "hitbox.png", hitbox_file, sizeof(hitbox_file)); // SDL2: device unused
        if (g_hitbox_textureID) {
            D3DSURFACE_DESC desc;
            ((LPDIRECT3DTEXTURE8)g_hitbox_textureID)->GetLevelDesc(0, &desc);
            g_hitbox_sz.x = static_cast<float>(desc.Width);
            g_hitbox_sz.y = static_cast<float>(desc.Height);
        }

        // Gui components creation
        THGuiPrac::singleton();
        THPauseMenu::singleton();
        THGuiRep::singleton();
        THOverlay::singleton();
        TH06InGameInfo::singleton();
        TH06Save::singleton();
        THConfigWnd::singleton();
        // Hooks
        EnableAllHooks(THMainHook);
        EnableAllHooks(THInGameInfo);
        EnableAllHooks(TH06_SavesHook);

        th06_white_screen.Setup();
        th06_result_screen_create.Setup();
        if (g_adv_igi_options.th06_bg_fix)
        {
            EnableAllHooks(TH06BgFix);
        }

        // Reset thPracParam
        thPracParam.Reset();

    }

    HOOKSET_DEFINE(THInitHook)
    EHOOK_DY(th06_gui_init_1, 0x43596f, 3, {
        THGuiCreate();
        self->Disable();
    })
    EHOOK_DY(th06_gui_init_2, 0x42140c, 1, {
        THGuiCreate();
        self->Disable();
    })
    HOOKSET_ENDDEF()

    void THPracUpdate()
    {
        if (!ImGui::GetCurrentContext())
            return;

        GameGuiBegin(IMPL_WIN32_DX8, !THAdvOptWnd::singleton().IsOpen() && !THConfigWnd::singleton().IsOpen() &&
                                         !THFirstRunWnd::singleton().IsOpen() && !th06::OnlineMenu::IsOpen());

        // Gui components update
        Gui::KeyboardInputUpdate(VK_ESCAPE);
        THPauseMenu::singleton().Update();
        THGuiPrac::singleton().Update();
        THGuiRep::singleton().Update();
        THOverlay::singleton().Update();

        TH06InGameInfo::singleton().Update();
        if (TH06InGameInfo::singleton().booksInfo.is_books) {
            TH06InGameInfo::singleton().booksInfo.time_books++;
        }
        TH06Save::singleton().IncreaseGameTime();

        auto p = ImGui::GetOverlayDrawList();
        RenderPlHitbox(ImGui::GetBackgroundDrawList());
        SSS::SSS_Update(6);

        RenderRepMarker(p);
        RenderBtHitbox(p);
        RenderLockTimer(p);
        GameUpdateOuter(p, 6);
        if (g_adv_igi_options.show_keyboard_monitor && (SV_CurState() == 2)) {
            g_adv_igi_options.keyboard_style.size = { 48.0f, 48.0f };
            KeysHUD(6, { 1280.0f, 0.0f }, { 833.0f, 0.0f }, g_adv_igi_options.keyboard_style);
        }
        {
            if (THAdvOptWnd::singleton().forceBossMoveDown) {
                auto sz = ImGui::CalcTextSize(S(TH_BOSS_FORCE_MOVE_DOWN));
                p->AddRectFilled({ 120.0f, 0.0f }, { sz.x + 120.0f, sz.y }, 0xFFCCCCCC);
                p->AddText({ 120.0f, 0.0f }, 0xFFFF0000, S(TH_BOSS_FORCE_MOVE_DOWN));
            }
        }

        th06::OnlineMenu::UpdateImGui();
        {
            std::string recoveryLine1;
            std::string recoveryLine2;
            int receivedChunks = 0;
            int totalChunks = 0;
            if (th06::Netplay::GetAuthoritativeRecoveryOverlay(recoveryLine1, recoveryLine2, receivedChunks, totalChunks))
            {
                ImGuiViewport *viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowBgAlpha(1.0f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(96, 96, 96, 230));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(180, 180, 180, 255));
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(245, 245, 245, 255));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
                const ImGuiWindowFlags recoveryWindowFlags =
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;
                if (ImGui::Begin("##authoritative_recovery_overlay", nullptr, recoveryWindowFlags))
                {
                    ImGui::TextUnformatted(recoveryLine1.c_str());
                    if (!recoveryLine2.empty())
                    {
                        ImGui::TextUnformatted(recoveryLine2.c_str());
                    }
                    if (totalChunks > 0)
                    {
                        ImGui::Text("%s: %d / %d", RecoveryChunksLabel(), receivedChunks, totalChunks);
                    }
                }
                ImGui::End();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(3);
            }
        }
        GameGuiEnd(THAdvOptWnd::StaticUpdate() || THConfigWnd::StaticUpdate() || THFirstRunWnd::StaticUpdate() ||
                   THGuiPrac::singleton().IsOpen() || THPauseMenu::singleton().IsOpen() || th06::OnlineMenu::IsOpen());
    }

    void THPracRender()
    {
        if (!ImGui::GetCurrentContext())
            return;
        GameGuiRender(IMPL_WIN32_DX8);
    }

    void THPracMenuOpen()
    {
        THGuiPrac::singleton().State(1);
    }

    void THPracMenuConfirm()
    {
        THGuiPrac::singleton().State(3);
    }

    void THPracMenuCancel()
    {
        THGuiPrac::singleton().State(4);
    }

    int THPracMenuApply()
    {
        int stage = thPracParam.stage;
        return stage;
    }

    bool THPracIsActive()
    {
        return thPracParam.mode != 0;
    }

    void THPracApplyStageParams()
    {
        TH06InGameInfo::singleton().GameStartInit();
        // Only reset miss count at the start of a new game run, not on stage transitions
        if (th06::g_Supervisor.curState != th06::SUPERVISOR_STATE_GAMEMANAGER_REINIT)
        {
            TH06InGameInfo::singleton().GameRunInit();
        }
        g_lock_timer = 0;
        g_last_rep_seed = GM_RngSeed();

        // NOTE: Fixed seed is applied later via THPracFixSeed(), called after
        // mgr->randomSeed = g_Rng.seed to match original thprac hook timing.

        if (thPracParam.mode != 1)
            return;

        // Apply player state overrides
        GM_Lives() = (int8_t)thPracParam.life;
        GM_Bombs() = (int8_t)thPracParam.bomb;
        GM_Power() = (int16_t)thPracParam.power;
        GM_GuiScore() = GM_Score() = (int32_t)thPracParam.score;
        GM_GrazeInStage() = GM_GrazeInTotal() = (int32_t)thPracParam.graze;
        GM_PointInStage() = GM_PointTotal() = (int16_t)thPracParam.point;

        // NOTE: Timeline warp (EM_TimelineCurrent) is set in THPracPostEclLoad(),
        // AFTER EnemyManager::Initialize() which memsets everything to 0.

        // Fix extra lives counter based on score thresholds
        if ((int8_t)GM_Difficulty() != 4) {
            if (thPracParam.score >= 60000000)
                GM_ExtraLives() = 4;
            else if (thPracParam.score >= 40000000)
                GM_ExtraLives() = 3;
            else if (thPracParam.score >= 20000000)
                GM_ExtraLives() = 2;
            else if (thPracParam.score >= 10000000)
                GM_ExtraLives() = 1;
        }

        // Apply rank
        GM_Rank() = (int32_t)thPracParam.rank;
        if (thPracParam.rankLock) {
            GM_MaxRank() = (int32_t)thPracParam.rank;
            GM_MinRank() = (int32_t)thPracParam.rank;
        }

        thPracParam._playLock = true;
    }

    bool THPracIsDeveloperModeEnabled()
    {
        return g_developer_mode_enabled;
    }

    bool THPracIsNewTouchEnabled()
    {
        return g_new_touch_enabled;
    }

    bool THPracIsMouseFollowEnabled()
    {
        return g_mouse_follow_enabled;
    }

    bool THPracIsMouseTouchDragEnabled()
    {
        return g_mouse_touch_drag_enabled;
    }

    bool THPracIsAstroBotEnabled()
    {
        return g_developer_mode_enabled && g_adv_igi_options.th06_enable_astrobot;
    }

    int THPracGetAstroBotTarget()
    {
        return static_cast<int>(g_adv_igi_options.th06_astrobot_target);
    }

    bool THPracIsAstroBotAutoShootEnabled()
    {
        return g_adv_igi_options.th06_astrobot_auto_shoot;
    }

    bool THPracIsAstroBotAutoBombEnabled()
    {
        return g_adv_igi_options.th06_astrobot_auto_bomb;
    }

    bool THPracIsAstroBotBossOnlyEnabled()
    {
        return g_adv_igi_options.th06_astrobot_boss_only;
    }

    bool THPracIsManualDumpHotkeyEnabled()
    {
        return g_developer_mode_enabled && g_adv_igi_options.th06_enable_manual_dump_hotkey;
    }

    bool THPracIsRecoveryAutoDumpEnabled()
    {
        return g_developer_mode_enabled && g_adv_igi_options.th06_enable_recovery_auto_dump;
    }

    bool THPracIsDebugLogEnabled()
    {
        return g_adv_igi_options.th06_enable_debug_logs;
    }

    bool THPracConsumeEndingShortcut()
    {
        if (!g_developer_mode_enabled)
        {
            THPracResetEndingShortcut();
            return false;
        }

        const bool rawE = Gui::KeyboardInputGetRaw('E');
        const bool rawN = Gui::KeyboardInputGetRaw('N');
        const bool rawD = Gui::KeyboardInputGetRaw('D');
        const bool pressE = rawE && !g_ending_shortcut_prev_e;
        const bool pressN = rawN && !g_ending_shortcut_prev_n;
        const bool pressD = rawD && !g_ending_shortcut_prev_d;
        g_ending_shortcut_prev_e = rawE;
        g_ending_shortcut_prev_n = rawN;
        g_ending_shortcut_prev_d = rawD;

        const u32 now = SDL_GetTicks();
        if (g_ending_shortcut_progress != 0 && now - g_ending_shortcut_last_tick > ENDING_SHORTCUT_TIMEOUT_MS)
        {
            g_ending_shortcut_progress = 0;
        }

        switch (g_ending_shortcut_progress)
        {
        case 0:
            if (pressE)
            {
                g_ending_shortcut_progress = 1;
                g_ending_shortcut_last_tick = now;
            }
            break;
        case 1:
            if (pressN)
            {
                g_ending_shortcut_progress = 2;
                g_ending_shortcut_last_tick = now;
            }
            else if (pressE)
            {
                g_ending_shortcut_last_tick = now;
            }
            else if (pressD)
            {
                g_ending_shortcut_progress = 0;
            }
            break;
        case 2:
            if (pressD)
            {
                THPracResetEndingShortcut();
                return true;
            }
            if (pressE)
            {
                g_ending_shortcut_progress = 1;
                g_ending_shortcut_last_tick = now;
            }
            else if (pressN)
            {
                g_ending_shortcut_last_tick = now;
            }
            break;
        default:
            g_ending_shortcut_progress = 0;
            break;
        }

        return false;
    }

    void THPracResetEndingShortcut()
    {
        g_ending_shortcut_progress = 0;
        g_ending_shortcut_last_tick = 0;
        g_ending_shortcut_prev_e = false;
        g_ending_shortcut_prev_n = false;
        g_ending_shortcut_prev_d = false;
    }

    void THPracPrepareDebugEndingJump()
    {
        g_debug_ending_jump_active = true;

        if (th06::g_GameManager.character > th06::CHARA_MARISA)
        {
            th06::g_GameManager.character = th06::CHARA_REIMU;
        }
        if (th06::g_GameManager.shotType > th06::SHOT_TYPE_B)
        {
            th06::g_GameManager.shotType = th06::SHOT_TYPE_A;
        }

        th06::g_GameManager.character2 = th06::g_GameManager.character;
        th06::g_GameManager.shotType2 = th06::g_GameManager.shotType;
        th06::g_GameManager.difficulty =
            std::clamp(th06::g_GameManager.difficulty, th06::NORMAL, th06::LUNATIC);
        th06::g_GameManager.numRetries = 0;
        th06::g_GameManager.isInReplay = 0;
        th06::g_GameManager.demoMode = 0;
        th06::g_GameManager.isInPracticeMode = 0;
        th06::g_GameManager.isInGameMenu = 0;
        th06::g_GameManager.isInRetryMenu = 0;
        th06::g_GameManager.isInMenu = 0;
        th06::g_GameManager.isGameCompleted = 0;
        th06::g_GameManager.currentStage = th06::FINAL_STAGE;
        th06::g_GameManager.guiScore = th06::g_GameManager.score;

        th06::g_Supervisor.framerateMultiplier = 1.0f;
        th06::g_Supervisor.effectiveFramerateMultiplier = 1.0f;
        th06::g_Supervisor.isInEnding = false;
    }

    bool THPracIsDebugEndingJumpActive()
    {
        return g_debug_ending_jump_active;
    }

    void THPracClearDebugEndingJump()
    {
        g_debug_ending_jump_active = false;
    }

    bool THPracIsMuteki()   { return *THOverlay::singleton().mMuteki; }
    bool THPracIsInfLives() { return *THOverlay::singleton().mInfLives; }
    bool THPracIsInfBombs() { return *THOverlay::singleton().mInfBombs; }
    bool THPracIsInfPower() { return *THOverlay::singleton().mInfPower; }
    bool THPracIsTimeLock() { return *THOverlay::singleton().mTimeLock; }
    bool THPracIsAutoBomb() { return *THOverlay::singleton().mAutoBomb; }

    void THPracCountMiss()  { TH06InGameInfo::singleton().mMissCount++; }
    bool THPracIsRankLockDown() { return g_adv_igi_options.th06_disable_drop_rank; }
    int32_t THPracGetRepSeed() { return g_last_rep_seed; }
    void THPracLockTimerTick()  { g_lock_timer++; }
    void THPracLockTimerReset() { g_lock_timer = 0; }
    void THPracCaptureRepSeed() { g_last_rep_seed = GM_RngSeed(); }
    void THPracResetParams()    { thPracParam.Reset(); }

    // Called AFTER mgr->randomSeed = g_Rng.seed, matching original thprac hook at 0x41BE47.
    void THPracFixSeed()
    {
        if (g_adv_igi_options.th06_fix_seed)
        {
            if (GM_Stage() == 1 || GM_Stage() == 7) {
                if (!thPracParam.mode || thPracParam.section == 10000 + 1 * 100 + 1 || thPracParam.section == 0)
                {
                    GM_RngSeed() = (uint16_t)((int32_t)g_adv_igi_options.th06_seed & 0xFFFF);
                }
            }
        }
    }

    // Fast-forward the Stage STD background script to the target frame.
    // This processes all camera, fog, and facing instructions so the
    // background matches the warp point instead of starting from frame 0.
    static void THPracFastForwardStage(int targetFrame)
    {
        th06::Stage& stg = th06::g_Stage;
        if (!stg.stdData || !stg.beginningOfScript) return;
        if (targetFrame <= 0) return;

        // Set scriptTime to target so all frame <= target instructions pass the >= check
        stg.scriptTime.SetCurrent(targetFrame);

        // Process all STD instructions up to targetFrame in a single pass
        for (;;) {
            th06::RawStageInstr* curInsn = &stg.beginningOfScript[stg.instructionIndex];
            bool advanced = false;

            switch (curInsn->opcode) {
            case th06::STDOP_CAMERA_POSITION_KEY:
                if (curInsn->frame == -1) {
                    // Initial position sentinel — set position and advance past it
                    // so subsequent instructions (FOG, FACING, etc.) get processed.
                    stg.positionInterpInitial = *(D3DXVECTOR3*)curInsn->args;
                    stg.position = stg.positionInterpInitial;
                    stg.instructionIndex++;
                    advanced = true;
                } else if (curInsn->frame <= targetFrame) {
                    D3DXVECTOR3 pos = *(D3DXVECTOR3*)curInsn->args;
                    stg.position = pos;
                    stg.positionInterpInitial = pos;
                    stg.positionInterpStartTime = curInsn->frame;
                    stg.instructionIndex++;
                    // Scan forward to find next position key for interpolation target
                    th06::RawStageInstr* next = curInsn + 1;
                    while (next->opcode != th06::STDOP_CAMERA_POSITION_KEY) next++;
                    stg.positionInterpEndTime = next->frame;
                    stg.positionInterpFinal = *(D3DXVECTOR3*)next->args;
                    advanced = true;
                }
                break;
            case th06::STDOP_FOG:
                if (curInsn->frame <= targetFrame) {
                    stg.skyFog.color = curInsn->args[0];
                    stg.skyFog.nearPlane = ((f32*)curInsn->args)[1];
                    stg.skyFog.farPlane = ((f32*)curInsn->args)[2];
                    stg.skyFogInterpFinal = stg.skyFog;
                    stg.instructionIndex++;
                    advanced = true;
                }
                break;
            case th06::STDOP_CAMERA_FACING:
                if (curInsn->frame <= targetFrame) {
                    stg.facingDirInterpInitial = stg.facingDirInterpFinal;
                    stg.facingDirInterpFinal = *(D3DXVECTOR3*)curInsn->args;
                    stg.instructionIndex++;
                    advanced = true;
                }
                break;
            case th06::STDOP_CAMERA_FACING_INTERP_LINEAR:
                if (curInsn->frame <= targetFrame) {
                    stg.facingDirInterpDuration = curInsn->args[0];
                    stg.facingDirInterpTimer.InitializeForPopup();
                    stg.instructionIndex++;
                    advanced = true;
                }
                break;
            case th06::STDOP_FOG_INTERP:
                if (curInsn->frame <= targetFrame) {
                    stg.skyFogInterpInitial = stg.skyFog;
                    stg.skyFogInterpDuration = curInsn->args[0];
                    stg.skyFogInterpTimer.InitializeForPopup();
                    stg.instructionIndex++;
                    advanced = true;
                }
                break;
            case th06::STDOP_PAUSE:
                // Force-skip pause during warp (normally waits for ECL unpause)
                stg.instructionIndex++;
                stg.unpauseFlag = 0;
                advanced = true;
                break;
            default:
                break;
            }
            if (!advanced) break;
        }

        // Compute interpolated camera position at targetFrame
        if (stg.positionInterpEndTime > stg.positionInterpStartTime) {
            f32 ratio = ((f32)targetFrame - (f32)stg.positionInterpStartTime) /
                        ((f32)stg.positionInterpEndTime - (f32)stg.positionInterpStartTime);
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            stg.position.x = stg.positionInterpInitial.x +
                             (stg.positionInterpFinal.x - stg.positionInterpInitial.x) * ratio;
            stg.position.y = stg.positionInterpInitial.y +
                             (stg.positionInterpFinal.y - stg.positionInterpInitial.y) * ratio;
            stg.position.z = stg.positionInterpInitial.z +
                             (stg.positionInterpFinal.z - stg.positionInterpInitial.z) * ratio;
        }

        // Complete facing direction interpolation instantly
        if (stg.facingDirInterpDuration > 0) {
            th06::g_GameManager.stageCameraFacingDir = stg.facingDirInterpFinal;
            stg.facingDirInterpDuration = 0;
        }

        // Complete fog interpolation and apply
        if (stg.skyFogInterpDuration > 0) {
            stg.skyFog = stg.skyFogInterpFinal;
            stg.skyFogInterpDuration = 0;
        }
        th06::g_Renderer->SetFog(1, stg.skyFog.color, stg.skyFog.nearPlane, stg.skyFog.farPlane);
    }

    void THPortableFastForwardStageShell(int targetFrame)
    {
        THPracFastForwardStage(targetFrame);
    }

    bool THPortableReloadBossSectionAssets(int profile)
    {
        switch (profile)
        {
        case PORTABLE_BOSS_ASSET_PROFILE_NONE:
            g_portable_current_boss_asset_profile = PORTABLE_BOSS_ASSET_PROFILE_NONE;
            return true;
        case PORTABLE_BOSS_ASSET_PROFILE_STAGE6_BOSS_EFFECTS:
            if (th06::g_AnmManager->LoadAnm(11, "data/eff06.anm", 691) != ZUN_SUCCESS)
            {
                return false;
            }
            g_portable_current_boss_asset_profile = PORTABLE_BOSS_ASSET_PROFILE_STAGE6_BOSS_EFFECTS;
            return true;
        case PORTABLE_BOSS_ASSET_PROFILE_STAGE7_END_EFFECTS:
            if (th06::g_AnmManager->LoadAnm(11, "data/eff07.anm", 691) != ZUN_SUCCESS)
            {
                return false;
            }
            if (th06::g_AnmManager->LoadAnm(18, "data/face12c.anm", 1192) != ZUN_SUCCESS)
            {
                return false;
            }
            g_portable_current_boss_asset_profile = PORTABLE_BOSS_ASSET_PROFILE_STAGE7_END_EFFECTS;
            return true;
        default:
            return false;
        }
    }

    bool THPortableSyncStageIntroSprites(bool hideStageNameIntro, bool hideSongNameIntro)
    {
        if (th06::g_Gui.impl == nullptr || th06::g_Stage.stdData == nullptr)
        {
            return false;
        }

        if (hideStageNameIntro)
        {
            th06::g_Gui.impl->stageNameSprite.flags.isVisible = 0;
            th06::g_Gui.impl->stageNameSprite.currentInstruction = NULL;
        }
        else
        {
            th06::g_AnmManager->SetAndExecuteScriptIdx(&th06::g_Gui.impl->stageNameSprite, ANM_SCRIPT_TEXT_STAGE_NAME);
            th06::AnmManager::DrawStringFormat2(th06::g_AnmManager, &th06::g_Gui.impl->stageNameSprite,
                                                COLOR_RGB(COLOR_LIGHTCYAN), COLOR_RGB(COLOR_BLACK),
                                                th06::g_Stage.stdData->stageName);
        }

        if (hideSongNameIntro)
        {
            th06::g_Gui.impl->songNameSprite.flags.isVisible = 0;
            th06::g_Gui.impl->songNameSprite.currentInstruction = NULL;
        }
        else
        {
            th06::g_AnmManager->SetAndExecuteScriptIdx(&th06::g_Gui.impl->songNameSprite, 0x701);
            th06::g_Gui.impl->songNameSprite.fontWidth = 16;
            th06::g_Gui.impl->songNameSprite.fontHeight = 16;
            const int songIdx = (g_portable_current_bgm_track_index >= 0 && g_portable_current_bgm_track_index <= 1)
                                    ? g_portable_current_bgm_track_index
                                    : 0;
            th06::AnmManager::DrawStringFormat(th06::g_AnmManager, &th06::g_Gui.impl->songNameSprite,
                                               COLOR_RGB(COLOR_LIGHTCYAN), COLOR_RGB(COLOR_BLACK), "\x01%s",
                                               th06::g_Stage.stdData->songNames[songIdx]);
        }

        return true;
    }

    bool THPortableSyncStageBgm(int trackIndex)
    {
        if (th06::g_Stage.stdData == nullptr || trackIndex < 0 || trackIndex > 1)
        {
            return false;
        }

        th06::g_Supervisor.ReadMidiFile(1, th06::g_Stage.stdData->songPaths[1]);
        if (th06::g_Supervisor.PlayAudio(th06::g_Stage.stdData->songPaths[trackIndex]) != ZUN_SUCCESS)
        {
            return false;
        }
        g_portable_current_bgm_track_index = trackIndex;
        return true;
    }

    void THPortableSetCurrentBgmTrackIndex(int trackIndex)
    {
        g_portable_current_bgm_track_index = trackIndex;
    }

    int THPortableGetCurrentBgmTrackIndex()
    {
        return g_portable_current_bgm_track_index;
    }

    void THPortableSetCurrentBossAssetProfile(int profile)
    {
        g_portable_current_boss_asset_profile = profile;
    }

    int THPortableGetCurrentBossAssetProfile()
    {
        return g_portable_current_boss_asset_profile;
    }

    void THPortableResetShellSyncTrackers()
    {
        g_portable_current_bgm_track_index = -1;
        g_portable_current_boss_asset_profile = PORTABLE_BOSS_ASSET_PROFILE_NONE;
    }

    // Called AFTER EnemyManager::RegisterChain + EclManager::Load.
    void THPracPostEclLoad()
    {
        if (thPracParam.mode != 1)
            return;

        // Set timeline warp AFTER EnemyManager init (which memsets to 0).
        // For frame-specific warps (type 6), thPracParam.frame is the target.
        // For other warps, ECLWarp() inside THSectionPatch will override this.
        *(uint32_t*)&EM_TimelineCurrent() = thPracParam.frame;

        THSectionPatch();

        // Read the actual timeline time after ECL patching — THSectionPatch
        // may have called ECLWarp() which sets EM_TimelineCurrent to a value
        // different from thPracParam.frame (for Chapter/Boss/Spell warps).
        int actualFrame = (int)EM_TimelineCurrent();

        // Fast-forward stage background script to match the warp point
        if (actualFrame > 0) {
            THPortableFastForwardStageShell(actualFrame);
            // Sync GameManager.counat (frame counter used for subrank timing)
            th06::g_GameManager.counat = actualFrame;
        }
    }

    // Returns true if the current practice section should use boss BGM.
    bool THPracShouldPlayBossBGM()
    {
        return THBGMTest();
    }

    // Called AFTER Gui::RegisterChain to suppress stage title and song name
    // sprites when warping (they would otherwise play their intro animation).
    void THPracPostGuiInit()
    {
        if (thPracParam.mode != 1)
            return;

        // Reload correct effects/face ANM files for boss sections.
        // ECLNameFix() loads these during THPracPostEclLoad, but they get
        // overwritten by EffectManager::AddedCallback and Gui::AddedCallback
        // which run afterwards. Must reload here after all subsystems init.
        auto section = thPracParam.section;
        if (thPracParam.stage == 5 && section >= TH06_ST6_BOSS1 && section <= TH06_ST6_BOSS9) {
            THPortableReloadBossSectionAssets(PORTABLE_BOSS_ASSET_PROFILE_STAGE6_BOSS_EFFECTS);
        } else if (thPracParam.stage == 6 && section >= TH06_ST7_END_NS1 && section <= TH06_ST7_END_S10) {
            THPortableReloadBossSectionAssets(PORTABLE_BOSS_ASSET_PROFILE_STAGE7_END_EFFECTS);
        }

        // Check the actual ECL timeline — for Boss/Spell warps, thPracParam.frame
        // is 0, but EM_TimelineCurrent was set by ECLWarp inside THSectionPatch.
        int actualFrame = (int)EM_TimelineCurrent();
        if (actualFrame <= 0)
            return;

        // Hide "STAGE X" and song name intro animations
        THPortableSyncStageIntroSprites(true, true);
    }

    void THPracSpellAttempt() {}
    void THPracSpellCapture() {}
    void THPracSpellTimeout() {}
    void THPracSaveData() {}

}

float THPracGetSpeedMultiplier() { return TH06::g_speed_multiplier; }

// d3d9types.h not needed in SDL2 build (D3DSURFACE_DESC provided by d3d8_stub.h)
void TH06Init()
{
    TH06::THGuiCreate();
}

void TH06Reset()
{
    TH06::s_guiCreated = false;
    TH06::THPortableResetShellSyncTrackers();
}

}
