#include "NetplayInternal.hpp"

namespace th06::Netplay
{
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

bool ShouldCompleteRuntimeSessionForEnding()
{
    return Session::GetKind() == SessionKind::Netplay &&
           !THPrac::TH06::THPracIsDebugEndingJumpActive() &&
           (g_Supervisor.curState == SUPERVISOR_STATE_ENDING || g_Supervisor.isInEnding);
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

void CompleteRuntimeSessionForEnding()
{
    TraceDiagnostic("complete-ending-session", "sup=%d isInEnding=%d", g_Supervisor.curState,
                    g_Supervisor.isInEnding ? 1 : 0);
    ResetLauncherState();
    SetStatus("session complete");
    Session::UseLocalSession();
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
void SendUiPhasePacket(int serial, bool isInUi, int boundaryFrame)
{
    Pack pack;
    pack.type = PACK_USUAL;
    pack.ctrl.ctrlType = Ctrl_UiPhase;
    pack.ctrl.uiPhase.serial = serial;
    pack.ctrl.uiPhase.flags = isInUi ? UiPhaseFlag_InUi : 0;
    pack.ctrl.uiPhase.boundaryFrame = boundaryFrame;
    TraceDiagnostic("send-ui-phase", "serial=%d ui=%d frame=%d", serial, isInUi ? 1 : 0, boundaryFrame);
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

void BeginUiPhaseBarrier(int serial, bool isInUi)
{
    g_State.pendingUiPhaseSerial = serial;
    g_State.pendingUiPhaseIsInUi = isInUi;
    g_State.awaitingUiPhaseAck = true;
    g_State.lastUiPhaseSendTick = 0;
}

void StartUiPhaseBroadcast(int serial, bool isInUi)
{
    g_State.broadcastUiPhaseSerial = serial;
    g_State.broadcastUiPhaseIsInUi = isInUi;
    g_State.broadcastUiPhaseUntilTick = SDL_GetTicks64() + kUiPhaseBroadcastMs;
    g_State.lastUiPhaseBroadcastTick = 0;
}

void DriveUiPhaseBroadcast(int frame)
{
    if (g_State.awaitingUiPhaseAck || g_State.broadcastUiPhaseSerial <= 0)
    {
        return;
    }

    const Uint64 now = SDL_GetTicks64();
    if (now >= g_State.broadcastUiPhaseUntilTick)
    {
        g_State.broadcastUiPhaseSerial = 0;
        g_State.broadcastUiPhaseIsInUi = false;
        g_State.broadcastUiPhaseUntilTick = 0;
        g_State.lastUiPhaseBroadcastTick = 0;
        return;
    }

    if (g_State.lastUiPhaseBroadcastTick != 0 && now - g_State.lastUiPhaseBroadcastTick < kReconnectPingMs)
    {
        return;
    }

    g_State.lastUiPhaseBroadcastTick = now;
    SendUiPhasePacket(g_State.broadcastUiPhaseSerial, g_State.broadcastUiPhaseIsInUi, frame);
    TraceDiagnostic("send-ui-phase-broadcast", "serial=%d ui=%d frame=%d",
                    g_State.broadcastUiPhaseSerial, g_State.broadcastUiPhaseIsInUi ? 1 : 0, frame);
}

bool DriveUiPhaseBarrier(int frame)
{
    if (!g_State.awaitingUiPhaseAck)
    {
        return false;
    }

    ReceiveRuntimePackets();

    const Uint64 now = SDL_GetTicks64();
    if (g_State.lastUiPhaseSendTick == 0 || now - g_State.lastUiPhaseSendTick >= kReconnectPingMs)
    {
        g_State.lastUiPhaseSendTick = now;
        SendUiPhasePacket(g_State.pendingUiPhaseSerial, g_State.pendingUiPhaseIsInUi, frame);
        if (g_State.lastFrame >= 0)
        {
            SendKeyPacket(g_State.lastFrame);
            TraceDiagnostic("send-ui-phase-runtime-bootstrap", "serial=%d frame=%d lastFrame=%d",
                            g_State.pendingUiPhaseSerial, frame, g_State.lastFrame);
        }
        else if (frame == 0 && g_State.remoteUiPhaseSerial == 0)
        {
            SendStartupBootstrapPacket();
            TraceDiagnostic("send-startup-bootstrap-for-ui-phase", "serial=%d frame=%d",
                            g_State.pendingUiPhaseSerial, frame);
        }
    }

    const bool acked = g_State.remoteUiPhaseSerial >= g_State.pendingUiPhaseSerial &&
                       g_State.remoteUiPhaseIsInUi == g_State.pendingUiPhaseIsInUi;
    if (acked)
    {
        TraceDiagnostic("ui-phase-ack", "serial=%d ui=%d remoteSerial=%d", g_State.pendingUiPhaseSerial,
                        g_State.pendingUiPhaseIsInUi ? 1 : 0, g_State.remoteUiPhaseSerial);
        const int ackSerial = g_State.pendingUiPhaseSerial;
        const bool ackIsInUi = g_State.pendingUiPhaseIsInUi;
        const bool enteringGameplay = !ackIsInUi;
        g_State.awaitingUiPhaseAck = false;
        g_State.lastUiPhaseSendTick = 0;
        g_State.pendingUiPhaseSerial = 0;
        g_State.pendingUiPhaseIsInUi = false;
        if (enteringGameplay)
        {
            ResetGameplayRuntimeStream();
            g_State.hasKnownUiState = true;
            g_State.knownUiState = false;
            g_State.localUiPhaseSerial = ackSerial;
            g_State.remoteUiPhaseSerial = ackSerial;
            g_State.remoteUiPhaseIsInUi = false;
        }
        StartUiPhaseBroadcast(ackSerial, ackIsInUi);
        return false;
    }

    SetStatus("waiting for ui sync...");
    Session::ApplyLegacyFrameInput(g_CurFrameInput);
    g_State.stallFrameRequested = true;
    TraceDiagnostic("ui-phase-wait", "serial=%d ui=%d remoteSerial=%d remoteUi=%d frame=%d",
                    g_State.pendingUiPhaseSerial, g_State.pendingUiPhaseIsInUi ? 1 : 0, g_State.remoteUiPhaseSerial,
                    g_State.remoteUiPhaseIsInUi ? 1 : 0, frame);
    return true;
}


} // namespace th06::Netplay
