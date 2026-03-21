#pragma once

#include "Session.hpp"

#include <string>

namespace th06::Netplay
{
constexpr int kProtocolVersion = 3800;

enum InGameCtrlType
{
    Quick_Quit,
    Quick_Restart,
    Inf_Life,
    Inf_Bomb,
    Inf_Power,
    Add_Delay,
    Dec_Delay,
    IGC_NONE
};

struct Snapshot
{
    bool isHost = false;
    bool isGuest = false;
    bool isConnected = false;
    bool isSessionActive = false;
    bool isSync = true;
    bool isTryingReconnect = false;
    bool isVersionMatched = true;
    bool canStartGame = false;
    bool hostIsPlayer1 = true;
    bool delayLocked = false;
    int targetDelay = 1;
    int lastRttMs = -1;
    std::string statusText = "no connection";
};

struct RelaySnapshot
{
    bool isConfigured = false;
    bool isConnecting = false;
    bool isReachable = false;
    int lastRttMs = -1;
    std::string endpointText;
    std::string statusText = "not configured";
};

ISession &GetSession();
void Shutdown();

Snapshot GetSnapshot();
RelaySnapshot GetRelaySnapshot();
bool BeginHosting(int listenPort, const std::string &relayEndpoint, const std::string &roomCode,
                  std::string *errorMessage);
bool BeginGuest(const std::string &hostIp, int hostPort, int listenPort, const std::string &relayEndpoint,
                const std::string &roomCode, std::string *errorMessage);
bool BeginRelayProbe(const std::string &endpoint, std::string *errorMessage);
void ClearRelayProbe();
void CancelPendingConnection();
bool RequestStartGame();
void StartLocalSession();

void TickLauncher();
bool ConsumeLauncherCloseRequested();

bool AllowsReplay();
bool IsSessionActive();
bool IsWaitingForStartup();
bool IsConnected();
bool IsHost();
bool IsLocalPlayer1();
bool IsSync();
bool NeedsRollbackCatchup();
int GetDelay();
void SetDelay(int delay);
void SetHostPlayer1(bool hostIsPlayer1);
bool ConsumeTitleColdStartRequested();
void PrepareGameplayStart();

InGameCtrlType ConsumeInGameControl();
void DrawOverlay();
}; // namespace th06::Netplay
