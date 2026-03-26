#include "NetplayInternal.hpp"

namespace th06::Netplay
{
RuntimeState g_State;

RelayState g_Relay;

NetSession g_NetSession;

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
        u16 peerSharedSeed = 0;
        if (!TryActivateFromStartupPacket(&peerSharedSeed))
        {
            Session::ApplyLegacyFrameInput(0);
            TraceDiagnostic("advance-startup-wait-no-activate", "applied=0");
            return;
        }
        if (peerSharedSeed != 0 && !g_State.sharedRngSeedCaptured)
        {
            g_Rng.seed = peerSharedSeed;
            g_Rng.generationCount = 0;
            g_State.sessionRngSeed = peerSharedSeed;
            g_State.sharedRngSeedCaptured = true;
            TraceDiagnostic("advance-startup-seed-adopt", "peerSeed=%u", peerSharedSeed);
        }
    }

    if (!g_State.isSessionActive)
    {
        const u16 localInput = Controller::GetInput();
        TraceDiagnostic("advance-local-only", "input=%s", FormatInputBits(localInput).c_str());
        Session::ApplyLegacyFrameInput(localInput);
        return;
    }

    if (ShouldCompleteRuntimeSessionForEnding())
    {
        CompleteRuntimeSessionForEnding();
        const u16 localInput = Controller::GetInput();
        TraceDiagnostic("advance-ending-local-only", "input=%s", FormatInputBits(localInput).c_str());
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

    if (DriveUiPhaseBarrier(frame))
    {
        return;
    }

    DriveUiPhaseBroadcast(frame);

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
        TraceFrameSubsystemHashes(frame, "rollback-active");
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
    bool stallForPeer = false;
    u16 finalInput = ResolveFrameInput(frame, isInUi, currentCtrl, rollbackStarted, stallForPeer);
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
    ClearRuntimeCaches();
    if (useSharedSeed)
    {
        g_Rng.seed = sharedRngSeed;
        g_Rng.generationCount = 0;
        g_State.sessionRngSeed = sharedRngSeed;
        g_State.sharedRngSeedCaptured = true;
        TraceDiagnostic("activate-session-shared-seed", "seed=%u", sharedRngSeed);
    }
    Session::SetActiveSession(g_NetSession);
    TraceDiagnostic("activate-session", "-");
    SetStatus("connected");

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

    const bool hadKnownUiState = g_State.hasKnownUiState;
    const bool knownUiState = g_State.knownUiState;

    ResetGameplayRuntimeStream();

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

} // namespace th06::Netplay
