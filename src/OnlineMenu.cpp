#include "OnlineMenu.hpp"

#include "NetplaySession.hpp"
#include "thprac_gui_locale.h"

#include <SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace th06::OnlineMenu
{
namespace
{
constexpr int kDefaultPort = 3036;
constexpr int kDefaultDelay = 2;
constexpr float kFrameTimeMs = 1000.0f / 60.0f;

struct State
{
    bool isOpen = false;
    bool closeRequested = false;
    bool configLoaded = false;
    bool dirty = false;

    int hostPort = kDefaultPort;
    int listenPort = kDefaultPort;
    int targetDelay = kDefaultDelay;
    char hostIp[128] = "::1";
    char relayServer[256] = "";
    char relayRoom[64] = "";
};

State g_State;

const char *Tr(const char *zh, const char *en, const char *ja)
{
    switch (THPrac::Gui::LocaleGet())
    {
    case THPrac::Gui::LOCALE_ZH_CN:
        return zh;
    case THPrac::Gui::LOCALE_JA_JP:
        return ja;
    case THPrac::Gui::LOCALE_EN_US:
    default:
        return en;
    }
}

std::string GetLauncherTitle()
{
    return std::string(Tr("联机启动器", "Game Launcher", "ネットワーク起動")) + " [ver=3.8.0]";
}

const char *GetHostIpLabel() { return Tr("主机 IP:", "Host IP:", "ホスト IP:"); }
const char *GetHostPortLabel() { return Tr("主机端口:", "Host Port:", "ホストポート:"); }
const char *GetListenPortLabel() { return Tr("监听端口:", "Listen Port:", "待受ポート:"); }
const char *GetRelayServerLabel() { return Tr("中转服务器ip:", "Relay Server:", "中継サーバー:"); }
const char *GetRelayConnectLabel() { return Tr("连接", "Connect", "接続"); }
const char *GetRelayStatusLabel() { return Tr("中转状态", "Relay", "中継状態"); }
const char *GetRelayRoomLabel() { return Tr("房间码:", "Room Code:", "ルームコード:"); }
const char *GetRelayTooltip()
{
    return Tr("可填写 ip:端口、域名，或 [IPv6]:端口。点击连接后会持续探测并显示延迟。",
              "Enter ip:port, a domain, or [IPv6]:port. After Connect, the launcher keeps probing and shows latency.",
              "ip:port、ドメイン、または [IPv6]:port を入力できます。接続後は継続的に疎通確認し、遅延を表示します。");
}
const char *GetCurStateLabel() { return Tr("当前状态:", "cur state:", "現在の状態:"); }
const char *GetTargetDelayLabel() { return Tr("目标延迟:", "target delay:", "目標遅延:"); }
const char *GetAutoDelayLabel() { return Tr("自动", "Auto", "自動"); }
const char *GetAutoDelayTooltip()
{
    return Tr("根据当前 RTT 自动估算一次目标延迟。算法使用“半 RTT + 1 帧安全裕量”，只会填入一次，不会持续改动。",
              "Estimate target delay once from the current RTT. Uses \"half RTT + one frame of safety\" and only fills the field once.",
              "現在の RTT から目標遅延を一度だけ自動推定します。\"RTT の半分 + 1 フレームの余裕\" を使い、継続的には変更しません。");
}
const char *GetRttLabel() { return "RTT"; }
const char *GetStartGameLabel() { return Tr("开始游戏", "Start Game", "ゲーム開始"); }
const char *GetStartGameLocalLabel() { return Tr("本地开始", "Start Game(local)", "ローカル開始"); }
const char *GetReturnTitleLabel() { return Tr("返回标题 (X)", "Return to title (X)", "タイトルに戻る (X)"); }
const char *GetVersionMismatchWarning() { return Tr("警告：host/guest 版本不一致", "warning: host/guest version mismatch", "警告: host/guest のバージョンが一致しません"); }

std::string LocalizeStatusText(const std::string &raw)
{
    if (raw == "no connection")
    {
        return Tr("未连接", "no connection", "未接続");
    }
    if (raw == "connected")
    {
        return Tr("已连接", "connected", "接続済み");
    }
    if (raw == "version mismatch")
    {
        return Tr("版本不一致", "version mismatch", "バージョン不一致");
    }
    if (raw.rfind("version mismatch", 0) == 0)
    {
        return std::string(Tr("版本不一致", "version mismatch", "バージョン不一致")) +
               raw.substr(std::strlen("version mismatch"));
    }
    if (raw == "disconnected")
    {
        return Tr("已断开", "disconnected", "切断");
    }
    if (raw == "try to reconnect...(sync)")
    {
        return Tr("正在重连...(同步)", "try to reconnect...(sync)", "再接続中...(同期)");
    }
    if (raw == "try to reconnect...(desynced)")
    {
        return Tr("正在重连...(已不同步)", "try to reconnect...(desynced)", "再接続中...(非同期)");
    }
    if (raw == "fail to start as host")
    {
        return Tr("host 启动失败", "fail to start as host", "ホスト起動失敗");
    }
    if (raw == "waiting guest...")
    {
        return Tr("等待 guest...", "waiting guest...", "ゲスト待機中...");
    }
    if (raw == "guest listen port conflicts with host")
    {
        return Tr("guest 监听端口与 host 冲突", "guest listen port conflicts with host", "ゲストの待受ポートがホストと競合しています");
    }
    if (raw == "fail to start as guest")
    {
        return Tr("guest 启动失败", "fail to start as guest", "ゲスト起動失敗");
    }
    if (raw == "trying connection...")
    {
        return Tr("正在连接...", "trying connection...", "接続中...");
    }
    if (raw == "starting game...")
    {
        return Tr("正在开始游戏...", "starting game...", "ゲーム開始中...");
    }
    if (raw == "waiting relay guest...")
    {
        return Tr("等待中转 guest...", "waiting relay guest...", "中継 guest 待機中...");
    }
    if (raw == "waiting relay host...")
    {
        return Tr("等待中转 host...", "waiting relay host...", "中継 host 待機中...");
    }
    if (raw == "registering relay guest...")
    {
        return Tr("正在注册中转 guest...", "registering relay guest...", "中継 guest 登録中...");
    }
    if (raw == "relay register failed")
    {
        return Tr("中转注册失败", "relay register failed", "中継登録失敗");
    }
    if (raw == "relay room occupied")
    {
        return Tr("房间被占用", "room occupied", "ルーム使用中");
    }
    if (raw == "relay endpoint/room required")
    {
        return Tr("中转地址和房间码必须同时填写", "relay endpoint and room code are both required",
                  "中継アドレスとルームコードは両方必要です");
    }
    return raw;
}

std::string LocalizeRelayStatusText(const std::string &raw)
{
    if (raw == "not configured")
    {
        return Tr("未配置", "not configured", "未設定");
    }
    if (raw == "invalid relay endpoint")
    {
        return Tr("地址格式无效", "invalid endpoint", "アドレス形式が無効です");
    }
    if (raw == "relay socket init failed")
    {
        return Tr("中转探测 socket 初始化失败", "relay socket init failed", "中継ソケット初期化失敗");
    }
    if (raw == "resolve failed")
    {
        return Tr("域名解析失败", "resolve failed", "名前解決失敗");
    }
    if (raw == "probe send failed")
    {
        return Tr("探测包发送失败", "probe send failed", "疎通確認パケット送信失敗");
    }
    if (raw == "probing...")
    {
        return Tr("正在探测...", "probing...", "疎通確認中...");
    }
    if (raw == "reachable")
    {
        return Tr("可达", "reachable", "到達可能");
    }
    if (raw == "probe timeout")
    {
        return Tr("探测超时", "probe timeout", "疎通確認タイムアウト");
    }
    if (raw == "probe socket error")
    {
        return Tr("中转探测 socket 错误", "probe socket error", "中継ソケットエラー");
    }
    return raw;
}

std::string Trim(std::string value)
{
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };

    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
    {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
    {
        value.pop_back();
    }
    return value;
}

const char *GetConfigPath()
{
    static std::string path;
    static bool initialized = false;
    if (!initialized)
    {
        initialized = true;
        if (char *prefPath = SDL_GetPrefPath("th06-sdl2", "netplay"))
        {
            path = prefPath;
            SDL_free(prefPath);
            if (!path.empty())
            {
                const char tail = path.back();
                if (tail != '/' && tail != '\\')
                {
                    path.push_back('/');
                }
            }
            path += "connect_config.ini";
        }
        else
        {
            path = "connect_config.ini";
        }
    }
    return path.c_str();
}

void ClampState()
{
    g_State.hostPort = std::clamp(g_State.hostPort, 0, 65535);
    g_State.listenPort = std::clamp(g_State.listenPort, 0, 65535);
    g_State.targetDelay = std::clamp(g_State.targetDelay, 1, 60);
}

int EstimateDelayFromRttMs(int rttMs)
{
    if (rttMs < 0)
    {
        return kDefaultDelay;
    }

    const float oneWayMs = static_cast<float>(rttMs) * 0.5f;
    const int estimated = static_cast<int>(std::ceil(oneWayMs / kFrameTimeMs)) + 1;
    return std::clamp(estimated, 1, 10);
}

void LoadConfig()
{
    if (g_State.configLoaded)
    {
        return;
    }

    g_State.configLoaded = true;

    std::ifstream file(GetConfigPath());
    if (!file)
    {
        ClampState();
        Netplay::SetDelay(g_State.targetDelay);
        return;
    }

    bool inConnectionSection = false;
    std::string line;
    while (std::getline(file, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
        {
            continue;
        }

        if (line.front() == '[' && line.back() == ']')
        {
            inConnectionSection = Trim(line.substr(1, line.size() - 2)) == "Connection";
            continue;
        }

        if (!inConnectionSection)
        {
            continue;
        }

        const std::size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos)
        {
            continue;
        }

        const std::string key = Trim(line.substr(0, equalsPos));
        const std::string value = Trim(line.substr(equalsPos + 1));

        if (key == "ip")
        {
            std::snprintf(g_State.hostIp, sizeof(g_State.hostIp), "%s", value.c_str());
        }
        else if (key == "port_host")
        {
            g_State.hostPort = std::atoi(value.c_str());
        }
        else if (key == "port_listen")
        {
            g_State.listenPort = std::atoi(value.c_str());
        }
        else if (key == "target_delay")
        {
            g_State.targetDelay = std::atoi(value.c_str());
        }
        else if (key == "relay_server")
        {
            std::snprintf(g_State.relayServer, sizeof(g_State.relayServer), "%s", value.c_str());
        }
        else if (key == "relay_room")
        {
            std::snprintf(g_State.relayRoom, sizeof(g_State.relayRoom), "%s", value.c_str());
        }
    }

    ClampState();
    Netplay::SetDelay(g_State.targetDelay);
}

void SaveConfig()
{
    if (!g_State.configLoaded)
    {
        return;
    }

    ClampState();

    std::ofstream file(GetConfigPath(), std::ios::trunc);
    if (!file)
    {
        return;
    }

    file << "[Connection]\n";
    file << "ip=" << g_State.hostIp << '\n';
    file << "port_host=" << g_State.hostPort << '\n';
    file << "port_listen=" << g_State.listenPort << '\n';
    file << "target_delay=" << g_State.targetDelay << '\n';
    file << "relay_server=" << g_State.relayServer << '\n';
    file << "relay_room=" << g_State.relayRoom << '\n';
}

const char *GetHostButtonLabel(const Netplay::Snapshot &snapshot)
{
    if (snapshot.isHost && snapshot.isConnected)
    {
        return Tr("已连接", "connected", "接続済み");
    }
    if (snapshot.isHost)
    {
        return Tr("等待 guest", "waiting guest", "guest 待機");
    }
    return Tr("作为 host", "as host", "host として");
}

const char *GetGuestButtonLabel(const Netplay::Snapshot &snapshot)
{
    if (snapshot.isGuest && snapshot.isConnected)
    {
        return Tr("已连接", "connected", "接続済み");
    }
    if (snapshot.isGuest)
    {
        return Tr("等待消息...", "waiting msg...", "メッセージ待機...");
    }
    return Tr("作为 guest", "as guest", "guest として");
}
} // namespace

void Open()
{
    LoadConfig();
    g_State.isOpen = true;
    g_State.closeRequested = false;
}

void Close()
{
    SaveConfig();
    Netplay::ClearRelayProbe();
    g_State.isOpen = false;
    g_State.closeRequested = false;
    if (!Netplay::IsSessionActive() && !Netplay::IsWaitingForStartup())
    {
        Netplay::CancelPendingConnection();
    }
}

void Reset()
{
    g_State = State {};
}

bool IsOpen()
{
    return g_State.isOpen;
}

bool ShouldForceRunInBackground()
{
    const SessionKind kind = Session::GetKind();
    return g_State.isOpen || Netplay::IsWaitingForStartup() || Netplay::IsSessionActive() ||
           kind == SessionKind::LocalNetplay || kind == SessionKind::Netplay;
}

bool ConsumeCloseRequested()
{
    if (!g_State.closeRequested)
    {
        return false;
    }

    Close();
    return true;
}

bool AllowsBackShortcut()
{
    if (!ImGui::GetCurrentContext())
    {
        return true;
    }

    const ImGuiIO &io = ImGui::GetIO();
    return !io.WantTextInput && !ImGui::IsAnyItemActive();
}

void UpdateImGui()
{
    if (!g_State.isOpen || !ImGui::GetCurrentContext())
    {
        return;
    }

    ClampState();
    Netplay::TickLauncher();
    if (Netplay::ConsumeLauncherCloseRequested())
    {
        g_State.closeRequested = true;
    }

    const Netplay::Snapshot snapshot = Netplay::GetSnapshot();
    const Netplay::RelaySnapshot relaySnapshot = Netplay::GetRelaySnapshot();
    g_State.targetDelay = snapshot.targetDelay;
    const std::string launcherTitle = GetLauncherTitle();
    const std::string localizedStatusText = LocalizeStatusText(snapshot.statusText);
    const std::string localizedRelayStatusText = LocalizeRelayStatusText(relaySnapshot.statusText);

    ImGui::SetNextWindowSize(ImVec2(430.0f, 458.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(320.0f, 240.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool keepOpen = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin(launcherTitle.c_str(), &keepOpen, flags))
    {
        ImGui::End();
        if (!keepOpen)
        {
            g_State.closeRequested = true;
        }
        return;
    }

    ImGui::TextUnformatted(GetHostIpLabel());
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(270.0f);
    if (ImGui::InputText("##online_host_ip", g_State.hostIp, sizeof(g_State.hostIp)))
    {
        g_State.dirty = true;
    }

    ImGui::TextUnformatted(GetHostPortLabel());
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputInt("##online_host_port", &g_State.hostPort, 0, 0))
    {
        g_State.dirty = true;
    }

    ImGui::TextUnformatted(GetListenPortLabel());
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputInt("##online_listen_port", &g_State.listenPort, 0, 0))
    {
        g_State.dirty = true;
    }

    ImGui::TextUnformatted(GetRelayServerLabel());
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", GetRelayTooltip());
    }
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(178.0f);
    if (ImGui::InputText("##online_relay_server", g_State.relayServer, sizeof(g_State.relayServer)))
    {
        g_State.dirty = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", GetRelayTooltip());
    }
    ImGui::SameLine();
    if (ImGui::Button(GetRelayConnectLabel(), ImVec2(86.0f, 0.0f)))
    {
        std::string error;
        if (!Netplay::BeginRelayProbe(g_State.relayServer, &error))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Relay probe start failed: %s", error.c_str());
        }
    }
    ImGui::SetCursorPosX(110.0f);
    if (relaySnapshot.lastRttMs >= 0)
    {
        ImGui::TextWrapped("%s: %s | %s: %d ms", GetRelayStatusLabel(), localizedRelayStatusText.c_str(), GetRttLabel(),
                           relaySnapshot.lastRttMs);
    }
    else
    {
        ImGui::TextWrapped("%s: %s | %s: --", GetRelayStatusLabel(), localizedRelayStatusText.c_str(), GetRttLabel());
    }

    ImGui::TextUnformatted(GetRelayRoomLabel());
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(178.0f);
    if (ImGui::InputText("##online_relay_room", g_State.relayRoom, sizeof(g_State.relayRoom)))
    {
        g_State.dirty = true;
    }

    ClampState();

    if (ImGui::Button(GetHostButtonLabel(snapshot), ImVec2(160.0f, 32.0f)))
    {
        std::string error;
        if (!Netplay::BeginHosting(g_State.listenPort, g_State.relayServer, g_State.relayRoom, &error))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Netplay host start failed: %s", error.c_str());
        }
    }
    ImGui::SameLine(220.0f);
    if (ImGui::Button(GetGuestButtonLabel(snapshot), ImVec2(160.0f, 32.0f)))
    {
        std::string error;
        if (!Netplay::BeginGuest(g_State.hostIp, g_State.hostPort, g_State.listenPort, g_State.relayServer,
                                 g_State.relayRoom, &error))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Netplay guest start failed: %s", error.c_str());
        }
    }

    ImGui::TextUnformatted(GetCurStateLabel());
    ImGui::SameLine(110.0f);
    ImGui::BeginChildFrame(ImGui::GetID("online_cur_state"), ImVec2(270.0f, 30.0f), ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(localizedStatusText.c_str());
    ImGui::EndChildFrame();

    ImGui::TextUnformatted(GetTargetDelayLabel());
    ImGui::SameLine(110.0f);
    if (snapshot.delayLocked)
    {
        ImGui::BeginDisabled();
    }
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputInt("##online_target_delay", &g_State.targetDelay, 0, 0))
    {
        ClampState();
        Netplay::SetDelay(g_State.targetDelay);
        g_State.dirty = true;
    }
    ImGui::SameLine();
    const int availableRttMs = snapshot.lastRttMs;
    if (availableRttMs < 0)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(GetAutoDelayLabel(), ImVec2(72.0f, 0.0f)))
    {
        g_State.targetDelay = EstimateDelayFromRttMs(availableRttMs);
        Netplay::SetDelay(g_State.targetDelay);
        g_State.dirty = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", GetAutoDelayTooltip());
    }
    if (availableRttMs < 0)
    {
        ImGui::EndDisabled();
    }
    if (snapshot.delayLocked)
    {
        ImGui::EndDisabled();
    }

    if (snapshot.lastRttMs >= 0)
    {
        ImGui::Text("%s: %d ms", GetRttLabel(), snapshot.lastRttMs);
    }
    else
    {
        ImGui::Text("%s: --", GetRttLabel());
    }

    if (!snapshot.canStartGame)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(GetStartGameLabel(), ImVec2(160.0f, 32.0f)))
    {
        Netplay::RequestStartGame();
    }
    if (!snapshot.canStartGame)
    {
        ImGui::EndDisabled();
    }

    ImGui::SameLine(220.0f);
    if (ImGui::Button(GetStartGameLocalLabel(), ImVec2(160.0f, 32.0f)))
    {
        Netplay::StartLocalSession();
    }

    if (ImGui::Button(GetReturnTitleLabel(), ImVec2(160.0f, 28.0f)))
    {
        g_State.closeRequested = true;
    }

    if (!snapshot.isVersionMatched)
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", GetVersionMismatchWarning());
    }

    ImGui::End();

    if (!keepOpen)
    {
        g_State.closeRequested = true;
    }

    if (g_State.dirty)
    {
        SaveConfig();
        g_State.dirty = false;
    }
}
} // namespace th06::OnlineMenu
