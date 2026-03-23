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
namespace
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
constexpr Uint64 kDisconnectTimeoutMs = 5000;
constexpr Uint64 kRelayProbeIntervalMs = 1000;
constexpr Uint64 kRelayProbeTimeoutMs = 3000;
constexpr int kRelayMaxDatagramBytes = 1024;

enum Control
{
    Ctrl_No_Ctrl,
    Ctrl_Start_Game,
    Ctrl_Key,
    Ctrl_Set_InitSetting,
    Ctrl_Try_Resync
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

struct RollbackSnapshot
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

static_assert(sizeof(Bits<16>) == 2, "Bits<16> layout mismatch");
static_assert(sizeof(CtrlPack) == 128, "CtrlPack layout mismatch");
static_assert(sizeof(Pack) == 152, "Pack layout mismatch");

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

int GetLastSocketError()
{
    return WSAGetLastError();
}

void CloseSocketHandle(SocketHandle socket)
{
    closesocket(socket);
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

int GetLastSocketError()
{
    return errno;
}

void CloseSocketHandle(SocketHandle socket)
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
    static int s_refCount;
};

int SocketSystem::s_refCount = 0;

bool IsWouldBlockError(int errorCode)
{
#ifdef _WIN32
    return errorCode == WSAEWOULDBLOCK;
#else
    return errorCode == EWOULDBLOCK || errorCode == EAGAIN;
#endif
}

bool SetSocketNonBlocking(SocketHandle socket)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool SetDualStack(SocketHandle socket)
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
    int seedValidationIgnoreUntilFrame = -1;
    int lastSeedRetryMismatchFrame = -1;
    int lastSeedRetryOlderThanSnapshotFrame = -1;
    u16 lastSeedRetryLocalSeed = 0;
    u16 lastSeedRetryRemoteSeed = 0;
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
    std::deque<RollbackSnapshot> rollbackSnapshots;
    bool predictionRollbackEnabled = true;
    bool rollbackActive = false;
    bool stallFrameRequested = false;
    bool hasKnownUiState = false;
    bool knownUiState = false;
};

RuntimeState g_State;

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

RelayState g_Relay;

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
bool SendRelayProbe();
void TickRelayProbe();
void ApplyNetplayMenuDefaults();
void RestoreNetplayMenuDefaults();
bool UsingHostConnection();
bool SendPacket(const Pack &pack);
bool PollPacket(Pack &pack, bool &hasData);
bool IsCurrentUiFrame();
bool CanUseRollbackSnapshots();
void ResetLauncherState();
void PruneOldFrameData(int frame);
InGameCtrlType CaptureControlKeys();
Pack MakePing(Control ctrlType);
int CurrentNetFrame();
int OldestRollbackSnapshotFrame();
bool IsRollbackFrameTooOld(int frame, int *outOldestSnapshotFrame = nullptr);
bool QueueRollbackFromFrame(int frame, const char *reason);
bool IsRollbackEpochFrame(int frame);
void ResetRollbackEpoch(int frame, const char *reason, int nextEpochStartFrame);
bool HasRollbackReplayHistory(int snapshotFrame, int rollbackTargetFrame, int *outRequiredStartFrame = nullptr,
                              int *outMissingLocalFrame = nullptr, int *outMissingRemoteFrame = nullptr);
void AdvanceConfirmedSyncFrame();
bool HasConsumedRemoteFrame(int frame);
bool ResolveRemoteFrameInput(int frame, u16 &outInput, InGameCtrlType &outCtrl, bool allowPrediction,
                             bool &outUsedPrediction);
bool FrameHasMenuBit(const std::map<int, Bits<16>> &inputMap, int frame);
bool TryStartQueuedRollback(int currentFrame);
void RefreshRollbackFrameState(int frame);
bool IsMenuTransitionFrame(int frame);
void CaptureRollbackSnapshot(int frame);
bool ResolveStoredFrameInput(int frame, bool isInUi, u16 &outInput, InGameCtrlType &outCtrl);
bool RestoreRollbackSnapshot(const RollbackSnapshot &snapshot);
bool TryStartAutomaticRollback(int currentFrame, int mismatchFrame, int olderThanSnapshotFrame = -1);
void SendPeriodicPing();
void ActivateNetplaySession();
void BeginSessionStartupWait();
void HandleStartGameHandshake();
void EnterConnectedState();
void HandleLauncherPacket(const Pack &pack);
void ProcessLauncherHost();
void ProcessLauncherGuest();
void StoreRemoteKeyPacket(const Pack &pack);
bool TryActivateFromStartupPacket();
bool ReceiveRuntimePackets();
void SendStartupBootstrapPacket();
void SendKeyPacket(int frame);
void SendResyncPacket();
void HandleDesync(int frame);
bool TryBeginStartupWaitFromRuntimePacket(const Pack &pack);
u16 ResolveFrameInput(int frame, bool isInUi, InGameCtrlType &outCtrl, bool &outRollbackStarted);
void ApplyDelayControl();
void TryReconnect(int frame);

class NetSession final : public ISession
{
public:
    SessionKind Kind() const override;
    void ResetInputState() override;
    void AdvanceFrameInput() override;
};

NetSession g_NetSession;
} // namespace

namespace
{

std::string TrimString(std::string text)
{
    const auto isSpace = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };

    while (!text.empty() && isSpace((unsigned char)text.front()))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && isSpace((unsigned char)text.back()))
    {
        text.pop_back();
    }
    return text;
}

bool ResolveIpPortToSockAddr(const std::string &ip, int port, int family, sockaddr_storage &outStorage, socklen_t &outLen)
{
    std::memset(&outStorage, 0, sizeof(outStorage));
    if (family == AF_INET)
    {
        sockaddr_in *addr4 = (sockaddr_in *)&outStorage;
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons((u16)port);
        if (ip.empty())
        {
            addr4->sin_addr.s_addr = htonl(INADDR_ANY);
            outLen = sizeof(sockaddr_in);
            return true;
        }
        if (inet_pton(AF_INET, ip.c_str(), &addr4->sin_addr) == 1)
        {
            outLen = sizeof(sockaddr_in);
            return true;
        }
    }
    else if (family == AF_INET6)
    {
        sockaddr_in6 *addr6 = (sockaddr_in6 *)&outStorage;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons((u16)port);
        if (ip.empty())
        {
            addr6->sin6_addr = in6addr_any;
            outLen = sizeof(sockaddr_in6);
            return true;
        }
        if (inet_pton(AF_INET6, ip.c_str(), &addr6->sin6_addr) == 1)
        {
            outLen = sizeof(sockaddr_in6);
            return true;
        }
    }
    else
    {
        return false;
    }

    if (ip.empty())
    {
        return false;
    }

    addrinfo hints {};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
#ifdef AI_V4MAPPED
    if (family == AF_INET6)
    {
        hints.ai_flags |= AI_V4MAPPED;
    }
#endif
    char portText[16] = {};
    std::snprintf(portText, sizeof(portText), "%d", port);

    addrinfo *result = nullptr;
    if (getaddrinfo(ip.c_str(), portText, &hints, &result) != 0 || result == nullptr)
    {
        return false;
    }

    const bool ok = result->ai_addrlen <= sizeof(outStorage);
    if (ok)
    {
        std::memcpy(&outStorage, result->ai_addr, result->ai_addrlen);
        outLen = (socklen_t)result->ai_addrlen;
    }
    freeaddrinfo(result);
    return ok;
}

bool TrySockAddrToIpPort(const sockaddr *addr, socklen_t addrLen, std::string &outIp, int &outPort)
{
    char buffer[128] = {};
    if (addr->sa_family == AF_INET)
    {
        const sockaddr_in *addr4 = (const sockaddr_in *)addr;
        if (inet_ntop(AF_INET, &addr4->sin_addr, buffer, sizeof(buffer)) == nullptr)
        {
            return false;
        }
        outIp = buffer;
        outPort = ntohs(addr4->sin_port);
        return true;
    }
    if (addr->sa_family == AF_INET6)
    {
        const sockaddr_in6 *addr6 = (const sockaddr_in6 *)addr;
        if (inet_ntop(AF_INET6, &addr6->sin6_addr, buffer, sizeof(buffer)) == nullptr)
        {
            return false;
        }
        outIp = buffer;
        outPort = ntohs(addr6->sin6_port);
        return true;
    }
    (void)addrLen;
    return false;
}

bool ParseEndpointText(const std::string &endpoint, std::string &outHost, int &outPort)
{
    const std::string trimmed = TrimString(endpoint);
    if (trimmed.empty())
    {
        return false;
    }

    std::string host;
    int port = kRelayDefaultPort;

    if (trimmed.front() == '[')
    {
        const std::size_t closePos = trimmed.find(']');
        if (closePos == std::string::npos)
        {
            return false;
        }

        host = trimmed.substr(1, closePos - 1);
        if (closePos + 1 < trimmed.size())
        {
            if (trimmed[closePos + 1] != ':')
            {
                return false;
            }
            port = std::atoi(trimmed.c_str() + closePos + 2);
        }
    }
    else
    {
        const std::size_t firstColon = trimmed.find(':');
        const std::size_t lastColon = trimmed.rfind(':');
        if (firstColon == std::string::npos)
        {
            host = trimmed;
        }
        else if (firstColon == lastColon)
        {
            host = TrimString(trimmed.substr(0, firstColon));
            port = std::atoi(trimmed.c_str() + firstColon + 1);
        }
        else
        {
            host = trimmed;
        }
    }

    host = TrimString(host);
    if (host.empty() || port <= 0 || port > 65535)
    {
        return false;
    }

    outHost = host;
    outPort = port;
    return true;
}

bool ConnectionBase::BindSocket(const std::string &bindIp, int port, int family)
{
    sockaddr_storage storage {};
    socklen_t storageLen = 0;
    if (!IpPortToSockAddr(bindIp, port, family, storage, storageLen))
    {
        return false;
    }
    return bind(m_socket, (sockaddr *)&storage, storageLen) == 0;
}

bool ConnectionBase::SendPackTo(const Pack &pack, const std::string &ip, int port)
{
    const int families[2] = {m_family == AF_INET6 ? AF_INET6 : AF_INET, m_family == AF_INET6 ? AF_INET : AF_INET6};
    for (const int family : families)
    {
        sockaddr_storage storage {};
        socklen_t storageLen = 0;
        if (!IpPortToSockAddr(ip, port, family, storage, storageLen))
        {
            continue;
        }

        const int sent = (int)sendto(m_socket, (const char *)&pack, sizeof(pack), 0, (sockaddr *)&storage, storageLen);
        if (sent == sizeof(pack))
        {
            return true;
        }
    }
    return false;
}

bool ConnectionBase::ReceiveOnePack(Pack &outPack, std::string &fromIp, int &fromPort, bool &hasData)
{
    hasData = false;
    fromIp.clear();
    fromPort = 0;
    if (m_socket == kInvalidSocket)
    {
        return false;
    }

    sockaddr_storage fromAddr {};
    socklen_t fromLen = sizeof(fromAddr);
    std::memset(&outPack, 0, sizeof(outPack));
    const int received = (int)recvfrom(m_socket, (char *)&outPack, sizeof(outPack), 0, (sockaddr *)&fromAddr, &fromLen);
    if (received < 0)
    {
        const int errorCode = GetLastSocketError();
        if (IsWouldBlockError(errorCode))
        {
            return true;
        }
        return false;
    }

    g_State.lastPacketBytes = received;
    hasData = SockAddrToIpPort((const sockaddr *)&fromAddr, fromLen, fromIp, fromPort);
    if (hasData)
    {
        g_State.lastPacketFromIp = fromIp;
        g_State.lastPacketFromPort = fromPort;
    }
    return hasData;
}

bool ConnectionBase::IpPortToSockAddr(const std::string &ip, int port, int family, sockaddr_storage &outStorage,
                                      socklen_t &outLen)
{
    return ResolveIpPortToSockAddr(ip, port, family, outStorage, outLen);
}

bool ConnectionBase::SockAddrToIpPort(const sockaddr *addr, socklen_t addrLen, std::string &outIp, int &outPort)
{
    return TrySockAddrToIpPort(addr, addrLen, outIp, outPort);
}

bool HostConnection::Start(const std::string &bindIp, int port, int family)
{
    Reset();
    if (!CreateSocket(family) || !BindSocket(bindIp, port, family))
    {
        Reset();
        return false;
    }
    m_hostIp = bindIp;
    m_hostPort = port;
    m_lastBindIp = bindIp;
    m_lastBindPort = port;
    m_lastFamily = family;
    return true;
}

bool HostConnection::PollReceive(Pack &outPack, bool &hasData)
{
    std::string fromIp;
    int fromPort = 0;
    if (!ReceiveOnePack(outPack, fromIp, fromPort, hasData))
    {
        return false;
    }
    if (hasData)
    {
        m_guestIp = fromIp;
        m_guestPort = fromPort;
    }
    return true;
}

bool HostConnection::SendPack(const Pack &pack)
{
    return !m_guestIp.empty() && m_guestPort > 0 && SendPackTo(pack, m_guestIp, m_guestPort);
}

void HostConnection::Reconnect()
{
    Start(m_lastBindIp, m_lastBindPort, m_lastFamily);
}

bool HostConnection::HasGuest() const
{
    return !m_guestIp.empty() && m_guestPort > 0;
}

const std::string &HostConnection::GetGuestIp() const
{
    return m_guestIp;
}

int HostConnection::GetGuestPort() const
{
    return m_guestPort;
}

bool GuestConnection::Start(const std::string &hostIp, int hostPort, int localPort, int family)
{
    Reset();
    if (!CreateSocket(family) || !BindSocket("", localPort, family))
    {
        Reset();
        return false;
    }
    m_hostIp = hostIp;
    m_hostPort = hostPort;
    m_localPort = localPort;
    m_lastHostIp = hostIp;
    m_lastHostPort = hostPort;
    m_lastLocalPort = localPort;
    m_lastFamily = family;
    return true;
}

bool GuestConnection::PollReceive(Pack &outPack, bool &hasData)
{
    std::string fromIp;
    int fromPort = 0;
    if (!ReceiveOnePack(outPack, fromIp, fromPort, hasData))
    {
        return false;
    }
    return true;
}

bool GuestConnection::SendPack(const Pack &pack)
{
    return !m_hostIp.empty() && m_hostPort > 0 && SendPackTo(pack, m_hostIp, m_hostPort);
}

void GuestConnection::Reconnect()
{
    Start(m_lastHostIp, m_lastHostPort, m_lastLocalPort, m_lastFamily);
}

const std::string &GuestConnection::GetHostIp() const
{
    return m_hostIp;
}

int GuestConnection::GetHostPort() const
{
    return m_hostPort;
}

bool RelayConnection::Start(const std::string &serverEndpoint, const std::string &roomCode, bool isHostRole, int localPort,
                            std::string *errorMessage)
{
    Reset();

    std::string host;
    int port = 0;
    if (!ParseEndpointText(serverEndpoint, host, port))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "invalid relay endpoint";
        }
        return false;
    }

    const std::string trimmedRoom = TrimString(roomCode);
    if (trimmedRoom.empty() || trimmedRoom.find_first_of(" \t\r\n") != std::string::npos)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "invalid relay room code";
        }
        return false;
    }

    const auto tryStart = [&](int family) -> bool {
        if (!CreateSocket(family) || !BindSocket("", localPort, family))
        {
            Reset();
            return false;
        }
        sockaddr_storage serverAddr {};
        socklen_t serverAddrLen = 0;
        if (!ResolveIpPortToSockAddr(host, port, family, serverAddr, serverAddrLen))
        {
            Reset();
            return false;
        }
        m_serverHost = host;
        m_serverEndpoint = TrimString(serverEndpoint);
        m_roomCode = trimmedRoom;
        m_serverPort = port;
        m_localPort = localPort;
        m_isHostRole = isHostRole;
        m_isReady = false;
        m_registrationRejected = false;
        if (m_sessionId.empty())
        {
            m_sessionId = GenerateRelaySessionId();
        }
        m_lastRegisterSendTick = 0;
        m_serverAddr = serverAddr;
        m_serverAddrLen = serverAddrLen;
        m_serverAddrValid = true;
        return true;
    };

    if (!tryStart(AF_INET6) && !tryStart(AF_INET))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to start relay socket";
        }
        return false;
    }

    SendRegister();
    return true;
}

void RelayConnection::Reset()
{
    SendLeave();
    ConnectionBase::Reset();
    m_serverHost.clear();
    m_serverEndpoint.clear();
    m_roomCode.clear();
    m_serverPort = 0;
    m_localPort = 0;
    m_isHostRole = false;
    m_isReady = false;
    m_registrationRejected = false;
    m_lastRegisterSendTick = 0;
    m_serverAddr = {};
    m_serverAddrLen = 0;
    m_serverAddrValid = false;
}

