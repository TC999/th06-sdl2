#include "NetplayInternal.hpp"

#include <vector>

namespace th06::Netplay
{
namespace
{
constexpr int kRecoveryHeuristicWindowFrames = 120;
constexpr int kRecoveryHeuristicResetCleanFrames = 30;
constexpr size_t kRecoveryHeuristicMaxCandidates = 8;
constexpr size_t kRecoveryHeuristicMaxMismatchFrames = 32;
constexpr size_t kRecoveryHeuristicMaxFailedRollbacks = 32;

struct RecoveryCandidate
{
    int frame = -1;
    int snapshotFrame = -1;
    int targetFrame = -1;
    bool viable = false;
    const char *reason = "unknown";
};

bool IsRecoveryHeuristicGameplayActive()
{
    return g_State.isSessionActive && g_State.startupActivationComplete && !g_State.isWaitingForStartup &&
           !g_State.isTryingReconnect && !g_State.recovery.active && CanUseRollbackSnapshots();
}

void PruneRecoveryHeuristicWindow(int currentFrame)
{
    auto &state = g_State.recoveryHeuristic;
    while (!state.mismatchFrames.empty() && currentFrame - state.mismatchFrames.front() > kRecoveryHeuristicWindowFrames)
    {
        state.mismatchFrames.pop_front();
    }
    while (!state.failedRollbacks.empty() &&
           currentFrame - state.failedRollbacks.front().mismatchFrame > kRecoveryHeuristicWindowFrames)
    {
        state.failedRollbacks.pop_front();
    }

    if (!state.mismatchFrames.empty())
    {
        state.clusterFirstMismatchFrame = state.mismatchFrames.front();
        state.clusterLastMismatchFrame = state.mismatchFrames.back();
        state.clusterActive = true;
    }
}

void PushMismatchFrame(int frame)
{
    if (frame < 0)
    {
        return;
    }

    auto &state = g_State.recoveryHeuristic;
    if (state.mismatchFrames.empty() || state.mismatchFrames.back() != frame)
    {
        state.mismatchFrames.push_back(frame);
        while (state.mismatchFrames.size() > kRecoveryHeuristicMaxMismatchFrames)
        {
            state.mismatchFrames.pop_front();
        }
    }
    state.clusterActive = true;
    if (state.clusterFirstMismatchFrame < 0 || frame < state.clusterFirstMismatchFrame)
    {
        state.clusterFirstMismatchFrame = frame;
    }
    if (frame > state.clusterLastMismatchFrame)
    {
        state.clusterLastMismatchFrame = frame;
    }
    state.consecutiveCleanCommittedFrames = 0;
    PruneRecoveryHeuristicWindow(frame);
}

bool HasFailedRollbackPair(int snapshotFrame, int targetFrame)
{
    for (const DesyncHeuristicFailedRollback &failed : g_State.recoveryHeuristic.failedRollbacks)
    {
        if (failed.snapshotFrame == snapshotFrame && failed.targetFrame == targetFrame)
        {
            return true;
        }
    }
    return false;
}

void RecordFailedRollbackPair(int mismatchFrame, int snapshotFrame, int targetFrame)
{
    if (snapshotFrame < 0 || targetFrame < 0)
    {
        return;
    }
    if (HasFailedRollbackPair(snapshotFrame, targetFrame))
    {
        return;
    }

    auto &failed = g_State.recoveryHeuristic.failedRollbacks;
    failed.push_back({mismatchFrame, snapshotFrame, targetFrame});
    while (failed.size() > kRecoveryHeuristicMaxFailedRollbacks)
    {
        failed.pop_front();
    }
}

void AppendCandidate(std::vector<int> &frames, int frame)
{
    if (frame < 0)
    {
        return;
    }
    for (int existing : frames)
    {
        if (existing == frame)
        {
            return;
        }
    }
    if (frames.size() < kRecoveryHeuristicMaxCandidates)
    {
        frames.push_back(frame);
    }
}

int ComputeRollbackTargetFrameForCandidate(int currentFrame, int candidateFrame)
{
    const int rollbackFrame = std::max(0, candidateFrame);
    int availableRemoteFrame = std::max(0, rollbackFrame - 1);
    while (g_State.remoteInputs.find(availableRemoteFrame + 1) != g_State.remoteInputs.end())
    {
        ++availableRemoteFrame;
    }

    if (!g_State.predictionRollbackEnabled)
    {
        return currentFrame;
    }

    return std::min(currentFrame, availableRemoteFrame + g_State.delay);
}

RecoveryCandidate EvaluateRecoveryCandidate(int currentFrame, int candidateFrame, int olderThanSnapshotFrame)
{
    RecoveryCandidate candidate {};
    candidate.frame = candidateFrame;
    candidate.reason = "candidate";

    if (candidateFrame < 0)
    {
        candidate.reason = "invalid-frame";
        return candidate;
    }

    if (currentFrame - candidateFrame > kRecoveryHeuristicWindowFrames)
    {
        candidate.reason = "outside-window";
        return candidate;
    }

    if (!IsRollbackEpochFrame(candidateFrame))
    {
        candidate.reason = "epoch-boundary";
        return candidate;
    }

    if (IsMenuTransitionFrame(candidateFrame))
    {
        candidate.reason = "ui-boundary";
        return candidate;
    }

    int oldestSnapshotFrame = -1;
    if (IsRollbackFrameTooOld(candidateFrame, &oldestSnapshotFrame))
    {
        candidate.reason = "too-old";
        return candidate;
    }

    const int rollbackTargetFrame = ComputeRollbackTargetFrameForCandidate(currentFrame, candidateFrame);
    candidate.targetFrame = rollbackTargetFrame;

    const GameplaySnapshot *snapshot = nullptr;
    int requiredStartFrame = -1;
    int missingLocalFrame = -1;
    int missingRemoteFrame = -1;
    bool skippedFailedPair = false;

    for (auto it = g_State.rollbackSnapshots.rbegin(); it != g_State.rollbackSnapshots.rend(); ++it)
    {
        if (it->stage != g_GameManager.currentStage || it->frame > candidateFrame || !IsRollbackEpochFrame(it->frame))
        {
            continue;
        }
        if (olderThanSnapshotFrame >= 0 && it->frame >= olderThanSnapshotFrame)
        {
            continue;
        }
        if (HasFailedRollbackPair(it->frame, rollbackTargetFrame))
        {
            skippedFailedPair = true;
            continue;
        }
        if (!HasRollbackReplayHistory(it->frame, rollbackTargetFrame, &requiredStartFrame, &missingLocalFrame,
                                      &missingRemoteFrame))
        {
            continue;
        }
        snapshot = &(*it);
        break;
    }

    if (snapshot == nullptr)
    {
        candidate.reason = skippedFailedPair ? "failed-pair" : "no-snapshot";
        return candidate;
    }

    candidate.snapshotFrame = snapshot->frame;
    candidate.viable = true;
    candidate.reason = "ok";
    return candidate;
}

void TraceRecoverySearchCandidate(int currentFrame, const RecoveryCandidate &candidate)
{
    TraceDiagnostic(
        "recovery-search-candidate",
        "stage=%d frame=%d epoch=%d clusterFirst=%d clusterLast=%d candidateFrame=%d snapshotFrame=%d targetFrame=%d "
        "viable=%d reason=%s",
        g_GameManager.currentStage, currentFrame, g_State.rollbackEpochStartFrame,
        g_State.recoveryHeuristic.clusterFirstMismatchFrame, g_State.recoveryHeuristic.clusterLastMismatchFrame,
        candidate.frame, candidate.snapshotFrame, candidate.targetFrame, candidate.viable ? 1 : 0, candidate.reason);
}

} // namespace

void ResetRecoveryHeuristicState()
{
    g_State.recoveryHeuristic = {};
}

void NoteRecoveryHeuristicMismatch(int frame, const char *reason)
{
    (void)reason;
    PushMismatchFrame(frame);
}

void NoteRecoveryHeuristicRollbackFailure(int currentFrame, int mismatchFrame, int snapshotFrame, int targetFrame,
                                          const char *reason)
{
    (void)reason;
    PushMismatchFrame(mismatchFrame >= 0 ? mismatchFrame : currentFrame);
    auto &state = g_State.recoveryHeuristic;
    state.failureCountInCluster++;
    RecordFailedRollbackPair(mismatchFrame >= 0 ? mismatchFrame : currentFrame, snapshotFrame, targetFrame);
    PruneRecoveryHeuristicWindow(currentFrame);
}

void NoteRecoveryHeuristicRollbackSuccess(int currentFrame, int mismatchFrame, int snapshotFrame, int targetFrame)
{
    (void)currentFrame;
    (void)mismatchFrame;
    (void)snapshotFrame;
    g_State.recoveryHeuristic.lastSuccessfulRollbackTargetFrame = targetFrame;
    g_State.recoveryHeuristic.consecutiveCleanCommittedFrames = 0;
}

void NoteRecoveryHeuristicCommittedFrame(int frame)
{
    auto &state = g_State.recoveryHeuristic;
    if (!state.clusterActive)
    {
        return;
    }

    if (!IsRecoveryHeuristicGameplayActive())
    {
        state.consecutiveCleanCommittedFrames = 0;
        return;
    }

    if (g_State.isSync && !g_State.rollbackActive && g_State.pendingRollbackFrame < 0)
    {
        ++state.consecutiveCleanCommittedFrames;
        if (state.consecutiveCleanCommittedFrames >= kRecoveryHeuristicResetCleanFrames)
        {
            TraceDiagnostic("recovery-search-reset", "stage=%d frame=%d epoch=%d clusterFirst=%d clusterLast=%d",
                            g_GameManager.currentStage, frame, g_State.rollbackEpochStartFrame,
                            state.clusterFirstMismatchFrame, state.clusterLastMismatchFrame);
            ResetRecoveryHeuristicState();
        }
        return;
    }

    state.consecutiveCleanCommittedFrames = 0;
}

bool TryHeuristicRollbackOrRecovery(int currentFrame, int preferredMismatchFrame, int olderThanSnapshotFrame,
                                    AuthoritativeRecoveryReason recoveryReason, const char *reason,
                                    bool *outRollbackStarted, bool *outRecoveryStarted)
{
    if (outRollbackStarted != nullptr)
    {
        *outRollbackStarted = false;
    }
    if (outRecoveryStarted != nullptr)
    {
        *outRecoveryStarted = false;
    }

    if (!IsRecoveryHeuristicGameplayActive())
    {
        return false;
    }

    PushMismatchFrame(preferredMismatchFrame >= 0 ? preferredMismatchFrame : currentFrame);

    TraceDiagnostic("recovery-search-begin",
                    "stage=%d frame=%d epoch=%d clusterFirst=%d clusterLast=%d preferred=%d pending=%d rollback=%d "
                    "reason=%s failures=%d",
                    g_GameManager.currentStage, currentFrame, g_State.rollbackEpochStartFrame,
                    g_State.recoveryHeuristic.clusterFirstMismatchFrame, g_State.recoveryHeuristic.clusterLastMismatchFrame,
                    preferredMismatchFrame, g_State.pendingRollbackFrame, g_State.rollbackActive ? 1 : 0,
                    reason != nullptr ? reason : "-", g_State.recoveryHeuristic.failureCountInCluster);

    std::vector<int> candidates;
    AppendCandidate(candidates, preferredMismatchFrame);
    AppendCandidate(candidates, g_State.pendingRollbackFrame);
    AppendCandidate(candidates, g_State.lastRollbackMismatchFrame);
    AppendCandidate(candidates, g_State.recoveryHeuristic.clusterLastMismatchFrame);
    AppendCandidate(candidates, g_State.recoveryHeuristic.clusterFirstMismatchFrame);
    for (auto it = g_State.recoveryHeuristic.mismatchFrames.rbegin();
         it != g_State.recoveryHeuristic.mismatchFrames.rend() && candidates.size() < kRecoveryHeuristicMaxCandidates;
         ++it)
    {
        AppendCandidate(candidates, *it);
    }

    for (int candidateFrame : candidates)
    {
        const int candidateOlderThan =
            candidateFrame == preferredMismatchFrame && olderThanSnapshotFrame >= 0 ? olderThanSnapshotFrame : -1;
        const RecoveryCandidate candidate = EvaluateRecoveryCandidate(currentFrame, candidateFrame, candidateOlderThan);
        TraceRecoverySearchCandidate(currentFrame, candidate);
        if (!candidate.viable)
        {
            continue;
        }

        const int tryOlderThan = candidate.snapshotFrame >= 0 ? candidate.snapshotFrame + 1 : -1;
        if (TryStartAutomaticRollback(currentFrame, candidate.frame, tryOlderThan))
        {
            if (outRollbackStarted != nullptr)
            {
                *outRollbackStarted = true;
            }
            TraceDiagnostic("recovery-search-decision",
                            "stage=%d frame=%d epoch=%d clusterFirst=%d clusterLast=%d candidateFrame=%d "
                            "snapshotFrame=%d targetFrame=%d decision=rollback reason=%s",
                            g_GameManager.currentStage, currentFrame, g_State.rollbackEpochStartFrame,
                            g_State.recoveryHeuristic.clusterFirstMismatchFrame,
                            g_State.recoveryHeuristic.clusterLastMismatchFrame, candidate.frame, candidate.snapshotFrame,
                            candidate.targetFrame, reason != nullptr ? reason : "-");
            return true;
        }
    }

    const bool repeatedFailuresOnly = g_State.recoveryHeuristic.failureCountInCluster >= 2;
    const char *decisionReason = repeatedFailuresOnly ? "failed-candidates" : (reason != nullptr ? reason : "-");
    TraceDiagnostic("recovery-search-decision",
                    "stage=%d frame=%d epoch=%d clusterFirst=%d clusterLast=%d candidateFrame=%d snapshotFrame=%d "
                    "targetFrame=%d decision=recovery reason=%s",
                    g_GameManager.currentStage, currentFrame, g_State.rollbackEpochStartFrame,
                    g_State.recoveryHeuristic.clusterFirstMismatchFrame,
                    g_State.recoveryHeuristic.clusterLastMismatchFrame, preferredMismatchFrame, -1, -1, decisionReason);

    if (!TryStartAuthoritativeRecovery(currentFrame, recoveryReason))
    {
        return false;
    }

    if (outRecoveryStarted != nullptr)
    {
        *outRecoveryStarted = true;
    }
    return true;
}

} // namespace th06::Netplay
