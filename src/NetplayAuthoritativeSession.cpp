#include "NetplayAuthoritativeSession.hpp"

#include "AstroBot.hpp"
#include "NetplayAuthoritativePresentation.hpp"
#include "NetplayAuthoritativeReplicator.hpp"

namespace th06::Netplay
{
AuthoritativeNetSession g_AuthoritativeNetSession;

namespace
{
constexpr u16 kPlayer1LaneButtons = TH_BUTTON_SHOOT | TH_BUTTON_BOMB | TH_BUTTON_FOCUS | TH_BUTTON_UP |
                                    TH_BUTTON_DOWN | TH_BUTTON_LEFT | TH_BUTTON_RIGHT;
constexpr u16 kPlayer2LaneButtons = TH_BUTTON_SHOOT2 | TH_BUTTON_BOMB2 | TH_BUTTON_FOCUS2 | TH_BUTTON_UP2 |
                                    TH_BUTTON_DOWN2 | TH_BUTTON_LEFT2 | TH_BUTTON_RIGHT2;

void CaptureAuthoritativeLocalFrame(int frame)
{
    const u16 rawInput = Controller::GetInput();
    u16 localInput = AstroBot::ProcessNetplayLocalInput(rawInput);
    InGameCtrlType localCtrl = CaptureControlKeys();
    if (IsPausePresentationHoldActive())
    {
        TraceDiagnostic("authoritative-pause-hold-mask-input", "frame=%d input=%s ctrl=%s", frame,
                        FormatInputBits(localInput).c_str(), InGameCtrlToString(localCtrl));
        localInput = 0;
        localCtrl = IGC_NONE;
    }
    if (ShouldFreezeSharedShellUiInput(frame))
    {
        TraceDiagnostic("authoritative-shell-ui-input-frozen", "frame=%d input=%s ctrl=%s serial=%u handoff=%u",
                        frame, FormatInputBits(localInput).c_str(), InGameCtrlToString(localCtrl),
                        g_State.shell.shellSerial, g_State.shell.authoritativeHandoffFrame);
        localInput = 0;
        localCtrl = IGC_NONE;
    }

    Bits<16> localBits {};
    ReadFromInt(localBits, localInput);
    g_State.localInputs[frame] = localBits;
    g_State.localSeeds[frame] = g_Rng.seed;
    g_State.localCtrls[frame] = localCtrl;
    AuthoritativePresentation::NoteLocalPredictedInput(frame, localInput);
    TraceDiagnostic("authoritative-capture-local-frame", "frame=%d input=%s seed=%u ctrl=%s", frame,
                    FormatInputBits(localInput).c_str(), g_Rng.seed, InGameCtrlToString(localCtrl));
}

bool TryCompareAuthoritativeHash(int frame)
{
    int mismatchFrame = -1;
    if (!AuthoritativeReplicator::TryConsumeHostMismatchFrame(mismatchFrame))
    {
        return false;
    }

    if (TryStartAuthoritativeRecovery(frame, AuthoritativeRecoveryReason_UnrollbackableDesync))
    {
        SetStatus("authoritative resync...");
        return true;
    }
    return false;
}

u16 MapLocalInputToGameplayLane(u16 localInput)
{
    const u16 laneInput = localInput & kPlayer1LaneButtons;
    if (IsLocalPlayer1())
    {
        return laneInput;
    }

    u16 mapped = 0;
    if ((laneInput & TH_BUTTON_SHOOT) != 0)
    {
        mapped |= TH_BUTTON_SHOOT2;
    }
    if ((laneInput & TH_BUTTON_BOMB) != 0)
    {
        mapped |= TH_BUTTON_BOMB2;
    }
    if ((laneInput & TH_BUTTON_FOCUS) != 0)
    {
        mapped |= TH_BUTTON_FOCUS2;
    }
    if ((laneInput & TH_BUTTON_UP) != 0)
    {
        mapped |= TH_BUTTON_UP2;
    }
    if ((laneInput & TH_BUTTON_DOWN) != 0)
    {
        mapped |= TH_BUTTON_DOWN2;
    }
    if ((laneInput & TH_BUTTON_LEFT) != 0)
    {
        mapped |= TH_BUTTON_LEFT2;
    }
    if ((laneInput & TH_BUTTON_RIGHT) != 0)
    {
        mapped |= TH_BUTTON_RIGHT2;
    }
    return mapped;
}
} // namespace

SessionKind AuthoritativeNetSession::Kind() const
{
    return SessionKind::NetplayAuthoritative;
}

void AuthoritativeNetSession::ResetInputState()
{
    Controller::ResetDeviceInputState();
    Session::ResetLegacyInputState();
    ClearRuntimeCaches();
    AuthoritativeReplicator::Reset();
    AuthoritativePresentation::Reset();
    g_State.lastFrame = -1;
    g_State.currentCtrl = IGC_NONE;
}

void AuthoritativeNetSession::AdvanceFrameInput()
{
    g_State.stallFrameRequested = false;

    if (g_State.isWaitingForStartup && !g_State.isSessionActive)
    {
        TraceDiagnostic("authoritative-advance-startup-wait", "-");
        SendStartupBootstrapPacket();
        if (!TryActivateFromStartupPacket())
        {
            Session::ApplyLegacyFrameInput(0);
            return;
        }
    }

    if (!g_State.isSessionActive)
    {
        const u16 localInput = Controller::GetInput();
        Session::ApplyLegacyFrameInput(localInput);
        return;
    }

    if (ShouldCompleteRuntimeSessionForEnding())
    {
        CompleteRuntimeSessionForEnding();
        Session::ApplyLegacyFrameInput(Controller::GetInput());
        return;
    }

    int frame = CurrentNetFrame();
    if (IsAuthoritativeRecoveryFreezeActive())
    {
        DriveAuthoritativeRecovery(frame);
        return;
    }

    if (g_State.lastFrame >= 0 && frame < g_State.lastFrame)
    {
        TraceDiagnostic("authoritative-frame-reset", "frame=%d lastFrame=%d", frame, g_State.lastFrame);
        ClearRuntimeCaches();
        AuthoritativeReplicator::Reset();
        AuthoritativePresentation::Reset();
    }

    if (g_State.isTryingReconnect)
    {
        TraceDiagnostic("authoritative-reconnect-redirect",
                        "frame=%d reason=%d status=%s connected=%d recoveryActive=%d", frame,
                        (int)g_State.reconnectReason, g_State.statusText.c_str(), g_State.isConnected ? 1 : 0,
                        IsAuthoritativeRecoveryActive() ? 1 : 0);
        g_State.isTryingReconnect = false;
        g_State.reconnectIssued = false;
        g_State.reconnectStartTick = 0;
        if (!IsAuthoritativeRecoveryActive())
        {
            TryStartAuthoritativeRecovery(
                frame, g_State.reconnectReason != AuthoritativeRecoveryReason_None
                           ? g_State.reconnectReason
                           : AuthoritativeRecoveryReason_ReconnectTimeout);
        }
        SetStatus("authoritative resync...");
        DriveAuthoritativeRecovery(frame);
        return;
    }

    ForceDeterministicNetplayStep();
    DriveSharedShellHandoff(frame);
    frame = CurrentNetFrame();

    bool isInUi = IsCurrentUiFrame();
    const bool hadKnownUiState = g_State.hasKnownUiState;
    const bool uiStateChanged = !hadKnownUiState || g_State.knownUiState != isInUi;
    if (uiStateChanged)
    {
        g_State.hasKnownUiState = true;
        g_State.knownUiState = isInUi;
        if (hadKnownUiState && ConsumeSharedShellUiPhaseBarrierBypass(frame, isInUi))
        {
            TraceDiagnostic("authoritative-ui-transition-shell-managed", "frame=%d isInUi=%d", frame,
                            isInUi ? 1 : 0);
        }
        else
        {
            if (!hadKnownUiState)
            {
                TraceDiagnostic("authoritative-ui-state-init", "ui=%d frame=%d", isInUi ? 1 : 0, frame);
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

    CaptureAuthoritativeLocalFrame(frame);
    PruneOldFrameData(frame);

    SendKeyPacket(frame);
    ReceiveRuntimePackets();

    if (IsAuthoritativeRecoveryFreezeActive())
    {
        DriveAuthoritativeRecovery(frame);
        return;
    }

    if (g_State.isHost)
    {
        if (isInUi)
        {
            const auto selfIt = g_State.localInputs.find(frame);
            const u16 finalInput = selfIt != g_State.localInputs.end() ? WriteToInt(selfIt->second) : 0;
            const auto selfCtrlIt = g_State.localCtrls.find(frame);
            g_State.currentCtrl = selfCtrlIt != g_State.localCtrls.end() ? selfCtrlIt->second : IGC_NONE;
            Session::ApplyLegacyFrameInput(finalInput);
            CommitProcessedFrameState(frame);
            g_State.lastFrame = frame;
            g_State.currentNetFrame = frame + 1;
            if (g_State.statusText == "waiting for authoritative input..." ||
                g_State.statusText == "waiting for authoritative ui..." ||
                g_State.statusText == "authoritative resync...")
            {
                SetStatus("connected");
            }

            AuthoritativeReplicator::ReplicatedWorldState state {};
            if (AuthoritativeReplicator::CaptureCurrentReplicatedWorldState(frame, state))
            {
                SendAuthoritativeFrameStateDatagram(state);
            }
            TraceDiagnostic("authoritative-advance-end", "frame=%d final=%s ctrl=%s role=host-ui",
                            frame, FormatInputBits(finalInput).c_str(), InGameCtrlToString(g_State.currentCtrl));
            return;
        }

        InGameCtrlType currentCtrl = IGC_NONE;
        u16 finalInput = 0;
        if (!ResolveStoredFrameInput(frame, isInUi, finalInput, currentCtrl))
        {
            if (g_State.isConnected && !g_State.isTryingReconnect)
            {
                SendKeyPacket(frame);
            }
            SetStatus("waiting for authoritative input...");
            Session::ApplyLegacyFrameInput(g_CurFrameInput);
            g_State.stallFrameRequested = true;
            TraceDiagnostic("authoritative-host-wait-peer", "frame=%d ui=%d reappliedCur=%s", frame, isInUi ? 1 : 0,
                            FormatInputBits(g_CurFrameInput).c_str());
            return;
        }

        g_State.currentCtrl = currentCtrl;
        ApplyDelayControl();
        Session::ApplyLegacyFrameInput(finalInput);
        CommitProcessedFrameState(frame);
        TraceFrameSubsystemHashes(frame, "authoritative-host-live");
        g_State.lastFrame = frame;
        g_State.currentNetFrame = frame + 1;
        if (g_State.isConnected && !g_State.isTryingReconnect &&
            (g_State.statusText == "waiting for authoritative input..." ||
             g_State.statusText == "authoritative resync..."))
        {
            SetStatus("connected");
        }

        AuthoritativeReplicator::ReplicatedWorldState state {};
        if (AuthoritativeReplicator::CaptureCurrentReplicatedWorldState(frame, state))
        {
            SendAuthoritativeFrameStateDatagram(state);
        }
        AuthoritativePresentation::SyncFromCanonicalLocalPlayer(frame);
        AuthoritativeReplicator::RecordLocalMirrorFrameHash(frame);
        TraceDiagnostic("authoritative-advance-end", "frame=%d final=%s ctrl=%s role=host",
                        frame, FormatInputBits(finalInput).c_str(), InGameCtrlToString(currentCtrl));
    }
    else
    {
        if (isInUi)
        {
            if (!g_State.latestAuthoritativeFrameState.valid)
            {
                if (g_State.isConnected)
                {
                    SendKeyPacket(frame);
                }
                SetStatus("waiting for authoritative ui state...");
                Session::ApplyLegacyFrameInput(g_CurFrameInput);
                g_State.stallFrameRequested = true;
                TraceDiagnostic("authoritative-guest-wait-ui-state", "frame=%d reappliedCur=%s", frame,
                                FormatInputBits(g_CurFrameInput).c_str());
                return;
            }

            AuthoritativeReplicator::ApplyLatestAuthoritativeStateToLiveWorld();
            g_State.currentCtrl = IGC_NONE;
            Session::ApplyLegacyFrameInput(0);
            CommitProcessedFrameState(frame);
            AuthoritativeReplicator::ApplyLatestAuthoritativeStateToLiveWorld();
            g_State.lastFrame = frame;
            g_State.currentNetFrame = frame + 1;
            if (g_State.statusText == "waiting for authoritative ui..." ||
                g_State.statusText == "waiting for authoritative ui state...")
            {
                SetStatus("connected");
            }
            TraceDiagnostic("authoritative-advance-end", "frame=%d final=%s ctrl=%s role=guest-ui-follow",
                            frame, FormatInputBits(0).c_str(), InGameCtrlToString(g_State.currentCtrl));
            return;
        }

        const u16 localLaneInput = MapLocalInputToGameplayLane(WriteToInt(g_State.localInputs[frame]));
        AuthoritativeReplicator::ApplyLatestAuthoritativeStateToLiveWorld();
        g_State.currentCtrl = IGC_NONE;
        Session::ApplyLegacyFrameInput(localLaneInput);
        CommitProcessedFrameState(frame);
        g_State.lastFrame = frame;
        g_State.currentNetFrame = frame + 1;
        if (g_State.statusText == "waiting for authoritative input..." || g_State.statusText == "authoritative resync...")
        {
            SetStatus("connected");
        }

        AuthoritativeReplicator::ApplyLatestAuthoritativeStateToLiveWorld();
        AuthoritativePresentation::SyncFromCanonicalLocalPlayer(frame);
        AuthoritativePresentation::ReconcileFromAuthoritativeState(g_State.latestAuthoritativeFrameState);
        AuthoritativeReplicator::RecordLocalMirrorFrameHash(frame);
        if (TryCompareAuthoritativeHash(frame))
        {
            g_State.stallFrameRequested = true;
        }
        TraceDiagnostic("authoritative-advance-end", "frame=%d final=%s ctrl=%s role=guest", frame,
                        FormatInputBits(localLaneInput).c_str(), InGameCtrlToString(g_State.currentCtrl));
    }
}
} // namespace th06::Netplay