bool RelayConnection::SendText(const std::string &text)
{
    if (m_socket == kInvalidSocket || !m_serverAddrValid)
    {
        return false;
    }

    const int sent =
        (int)sendto(m_socket, text.data(), (int)text.size(), 0, (sockaddr *)&m_serverAddr, m_serverAddrLen);
    return sent == (int)text.size();
}

void RelayConnection::SendLeave()
{
    if (m_socket == kInvalidSocket || m_roomCode.empty())
    {
        return;
    }

    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "THR1 LEAVE %s", m_roomCode.c_str());
    SendText(buffer);
}

void RelayConnection::SendRegister()
{
    if (m_roomCode.empty() || m_registrationRejected)
    {
        return;
    }

    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "THR1 REGISTER %s %s %d %s", m_roomCode.c_str(),
                  m_isHostRole ? "host" : "guest", kProtocolVersion, m_sessionId.c_str());
    if (SendText(buffer))
    {
        m_lastRegisterSendTick = SDL_GetTicks64();
        SetStatus(m_isHostRole ? "waiting relay guest..." : "registering relay guest...");
    }
    else
    {
        SetStatus("relay register failed");
    }
}

void RelayConnection::HandleControl(const std::string &text)
{
    std::istringstream parser(text);
    std::vector<std::string> tokens;
    for (std::string token; parser >> token;)
    {
        tokens.push_back(token);
    }

    if (tokens.size() < 2 || tokens[0] != "THR1")
    {
        return;
    }

    if (tokens[1] == "REGISTERED")
    {
        m_registrationRejected = false;
        SetStatus(m_isHostRole ? "waiting relay guest..." : "waiting relay host...");
        return;
    }

    if (tokens[1] == "WAIT")
    {
        m_registrationRejected = false;
        SetStatus(m_isHostRole ? "waiting relay guest..." : "waiting relay host...");
        return;
    }

    if (tokens[1] == "READY")
    {
        m_isReady = true;
        m_registrationRejected = false;
        EnterConnectedState();
        return;
    }

    if (tokens[1] == "VERSION_MISMATCH")
    {
        g_State.versionMatched = false;
        SetStatus("version mismatch");
        return;
    }

    if (tokens[1] == "REGISTER_FAILED")
    {
        if (tokens.size() >= 3 && tokens[2] == "room_occupied")
        {
            m_isReady = false;
            m_registrationRejected = true;
            g_State.isConnected = false;
            CloseSocket();
            m_serverAddrValid = false;
            SetStatus("relay room occupied");
            return;
        }
        m_isReady = false;
        g_State.isConnected = false;
        CloseSocket();
        m_serverAddrValid = false;
        SetStatus("relay register failed");
        return;
    }

    if (tokens[1] == "META" || tokens[1] == "TRACE")
    {
        return;
    }
}

void RelayConnection::Tick()
{
    if (m_socket == kInvalidSocket)
    {
        return;
    }

    const Uint64 now = SDL_GetTicks64();
    if (!m_isReady && !m_registrationRejected &&
        (m_lastRegisterSendTick == 0 || now - m_lastRegisterSendTick >= kPeriodicPingMs))
    {
        SendRegister();
    }
}

bool RelayConnection::PollReceive(Pack &outPack, bool &hasData)
{
    hasData = false;
    if (m_socket == kInvalidSocket)
    {
        return false;
    }

    while (true)
    {
        char buffer[1024] = {};
        sockaddr_storage fromAddr {};
        socklen_t fromLen = sizeof(fromAddr);
        const int received = (int)recvfrom(m_socket, buffer, sizeof(buffer), 0, (sockaddr *)&fromAddr, &fromLen);
        if (received < 0)
        {
            const int errorCode = GetLastSocketError();
            return IsWouldBlockError(errorCode);
        }
        if (received == 0)
        {
            continue;
        }

        std::string fromIp;
        int fromPort = 0;
        TrySockAddrToIpPort((const sockaddr *)&fromAddr, fromLen, fromIp, fromPort);

        if (received >= 5 && std::memcmp(buffer, "THR1 ", 5) == 0)
        {
            HandleControl(std::string(buffer, buffer + received));
            continue;
        }

        if (received != sizeof(Pack))
        {
            continue;
        }

        std::memcpy(&outPack, buffer, sizeof(Pack));
        g_State.lastPacketBytes = received;
        g_State.lastPacketFromIp = fromIp;
        g_State.lastPacketFromPort = fromPort;
        hasData = true;
        return true;
    }

    return true;
}

bool RelayConnection::SendPack(const Pack &pack)
{
    if (m_socket == kInvalidSocket || !m_serverAddrValid)
    {
        return false;
    }

    const int sent =
        (int)sendto(m_socket, (const char *)&pack, sizeof(pack), 0, (sockaddr *)&m_serverAddr, m_serverAddrLen);
    return sent == sizeof(pack);
}

bool RelayConnection::IsReady() const
{
    return m_isReady;
}

void ClearRuntimeCaches()
{
    TraceDiagnostic("clear-runtime-caches", "-");
    g_State.localInputs.clear();
    g_State.remoteInputs.clear();
    g_State.predictedRemoteInputs.clear();
    g_State.localSeeds.clear();
    g_State.remoteSeeds.clear();
    g_State.localCtrls.clear();
    g_State.remoteCtrls.clear();
    g_State.predictedRemoteCtrls.clear();
    g_State.remoteFramesPendingRollbackCheck.clear();
    g_State.rollbackSnapshots.clear();
    g_State.pendingRollbackFrame = -1;
    g_State.rollbackActive = false;
    g_State.stallFrameRequested = false;
    g_State.rollbackTargetFrame = 0;
    g_State.rollbackSendFrame = -1;
    g_State.rollbackStage = -1;
    g_State.rollbackEpochStartFrame = 0;
    g_State.lastRollbackMismatchFrame = -1;
    g_State.lastRollbackSnapshotFrame = -1;
    g_State.lastRollbackTargetFrame = -1;
    g_State.seedValidationIgnoreUntilFrame = -1;
    g_State.lastSeedRetryMismatchFrame = -1;
    g_State.lastSeedRetryOlderThanSnapshotFrame = -1;
    g_State.lastSeedRetryLocalSeed = 0;
    g_State.lastSeedRetryRemoteSeed = 0;
    g_State.lastConfirmedSyncFrame = 0;
    g_State.currentCtrl = IGC_NONE;
    g_State.hasKnownUiState = false;
    g_State.knownUiState = false;
}

void ClearRemoteRuntimeCaches()
{
    TraceDiagnostic("clear-remote-runtime-caches", "-");
    g_State.remoteInputs.clear();
    g_State.predictedRemoteInputs.clear();
    g_State.remoteSeeds.clear();
    g_State.remoteCtrls.clear();
    g_State.predictedRemoteCtrls.clear();
    g_State.remoteFramesPendingRollbackCheck.clear();
    g_State.pendingRollbackFrame = -1;
    g_State.currentCtrl = IGC_NONE;
    g_State.isSync = true;
    g_State.isTryingReconnect = false;
    g_State.stallFrameRequested = false;
    g_State.reconnectIssued = false;
    g_State.rollbackSendFrame = -1;
    g_State.lastRollbackMismatchFrame = -1;
    g_State.lastRollbackSnapshotFrame = -1;
    g_State.lastRollbackTargetFrame = -1;
    g_State.seedValidationIgnoreUntilFrame = -1;
    g_State.lastSeedRetryMismatchFrame = -1;
    g_State.lastSeedRetryOlderThanSnapshotFrame = -1;
    g_State.lastSeedRetryLocalSeed = 0;
    g_State.lastSeedRetryRemoteSeed = 0;
    g_State.resyncTriggered = false;
    g_State.resyncTargetFrame = 0;
}

void ResetGameplayRuntimeStream()
{
    TraceDiagnostic("reset-gameplay-runtime-stream", "oldFrame=%d oldNet=%d", g_State.lastFrame, g_State.currentNetFrame);
    ClearRuntimeCaches();
    g_State.lastFrame = -1;
    g_State.currentNetFrame = 0;
    g_State.sessionBaseCalcCount = 0;
    g_State.lastRuntimeReceiveTick = SDL_GetTicks64();
    g_State.isTryingReconnect = false;
    g_State.reconnectIssued = false;
    g_State.isSync = true;
    g_State.resyncTriggered = false;
    g_State.resyncTargetFrame = 0;
    SetStatus("connected");
}

void SetStatus(const std::string &text)
{
    if (g_State.statusText != text)
    {
        TraceDiagnostic("status", "from=%s to=%s", g_State.statusText.c_str(), text.c_str());
    }
    g_State.statusText = text;
}

std::string GenerateRelaySessionId()
{
    static unsigned int s_counter = 0;
    char buffer[64] = {};
    const Uint64 ticks = SDL_GetTicks64();
    const Uint64 perf = SDL_GetPerformanceCounter();
    std::snprintf(buffer, sizeof(buffer), "%08x%08x%08x", (unsigned int)(ticks & 0xffffffffu),
                  (unsigned int)(perf & 0xffffffffu), ++s_counter);
    return buffer;
}

unsigned long GetDiagnosticProcessId()
{
#ifdef _WIN32
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

const char *ControlToString(Control control)
{
    switch (control)
    {
    case Ctrl_No_Ctrl:
        return "none";
    case Ctrl_Start_Game:
        return "start_game";
    case Ctrl_Key:
        return "key";
    case Ctrl_Set_InitSetting:
        return "init_setting";
    case Ctrl_Try_Resync:
        return "try_resync";
    default:
        return "unknown";
    }
}

const char *InGameCtrlToString(InGameCtrlType ctrl)
{
    switch (ctrl)
    {
    case Quick_Quit:
        return "quick_quit";
    case Quick_Restart:
        return "quick_restart";
    case Inf_Life:
        return "inf_life";
    case Inf_Bomb:
        return "inf_bomb";
    case Inf_Power:
        return "inf_power";
    case Add_Delay:
        return "add_delay";
    case Dec_Delay:
        return "dec_delay";
    case IGC_NONE:
        return "none";
    default:
        return "unknown";
    }
}

const char *SessionRoleTag()
{
    if (g_State.isHost)
    {
        return "host";
    }
    if (g_State.isGuest)
    {
        return "guest";
    }
    return "idle";
}

void AppendButtonName(char *buffer, size_t size, size_t &offset, bool active, const char *name)
{
    if (!active || buffer == nullptr || size == 0 || offset >= size - 1)
    {
        return;
    }

    const int written =
        std::snprintf(buffer + offset, size - offset, offset == 0 ? "%s" : "|%s", name);
    if (written <= 0)
    {
        return;
    }

    const size_t consumed = (size_t)written;
    offset = consumed >= size - offset ? size - 1 : offset + consumed;
}

std::string FormatInputBits(u16 input)
{
    char names[160] = {};
    size_t offset = 0;
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_SHOOT) != 0, "shoot");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_BOMB) != 0, "bomb");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_FOCUS) != 0, "focus");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_MENU) != 0, "menu");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_UP) != 0, "up");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_DOWN) != 0, "down");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_LEFT) != 0, "left");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_RIGHT) != 0, "right");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_SKIP) != 0, "skip");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_SHOOT2) != 0, "shoot2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_BOMB2) != 0, "bomb2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_FOCUS2) != 0, "focus2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_UP2) != 0, "up2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_DOWN2) != 0, "down2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_LEFT2) != 0, "left2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_RIGHT2) != 0, "right2");
    if (offset == 0)
    {
        std::snprintf(names, sizeof(names), "none");
    }

    char summary[192] = {};
    std::snprintf(summary, sizeof(summary), "0x%04X[%s]", input, names);
    return summary;
}

std::string BuildCtrlPacketWindowSummary(const CtrlPack &ctrl, int count)
{
    std::ostringstream stream;
    const int frameCount = std::max(0, std::min(count, kKeyPackFrameCount));
    for (int i = 0; i < frameCount; ++i)
    {
        if (i != 0)
        {
            stream << ' ';
        }
        stream << (ctrl.frame - i) << ':' << FormatInputBits(WriteToInt(ctrl.keys[i])) << '/'
               << ctrl.rngSeed[i] << '/' << InGameCtrlToString(ctrl.inGameCtrl[i]);
    }
    return stream.str();
}

std::string BuildRuntimeStateSummary()
{
    const bool inGameManager = g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER;
    const bool isInGameMenu = inGameManager && g_GameManager.isInGameMenu;
    const bool isInRetryMenu = inGameManager && g_GameManager.isInRetryMenu;
    const bool isInUi = !inGameManager || isInGameMenu || isInRetryMenu;

    char summary[640] = {};
    std::snprintf(summary, sizeof(summary),
                  "role=%s sup=%d gf=%d ui=%d menu=%d retry=%d net=%d last=%d delay=%d sync=%d conn=%d reconn=%d "
                  "rb=%d pend=%d tgt=%d send=%d stall=%d ctrl=%s lastIn=%s curIn=%s status=%s",
                  SessionRoleTag(), (int)g_Supervisor.curState, inGameManager ? g_GameManager.gameFrames : -1,
                  isInUi ? 1 : 0, isInGameMenu ? 1 : 0, isInRetryMenu ? 1 : 0, g_State.currentNetFrame,
                  g_State.lastFrame, g_State.delay, g_State.isSync ? 1 : 0, g_State.isConnected ? 1 : 0,
                  g_State.isTryingReconnect ? 1 : 0, g_State.rollbackActive ? 1 : 0,
                  g_State.pendingRollbackFrame, g_State.rollbackTargetFrame, g_State.rollbackSendFrame,
                  g_State.stallFrameRequested ? 1 : 0, InGameCtrlToString(g_State.currentCtrl),
                  FormatInputBits(g_LastFrameInput).c_str(), FormatInputBits(g_CurFrameInput).c_str(),
                  g_State.statusText.c_str());
    return summary;
}

FILE *OpenDiagnosticLogFile()
{
    static FILE *file = nullptr;
    static bool firstOpen = true;
    static char resolvedPath[512] = {};

    if (file != nullptr)
    {
        return file;
    }

    if (resolvedPath[0] == '\0')
    {
        char relativePath[128] = {};
        std::snprintf(relativePath, sizeof(relativePath), "netplay_diag_%lu.log", GetDiagnosticProcessId());
        GamePaths::Resolve(resolvedPath, sizeof(resolvedPath), relativePath);
    }

    GamePaths::EnsureParentDir(resolvedPath);
    file = std::fopen(resolvedPath, firstOpen ? "wt" : "at");
    if (file == nullptr)
    {
        return nullptr;
    }

    firstOpen = false;
    std::fprintf(file, "==== netplay diagnostic log pid=%lu ====\n", GetDiagnosticProcessId());
    std::fflush(file);
    return file;
}

void TraceDiagnostic(const char *event, const char *fmt, ...)
{
    FILE *file = OpenDiagnosticLogFile();
    if (file == nullptr)
    {
        return;
    }

    char message[1536] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    static Uint64 eventCounter = 0;
    std::fprintf(file, "#%llu t=%llu %s event=%s %s\n", (unsigned long long)++eventCounter,
                 (unsigned long long)SDL_GetTicks64(), BuildRuntimeStateSummary().c_str(), event,
                 message[0] != '\0' ? message : "-");
    std::fflush(file);
}

std::string BuildPacketSummary(const Pack &pack)
{
    char bytes[128] = {};
    char *cursor = bytes;
    const unsigned char *raw = reinterpret_cast<const unsigned char *>(&pack);
    const int dumpCount = std::min(g_State.lastPacketBytes > 0 ? g_State.lastPacketBytes : 16, 16);
    for (int i = 0; i < dumpCount; ++i)
    {
        const ptrdiff_t remaining = (bytes + sizeof(bytes)) - cursor;
        if (remaining <= 1)
        {
            break;
        }

        const int written = std::snprintf(cursor, (size_t)remaining, "%s%02X", i == 0 ? "" : " ", raw[i]);
        if (written <= 0 || written >= remaining)
        {
            break;
        }
        cursor += written;
    }

    char summary[384] = {};
    std::snprintf(summary, sizeof(summary),
                  "from=%s:%d bytes=%d type=%d ctrl=%d delay=%d ver=%d flags=%d seq=%u raw=%s",
                  g_State.lastPacketFromIp.empty() ? "?" : g_State.lastPacketFromIp.c_str(), g_State.lastPacketFromPort,
                  g_State.lastPacketBytes, pack.type, (int)pack.ctrl.ctrlType, pack.ctrl.initSetting.delay,
                  pack.ctrl.initSetting.ver, pack.ctrl.initSetting.flags, pack.seq, bytes);
    return summary;
}

