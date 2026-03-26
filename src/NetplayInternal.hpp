#pragma once

#include "NetplaySession.hpp"

#include "AsciiManager.hpp"
#include "BulletManager.hpp"
#include "Controller.hpp"
#include "EclManager.hpp"
#include "EffectManager.hpp"
#include "EnemyEclInstr.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "GameWindow.hpp"
#include "GamePaths.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "Rng.hpp"
#include "ScreenEffect.hpp"
#include "SoundPlayer.hpp"
#include "Stage.hpp"
#include "Supervisor.hpp"
#include "ZunColor.hpp"
#include "thprac_th06.h"

#include <SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace th06::Netplay
{
constexpr int kDefaultDelay = 2;
constexpr int kMaxDelay = 10;
constexpr int kKeyPackFrameCount = 15;
constexpr int kFrameCacheSize = 360;
constexpr int kRollbackSnapshotInterval = 15;
constexpr int kRollbackMaxSnapshots = 16;
constexpr int kRelayDefaultPort = 3478;
constexpr Uint64 kPeriodicPingMs = 1000;
constexpr Uint64 kReconnectPingMs = 100;
constexpr Uint64 kUiPhaseBroadcastMs = 2000;
constexpr Uint64 kDisconnectTimeoutMs = 5000;
constexpr Uint64 kPeerFreezeDisconnectMs = 30000;
constexpr Uint64 kRelayProbeIntervalMs = 1000;
constexpr Uint64 kRelayProbeTimeoutMs = 3000;
constexpr int kRelayMaxDatagramBytes = 1024;

enum Control
{
    Ctrl_No_Ctrl,
    Ctrl_Start_Game,
    Ctrl_Key,
    Ctrl_Set_InitSetting,
    Ctrl_Try_Resync,
    Ctrl_Debug_EndingJump,
    Ctrl_UiPhase
};

enum PackType
{
    PACK_NONE = 0,
    PACK_HELLO = 1,
    PACK_PING = 2,
    PACK_PONG = 3,
    PACK_USUAL = 4
};

enum InitSettingFlags
{
    InitSettingFlag_PredictionRollback = 1 << 0
};

enum UiPhaseFlags
{
    UiPhaseFlag_InUi = 1 << 0
};

template <int NBits> struct Bits
{
    unsigned char data[NBits / 8];

    void Clear()
    {
        std::memset(data, 0, sizeof(data));
    }
};

inline void ReadFromInt(Bits<16> &bits, u16 value)
{
    bits.data[0] = (unsigned char)(value & 0xff);
    bits.data[1] = (unsigned char)((value >> 8) & 0xff);
}

inline u16 WriteToInt(const Bits<16> &bits)
{
    return (u16)(bits.data[0]) | (u16)(bits.data[1] << 8);
}

#pragma pack(push, 1)
struct CtrlPack
{
    int frame = 0;
    Control ctrlType = Ctrl_No_Ctrl;
    union
    {
        Bits<16> keys[kKeyPackFrameCount];
        struct
        {
            int delay;
            int ver;
            int flags;
        } initSetting;
        struct
        {
            int frameToResync;
        } resyncSetting;
        struct
        {
            int serial;
            int flags;
            int boundaryFrame;
        } uiPhase;
    };
    InGameCtrlType inGameCtrl[kKeyPackFrameCount] {};
    u16 rngSeed[kKeyPackFrameCount] {};

    CtrlPack()
    {
        std::memset(keys, 0, sizeof(keys));
    }
};

struct Pack
{
    int type = PACK_NONE;
    unsigned int seq = 0;
    Uint64 sendTick = 0;
    Uint64 echoTick = 0;
    CtrlPack ctrl {};
};
#pragma pack(pop)

struct GameplaySnapshot
{
    struct SupervisorRuntimeState
    {
        i32 calcCount = 0;
        i32 wantedState = 0;
        i32 curState = 0;
        i32 wantedState2 = 0;
        i32 unk194 = 0;
        i32 unk198 = 0;
        ZunBool isInEnding = false;
        i32 vsyncEnabled = 0;
        i32 lastFrameTime = 0;
        f32 effectiveFramerateMultiplier = 1.0f;
        f32 framerateMultiplier = 1.0f;
        f32 unk1b4 = 0.0f;
        f32 unk1b8 = 0.0f;
        u32 startupTimeBeforeMenuMusic = 0;
    };

    struct GameWindowRuntimeState
    {
        i32 tickCountToEffectiveFramerate = 0;
        double lastFrameTime = 0.0;
        u8 curFrame = 0;
    };

    struct StageRuntimeState
    {
        std::vector<RawStageObjectInstance> objectInstances;
        std::vector<AnmVm> quadVms;
    };

    struct SoundRuntimeState
    {
        i32 soundBuffersToPlay[3] {};
        i32 queuedSfxState[128] {};
        i32 isLooping = 0;
    };

    int frame = 0;
    int stage = 0;
    int delay = 1;
    int currentDelayCooldown = 0;
    bool hasGuiImpl = false;

    GameManager gameManager;
    Player player1;
    Player player2;
    BulletManager bulletManager;
    EnemyManager enemyManager;
    ItemManager itemManager;
    EffectManager effectManager;
    Gui gui;
    GuiImpl guiImpl;
    AsciiManager asciiManager;
    Stage stageState;
    EclManager eclManager;
    Rng rng;
    EnemyEclInstr::RuntimeState enemyEclRuntimeState;
    ScreenEffect::RuntimeState screenEffectRuntimeState;
    Controller::RuntimeState controllerRuntimeState;
    SupervisorRuntimeState supervisorRuntimeState;
    GameWindowRuntimeState gameWindowRuntimeState;
    StageRuntimeState stageRuntimeState;
    SoundRuntimeState soundRuntimeState;

    u16 lastFrameInput = 0;
    u16 curFrameInput = 0;
    u16 eighthFrameHeldInput = 0;
    u16 heldInputFrames = 0;
};

struct DelayedPack
{
    Pack pack {};
    Uint64 releaseTick = 0;
};

static_assert(sizeof(Bits<16>) == 2, "Bits<16> layout mismatch");
static_assert(sizeof(CtrlPack) == 128, "CtrlPack layout mismatch");
static_assert(sizeof(Pack) == 152, "Pack layout mismatch");

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

inline int GetLastSocketError()
{
    return WSAGetLastError();
}

inline void CloseSocketHandle(SocketHandle socket)
{
    closesocket(socket);
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

inline int GetLastSocketError()
{
    return errno;
}

inline void CloseSocketHandle(SocketHandle socket)
{
    close(socket);
}
#endif

class SocketSystem
{
public:
    static bool Acquire()
    {
#ifdef _WIN32
        if (s_refCount == 0)
        {
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                return false;
            }
        }
#endif
        ++s_refCount;
        return true;
    }

    static void Release()
    {
        if (s_refCount <= 0)
        {
            return;
        }

        --s_refCount;
#ifdef _WIN32
        if (s_refCount == 0)
        {
            WSACleanup();
        }
#endif
    }

private:
    static inline int s_refCount = 0;
};

inline bool IsWouldBlockError(int errorCode)
{
#ifdef _WIN32
    return errorCode == WSAEWOULDBLOCK;
#else
    return errorCode == EWOULDBLOCK || errorCode == EAGAIN;
#endif
}

inline bool SetSocketNonBlocking(SocketHandle socket)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

inline bool SetDualStack(SocketHandle socket)
{
#ifdef IPV6_V6ONLY
    int off = 0;
    setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof(off));
#else
    (void)socket;
#endif
    return true;
}

