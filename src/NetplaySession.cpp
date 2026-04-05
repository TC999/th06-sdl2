#include "NetplayInternal.hpp"

#include "AstroBot.hpp"

namespace th06::Netplay
{
RuntimeState g_State;

RelayState g_Relay;

NetSession g_NetSession;

namespace
{
struct AsyncResolveOutcome
{
    AsyncResolveKind kind = AsyncResolve_None;
    uint32_t token = 0;
    bool success = false;
    std::string originalHost;
    int port = 0;
    int preferredFamily = AF_UNSPEC;
    int localPort = 0;
    bool relayIsHost = false;
    std::string roomCode;
    std::string errorText;
    std::vector<AsyncResolveCandidate> candidates;
};

bool IsNumericHostLiteral(const std::string &host)
{
    if (host.empty())
    {
        return false;
    }

    in_addr addr4 {};
    if (inet_pton(AF_INET, host.c_str(), &addr4) == 1)
    {
        return true;
    }

    in6_addr addr6 {};
    return inet_pton(AF_INET6, host.c_str(), &addr6) == 1;
}

std::string BuildEndpointTextFromIp(const std::string &ip, int port)
{
    if (ip.find(':') != std::string::npos)
    {
        return "[" + ip + "]:" + std::to_string(port);
    }
    return ip + ":" + std::to_string(port);
}

void CancelAsyncResolveState(AsyncResolveState &state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.token++;
    state.active = false;
    state.ready = false;
    state.success = false;
    state.startTick = 0;
    state.kind = AsyncResolve_None;
    state.originalHost.clear();
    state.port = 0;
    state.preferredFamily = AF_UNSPEC;
    state.localPort = 0;
    state.relayIsHost = false;
    state.roomCode.clear();
    state.errorText.clear();
    state.candidates.clear();
}

void StartAsyncResolveState(AsyncResolveState &state, AsyncResolveKind kind, const std::string &host, int port,
                            int preferredFamily, int localPort, bool relayIsHost, const std::string &roomCode)
{
    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.token++;
        token = state.token;
        state.kind = kind;
        state.active = true;
        state.ready = false;
        state.success = false;
        state.startTick = SDL_GetTicks64();
        state.originalHost = host;
        state.port = port;
        state.preferredFamily = preferredFamily;
        state.localPort = localPort;
        state.relayIsHost = relayIsHost;
        state.roomCode = roomCode;
        state.errorText.clear();
        state.candidates.clear();
    }