void TraceLauncherPacket(const char *phase, const Pack &pack)
{
    FILE *file = std::fopen("netplay_trace.log", "a");
    if (file == nullptr)
    {
        TraceDiagnostic("launcher-packet", "phase=%s summary=%s", phase, BuildPacketSummary(pack).c_str());
        return;
    }

    std::fprintf(file, "[%s] %s\n", phase, BuildPacketSummary(pack).c_str());
    std::fclose(file);
    TraceDiagnostic("launcher-packet", "phase=%s summary=%s", phase, BuildPacketSummary(pack).c_str());
}

void SetRelayStatus(const std::string &text)
{
    g_Relay.statusText = text;
}

void CloseRelayProbeSocket()
{
    if (g_Relay.socket != kInvalidSocket)
    {
        CloseSocketHandle(g_Relay.socket);
        g_Relay.socket = kInvalidSocket;
        SocketSystem::Release();
    }
    g_Relay.family = AF_UNSPEC;
    g_Relay.isConnecting = false;
    g_Relay.isReachable = false;
    g_Relay.lastRttMs = -1;
    g_Relay.lastProbeTick = 0;
    g_Relay.lastProbeSendTick = 0;
    g_Relay.pendingNonce.clear();
}

bool OpenRelayProbeSocket()
{
    CloseRelayProbeSocket();

    const auto tryOpen = [](int family) -> bool {
        if (!SocketSystem::Acquire())
        {
            return false;
        }

        SocketHandle socketHandle = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (socketHandle == kInvalidSocket)
        {
            SocketSystem::Release();
            return false;
        }

        if (family == AF_INET6)
        {
            SetDualStack(socketHandle);
        }
        if (!SetSocketNonBlocking(socketHandle))
        {
            CloseSocketHandle(socketHandle);
            SocketSystem::Release();
            return false;
        }

        sockaddr_storage storage {};
        socklen_t storageLen = 0;
        if (!ResolveIpPortToSockAddr("", 0, family, storage, storageLen) ||
            bind(socketHandle, (sockaddr *)&storage, storageLen) != 0)
        {
            CloseSocketHandle(socketHandle);
            SocketSystem::Release();
            return false;
        }

        g_Relay.socket = socketHandle;
        g_Relay.family = family;
        return true;
    };

    return tryOpen(AF_INET6) || tryOpen(AF_INET);
}

bool SendRelayProbe()
{
    if (g_Relay.socket == kInvalidSocket || g_Relay.host.empty() || g_Relay.port <= 0)
    {
        return false;
    }

    sockaddr_storage storage {};
    socklen_t storageLen = 0;
    if (!ResolveIpPortToSockAddr(g_Relay.host, g_Relay.port, g_Relay.family, storage, storageLen))
    {
        SetRelayStatus("resolve failed");
        g_Relay.isConnecting = false;
        g_Relay.isReachable = false;
        return false;
    }

    char nonce[16] = {};
    std::snprintf(nonce, sizeof(nonce), "%08X", g_Relay.nextNonce++);
    char payload[128] = {};
    std::snprintf(payload, sizeof(payload), "THR1 PROBE %s %d", nonce, kProtocolVersion);

    const int sent =
        (int)sendto(g_Relay.socket, payload, (int)std::strlen(payload), 0, (sockaddr *)&storage, storageLen);
    if (sent <= 0)
    {
        SetRelayStatus("probe send failed");
        g_Relay.isConnecting = false;
        g_Relay.isReachable = false;
        return false;
    }

    g_Relay.pendingNonce = nonce;
    g_Relay.lastProbeTick = SDL_GetTicks64();
    g_Relay.lastProbeSendTick = g_Relay.lastProbeTick;
    g_Relay.isConnecting = true;
    if (!g_Relay.isReachable)
    {
        SetRelayStatus("probing...");
    }
    return true;
}

void TickRelayProbe()
{
    if (!g_Relay.isConfigured || g_Relay.socket == kInvalidSocket)
    {
        return;
    }

    while (true)
    {
        char buffer[kRelayMaxDatagramBytes] = {};
        sockaddr_storage fromAddr {};
        socklen_t fromLen = sizeof(fromAddr);
        const int received =
            (int)recvfrom(g_Relay.socket, buffer, sizeof(buffer) - 1, 0, (sockaddr *)&fromAddr, &fromLen);
        if (received < 0)
        {
            const int errorCode = GetLastSocketError();
            if (IsWouldBlockError(errorCode))
            {
                break;
            }
            SetRelayStatus("probe socket error");
            g_Relay.isConnecting = false;
            g_Relay.isReachable = false;
            break;
        }
        if (received == 0)
        {
            break;
        }

        buffer[received] = '\0';
        std::istringstream parser(std::string(buffer, buffer + received));
        std::vector<std::string> tokens;
        for (std::string token; parser >> token;)
        {
            tokens.push_back(token);
        }

        if (tokens.size() >= 3 && tokens[0] == "THR1" && tokens[1] == "PROBE_ACK" && tokens[2] == g_Relay.pendingNonce)
        {
            g_Relay.lastRttMs = (int)(SDL_GetTicks64() - g_Relay.lastProbeSendTick);
            g_Relay.isConnecting = false;
            g_Relay.isReachable = true;
            SetRelayStatus("reachable");
            continue;
        }

        if (tokens.size() >= 2 && tokens[0] == "THR1" && (tokens[1] == "META" || tokens[1] == "TRACE"))
        {
            continue;
        }
    }

    const Uint64 now = SDL_GetTicks64();
    if (g_Relay.isConnecting && g_Relay.lastProbeSendTick != 0 && now - g_Relay.lastProbeSendTick >= kRelayProbeTimeoutMs)
    {
        g_Relay.isConnecting = false;
        if (!g_Relay.isReachable)
        {
            SetRelayStatus("probe timeout");
        }
    }

    if (g_Relay.lastProbeTick == 0 || now - g_Relay.lastProbeTick >= kRelayProbeIntervalMs)
    {
        SendRelayProbe();
    }
}

bool UsingHostConnection()
{
    return g_State.isHost;
}

bool SendPacket(const Pack &pack)
{
    if (g_State.useRelayTransport)
    {
        return g_State.relay.SendPack(pack);
    }
    if (UsingHostConnection())
    {
        return g_State.host.SendPack(pack);
    }
    return g_State.guest.SendPack(pack);
}

bool PollPacket(Pack &pack, bool &hasData)
{
    if (g_State.useRelayTransport)
    {
        return g_State.relay.PollReceive(pack, hasData);
    }
    if (UsingHostConnection())
    {
        return g_State.host.PollReceive(pack, hasData);
    }
    return g_State.guest.PollReceive(pack, hasData);
}

bool IsCurrentUiFrame()
{
    return g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER || g_GameManager.isInGameMenu ||
           g_GameManager.isInRetryMenu;
}

int GetDisplayedRttMs()
{
    if (g_State.lastRttMs >= 0)
    {
        return g_State.lastRttMs;
    }

    if (g_State.useRelayTransport && g_Relay.lastRttMs >= 0)
    {
        return g_Relay.lastRttMs;
    }

    return -1;
}

bool CanUseRollbackSnapshots()
{
    return g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER && !g_GameManager.demoMode && !IsCurrentUiFrame();
}

void ForceDeterministicNetplayStep()
{
    // Remote lockstep must not depend on per-process render pacing.
    g_Supervisor.framerateMultiplier = 1.0f;
    g_Supervisor.effectiveFramerateMultiplier = 1.0f;
}

void ResetLauncherState()
{
    TraceDiagnostic("reset-launcher-state", "-");
    RestoreNetplayMenuDefaults();
    g_State.host.Reset();
    g_State.guest.Reset();
    g_State.relay.Reset();
    CloseRelayProbeSocket();
    g_State.isHost = false;
    g_State.isGuest = false;
    g_State.isConnected = false;
    g_State.isSessionActive = false;
    g_State.isSync = true;
    g_State.isTryingReconnect = false;
    g_State.reconnectIssued = false;
    g_State.launcherCloseRequested = false;
    g_State.versionMatched = true;
    g_State.hostIsPlayer1 = true;
    g_State.useRelayTransport = false;
    g_State.isWaitingForStartup = false;
    g_State.titleColdStartRequested = false;
    g_State.delay = kDefaultDelay;
    g_State.currentDelayCooldown = 0;
    g_State.resyncTargetFrame = 0;
    g_State.resyncTriggered = false;
    g_State.lastFrame = -1;
    g_State.sessionBaseCalcCount = 0;
    g_State.lastPeriodicPingTick = 0;
    g_State.lastRuntimeReceiveTick = 0;
    g_State.guestWaitStartTick = 0;
    g_State.lastRttMs = -1;
    g_State.nextSeq = 1;
    g_State.predictionRollbackEnabled = true;
    SetStatus("no connection");
    ClearRuntimeCaches();
}

void ApplyNetplayMenuDefaults()
{
    if (!g_State.savedDefaultDifficultyValid)
    {
        g_State.savedDefaultDifficulty = g_Supervisor.cfg.defaultDifficulty;
        g_State.savedDefaultDifficultyValid = true;
    }

    g_Supervisor.cfg.defaultDifficulty = NORMAL;
}

void RestoreNetplayMenuDefaults()
{
    if (!g_State.savedDefaultDifficultyValid)
    {
        return;
    }

    g_Supervisor.cfg.defaultDifficulty = g_State.savedDefaultDifficulty;
    g_State.savedDefaultDifficultyValid = false;
}

void PruneOldFrameData(int frame)
{
    const int pruneFrame = frame - kFrameCacheSize;
    g_State.localInputs.erase(pruneFrame);
    g_State.remoteInputs.erase(pruneFrame);
    g_State.predictedRemoteInputs.erase(pruneFrame);
    g_State.localSeeds.erase(pruneFrame);
    g_State.remoteSeeds.erase(pruneFrame);
    g_State.localCtrls.erase(pruneFrame);
    g_State.remoteCtrls.erase(pruneFrame);
    g_State.predictedRemoteCtrls.erase(pruneFrame);
    g_State.remoteFramesPendingRollbackCheck.erase(pruneFrame);
}

int OldestRollbackSnapshotFrame()
{
    for (const RollbackSnapshot &snapshot : g_State.rollbackSnapshots)
    {
        if (snapshot.stage == g_GameManager.currentStage)
        {
            return snapshot.frame;
        }
    }

    return -1;
}

bool IsRollbackFrameTooOld(int frame, int *outOldestSnapshotFrame)
{
    const int oldestSnapshotFrame = OldestRollbackSnapshotFrame();
    if (outOldestSnapshotFrame != nullptr)
    {
        *outOldestSnapshotFrame = oldestSnapshotFrame;
    }

    return frame >= 0 && oldestSnapshotFrame >= 0 && frame < oldestSnapshotFrame;
}

bool QueueRollbackFromFrame(int frame, const char *reason)
{
    if (frame < 0)
    {
        return false;
    }

    if (!IsRollbackEpochFrame(frame))
    {
        TraceDiagnostic("rollback-queue-drop-epoch", "frame=%d epochStart=%d reason=%s", frame,
                        g_State.rollbackEpochStartFrame, reason != nullptr ? reason : "-");
        return false;
    }

    int oldestSnapshotFrame = -1;
    if (IsRollbackFrameTooOld(frame, &oldestSnapshotFrame))
    {
        TraceDiagnostic("rollback-queue-drop-too-old", "frame=%d oldestSnapshot=%d reason=%s", frame,
                        oldestSnapshotFrame, reason != nullptr ? reason : "-");
        return false;
    }

    if (g_State.pendingRollbackFrame < 0 || frame < g_State.pendingRollbackFrame)
    {
        g_State.pendingRollbackFrame = frame;
    }

    return true;
}

bool IsRollbackEpochFrame(int frame)
{
    return frame >= g_State.rollbackEpochStartFrame;
}

void ResetRollbackEpoch(int frame, const char *reason, int nextEpochStartFrame)
{
    TraceDiagnostic("rollback-epoch-reset",
                    "frame=%d nextStart=%d reason=%s active=%d pending=%d snapshots=%d", frame,
                    std::max(0, nextEpochStartFrame), reason != nullptr ? reason : "-",
                    g_State.rollbackActive ? 1 : 0, g_State.pendingRollbackFrame,
                    (int)g_State.rollbackSnapshots.size());

    g_State.rollbackActive = false;
    g_State.pendingRollbackFrame = -1;
    g_State.rollbackTargetFrame = 0;
    g_State.rollbackSendFrame = -1;
    g_State.rollbackStage = -1;
    g_State.lastRollbackMismatchFrame = -1;
    g_State.lastRollbackSnapshotFrame = -1;
    g_State.lastRollbackTargetFrame = -1;
    g_State.seedValidationIgnoreUntilFrame = -1;
    g_State.lastSeedRetryMismatchFrame = -1;
    g_State.lastSeedRetryOlderThanSnapshotFrame = -1;
    g_State.lastSeedRetryLocalSeed = 0;
    g_State.lastSeedRetryRemoteSeed = 0;
    g_State.rollbackSnapshots.clear();
    g_State.predictedRemoteInputs.clear();
    g_State.predictedRemoteCtrls.clear();
    g_State.remoteFramesPendingRollbackCheck.clear();
    g_State.stallFrameRequested = false;
    g_State.rollbackEpochStartFrame = std::max(0, nextEpochStartFrame);

    if (g_State.isConnected && !g_State.isTryingReconnect)
    {
        SetStatus("connected");
    }
}

bool HasRollbackReplayHistory(int snapshotFrame, int rollbackTargetFrame, int *outRequiredStartFrame,
                              int *outMissingLocalFrame, int *outMissingRemoteFrame)
{
    if (outRequiredStartFrame != nullptr)
    {
        *outRequiredStartFrame = std::max(0, snapshotFrame - g_State.delay);
    }
    if (outMissingLocalFrame != nullptr)
    {
        *outMissingLocalFrame = -1;
    }
    if (outMissingRemoteFrame != nullptr)
    {
        *outMissingRemoteFrame = -1;
    }

    const int startFrame = std::max(0, snapshotFrame - g_State.delay);
    const int endFrame = rollbackTargetFrame - g_State.delay;
    if (endFrame < startFrame)
    {
        return true;
    }

    for (int storedFrame = startFrame; storedFrame <= endFrame; ++storedFrame)
    {
        if (g_State.localInputs.find(storedFrame) == g_State.localInputs.end())
        {
            if (outMissingLocalFrame != nullptr)
            {
                *outMissingLocalFrame = storedFrame;
            }
            return false;
        }

        if (g_State.remoteInputs.find(storedFrame) == g_State.remoteInputs.end())
        {
            if (outMissingRemoteFrame != nullptr)
            {
                *outMissingRemoteFrame = storedFrame;
            }
            return false;
        }
    }

    return true;
}

bool HasConsumedRemoteFrame(int frame)
{
    return frame < CurrentNetFrame() - g_State.delay;
}

void AdvanceConfirmedSyncFrame()
{
    while (true)
    {
        const int nextFrame = g_State.lastConfirmedSyncFrame + 1;
        const auto remoteInputIt = g_State.remoteInputs.find(nextFrame);
        if (remoteInputIt == g_State.remoteInputs.end())
        {
            break;
        }
        g_State.lastConfirmedSyncFrame = nextFrame;
    }
}

