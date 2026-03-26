#include "NetplayInternal.hpp"

namespace th06::Netplay
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
    ClearDebugNetworkQueues();
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
    g_State.pendingDebugEndingJump = false;
}

void ClearRemoteRuntimeCaches()
{
    TraceDiagnostic("clear-remote-runtime-caches", "-");
    ClearDebugNetworkQueues();
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
    g_State.lastRollbackSourceFrame = -1;
    g_State.lastLatentRetrySourceFrame = -1;
    g_State.seedValidationIgnoreUntilFrame = -1;
    g_State.lastSeedRetryMismatchFrame = -1;
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
    g_State.pendingDebugEndingJump = false;
}

void ResetGameplayRuntimeStream()
{
    TraceDiagnostic("reset-gameplay-runtime-stream", "oldFrame=%d oldNet=%d", g_State.lastFrame, g_State.currentNetFrame);

    // Keep ui-phase serial monotonic across continue/restart gameplay stream resets.
    // Reusing low serials (e.g. restarting from 1) can make delayed/stale ui-phase
    // packets from the previous stream look valid and falsely satisfy the barrier.
    const int nextUiSerialBase = std::max(g_State.localUiPhaseSerial, g_State.remoteUiPhaseSerial) + 1;

    ClearRuntimeCaches();
    g_State.localUiPhaseSerial = nextUiSerialBase;
    g_State.remoteUiPhaseSerial = nextUiSerialBase;

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

bool PollPacketImmediate(Pack &pack, bool &hasData)
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

    const u16 peerSharedSeed = pack.ctrl.rngSeed[0];
    StoreRemoteKeyPacket(pack, peerSharedSeed, true);
    TraceDiagnostic("startup-activate-from-runtime-packet", "frame=%d seq=%u peerSharedSeed=%u", pack.ctrl.frame, pack.seq,
                    peerSharedSeed);
    ActivateNetplaySession(peerSharedSeed, true);
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
        else if (pack.ctrl.ctrlType == Ctrl_Debug_EndingJump)
        {
            g_State.pendingDebugEndingJump = true;
            TraceDiagnostic("recv-debug-ending-jump", "source=ping seq=%u", pack.seq);
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
        else if (pack.ctrl.ctrlType == Ctrl_Debug_EndingJump)
        {
            g_State.pendingDebugEndingJump = true;
            TraceDiagnostic("recv-debug-ending-jump", "source=pong seq=%u", pack.seq);
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
    if (captureShared && sharedRngSeed != 0 && !g_State.sharedRngSeedCaptured)
    {
        g_State.sessionRngSeed = sharedRngSeed;
        g_State.sharedRngSeedCaptured = true;
        TraceDiagnostic("recv-key-packet-shared-seed-capture", "peerSeed=%u", sharedRngSeed);
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
            const bool lateAuthoritativeArrival =
                existingInputIt == g_State.remoteInputs.end() || existingSeedIt == g_State.remoteSeeds.end() ||
                existingCtrlIt == g_State.remoteCtrls.end();
            if (lateAuthoritativeArrival)
            {
                queuedRollback = QueueRollbackFromFrame(frame, "recv-late-authoritative");
                TraceDiagnostic("recv-key-frame-seed-mismatch",
                                "frame=%d input=%s localSeed=%u remoteSeed=%u ctrl=%s queued=%d reason=late-authoritative",
                                frame, FormatInputBits(WriteToInt(actualBits)).c_str(), localSeedIt->second, actualSeed,
                                InGameCtrlToString(actualCtrl), queuedRollback ? 1 : 0);
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
        StoreRemoteKeyPacket(pack, peerSharedSeed, true);
        if (outPeerSharedSeed)
        {
            *outPeerSharedSeed = peerSharedSeed;
        }
        ActivateNetplaySession(peerSharedSeed, true);
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


} // namespace th06::Netplay