    std::thread([statePtr = &state, token, host, port, preferredFamily]() {
        std::vector<AsyncResolveCandidate> candidates;
        std::string errorText;
        bool socketReady = SocketSystem::Acquire();
        if (!socketReady)
        {
            errorText = "socket init failed";
        }

        addrinfo hints {};
        hints.ai_family = preferredFamily == AF_INET || preferredFamily == AF_INET6 ? preferredFamily : AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
#ifdef AI_V4MAPPED
        if (hints.ai_family == AF_INET6 || hints.ai_family == AF_UNSPEC)
        {
            hints.ai_flags |= AI_V4MAPPED;
        }
#endif
        char portText[16] = {};
        std::snprintf(portText, sizeof(portText), "%d", port);

        addrinfo *result = nullptr;
        int gaiStatus = 0;
        if (socketReady)
        {
            gaiStatus = getaddrinfo(host.c_str(), portText, &hints, &result);
        }
        if (socketReady && (gaiStatus != 0 || result == nullptr))
        {
#ifdef _WIN32
            errorText = gaiStatus != 0 ? ("resolve failed:" + std::to_string(gaiStatus)) : "resolve failed";
#else
            errorText = gaiStatus != 0 ? std::string("resolve failed:") + gai_strerror(gaiStatus) : "resolve failed";
#endif
        }
        else
        {
            auto appendCandidate = [&](const addrinfo *entry) {
                if (entry->ai_addr == nullptr || entry->ai_addrlen > sizeof(sockaddr_storage))
                {
                    return;
                }
                AsyncResolveCandidate candidate;
                candidate.family = entry->ai_family;
                std::memcpy(&candidate.addr, entry->ai_addr, entry->ai_addrlen);
                candidate.addrLen = (socklen_t)entry->ai_addrlen;
                int resolvedPort = 0;
                if (!TrySockAddrToIpPort(entry->ai_addr, (socklen_t)entry->ai_addrlen, candidate.ip, resolvedPort))
                {
                    return;
                }
                for (const AsyncResolveCandidate &existing : candidates)
                {
                    if (existing.family == candidate.family && existing.ip == candidate.ip)
                    {
                        return;
                    }
                }
                candidates.push_back(candidate);
            };

            for (const addrinfo *entry = result; entry != nullptr; entry = entry->ai_next)
            {
                if (entry->ai_family == AF_INET6)
                {
                    appendCandidate(entry);
                }
            }
            for (const addrinfo *entry = result; entry != nullptr; entry = entry->ai_next)
            {
                if (entry->ai_family == AF_INET)
                {
                    appendCandidate(entry);
                }
            }
            freeaddrinfo(result);
            if (candidates.empty())
            {
                errorText = "resolve failed";
            }
        }

        if (socketReady)
        {
            SocketSystem::Release();
        }

        std::lock_guard<std::mutex> lock(statePtr->mutex);
        if (statePtr->token != token || !statePtr->active)
        {
            return;
        }
        statePtr->ready = true;
        statePtr->success = !candidates.empty();
        statePtr->errorText = !errorText.empty() ? errorText : "";
        statePtr->candidates = std::move(candidates);
    }).detach();
}

bool TryTakeAsyncResolveOutcome(AsyncResolveState &state, AsyncResolveOutcome &outcome)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.active || !state.ready)
    {
        return false;
    }

    outcome.kind = state.kind;
    outcome.token = state.token;
    outcome.success = state.success;
    outcome.originalHost = state.originalHost;
    outcome.port = state.port;
    outcome.preferredFamily = state.preferredFamily;
    outcome.localPort = state.localPort;
    outcome.relayIsHost = state.relayIsHost;
    outcome.roomCode = state.roomCode;
    outcome.errorText = state.errorText;
    outcome.candidates = state.candidates;

    state.active = false;
    state.ready = false;
    state.success = false;
    state.kind = AsyncResolve_None;
    state.startTick = 0;
    state.originalHost.clear();
    state.port = 0;
    state.preferredFamily = AF_UNSPEC;
    state.localPort = 0;
    state.relayIsHost = false;
    state.roomCode.clear();
    state.errorText.clear();
    state.candidates.clear();
    return true;
}

void TraceAsyncResolveOutcome(const char *event, const AsyncResolveOutcome &outcome)
{
    const char *kind = "unknown";
    switch (outcome.kind)
    {
    case AsyncResolve_RelayProbe:
        kind = "relay-probe";
        break;
    case AsyncResolve_RelayHost:
        kind = "relay-host";
        break;
    case AsyncResolve_RelayGuest:
        kind = "relay-guest";
        break;
    case AsyncResolve_DirectGuest:
        kind = "direct-guest";
        break;
    default:
        break;
    }
    TraceDiagnostic(event, "kind=%s host=%s port=%d success=%d candidates=%d error=%s", kind,
                    outcome.originalHost.c_str(), outcome.port, outcome.success ? 1 : 0,
                    (int)outcome.candidates.size(), outcome.errorText.empty() ? "-" : outcome.errorText.c_str());
}

void ApplyResolvedRelayLaunchState(bool isHostRole)
{
    g_State.host.Reset();
    g_State.guest.Reset();
    g_State.useRelayTransport = true;
    g_State.isHost = isHostRole;
    g_State.isGuest = !isHostRole;
    g_State.isConnected = false;
    g_State.guestWaitStartTick = isHostRole ? 0 : SDL_GetTicks64();
    g_State.lastPeriodicPingTick = 0;
}