bool ResolveRemoteFrameInput(int frame, u16 &outInput, InGameCtrlType &outCtrl, bool allowPrediction,
                             bool &outUsedPrediction)
{
    outInput = 0;
    outCtrl = IGC_NONE;
    outUsedPrediction = false;

    const auto remoteIt = g_State.remoteInputs.find(frame);
    if (remoteIt != g_State.remoteInputs.end())
    {
        outInput = WriteToInt(remoteIt->second);
        const auto remoteCtrlIt = g_State.remoteCtrls.find(frame);
        if (remoteCtrlIt != g_State.remoteCtrls.end())
        {
            outCtrl = remoteCtrlIt->second;
        }
        return true;
    }

    if (!allowPrediction || !g_State.predictionRollbackEnabled)
    {
        return false;
    }

    auto predictedIt = g_State.predictedRemoteInputs.find(frame);
    if (predictedIt == g_State.predictedRemoteInputs.end())
    {
        u16 predictedInput = 0;
        InGameCtrlType predictedCtrl = IGC_NONE;
        const char *predictionSource = "zero";
        const auto prevRemoteIt = g_State.remoteInputs.find(frame - 1);
        if (prevRemoteIt != g_State.remoteInputs.end())
        {
            predictedInput = WriteToInt(prevRemoteIt->second);
            predictionSource = "remote-prev";
        }
        else
        {
            const auto prevPredictedIt = g_State.predictedRemoteInputs.find(frame - 1);
            if (prevPredictedIt != g_State.predictedRemoteInputs.end())
            {
                predictedInput = WriteToInt(prevPredictedIt->second);
                predictionSource = "pred-prev";
            }
        }

        // Do not predict a repeated pause/menu edge across frames.
        predictedInput &= (u16)~TH_BUTTON_MENU;

        const auto prevRemoteCtrlIt = g_State.remoteCtrls.find(frame - 1);
        if (prevRemoteCtrlIt != g_State.remoteCtrls.end())
        {
            predictedCtrl = prevRemoteCtrlIt->second;
        }
        else
        {
            const auto prevPredictedCtrlIt = g_State.predictedRemoteCtrls.find(frame - 1);
            if (prevPredictedCtrlIt != g_State.predictedRemoteCtrls.end())
            {
                predictedCtrl = prevPredictedCtrlIt->second;
            }
        }

        Bits<16> predictedBits;
        ReadFromInt(predictedBits, predictedInput);
        g_State.predictedRemoteInputs[frame] = predictedBits;
        g_State.predictedRemoteCtrls[frame] = predictedCtrl;
        predictedIt = g_State.predictedRemoteInputs.find(frame);
        TraceDiagnostic("predict-remote-frame", "frame=%d source=%s input=%s ctrl=%s", frame, predictionSource,
                        FormatInputBits(predictedInput).c_str(), InGameCtrlToString(predictedCtrl));
    }

    outInput = WriteToInt(predictedIt->second);
    const auto predictedCtrlIt = g_State.predictedRemoteCtrls.find(frame);
    if (predictedCtrlIt != g_State.predictedRemoteCtrls.end())
    {
        outCtrl = predictedCtrlIt->second;
    }
    outUsedPrediction = true;
    return true;
}

bool FrameHasMenuBit(const std::map<int, Bits<16>> &inputMap, int frame)
{
    const auto it = inputMap.find(frame);
    return it != inputMap.end() && (WriteToInt(it->second) & TH_BUTTON_MENU) != 0;
}

InGameCtrlType CaptureControlKeys()
{
    const Uint8 *keyboardState = SDL_GetKeyboardState(nullptr);
    if (keyboardState[SDL_SCANCODE_F2])
    {
        return Inf_Life;
    }
    if (keyboardState[SDL_SCANCODE_F3])
    {
        return Inf_Bomb;
    }
    if (keyboardState[SDL_SCANCODE_F4])
    {
        return Inf_Power;
    }
    if (keyboardState[SDL_SCANCODE_Q])
    {
        return Quick_Quit;
    }
    if (keyboardState[SDL_SCANCODE_R])
    {
        return Quick_Restart;
    }
    if (keyboardState[SDL_SCANCODE_M])
    {
        return Add_Delay;
    }
    if (keyboardState[SDL_SCANCODE_N])
    {
        return Dec_Delay;
    }
    return IGC_NONE;
}

Pack MakePing(Control ctrlType)
{
    Pack pack;
    pack.type = PACK_PING;
    pack.seq = g_State.nextSeq++;
    pack.sendTick = SDL_GetTicks64();
    pack.ctrl.ctrlType = ctrlType;
    pack.ctrl.initSetting.delay = g_State.delay;
    pack.ctrl.initSetting.ver = kProtocolVersion;
    pack.ctrl.initSetting.flags = g_State.predictionRollbackEnabled ? InitSettingFlag_PredictionRollback : 0;
    TraceLauncherPacket("launcher-send-ping", pack);
    return pack;
}

int CurrentNetFrame()
{
    return std::max(0, g_State.currentNetFrame);
}

void CaptureRollbackSnapshot(int frame)
{
    if (!g_State.isSessionActive || !g_State.isConnected || !Session::IsRemoteNetplaySession())
    {
        return;
    }

    if (!CanUseRollbackSnapshots() || g_State.rollbackActive)
    {
        return;
    }

    if (g_State.rollbackStage != g_GameManager.currentStage)
    {
        g_State.rollbackSnapshots.clear();
        g_State.rollbackStage = g_GameManager.currentStage;
        g_State.lastConfirmedSyncFrame = frame;
    }

    if (!g_State.rollbackSnapshots.empty() && g_State.rollbackSnapshots.back().frame == frame)
    {
        return;
    }

    if (!g_State.rollbackSnapshots.empty() && frame != 0 && frame % kRollbackSnapshotInterval != 0)
    {
        return;
    }

    g_State.rollbackSnapshots.emplace_back();
    RollbackSnapshot &snapshot = g_State.rollbackSnapshots.back();

    snapshot.frame = frame;
    snapshot.stage = g_GameManager.currentStage;
    snapshot.delay = g_State.delay;
    snapshot.currentDelayCooldown = g_State.currentDelayCooldown;
    snapshot.hasGuiImpl = g_Gui.impl != nullptr;
    snapshot.gameManager = g_GameManager;
    snapshot.player1 = g_Player;
    snapshot.player2 = g_Player2;
    snapshot.bulletManager = g_BulletManager;
    snapshot.enemyManager = g_EnemyManager;
    snapshot.itemManager = g_ItemManager;
    snapshot.effectManager = g_EffectManager;
    snapshot.gui = g_Gui;
    if (snapshot.hasGuiImpl)
    {
        snapshot.guiImpl = *g_Gui.impl;
    }
    snapshot.asciiManager = g_AsciiManager;
    snapshot.stageState = g_Stage;
    snapshot.eclManager = g_EclManager;
    snapshot.rng = g_Rng;
    snapshot.enemyEclRuntimeState = EnemyEclInstr::CaptureRuntimeState();
    snapshot.screenEffectRuntimeState = ScreenEffect::CaptureRuntimeState();
    snapshot.controllerRuntimeState = Controller::CaptureRuntimeState();
    snapshot.supervisorRuntimeState.calcCount = g_Supervisor.calcCount;
    snapshot.supervisorRuntimeState.wantedState = g_Supervisor.wantedState;
    snapshot.supervisorRuntimeState.curState = g_Supervisor.curState;
    snapshot.supervisorRuntimeState.wantedState2 = g_Supervisor.wantedState2;
    snapshot.supervisorRuntimeState.unk194 = g_Supervisor.unk194;
    snapshot.supervisorRuntimeState.unk198 = g_Supervisor.unk198;
    snapshot.supervisorRuntimeState.isInEnding = g_Supervisor.isInEnding;
    snapshot.supervisorRuntimeState.vsyncEnabled = g_Supervisor.vsyncEnabled;
    snapshot.supervisorRuntimeState.lastFrameTime = g_Supervisor.lastFrameTime;
    snapshot.supervisorRuntimeState.effectiveFramerateMultiplier = g_Supervisor.effectiveFramerateMultiplier;
    snapshot.supervisorRuntimeState.framerateMultiplier = g_Supervisor.framerateMultiplier;
    snapshot.supervisorRuntimeState.unk1b4 = g_Supervisor.unk1b4;
    snapshot.supervisorRuntimeState.unk1b8 = g_Supervisor.unk1b8;
    snapshot.supervisorRuntimeState.startupTimeBeforeMenuMusic = g_Supervisor.startupTimeBeforeMenuMusic;
    snapshot.gameWindowRuntimeState.tickCountToEffectiveFramerate = g_TickCountToEffectiveFramerate;
    snapshot.gameWindowRuntimeState.lastFrameTime = g_LastFrameTime;
    snapshot.gameWindowRuntimeState.curFrame = g_GameWindow.curFrame;
    snapshot.stageRuntimeState.objectInstances.clear();
    snapshot.stageRuntimeState.quadVms.clear();
    if (g_Stage.objectInstances != nullptr && g_Stage.objectsCount > 0)
    {
        snapshot.stageRuntimeState.objectInstances.assign(g_Stage.objectInstances,
                                                         g_Stage.objectInstances + g_Stage.objectsCount);
    }
    if (g_Stage.quadVms != nullptr && g_Stage.quadCount > 0)
    {
        snapshot.stageRuntimeState.quadVms.assign(g_Stage.quadVms, g_Stage.quadVms + g_Stage.quadCount);
    }
    std::memcpy(snapshot.soundRuntimeState.soundBuffersToPlay, g_SoundPlayer.soundBuffersToPlay,
                sizeof(snapshot.soundRuntimeState.soundBuffersToPlay));
    std::memcpy(snapshot.soundRuntimeState.queuedSfxState, g_SoundPlayer.unk408,
                sizeof(snapshot.soundRuntimeState.queuedSfxState));
    snapshot.soundRuntimeState.isLooping = g_SoundPlayer.isLooping;
    snapshot.lastFrameInput = g_LastFrameInput;
    snapshot.curFrameInput = g_CurFrameInput;
    snapshot.eighthFrameHeldInput = g_IsEigthFrameOfHeldInput;
    snapshot.heldInputFrames = g_NumOfFramesInputsWereHeld;

    while ((int)g_State.rollbackSnapshots.size() > kRollbackMaxSnapshots)
    {
        g_State.rollbackSnapshots.pop_front();
    }
}

bool RestoreRollbackSnapshot(const RollbackSnapshot &snapshot)
{
    if (snapshot.stage != g_GameManager.currentStage)
    {
        return false;
    }

    g_GameManager = snapshot.gameManager;
    g_Player = snapshot.player1;
    g_Player2 = snapshot.player2;
    g_BulletManager = snapshot.bulletManager;
    g_EnemyManager = snapshot.enemyManager;
    g_ItemManager = snapshot.itemManager;
    g_EffectManager = snapshot.effectManager;
    g_Gui = snapshot.gui;
    if (snapshot.hasGuiImpl && g_Gui.impl != nullptr)
    {
        *g_Gui.impl = snapshot.guiImpl;
    }
    g_AsciiManager = snapshot.asciiManager;
    g_Stage = snapshot.stageState;
    g_EclManager = snapshot.eclManager;
    g_Rng = snapshot.rng;
    if (!snapshot.stageRuntimeState.objectInstances.empty())
    {
        if (g_Stage.objectInstances == nullptr || (int)snapshot.stageRuntimeState.objectInstances.size() != g_Stage.objectsCount)
        {
            return false;
        }
        std::memcpy(g_Stage.objectInstances, snapshot.stageRuntimeState.objectInstances.data(),
                    snapshot.stageRuntimeState.objectInstances.size() * sizeof(RawStageObjectInstance));
    }
    if (!snapshot.stageRuntimeState.quadVms.empty())
    {
        if (g_Stage.quadVms == nullptr || (int)snapshot.stageRuntimeState.quadVms.size() != g_Stage.quadCount)
        {
            return false;
        }
        std::memcpy(g_Stage.quadVms, snapshot.stageRuntimeState.quadVms.data(),
                    snapshot.stageRuntimeState.quadVms.size() * sizeof(AnmVm));
    }
    EnemyEclInstr::RestoreRuntimeState(snapshot.enemyEclRuntimeState);
    ScreenEffect::RestoreRuntimeState(snapshot.screenEffectRuntimeState);
    Controller::RestoreRuntimeState(snapshot.controllerRuntimeState);
    g_Supervisor.calcCount = snapshot.supervisorRuntimeState.calcCount;
    g_Supervisor.wantedState = snapshot.supervisorRuntimeState.wantedState;
    g_Supervisor.curState = snapshot.supervisorRuntimeState.curState;
    g_Supervisor.wantedState2 = snapshot.supervisorRuntimeState.wantedState2;
    g_Supervisor.unk194 = snapshot.supervisorRuntimeState.unk194;
    g_Supervisor.unk198 = snapshot.supervisorRuntimeState.unk198;
    g_Supervisor.isInEnding = snapshot.supervisorRuntimeState.isInEnding;
    g_Supervisor.vsyncEnabled = snapshot.supervisorRuntimeState.vsyncEnabled;
    g_Supervisor.lastFrameTime = snapshot.supervisorRuntimeState.lastFrameTime;
    g_Supervisor.effectiveFramerateMultiplier = snapshot.supervisorRuntimeState.effectiveFramerateMultiplier;
    g_Supervisor.framerateMultiplier = snapshot.supervisorRuntimeState.framerateMultiplier;
    g_Supervisor.unk1b4 = snapshot.supervisorRuntimeState.unk1b4;
    g_Supervisor.unk1b8 = snapshot.supervisorRuntimeState.unk1b8;
    g_Supervisor.startupTimeBeforeMenuMusic = snapshot.supervisorRuntimeState.startupTimeBeforeMenuMusic;
    g_TickCountToEffectiveFramerate = snapshot.gameWindowRuntimeState.tickCountToEffectiveFramerate;
    g_LastFrameTime = snapshot.gameWindowRuntimeState.lastFrameTime;
    g_GameWindow.curFrame = snapshot.gameWindowRuntimeState.curFrame;
    std::memcpy(g_SoundPlayer.soundBuffersToPlay, snapshot.soundRuntimeState.soundBuffersToPlay,
                sizeof(g_SoundPlayer.soundBuffersToPlay));
    std::memcpy(g_SoundPlayer.unk408, snapshot.soundRuntimeState.queuedSfxState, sizeof(g_SoundPlayer.unk408));
    g_SoundPlayer.isLooping = snapshot.soundRuntimeState.isLooping;
    g_LastFrameInput = snapshot.lastFrameInput;
    g_CurFrameInput = snapshot.curFrameInput;
    g_IsEigthFrameOfHeldInput = snapshot.eighthFrameHeldInput;
    g_NumOfFramesInputsWereHeld = snapshot.heldInputFrames;
    g_State.delay = snapshot.delay;
    g_State.currentDelayCooldown = snapshot.currentDelayCooldown;
    g_State.currentCtrl = IGC_NONE;
    return true;
}

bool ResolveStoredFrameInput(int frame, bool isInUi, u16 &outInput, InGameCtrlType &outCtrl)
{
    const auto mapToPlayer2 = [](u16 input) -> u16 {
        u16 mapped = 0;
        mapped |= (input & TH_BUTTON_LEFT) ? TH_BUTTON_LEFT2 : 0;
        mapped |= (input & TH_BUTTON_RIGHT) ? TH_BUTTON_RIGHT2 : 0;
        mapped |= (input & TH_BUTTON_UP) ? TH_BUTTON_UP2 : 0;
        mapped |= (input & TH_BUTTON_DOWN) ? TH_BUTTON_DOWN2 : 0;
        mapped |= (input & TH_BUTTON_SHOOT) ? TH_BUTTON_SHOOT2 : 0;
        mapped |= (input & TH_BUTTON_BOMB) ? TH_BUTTON_BOMB2 : 0;
        mapped |= (input & TH_BUTTON_FOCUS) ? TH_BUTTON_FOCUS2 : 0;
        mapped |= (input & TH_BUTTON_MENU) ? TH_BUTTON_MENU : 0;
        mapped |= (input & TH_BUTTON_SKIP) ? TH_BUTTON_SKIP : 0;
        return mapped;
    };

    outCtrl = IGC_NONE;
    outInput = 0;

    if (frame - g_State.delay < 0)
    {
        return true;
    }

    const int delayedFrame = frame - g_State.delay;
    const bool localIsPlayer1 = IsLocalPlayer1();

    const auto selfIt = g_State.localInputs.find(delayedFrame);
    if (selfIt == g_State.localInputs.end())
    {
        return false;
    }
    const u16 selfInput = WriteToInt(selfIt->second);

    InGameCtrlType selfCtrl = IGC_NONE;
    const auto selfCtrlIt = g_State.localCtrls.find(delayedFrame);
    if (selfCtrlIt != g_State.localCtrls.end())
    {
        selfCtrl = selfCtrlIt->second;
    }

    u16 remoteInput = 0;
    InGameCtrlType remoteCtrl = IGC_NONE;
    bool usedPrediction = false;
    if (!ResolveRemoteFrameInput(delayedFrame, remoteInput, remoteCtrl, false, usedPrediction))
    {
        return false;
    }

    if (selfCtrl != IGC_NONE && remoteCtrl != IGC_NONE)
    {
        outCtrl = localIsPlayer1 ? selfCtrl : remoteCtrl;
    }
    else
    {
        outCtrl = selfCtrl == IGC_NONE ? remoteCtrl : selfCtrl;
    }

    if (isInUi)
    {
        outInput = selfInput | remoteInput;
        return true;
    }

    if (localIsPlayer1)
    {
        outInput = selfInput | mapToPlayer2(remoteInput);
    }
    else
    {
        outInput = remoteInput | mapToPlayer2(selfInput);
    }
    return true;
}

