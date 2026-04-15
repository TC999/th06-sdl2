#include "NetplayInternal.hpp"

namespace th06::Netplay
{
namespace
{
u16 NormalizeRollbackStartupSeed(u16 seed)
{
    return seed != 0 ? seed : 1;
}
} // namespace

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
    g_State.authoritativeFrameHashes.erase(pruneFrame);
    g_State.localTouchData.erase(pruneFrame);
    g_State.remoteTouchData.erase(pruneFrame);
}

int OldestGameplaySnapshotFrame()
{
    for (const GameplaySnapshot &snapshot : g_State.rollbackSnapshots)
    {
        if (snapshot.stage == g_GameManager.currentStage)
        {
            return snapshot.frame;
        }
    }

    return -1;
}

int LatestGameplaySnapshotFrame()
{
    for (auto it = g_State.rollbackSnapshots.rbegin(); it != g_State.rollbackSnapshots.rend(); ++it)
    {
        if (it->stage == g_GameManager.currentStage)
        {
            return it->frame;
        }
    }

    return -1;
}

bool IsRollbackFrameTooOld(int frame, int *outOldestSnapshotFrame)
{
    const int oldestSnapshotFrame = OldestGameplaySnapshotFrame();
    if (outOldestSnapshotFrame != nullptr)
    {
        *outOldestSnapshotFrame = oldestSnapshotFrame;
    }

    return frame >= 0 && oldestSnapshotFrame >= 0 && frame < oldestSnapshotFrame;
}