bool FinalizeResolvedLauncherRequest(const AsyncResolveOutcome &outcome)
{
    switch (outcome.kind)
    {
    case AsyncResolve_RelayHost:
    case AsyncResolve_RelayGuest:
    {
        std::string error;
        for (const AsyncResolveCandidate &candidate : outcome.candidates)
        {
            const std::string endpoint = BuildEndpointTextFromIp(candidate.ip, outcome.port);
            if (g_State.relay.Start(endpoint, outcome.roomCode, outcome.relayIsHost, outcome.localPort, &error))
            {
                ApplyResolvedRelayLaunchState(outcome.relayIsHost);
                return true;
            }
        }
        SetStatus("relay register failed");
        return false;
    }
    case AsyncResolve_DirectGuest:
    {
        bool loopbackPortConflict = false;
        for (const AsyncResolveCandidate &candidate : outcome.candidates)
        {
            if ((candidate.ip == "127.0.0.1" || candidate.ip == "::1") && outcome.port == outcome.localPort)
            {
                loopbackPortConflict = true;
                continue;
            }
            if (g_State.guest.Start(candidate.ip, outcome.port, outcome.localPort, candidate.family))
            {
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
        }
        if (loopbackPortConflict)
        {
            SetStatus("guest listen port conflicts with host");
            return false;
        }
        SetStatus("fail to start as guest");
        return false;
    }
    default:
        return false;
    }
}

void DrivePendingLauncherResolve()
{
    AsyncResolveOutcome outcome;
    if (!TryTakeAsyncResolveOutcome(g_State.launcherResolve, outcome))
    {
        return;
    }

    TraceAsyncResolveOutcome("async-resolve-complete", outcome);
    if (!outcome.success)
    {
        if (outcome.kind == AsyncResolve_DirectGuest)
        {
            SetStatus("resolve host failed");
        }
        else
        {
            SetStatus("resolve relay failed");
        }
        return;
    }

    FinalizeResolvedLauncherRequest(outcome);
}

void DrivePendingRelayProbeResolve()
{
    AsyncResolveOutcome outcome;
    if (!TryTakeAsyncResolveOutcome(g_Relay.probeResolve, outcome))
    {
        return;
    }

    TraceAsyncResolveOutcome("async-resolve-complete", outcome);
    if (!outcome.success || outcome.candidates.empty())
    {
        g_Relay.isConfigured = false;
        SetRelayStatus("resolve failed");
        return;
    }

    const AsyncResolveCandidate &candidate = outcome.candidates.front();
    g_Relay.host = candidate.ip;
    g_Relay.port = outcome.port;
    g_Relay.resolvedAddr = candidate.addr;
    g_Relay.resolvedAddrLen = candidate.addrLen;
    g_Relay.resolvedAddrValid = true;
    if (!OpenRelayProbeSocket(candidate.family))
    {
        SetRelayStatus("relay socket init failed");
        return;
    }
    SendRelayProbe();
}
} // namespace

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
        const u16 localInput = AstroBot::ProcessNetplayLocalInput(Controller::GetInput());
        TraceDiagnostic("advance-local-only", "input=%s", FormatInputBits(localInput).c_str());
        Session::ApplyLegacyFrameInput(localInput);
        return;
    }

    if (ShouldCompleteRuntimeSessionForEnding())
    {
        CompleteRuntimeSessionForEnding();
        const u16 localInput = AstroBot::ProcessNetplayLocalInput(Controller::GetInput());
        TraceDiagnostic("advance-ending-local-only", "input=%s", FormatInputBits(localInput).c_str());
        Session::ApplyLegacyFrameInput(localInput);
        return;
    }

    int frame = CurrentNetFrame();
    TraceDiagnostic("advance-begin", "frame=%d", frame);
    if (IsAuthoritativeRecoveryFreezeActive())
    {
        DriveAuthoritativeRecovery(frame);
        return;
    }
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
    DriveSharedShellHandoff(frame);
    frame = CurrentNetFrame();

    bool isInUi = IsCurrentUiFrame();
    const bool hadKnownUiState = g_State.hasKnownUiState;
    bool uiStateChanged = !hadKnownUiState || g_State.knownUiState != isInUi;
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
        if (hadKnownUiState && ConsumeSharedShellUiPhaseBarrierBypass(frame, isInUi))
        {
            TraceDiagnostic("ui-transition-shell-managed", "frame=%d isInUi=%d", frame, isInUi ? 1 : 0);
        }
        else
        {
            // Keep UI frames outside rollback, but let the first gameplay frame after ui-exit become
            // rollbackable immediately. Otherwise a late authoritative remote frame on that boundary
            // can poison the new gameplay timeline before any snapshot can correct it.
            const int nextEpochStartFrame = isInUi ? frame + 1 : frame;
            ResetRollbackEpoch(frame, isInUi ? "ui-enter" : "ui-exit", nextEpochStartFrame);
            if (!hadKnownUiState)
            {
                // The initial UI/gameplay state becomes known when the session first activates.
                // Treating that as a cross-peer phase transition deadlocks startup because the
                // peer has not yet had a chance to advertise its own baseline state.
                TraceDiagnostic("ui-state-init", "ui=%d frame=%d", isInUi ? 1 : 0, frame);
            }
            else
            {
                g_State.localUiPhaseSerial++;
                BeginUiPhaseBarrier(g_State.localUiPhaseSerial, isInUi);
            }
        }
    }

    if (DriveUiPhaseBarrier(frame))
    {
        return;
    }

    DriveUiPhaseBroadcast(frame);
    DriveSharedShellStateBroadcast();

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
        CaptureGameplaySnapshot(frame);
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
        TraceFrameSubsystemHashes(processedFrame, "rollback-queued");
        NoteRecoveryHeuristicCommittedFrame(processedFrame);
        g_State.lastFrame = processedFrame;
        g_State.currentNetFrame = processedFrame + 1;
        if (g_State.currentNetFrame >= g_State.rollbackTargetFrame)
        {
            g_State.rollbackActive = false;
            g_State.rollbackSendFrame = -1;
            g_State.rollbackSnapshotCaptureRequested = true;
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
        TraceFrameSubsystemHashes(frame, "rollback-active");
        NoteRecoveryHeuristicCommittedFrame(frame);
        g_State.lastFrame = frame;
        g_State.currentNetFrame = frame + 1;
        if (g_State.currentNetFrame >= g_State.rollbackTargetFrame)
        {
            g_State.rollbackActive = false;
            g_State.rollbackSendFrame = -1;
            g_State.rollbackSnapshotCaptureRequested = true;
            SetStatus("connected");
        }
        TraceDiagnostic("rollback-step-finished", "phase=active frame=%d final=%s ctrl=%s", frame,
                        FormatInputBits(finalInput).c_str(), InGameCtrlToString(currentCtrl));
        return;
    }

    HandleDesync(frame);

    u16 localInput = AstroBot::ProcessNetplayLocalInput(Controller::GetInput());
    InGameCtrlType localCtrl = CaptureControlKeys();
    if (IsPausePresentationHoldActive())
    {
        TraceDiagnostic("pause-hold-mask-input", "frame=%d input=%s ctrl=%s", frame, FormatInputBits(localInput).c_str(),
                        InGameCtrlToString(localCtrl));
        localInput = 0;
        localCtrl = IGC_NONE;
    }
    if (ShouldFreezeSharedShellUiInput(frame))
    {
        if (localInput != 0 || localCtrl != IGC_NONE)
        {
            TraceDiagnostic("shell-ui-input-frozen", "frame=%d input=%s ctrl=%s serial=%u handoff=%u", frame,
                            FormatInputBits(localInput).c_str(), InGameCtrlToString(localCtrl),
                            g_State.shell.shellSerial, g_State.shell.authoritativeHandoffFrame);
        }
        localInput = 0;
        localCtrl = IGC_NONE;
    }
    Bits<16> localBits;
    ReadFromInt(localBits, localInput);
    g_State.localInputs[frame] = localBits;
    g_State.localSeeds[frame] = g_Rng.seed;
    g_State.localCtrls[frame] = localCtrl;
    TraceDiagnostic("capture-local-frame", "frame=%d input=%s seed=%u ctrl=%s", frame,
                    FormatInputBits(localInput).c_str(), g_Rng.seed, InGameCtrlToString(g_State.localCtrls[frame]));
    PruneOldFrameData(frame);

    SendKeyPacket(frame);
    ReceiveRuntimePackets();

    InGameCtrlType currentCtrl = IGC_NONE;
    bool rollbackStarted = false;
    bool stallForPeer = false;
    u16 finalInput = ResolveFrameInput(frame, isInUi, currentCtrl, rollbackStarted, stallForPeer);
    if (IsAuthoritativeRecoveryFreezeActive())
    {
        DriveAuthoritativeRecovery(frame);
        return;
    }
    if (stallForPeer)
    {
        Session::ApplyLegacyFrameInput(g_CurFrameInput);
        g_State.stallFrameRequested = true;
        return;
    }
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
    if (g_State.isConnected && !g_State.isTryingReconnect && g_State.statusText == "waiting for peer...")
    {
        SetStatus("connected");
    }
    g_State.currentCtrl = currentCtrl;
    ApplyDelayControl();
    Session::ApplyLegacyFrameInput(finalInput);
    CommitProcessedFrameState(processedFrame);
    TraceFrameSubsystemHashes(processedFrame, rollbackStarted ? "rollback-post-start" : "live");
    NoteRecoveryHeuristicCommittedFrame(processedFrame);
    g_State.lastFrame = processedFrame;
    if (g_State.isConnected && !g_State.isTryingReconnect)
    {
        g_State.currentNetFrame = processedFrame + 1;
        if (g_State.rollbackActive && g_State.currentNetFrame >= g_State.rollbackTargetFrame)
        {
            g_State.rollbackActive = false;
            g_State.rollbackSendFrame = -1;
            g_State.rollbackSnapshotCaptureRequested = true;
            CaptureGameplaySnapshot(processedFrame);
            SetStatus("connected");
        }
    }
    TraceDiagnostic("advance-end", "frame=%d processed=%d final=%s ctrl=%s rollbackStarted=%d", frame, processedFrame,
                    FormatInputBits(finalInput).c_str(), InGameCtrlToString(currentCtrl),
                    rollbackStarted ? 1 : 0);
}