bool TryStartAutomaticRollback(int currentFrame, int mismatchFrame, int olderThanSnapshotFrame)
{
    if (g_State.rollbackActive || g_State.rollbackSnapshots.empty() || mismatchFrame < 0)
    {
        TraceDiagnostic("rollback-start-skip", "current=%d mismatch=%d active=%d snapshots=%d", currentFrame,
                        mismatchFrame, g_State.rollbackActive ? 1 : 0, (int)g_State.rollbackSnapshots.size());
        return false;
    }

    if (!IsRollbackEpochFrame(mismatchFrame))
    {
        TraceDiagnostic("rollback-start-drop-epoch", "current=%d mismatch=%d epochStart=%d", currentFrame,
                        mismatchFrame, g_State.rollbackEpochStartFrame);
        if (g_State.pendingRollbackFrame == mismatchFrame)
        {
            g_State.pendingRollbackFrame = -1;
        }
        return false;
    }

    int oldestSnapshotFrame = -1;
    if (IsRollbackFrameTooOld(mismatchFrame, &oldestSnapshotFrame))
    {
        TraceDiagnostic("rollback-start-drop-too-old", "current=%d mismatch=%d oldestSnapshot=%d", currentFrame,
                        mismatchFrame, oldestSnapshotFrame);
        if (g_State.pendingRollbackFrame == mismatchFrame)
        {
            g_State.pendingRollbackFrame = -1;
        }
        return false;
    }

    const int rollbackFrame = std::max(0, mismatchFrame);
    int availableRemoteFrame = std::max(0, rollbackFrame - 1);
    while (g_State.remoteInputs.find(availableRemoteFrame + 1) != g_State.remoteInputs.end())
    {
        ++availableRemoteFrame;
    }

    const int rollbackTargetFrame =
        g_State.predictionRollbackEnabled ? std::min(currentFrame, availableRemoteFrame + g_State.delay) : currentFrame;

    const RollbackSnapshot *snapshot = nullptr;
    for (auto it = g_State.rollbackSnapshots.rbegin(); it != g_State.rollbackSnapshots.rend(); ++it)
    {
        if (it->stage == g_GameManager.currentStage && it->frame <= rollbackFrame &&
            (olderThanSnapshotFrame < 0 || it->frame < olderThanSnapshotFrame) && IsRollbackEpochFrame(it->frame))
        {
            snapshot = &(*it);
            break;
        }
    }

    int requiredStartFrame = -1;
    int missingLocalFrame = -1;
    int missingRemoteFrame = -1;
    if (snapshot == nullptr || snapshot->frame > rollbackTargetFrame)
    {
        TraceDiagnostic("rollback-start-fail",
                        "current=%d mismatch=%d rollbackFrame=%d target=%d availableRemote=%d snapshot=%d olderThan=%d",
                        currentFrame, mismatchFrame, rollbackFrame, rollbackTargetFrame, availableRemoteFrame,
                        snapshot != nullptr ? snapshot->frame : -1, olderThanSnapshotFrame);
        return false;
    }

    if (!HasRollbackReplayHistory(snapshot->frame, rollbackTargetFrame, &requiredStartFrame, &missingLocalFrame,
                                  &missingRemoteFrame))
    {
        TraceDiagnostic(
            "rollback-start-drop-history",
            "current=%d mismatch=%d snapshot=%d target=%d requiredStart=%d missingLocal=%d missingRemote=%d",
            currentFrame, mismatchFrame, snapshot->frame, rollbackTargetFrame, requiredStartFrame, missingLocalFrame,
            missingRemoteFrame);
        if (g_State.pendingRollbackFrame == mismatchFrame)
        {
            g_State.pendingRollbackFrame = -1;
        }
        return false;
    }

    if (!RestoreRollbackSnapshot(*snapshot))
    {
        TraceDiagnostic("rollback-start-fail",
                        "current=%d mismatch=%d rollbackFrame=%d target=%d availableRemote=%d snapshot=%d",
                        currentFrame, mismatchFrame, rollbackFrame, rollbackTargetFrame, availableRemoteFrame,
                        snapshot->frame);
        return false;
    }

    while (!g_State.rollbackSnapshots.empty() && g_State.rollbackSnapshots.back().frame > snapshot->frame)
    {
        g_State.rollbackSnapshots.pop_back();
    }

    g_State.rollbackActive = true;
    g_State.rollbackTargetFrame = rollbackTargetFrame;
    g_State.rollbackSendFrame = currentFrame;
    g_State.lastRollbackMismatchFrame = mismatchFrame;
    g_State.lastRollbackSnapshotFrame = snapshot->frame;
    g_State.lastRollbackTargetFrame = rollbackTargetFrame;
    g_State.seedValidationIgnoreUntilFrame =
        std::max(g_State.seedValidationIgnoreUntilFrame, availableRemoteFrame + g_State.delay);
    g_State.currentNetFrame = snapshot->frame;
    g_State.lastFrame = snapshot->frame - 1;
    g_State.lastConfirmedSyncFrame = availableRemoteFrame;
    g_State.resyncTriggered = false;
    g_State.resyncTargetFrame = 0;
    g_State.isSync = true;
    g_State.currentCtrl = IGC_NONE;
    TraceDiagnostic("rollback-start",
                    "current=%d mismatch=%d rollbackFrame=%d snapshot=%d target=%d available=%d olderThan=%d",
                    currentFrame, mismatchFrame, rollbackFrame, snapshot->frame, rollbackTargetFrame,
                    availableRemoteFrame, olderThanSnapshotFrame);
    SetStatus("rollback catchup...");
    return true;
}

bool TryStartQueuedRollback(int currentFrame)
{
    if (!g_State.predictionRollbackEnabled || g_State.rollbackActive || g_State.pendingRollbackFrame < 0)
    {
        return false;
    }

    if (!CanUseRollbackSnapshots())
    {
        TraceDiagnostic("rollback-queued-deferred", "current=%d pending=%d canUseSnapshots=0", currentFrame,
                        g_State.pendingRollbackFrame);
        return false;
    }

    int oldestSnapshotFrame = -1;
    if (IsRollbackFrameTooOld(g_State.pendingRollbackFrame, &oldestSnapshotFrame))
    {
        TraceDiagnostic("rollback-queued-clear-too-old", "current=%d pending=%d oldestSnapshot=%d", currentFrame,
                        g_State.pendingRollbackFrame, oldestSnapshotFrame);
        g_State.pendingRollbackFrame = -1;
        if (!g_State.rollbackActive)
        {
            SetStatus("connected");
        }
        return false;
    }

    const int mismatchFrame = g_State.pendingRollbackFrame;
    if (TryStartAutomaticRollback(currentFrame, mismatchFrame))
    {
        g_State.pendingRollbackFrame = -1;
        return true;
    }

    return false;
}

void RefreshRollbackFrameState(int frame)
{
    if (frame < 0)
    {
        return;
    }

    g_State.localSeeds[frame] = g_Rng.seed;
}

void CommitProcessedFrameState(int frame)
{
    if (frame < 0)
    {
        return;
    }

    // localSeeds[N] is sampled at the beginning of frame N. Once frame N has
    // finished, we already know the exact pre-frame seed for N+1.
    g_State.localSeeds[frame + 1] = g_Rng.seed;
}

bool IsMenuTransitionFrame(int frame)
{
    return FrameHasMenuBit(g_State.localInputs, frame) || FrameHasMenuBit(g_State.remoteInputs, frame) ||
           FrameHasMenuBit(g_State.predictedRemoteInputs, frame);
}

void SendPeriodicPing()
{
    if (!g_State.isConnected)
    {
        return;
    }

    const Uint64 now = SDL_GetTicks64();
    if (g_State.lastPeriodicPingTick == 0 || now - g_State.lastPeriodicPingTick >= kPeriodicPingMs)
    {
        SendPacket(MakePing(Ctrl_Set_InitSetting));
        g_State.lastPeriodicPingTick = now;
    }
}

void BeginSessionStartupWait()
{
    ApplyNetplayMenuDefaults();
    g_State.isWaitingForStartup = true;
    g_State.titleColdStartRequested = true;
    g_State.launcherCloseRequested = true;
    g_State.currentCtrl = IGC_NONE;
    g_State.lastFrame = -1;
    g_State.sessionBaseCalcCount = 0;
    g_State.currentNetFrame = 0;
    ClearRuntimeCaches();
    Session::SetActiveSession(g_NetSession);
    TraceDiagnostic("begin-startup-wait", "-");
    SetStatus("waiting for peer startup...");
}

void HandleStartGameHandshake()
{
    BeginSessionStartupWait();
}

bool TryBeginStartupWaitFromRuntimePacket(const Pack &pack)
{
    if (g_State.isSessionActive || g_State.isWaitingForStartup)
    {
        return false;
    }
    if (pack.type != PACK_USUAL || pack.ctrl.ctrlType != Ctrl_Key)
    {
        return false;
    }

    BeginSessionStartupWait();
    TraceDiagnostic("startup-wait-from-runtime-packet", "frame=%d seq=%u", pack.ctrl.frame, pack.seq);
    return true;
}

void EnterConnectedState()
{
    g_State.isConnected = true;
    g_State.lastPeriodicPingTick = 0;
    if (g_State.lastRttMs < 0 && g_State.useRelayTransport && g_Relay.lastRttMs >= 0)
    {
        g_State.lastRttMs = g_Relay.lastRttMs;
    }
    TraceDiagnostic("enter-connected", "-");
    SetStatus("connected");
    SendPeriodicPing();
}

void HandleLauncherPacket(const Pack &pack)
{
    TraceLauncherPacket("launcher-recv", pack);

    if (pack.type != PACK_PING && pack.type != PACK_PONG)
    {
        return;
    }

    if (pack.ctrl.ctrlType == Ctrl_Set_InitSetting && pack.ctrl.initSetting.ver != kProtocolVersion)
    {
        g_State.versionMatched = false;
        SetStatus("version mismatch " + BuildPacketSummary(pack));
        return;
    }

    if (pack.type == PACK_PING)
    {
        Pack reply;
        reply.type = PACK_PONG;
        reply.seq = pack.seq;
        reply.sendTick = pack.sendTick;
        reply.echoTick = SDL_GetTicks64();
        reply.ctrl = pack.ctrl;
        if (reply.ctrl.ctrlType == Ctrl_Set_InitSetting)
        {
            reply.ctrl.initSetting.delay = g_State.delay;
            reply.ctrl.initSetting.ver = kProtocolVersion;
            reply.ctrl.initSetting.flags =
                g_State.predictionRollbackEnabled ? InitSettingFlag_PredictionRollback : 0;
        }
        SendPacket(reply);
        TraceLauncherPacket("launcher-send-pong", reply);
        if (!g_State.isConnected)
        {
            EnterConnectedState();
        }
        if (pack.ctrl.ctrlType == Ctrl_Start_Game)
        {
            HandleStartGameHandshake();
        }
        else if (g_State.isGuest && pack.ctrl.ctrlType == Ctrl_Set_InitSetting)
        {
            g_State.delay = std::clamp(pack.ctrl.initSetting.delay, 0, kMaxDelay);
            SetPredictionRollbackEnabled((pack.ctrl.initSetting.flags & InitSettingFlag_PredictionRollback) != 0);
        }
        return;
    }

    if (pack.type == PACK_PONG)
    {
        g_State.lastRttMs = (int)(SDL_GetTicks64() - pack.sendTick);
        if (!g_State.isConnected)
        {
            EnterConnectedState();
        }
        if (pack.ctrl.ctrlType == Ctrl_Start_Game)
        {
            HandleStartGameHandshake();
        }
        else if (g_State.isGuest && pack.ctrl.ctrlType == Ctrl_Set_InitSetting)
        {
            g_State.delay = std::clamp(pack.ctrl.initSetting.delay, 0, kMaxDelay);
            SetPredictionRollbackEnabled((pack.ctrl.initSetting.flags & InitSettingFlag_PredictionRollback) != 0);
        }
    }
}

void ProcessLauncherHost()
{
    if (g_State.useRelayTransport)
    {
        g_State.relay.Tick();
    }
    while (true)
    {
        Pack pack;
        bool hasData = false;
        if (!PollPacket(pack, hasData))
        {
            break;
        }
        if (!hasData)
        {
            break;
        }
        if (TryBeginStartupWaitFromRuntimePacket(pack))
        {
            return;
        }
        HandleLauncherPacket(pack);
        if (!g_State.versionMatched)
        {
            if (g_State.useRelayTransport)
            {
                g_State.relay.Reset();
                g_State.useRelayTransport = false;
            }
            else
            {
                g_State.host.Reset();
            }
            g_State.isHost = false;
            g_State.isConnected = false;
            return;
        }
    }
    if (!g_State.useRelayTransport || g_State.relay.IsReady())
    {
        SendPeriodicPing();
    }
}

void ProcessLauncherGuest()
{
    if (g_State.useRelayTransport)
    {
        g_State.relay.Tick();
    }
    bool gotAnyData = false;
    while (true)
    {
        Pack pack;
        bool hasData = false;
        if (!PollPacket(pack, hasData))
        {
            break;
        }
        if (!hasData)
        {
            break;
        }
        gotAnyData = true;
        if (TryBeginStartupWaitFromRuntimePacket(pack))
        {
            return;
        }
        HandleLauncherPacket(pack);
        if (!g_State.versionMatched)
        {
            if (g_State.useRelayTransport)
            {
                g_State.relay.Reset();
                g_State.useRelayTransport = false;
            }
            else
            {
                g_State.guest.Reset();
            }
            g_State.isGuest = false;
            g_State.isConnected = false;
            return;
        }
    }

    if (g_State.useRelayTransport)
    {
        if (g_State.isConnected)
        {
            SendPeriodicPing();
        }
        return;
    }

    if (!g_State.isConnected)
    {
        const Uint64 now = SDL_GetTicks64();
        if (!gotAnyData && g_State.guestWaitStartTick != 0 && now - g_State.guestWaitStartTick > kPeriodicPingMs)
        {
            g_State.guest.Reset();
            g_State.isGuest = false;
            SetStatus("no connection");
        }
    }
    else
    {
        SendPeriodicPing();
    }
}