class ConnectionBase
{
public:
    ConnectionBase() = default;
    virtual ~ConnectionBase()
    {
        Reset();
    }

    void Reset()
    {
        CloseSocket();
        m_family = AF_INET;
    }

protected:
    bool CreateSocket(int family)
    {
        CloseSocket();
        if (!SocketSystem::Acquire())
        {
            return false;
        }

        m_socket = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == kInvalidSocket)
        {
            SocketSystem::Release();
            return false;
        }

        m_family = family;
        if (family == AF_INET6)
        {
            SetDualStack(m_socket);
        }
        if (!SetSocketNonBlocking(m_socket))
        {
            CloseSocket();
            return false;
        }
        return true;
    }

    bool BindSocket(const std::string &bindIp, int port, int family);
    bool SendPackTo(const Pack &pack, const std::string &ip, int port);
    bool ReceiveOnePack(Pack &outPack, std::string &fromIp, int &fromPort, bool &hasData);

    void CloseSocket()
    {
        if (m_socket != kInvalidSocket)
        {
            CloseSocketHandle(m_socket);
            m_socket = kInvalidSocket;
            SocketSystem::Release();
        }
    }

private:
    static bool IpPortToSockAddr(const std::string &ip, int port, int family, sockaddr_storage &outStorage,
                                 socklen_t &outLen);
    static bool SockAddrToIpPort(const sockaddr *addr, socklen_t addrLen, std::string &outIp, int &outPort);

