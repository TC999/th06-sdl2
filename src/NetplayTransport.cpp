#include "NetplayInternal.hpp"
#include "NetplayAuthoritativeTransport.hpp"
#include <new>
#include "AndroidTouchInput.hpp"

namespace th06::Netplay
{
namespace
{
constexpr u8 kSnapshotDatagramMagic[4] = {'T', 'S', 'N', 'P'};
constexpr u8 kSnapshotDatagramVersion = 1;
constexpr u8 kConsistencyDatagramMagic[4] = {'T', 'D', 'H', 'G'};
constexpr u8 kConsistencyDatagramVersion = 1;
constexpr u8 kAuthoritativeStateDatagramMagic[4] = {'T', 'A', 'U', 'T'};
constexpr u8 kAuthoritativeStateDatagramVersion = 1;

u16 NormalizeStartupSharedSeed(u16 seed)
{
    return seed != 0 ? seed : 1;
}

void AbortStartupSeedMismatch(u16 peerSharedSeed, const char *source)
{
    TraceDiagnostic("startup-seed-mismatch-abort", "source=%s peerSeed=%u authoritySeed=%u",
                    source != nullptr ? source : "-", peerSharedSeed, g_State.authoritySharedSeed);
    g_State.isSessionActive = false;
    g_State.isWaitingForStartup = false;
    g_State.titleColdStartRequested = false;
    g_State.launcherCloseRequested = false;
    g_State.currentCtrl = IGC_NONE;
    g_State.lastFrame = -1;
    g_State.currentNetFrame = 0;
    g_State.authoritySharedSeed = 0;
    g_State.authoritySharedSeedKnown = false;
    g_State.startupPeerSeedConfirmed = false;
    g_State.authoritySharedSeedApplied = false;
    g_State.gameplayRuntimeStreamRebasedForStartup = false;
    g_State.startupActivationComplete = false;
    ClearRuntimeCaches();
    Session::UseLocalSession();
    SetStatus("startup seed mismatch");
}

bool TryAdoptStartupAuthoritySeedFromPeer(u16 peerSharedSeed, const char *source)
{
    if (!g_State.isWaitingForStartup || g_State.isSessionActive)
    {
        return false;
    }

    if (g_State.isHost)
    {
        if (!g_State.authoritySharedSeedKnown || peerSharedSeed == 0)
        {
            return false;
        }

        const u16 normalizedSeed = NormalizeStartupSharedSeed(peerSharedSeed);
        if (normalizedSeed != g_State.authoritySharedSeed)
        {
            TraceDiagnostic("startup-seed-wait-peer-confirm", "source=%s peerSeed=%u authoritySeed=%u role=host",
                            source != nullptr ? source : "-", normalizedSeed, g_State.authoritySharedSeed);
            return false;
        }

        if (!g_State.startupPeerSeedConfirmed)
        {
            g_State.startupPeerSeedConfirmed = true;
            TraceDiagnostic("startup-seed-peer-confirmed", "source=%s seed=%u role=host",
                            source != nullptr ? source : "-", normalizedSeed);
        }
        return true;
    }

    if (peerSharedSeed == 0)
    {
        return false;
    }

    const u16 normalizedSeed = NormalizeStartupSharedSeed(peerSharedSeed);
    if (!g_State.authoritySharedSeedKnown)
    {
        g_State.authoritySharedSeed = normalizedSeed;
        g_State.authoritySharedSeedKnown = true;
        g_State.startupPeerSeedConfirmed = true;
        TraceDiagnostic("startup-seed-adopt", "source=%s seed=%u", source != nullptr ? source : "-", normalizedSeed);
        return true;
    }

    if (g_State.authoritySharedSeed != normalizedSeed)
    {
        AbortStartupSeedMismatch(normalizedSeed, source);
        return false;
    }

    if (!g_State.startupPeerSeedConfirmed)
    {
        g_State.startupPeerSeedConfirmed = true;
        TraceDiagnostic("startup-seed-peer-confirmed", "source=%s seed=%u role=guest",
                        source != nullptr ? source : "-", normalizedSeed);
    }
    return true;
}

void ApplyStartupInitSettingsFromPeer(const CtrlPack &ctrl, const char *source)
{
    g_State.startupInitSettingReceived = true;

    if (g_State.isGuest)
    {
        g_State.delay = std::clamp(ctrl.initSetting.delay, 0, kMaxDelay);
        SetPredictionRollbackEnabled((ctrl.initSetting.flags & InitSettingFlag_PredictionRollback) != 0);
        SetAuthoritativeModeEnabled((ctrl.initSetting.flags & InitSettingFlag_AuthoritativeMode) != 0);
        g_State.hostIsPlayer1 = ctrl.initSetting.hostIsPlayer1 != 0;

        const u16 sharedSeed = NormalizeStartupSharedSeed(ctrl.initSetting.sharedSeed);
        if (sharedSeed != 0)
        {
            g_State.authoritySharedSeed = sharedSeed;
            g_State.authoritySharedSeedKnown = true;
        }

        g_GameManager.difficulty = (Difficulty)std::clamp<int>(ctrl.initSetting.difficulty, EASY, EXTRA);
        g_GameManager.character = ctrl.initSetting.character1;
        g_GameManager.shotType = ctrl.initSetting.shotType1;
        g_GameManager.character2 = ctrl.initSetting.character2;
        g_GameManager.shotType2 = ctrl.initSetting.shotType2;
        g_GameManager.isInPracticeMode = ctrl.initSetting.practiceMode != 0;
    }

    TraceDiagnostic("startup-init-setting-recv",
                    "source=%s role=%s delay=%d prediction=%d authoritative=%d seed=%u hostP1=%d diff=%d "
                    "c1=%d s1=%d c2=%d s2=%d practice=%d",
                    source != nullptr ? source : "-", g_State.isHost ? "host" : (g_State.isGuest ? "guest" : "idle"),
                    g_State.delay, g_State.predictionRollbackEnabled ? 1 : 0, g_State.authoritativeModeEnabled ? 1 : 0,
                    ctrl.initSetting.sharedSeed, g_State.hostIsPlayer1 ? 1 : 0, (int)g_GameManager.difficulty,
                    g_GameManager.character, g_GameManager.shotType, g_GameManager.character2, g_GameManager.shotType2,
                    g_GameManager.isInPracticeMode ? 1 : 0);
}

RawDatagramKind ClassifyRawDatagram(const u8 *bytes, int size)
{
    if (bytes == nullptr || size <= 0)
    {
        return RawDatagram_Unknown;
    }
    if (size >= 5 && std::memcmp(bytes, "THR1 ", 5) == 0)
    {
        return RawDatagram_RelayText;
    }
    if (size >= (int)sizeof(SnapshotDatagramHeader))
    {
        const SnapshotDatagramHeader *header = (const SnapshotDatagramHeader *)bytes;
        if (std::memcmp(header->magic, kSnapshotDatagramMagic, sizeof(kSnapshotDatagramMagic)) == 0 &&
            header->version == kSnapshotDatagramVersion)
        {
            return RawDatagram_SnapshotSideband;
        }
    }
    if (size >= (int)sizeof(ConsistencyDatagramHeader))
    {
        const ConsistencyDatagramHeader *header = (const ConsistencyDatagramHeader *)bytes;
        if (std::memcmp(header->magic, kConsistencyDatagramMagic, sizeof(kConsistencyDatagramMagic)) == 0 &&
            header->version == kConsistencyDatagramVersion)
        {
            return RawDatagram_ConsistencySideband;
        }
    }
    if (size >= (int)sizeof(AuthoritativeDatagramPrefix))
    {
        const AuthoritativeDatagramPrefix *header = (const AuthoritativeDatagramPrefix *)bytes;
        if (std::memcmp(header->magic, kAuthoritativeStateDatagramMagic, sizeof(kAuthoritativeStateDatagramMagic)) == 0 &&
            header->version == kAuthoritativeStateDatagramVersion)
        {
            return RawDatagram_AuthoritativeState;
        }
    }
    if (size == (int)sizeof(Pack))
    {
        return RawDatagram_Pack;
    }
    return RawDatagram_Unknown;
}
} // namespace

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
    in_addr parsedIpv4 {};
    in6_addr parsedIpv6 {};
    const bool isNumericIpv4 = !ip.empty() && inet_pton(AF_INET, ip.c_str(), &parsedIpv4) == 1;
    const bool isNumericIpv6 = !ip.empty() && inet_pton(AF_INET6, ip.c_str(), &parsedIpv6) == 1;
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
        if (isNumericIpv4)
        {
            addr4->sin_addr = parsedIpv4;
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
        if (isNumericIpv6)
        {
            addr6->sin6_addr = parsedIpv6;
            outLen = sizeof(sockaddr_in6);
            return true;
        }
    }
    else
    {
        return false;
    }