void StoreRemoteKeyPacket(const Pack &pack)
{
    TraceDiagnostic("recv-key-packet", "frame=%d seq=%u window=%s", pack.ctrl.frame, pack.seq,
                    BuildCtrlPacketWindowSummary(pack.ctrl, 4).c_str());
    if (g_State.isSessionActive && !g_State.isWaitingForStartup)
    {
        const int currentFrame = CurrentNetFrame();
        const int maxAcceptedFrame = currentFrame + kFrameCacheSize;
        if (pack.ctrl.frame > maxAcceptedFrame)
        {
            TraceDiagnostic("recv-key-packet-drop-future", "frame=%d current=%d maxAccepted=%d", pack.ctrl.frame,
                            currentFrame, maxAcceptedFrame);
            return;
        }
    }
    for (int i = 0; i < kKeyPackFrameCount; ++i)
    {
        const int frame = pack.ctrl.frame - i;
        const Bits<16> actualBits = pack.ctrl.keys[i];
        const InGameCtrlType actualCtrl = pack.ctrl.inGameCtrl[i];
        const u16 actualSeed = pack.ctrl.rngSeed[i];
        const bool consumedFrame = g_State.predictionRollbackEnabled && HasConsumedRemoteFrame(frame);
        const auto existingInputIt = g_State.remoteInputs.find(frame);
        const auto existingSeedIt = g_State.remoteSeeds.find(frame);
        const auto existingCtrlIt = g_State.remoteCtrls.find(frame);
        const bool actualChanged =
            existingInputIt == g_State.remoteInputs.end() ||
            WriteToInt(existingInputIt->second) != WriteToInt(actualBits) ||
            existingSeedIt == g_State.remoteSeeds.end() || existingSeedIt->second != actualSeed ||
            existingCtrlIt == g_State.remoteCtrls.end() || existingCtrlIt->second != actualCtrl;

        g_State.remoteInputs[frame] = pack.ctrl.keys[i];
        g_State.remoteSeeds[frame] = actualSeed;
        g_State.remoteCtrls[frame] = actualCtrl;

        if (actualChanged)
        {
            g_State.remoteFramesPendingRollbackCheck.insert(frame);
        }

        const auto pendingCheckIt = g_State.remoteFramesPendingRollbackCheck.find(frame);
        if (pendingCheckIt == g_State.remoteFramesPendingRollbackCheck.end())
        {
            continue;
        }
        if (!consumedFrame)
        {
            if (actualChanged)
            {
                TraceDiagnostic("recv-key-frame-buffered",
                                "frame=%d input=%s seed=%u ctrl=%s consumed=0 pendingCheck=1", frame,
                                FormatInputBits(WriteToInt(actualBits)).c_str(), actualSeed,
                                InGameCtrlToString(actualCtrl));
            }
            continue;
        }

        const auto predictedIt = g_State.predictedRemoteInputs.find(frame);
        const auto predictedCtrlIt = g_State.predictedRemoteCtrls.find(frame);
        bool queuedRollback = false;
        bool predictedMismatch = false;
        const bool isRollbackEpochFrame = IsRollbackEpochFrame(frame);
        if (predictedIt != g_State.predictedRemoteInputs.end() || predictedCtrlIt != g_State.predictedRemoteCtrls.end())
        {
            const u16 predictedInput =
                predictedIt != g_State.predictedRemoteInputs.end() ? WriteToInt(predictedIt->second) : 0;
            const InGameCtrlType predictedCtrl =
                predictedCtrlIt != g_State.predictedRemoteCtrls.end() ? predictedCtrlIt->second : IGC_NONE;
            predictedMismatch = predictedInput != WriteToInt(actualBits) || predictedCtrl != actualCtrl;
            if (predictedMismatch && !IsCurrentUiFrame() && isRollbackEpochFrame)
            {
                queuedRollback = QueueRollbackFromFrame(frame, "recv-prediction");
            }
            TraceDiagnostic("recv-key-frame-prediction-check",
                            "frame=%d actual=%s/%u/%s predicted=%s/%s mismatch=%d ui=%d epoch=%d queued=%d", frame,
                            FormatInputBits(WriteToInt(actualBits)).c_str(), actualSeed,
                            InGameCtrlToString(actualCtrl), FormatInputBits(predictedInput).c_str(),
                            InGameCtrlToString(predictedCtrl), predictedMismatch ? 1 : 0, IsCurrentUiFrame() ? 1 : 0,
                            isRollbackEpochFrame ? 1 : 0, queuedRollback ? 1 : 0);
            g_State.predictedRemoteInputs.erase(frame);
            g_State.predictedRemoteCtrls.erase(frame);
        }

        const auto localSeedIt = g_State.localSeeds.find(frame);
        const bool seedMismatch = localSeedIt != g_State.localSeeds.end() && localSeedIt->second != actualSeed;
        if (seedMismatch && !predictedMismatch && !IsCurrentUiFrame() && isRollbackEpochFrame)
        {
            TraceDiagnostic("recv-key-frame-seed-mismatch",
                            "frame=%d input=%s localSeed=%u remoteSeed=%u ctrl=%s ignored=1", frame,
                            FormatInputBits(WriteToInt(actualBits)).c_str(), localSeedIt->second, actualSeed,
                            InGameCtrlToString(actualCtrl));
        }
        TraceDiagnostic("recv-key-frame-commit",
                        "frame=%d changed=%d consumed=1 input=%s seed=%u ctrl=%s localSeed=%d seedMismatch=%d "
                        "queued=%d",
                        frame,
                        actualChanged ? 1 : 0, FormatInputBits(WriteToInt(actualBits)).c_str(), actualSeed,
                        InGameCtrlToString(actualCtrl),
                        localSeedIt != g_State.localSeeds.end() ? (int)localSeedIt->second : -1,
                        seedMismatch ? 1 : 0,
                        queuedRollback ? 1 : 0);
        g_State.remoteFramesPendingRollbackCheck.erase(frame);
    }

    AdvanceConfirmedSyncFrame();
}

bool TryActivateFromStartupPacket()
{
    while (true)
    {
        Pack pack;
        bool hasData = false;
        if (!PollPacket(pack, hasData))
        {
            return false;
        }
        if (!hasData)
        {
            return false;
        }
        if (pack.type != PACK_USUAL || pack.ctrl.ctrlType != Ctrl_Key)
        {
            continue;
        }

        StoreRemoteKeyPacket(pack);
        ActivateNetplaySession();
        return true;
    }
}

bool ReceiveRuntimePackets()
{
    bool gotAnyData = false;
    while (true)
    {
        Pack pack;
        bool hasData = false;
        if (!PollPacket(pack, hasData))
        {
            break;
        }
        if (!hasData)
        {
            break;
        }
        gotAnyData = true;
        g_State.lastRuntimeReceiveTick = SDL_GetTicks64();
        if (pack.type != PACK_USUAL)
        {
            TraceDiagnostic("recv-runtime-packet-skip", "type=%d ctrl=%s seq=%u", pack.type,
                            ControlToString(pack.ctrl.ctrlType), pack.seq);
            continue;
        }
        if (pack.ctrl.ctrlType == Ctrl_Key)
        {
            StoreRemoteKeyPacket(pack);
        }
        else if (pack.ctrl.ctrlType == Ctrl_Try_Resync)
        {
            const int frameToResync = pack.ctrl.resyncSetting.frameToResync;
            const int currentFrame = CurrentNetFrame();
            if (frameToResync > currentFrame &&
                frameToResync <= currentFrame + g_State.delay * 2 + 2)
            {
                g_State.resyncTriggered = true;
                g_State.resyncTargetFrame = frameToResync;
            }
            TraceDiagnostic("recv-resync-packet", "frameToResync=%d current=%d accepted=%d", frameToResync,
                            currentFrame,
                            (frameToResync > currentFrame &&
                             frameToResync <= currentFrame + g_State.delay * 2 + 2)
                                ? 1
                                : 0);
        }
        else
        {
            TraceDiagnostic("recv-runtime-packet", "type=%d ctrl=%s seq=%u", pack.type,
                            ControlToString(pack.ctrl.ctrlType), pack.seq);
        }
    }
    return gotAnyData;
}

void SendStartupBootstrapPacket()
{
    const int frame = 0;
    Bits<16> zeroBits;
    zeroBits.Clear();

    g_State.localInputs[frame] = zeroBits;
    g_State.localSeeds[frame] = g_Rng.seed;
    g_State.localCtrls[frame] = IGC_NONE;
    PruneOldFrameData(frame);
    SendKeyPacket(frame);
}

void SendKeyPacket(int frame)
{
    Pack pack;
    pack.type = PACK_USUAL;
    pack.ctrl.ctrlType = Ctrl_Key;
    pack.ctrl.frame = frame;
    for (int i = 0; i < kKeyPackFrameCount; ++i)
    {
        const int srcFrame = frame - i;
        const auto inputIt = g_State.localInputs.find(srcFrame);
        if (inputIt != g_State.localInputs.end())
        {
            pack.ctrl.keys[i] = inputIt->second;
        }
        else
        {
            pack.ctrl.keys[i].Clear();
        }

        const auto seedIt = g_State.localSeeds.find(srcFrame);
        pack.ctrl.rngSeed[i] = seedIt != g_State.localSeeds.end() ? seedIt->second : 0;

        const auto ctrlIt = g_State.localCtrls.find(srcFrame);
        pack.ctrl.inGameCtrl[i] = ctrlIt != g_State.localCtrls.end() ? ctrlIt->second : IGC_NONE;
    }
    TraceDiagnostic("send-key-packet", "frame=%d window=%s", frame, BuildCtrlPacketWindowSummary(pack.ctrl, 4).c_str());
    SendPacket(pack);
}

void SendResyncPacket()
{
    Pack pack;
    pack.type = PACK_USUAL;
    pack.ctrl.ctrlType = Ctrl_Try_Resync;
    pack.ctrl.resyncSetting.frameToResync = g_State.resyncTargetFrame;
    TraceDiagnostic("send-resync-packet", "frameToResync=%d", g_State.resyncTargetFrame);
    SendPacket(pack);
}

void HandleDesync(int frame)
{
    if (!g_State.isConnected || g_State.isSync)
    {
        return;
    }

    if (g_State.predictionRollbackEnabled)
    {
        return;
    }

    if (g_State.resyncTriggered && g_State.resyncTargetFrame <= frame)
    {
        g_State.resyncTriggered = false;
        ReceiveRuntimePackets();
        g_Rng.seed = 0;
        g_State.remoteInputs.clear();
        g_State.predictedRemoteInputs.clear();
        g_State.remoteSeeds.clear();
        g_State.remoteCtrls.clear();
        g_State.predictedRemoteCtrls.clear();
        g_State.pendingRollbackFrame = -1;
        g_State.currentCtrl = IGC_NONE;
        g_State.isSync = true;
        return;
    }

    if (g_State.isHost)
    {
        if (!g_State.resyncTriggered)
        {
            g_State.resyncTargetFrame = frame + g_State.delay * 2 + 2;
            if (g_State.resyncTargetFrame > g_State.delay * 2 + 2)
            {
                g_State.resyncTriggered = true;
            }
        }
        if (g_State.resyncTriggered)
        {
            SendResyncPacket();
        }
    }
}