protected:
    SocketHandle m_socket = kInvalidSocket;
    int m_family = AF_INET;
};

class HostConnection final : public ConnectionBase
{
public:
    bool Start(const std::string &bindIp, int port, int family = AF_INET6);
    bool PollReceive(Pack &outPack, bool &hasData);
    bool SendPack(const Pack &pack);
    void Reconnect();
    bool HasGuest() const;
    const std::string &GetGuestIp() const;
    int GetGuestPort() const;

private:
    std::string m_hostIp;
    int m_hostPort = 0;
    std::string m_guestIp;
    int m_guestPort = 0;
    std::string m_lastBindIp;
    int m_lastBindPort = 0;
    int m_lastFamily = AF_INET6;
};

class GuestConnection final : public ConnectionBase
{
public:
    bool Start(const std::string &hostIp, int hostPort, int localPort, int family);
    bool PollReceive(Pack &outPack, bool &hasData);
    bool SendPack(const Pack &pack);
    void Reconnect();
    const std::string &GetHostIp() const;
    int GetHostPort() const;

private:
    std::string m_hostIp;
    int m_hostPort = 0;
    int m_localPort = 0;
    std::string m_lastHostIp;
    int m_lastHostPort = 0;
    int m_lastLocalPort = 0;
    int m_lastFamily = AF_INET;
};

class RelayConnection final : public ConnectionBase
{
public:
    bool Start(const std::string &serverEndpoint, const std::string &roomCode, bool isHostRole, int localPort,
               std::string *errorMessage);
    void Reset();
    void Tick();
    bool PollReceive(Pack &outPack, bool &hasData);
    bool SendPack(const Pack &pack);
    bool IsReady() const;

private:
    bool SendText(const std::string &text);
    void SendLeave();
    void SendRegister();
    void HandleControl(const std::string &text);

private:
    std::string m_serverHost;
    std::string m_serverEndpoint;
    std::string m_roomCode;
    int m_serverPort = 0;
    int m_localPort = 0;
    bool m_isHostRole = false;
    bool m_isReady = false;
    bool m_registrationRejected = false;
    std::string m_sessionId;
    Uint64 m_lastRegisterSendTick = 0;
    sockaddr_storage m_serverAddr {};
    socklen_t m_serverAddrLen = 0;
    bool m_serverAddrValid = false;
};

struct RuntimeState
{
    HostConnection host;
    GuestConnection guest;
    RelayConnection relay;

    bool isHost = false;
    bool isGuest = false;
    bool isConnected = false;
    bool isSessionActive = false;
    bool isSync = true;
    bool isTryingReconnect = false;
    bool reconnectIssued = false;
    bool launcherCloseRequested = false;
    bool versionMatched = true;
    bool hostIsPlayer1 = true;
    bool useRelayTransport = false;
    bool isWaitingForStartup = false;
    bool titleColdStartRequested = false;
    bool savedDefaultDifficultyValid = false;
    u8 savedDefaultDifficulty = NORMAL;
    bool pendingDebugEndingJump = false;

    int delay = kDefaultDelay;
    int currentDelayCooldown = 0;
    int resyncTargetFrame = 0;
    bool resyncTriggered = false;
    int lastFrame = -1;
    int lastConfirmedSyncFrame = 0;
    int pendingRollbackFrame = -1;
    int rollbackTargetFrame = 0;
    int rollbackSendFrame = -1;
    int rollbackStage = -1;
    int rollbackEpochStartFrame = 0;
    int lastRollbackMismatchFrame = -1;
    int lastRollbackSnapshotFrame = -1;
    int lastRollbackTargetFrame = -1;
    int lastRollbackSourceFrame = -1;
    int lastLatentRetrySourceFrame = -1;
    int seedValidationIgnoreUntilFrame = -1;
    int lastSeedRetryMismatchFrame = -1;
    int lastSeedRetryOlderThanSnapshotFrame = -1;
    u16 lastSeedRetryLocalSeed = 0;
    u16 lastSeedRetryRemoteSeed = 0;
    u16 sessionRngSeed = 0;
    bool sharedRngSeedCaptured = false;
    int sessionBaseCalcCount = 0;
    int currentNetFrame = 0;
    Uint64 lastPeriodicPingTick = 0;
    Uint64 lastRuntimeReceiveTick = 0;
    Uint64 guestWaitStartTick = 0;
    int lastRttMs = -1;
    unsigned int nextSeq = 1;
    InGameCtrlType currentCtrl = IGC_NONE;
    std::string statusText = "no connection";
    int lastPacketBytes = 0;
    std::string lastPacketFromIp;
    int lastPacketFromPort = 0;