void ActivateNetplaySession(u16 sharedRngSeed, bool useSharedSeed)
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
    g_State.pendingDebugEndingJump = false;
    g_State.reconnectReason = AuthoritativeRecoveryReason_None;
    ClearRuntimeCaches();
    // Entering a fresh remote gameplay stream must not inherit menu-held inputs or
    // previous legacy edge state; otherwise frame 0 can already diverge in player
    // state before RNG or gameplay managers differ.
    Controller::ResetDeviceInputState();
    Session::ResetLegacyInputState();
    if (useSharedSeed)
    {
        const u16 appliedSeed = g_State.authoritySharedSeedKnown ? g_State.authoritySharedSeed
                                                                 : (sharedRngSeed != 0 ? sharedRngSeed : 1);
        if (!g_State.authoritySharedSeedKnown)
        {
            g_State.authoritySharedSeed = appliedSeed;
            g_State.authoritySharedSeedKnown = true;
        }
        g_Rng.seed = appliedSeed;
        g_Rng.generationCount = 0;
        g_State.sessionRngSeed = appliedSeed;
        g_State.sharedRngSeedCaptured = true;
        g_State.startupPeerSeedConfirmed = true;
        g_State.authoritySharedSeedApplied = true;
        TraceDiagnostic("startup-seed-rebase", "seed=%u role=%s", appliedSeed, g_State.isHost ? "host" : "guest");
        TraceDiagnostic("activate-session-shared-seed", "seed=%u", appliedSeed);
    }
    else
    {
        g_State.authoritySharedSeedApplied = false;
    }
    g_State.startupInitSettingReceived = false;
    g_State.startupActivationComplete = true;
    // Temporarily force the stable rollback session. The experimental authoritative
    // session remains in the tree for future work, but it is not safe to activate.
    Session::SetActiveSession(static_cast<ISession &>(g_NetSession));
    TraceDiagnostic("activate-session", "-");
    SetStatus("connected");

} // namespace