u16 ResolveFrameInput(int frame, bool isInUi, InGameCtrlType &outCtrl, bool &outRollbackStarted)
{
    const auto mapToPlayer2 = [](u16 input) -> u16 {
        u16 mapped = 0;
        mapped |= (input & TH_BUTTON_LEFT) ? TH_BUTTON_LEFT2 : 0;
        mapped |= (input & TH_BUTTON_RIGHT) ? TH_BUTTON_RIGHT2 : 0;
        mapped |= (input & TH_BUTTON_UP) ? TH_BUTTON_UP2 : 0;
        mapped |= (input & TH_BUTTON_DOWN) ? TH_BUTTON_DOWN2 : 0;
        mapped |= (input & TH_BUTTON_SHOOT) ? TH_BUTTON_SHOOT2 : 0;
        mapped |= (input & TH_BUTTON_BOMB) ? TH_BUTTON_BOMB2 : 0;
        mapped |= (input & TH_BUTTON_FOCUS) ? TH_BUTTON_FOCUS2 : 0;
        mapped |= (input & TH_BUTTON_MENU) ? TH_BUTTON_MENU : 0;
        mapped |= (input & TH_BUTTON_SKIP) ? TH_BUTTON_SKIP : 0;
        return mapped;
    };
    const bool localIsPlayer1 = IsLocalPlayer1();

    outRollbackStarted = false;
    outCtrl = IGC_NONE;
    if (frame - g_State.delay < 0)
    {
        return 0;
    }

    const int delayedFrame = frame - g_State.delay;
    const auto selfIt = g_State.localInputs.find(delayedFrame);
    u16 selfInput = selfIt != g_State.localInputs.end() ? WriteToInt(selfIt->second) : 0;
    InGameCtrlType selfCtrl = IGC_NONE;
    const auto selfCtrlIt = g_State.localCtrls.find(delayedFrame);
    if (selfCtrlIt != g_State.localCtrls.end())
    {
        selfCtrl = selfCtrlIt->second;
    }

    u16 remoteInput = 0;
    InGameCtrlType remoteCtrl = IGC_NONE;
    bool usedPrediction = false;
    const bool rollbackEpochBarrier = !isInUi && !IsRollbackEpochFrame(delayedFrame);
    const bool remoteMenuBarrier =
        !isInUi && (FrameHasMenuBit(g_State.remoteInputs, delayedFrame - 1) ||
                    FrameHasMenuBit(g_State.predictedRemoteInputs, delayedFrame - 1));
    const bool requiresAccurateRemoteInput =
        !isInUi && (rollbackEpochBarrier || ((selfInput & TH_BUTTON_MENU) != 0) || remoteMenuBarrier);
    const bool allowPrediction =
        g_State.predictionRollbackEnabled && CanUseRollbackSnapshots() && !isInUi && !requiresAccurateRemoteInput;
    const bool canRollbackNow =
        g_State.predictionRollbackEnabled && CanUseRollbackSnapshots() && !isInUi && !rollbackEpochBarrier;
    TraceDiagnostic("resolve-frame-begin",
                    "frame=%d delayed=%d ui=%d self=%s selfCtrl=%s allowPrediction=%d epochBarrier=%d "
                    "remoteMenuBarrier=%d requiresAccurate=%d canRollbackNow=%d",
                    frame, delayedFrame, isInUi ? 1 : 0, FormatInputBits(selfInput).c_str(),
                    InGameCtrlToString(selfCtrl), allowPrediction ? 1 : 0, rollbackEpochBarrier ? 1 : 0,
                    remoteMenuBarrier ? 1 : 0,
                    requiresAccurateRemoteInput ? 1 : 0, canRollbackNow ? 1 : 0);
    const auto tryCompareSyncForFrame = [&](int syncFrame) -> bool {
        const auto remoteSeedIt = g_State.remoteSeeds.find(syncFrame);
        const auto localSeedIt = g_State.localSeeds.find(syncFrame);
        if (remoteSeedIt == g_State.remoteSeeds.end() || localSeedIt == g_State.localSeeds.end())
        {
            TraceDiagnostic("resolve-frame-sync-missing-seed", "frame=%d remoteSeed=%d localSeed=%d", syncFrame,
                            remoteSeedIt != g_State.remoteSeeds.end() ? (int)remoteSeedIt->second : -1,
                            localSeedIt != g_State.localSeeds.end() ? (int)localSeedIt->second : -1);
            return false;
        }

        if (remoteSeedIt->second == localSeedIt->second)
        {
            g_State.isSync = true;
            if (syncFrame >= g_State.seedValidationIgnoreUntilFrame)
            {
                g_State.seedValidationIgnoreUntilFrame = -1;
            }
            AdvanceConfirmedSyncFrame();
            TraceDiagnostic("resolve-frame-sync-match", "frame=%d seed=%u", syncFrame, remoteSeedIt->second);
            return false;
        }

        TraceDiagnostic("resolve-frame-sync-mismatch", "frame=%d remoteSeed=%u localSeed=%u canRollback=%d ui=%d",
                        syncFrame, remoteSeedIt->second, localSeedIt->second, canRollbackNow ? 1 : 0,
                        IsCurrentUiFrame() ? 1 : 0);

        if (g_State.predictionRollbackEnabled)
        {
            if (!canRollbackNow)
            {
                TraceDiagnostic("resolve-frame-sync-seed-mismatch",
                                "frame=%d remoteSeed=%u localSeed=%u pending=%d canRollback=%d ignored=1 "
                                "reason=unrollbackable",
                                syncFrame, remoteSeedIt->second, localSeedIt->second, g_State.pendingRollbackFrame,
                                canRollbackNow ? 1 : 0);
                return false;
            }

            if (syncFrame <= g_State.seedValidationIgnoreUntilFrame)
            {
                TraceDiagnostic("resolve-frame-sync-seed-mismatch",
                                "frame=%d remoteSeed=%u localSeed=%u pending=%d canRollback=%d ignored=1 "
                                "reason=rollback-grace graceUntil=%d",
                                syncFrame, remoteSeedIt->second, localSeedIt->second, g_State.pendingRollbackFrame,
                                canRollbackNow ? 1 : 0, g_State.seedValidationIgnoreUntilFrame);
                g_State.isSync = true;
                return false;
            }

            g_State.isSync = false;
            const auto trySeedMismatchRollbackRetry = [&](int olderThanSnapshotFrame, const char *reason) -> bool {
                const bool repeatedRetry = g_State.lastSeedRetryMismatchFrame == syncFrame &&
                                           g_State.lastSeedRetryOlderThanSnapshotFrame == olderThanSnapshotFrame &&
                                           g_State.lastSeedRetryLocalSeed == localSeedIt->second &&
                                           g_State.lastSeedRetryRemoteSeed == remoteSeedIt->second;
                TraceDiagnostic("resolve-frame-sync-seed-mismatch-retry",
                                "frame=%d remoteSeed=%u localSeed=%u olderThan=%d reason=%s repeated=%d",
                                syncFrame, remoteSeedIt->second, localSeedIt->second, olderThanSnapshotFrame,
                                reason != nullptr ? reason : "-", repeatedRetry ? 1 : 0);
                if (repeatedRetry)
                {
                    return false;
                }

                g_State.lastSeedRetryMismatchFrame = syncFrame;
                g_State.lastSeedRetryOlderThanSnapshotFrame = olderThanSnapshotFrame;
                g_State.lastSeedRetryLocalSeed = localSeedIt->second;
                g_State.lastSeedRetryRemoteSeed = remoteSeedIt->second;

                if (!TryStartAutomaticRollback(frame, syncFrame, olderThanSnapshotFrame))
                {
                    return false;
                }

                outRollbackStarted = true;
                return true;
            };
            const bool canRetryFromOlderSnapshot =
                g_State.lastRollbackSnapshotFrame >= 0 && g_State.lastRollbackTargetFrame > g_State.lastRollbackSnapshotFrame &&
                syncFrame >= g_State.lastRollbackSnapshotFrame && syncFrame <= g_State.lastRollbackTargetFrame;
            const bool canRetryPostRollbackDrift =
                g_State.lastRollbackSnapshotFrame >= 0 && g_State.lastRollbackTargetFrame > g_State.lastRollbackSnapshotFrame &&
                syncFrame > g_State.lastRollbackTargetFrame &&
                syncFrame <= g_State.lastRollbackTargetFrame + kRollbackSnapshotInterval;
            if (canRetryFromOlderSnapshot)
            {
                TraceDiagnostic("resolve-frame-sync-seed-mismatch-retry-older",
                                "frame=%d remoteSeed=%u localSeed=%u lastRollbackMismatch=%d lastSnapshot=%d "
                                "lastTarget=%d",
                                syncFrame, remoteSeedIt->second, localSeedIt->second,
                                g_State.lastRollbackMismatchFrame, g_State.lastRollbackSnapshotFrame,
                                g_State.lastRollbackTargetFrame);
                if (trySeedMismatchRollbackRetry(g_State.lastRollbackSnapshotFrame, "within-last-rollback"))
                {
                    return true;
                }

                // When a just-finished rollback still has late remote packet corrections draining in,
                // the first live seed check can fire before the matching input correction is applied.
                // In that narrow window, immediately demoting the session to desynced is premature:
                // the same frame may still queue a normal prediction rollback on the next receive pass.
                TraceDiagnostic("resolve-frame-sync-seed-mismatch-defer-post-rollback",
                                "frame=%d remoteSeed=%u localSeed=%u pending=%d lastRollbackMismatch=%d "
                                "lastSnapshot=%d lastTarget=%d",
                                syncFrame, remoteSeedIt->second, localSeedIt->second, g_State.pendingRollbackFrame,
                                g_State.lastRollbackMismatchFrame, g_State.lastRollbackSnapshotFrame,
                                g_State.lastRollbackTargetFrame);
                g_State.isSync = true;
                return false;
            }
            if (canRetryPostRollbackDrift &&
                trySeedMismatchRollbackRetry(g_State.lastRollbackSnapshotFrame, "post-rollback-latent-drift"))
            {
                return true;
            }
            TraceDiagnostic("resolve-frame-sync-seed-mismatch",
                            "frame=%d remoteSeed=%u localSeed=%u pending=%d canRollback=%d ignored=1", syncFrame,
                            remoteSeedIt->second, localSeedIt->second, g_State.pendingRollbackFrame,
                            canRollbackNow ? 1 : 0);
            SetStatus(g_State.pendingRollbackFrame >= 0 ? "rollback pending..." : "desynced");
            return false;
        }

        if (!canRollbackNow)
        {
            g_State.isSync = isInUi;
            return false;
        }

        g_State.isSync = false;
        if (TryStartAutomaticRollback(frame, syncFrame))
        {
            outRollbackStarted = true;
            return true;
        }
        return false;
    };

    if (!allowPrediction)
    {
        const Uint64 waitUntil = SDL_GetTicks64() + kDisconnectTimeoutMs;
        Uint64 nextResendTick = SDL_GetTicks64() + kReconnectPingMs;

        while (SDL_GetTicks64() < waitUntil)
        {
            if (ResolveRemoteFrameInput(delayedFrame, remoteInput, remoteCtrl, false, usedPrediction))
            {
                if (tryCompareSyncForFrame(delayedFrame))
                {
                    return 0;
                }
                break;
            }

            ReceiveRuntimePackets();
            const Uint64 now = SDL_GetTicks64();
            if (now >= nextResendTick)
            {
                nextResendTick = now + kReconnectPingMs;
                SendKeyPacket(frame);
            }
            SDL_Delay(1);
        }
        if (!ResolveRemoteFrameInput(delayedFrame, remoteInput, remoteCtrl, false, usedPrediction))
        {
            TraceDiagnostic("resolve-frame-disconnect", "frame=%d delayed=%d reason=missing-remote-no-prediction", frame,
                            delayedFrame);
            g_State.isConnected = false;
            g_State.isTryingReconnect = true;
            g_State.reconnectIssued = false;
            g_State.currentCtrl = IGC_NONE;
            SetStatus("disconnected");
            return 0;
        }
    }
    else
    {
        if (TryStartQueuedRollback(frame))
        {
            outRollbackStarted = true;
            return 0;
        }

        if (!ResolveRemoteFrameInput(delayedFrame, remoteInput, remoteCtrl, true, usedPrediction))
        {
            TraceDiagnostic("resolve-frame-disconnect", "frame=%d delayed=%d reason=missing-remote-with-prediction", frame,
                            delayedFrame);
            g_State.isConnected = false;
            g_State.isTryingReconnect = true;
            g_State.reconnectIssued = false;
            g_State.currentCtrl = IGC_NONE;
            SetStatus("disconnected");
            return 0;
        }

        if (usedPrediction)
        {
            if (g_State.lastRuntimeReceiveTick != 0 &&
                SDL_GetTicks64() - g_State.lastRuntimeReceiveTick >= kDisconnectTimeoutMs)
            {
                TraceDiagnostic("resolve-frame-disconnect",
                                "frame=%d delayed=%d reason=prediction-timeout lastRecvAgo=%llu", frame, delayedFrame,
                                (unsigned long long)(SDL_GetTicks64() - g_State.lastRuntimeReceiveTick));
                g_State.isConnected = false;
                g_State.isTryingReconnect = true;
                g_State.reconnectIssued = false;
                g_State.currentCtrl = IGC_NONE;
                SetStatus("disconnected");
                return 0;
            }
        }
        else if (tryCompareSyncForFrame(delayedFrame))
        {
            return 0;
        }
    }

    if (selfCtrl != IGC_NONE && remoteCtrl != IGC_NONE)
    {
        outCtrl = localIsPlayer1 ? selfCtrl : remoteCtrl;
    }
    else
    {
        outCtrl = selfCtrl == IGC_NONE ? remoteCtrl : selfCtrl;
    }

    if (isInUi)
    {
        const u16 finalUiInput = selfInput | remoteInput;
        TraceDiagnostic("resolve-frame-result",
                        "frame=%d delayed=%d ui=1 self=%s remote=%s usedPrediction=%d remoteCtrl=%s outCtrl=%s final=%s",
                        frame, delayedFrame, FormatInputBits(selfInput).c_str(), FormatInputBits(remoteInput).c_str(),
                        usedPrediction ? 1 : 0, InGameCtrlToString(remoteCtrl), InGameCtrlToString(outCtrl),
                        FormatInputBits(finalUiInput).c_str());
        return finalUiInput;
    }

    if (localIsPlayer1)
    {
        const u16 finalInput = selfInput | mapToPlayer2(remoteInput);
        TraceDiagnostic("resolve-frame-result",
                        "frame=%d delayed=%d ui=0 p1local=1 self=%s remote=%s usedPrediction=%d remoteCtrl=%s "
                        "outCtrl=%s final=%s",
                        frame, delayedFrame, FormatInputBits(selfInput).c_str(), FormatInputBits(remoteInput).c_str(),
                        usedPrediction ? 1 : 0, InGameCtrlToString(remoteCtrl), InGameCtrlToString(outCtrl),
                        FormatInputBits(finalInput).c_str());
        return finalInput;
    }
    const u16 finalInput = remoteInput | mapToPlayer2(selfInput);
    TraceDiagnostic("resolve-frame-result",
                    "frame=%d delayed=%d ui=0 p1local=0 self=%s remote=%s usedPrediction=%d remoteCtrl=%s outCtrl=%s "
                    "final=%s",
                    frame, delayedFrame, FormatInputBits(selfInput).c_str(), FormatInputBits(remoteInput).c_str(),
                    usedPrediction ? 1 : 0, InGameCtrlToString(remoteCtrl), InGameCtrlToString(outCtrl),
                    FormatInputBits(finalInput).c_str());
    return finalInput;
}

void ApplyDelayControl()
{
    if (g_State.currentDelayCooldown > 0)
    {
        --g_State.currentDelayCooldown;
    }

    if (g_State.currentCtrl == Add_Delay && g_State.currentDelayCooldown == 0)
    {
        g_State.currentDelayCooldown = 40;
        g_State.delay = std::min(g_State.delay + 1, kMaxDelay);
        g_State.currentCtrl = IGC_NONE;
    }
    else if (g_State.currentCtrl == Dec_Delay && g_State.currentDelayCooldown == 0)
    {
        g_State.currentDelayCooldown = 40;
        g_State.delay = std::max(g_State.delay - 1, 0);
        g_State.currentCtrl = IGC_NONE;
    }
}

void TryReconnect(int frame)
{
    Session::ApplyLegacyFrameInput(0);
    g_State.currentCtrl = IGC_NONE;
    TraceDiagnostic("reconnect-tick", "frame=%d issued=%d", frame, g_State.reconnectIssued ? 1 : 0);

    if (!g_State.reconnectIssued)
    {
        if (g_State.isHost)
        {
            g_State.host.Reconnect();
        }
        else if (g_State.isGuest)
        {
            g_State.guest.Reconnect();
        }
        g_State.reconnectIssued = true;
    }

    SendKeyPacket(frame);
    if (ReceiveRuntimePackets())
    {
        g_Rng.seed = 0;
        g_State.isConnected = true;
        g_State.isTryingReconnect = false;
        g_State.reconnectIssued = false;
        g_State.remoteInputs.clear();
        g_State.predictedRemoteInputs.clear();
        g_State.remoteSeeds.clear();
        g_State.remoteCtrls.clear();
        g_State.predictedRemoteCtrls.clear();
        g_State.pendingRollbackFrame = -1;
        g_State.currentCtrl = IGC_NONE;
        g_State.isSync = true;
        TraceDiagnostic("reconnect-success", "frame=%d", frame);
        SetStatus("connected");
    }
}

SessionKind NetSession::Kind() const
{
    return SessionKind::Netplay;
}

void NetSession::ResetInputState()
{
    Controller::ResetDeviceInputState();
    Session::ResetLegacyInputState();
    ClearRuntimeCaches();
    g_State.lastFrame = -1;
    g_State.currentCtrl = IGC_NONE;
}

void NetSession::AdvanceFrameInput()
{
    g_State.stallFrameRequested = false;

    if (g_State.isWaitingForStartup && !g_State.isSessionActive)
    {
        TraceDiagnostic("advance-startup-wait", "-");
        SendStartupBootstrapPacket();
        if (!TryActivateFromStartupPacket())
        {
            Session::ApplyLegacyFrameInput(0);
            TraceDiagnostic("advance-startup-wait-no-activate", "applied=0");
            return;
        }
    }

    if (!g_State.isSessionActive)
    {
        const u16 localInput = Controller::GetInput();
        TraceDiagnostic("advance-local-only", "input=%s", FormatInputBits(localInput).c_str());
        Session::ApplyLegacyFrameInput(localInput);
        return;
    }

    const int frame = CurrentNetFrame();
    TraceDiagnostic("advance-begin", "frame=%d", frame);
    if (g_State.lastFrame >= 0 && frame < g_State.lastFrame && !g_State.rollbackActive)
    {
        TraceDiagnostic("advance-frame-reset", "frame=%d lastFrame=%d", frame, g_State.lastFrame);
        ClearRuntimeCaches();
    }

    if (g_State.isTryingReconnect)
    {
        SetStatus(g_State.isSync ? "try to reconnect...(sync)" : "try to reconnect...(desynced)");
        TryReconnect(frame);
        return;
    }

    ForceDeterministicNetplayStep();

    bool isInUi = IsCurrentUiFrame();
    bool uiStateChanged = !g_State.hasKnownUiState || g_State.knownUiState != isInUi;
    if (uiStateChanged && isInUi && g_State.pendingRollbackFrame >= 0 &&
        IsMenuTransitionFrame(g_State.pendingRollbackFrame))
    {
        const int pendingFrame = g_State.pendingRollbackFrame;
        TraceDiagnostic("ui-enter-pending-rollback", "frame=%d pending=%d", frame, pendingFrame);
        if (TryStartAutomaticRollback(frame, pendingFrame))
        {
            g_State.pendingRollbackFrame = -1;
            isInUi = IsCurrentUiFrame();
            uiStateChanged = !g_State.hasKnownUiState || g_State.knownUiState != isInUi;
            TraceDiagnostic("ui-enter-pending-rollback-started",
                            "frame=%d pending=%d restoredUi=%d target=%d", frame, pendingFrame, isInUi ? 1 : 0,
                            g_State.rollbackTargetFrame);
        }
    }
    if (uiStateChanged)
    {
        TraceDiagnostic("ui-transition", "from=%d to=%d menu=%d retry=%d", g_State.hasKnownUiState ? (g_State.knownUiState ? 1 : 0) : -1,
                        isInUi ? 1 : 0,
                        (g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER && g_GameManager.isInGameMenu) ? 1 : 0,
                        (g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER && g_GameManager.isInRetryMenu) ? 1 : 0);
        g_State.hasKnownUiState = true;
        g_State.knownUiState = isInUi;
        // Treat the first frame after a UI boundary as a transition frame. In logs this frame can
        // still carry menu/start-confirm side effects even though both peers converge one frame later,
        // so letting rollback/sync checks start immediately at ui-exit produces false desyncs.
        ResetRollbackEpoch(frame, isInUi ? "ui-enter" : "ui-exit", frame + 1);
    }

    if (isInUi)
    {
        g_State.predictedRemoteInputs.clear();
        g_State.predictedRemoteCtrls.clear();
        if (g_State.pendingRollbackFrame >= 0 && IsMenuTransitionFrame(g_State.pendingRollbackFrame))
        {
            TraceDiagnostic("ui-clear-menu-pending-rollback", "pending=%d", g_State.pendingRollbackFrame);
            g_State.pendingRollbackFrame = -1;
        }
        TraceDiagnostic("ui-clear-prediction-cache", "-");
    }

    if (!CanUseRollbackSnapshots())
    {
        if (g_State.rollbackActive)
        {
            TraceDiagnostic("rollback-cancel-ui", "frame=%d target=%d", frame, g_State.rollbackTargetFrame);
            g_State.rollbackActive = false;
            g_State.rollbackTargetFrame = 0;
            g_State.rollbackSendFrame = -1;
            SetStatus("connected");
        }
    }

    if (!g_State.rollbackActive)
    {
        CaptureRollbackSnapshot(frame);
    }

    if (TryStartQueuedRollback(frame))
    {
        InGameCtrlType currentCtrl = IGC_NONE;
        u16 finalInput = 0;
        const int processedFrame = CurrentNetFrame();
        const bool rollbackIsInUi = IsCurrentUiFrame();
        if (!ResolveStoredFrameInput(processedFrame, rollbackIsInUi, finalInput, currentCtrl))
        {
            if (g_State.isConnected && !g_State.isTryingReconnect)
            {
                SendKeyPacket(g_State.rollbackSendFrame >= 0 ? g_State.rollbackSendFrame : processedFrame);
            }
            SetStatus("rollback waiting...");
            Session::ApplyLegacyFrameInput(g_CurFrameInput);
            TraceDiagnostic("rollback-wait", "phase=queued processed=%d rollbackUi=%d reappliedCur=%s", processedFrame,
                            rollbackIsInUi ? 1 : 0, FormatInputBits(g_CurFrameInput).c_str());
            g_State.stallFrameRequested = true;
            return;
        }

        RefreshRollbackFrameState(processedFrame);
        g_State.currentCtrl = currentCtrl;
        ApplyDelayControl();
        Session::ApplyLegacyFrameInput(finalInput);
        CommitProcessedFrameState(processedFrame);
        g_State.lastFrame = processedFrame;
        g_State.currentNetFrame = processedFrame + 1;
        if (g_State.currentNetFrame >= g_State.rollbackTargetFrame)
        {
            g_State.rollbackActive = false;
            g_State.rollbackSendFrame = -1;
            SetStatus("connected");
        }
        TraceDiagnostic("rollback-step-finished", "phase=queued processed=%d final=%s ctrl=%s", processedFrame,
                        FormatInputBits(finalInput).c_str(), InGameCtrlToString(currentCtrl));
        return;
    }

    if (g_State.rollbackActive)
    {
        ReceiveRuntimePackets();
        InGameCtrlType currentCtrl = IGC_NONE;
        u16 finalInput = 0;
        const bool rollbackIsInUi = IsCurrentUiFrame();
        if (!ResolveStoredFrameInput(frame, rollbackIsInUi, finalInput, currentCtrl))
        {
            if (g_State.isConnected && !g_State.isTryingReconnect)
            {
                SendKeyPacket(g_State.rollbackSendFrame >= 0 ? g_State.rollbackSendFrame : frame);
            }
            SetStatus("rollback waiting...");
            Session::ApplyLegacyFrameInput(g_CurFrameInput);
            TraceDiagnostic("rollback-wait", "phase=active frame=%d rollbackUi=%d reappliedCur=%s", frame,
                            rollbackIsInUi ? 1 : 0, FormatInputBits(g_CurFrameInput).c_str());
            g_State.stallFrameRequested = true;
            return;
        }

        RefreshRollbackFrameState(frame);
        g_State.currentCtrl = currentCtrl;
        ApplyDelayControl();
        Session::ApplyLegacyFrameInput(finalInput);
        CommitProcessedFrameState(frame);
        g_State.lastFrame = frame;
        g_State.currentNetFrame = frame + 1;
        if (g_State.currentNetFrame >= g_State.rollbackTargetFrame)
        {
            g_State.rollbackActive = false;
            g_State.rollbackSendFrame = -1;
            SetStatus("connected");
        }
        TraceDiagnostic("rollback-step-finished", "phase=active frame=%d final=%s ctrl=%s", frame,
                        FormatInputBits(finalInput).c_str(), InGameCtrlToString(currentCtrl));
        return;
    }

    HandleDesync(frame);

    const u16 localInput = Controller::GetInput();
    Bits<16> localBits;
    ReadFromInt(localBits, localInput);
    g_State.localInputs[frame] = localBits;
    g_State.localSeeds[frame] = g_Rng.seed;
    g_State.localCtrls[frame] = CaptureControlKeys();
    TraceDiagnostic("capture-local-frame", "frame=%d input=%s seed=%u ctrl=%s", frame,
                    FormatInputBits(localInput).c_str(), g_Rng.seed, InGameCtrlToString(g_State.localCtrls[frame]));
    PruneOldFrameData(frame);

    SendKeyPacket(frame);
    ReceiveRuntimePackets();

    InGameCtrlType currentCtrl = IGC_NONE;
    bool rollbackStarted = false;
    u16 finalInput = ResolveFrameInput(frame, isInUi, currentCtrl, rollbackStarted);
    int processedFrame = frame;
    if (rollbackStarted)
    {
        processedFrame = CurrentNetFrame();
        const bool rollbackIsInUi = IsCurrentUiFrame();
        if (!ResolveStoredFrameInput(processedFrame, rollbackIsInUi, finalInput, currentCtrl))
        {
            if (g_State.isConnected && !g_State.isTryingReconnect)
            {
                SendKeyPacket(g_State.rollbackSendFrame >= 0 ? g_State.rollbackSendFrame : processedFrame);
            }
            SetStatus("rollback waiting...");
            Session::ApplyLegacyFrameInput(g_CurFrameInput);
            TraceDiagnostic("rollback-wait", "phase=post-start processed=%d rollbackUi=%d reappliedCur=%s",
                            processedFrame, rollbackIsInUi ? 1 : 0, FormatInputBits(g_CurFrameInput).c_str());
            g_State.stallFrameRequested = true;
            return;
        }
    }
    if (rollbackStarted)
    {
        RefreshRollbackFrameState(processedFrame);
    }
    g_State.currentCtrl = currentCtrl;
    ApplyDelayControl();
    Session::ApplyLegacyFrameInput(finalInput);
    CommitProcessedFrameState(processedFrame);
    g_State.lastFrame = processedFrame;
    if (g_State.isConnected && !g_State.isTryingReconnect)
    {
        g_State.currentNetFrame = processedFrame + 1;
        if (g_State.rollbackActive && g_State.currentNetFrame >= g_State.rollbackTargetFrame)
        {
            g_State.rollbackActive = false;
            g_State.rollbackSendFrame = -1;
            SetStatus("connected");
        }
    }
    TraceDiagnostic("advance-end", "frame=%d processed=%d final=%s ctrl=%s rollbackStarted=%d", frame, processedFrame,
                    FormatInputBits(finalInput).c_str(), InGameCtrlToString(currentCtrl),
                    rollbackStarted ? 1 : 0);
}