    std::map<int, Bits<16>> localInputs;
    std::map<int, Bits<16>> remoteInputs;
    std::map<int, Bits<16>> predictedRemoteInputs;
    std::map<int, u16> localSeeds;
    std::map<int, u16> remoteSeeds;
    std::map<int, InGameCtrlType> localCtrls;
    std::map<int, InGameCtrlType> remoteCtrls;
    std::map<int, InGameCtrlType> predictedRemoteCtrls;
    std::set<int> remoteFramesPendingRollbackCheck;
    std::deque<GameplaySnapshot> rollbackSnapshots;
    bool predictionRollbackEnabled = true;
    bool rollbackActive = false;
    bool stallFrameRequested = false;
    bool hasKnownUiState = false;
    bool knownUiState = false;
    int localUiPhaseSerial = 0;
    int remoteUiPhaseSerial = 0;
    int pendingUiPhaseSerial = 0;
    bool remoteUiPhaseIsInUi = false;
    bool pendingUiPhaseIsInUi = false;
    bool awaitingUiPhaseAck = false;
    Uint64 lastUiPhaseSendTick = 0;
    int broadcastUiPhaseSerial = 0;
    bool broadcastUiPhaseIsInUi = false;
    Uint64 broadcastUiPhaseUntilTick = 0;
    Uint64 lastUiPhaseBroadcastTick = 0;
    DebugNetworkConfig debugNetworkConfig {};
    Uint32 debugRandomState = 0x6A09E667u;
    std::vector<DelayedPack> delayedOutgoingPacks;
    std::vector<DelayedPack> delayedIncomingPacks;
};

extern RuntimeState g_State;

struct RelayState
{
    SocketHandle socket = kInvalidSocket;
    int family = AF_UNSPEC;
    bool isConfigured = false;
    bool isConnecting = false;
    bool isReachable = false;
    int lastRttMs = -1;
    int port = kRelayDefaultPort;
    unsigned int nextNonce = 1;
    Uint64 lastProbeTick = 0;
    Uint64 lastProbeSendTick = 0;
    std::string endpointText;
    std::string host;
    std::string pendingNonce;
    std::string statusText = "not configured";
};

extern RelayState g_Relay;

void ClearRuntimeCaches();
void SetStatus(const std::string &text);
std::string BuildPacketSummary(const Pack &pack);
void TraceLauncherPacket(const char *phase, const Pack &pack);
void TraceDiagnostic(const char *event, const char *fmt, ...);
std::string TrimString(std::string text);
bool ResolveIpPortToSockAddr(const std::string &ip, int port, int family, sockaddr_storage &outStorage,
                             socklen_t &outLen);