bool QueueRollbackFromFrame(int frame, const char *reason)
{
#if !TH06_ENABLE_PREDICTION_ROLLBACK
    (void)frame;
    (void)reason;
    return false;
#else
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
#endif
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
    g_State.rollbackSnapshotCaptureRequested = false;
    g_State.pendingRollbackFrame = -1;
    g_State.rollbackTargetFrame = 0;
    g_State.rollbackSendFrame = -1;
    g_State.rollbackStage = -1;
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

int LatestRemoteReceivedFrame()
{
    if (g_State.remoteInputs.empty())
    {
        return -1;
    }

    return g_State.remoteInputs.rbegin()->first;
}

Uint64 ComputePeerFreezeStallMs()
{
    if (g_State.lastRttMs >= 0)
    {
        const Uint64 dynamicThreshold = (Uint64)std::clamp(g_State.lastRttMs * 2 + 20, 120, 1000);
        return dynamicThreshold;
    }

    return 120;
}

bool ShouldStallForPeerFreeze(int delayedFrame, Uint64 *outLastRecvAgo, int *outLatestRemoteFrame)
{
    const int latestRemoteFrame = LatestRemoteReceivedFrame();
    if (outLatestRemoteFrame != nullptr)
    {
        *outLatestRemoteFrame = latestRemoteFrame;
    }

    if (latestRemoteFrame >= delayedFrame || g_State.lastRuntimeReceiveTick == 0)
    {
        if (outLastRecvAgo != nullptr)
        {
            *outLastRecvAgo = 0;
        }
        return false;
    }

    const Uint64 lastRecvAgo = SDL_GetTicks64() - g_State.lastRuntimeReceiveTick;
    if (outLastRecvAgo != nullptr)
    {
        *outLastRecvAgo = lastRecvAgo;
    }

    return lastRecvAgo >= ComputePeerFreezeStallMs();
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

#if !TH06_ENABLE_PREDICTION_ROLLBACK
    (void)allowPrediction;
    return false;
#else
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
#endif
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
    if (!Session::IsRemoteNetplaySession() && keyboardState[SDL_SCANCODE_Q])
    {
        return Quick_Quit;
    }
    if (!Session::IsRemoteNetplaySession() && keyboardState[SDL_SCANCODE_R])
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
    pack.ctrl.initSetting.flags = (g_State.predictionRollbackEnabled ? InitSettingFlag_PredictionRollback : 0) |
                                  (g_State.authoritativeModeEnabled ? InitSettingFlag_AuthoritativeMode : 0);
    pack.ctrl.initSetting.sharedSeed =
        NormalizeRollbackStartupSeed(g_State.authoritySharedSeedKnown ? g_State.authoritySharedSeed : g_Rng.seed);
    pack.ctrl.initSetting.hostIsPlayer1 = g_State.hostIsPlayer1 ? 1 : 0;
    pack.ctrl.initSetting.difficulty = (u8)g_GameManager.difficulty;
    pack.ctrl.initSetting.character1 = g_GameManager.character;
    pack.ctrl.initSetting.shotType1 = g_GameManager.shotType;
    pack.ctrl.initSetting.character2 = g_GameManager.character2;
    pack.ctrl.initSetting.shotType2 = g_GameManager.shotType2;
    pack.ctrl.initSetting.practiceMode = g_GameManager.isInPracticeMode ? 1 : 0;
    std::memset(pack.ctrl.initSetting.reserved, 0, sizeof(pack.ctrl.initSetting.reserved));
    TraceLauncherPacket("launcher-send-ping", pack);
    return pack;
}

int CurrentNetFrame()
{
    return std::max(0, g_State.currentNetFrame);
}

void CaptureGameplaySnapshot(int frame)
{
#if !TH06_ENABLE_PREDICTION_ROLLBACK
    (void)frame;
    g_State.rollbackSnapshotCaptureRequested = false;
    return;
#else
    const bool forceCapture = g_State.rollbackSnapshotCaptureRequested;
    if (!g_State.isSessionActive || !g_State.isConnected || !Session::IsRemoteNetplaySession())
    {
        g_State.rollbackSnapshotCaptureRequested = false;
        return;
    }

    if (!CanUseRollbackSnapshots() || g_State.rollbackActive)
    {
        if (!g_State.rollbackActive)
        {
            g_State.rollbackSnapshotCaptureRequested = false;
        }
        return;
    }

    if (g_State.rollbackStage != g_GameManager.currentStage)
    {
        g_State.rollbackSnapshots.clear();
        g_State.rollbackStage = g_GameManager.currentStage;
        g_State.lastConfirmedSyncFrame = frame;
    }

    // Do not let a pre-epoch boundary frame occupy the "first snapshot" slot.
    // Otherwise early gameplay corrections (for example frame 1-14 after ui-exit)
    // have no usable snapshot until the first interval boundary.
    if (frame < g_State.rollbackEpochStartFrame)
    {
        if (!forceCapture)
        {
            return;
        }
    }

    if (!g_State.rollbackSnapshots.empty() && g_State.rollbackSnapshots.back().frame == frame)
    {
        g_State.rollbackSnapshotCaptureRequested = false;
        return;
    }

    if (!forceCapture && !g_State.rollbackSnapshots.empty() && frame != 0 && frame % kRollbackSnapshotInterval != 0)
    {
        return;
    }

    g_State.rollbackSnapshots.emplace_back();
    GameplaySnapshot &snapshot = g_State.rollbackSnapshots.back();

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
    DGS::CaptureSharedGameplaySnapshotState(snapshot);

    while ((int)g_State.rollbackSnapshots.size() > kRollbackMaxSnapshots)
    {
        g_State.rollbackSnapshots.pop_front();
    }

    g_State.rollbackSnapshotCaptureRequested = false;
    if (forceCapture)
    {
        TraceDiagnostic("rollback-snapshot-refresh", "frame=%d snapshots=%d", frame, (int)g_State.rollbackSnapshots.size());
    }
#endif
}

bool RestoreGameplaySnapshot(const GameplaySnapshot &snapshot)
{
#if !TH06_ENABLE_PREDICTION_ROLLBACK
    (void)snapshot;
    return false;
#else
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
    if (!DGS::RestoreSharedGameplaySnapshotState(snapshot))
    {
        return false;
    }
    g_State.delay = snapshot.delay;
    g_State.currentDelayCooldown = snapshot.currentDelayCooldown;
    g_State.currentCtrl = IGC_NONE;
    return true;
#endif
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

    if (isInUi && ShouldFreezeSharedShellUiInput(frame))
    {
        outCtrl = IGC_NONE;
        TraceDiagnostic("resolve-frame-shared-shell-handoff",
                        "frame=%d delayed=%d serial=%u phase=%u handoff=%u final=0", frame, delayedFrame,
                        g_State.shell.shellSerial, (unsigned)g_State.shell.authoritativePhase,
                        g_State.shell.authoritativeHandoffFrame);
        return 0;
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
#if !TH06_ENABLE_PREDICTION_ROLLBACK
    (void)currentFrame;
    (void)mismatchFrame;
    (void)olderThanSnapshotFrame;
    return false;
#else
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
        NoteRecoveryHeuristicRollbackFailure(currentFrame, mismatchFrame, -1, mismatchFrame, "epoch-boundary");
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
        NoteRecoveryHeuristicRollbackFailure(currentFrame, mismatchFrame, oldestSnapshotFrame, mismatchFrame, "too-old");
        if (g_State.pendingRollbackFrame == mismatchFrame)
        {
            g_State.pendingRollbackFrame = -1;
        }
        return false;
    }

    const int latestSnapshotFrame = LatestGameplaySnapshotFrame();
    if (g_State.lastRollbackSnapshotFrame >= 0 && g_State.lastRollbackTargetFrame > g_State.lastRollbackSnapshotFrame &&
        mismatchFrame > g_State.lastRollbackTargetFrame && latestSnapshotFrame >= 0 &&
        latestSnapshotFrame < g_State.lastRollbackTargetFrame)
    {
        TraceDiagnostic("rollback-start-drop-stale-window",
                        "current=%d mismatch=%d lastSnapshot=%d lastTarget=%d latestSnapshot=%d olderThan=%d",
                        currentFrame, mismatchFrame, g_State.lastRollbackSnapshotFrame, g_State.lastRollbackTargetFrame,
                        latestSnapshotFrame, olderThanSnapshotFrame);
        NoteRecoveryHeuristicRollbackFailure(currentFrame, mismatchFrame, latestSnapshotFrame, mismatchFrame,
                                             "stale-window");
        return false;
    }

    const int rollbackFrame = std::max(0, mismatchFrame);
    const int rollbackSourceFrame =
        olderThanSnapshotFrame >= 0 && g_State.lastRollbackSourceFrame >= 0 ? g_State.lastRollbackSourceFrame
                                                                             : mismatchFrame;
    int availableRemoteFrame = std::max(0, rollbackFrame - 1);
    while (g_State.remoteInputs.find(availableRemoteFrame + 1) != g_State.remoteInputs.end())
    {
        ++availableRemoteFrame;
    }

    const int rollbackTargetFrame =
        g_State.predictionRollbackEnabled ? std::min(currentFrame, availableRemoteFrame + g_State.delay) : currentFrame;

    const GameplaySnapshot *snapshot = nullptr;
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
        NoteRecoveryHeuristicRollbackFailure(currentFrame, mismatchFrame, snapshot != nullptr ? snapshot->frame : -1,
                                             rollbackTargetFrame, "no-snapshot");
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
        NoteRecoveryHeuristicRollbackFailure(currentFrame, mismatchFrame, snapshot->frame, rollbackTargetFrame,
                                             "history");
        if (g_State.pendingRollbackFrame == mismatchFrame)
        {
            g_State.pendingRollbackFrame = -1;
        }
        return false;
    }

    if (!RestoreGameplaySnapshot(*snapshot))
    {
        TraceDiagnostic("rollback-start-fail",
                        "current=%d mismatch=%d rollbackFrame=%d target=%d availableRemote=%d snapshot=%d",
                        currentFrame, mismatchFrame, rollbackFrame, rollbackTargetFrame, availableRemoteFrame,
                        snapshot->frame);
        NoteRecoveryHeuristicRollbackFailure(currentFrame, mismatchFrame, snapshot->frame, rollbackTargetFrame,
                                             "restore-fail");
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
    g_State.lastRollbackSourceFrame = rollbackSourceFrame;
    g_State.seedValidationIgnoreUntilFrame =
        std::max(g_State.seedValidationIgnoreUntilFrame,
                 std::max(availableRemoteFrame + g_State.delay, rollbackTargetFrame + kRollbackSnapshotInterval));
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
    NoteRecoveryHeuristicRollbackSuccess(currentFrame, mismatchFrame, snapshot->frame, rollbackTargetFrame);
    SetStatus("rollback catchup...");
    return true;
#endif
}

bool TryStartQueuedRollback(int currentFrame)
{
#if !TH06_ENABLE_PREDICTION_ROLLBACK
    (void)currentFrame;
    return false;
#else
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
        NoteRecoveryHeuristicRollbackFailure(currentFrame, g_State.pendingRollbackFrame, oldestSnapshotFrame,
                                             g_State.pendingRollbackFrame, "queued-too-old");
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
#endif
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

void HandleDesync(int frame)
{
    if (!g_State.isConnected || g_State.isSync || g_State.isTryingReconnect || g_State.recovery.active)
    {
        g_State.desyncStartTick = 0;
        return;
    }

#if TH06_ENABLE_PREDICTION_ROLLBACK
    if (g_State.predictionRollbackEnabled)
    {
        if (g_State.rollbackActive || g_State.pendingRollbackFrame >= 0)
        {
            return;
        }

        const Uint64 now = SDL_GetTicks64();
        if (g_State.desyncStartTick == 0)
        {
            g_State.desyncStartTick = now;
            TraceDiagnostic("desync-observed", "frame=%d pending=%d rollback=%d", frame, g_State.pendingRollbackFrame,
                            g_State.rollbackActive ? 1 : 0);
        }

        bool recoveryStarted = false;
        if (TryHeuristicRollbackOrRecovery(
                frame,
                g_State.recoveryHeuristic.clusterLastMismatchFrame >= 0 ? g_State.recoveryHeuristic.clusterLastMismatchFrame
                                                                        : frame,
                -1, AuthoritativeRecoveryReason_UnrollbackableDesync, "desync-observed", nullptr, &recoveryStarted))
        {
            return;
        }

        if (now - g_State.desyncStartTick >= kDesyncRecoveryTimeoutMs)
        {
            TraceDiagnostic("desync-escalate-recovery", "frame=%d elapsed=%llu", frame,
                            (unsigned long long)(now - g_State.desyncStartTick));
            TryStartAuthoritativeRecovery(frame, AuthoritativeRecoveryReason_UnrollbackableDesync);
        }
        return;
    }
#endif

    if (g_State.resyncTriggered && g_State.resyncTargetFrame <= frame)
    {
        g_State.resyncTriggered = false;
        ReceiveRuntimePackets();
        g_Rng.seed = 0;
        g_Rng.generationCount = 0;
        g_State.sessionRngSeed = 0;
        g_State.sharedRngSeedCaptured = false;
        g_State.remoteInputs.clear();
        g_State.predictedRemoteInputs.clear();
        g_State.remoteSeeds.clear();
        g_State.remoteCtrls.clear();
        g_State.predictedRemoteCtrls.clear();
        g_State.remoteTouchData.clear();
        g_State.pendingRollbackFrame = -1;
        g_State.currentCtrl = IGC_NONE;
        g_State.isSync = true;
        g_State.desyncStartTick = 0;
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

u16 ResolveFrameInput(int frame, bool isInUi, InGameCtrlType &outCtrl, bool &outRollbackStarted,
                      bool &outStallForPeer)
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
    outStallForPeer = false;
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
#if TH06_ENABLE_PREDICTION_ROLLBACK
    const bool allowPrediction =
        g_State.predictionRollbackEnabled && CanUseRollbackSnapshots() && !isInUi && !requiresAccurateRemoteInput;
    const bool canRollbackNow =
        g_State.predictionRollbackEnabled && CanUseRollbackSnapshots() && !isInUi && !rollbackEpochBarrier;
#else
    const bool allowPrediction = false;
    const bool canRollbackNow = false;
#endif
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

#if TH06_ENABLE_PREDICTION_ROLLBACK
        if (g_State.predictionRollbackEnabled)
        {
            if (!canRollbackNow)
            {
                TraceDiagnostic("resolve-frame-sync-seed-mismatch",
                                "frame=%d remoteSeed=%u localSeed=%u pending=%d canRollback=%d ignored=1 "
                                "reason=unrollbackable",
                                syncFrame, remoteSeedIt->second, localSeedIt->second, g_State.pendingRollbackFrame,
                                canRollbackNow ? 1 : 0);
                NoteRecoveryHeuristicMismatch(syncFrame, "unrollbackable-mismatch");
                bool heuristicRollbackStarted = false;
                bool heuristicRecoveryStarted = false;
                if (TryHeuristicRollbackOrRecovery(frame, syncFrame, -1,
                                                   AuthoritativeRecoveryReason_UnrollbackableDesync,
                                                   "unrollbackable-mismatch", &heuristicRollbackStarted,
                                                   &heuristicRecoveryStarted))
                {
                    if (heuristicRecoveryStarted)
                    {
                        outStallForPeer = true;
                        return true;
                    }
                    if (heuristicRollbackStarted)
                    {
                        outRollbackStarted = true;
                        return true;
                    }
                }
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
                g_State.desyncStartTick = 0;
                return false;
            }

            g_State.isSync = false;
            NoteRecoveryHeuristicMismatch(syncFrame, "seed-mismatch");
            const auto trySeedMismatchRollbackRetry = [&](int olderThanSnapshotFrame, const char *reason) -> bool {
                const int rollbackSourceFrame = g_State.lastRollbackSourceFrame;
                const bool isPostRollbackLatentDrift =
                    reason != nullptr && std::strcmp(reason, "post-rollback-latent-drift") == 0;
                const auto noteLatentDriftRollbackPairFailure = [&](const char *failureReason) {
                    if (!isPostRollbackLatentDrift)
                    {
                        return;
                    }

                    const int failedSnapshotFrame = g_State.lastRollbackSnapshotFrame;
                    const int failedTargetFrame = g_State.lastRollbackTargetFrame;
                    if (failedSnapshotFrame < 0 || failedTargetFrame < 0)
                    {
                        return;
                    }

                    NoteRecoveryHeuristicRollbackFailure(frame, syncFrame, failedSnapshotFrame, failedTargetFrame,
                                                         failureReason);
                };
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
                    noteLatentDriftRollbackPairFailure("latent-drift-repeat");
                    return false;
                }

                if (isPostRollbackLatentDrift && rollbackSourceFrame >= 0 &&
                    g_State.lastLatentRetrySourceFrame == rollbackSourceFrame)
                {
                    TraceDiagnostic("resolve-frame-sync-seed-mismatch-retry-skip",
                                    "frame=%d remoteSeed=%u localSeed=%u olderThan=%d reason=%s source=%d skipped=1",
                                    syncFrame, remoteSeedIt->second, localSeedIt->second, olderThanSnapshotFrame,
                                    reason != nullptr ? reason : "-", rollbackSourceFrame);
                    noteLatentDriftRollbackPairFailure("latent-drift-loop");
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

                if (isPostRollbackLatentDrift && rollbackSourceFrame >= 0)
                {
                    g_State.lastLatentRetrySourceFrame = rollbackSourceFrame;
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
                g_State.desyncStartTick = 0;
                return false;
            }
            if (canRetryPostRollbackDrift &&
                trySeedMismatchRollbackRetry(g_State.lastRollbackSnapshotFrame, "post-rollback-latent-drift"))
            {
                return true;
            }
                const bool lateAuthoritativeFrame =
                    g_State.remoteFramesPendingRollbackCheck.find(syncFrame) != g_State.remoteFramesPendingRollbackCheck.end();
                if (lateAuthoritativeFrame && syncFrame <= g_State.seedValidationIgnoreUntilFrame)
                {
                    TraceDiagnostic("resolve-frame-sync-seed-mismatch",
                                    "frame=%d remoteSeed=%u localSeed=%u pending=%d canRollback=%d ignored=1 "
                                    "reason=rollback-grace graceUntil=%d",
                                    syncFrame, remoteSeedIt->second, localSeedIt->second, g_State.pendingRollbackFrame,
                                    canRollbackNow ? 1 : 0, g_State.seedValidationIgnoreUntilFrame);
                    g_State.isSync = true;
                    g_State.desyncStartTick = 0;
                    return false;
                }
                if (lateAuthoritativeFrame && TryStartAutomaticRollback(frame, syncFrame))
                {
                    TraceDiagnostic("resolve-frame-sync-seed-mismatch-retry",
                                    "frame=%d remoteSeed=%u localSeed=%u olderThan=%d reason=late-authoritative repeated=0",
                                    syncFrame, remoteSeedIt->second, localSeedIt->second, -1);
                    outRollbackStarted = true;
                    return true;
                }
                TraceDiagnostic("resolve-frame-sync-seed-mismatch",
                                "frame=%d remoteSeed=%u localSeed=%u pending=%d canRollback=%d ignored=1",
                                syncFrame, remoteSeedIt->second, localSeedIt->second, g_State.pendingRollbackFrame,
                                canRollbackNow ? 1 : 0);
                bool heuristicRollbackStarted = false;
                bool heuristicRecoveryStarted = false;
                if (TryHeuristicRollbackOrRecovery(frame, syncFrame, -1,
                                                   AuthoritativeRecoveryReason_UnrollbackableDesync,
                                                   "seed-mismatch-desynced", &heuristicRollbackStarted,
                                                   &heuristicRecoveryStarted))
                {
                    if (heuristicRecoveryStarted)
                    {
                        outStallForPeer = true;
                        return true;
                    }
                    if (heuristicRollbackStarted)
                    {
                        outRollbackStarted = true;
                        return true;
                    }
                }
                SetStatus(g_State.pendingRollbackFrame >= 0 ? "rollback pending..." : "desynced");
                return false;
            }
#endif

        g_State.isSync = isInUi;
        if (g_State.isSync)
        {
            g_State.desyncStartTick = 0;
        }

#if TH06_ENABLE_PREDICTION_ROLLBACK
        if (!canRollbackNow)
        {
            return false;
        }

        g_State.isSync = false;
        if (TryStartAutomaticRollback(frame, syncFrame))
        {
            outRollbackStarted = true;
            g_State.desyncStartTick = 0;
            return true;
        }
#endif
        return false;
    };

    if (!allowPrediction)
    {
        const Uint64 waitUntil = SDL_GetTicks64() + ComputePeerFreezeStallMs();
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
            Uint64 lastRecvAgo = 0;
            int latestRemoteFrame = -1;
            if (g_State.lastRuntimeReceiveTick != 0)
            {
                lastRecvAgo = SDL_GetTicks64() - g_State.lastRuntimeReceiveTick;
            }
            latestRemoteFrame = LatestRemoteReceivedFrame();
            if (lastRecvAgo < kPeerFreezeDisconnectMs)
            {
                TraceDiagnostic("resolve-frame-stall",
                                "frame=%d delayed=%d reason=missing-accurate-remote prediction=0 lastRecvAgo=%llu latestRemote=%d threshold=%llu",
                                frame, delayedFrame, (unsigned long long)lastRecvAgo, latestRemoteFrame,
                                (unsigned long long)ComputePeerFreezeStallMs());
                SetStatus("waiting for peer...");
                outStallForPeer = true;
                return 0;
            }
            TraceDiagnostic("resolve-frame-disconnect", "frame=%d delayed=%d reason=missing-remote-no-prediction", frame,
                            delayedFrame);
            g_State.isConnected = false;
            g_State.isTryingReconnect = true;
            g_State.reconnectIssued = false;
            g_State.reconnectStartTick = SDL_GetTicks64();
            g_State.reconnectReason = AuthoritativeRecoveryReason_ReconnectTimeout;
            g_State.currentCtrl = IGC_NONE;
            SetStatus("disconnected");
            return 0;
        }
    }
#if TH06_ENABLE_PREDICTION_ROLLBACK
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
            g_State.reconnectStartTick = SDL_GetTicks64();
            g_State.reconnectReason = AuthoritativeRecoveryReason_ReconnectTimeout;
            g_State.currentCtrl = IGC_NONE;
            SetStatus("disconnected");
            return 0;
        }

        if (usedPrediction)
        {
            Uint64 lastRecvAgo = 0;
            int latestRemoteFrame = -1;
            if (ShouldStallForPeerFreeze(delayedFrame, &lastRecvAgo, &latestRemoteFrame))
            {
                if (lastRecvAgo >= kPeerFreezeDisconnectMs)
                {
                    TraceDiagnostic("resolve-frame-disconnect",
                                    "frame=%d delayed=%d reason=prediction-timeout lastRecvAgo=%llu latestRemote=%d",
                                    frame, delayedFrame, (unsigned long long)lastRecvAgo, latestRemoteFrame);
                    g_State.isConnected = false;
                    g_State.isTryingReconnect = true;
                    g_State.reconnectIssued = false;
                    g_State.reconnectStartTick = SDL_GetTicks64();
                    g_State.reconnectReason = AuthoritativeRecoveryReason_PredictionTimeout;
                    g_State.currentCtrl = IGC_NONE;
                    SetStatus("disconnected");
                    return 0;
                }

                TraceDiagnostic("resolve-frame-stall",
                                "frame=%d delayed=%d reason=peer-runtime-gap prediction=1 lastRecvAgo=%llu latestRemote=%d threshold=%llu",
                                frame, delayedFrame, (unsigned long long)lastRecvAgo, latestRemoteFrame,
                                (unsigned long long)ComputePeerFreezeStallMs());
                SetStatus("waiting for peer...");
                outStallForPeer = true;
                return 0;
            }
        }
        else if (tryCompareSyncForFrame(delayedFrame))
        {
            return 0;
        }
    }
#endif

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

    const Uint64 now = SDL_GetTicks64();
    if (g_State.reconnectStartTick == 0)
    {
        g_State.reconnectStartTick = now;
    }
    if (now - g_State.reconnectStartTick >= kDisconnectTimeoutMs)
    {
        const AuthoritativeRecoveryReason reason = g_State.reconnectReason == AuthoritativeRecoveryReason_PredictionTimeout
                                                       ? AuthoritativeRecoveryReason_PredictionTimeout
                                                       : AuthoritativeRecoveryReason_ReconnectTimeout;
        NoteRecoveryHeuristicMismatch(frame, reason == AuthoritativeRecoveryReason_PredictionTimeout
                                                 ? "prediction-timeout"
                                                 : "reconnect-timeout");
        TraceDiagnostic("recovery-search-decision",
                        "stage=%d frame=%d epoch=%d clusterFirst=%d clusterLast=%d candidateFrame=%d snapshotFrame=%d "
                        "targetFrame=%d decision=recovery reason=%s",
                        g_GameManager.currentStage, frame, g_State.rollbackEpochStartFrame,
                        g_State.recoveryHeuristic.clusterFirstMismatchFrame,
                        g_State.recoveryHeuristic.clusterLastMismatchFrame, -1, -1, -1,
                        reason == AuthoritativeRecoveryReason_PredictionTimeout ? "prediction-timeout"
                                                                                 : "reconnect-timeout");
        TraceDiagnostic("reconnect-escalate-recovery", "frame=%d reason=%u elapsed=%llu", frame, reason,
                        (unsigned long long)(now - g_State.reconnectStartTick));
        TryStartAuthoritativeRecovery(frame, reason);
        return;
    }

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
        g_Rng.generationCount = 0;
        g_State.sessionRngSeed = 0;
        g_State.sharedRngSeedCaptured = false;
        g_State.isConnected = true;
        g_State.isTryingReconnect = false;
        g_State.reconnectIssued = false;
        g_State.reconnectStartTick = 0;
        g_State.desyncStartTick = 0;
        g_State.reconnectReason = AuthoritativeRecoveryReason_None;
        g_State.remoteInputs.clear();
        g_State.predictedRemoteInputs.clear();
        g_State.remoteSeeds.clear();
        g_State.remoteCtrls.clear();
        g_State.predictedRemoteCtrls.clear();
        g_State.remoteTouchData.clear();
        g_State.pendingRollbackFrame = -1;
        g_State.currentCtrl = IGC_NONE;
        g_State.isSync = true;
        ResetRecoveryHeuristicState();
        TraceDiagnostic("reconnect-success", "frame=%d", frame);
        SetStatus("connected");
    }
}


} // namespace th06::Netplay