ISession &GetSession()
{
    return static_cast<ISession &>(g_NetSession);
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
    snapshot.authoritativeMode = false;
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

        std::string relayHost;
        int relayPort = 0;
        if (!ParseEndpointText(trimmedRelay, relayHost, relayPort))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "invalid relay endpoint";
            }
            SetStatus("invalid relay endpoint");
            return false;
        }
        if (!IsNumericHostLiteral(relayHost))
        {
            StartAsyncResolveState(g_State.launcherResolve, AsyncResolve_RelayHost, relayHost, relayPort, AF_UNSPEC,
                                   listenPort, true, trimmedRoom);
            TraceDiagnostic("async-resolve-begin", "kind=relay-host host=%s port=%d", relayHost.c_str(), relayPort);
            SetStatus("resolving relay...");
            return true;
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

        std::string relayHost;
        int relayPort = 0;
        if (!ParseEndpointText(trimmedRelay, relayHost, relayPort))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "invalid relay endpoint";
            }
            SetStatus("invalid relay endpoint");
            return false;
        }
        if (!IsNumericHostLiteral(relayHost))
        {
            StartAsyncResolveState(g_State.launcherResolve, AsyncResolve_RelayGuest, relayHost, relayPort, AF_UNSPEC,
                                   listenPort, false, trimmedRoom);
            TraceDiagnostic("async-resolve-begin", "kind=relay-guest host=%s port=%d", relayHost.c_str(), relayPort);
            SetStatus("resolving relay...");
            return true;
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
    if (!IsNumericHostLiteral(hostIp))
    {
        StartAsyncResolveState(g_State.launcherResolve, AsyncResolve_DirectGuest, hostIp, hostPort, AF_UNSPEC,
                               listenPort, false, "");
        TraceDiagnostic("async-resolve-begin", "kind=direct-guest host=%s port=%d", hostIp.c_str(), hostPort);
        SetStatus("resolving host...");
        return true;
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
    g_Relay.resolvedAddr = {};
    g_Relay.resolvedAddrLen = 0;
    g_Relay.resolvedAddrValid = false;

    if (!IsNumericHostLiteral(host))
    {
        StartAsyncResolveState(g_Relay.probeResolve, AsyncResolve_RelayProbe, host, port, AF_UNSPEC, 0, false, "");
        TraceDiagnostic("async-resolve-begin", "kind=relay-probe host=%s port=%d", host.c_str(), port);
        SetRelayStatus("resolving relay...");
        return true;
    }

    const int preferredFamily = host.find(':') != std::string::npos ? AF_INET6 : AF_INET;
    if (!OpenRelayProbeSocket(preferredFamily))
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
    CancelAsyncResolveState(g_Relay.probeResolve);
    CloseRelayProbeSocket();
    g_Relay.endpointText.clear();
    g_Relay.host.clear();
    g_Relay.port = kRelayDefaultPort;
    g_Relay.isConfigured = false;
    g_Relay.nextNonce = 1;
    SetRelayStatus("not configured");
}

void CancelPendingAsyncResolveJobs()
{
    CancelAsyncResolveState(g_State.launcherResolve);
    CancelAsyncResolveState(g_Relay.probeResolve);
}

void CancelPendingConnection()
{
    if (g_State.isSessionActive)
    {
        return;
    }

    CancelAsyncResolveState(g_State.launcherResolve);
    ClearDebugNetworkQueues();
    g_State.host.Reset();
    g_State.guest.Reset();
    g_State.relay.Reset();
    g_State.isHost = false;
    g_State.isGuest = false;
    g_State.isConnected = false;
    g_State.useRelayTransport = false;
    g_State.isWaitingForStartup = false;
    g_State.titleColdStartRequested = false;
    g_State.pendingDebugEndingJump = false;
    g_State.lastPeriodicPingTick = 0;
    g_State.guestWaitStartTick = 0;
    g_State.lastRttMs = -1;
    g_State.versionMatched = true;
    g_State.authoritySharedSeed = 0;
    g_State.authoritySharedSeedKnown = false;
    g_State.startupPeerSeedConfirmed = false;
    g_State.authoritySharedSeedApplied = false;
    g_State.gameplayRuntimeStreamRebasedForStartup = false;
    g_State.startupActivationComplete = false;
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

bool RequestDebugEndingJump()
{
    if (!g_State.isConnected || (g_State.useRelayTransport && !g_State.relay.IsReady()))
    {
        return false;
    }

    // The sender does not receive its own launcher event back, so mark the local
    // shortcut as pending too; the peer still receives the explicit network event.
    g_State.pendingDebugEndingJump = true;

    // Developer events are infrequent, so a short burst is enough to survive a dropped UDP packet.
    for (int i = 0; i < 4; ++i)
    {
        if (g_State.isSessionActive)
        {
            Pack pack;
            pack.type = PACK_USUAL;
            pack.ctrl.ctrlType = Ctrl_Debug_EndingJump;
            SendPacket(pack);
        }
        else
        {
            SendPacket(MakePing(Ctrl_Debug_EndingJump));
        }
    }
    TraceDiagnostic("send-debug-ending-jump", "burst=4 localPending=1 via=%s",
                    g_State.isSessionActive ? "runtime" : "launcher");
    return true;
}

bool ConsumeRequestedDebugEndingJump()
{
    const bool requested = g_State.pendingDebugEndingJump;
    g_State.pendingDebugEndingJump = false;
    return requested;
}

void ActivateUiSession()
{
    if (!g_State.isConnected || g_State.isSessionActive)
    {
        return;
    }

    ActivateNetplaySession();
    TraceDiagnostic("activate-ui-session", "-");
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

    DrivePendingLauncherResolve();
    DrivePendingRelayProbeResolve();
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
    return !Session::IsRemoteNetplaySession();
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
    if (g_State.isHost)
    {
        return g_State.hostIsPlayer1;
    }
    if (g_State.isGuest)
    {
        return !g_State.hostIsPlayer1;
    }
    return true;
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

DebugNetworkConfig GetDebugNetworkConfig()
{
    return g_State.debugNetworkConfig;
}

void SetDelay(int delay)
{
    g_State.delay = std::clamp(delay, 0, kMaxDelay);
}

void SetPredictionRollbackEnabled(bool enabled)
{
#if !TH06_ENABLE_PREDICTION_ROLLBACK
    enabled = false;
#endif
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

void SetAuthoritativeModeEnabled(bool enabled)
{
    (void)enabled;
    TraceDiagnostic("set-authoritative-mode-disabled", "requested=%d old=%d", enabled ? 1 : 0,
                    g_State.authoritativeModeEnabled ? 1 : 0);
    // Disabled for now. Force fallback to the stable rollback netplay path.
    g_State.authoritativeModeEnabled = false;
    g_State.authoritativeHashCheckEnabled = false;
    g_State.authoritativeHashMismatchPending = false;
}

void SetDebugNetworkConfig(const DebugNetworkConfig &config)
{
    DebugNetworkConfig clamped = config;
    clamped.latencyMs = std::clamp(clamped.latencyMs, 0, 1000);
    clamped.jitterMs = std::clamp(clamped.jitterMs, 0, 500);
    clamped.packetLossPercent = std::clamp(clamped.packetLossPercent, 0, 100);
    clamped.duplicatePercent = std::clamp(clamped.duplicatePercent, 0, 100);

    TraceDiagnostic("set-debug-network",
                    "enabled=%d latency=%d jitter=%d loss=%d duplicate=%d oldEnabled=%d oldLatency=%d oldJitter=%d "
                    "oldLoss=%d oldDuplicate=%d",
                    clamped.enabled ? 1 : 0, clamped.latencyMs, clamped.jitterMs, clamped.packetLossPercent,
                    clamped.duplicatePercent, g_State.debugNetworkConfig.enabled ? 1 : 0,
                    g_State.debugNetworkConfig.latencyMs, g_State.debugNetworkConfig.jitterMs,
                    g_State.debugNetworkConfig.packetLossPercent, g_State.debugNetworkConfig.duplicatePercent);

    const bool disabling = g_State.debugNetworkConfig.enabled && !clamped.enabled;
    g_State.debugNetworkConfig = clamped;
    if (disabling)
    {
        ClearDebugNetworkQueues();
    }
}

void SetHostPlayer1(bool hostIsPlayer1)
{
    g_State.hostIsPlayer1 = hostIsPlayer1;
}

bool ConsumeTitleColdStartRequested()
{
    const bool requested = g_State.titleColdStartRequested;
    g_State.titleColdStartRequested = false;
    return requested;
}

void PrepareGameplayStart()
{
    if (!Session::IsRemoteNetplaySession())
    {
        return;
    }

    const bool hadKnownUiState = g_State.hasKnownUiState;
    const bool knownUiState = g_State.knownUiState;

    if (!g_State.authoritativeModeEnabled)
    {
        if (!g_State.gameplayRuntimeStreamRebasedForStartup)
        {
            ResetGameplayRuntimeStream();
        }
    }
    else if (g_State.authoritativeGameplayResetPending)
    {
        ResetGameplayRuntimeStream();
    }

    g_State.authoritativeGameplayResetPending = false;
    g_State.gameplayRuntimeStreamRebasedForStartup = false;

    if (g_State.authoritativeModeEnabled && g_State.authoritySharedSeedKnown)
    {
        const u16 appliedSeed = g_State.authoritySharedSeed != 0 ? g_State.authoritySharedSeed : 1;
        g_Rng.seed = appliedSeed;
        g_Rng.generationCount = 0;
        g_State.sessionRngSeed = appliedSeed;
        g_State.sharedRngSeedCaptured = true;
        g_State.authoritySharedSeedApplied = true;
        TraceDiagnostic("authoritative-gameplay-seed-rebase", "seed=%u source=prepare-gameplay", appliedSeed);
    }

    if (hadKnownUiState)
    {
        g_State.hasKnownUiState = true;
        g_State.knownUiState = knownUiState;
    }
}

InGameCtrlType ConsumeInGameControl()
{
    const InGameCtrlType ctrl = g_State.currentCtrl;
    g_State.currentCtrl = IGC_NONE;
    return ctrl;
}

void DrawOverlay()
{
    if (!Session::IsRemoteNetplaySession())
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

} // namespace th06::Netplay
