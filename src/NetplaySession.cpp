#include "NetplaySession.hpp"

#include "AsciiManager.hpp"
#include "BulletManager.hpp"
#include "Controller.hpp"
#include "EclManager.hpp"
#include "EffectManager.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "Rng.hpp"
#include "Stage.hpp"
#include "Supervisor.hpp"
#include "ZunColor.hpp"

#include <SDL.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <sstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
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
constexpr int kMaxDelay = 10;
constexpr int kKeyPackFrameCount = 15;
constexpr int kFrameCacheSize = 180;
constexpr int kRollbackSnapshotInterval = 15;
constexpr int kRollbackMaxSnapshots = 8;
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

    int delay = 1;
    int currentDelayCooldown = 0;
    int resyncTargetFrame = 0;
    bool resyncTriggered = false;
    int lastFrame = -1;
    int lastConfirmedSyncFrame = 0;
    int rollbackTargetFrame = 0;
    int rollbackStage = -1;
    int sessionBaseCalcCount = 0;
    int currentNetFrame = 0;
    Uint64 lastPeriodicPingTick = 0;
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
    std::map<int, u16> localSeeds;
    std::map<int, u16> remoteSeeds;
    std::map<int, InGameCtrlType> localCtrls;
    std::map<int, InGameCtrlType> remoteCtrls;
    std::deque<RollbackSnapshot> rollbackSnapshots;
    bool rollbackActive = false;
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
std::string TrimString(std::string text);
bool ResolveIpPortToSockAddr(const std::string &ip, int port, int family, sockaddr_storage &outStorage,
                             socklen_t &outLen);
bool TrySockAddrToIpPort(const sockaddr *addr, socklen_t addrLen, std::string &outIp, int &outPort);
bool ParseEndpointText(const std::string &endpoint, std::string &outHost, int &outPort);
bool OpenRelayProbeSocket();
void CloseRelayProbeSocket();
void SetRelayStatus(const std::string &text);
bool SendRelayProbe();
void TickRelayProbe();
void ApplyNetplayMenuDefaults();
void RestoreNetplayMenuDefaults();
bool UsingHostConnection();
bool SendPacket(const Pack &pack);
bool PollPacket(Pack &pack, bool &hasData);
void ResetLauncherState();
void PruneOldFrameData(int frame);
InGameCtrlType CaptureControlKeys();
Pack MakePing(Control ctrlType);
int CurrentNetFrame();
void CaptureRollbackSnapshot(int frame);
bool ResolveStoredFrameInput(int frame, bool isInUi, u16 &outInput, InGameCtrlType &outCtrl);
bool RestoreRollbackSnapshot(const RollbackSnapshot &snapshot);
bool TryStartAutomaticRollback(int currentFrame, int mismatchFrame);
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
    if (m_roomCode.empty())
    {
        return;
    }

    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "THR1 REGISTER %s %s %d", m_roomCode.c_str(),
                  m_isHostRole ? "host" : "guest", kProtocolVersion);
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
        SetStatus(m_isHostRole ? "waiting relay guest..." : "waiting relay host...");
        return;
    }

    if (tokens[1] == "WAIT")
    {
        SetStatus(m_isHostRole ? "waiting relay guest..." : "waiting relay host...");
        return;
    }

    if (tokens[1] == "READY")
    {
        m_isReady = true;
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
    if (!m_isReady && (m_lastRegisterSendTick == 0 || now - m_lastRegisterSendTick >= kPeriodicPingMs))
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
    g_State.localInputs.clear();
    g_State.remoteInputs.clear();
    g_State.localSeeds.clear();
    g_State.remoteSeeds.clear();
    g_State.localCtrls.clear();
    g_State.remoteCtrls.clear();
    g_State.rollbackSnapshots.clear();
    g_State.rollbackActive = false;
    g_State.rollbackTargetFrame = 0;
    g_State.rollbackStage = -1;
    g_State.lastConfirmedSyncFrame = 0;
    g_State.currentCtrl = IGC_NONE;
}