    if (isNumericIpv4 || isNumericIpv6)
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
    return SendBytesTo(&pack, sizeof(pack), ip, port);
}

bool ConnectionBase::SendBytesTo(const void *data, size_t size, const std::string &ip, int port)
{
    if (data == nullptr || size == 0 || size > (size_t)kRelayMaxDatagramBytes)
    {
        return false;
    }

    const int families[2] = {m_family == AF_INET6 ? AF_INET6 : AF_INET, m_family == AF_INET6 ? AF_INET : AF_INET6};
    for (const int family : families)
    {
        sockaddr_storage storage {};
        socklen_t storageLen = 0;
        if (!IpPortToSockAddr(ip, port, family, storage, storageLen))
        {
            continue;
        }

        const int sent = (int)sendto(m_socket, (const char *)data, (int)size, 0, (sockaddr *)&storage, storageLen);
        if (sent == (int)size)
        {
            return true;
        }
    }
    return false;
}

bool ConnectionBase::ReceiveOnePack(Pack &outPack, std::string &fromIp, int &fromPort, bool &hasData)
{
    DatagramBuffer datagram;
    if (!ReceiveOneDatagram(datagram, hasData))
    {
        return false;
    }
    if (!hasData)
    {
        fromIp.clear();
        fromPort = 0;
        return true;
    }
    fromIp = datagram.fromIp;
    fromPort = datagram.fromPort;
    if (datagram.kind != RawDatagram_Pack || datagram.size != (int)sizeof(Pack))
    {
        hasData = false;
        return true;
    }
    std::memcpy(&outPack, datagram.bytes, sizeof(Pack));
    return true;
}