void ActivateNetplaySession()
{
    g_State.isSessionActive = true;
    g_State.isWaitingForStartup = false;
    g_State.launcherCloseRequested = true;
    g_State.isTryingReconnect = false;
    g_State.reconnectIssued = false;
    g_State.currentCtrl = IGC_NONE;
    g_State.isSync = true;
    g_State.resyncTriggered = false;
    g_State.resyncTargetFrame = 0;
    g_State.lastFrame = -1;
    g_State.sessionBaseCalcCount = 0;
    g_State.currentNetFrame = 0;
    g_State.lastRuntimeReceiveTick = SDL_GetTicks64();
    ClearRuntimeCaches();
    Session::SetActiveSession(g_NetSession);
    TraceDiagnostic("activate-session", "-");
    SetStatus("connected");
}

} // namespace

ISession &GetSession()
{
    return g_NetSession;
}

void Shutdown()
{
    ResetLauncherState();
    Session::UseLocalSession();
}

Snapshot GetSnapshot()
{
    Snapshot snapshot;
    snapshot.isHost = g_State.isHost;
    snapshot.isGuest = g_State.isGuest;
    snapshot.isConnected = g_State.isConnected;
    snapshot.isSessionActive = g_State.isSessionActive;
    snapshot.isSync = g_State.isSync;
    snapshot.isTryingReconnect = g_State.isTryingReconnect;
    snapshot.isVersionMatched = g_State.versionMatched;
    snapshot.canStartGame = g_State.isConnected && (!g_State.useRelayTransport || g_State.relay.IsReady());
    snapshot.hostIsPlayer1 = g_State.hostIsPlayer1;
    snapshot.delayLocked = g_State.isGuest;
    snapshot.predictionRollbackEnabled = g_State.predictionRollbackEnabled;
    snapshot.targetDelay = g_State.delay;
    snapshot.lastRttMs = GetDisplayedRttMs();
    snapshot.statusText = g_State.statusText;
    return snapshot;
}

RelaySnapshot GetRelaySnapshot()
{
    RelaySnapshot snapshot;
    snapshot.isConfigured = g_Relay.isConfigured;
    snapshot.isConnecting = g_Relay.isConnecting;
    snapshot.isReachable = g_Relay.isReachable;
    snapshot.lastRttMs = g_Relay.lastRttMs;
    snapshot.endpointText = g_Relay.endpointText;
    snapshot.statusText = g_Relay.statusText;
    return snapshot;
}

bool BeginHosting(int listenPort, const std::string &relayEndpoint, const std::string &roomCode,
                  std::string *errorMessage)
{
    CancelPendingConnection();
    g_State.versionMatched = true;
    g_State.launcherCloseRequested = false;

    const std::string trimmedRelay = TrimString(relayEndpoint);
    const std::string trimmedRoom = TrimString(roomCode);
    if (!trimmedRelay.empty() || !trimmedRoom.empty())
    {
        if (trimmedRelay.empty() || trimmedRoom.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "relay endpoint and room code are both required";
            }
            SetStatus("relay endpoint/room required");
            return false;
        }

        if (!g_State.relay.Start(trimmedRelay, trimmedRoom, true, listenPort, errorMessage))
        {
            SetStatus("relay register failed");
            return false;
        }

        g_State.host.Reset();
        g_State.guest.Reset();
        g_State.useRelayTransport = true;
        g_State.isHost = true;
        g_State.isGuest = false;
        g_State.isConnected = false;
        g_State.lastPeriodicPingTick = 0;
        return true;
    }

    if (!g_State.host.Start("", listenPort, AF_INET6))
    {
        if (!g_State.host.Start("", listenPort, AF_INET))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "failed to start host socket";
            }
            SetStatus("fail to start as host");
            return false;
        }
    }

    g_State.guest.Reset();
    g_State.relay.Reset();
    g_State.useRelayTransport = false;
    g_State.isHost = true;
    g_State.isGuest = false;
    g_State.isConnected = false;
    g_State.lastPeriodicPingTick = 0;
    SetStatus("waiting guest...");
    return true;
}

bool BeginGuest(const std::string &hostIp, int hostPort, int listenPort, const std::string &relayEndpoint,
                const std::string &roomCode, std::string *errorMessage)
{
    CancelPendingConnection();
    g_State.versionMatched = true;
    g_State.launcherCloseRequested = false;

    const std::string trimmedRelay = TrimString(relayEndpoint);
    const std::string trimmedRoom = TrimString(roomCode);
    if (!trimmedRelay.empty() || !trimmedRoom.empty())
    {
        if (trimmedRelay.empty() || trimmedRoom.empty())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "relay endpoint and room code are both required";
            }
            SetStatus("relay endpoint/room required");
            return false;
        }

        if (!g_State.relay.Start(trimmedRelay, trimmedRoom, false, listenPort, errorMessage))
        {
            SetStatus("relay register failed");
            return false;
        }

        g_State.host.Reset();
        g_State.guest.Reset();
        g_State.useRelayTransport = true;
        g_State.isHost = false;
        g_State.isGuest = true;
        g_State.isConnected = false;
        g_State.guestWaitStartTick = SDL_GetTicks64();
        g_State.lastPeriodicPingTick = 0;
        return true;
    }

    if ((hostIp == "127.0.0.1" || hostIp == "::1") && hostPort == listenPort)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "same-machine guest must use a different listen port";
        }
        SetStatus("guest listen port conflicts with host");
        return false;
    }
    const int family = hostIp.find(':') != std::string::npos ? AF_INET6 : AF_INET;
    if (!g_State.guest.Start(hostIp, hostPort, listenPort, family))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to start guest socket";
        }
        SetStatus("fail to start as guest");
        return false;
    }

    g_State.host.Reset();
    g_State.relay.Reset();
    g_State.useRelayTransport = false;
    g_State.isHost = false;
    g_State.isGuest = true;
    g_State.isConnected = false;
    g_State.guestWaitStartTick = SDL_GetTicks64();
    g_State.lastPeriodicPingTick = 0;
    SetStatus("trying connection...");
    SendPacket(MakePing(Ctrl_Set_InitSetting));
    return true;
}

bool BeginRelayProbe(const std::string &endpoint, std::string *errorMessage)
{
    std::string host;
    int port = 0;
    if (!ParseEndpointText(endpoint, host, port))
    {
        CloseRelayProbeSocket();
        if (errorMessage != nullptr)
        {
            *errorMessage = "invalid relay endpoint";
        }
        g_Relay.isConfigured = false;
        SetRelayStatus("invalid relay endpoint");
        return false;
    }

    g_Relay.endpointText = TrimString(endpoint);
    g_Relay.host = host;
    g_Relay.port = port;
    g_Relay.isConfigured = true;
    g_Relay.nextNonce = 1;

    if (!OpenRelayProbeSocket())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to initialize relay probe socket";
        }
        SetRelayStatus("relay socket init failed");
        return false;
    }

    if (!SendRelayProbe())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to send relay probe";
        }
        return false;
    }
    return true;
}

void ClearRelayProbe()
{
    CloseRelayProbeSocket();
    g_Relay.endpointText.clear();
    g_Relay.host.clear();
    g_Relay.port = kRelayDefaultPort;
    g_Relay.isConfigured = false;
    g_Relay.nextNonce = 1;
    SetRelayStatus("not configured");
}

void CancelPendingConnection()
{
    if (g_State.isSessionActive)
    {
        return;
    }

    g_State.host.Reset();
    g_State.guest.Reset();
    g_State.relay.Reset();
    g_State.isHost = false;
    g_State.isGuest = false;
    g_State.isConnected = false;
    g_State.useRelayTransport = false;
    g_State.isWaitingForStartup = false;
    g_State.titleColdStartRequested = false;
    g_State.lastPeriodicPingTick = 0;
    g_State.guestWaitStartTick = 0;
    g_State.lastRttMs = -1;
    g_State.versionMatched = true;
    Session::UseLocalSession();
    SetStatus("no connection");
}

bool RequestStartGame()
{
    if (!g_State.isConnected || (g_State.useRelayTransport && !g_State.relay.IsReady()))
    {
        return false;
    }

    SendPacket(MakePing(Ctrl_Start_Game));
    SetStatus("starting game...");
    return true;
}

void StartLocalSession()
{
    ResetLauncherState();
    ApplyNetplayMenuDefaults();
    Session::UseLocalNetplaySession();
    g_State.titleColdStartRequested = true;
    g_State.launcherCloseRequested = true;
    SetStatus("local game");
}

void TickLauncher()
{
    if (g_State.isSessionActive)
    {
        return;
    }

    TickRelayProbe();

    if (g_State.isHost)
    {
        ProcessLauncherHost();
    }
    else if (g_State.isGuest)
    {
        ProcessLauncherGuest();
    }
}

bool ConsumeLauncherCloseRequested()
{
    const bool requested = g_State.launcherCloseRequested;
    g_State.launcherCloseRequested = false;
    return requested;
}

bool AllowsReplay()
{
    return Session::GetKind() != SessionKind::Netplay;
}

bool IsSessionActive()
{
    return g_State.isSessionActive;
}

bool IsWaitingForStartup()
{
    return g_State.isWaitingForStartup;
}

bool IsConnected()
{
    return g_State.isConnected;
}

bool IsHost()
{
    return g_State.isHost;
}

bool IsLocalPlayer1()
{
    return !g_State.isGuest;
}

bool IsSync()
{
    return g_State.isSync;
}

bool NeedsRollbackCatchup()
{
    return g_State.rollbackActive && !g_State.stallFrameRequested;
}

bool ConsumeFrameStallRequested()
{
    const bool requested = g_State.stallFrameRequested;
    g_State.stallFrameRequested = false;
    if (requested)
    {
        TraceDiagnostic("consume-frame-stall", "-");
    }
    return requested;
}

int GetDelay()
{
    return g_State.delay;
}

void SetDelay(int delay)
{
    g_State.delay = std::clamp(delay, 0, kMaxDelay);
}

void SetPredictionRollbackEnabled(bool enabled)
{
    TraceDiagnostic("set-prediction-rollback", "enabled=%d old=%d", enabled ? 1 : 0,
                    g_State.predictionRollbackEnabled ? 1 : 0);
    g_State.predictionRollbackEnabled = enabled;
    if (!enabled)
    {
        g_State.predictedRemoteInputs.clear();
        g_State.predictedRemoteCtrls.clear();
        g_State.pendingRollbackFrame = -1;
    }
}

void SetHostPlayer1(bool hostIsPlayer1)
{
    (void)hostIsPlayer1;
    g_State.hostIsPlayer1 = true;
}

bool ConsumeTitleColdStartRequested()
{
    const bool requested = g_State.titleColdStartRequested;
    g_State.titleColdStartRequested = false;
    return requested;
}

void PrepareGameplayStart()
{
    if (Session::GetKind() != SessionKind::Netplay)
    {
        return;
    }
    ResetGameplayRuntimeStream();
}

InGameCtrlType ConsumeInGameControl()
{
    const InGameCtrlType ctrl = g_State.currentCtrl;
    g_State.currentCtrl = IGC_NONE;
    return ctrl;
}

void DrawOverlay()
{
    if (Session::GetKind() != SessionKind::Netplay)
    {
        return;
    }

    D3DXVECTOR3 pos(0.0f, 0.0f, 0.0f);
    if (g_State.isWaitingForStartup && !g_State.isSessionActive)
    {
        g_AsciiManager.AddFormatText(&pos, "waiting for peer startup...");
    }
    else if (g_State.isTryingReconnect)
    {
        g_AsciiManager.AddFormatText(&pos, "try to reconnect...(%s)", g_State.isSync ? "sync" : "desynced");
    }
    else
    {
        g_AsciiManager.AddFormatText(&pos, "%s: %s %s(%d/%d);[%d,%d]", g_State.isHost ? "H" : "G",
                                     g_State.isConnected ? "connected" : "disconnected",
                                     g_State.isSync ? "sync" : "desynced", CurrentNetFrame(),
                                     g_GameManager.gameFrames, g_State.resyncTargetFrame,
                                     g_State.resyncTriggered ? 1 : 0);
    }

    pos.x = 500.0f;
    pos.y = 428.0f;
    const int displayedRttMs = GetDisplayedRttMs();
    if (displayedRttMs >= 0)
    {
        g_AsciiManager.AddFormatText(&pos, "RTT %dms", displayedRttMs);
    }
    else
    {
        g_AsciiManager.AddFormatText(&pos, "RTT --");
    }

    pos.x = 500.0f;
    pos.y = 440.0f;
    g_AsciiManager.AddFormatText(&pos, "delay: %d", g_State.delay);
}
}; // namespace th06::Netplay
