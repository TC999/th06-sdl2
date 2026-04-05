#include "NetplayInternal.hpp"

namespace th06::Netplay
{
void ResetRecoveryHeuristicState()
{
    g_State.recoveryHeuristic = {};
}

void NoteRecoveryHeuristicMismatch(int frame, const char *reason)
{
    (void)frame;
    (void)reason;
}

void NoteRecoveryHeuristicRollbackFailure(int currentFrame, int mismatchFrame, int snapshotFrame, int targetFrame,
                                          const char *reason)
{
    (void)currentFrame;
    (void)mismatchFrame;
    (void)snapshotFrame;
    (void)targetFrame;
    (void)reason;
}

void NoteRecoveryHeuristicRollbackSuccess(int currentFrame, int mismatchFrame, int snapshotFrame, int targetFrame)
{
    (void)currentFrame;
    (void)mismatchFrame;
    (void)snapshotFrame;
    (void)targetFrame;
}

void NoteRecoveryHeuristicCommittedFrame(int frame)
{
    (void)frame;
}

bool TryHeuristicRollbackOrRecovery(int currentFrame, int preferredMismatchFrame, int olderThanSnapshotFrame,
                                    AuthoritativeRecoveryReason recoveryReason, const char *reason,
                                    bool *outRollbackStarted, bool *outRecoveryStarted)
{
    (void)currentFrame;
    (void)preferredMismatchFrame;
    (void)olderThanSnapshotFrame;
    (void)recoveryReason;
    (void)reason;
    if (outRollbackStarted != nullptr)
    {
        *outRollbackStarted = false;
    }
    if (outRecoveryStarted != nullptr)
    {
        *outRecoveryStarted = false;
    }
    return false;
}
} // namespace th06::Netplay