void ClearRemoteRuntimeCaches()
{
    g_State.remoteInputs.clear();
    g_State.remoteSeeds.clear();
    g_State.remoteCtrls.clear();
    g_State.currentCtrl = IGC_NONE;
    g_State.isSync = true;
    g_State.isTryingReconnect = false;
    g_State.reconnectIssued = false;
    g_State.resyncTriggered = false;
    g_State.resyncTargetFrame = 0;
}

void SetStatus(const std::string &text)
{
    g_State.statusText = text;
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
                  "from=%s:%d bytes=%d type=%d ctrl=%d delay=%d ver=%d seq=%u raw=%s",
                  g_State.lastPacketFromIp.empty() ? "?" : g_State.lastPacketFromIp.c_str(), g_State.lastPacketFromPort,
                  g_State.lastPacketBytes, pack.type, (int)pack.ctrl.ctrlType, pack.ctrl.initSetting.delay,
                  pack.ctrl.initSetting.ver, pack.seq, bytes);
    return summary;
}

void TraceLauncherPacket(const char *phase, const Pack &pack)
{
    FILE *file = std::fopen("netplay_trace.log", "a");
    if (file == nullptr)
    {
        return;
    }

    std::fprintf(file, "[%s] %s\n", phase, BuildPacketSummary(pack).c_str());
    std::fclose(file);
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

void ResetLauncherState()
{
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
    g_State.delay = 1;
    g_State.currentDelayCooldown = 0;
    g_State.resyncTargetFrame = 0;
    g_State.resyncTriggered = false;
    g_State.lastFrame = -1;
    g_State.sessionBaseCalcCount = 0;
    g_State.lastPeriodicPingTick = 0;
    g_State.guestWaitStartTick = 0;
    g_State.lastRttMs = -1;
    g_State.nextSeq = 1;
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
    g_State.localSeeds.erase(pruneFrame);
    g_State.remoteSeeds.erase(pruneFrame);
    g_State.localCtrls.erase(pruneFrame);
    g_State.remoteCtrls.erase(pruneFrame);
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

    if (g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER || g_GameManager.demoMode || g_State.rollbackActive)
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

    if (frame != 0 && frame % kRollbackSnapshotInterval != 0)
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

    const auto remoteIt = g_State.remoteInputs.find(delayedFrame);
    if (remoteIt == g_State.remoteInputs.end())
    {
        return false;
    }
    const u16 remoteInput = WriteToInt(remoteIt->second);

    InGameCtrlType selfCtrl = IGC_NONE;
    const auto selfCtrlIt = g_State.localCtrls.find(delayedFrame);
    if (selfCtrlIt != g_State.localCtrls.end())
    {
        selfCtrl = selfCtrlIt->second;
    }

    InGameCtrlType remoteCtrl = IGC_NONE;
    const auto remoteCtrlIt = g_State.remoteCtrls.find(delayedFrame);
    if (remoteCtrlIt != g_State.remoteCtrls.end())
    {
        remoteCtrl = remoteCtrlIt->second;
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

bool TryStartAutomaticRollback(int currentFrame, int mismatchFrame)
{
    if (g_State.rollbackActive || g_State.rollbackSnapshots.empty() || mismatchFrame <= 0)
    {
        return false;
    }

    int lastSynchronizedFrame = std::min(g_State.lastConfirmedSyncFrame, mismatchFrame - 1);
    while (lastSynchronizedFrame >= 0)
    {
        const auto localSeedIt = g_State.localSeeds.find(lastSynchronizedFrame);
        const auto remoteSeedIt = g_State.remoteSeeds.find(lastSynchronizedFrame);
        if (localSeedIt != g_State.localSeeds.end() && remoteSeedIt != g_State.remoteSeeds.end() &&
            localSeedIt->second == remoteSeedIt->second)
        {
            break;
        }
        --lastSynchronizedFrame;
    }

    if (lastSynchronizedFrame < 0)
    {
        return false;
    }

    const RollbackSnapshot *snapshot = nullptr;
    for (auto it = g_State.rollbackSnapshots.rbegin(); it != g_State.rollbackSnapshots.rend(); ++it)
    {
        if (it->stage == g_GameManager.currentStage && it->frame <= lastSynchronizedFrame)
        {
            snapshot = &(*it);
            break;
        }
    }

    if (snapshot == nullptr || snapshot->frame >= currentFrame || !RestoreRollbackSnapshot(*snapshot))
    {
        return false;
    }

    while (!g_State.rollbackSnapshots.empty() && g_State.rollbackSnapshots.back().frame > snapshot->frame)
    {
        g_State.rollbackSnapshots.pop_back();
    }

    g_State.rollbackActive = true;
    g_State.rollbackTargetFrame = currentFrame;
    g_State.currentNetFrame = snapshot->frame;
    g_State.lastFrame = snapshot->frame - 1;
    g_State.lastConfirmedSyncFrame = lastSynchronizedFrame;
    g_State.resyncTriggered = false;
    g_State.resyncTargetFrame = 0;
    g_State.isSync = true;
    g_State.currentCtrl = IGC_NONE;
    SetStatus("rollback catchup...");
    return true;
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
    SetStatus("waiting for peer startup...");
}

void HandleStartGameHandshake()
{
    BeginSessionStartupWait();
}

void EnterConnectedState()
{
    g_State.isConnected = true;
    g_State.lastPeriodicPingTick = 0;
    SetStatus("connected");
}

void HandleLauncherPacket(const Pack &pack)
{
    TraceLauncherPacket("launcher-recv", pack);

    if (pack.type != PACK_PING && pack.type != PACK_PONG)
    {
        SetStatus("unexpected launcher packet");
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
    SendPeriodicPing();
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
    for (int i = 0; i < kKeyPackFrameCount; ++i)
    {
        const int frame = pack.ctrl.frame - i;
        g_State.remoteInputs[frame] = pack.ctrl.keys[i];
        g_State.remoteSeeds[frame] = pack.ctrl.rngSeed[i];
        g_State.remoteCtrls[frame] = pack.ctrl.inGameCtrl[i];
    }
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
        if (pack.type != PACK_USUAL)
        {
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
    SendPacket(pack);
}

void SendResyncPacket()
{
    Pack pack;
    pack.type = PACK_USUAL;
    pack.ctrl.ctrlType = Ctrl_Try_Resync;
    pack.ctrl.resyncSetting.frameToResync = g_State.resyncTargetFrame;
    SendPacket(pack);
}

void HandleDesync(int frame)
{
    if (!g_State.isConnected || g_State.isSync)
    {
        return;
    }

    if (g_State.resyncTriggered && g_State.resyncTargetFrame <= frame)
    {
        g_State.resyncTriggered = false;
        ReceiveRuntimePackets();
        g_Rng.seed = 0;
        g_State.remoteInputs.clear();
        g_State.remoteSeeds.clear();
        g_State.remoteCtrls.clear();
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
    bool hasRemoteData = false;
    const Uint64 waitUntil = SDL_GetTicks64() + kDisconnectTimeoutMs;
    Uint64 nextResendTick = SDL_GetTicks64() + kReconnectPingMs;

    while (SDL_GetTicks64() < waitUntil)
    {
        const auto remoteIt = g_State.remoteInputs.find(delayedFrame);
        if (remoteIt != g_State.remoteInputs.end())
        {
            remoteInput = WriteToInt(remoteIt->second);
            const auto remoteSeedIt = g_State.remoteSeeds.find(delayedFrame);
            const auto localSeedIt = g_State.localSeeds.find(delayedFrame);
            if (remoteSeedIt != g_State.remoteSeeds.end() && localSeedIt != g_State.localSeeds.end())
            {
                if (remoteSeedIt->second == localSeedIt->second)
                {
                    g_State.isSync = true;
                    g_State.lastConfirmedSyncFrame = delayedFrame;
                }
                else
                {
                    g_State.isSync = false;
                    if (TryStartAutomaticRollback(frame, delayedFrame))
                    {
                        outRollbackStarted = true;
                        return 0;
                    }
                }
            }

            const auto remoteCtrlIt = g_State.remoteCtrls.find(delayedFrame);
            if (remoteCtrlIt != g_State.remoteCtrls.end())
            {
                remoteCtrl = remoteCtrlIt->second;
            }
            hasRemoteData = true;
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

    if (!hasRemoteData)
    {
        g_State.isConnected = false;
        g_State.isTryingReconnect = true;
        g_State.reconnectIssued = false;
        g_State.currentCtrl = IGC_NONE;
        SetStatus("disconnected");
        return 0;
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
        return selfInput | remoteInput;
    }

    if (localIsPlayer1)
    {
        return selfInput | mapToPlayer2(remoteInput);
    }
    return remoteInput | mapToPlayer2(selfInput);
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
        g_State.remoteSeeds.clear();
        g_State.remoteCtrls.clear();
        g_State.currentCtrl = IGC_NONE;
        g_State.isSync = true;
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
    if (g_State.isWaitingForStartup && !g_State.isSessionActive)
    {
        SendStartupBootstrapPacket();
        if (!TryActivateFromStartupPacket())
        {
            Session::ApplyLegacyFrameInput(0);
            return;
        }
    }

    if (!g_State.isSessionActive)
    {
        Session::ApplyLegacyFrameInput(Controller::GetInput());
        return;
    }

    const int frame = CurrentNetFrame();
    if (g_State.lastFrame >= 0 && frame < g_State.lastFrame && !g_State.rollbackActive)
    {
        ClearRuntimeCaches();
    }

    if (g_State.isTryingReconnect)
    {
        SetStatus(g_State.isSync ? "try to reconnect...(sync)" : "try to reconnect...(desynced)");
        TryReconnect(frame);
        return;
    }

    const bool isInUi =
        g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER ||
        (g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER && g_GameManager.isInGameMenu);

    CaptureRollbackSnapshot(frame);

    if (g_State.rollbackActive)
    {
        InGameCtrlType currentCtrl = IGC_NONE;
        u16 finalInput = 0;
        if (!ResolveStoredFrameInput(frame, isInUi, finalInput, currentCtrl))
        {
            g_State.rollbackActive = false;
            SetStatus("rollback aborted");
            Session::ApplyLegacyFrameInput(0);
            return;
        }

        g_State.currentCtrl = currentCtrl;
        ApplyDelayControl();
        Session::ApplyLegacyFrameInput(finalInput);
        g_State.lastFrame = frame;
        g_State.currentNetFrame = frame + 1;
        if (g_State.currentNetFrame >= g_State.rollbackTargetFrame)
        {
            g_State.rollbackActive = false;
            SetStatus("connected");
        }
        return;
    }

    HandleDesync(frame);

    const u16 localInput = Controller::GetInput();
    Bits<16> localBits;
    ReadFromInt(localBits, localInput);
    g_State.localInputs[frame] = localBits;
    g_State.localSeeds[frame] = g_Rng.seed;
    g_State.localCtrls[frame] = CaptureControlKeys();
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
        if (!ResolveStoredFrameInput(processedFrame, isInUi, finalInput, currentCtrl))
        {
            g_State.rollbackActive = false;
            SetStatus("rollback aborted");
            Session::ApplyLegacyFrameInput(0);
            return;
        }
    }
    g_State.currentCtrl = currentCtrl;
    ApplyDelayControl();
    Session::ApplyLegacyFrameInput(finalInput);
    g_State.lastFrame = processedFrame;
    if (g_State.isConnected && !g_State.isTryingReconnect)
    {
        g_State.currentNetFrame = processedFrame + 1;
    }
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
    ClearRuntimeCaches();
    Session::SetActiveSession(g_NetSession);
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
    snapshot.canStartGame = g_State.isConnected;
    snapshot.hostIsPlayer1 = g_State.hostIsPlayer1;
    snapshot.delayLocked = g_State.isGuest;
    snapshot.targetDelay = g_State.delay;
    snapshot.lastRttMs = g_State.lastRttMs;
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
    if (!g_State.isConnected)
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
    return g_State.rollbackActive;
}

int GetDelay()
{
    return g_State.delay;
}

void SetDelay(int delay)
{
    g_State.delay = std::clamp(delay, 0, kMaxDelay);
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
    ClearRemoteRuntimeCaches();
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
    pos.y = 440.0f;
    g_AsciiManager.AddFormatText(&pos, "delay: %d", g_State.delay);
}
}; // namespace th06::Netplay