bool ConnectionBase::ReceiveOneDatagram(DatagramBuffer &outDatagram, bool &hasData)
{
    hasData = false;
    outDatagram = {};
    if (m_socket == kInvalidSocket)
    {
        return false;
    }

    sockaddr_storage fromAddr {};
    socklen_t fromLen = sizeof(fromAddr);
    const int received =
        (int)recvfrom(m_socket, (char *)outDatagram.bytes, sizeof(outDatagram.bytes), 0, (sockaddr *)&fromAddr, &fromLen);
    if (received < 0)
    {
        const int errorCode = GetLastSocketError();
        if (IsWouldBlockError(errorCode))
        {
            return true;
        }
        return false;
    }
    if (received == 0)
    {
        return true;
    }

    outDatagram.size = received;
    outDatagram.kind = ClassifyRawDatagram(outDatagram.bytes, outDatagram.size);
    hasData = SockAddrToIpPort((const sockaddr *)&fromAddr, fromLen, outDatagram.fromIp, outDatagram.fromPort);
    if (hasData)
    {
        g_State.lastPacketBytes = received;
        g_State.lastPacketFromIp = outDatagram.fromIp;
        g_State.lastPacketFromPort = outDatagram.fromPort;
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
    DatagramBuffer datagram;
    if (!PollDatagram(datagram, hasData))
    {
        return false;
    }
    if (hasData)
    {
        if (datagram.kind == RawDatagram_Pack && datagram.size == (int)sizeof(Pack))
        {
            std::memcpy(&outPack, datagram.bytes, sizeof(Pack));
        }
        else
        {
            hasData = false;
        }
    }
    return true;
}

bool HostConnection::PollDatagram(DatagramBuffer &outDatagram, bool &hasData)
{
    if (!ReceiveOneDatagram(outDatagram, hasData))
    {
        return false;
    }
    if (hasData)
    {
        m_guestIp = outDatagram.fromIp;
        m_guestPort = outDatagram.fromPort;
    }
    return true;
}

bool HostConnection::SendDatagram(const void *data, size_t size)
{
    return !m_guestIp.empty() && m_guestPort > 0 && SendBytesTo(data, size, m_guestIp, m_guestPort);
}

bool GuestConnection::PollReceive(Pack &outPack, bool &hasData)
{
    DatagramBuffer datagram;
    if (!PollDatagram(datagram, hasData))
    {
        return false;
    }
    if (hasData)
    {
        if (datagram.kind == RawDatagram_Pack && datagram.size == (int)sizeof(Pack))
        {
            std::memcpy(&outPack, datagram.bytes, sizeof(Pack));
        }
        else
        {
            hasData = false;
        }
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

bool GuestConnection::PollDatagram(DatagramBuffer &outDatagram, bool &hasData)
{
    if (!ReceiveOneDatagram(outDatagram, hasData))
    {
        return false;
    }
    return true;
}

bool GuestConnection::SendPack(const Pack &pack)
{
    return !m_hostIp.empty() && m_hostPort > 0 && SendPackTo(pack, m_hostIp, m_hostPort);
}

bool GuestConnection::SendDatagram(const void *data, size_t size)
{
    return !m_hostIp.empty() && m_hostPort > 0 && SendBytesTo(data, size, m_hostIp, m_hostPort);
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
    DatagramBuffer datagram;
    if (!PollDatagram(datagram, hasData))
    {
        return false;
    }
    if (hasData)
    {
        if (datagram.kind == RawDatagram_Pack && datagram.size == (int)sizeof(Pack))
        {
            std::memcpy(&outPack, datagram.bytes, sizeof(Pack));
        }
        else
        {
            hasData = false;
        }
    }
    return true;
}

bool RelayConnection::SendPack(const Pack &pack)
{
    return SendDatagram(&pack, sizeof(pack));
}

bool RelayConnection::PollDatagram(DatagramBuffer &outDatagram, bool &hasData)
{
    hasData = false;
    if (m_socket == kInvalidSocket)
    {
        return false;
    }

    while (true)
    {
        if (!ReceiveOneDatagram(outDatagram, hasData))
        {
            return false;
        }
        if (!hasData)
        {
            return true;
        }
        if (outDatagram.kind == RawDatagram_RelayText)
        {
            HandleControl(std::string((const char *)outDatagram.bytes, (const char *)outDatagram.bytes + outDatagram.size));
            continue;
        }
        return true;
    }
}

bool RelayConnection::SendDatagram(const void *data, size_t size)
{
    if (m_socket == kInvalidSocket || !m_serverAddrValid || data == nullptr || size == 0 ||
        size > (size_t)kRelayMaxDatagramBytes)
    {
        return false;
    }

    const int sent = (int)sendto(m_socket, (const char *)data, (int)size, 0, (sockaddr *)&m_serverAddr, m_serverAddrLen);
    return sent == (int)size;
}

bool RelayConnection::IsReady() const
{
    return m_isReady;
}

void ClearRuntimeCaches()
{
    TraceDiagnostic("clear-runtime-caches", "-");
    ClearDebugNetworkQueues();
    ResetConsistencyDebugState();
    ResetRecoveryHeuristicState();
    g_State.authoritativeFrameHashes.clear();
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
    g_State.localTouchData.clear();
    g_State.remoteTouchData.clear();
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
    g_State.lastRollbackSourceFrame = -1;
    g_State.lastLatentRetrySourceFrame = -1;
    g_State.seedValidationIgnoreUntilFrame = -1;
    g_State.lastSeedRetryMismatchFrame = -1;
    g_State.lastSeedRetryOlderThanSnapshotFrame = -1;
    g_State.lastSeedRetryLocalSeed = 0;
    g_State.lastSeedRetryRemoteSeed = 0;
    g_State.sessionRngSeed = 0;
    g_State.sharedRngSeedCaptured = false;
    g_State.lastConfirmedSyncFrame = 0;
    g_State.currentCtrl = IGC_NONE;
    g_State.hasKnownUiState = false;
    g_State.knownUiState = false;
    g_State.localUiPhaseSerial = 0;
    g_State.remoteUiPhaseSerial = 0;
    g_State.pendingUiPhaseSerial = 0;
    g_State.remoteUiPhaseIsInUi = false;
    g_State.pendingUiPhaseIsInUi = false;
    g_State.awaitingUiPhaseAck = false;
    g_State.lastUiPhaseSendTick = 0;
    g_State.broadcastUiPhaseSerial = 0;
    g_State.broadcastUiPhaseIsInUi = false;
    g_State.broadcastUiPhaseUntilTick = 0;
    g_State.lastUiPhaseBroadcastTick = 0;
    ResetSharedShellState();
    ResetAuthoritativeRecoveryState();
    g_State.pendingDebugEndingJump = false;
    g_State.reconnectStartTick = 0;
    g_State.desyncStartTick = 0;
    g_State.reconnectReason = AuthoritativeRecoveryReason_None;
    // GCC ICE on `= AuthoritativeFrameState{}` for this large struct (gimplify
    // crashes on the brace-init temporary). Reconstruct in-place to avoid it.
    g_State.latestAuthoritativeFrameState.~AuthoritativeFrameState();
    new (&g_State.latestAuthoritativeFrameState) AuthoritativeFrameState();
    g_State.lastAuthoritativeHashComparedFrame = -1;
    g_State.authoritativeHashMismatchPending = false;
    g_State.authoritativeHashCheckEnabled = false;
}

void ClearRemoteRuntimeCaches()
{
    TraceDiagnostic("clear-remote-runtime-caches", "-");
    ClearDebugNetworkQueues();
    ResetConsistencyDebugState();
    ResetRecoveryHeuristicState();
    g_State.authoritativeFrameHashes.clear();
    g_State.remoteInputs.clear();
    g_State.predictedRemoteInputs.clear();
    g_State.remoteSeeds.clear();
    g_State.remoteCtrls.clear();
    g_State.predictedRemoteCtrls.clear();
    g_State.remoteFramesPendingRollbackCheck.clear();
    g_State.remoteTouchData.clear();
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
    g_State.lastRollbackSourceFrame = -1;
    g_State.lastLatentRetrySourceFrame = -1;
    g_State.seedValidationIgnoreUntilFrame = -1;
    g_State.lastSeedRetryMismatchFrame = -1;
    g_State.latestAuthoritativeFrameState.~AuthoritativeFrameState();
    new (&g_State.latestAuthoritativeFrameState) AuthoritativeFrameState();
    g_State.lastAuthoritativeHashComparedFrame = -1;
    g_State.authoritativeHashMismatchPending = false;
    g_State.lastSeedRetryOlderThanSnapshotFrame = -1;
    g_State.lastSeedRetryLocalSeed = 0;
    g_State.lastSeedRetryRemoteSeed = 0;
    g_State.resyncTriggered = false;
    g_State.resyncTargetFrame = 0;
    g_State.remoteUiPhaseSerial = 0;
    g_State.pendingUiPhaseSerial = 0;
    g_State.remoteUiPhaseIsInUi = false;
    g_State.pendingUiPhaseIsInUi = false;
    g_State.awaitingUiPhaseAck = false;
    g_State.lastUiPhaseSendTick = 0;
    g_State.broadcastUiPhaseSerial = 0;
    g_State.broadcastUiPhaseIsInUi = false;
    g_State.broadcastUiPhaseUntilTick = 0;
    g_State.lastUiPhaseBroadcastTick = 0;
    ResetSharedShellState();
    ResetAuthoritativeRecoveryState();
    g_State.pendingDebugEndingJump = false;
    g_State.reconnectStartTick = 0;
    g_State.desyncStartTick = 0;
    g_State.reconnectReason = AuthoritativeRecoveryReason_None;
}

void ResetGameplayRuntimeStream()
{
    TraceDiagnostic("reset-gameplay-runtime-stream", "oldFrame=%d oldNet=%d", g_State.lastFrame, g_State.currentNetFrame);

    // Keep ui-phase serial monotonic across continue/restart gameplay stream resets.
    // Reusing low serials (e.g. restarting from 1) can make delayed/stale ui-phase
    // packets from the previous stream look valid and falsely satisfy the barrier.
    const int nextUiSerialBase = std::max(g_State.localUiPhaseSerial, g_State.remoteUiPhaseSerial) + 1;
    const u16 nextShellSerialBase = std::max<u16>(1, std::max(g_State.shell.nextShellSerial,
                                                              (u16)(g_State.shell.shellSerial + 1)));

    ClearRuntimeCaches();
    // Frame-0 rollback divergence was starting from menu carry-over: legacy input
    // edge state and held-device state were surviving the UI -> gameplay boundary,
    // so the very first gameplay frame could already disagree in player/stage hashes.
    Controller::ResetDeviceInputState();
    Session::ResetLegacyInputState();
    g_State.localUiPhaseSerial = nextUiSerialBase;
    g_State.remoteUiPhaseSerial = nextUiSerialBase;
    g_State.shell.nextShellSerial = nextShellSerialBase;

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

bool OpenRelayProbeSocket(int preferredFamily)
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

    if (preferredFamily == AF_INET6)
    {
        return tryOpen(AF_INET6) || tryOpen(AF_INET);
    }
    if (preferredFamily == AF_INET)
    {
        return tryOpen(AF_INET) || tryOpen(AF_INET6);
    }
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
    if (g_Relay.resolvedAddrValid)
    {
        storage = g_Relay.resolvedAddr;
        storageLen = g_Relay.resolvedAddrLen;
    }
    else if (!ResolveIpPortToSockAddr(g_Relay.host, g_Relay.port, g_Relay.family, storage, storageLen))
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

bool SendPacketImmediate(const Pack &pack)
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

bool SendDatagramImmediate(const void *data, size_t size)
{
    if (g_State.useRelayTransport)
    {
        return g_State.relay.SendDatagram(data, size);
    }
    if (UsingHostConnection())
    {
        return g_State.host.SendDatagram(data, size);
    }
    return g_State.guest.SendDatagram(data, size);
}

bool PollPacketImmediate(Pack &pack, bool &hasData)
{
    hasData = false;

    while (true)
    {
        DatagramBuffer datagram;
        bool datagramHasData = false;
        const bool ok = g_State.useRelayTransport ? g_State.relay.PollDatagram(datagram, datagramHasData)
                                                  : (UsingHostConnection() ? g_State.host.PollDatagram(datagram, datagramHasData)
                                                                           : g_State.guest.PollDatagram(datagram, datagramHasData));
        if (!ok || !datagramHasData)
        {
            return ok;
        }

        if (datagram.kind == RawDatagram_SnapshotSideband)
        {
            if (datagram.size < (int)sizeof(SnapshotDatagramHeader))
            {
                continue;
            }
            const SnapshotDatagramHeader *header = (const SnapshotDatagramHeader *)datagram.bytes;
            const int payloadBytes = datagram.size - (int)sizeof(SnapshotDatagramHeader);
            if (payloadBytes < 0 || payloadBytes != (int)header->payloadBytes)
            {
                continue;
            }
            HandleSnapshotSidebandDatagram(*header, datagram.bytes + sizeof(SnapshotDatagramHeader), payloadBytes);
            continue;
        }

        if (datagram.kind == RawDatagram_ConsistencySideband)
        {
            HandleConsistencySidebandDatagram(datagram.bytes, datagram.size);
            continue;
        }

        if (datagram.kind == RawDatagram_AuthoritativeState)
        {
            HandleAuthoritativeStateDatagramBytes(datagram.bytes, datagram.size);
            continue;
        }

        if (datagram.kind != RawDatagram_Pack || datagram.size != (int)sizeof(Pack))
        {
            continue;
        }

        std::memcpy(&pack, datagram.bytes, sizeof(Pack));
        hasData = true;
        return true;
    }
}

void ClearDebugNetworkQueues()
{
    g_State.delayedOutgoingPacks.clear();
    g_State.delayedIncomingPacks.clear();
}

bool IsDebugNetworkSimulationEnabled()
{
    return g_State.debugNetworkConfig.enabled;
}

Uint32 NextDebugNetworkRandom()
{
    g_State.debugRandomState = g_State.debugRandomState * 1664525u + 1013904223u;
    return g_State.debugRandomState;
}

bool DebugNetworkRollPercent(int percent)
{
    if (percent <= 0)
    {
        return false;
    }
    if (percent >= 100)
    {
        return true;
    }
    return (NextDebugNetworkRandom() % 100u) < (Uint32)percent;
}

int ComputeDebugNetworkDelayMs()
{
    const DebugNetworkConfig &config = g_State.debugNetworkConfig;
    int delayMs = std::max(0, config.latencyMs);
    const int jitterMs = std::max(0, config.jitterMs);
    if (jitterMs > 0)
    {
        const int span = jitterMs * 2 + 1;
        delayMs += (int)(NextDebugNetworkRandom() % (Uint32)span) - jitterMs;
    }
    return std::max(0, delayMs);
}

bool FlushDebugOutgoingPackets()
{
    if (g_State.delayedOutgoingPacks.empty())
    {
        return true;
    }

    const Uint64 now = SDL_GetTicks64();
    bool allSucceeded = true;
    for (std::size_t i = 0; i < g_State.delayedOutgoingPacks.size();)
    {
        if (g_State.delayedOutgoingPacks[i].releaseTick > now)
        {
            ++i;
            continue;
        }

        if (!SendPacketImmediate(g_State.delayedOutgoingPacks[i].pack))
        {
            allSucceeded = false;
        }
        g_State.delayedOutgoingPacks.erase(g_State.delayedOutgoingPacks.begin() + (std::ptrdiff_t)i);
    }
    return allSucceeded;
}

bool PopDueDebugIncomingPacket(Pack &pack)
{
    if (g_State.delayedIncomingPacks.empty())
    {
        return false;
    }

    const Uint64 now = SDL_GetTicks64();
    std::size_t bestIndex = g_State.delayedIncomingPacks.size();
    Uint64 bestTick = 0;
    for (std::size_t i = 0; i < g_State.delayedIncomingPacks.size(); ++i)
    {
        const DelayedPack &candidate = g_State.delayedIncomingPacks[i];
        if (candidate.releaseTick > now)
        {
            continue;
        }
        if (bestIndex == g_State.delayedIncomingPacks.size() || candidate.releaseTick < bestTick)
        {
            bestIndex = i;
            bestTick = candidate.releaseTick;
        }
    }

    if (bestIndex == g_State.delayedIncomingPacks.size())
    {
        return false;
    }

    pack = g_State.delayedIncomingPacks[bestIndex].pack;
    g_State.delayedIncomingPacks.erase(g_State.delayedIncomingPacks.begin() + (std::ptrdiff_t)bestIndex);
    return true;
}

void QueueDebugIncomingPacket(const Pack &pack)
{
    if (DebugNetworkRollPercent(g_State.debugNetworkConfig.packetLossPercent))
    {
        return;
    }

    const Uint64 now = SDL_GetTicks64();
    const int copies = DebugNetworkRollPercent(g_State.debugNetworkConfig.duplicatePercent) ? 2 : 1;
    for (int i = 0; i < copies; ++i)
    {
        DelayedPack delayedPack;
        delayedPack.pack = pack;
        delayedPack.releaseTick = now + (Uint64)ComputeDebugNetworkDelayMs();
        g_State.delayedIncomingPacks.push_back(delayedPack);
    }
}

bool SendPacket(const Pack &pack)
{
    if (!FlushDebugOutgoingPackets())
    {
        return false;
    }

    if (!IsDebugNetworkSimulationEnabled())
    {
        return SendPacketImmediate(pack);
    }

    if (DebugNetworkRollPercent(g_State.debugNetworkConfig.packetLossPercent))
    {
        return true;
    }

    const Uint64 now = SDL_GetTicks64();
    const int copies = DebugNetworkRollPercent(g_State.debugNetworkConfig.duplicatePercent) ? 2 : 1;
    for (int i = 0; i < copies; ++i)
    {
        const int delayMs = ComputeDebugNetworkDelayMs();
        if (delayMs <= 0)
        {
            if (!SendPacketImmediate(pack))
            {
                return false;
            }
            continue;
        }

        DelayedPack delayedPack;
        delayedPack.pack = pack;
        delayedPack.releaseTick = now + (Uint64)delayMs;
        g_State.delayedOutgoingPacks.push_back(delayedPack);
    }
    return true;
}

bool PollPacket(Pack &pack, bool &hasData)
{
    hasData = false;

    if (!FlushDebugOutgoingPackets())
    {
        return false;
    }

    if (IsDebugNetworkSimulationEnabled() && PopDueDebugIncomingPacket(pack))
    {
        hasData = true;
        return true;
    }

    if (!IsDebugNetworkSimulationEnabled())
    {
        return PollPacketImmediate(pack, hasData);
    }

    while (true)
    {
        Pack immediatePack;
        bool immediateHasData = false;
        if (!PollPacketImmediate(immediatePack, immediateHasData))
        {
            return false;
        }
        if (!immediateHasData)
        {
            break;
        }

        QueueDebugIncomingPacket(immediatePack);
        if (PopDueDebugIncomingPacket(pack))
        {
            hasData = true;
            return true;
        }
    }

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

bool TryActivateStartupFromRuntimePacket(const Pack &pack)
{
    if (!g_State.isWaitingForStartup || g_State.isSessionActive)
    {
        return false;
    }
    if (pack.type != PACK_USUAL || pack.ctrl.ctrlType != Ctrl_Key)
    {
        return false;
    }

    if (!g_State.startupInitSettingReceived)
    {
        TraceDiagnostic("startup-activate-wait-init-setting",
                        "frame=%d seq=%u role=%s authoritativeLocal=%d waitingForNegotiation=1", pack.ctrl.frame,
                        pack.seq, g_State.isHost ? "host" : (g_State.isGuest ? "guest" : "idle"),
                        g_State.authoritativeModeEnabled ? 1 : 0);
        return false;
    }

    const u16 peerSharedSeed = pack.ctrl.rngSeed[0];
    if (!TryAdoptStartupAuthoritySeedFromPeer(peerSharedSeed, "runtime-packet"))
    {
        return false;
    }
    if (!g_State.authoritySharedSeedKnown)
    {
        TraceDiagnostic("startup-activate-wait-seed-confirm",
                        "frame=%d seq=%u peerSharedSeed=%u authorityKnown=%d", pack.ctrl.frame, pack.seq,
                        peerSharedSeed, g_State.authoritySharedSeedKnown ? 1 : 0);
        return false;
    }
    TraceDiagnostic("startup-activate-from-runtime-packet",
                    "frame=%d seq=%u peerSharedSeed=%u authoritySeed=%u", pack.ctrl.frame, pack.seq, peerSharedSeed,
                    g_State.authoritySharedSeed);
    ActivateNetplaySession(g_State.authoritySharedSeed, true);
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
            reply.ctrl.initSetting.flags = (g_State.predictionRollbackEnabled ? InitSettingFlag_PredictionRollback : 0) |
                                           (g_State.authoritativeModeEnabled ? InitSettingFlag_AuthoritativeMode : 0);
            reply.ctrl.initSetting.sharedSeed =
                NormalizeStartupSharedSeed(g_State.authoritySharedSeedKnown ? g_State.authoritySharedSeed : g_Rng.seed);
            reply.ctrl.initSetting.hostIsPlayer1 = g_State.hostIsPlayer1 ? 1 : 0;
            reply.ctrl.initSetting.difficulty = (u8)g_GameManager.difficulty;
            reply.ctrl.initSetting.character1 = g_GameManager.character;
            reply.ctrl.initSetting.shotType1 = g_GameManager.shotType;
            reply.ctrl.initSetting.character2 = g_GameManager.character2;
            reply.ctrl.initSetting.shotType2 = g_GameManager.shotType2;
            reply.ctrl.initSetting.practiceMode = g_GameManager.isInPracticeMode ? 1 : 0;
            std::memset(reply.ctrl.initSetting.reserved, 0, sizeof(reply.ctrl.initSetting.reserved));
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
        else if (pack.ctrl.ctrlType == Ctrl_Debug_EndingJump)
        {
            g_State.pendingDebugEndingJump = true;
            TraceDiagnostic("recv-debug-ending-jump", "source=ping seq=%u", pack.seq);
        }
        else if (pack.ctrl.ctrlType == Ctrl_Set_InitSetting)
        {
            ApplyStartupInitSettingsFromPeer(pack.ctrl, "ping");
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
        else if (pack.ctrl.ctrlType == Ctrl_Debug_EndingJump)
        {
            g_State.pendingDebugEndingJump = true;
            TraceDiagnostic("recv-debug-ending-jump", "source=pong seq=%u", pack.seq);
        }
        else if (pack.ctrl.ctrlType == Ctrl_Set_InitSetting)
        {
            ApplyStartupInitSettingsFromPeer(pack.ctrl, "pong");
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
        if (TryActivateStartupFromRuntimePacket(pack))
        {
            return;
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
        if (TryActivateStartupFromRuntimePacket(pack))
        {
            return;
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

void StoreRemoteKeyPacket(const Pack &pack, u16 sharedRngSeed, bool captureShared)
{
    TraceDiagnostic("recv-key-packet", "frame=%d seq=%u window=%s", pack.ctrl.frame, pack.seq,
                    BuildCtrlPacketWindowSummary(pack.ctrl, 4).c_str());
    if (captureShared && g_State.isWaitingForStartup && !g_State.isSessionActive)
    {
        TryAdoptStartupAuthoritySeedFromPeer(sharedRngSeed, "store-remote-key");
    }
    if (g_State.isSessionActive && !g_State.isWaitingForStartup)
    {
        const int currentFrame = CurrentNetFrame();
        const int minAcceptedFrame = std::max(0, currentFrame - kFrameCacheSize);
        const int maxAcceptedFrame = currentFrame + kFrameCacheSize;
        if (pack.ctrl.frame < minAcceptedFrame)
        {
            TraceDiagnostic("recv-key-packet-drop-stale", "frame=%d current=%d minAccepted=%d", pack.ctrl.frame,
                            currentFrame, minAcceptedFrame);
            return;
        }
        if (pack.ctrl.frame > maxAcceptedFrame)
        {
            TraceDiagnostic("recv-key-packet-drop-future", "frame=%d current=%d maxAccepted=%d", pack.ctrl.frame,
                            currentFrame, maxAcceptedFrame);
            return;
        }
    }

    if (g_State.authoritativeModeEnabled)
    {
        for (int i = 0; i < kKeyPackFrameCount; ++i)
        {
            const int frame = pack.ctrl.frame - i;
            const Bits<16> actualBits = pack.ctrl.keys[i];
            const InGameCtrlType actualCtrl = pack.ctrl.inGameCtrl[i];
            const u16 actualSeed = pack.ctrl.rngSeed[i];
            g_State.remoteInputs[frame] = actualBits;
            g_State.remoteSeeds[frame] = actualSeed;
            g_State.remoteCtrls[frame] = actualCtrl;
            g_State.remoteTouchData[frame] = pack.ctrl.touchData[i];
            TraceDiagnostic("recv-key-frame-commit-authoritative", "frame=%d input=%s seed=%u ctrl=%s", frame,
                            FormatInputBits(WriteToInt(actualBits)).c_str(), actualSeed,
                            InGameCtrlToString(actualCtrl));
        }
        AdvanceConfirmedSyncFrame();
        return;
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
        g_State.remoteTouchData[frame] = pack.ctrl.touchData[i];

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
            const bool lateAuthoritativeArrival =
                existingInputIt == g_State.remoteInputs.end() || existingSeedIt == g_State.remoteSeeds.end() ||
                existingCtrlIt == g_State.remoteCtrls.end();
            const bool inSeedValidationGrace = frame <= g_State.seedValidationIgnoreUntilFrame;
            if (lateAuthoritativeArrival)
            {
                if (inSeedValidationGrace)
                {
                    TraceDiagnostic("recv-key-frame-seed-mismatch",
                                    "frame=%d input=%s localSeed=%u remoteSeed=%u ctrl=%s ignored=1 reason=rollback-grace "
                                    "graceUntil=%d",
                                    frame, FormatInputBits(WriteToInt(actualBits)).c_str(), localSeedIt->second,
                                    actualSeed, InGameCtrlToString(actualCtrl), g_State.seedValidationIgnoreUntilFrame);
                }
                else
                {
                    queuedRollback = QueueRollbackFromFrame(frame, "recv-late-authoritative");
                    TraceDiagnostic(
                        "recv-key-frame-seed-mismatch",
                        "frame=%d input=%s localSeed=%u remoteSeed=%u ctrl=%s queued=%d reason=late-authoritative",
                        frame, FormatInputBits(WriteToInt(actualBits)).c_str(), localSeedIt->second, actualSeed,
                        InGameCtrlToString(actualCtrl), queuedRollback ? 1 : 0);
                }
            }
            else
            {
                TraceDiagnostic("recv-key-frame-seed-mismatch",
                                "frame=%d input=%s localSeed=%u remoteSeed=%u ctrl=%s ignored=1", frame,
                                FormatInputBits(WriteToInt(actualBits)).c_str(), localSeedIt->second, actualSeed,
                                InGameCtrlToString(actualCtrl));
            }
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

bool TryActivateFromStartupPacket(u16 *outPeerSharedSeed)
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
        if (TryActivateStartupFromRuntimePacket(pack))
        {
            return true;
        }
        if (pack.type != PACK_USUAL || pack.ctrl.ctrlType != Ctrl_Key)
        {
            continue;
        }

        const u16 peerSharedSeed = pack.ctrl.rngSeed[0];
        if (!TryAdoptStartupAuthoritySeedFromPeer(peerSharedSeed, "startup-packet"))
        {
            if (!g_State.isWaitingForStartup || g_State.isSessionActive)
            {
                return false;
            }
            continue;
        }
        if (!g_State.startupInitSettingReceived)
        {
            continue;
        }
        if (!g_State.authoritySharedSeedKnown)
        {
            continue;
        }
        if (outPeerSharedSeed)
        {
            *outPeerSharedSeed = g_State.authoritySharedSeed;
        }
        TraceDiagnostic("startup-activate-from-packet", "seq=%u peerSharedSeed=%u authoritySeed=%u", pack.seq,
                        peerSharedSeed, g_State.authoritySharedSeed);
        ActivateNetplaySession(g_State.authoritySharedSeed, true);
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
            TraceDiagnostic("recv-runtime-packet-skip", "type=%d ctrl=%s seq=%u", pack.type,
                            ControlToString(pack.ctrl.ctrlType), pack.seq);
            continue;
        }
        if (pack.ctrl.ctrlType == Ctrl_Key)
        {
            const Uint64 receiveTick = SDL_GetTicks64();
            const int previousLatestRemoteFrame = LatestRemoteReceivedFrame();
            StoreRemoteKeyPacket(pack);
            const int latestRemoteFrame = LatestRemoteReceivedFrame();
            if (pack.ctrl.frame >= previousLatestRemoteFrame && latestRemoteFrame >= previousLatestRemoteFrame)
            {
                g_State.lastRuntimeReceiveTick = receiveTick;
            }
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
        else if (pack.ctrl.ctrlType == Ctrl_UiPhase)
        {
            const Uint64 receiveTick = SDL_GetTicks64();
            g_State.lastRuntimeReceiveTick = receiveTick;
            if (pack.ctrl.uiPhase.serial >= g_State.remoteUiPhaseSerial)
            {
                g_State.remoteUiPhaseSerial = pack.ctrl.uiPhase.serial;
                g_State.remoteUiPhaseIsInUi = (pack.ctrl.uiPhase.flags & UiPhaseFlag_InUi) != 0;
            }
            TraceDiagnostic("recv-ui-phase", "serial=%d ui=%d frame=%d remoteSerial=%d", pack.ctrl.uiPhase.serial,
                            (pack.ctrl.uiPhase.flags & UiPhaseFlag_InUi) != 0 ? 1 : 0, pack.ctrl.uiPhase.boundaryFrame,
                            g_State.remoteUiPhaseSerial);
        }
        else if (pack.ctrl.ctrlType == Ctrl_ShellIntent)
        {
            HandleSharedShellIntentPacket(pack.ctrl);
        }
        else if (pack.ctrl.ctrlType == Ctrl_ShellState)
        {
            HandleSharedShellStatePacket(pack.ctrl);
        }
        else if (pack.ctrl.ctrlType == Ctrl_Debug_EndingJump)
        {
            g_State.lastRuntimeReceiveTick = SDL_GetTicks64();
            g_State.pendingDebugEndingJump = true;
            TraceDiagnostic("recv-debug-ending-jump", "source=runtime seq=%u", pack.seq);
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
    // During rollback startup wait, the host is the only authority allowed to advertise
    // a real shared seed immediately. Guests must first learn the peer init-setting;
    // otherwise the host can activate early while the guest is still missing startup
    // configuration, leaving the two sides in different startup states.
    const bool canAdvertiseAuthoritySeed =
        g_State.isHost || (g_State.authoritySharedSeedKnown && g_State.startupInitSettingReceived);
    const u16 startupSeed = canAdvertiseAuthoritySeed
                                ? NormalizeStartupSharedSeed(g_State.authoritySharedSeedKnown
                                                                 ? g_State.authoritySharedSeed
                                                                 : g_Rng.seed)
                                : 0;

    g_State.localInputs[frame] = zeroBits;
    g_State.localSeeds[frame] = startupSeed;
    g_State.localCtrls[frame] = IGC_NONE;
    PruneOldFrameData(frame);
    TraceDiagnostic("send-startup-bootstrap", "role=%s seed=%u authorityKnown=%d initSetting=%d", g_State.isHost ? "host" : "guest",
                    startupSeed, g_State.authoritySharedSeedKnown ? 1 : 0, g_State.startupInitSettingReceived ? 1 : 0);
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

        const auto touchIt = g_State.localTouchData.find(srcFrame);
        if (touchIt != g_State.localTouchData.end())
        {
            pack.ctrl.touchData[i] = touchIt->second;
        }
        else
        {
            pack.ctrl.touchData[i].Clear();
        }
    }
    TraceDiagnostic("send-key-packet", "frame=%d window=%s", frame, BuildCtrlPacketWindowSummary(pack.ctrl, 4).c_str());
    SendPacket(pack);
}

void ApplyRemoteTouchForCurrentFrame(int frame)
{
    const auto it = g_State.remoteTouchData.find(frame);
    if (it == g_State.remoteTouchData.end())
        return;

    const TouchFrameData &td = it->second;
    if (td.flags == 0)
        return;

    AndroidTouchInput::ApplyRemoteTouchFrameData(td);
    TraceDiagnostic("apply-remote-touch", "frame=%d flags=0x%02x tap=(%.1f,%.1f) swX=%.2f swY=%.2f analog=(%d,%d)",
                    frame, td.flags, td.tapGameX, td.tapGameY, td.swipeXDelta, td.swipeYDelta,
                    (int)td.analogX, (int)td.analogY);
}


} // namespace th06::Netplay