bool TrySockAddrToIpPort(const sockaddr *addr, socklen_t addrLen, std::string &outIp, int &outPort);
bool ParseEndpointText(const std::string &endpoint, std::string &outHost, int &outPort);
bool OpenRelayProbeSocket();
void CloseRelayProbeSocket();
void SetRelayStatus(const std::string &text);
std::string GenerateRelaySessionId();
unsigned long GetDiagnosticProcessId();
bool SendRelayProbe();
void TickRelayProbe();
void ClearRemoteRuntimeCaches();
void ResetGameplayRuntimeStream();
int GetDisplayedRttMs();
void ForceDeterministicNetplayStep();
const char *ControlToString(Control control);
const char *InGameCtrlToString(InGameCtrlType ctrl);
std::string FormatInputBits(u16 input);
std::string BuildCtrlPacketWindowSummary(const CtrlPack &ctrl, int count);
bool SendPacketImmediate(const Pack &pack);
bool PollPacketImmediate(Pack &pack, bool &hasData);
void ClearDebugNetworkQueues();
bool IsDebugNetworkSimulationEnabled();
Uint32 NextDebugNetworkRandom();
int ComputeDebugNetworkDelayMs();
bool DebugNetworkRollPercent(int percent);
bool FlushDebugOutgoingPackets();
bool PopDueDebugIncomingPacket(Pack &pack);
void QueueDebugIncomingPacket(const Pack &pack);
void ApplyNetplayMenuDefaults();
void RestoreNetplayMenuDefaults();
bool UsingHostConnection();
bool SendPacket(const Pack &pack);
bool PollPacket(Pack &pack, bool &hasData);
bool IsCurrentUiFrame();
bool CanUseRollbackSnapshots();
bool ShouldCompleteRuntimeSessionForEnding();
void StartUiPhaseBroadcast(int serial, bool isInUi);
void DriveUiPhaseBroadcast(int frame);
void ResetLauncherState();
void CompleteRuntimeSessionForEnding();
void PruneOldFrameData(int frame);
InGameCtrlType CaptureControlKeys();
Pack MakePing(Control ctrlType);
int CurrentNetFrame();
int OldestRollbackSnapshotFrame();
int LatestRollbackSnapshotFrame();
bool IsRollbackFrameTooOld(int frame, int *outOldestSnapshotFrame = nullptr);
bool QueueRollbackFromFrame(int frame, const char *reason);
bool IsRollbackEpochFrame(int frame);
void ResetRollbackEpoch(int frame, const char *reason, int nextEpochStartFrame);
bool HasRollbackReplayHistory(int snapshotFrame, int rollbackTargetFrame, int *outRequiredStartFrame = nullptr,
                              int *outMissingLocalFrame = nullptr, int *outMissingRemoteFrame = nullptr);
void AdvanceConfirmedSyncFrame();
int LatestRemoteReceivedFrame();
Uint64 ComputePeerFreezeStallMs();
bool ShouldStallForPeerFreeze(int delayedFrame, Uint64 *outLastRecvAgo = nullptr, int *outLatestRemoteFrame = nullptr);
bool HasConsumedRemoteFrame(int frame);
bool ResolveRemoteFrameInput(int frame, u16 &outInput, InGameCtrlType &outCtrl, bool allowPrediction,
                             bool &outUsedPrediction);
bool FrameHasMenuBit(const std::map<int, Bits<16>> &inputMap, int frame);
bool TryStartQueuedRollback(int currentFrame);
void RefreshRollbackFrameState(int frame);
void CommitProcessedFrameState(int frame);
bool IsMenuTransitionFrame(int frame);
void CaptureGameplaySnapshot(int frame);
bool ResolveStoredFrameInput(int frame, bool isInUi, u16 &outInput, InGameCtrlType &outCtrl);
bool RestoreGameplaySnapshot(const GameplaySnapshot &snapshot);
bool TryStartAutomaticRollback(int currentFrame, int mismatchFrame, int olderThanSnapshotFrame = -1);
void TraceFrameSubsystemHashes(int frame, const char *phase);
void SendPeriodicPing();
void ActivateNetplaySession(u16 sharedRngSeed = 0, bool useSharedSeed = false);
void BeginSessionStartupWait();
void HandleStartGameHandshake();
void EnterConnectedState();
void HandleLauncherPacket(const Pack &pack);
void ProcessLauncherHost();
void ProcessLauncherGuest();
void StoreRemoteKeyPacket(const Pack &pack, u16 sharedRngSeed = 0, bool captureShared = false);
bool TryActivateFromStartupPacket(u16 *outPeerSharedSeed = nullptr);
bool ReceiveRuntimePackets();
void SendStartupBootstrapPacket();
void SendKeyPacket(int frame);
void SendUiPhasePacket(int serial, bool isInUi, int boundaryFrame);
void SendResyncPacket();
void HandleDesync(int frame);
bool TryBeginStartupWaitFromRuntimePacket(const Pack &pack);
bool TryActivateStartupFromRuntimePacket(const Pack &pack);
u16 ResolveFrameInput(int frame, bool isInUi, InGameCtrlType &outCtrl, bool &outRollbackStarted,
                      bool &outStallForPeer);
void ApplyDelayControl();
void TryReconnect(int frame);
void BeginUiPhaseBarrier(int serial, bool isInUi);
bool DriveUiPhaseBarrier(int frame);

class NetSession final : public ISession
{
public:
    SessionKind Kind() const override;
    void ResetInputState() override;
    void AdvanceFrameInput() override;
};

extern NetSession g_NetSession;

} // namespace th06::Netplay
