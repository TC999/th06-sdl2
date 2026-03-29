#include "NetplayInternal.hpp"

#include "utils.hpp"

namespace th06::Netplay
{
namespace
{
enum PauseMenuSelection
{
    PauseSelection_Unpause = 0,
    PauseSelection_Quit = 1,
    PauseSelection_QuitYes = 2,
    PauseSelection_QuitNo = 3
};

enum RetryMenuSelection
{
    RetrySelection_Yes = 0,
    RetrySelection_No = 1
};

enum LegacyPauseMenuState
{
    GAME_MENU_PAUSE_OPENING,
    GAME_MENU_PAUSE_CURSOR_UNPAUSE,
    GAME_MENU_PAUSE_CURSOR_QUIT,
    GAME_MENU_PAUSE_SELECTED_UNPAUSE,
    GAME_MENU_QUIT_CURSOR_YES,
    GAME_MENU_QUIT_CURSOR_NO,
    GAME_MENU_QUIT_SELECTED_YES,
};

enum LegacyRetryMenuState
{
    RETRY_MENU_OPENING,
    RETRY_MENU_CURSOR_YES,
    RETRY_MENU_CURSOR_NO,
    RETRY_MENU_SELECTED_YES,
    RETRY_MENU_SELECTED_NO,
};

constexpr int GAME_MENU_SPRITE_TITLE_PAUSE = 0;
constexpr int GAME_MENU_SPRITE_CURSOR_UNPAUSE = 1;
constexpr int GAME_MENU_SPRITE_CURSOR_QUIT = 2;
constexpr int GAME_MENU_SPRITE_TITLE_QUIT = 3;
constexpr int GAME_MENU_SPRITE_CURSOR_YES = 4;
constexpr int GAME_MENU_SPRITE_CURSOR_NO = 5;

constexpr int GAME_MENU_SPRITES_START_PAUSE = GAME_MENU_SPRITE_TITLE_PAUSE;
constexpr int GAME_MENU_SPRITES_END_PAUSE = GAME_MENU_SPRITE_CURSOR_QUIT + 1;
constexpr int GAME_MENU_SPRITES_START_QUIT = GAME_MENU_SPRITE_TITLE_QUIT;
constexpr int GAME_MENU_SPRITES_END_QUIT = GAME_MENU_SPRITE_CURSOR_NO + 1;

constexpr int RETRY_MENU_SPRITE_TITLE = 0;
constexpr int RETRY_MENU_SPRITE_RETRIES_LABEL = 1;
constexpr int RETRY_MENU_SPRITE_YES = 2;
constexpr int RETRY_MENU_SPRITE_NO = 3;
constexpr int RETRY_MENU_SPRITE_RETRIES_NUMBER = 4;

constexpr int RETRY_MENU_SPRITES_START = RETRY_MENU_SPRITE_TITLE;
constexpr int RETRY_MENU_SPRITES_END = RETRY_MENU_SPRITE_NO + 1;

int LocalPlayerIndex()
{
    return IsLocalPlayer1() ? 0 : 1;
}

int RemotePlayerIndex()
{
    return 1 - LocalPlayerIndex();
}

int GetSharedShellLeadFrames()
{
    return std::max(4, g_State.delay * 2 + 2);
}

const char *GetPauseShellEntryBlockReason()
{
    if (IsAuthoritativeRecoveryFreezeActive())
    {
        return "recovery-freeze";
    }
    if (g_State.isTryingReconnect)
    {
        return "reconnecting";
    }
    if (g_State.rollbackActive)
    {
        return "rollback-active";
    }
    if (g_State.pendingRollbackFrame >= 0)
    {
        return "rollback-pending";
    }
    if (!g_State.isSync)
    {
        return "desynced";
    }

    const int handoffFrame = CurrentNetFrame() + GetSharedShellLeadFrames();
    const int confirmedFrame = std::max(0, g_State.lastConfirmedSyncFrame);
    if (handoffFrame - confirmedFrame >= kKeyPackFrameCount)
    {
        return "remote-gap";
    }

    return nullptr;
}

void SetLocalPausePresentationHold(bool active, const char *reason)
{
    if (g_State.shell.localPausePresentationHold == active)
    {
        return;
    }

    g_State.shell.localPausePresentationHold = active;
    TraceDiagnostic(active ? "shell-presentation-hold-on" : "shell-presentation-hold-off",
                    "reason=%s frame=%d serial=%u kind=%u phase=%u handoff=%u",
                    reason != nullptr ? reason : "-", CurrentNetFrame(), g_State.shell.shellSerial,
                    (unsigned)g_State.shell.shellKind, (unsigned)g_State.shell.authoritativePhase,
                    g_State.shell.authoritativeHandoffFrame);
}

void SetSharedShellUiPhaseBarrierBypass(int frame, bool isInUi)
{
    g_State.shell.suppressUiPhaseBarrier = true;
    g_State.shell.suppressUiPhaseBarrierFrame = frame;
    g_State.shell.suppressUiPhaseBarrierIsInUi = isInUi;
}

bool IsPauseConfirmNode(SharedShellNode node)
{
    return node == SharedShellNode_PauseFocusQuitYes || node == SharedShellNode_PauseFocusQuitNo;
}

bool IsClosingNode(SharedShellNode node)
{
    return node == SharedShellNode_PauseClosingResume || node == SharedShellNode_PauseClosingQuit ||
           node == SharedShellNode_RetryClosingRetry || node == SharedShellNode_RetryClosingContinue;
}

int GetClosingThreshold(SharedShellNode node)
{
    switch (node)
    {
    case SharedShellNode_PauseClosingResume:
    case SharedShellNode_PauseClosingQuit:
    case SharedShellNode_RetryClosingContinue:
        return 20;
    case SharedShellNode_RetryClosingRetry:
        return 30;
    default:
        return 0;
    }
}

u16 NextShellSerialBase()
{
    const u16 candidate = (u16)(g_State.shell.shellSerial + 1);
    return std::max<u16>(1, std::max(g_State.shell.nextShellSerial, candidate));
}

void ResetSharedShellStatePreserving(u16 nextShellSerial)
{
    if (g_State.shell.localPausePresentationHold)
    {
        TraceDiagnostic("shell-presentation-hold-off", "reason=reset frame=%d serial=%u", CurrentNetFrame(),
                        g_State.shell.shellSerial);
    }
    g_State.shell = ShellRuntimeState();
    g_State.shell.nextShellSerial = std::max<u16>(1, nextShellSerial);
}

u8 PackVoteMask(const u8 votes[2])
{
    return (u8)((votes[0] & 0x0f) | ((votes[1] & 0x0f) << 4));
}

void UnpackVoteMask(u8 mask, u8 votes[2])
{
    votes[0] = (u8)(mask & 0x0f);
    votes[1] = (u8)((mask >> 4) & 0x0f);
}

void CopyAuthoritativeToPredicted()
{
    g_State.shell.predictedPhase = g_State.shell.authoritativePhase;
    g_State.shell.predictedNode = g_State.shell.authoritativeNode;
    g_State.shell.predictedTransitionReason = g_State.shell.authoritativeTransitionReason;
    g_State.shell.predictedPhaseFrames = g_State.shell.authoritativePhaseFrames;
    g_State.shell.predictedHandoffFrame = g_State.shell.authoritativeHandoffFrame;
    for (int i = 0; i < 2; ++i)
    {
        g_State.shell.predictedSelection[i] = g_State.shell.authoritativeSelection[i];
        g_State.shell.predictedVotes[i] = g_State.shell.authoritativeVotes[i];
    }
}

SharedShellNode DefaultOpenNode(SharedShellKind kind)
{
    switch (kind)
    {
    case SharedShell_Pause:
        return SharedShellNode_PauseOpen;
    case SharedShell_Retry:
        return SharedShellNode_RetryOpen;
    default:
        return SharedShellNode_None;
    }
}

void InitializeShellSelections(SharedShellKind kind, u8 selection[2], u8 votes[2])
{
    for (int i = 0; i < 2; ++i)
    {
        selection[i] = kind == SharedShell_Retry ? RetrySelection_No : PauseSelection_Unpause;
        votes[i] = SharedShellVote_None;
    }
}

bool ShouldBypassRetryMenu()
{
    return g_GameManager.isInPracticeMode || g_GameManager.isInReplay || g_GameManager.numRetries >= 3 ||
           g_GameManager.difficulty >= EXTRA;
}

void SyncNodeToSelection(int playerIndex, SharedShellNode &node, SharedShellKind kind, u8 selection[2])
{
    if (kind == SharedShell_Pause)
    {
        if (node == SharedShellNode_PauseOpen || node == SharedShellNode_PauseFocusUnpause ||
            node == SharedShellNode_PauseFocusQuit)
        {
            node = selection[playerIndex] == PauseSelection_Quit ? SharedShellNode_PauseFocusQuit
                                                                 : SharedShellNode_PauseFocusUnpause;
        }
        else if (IsPauseConfirmNode(node))
        {
            node = selection[playerIndex] == PauseSelection_QuitYes ? SharedShellNode_PauseFocusQuitYes
                                                                    : SharedShellNode_PauseFocusQuitNo;
        }
    }
    else if (kind == SharedShell_Retry)
    {
        if (node == SharedShellNode_RetryOpen || node == SharedShellNode_RetryFocusYes ||
            node == SharedShellNode_RetryFocusNo)
        {
            node = selection[playerIndex] == RetrySelection_Yes ? SharedShellNode_RetryFocusYes
                                                                : SharedShellNode_RetryFocusNo;
        }
    }
}

SharedShellAction CaptureLocalShellAction(SharedShellKind kind)
{
    if (kind == SharedShell_Pause && WAS_PRESSED(TH_BUTTON_MENU))
    {
        return ShellAction_PauseRequest;
    }
    if (WAS_PRESSED(TH_BUTTON_BOMB) || WAS_PRESSED(TH_BUTTON_BOMB2))
    {
        return ShellAction_Cancel;
    }
    if (WAS_PRESSED(TH_BUTTON_UP))
    {
        return ShellAction_MoveUp;
    }
    if (WAS_PRESSED(TH_BUTTON_DOWN))
    {
        return ShellAction_MoveDown;
    }
    if (WAS_PRESSED(TH_BUTTON_SHOOT))
    {
        return ShellAction_Confirm;
    }
    return ShellAction_None;
}

void SendSharedShellIntent(SharedShellKind kind, SharedShellAction action, u16 shellSerial)
{
    Pack pack;
    pack.type = PACK_USUAL;
    pack.ctrl.ctrlType = Ctrl_ShellIntent;
    pack.ctrl.frame = CurrentNetFrame();
    pack.ctrl.shellIntent.shellSerial = shellSerial;
    pack.ctrl.shellIntent.intentSeq = ++g_State.shell.localIntentSeq;
    pack.ctrl.shellIntent.playerSlot = (u8)LocalPlayerIndex();
    pack.ctrl.shellIntent.shellKind = (u8)kind;
    pack.ctrl.shellIntent.action = (u8)action;
    pack.ctrl.shellIntent.reserved = 0;
    TraceDiagnostic("send-shell-intent", "serial=%u seq=%u player=%u kind=%u action=%u frame=%d", shellSerial,
                    pack.ctrl.shellIntent.intentSeq, pack.ctrl.shellIntent.playerSlot, pack.ctrl.shellIntent.shellKind,
                    pack.ctrl.shellIntent.action, pack.ctrl.frame);
    SendPacket(pack);
}

u8 BuildSharedShellFlags()
{
    u8 flags = g_State.shell.active ? SharedShellFlag_Active : 0;
    const bool exiting = g_State.shell.authoritativePhase == SharedShellPhase_PendingLeave ||
                         IsClosingNode(g_State.shell.authoritativeNode);
    if (exiting)
    {
        flags |= SharedShellFlag_Exiting;
        bool commitReady = false;
        if (g_State.shell.authoritativePhase == SharedShellPhase_PendingLeave)
        {
            commitReady = CurrentNetFrame() >= g_State.shell.authoritativeHandoffFrame;
        }
        else if ((int)g_State.shell.authoritativePhaseFrames >= GetClosingThreshold(g_State.shell.authoritativeNode))
        {
            commitReady = true;
        }
        if (commitReady)
        {
            flags |= SharedShellFlag_CommitReady;
        }
    }
    return flags;
}

void SendSharedShellStateNow()
{
    if (!g_State.shell.active || !g_State.isHost)
    {
        return;
    }

    Pack pack;
    pack.type = PACK_USUAL;
    pack.ctrl.ctrlType = Ctrl_ShellState;
    pack.ctrl.frame = CurrentNetFrame();
    pack.ctrl.shellState.shellSerial = g_State.shell.shellSerial;
    pack.ctrl.shellState.stateRevision = g_State.shell.stateRevision;
    pack.ctrl.shellState.shellKind = (u8)g_State.shell.shellKind;
    pack.ctrl.shellState.shellNode = (u8)g_State.shell.authoritativeNode;
    pack.ctrl.shellState.shellPhase = (u8)g_State.shell.authoritativePhase;
    pack.ctrl.shellState.transitionReason = (u8)g_State.shell.authoritativeTransitionReason;
    pack.ctrl.shellState.phaseFrames = g_State.shell.authoritativePhaseFrames;
    pack.ctrl.shellState.handoffFrame = g_State.shell.authoritativeHandoffFrame;
    pack.ctrl.shellState.selectionP1 = g_State.shell.authoritativeSelection[0];
    pack.ctrl.shellState.selectionP2 = g_State.shell.authoritativeSelection[1];
    pack.ctrl.shellState.voteMask = PackVoteMask(g_State.shell.authoritativeVotes);
    pack.ctrl.shellState.flags = BuildSharedShellFlags();
    g_State.shell.lastShellStateSendTick = SDL_GetTicks64();
    SendPacket(pack);
}

void MarkAuthoritativeShellChanged()
{
    g_State.shell.stateRevision++;
    CopyAuthoritativeToPredicted();
    SendSharedShellStateNow();
}

SharedShellTransitionReason TransitionReasonForPauseClosingNode(SharedShellNode node)
{
    return node == SharedShellNode_PauseClosingQuit ? SharedShellTransitionReason_Quit
                                                    : SharedShellTransitionReason_Resume;
}

void SchedulePauseLeaveOnHost()
{
    if (g_State.shell.shellKind != SharedShell_Pause || g_State.shell.authoritativePhase != SharedShellPhase_Active ||
        !IsClosingNode(g_State.shell.authoritativeNode))
    {
        return;
    }

    g_State.shell.authoritativePhase = SharedShellPhase_PendingLeave;
    g_State.shell.authoritativeTransitionReason = TransitionReasonForPauseClosingNode(g_State.shell.authoritativeNode);
    g_State.shell.authoritativeHandoffFrame =
        (u16)(CurrentNetFrame() +
              std::max(GetSharedShellLeadFrames(), GetClosingThreshold(g_State.shell.authoritativeNode)));
    TraceDiagnostic("shell-leave-scheduled", "frame=%d serial=%u node=%u leaveFrame=%u reason=%u",
                    CurrentNetFrame(), g_State.shell.shellSerial, (unsigned)g_State.shell.authoritativeNode,
                    g_State.shell.authoritativeHandoffFrame, (unsigned)g_State.shell.authoritativeTransitionReason);
}

void ScheduleRetryLeaveOnHost()
{
    if (g_State.shell.shellKind != SharedShell_Retry || g_State.shell.authoritativePhase != SharedShellPhase_Active ||
        !IsClosingNode(g_State.shell.authoritativeNode))
    {
        return;
    }

    g_State.shell.authoritativePhase = SharedShellPhase_PendingLeave;
    g_State.shell.authoritativeTransitionReason = SharedShellTransitionReason_None;
    g_State.shell.authoritativeHandoffFrame =
        (u16)(CurrentNetFrame() +
              std::max(GetSharedShellLeadFrames(), GetClosingThreshold(g_State.shell.authoritativeNode)));
    TraceDiagnostic("shell-retry-leave-scheduled", "frame=%d serial=%u node=%u leaveFrame=%u", CurrentNetFrame(),
                    g_State.shell.shellSerial, (unsigned)g_State.shell.authoritativeNode,
                    g_State.shell.authoritativeHandoffFrame);
}

bool ApplyPauseActionToModel(SharedShellNode &node, u16 &phaseFrames, u8 selection[2], u8 votes[2], int playerIndex,
                             SharedShellAction action)
{
    if (IsClosingNode(node))
    {
        return false;
    }

    const bool inConfirm = IsPauseConfirmNode(node);
    const bool inputReady = phaseFrames >= 4;

    if (action == ShellAction_PauseRequest)
    {
        node = SharedShellNode_PauseClosingResume;
        phaseFrames = 0;
        return true;
    }

    if (votes[playerIndex] != SharedShellVote_None && action != ShellAction_Cancel)
    {
        return false;
    }
    if (!inputReady)
    {
        return false;
    }

    switch (action)
    {
    case ShellAction_MoveUp:
    case ShellAction_MoveDown:
    {
        const u8 previous = selection[playerIndex];
        if (inConfirm)
        {
            selection[playerIndex] =
                previous == PauseSelection_QuitYes ? PauseSelection_QuitNo : PauseSelection_QuitYes;
        }
        else
        {
            selection[playerIndex] = previous == PauseSelection_Quit ? PauseSelection_Unpause : PauseSelection_Quit;
        }
        SyncNodeToSelection(playerIndex, node, SharedShell_Pause, selection);
        return selection[playerIndex] != previous;
    }
    case ShellAction_Confirm:
        if (!inConfirm)
        {
            if (selection[playerIndex] == PauseSelection_Unpause)
            {
                node = SharedShellNode_PauseClosingResume;
                phaseFrames = 0;
                return true;
            }

            node = SharedShellNode_PauseFocusQuitNo;
            phaseFrames = 0;
            for (int i = 0; i < 2; ++i)
            {
                if (selection[i] < PauseSelection_QuitYes)
                {
                    selection[i] = PauseSelection_QuitNo;
                }
            }
            return true;
        }

        if (selection[playerIndex] == PauseSelection_QuitYes)
        {
            const u8 previousVote = votes[playerIndex];
            votes[playerIndex] = SharedShellVote_Quit;
            if (votes[0] == SharedShellVote_Quit && votes[1] == SharedShellVote_Quit)
            {
                node = SharedShellNode_PauseClosingQuit;
                phaseFrames = 0;
            }
            SyncNodeToSelection(playerIndex, node, SharedShell_Pause, selection);
            return previousVote != votes[playerIndex] || node == SharedShellNode_PauseClosingQuit;
        }

        votes[playerIndex] = SharedShellVote_None;
        if (votes[0] != SharedShellVote_Quit && votes[1] != SharedShellVote_Quit)
        {
            node = SharedShellNode_PauseFocusQuit;
            phaseFrames = 0;
            for (int i = 0; i < 2; ++i)
            {
                if (selection[i] >= PauseSelection_QuitYes)
                {
                    selection[i] = PauseSelection_Quit;
                }
            }
            return true;
        }
        SyncNodeToSelection(playerIndex, node, SharedShell_Pause, selection);
        return true;
    case ShellAction_Cancel:
    {
        const u8 previousVote = votes[playerIndex];
        votes[playerIndex] = SharedShellVote_None;
        if (inConfirm && votes[0] != SharedShellVote_Quit && votes[1] != SharedShellVote_Quit)
        {
            node = SharedShellNode_PauseFocusQuit;
            phaseFrames = 0;
            for (int i = 0; i < 2; ++i)
            {
                if (selection[i] >= PauseSelection_QuitYes)
                {
                    selection[i] = PauseSelection_Quit;
                }
            }
            return true;
        }
        return previousVote != SharedShellVote_None;
    }
    default:
        return false;
    }
}

bool ApplyRetryActionToModel(SharedShellNode &node, u16 &phaseFrames, u8 selection[2], u8 votes[2], int playerIndex,
                             SharedShellAction action)
{
    if (IsClosingNode(node))
    {
        return false;
    }

    const bool focusingYes = selection[playerIndex] == RetrySelection_Yes;
    const bool inputReady = focusingYes ? phaseFrames >= 4 : phaseFrames >= 30;

    if (votes[playerIndex] != SharedShellVote_None && action != ShellAction_Cancel)
    {
        return false;
    }
    if (action != ShellAction_Cancel && !inputReady)
    {
        return false;
    }

    switch (action)
    {
    case ShellAction_MoveUp:
    case ShellAction_MoveDown:
    {
        const u8 previous = selection[playerIndex];
        selection[playerIndex] = previous == RetrySelection_Yes ? RetrySelection_No : RetrySelection_Yes;
        SyncNodeToSelection(playerIndex, node, SharedShell_Retry, selection);
        return selection[playerIndex] != previous;
    }
    case ShellAction_Confirm:
    {
        if (selection[playerIndex] == RetrySelection_Yes)
        {
            const u8 previousVote = votes[playerIndex];
            votes[playerIndex] = SharedShellVote_Retry;
            if (votes[0] == SharedShellVote_Retry && votes[1] == SharedShellVote_Retry)
            {
                node = SharedShellNode_RetryClosingRetry;
                phaseFrames = 0;
            }
            SyncNodeToSelection(playerIndex, node, SharedShell_Retry, selection);
            return previousVote != votes[playerIndex] || node == SharedShellNode_RetryClosingRetry;
        }

        const u8 previousVote = votes[playerIndex];
        votes[playerIndex] = SharedShellVote_Continue;
        if (votes[0] == SharedShellVote_Continue && votes[1] == SharedShellVote_Continue)
        {
            node = SharedShellNode_RetryClosingContinue;
            phaseFrames = 0;
        }
        SyncNodeToSelection(playerIndex, node, SharedShell_Retry, selection);
        return previousVote != votes[playerIndex] || node == SharedShellNode_RetryClosingContinue;
    }
    case ShellAction_Cancel:
    {
        const u8 previousVote = votes[playerIndex];
        votes[playerIndex] = SharedShellVote_None;
        return previousVote != SharedShellVote_None;
    }
    default:
        return false;
    }
}

bool ApplyActionToModel(SharedShellKind kind, SharedShellNode &node, u16 &phaseFrames, u8 selection[2], u8 votes[2],
                        int playerIndex, SharedShellAction action)
{
    switch (kind)
    {
    case SharedShell_Pause:
        return ApplyPauseActionToModel(node, phaseFrames, selection, votes, playerIndex, action);
    case SharedShell_Retry:
        return ApplyRetryActionToModel(node, phaseFrames, selection, votes, playerIndex, action);
    default:
        return false;
    }
}

bool OpenSharedShellOnHost(SharedShellKind kind)
{
    if (g_State.shell.active)
    {
        return g_State.shell.shellKind == kind;
    }

    if (kind == SharedShell_Pause)
    {
        const char *blockReason = GetPauseShellEntryBlockReason();
        if (blockReason != nullptr)
        {
            TraceDiagnostic("shell-enter-blocked",
                            "reason=%s frame=%d confirmed=%d latestRemote=%d pending=%d rollback=%d sync=%d",
                            blockReason, CurrentNetFrame(), g_State.lastConfirmedSyncFrame, LatestRemoteReceivedFrame(),
                            g_State.pendingRollbackFrame, g_State.rollbackActive ? 1 : 0, g_State.isSync ? 1 : 0);
            return false;
        }
    }

    g_State.shell.active = true;
    g_State.shell.pendingActivation = true;
    g_State.shell.shellSerial = g_State.shell.nextShellSerial++;
    g_State.shell.stateRevision = 1;
    g_State.shell.shellKind = kind;
    g_State.shell.authoritativePhase = SharedShellPhase_Active;
    g_State.shell.predictedPhase = SharedShellPhase_Active;
    g_State.shell.authoritativeNode = DefaultOpenNode(kind);
    g_State.shell.predictedNode = g_State.shell.authoritativeNode;
    g_State.shell.authoritativeTransitionReason = SharedShellTransitionReason_None;
    g_State.shell.predictedTransitionReason = SharedShellTransitionReason_None;
    g_State.shell.authoritativePhaseFrames = 0;
    g_State.shell.predictedPhaseFrames = 0;
    g_State.shell.authoritativeHandoffFrame = 0;
    g_State.shell.predictedHandoffFrame = 0;
    InitializeShellSelections(kind, g_State.shell.authoritativeSelection, g_State.shell.authoritativeVotes);
    CopyAuthoritativeToPredicted();
    std::memset(g_State.shell.lastProcessedIntentSeq, 0, sizeof(g_State.shell.lastProcessedIntentSeq));

    if (kind == SharedShell_Pause)
    {
        g_State.shell.pendingActivation = false;
        g_State.shell.authoritativePhase = SharedShellPhase_PendingEnter;
        g_State.shell.authoritativeHandoffFrame = (u16)(CurrentNetFrame() + GetSharedShellLeadFrames());
        CopyAuthoritativeToPredicted();
        TraceDiagnostic("shell-enter-scheduled", "frame=%d serial=%u enterFrame=%u kind=%u", CurrentNetFrame(),
                        g_State.shell.shellSerial, g_State.shell.authoritativeHandoffFrame, (unsigned)kind);
    }
    else
    {
        g_State.shell.pendingActivation = true;
        if (kind == SharedShell_Retry && ShouldBypassRetryMenu())
        {
            g_State.shell.authoritativeNode = SharedShellNode_RetryClosingContinue;
            g_State.shell.authoritativeSelection[0] = RetrySelection_No;
            g_State.shell.authoritativeSelection[1] = RetrySelection_No;
            ScheduleRetryLeaveOnHost();
            CopyAuthoritativeToPredicted();
        }
    }

    SendSharedShellStateNow();
    return true;
}

void ApplyMenuViewport()
{
    g_GameManager.arcadeRegionTopLeftPos.x = GAME_REGION_LEFT;
    g_GameManager.arcadeRegionTopLeftPos.y = GAME_REGION_TOP;
    g_GameManager.arcadeRegionSize.x = GAME_REGION_WIDTH;
    g_GameManager.arcadeRegionSize.y = GAME_REGION_HEIGHT;
    g_Supervisor.unk198 = 3;
}

void HideMenuSprites(StageMenu &menu, int start, int end)
{
    for (int i = start; i < end; ++i)
    {
        menu.menuSprites[i].SetInvisible();
    }
}

int PauseLegacyStateForDisplay(SharedShellNode node, u8 localSelection)
{
    if (node == SharedShellNode_PauseClosingResume)
    {
        return GAME_MENU_PAUSE_SELECTED_UNPAUSE;
    }
    if (node == SharedShellNode_PauseClosingQuit)
    {
        return GAME_MENU_QUIT_SELECTED_YES;
    }
    if (localSelection == PauseSelection_QuitYes)
    {
        return GAME_MENU_QUIT_CURSOR_YES;
    }
    if (localSelection == PauseSelection_QuitNo)
    {
        return GAME_MENU_QUIT_CURSOR_NO;
    }
    if (localSelection == PauseSelection_Quit)
    {
        return GAME_MENU_PAUSE_CURSOR_QUIT;
    }
    return GAME_MENU_PAUSE_CURSOR_UNPAUSE;
}

int RetryLegacyStateForDisplay(SharedShellNode node, u8 localSelection)
{
    if (node == SharedShellNode_RetryClosingRetry)
    {
        return RETRY_MENU_SELECTED_YES;
    }
    if (node == SharedShellNode_RetryClosingContinue)
    {
        return RETRY_MENU_SELECTED_NO;
    }
    return localSelection == RetrySelection_Yes ? RETRY_MENU_CURSOR_YES : RETRY_MENU_CURSOR_NO;
}

void InitializePauseMenuView(StageMenu &menu)
{
    for (int i = 0; i < ARRAY_SIZE_SIGNED(menu.menuSprites); ++i)
    {
        g_AnmManager->SetAndExecuteScriptIdx(&menu.menuSprites[i], i + 2);
    }
    for (int i = GAME_MENU_SPRITES_START_PAUSE; i < GAME_MENU_SPRITES_END_PAUSE; ++i)
    {
        menu.menuSprites[i].pendingInterrupt = 1;
    }
    if (g_Supervisor.lockableBackbuffer)
    {
        g_AnmManager->RequestScreenshot();
        g_AnmManager->SetAndExecuteScriptIdx(&menu.menuBackground, ANM_SCRIPT_CAPTURE_PAUSE_BG);
        menu.menuBackground.pos.x = GAME_REGION_LEFT;
        menu.menuBackground.pos.y = GAME_REGION_TOP;
        menu.menuBackground.pos.z = 0.0f;
    }
}

void InitializeRetryMenuView(StageMenu &menu)
{
    for (int i = RETRY_MENU_SPRITES_START; i < RETRY_MENU_SPRITES_END; ++i)
    {
        g_AnmManager->SetAndExecuteScriptIdx(&menu.menuSprites[i], i < 2 ? i + 8 : i + 4);
        menu.menuSprites[i].pendingInterrupt = 1;
    }
    if (g_Supervisor.lockableBackbuffer)
    {
        g_AnmManager->RequestScreenshot();
        g_AnmManager->SetAndExecuteScriptIdx(&menu.menuBackground, ANM_SCRIPT_CAPTURE_PAUSE_BG);
        menu.menuBackground.pos.x = GAME_REGION_LEFT;
        menu.menuBackground.pos.y = GAME_REGION_TOP;
        menu.menuBackground.pos.z = 0.0f;
    }
}

void ApplyPauseMenuDisplay(StageMenu &menu, int legacyState, int phaseFrames)
{
    if (menu.curState == 0 && menu.numFrames == 0 && !menu.menuSprites[GAME_MENU_SPRITE_TITLE_PAUSE].flags.isVisible)
    {
        InitializePauseMenuView(menu);
    }

    if (legacyState == GAME_MENU_PAUSE_CURSOR_UNPAUSE)
    {
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_UNPAUSE].color = COLOR_LIGHT_RED;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_QUIT].color = COLOR_SET_ALPHA(COLOR_GREY, 0x80);
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_UNPAUSE].scaleY = 1.7f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_UNPAUSE].scaleX = 1.7f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_QUIT].scaleY = 1.5f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_QUIT].scaleX = 1.5f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_UNPAUSE].posOffset = D3DXVECTOR3(-4.0f, -4.0f, 0.0f);
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_QUIT].posOffset = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
    }
    else if (legacyState == GAME_MENU_PAUSE_CURSOR_QUIT)
    {
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_UNPAUSE].color = COLOR_SET_ALPHA(COLOR_GREY, 0x80);
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_QUIT].color = COLOR_LIGHT_RED;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_UNPAUSE].scaleY = 1.5f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_UNPAUSE].scaleX = 1.5f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_QUIT].scaleY = 1.7f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_QUIT].scaleX = 1.7f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_UNPAUSE].posOffset = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_QUIT].posOffset = D3DXVECTOR3(-4.0f, -4.0f, 0.0f);
    }
    else if (legacyState == GAME_MENU_QUIT_CURSOR_YES)
    {
        if (!menu.menuSprites[GAME_MENU_SPRITE_TITLE_QUIT].flags.isVisible)
        {
            for (int i = GAME_MENU_SPRITES_START_PAUSE; i < GAME_MENU_SPRITES_END_PAUSE; ++i)
            {
                menu.menuSprites[i].pendingInterrupt = 2;
            }
            for (int i = GAME_MENU_SPRITES_START_QUIT; i < GAME_MENU_SPRITES_END_QUIT; ++i)
            {
                menu.menuSprites[i].pendingInterrupt = 1;
            }
        }
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_YES].color = COLOR_LIGHT_RED;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_NO].color = COLOR_SET_ALPHA(COLOR_GREY, 0x80);
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_YES].scaleY = 1.7f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_YES].scaleX = 1.7f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_NO].scaleY = 1.5f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_NO].scaleX = 1.5f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_YES].posOffset = D3DXVECTOR3(-4.0f, -4.0f, 0.0f);
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_NO].posOffset = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
    }
    else if (legacyState == GAME_MENU_QUIT_CURSOR_NO)
    {
        if (!menu.menuSprites[GAME_MENU_SPRITE_TITLE_QUIT].flags.isVisible)
        {
            for (int i = GAME_MENU_SPRITES_START_PAUSE; i < GAME_MENU_SPRITES_END_PAUSE; ++i)
            {
                menu.menuSprites[i].pendingInterrupt = 2;
            }
            for (int i = GAME_MENU_SPRITES_START_QUIT; i < GAME_MENU_SPRITES_END_QUIT; ++i)
            {
                menu.menuSprites[i].pendingInterrupt = 1;
            }
        }
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_YES].color = COLOR_SET_ALPHA(COLOR_GREY, 0x80);
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_NO].color = COLOR_LIGHT_RED;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_YES].scaleY = 1.5f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_YES].scaleX = 1.5f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_NO].scaleY = 1.7f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_NO].scaleX = 1.7f;
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_YES].posOffset = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
        menu.menuSprites[GAME_MENU_SPRITE_CURSOR_NO].posOffset = D3DXVECTOR3(-4.0f, -4.0f, 0.0f);
    }
    else if (legacyState == GAME_MENU_PAUSE_SELECTED_UNPAUSE)
    {
        for (int i = 0; i < ARRAY_SIZE_SIGNED(menu.menuSprites); ++i)
        {
            if (menu.menuSprites[i].flags.isVisible)
            {
                menu.menuSprites[i].pendingInterrupt = 2;
            }
        }
        menu.menuBackground.pendingInterrupt = 1;
    }
    else if (legacyState == GAME_MENU_QUIT_SELECTED_YES)
    {
        for (int i = 0; i < ARRAY_SIZE_SIGNED(menu.menuSprites); ++i)
        {
            if (menu.menuSprites[i].flags.isVisible)
            {
                menu.menuSprites[i].pendingInterrupt = 2;
            }
        }
    }

    for (int i = 0; i < ARRAY_SIZE_SIGNED(menu.menuSprites); ++i)
    {
        g_AnmManager->ExecuteScript(&menu.menuSprites[i]);
    }
    if (g_Supervisor.lockableBackbuffer)
    {
        g_AnmManager->ExecuteScript(&menu.menuBackground);
    }
    menu.curState = legacyState;
    menu.numFrames = phaseFrames;
}

void ApplyRetryMenuDisplay(StageMenu &menu, int legacyState, int phaseFrames)
{
    if (menu.curState == 0 && menu.numFrames == 0 && !menu.menuSprites[RETRY_MENU_SPRITE_TITLE].flags.isVisible)
    {
        InitializeRetryMenuView(menu);
    }

    if (legacyState == RETRY_MENU_CURSOR_YES)
    {
        menu.menuSprites[RETRY_MENU_SPRITE_YES].color = COLOR_LIGHT_RED;
        menu.menuSprites[RETRY_MENU_SPRITE_NO].color = COLOR_SET_ALPHA(COLOR_GREY, 0x80);
        menu.menuSprites[RETRY_MENU_SPRITE_YES].scaleY = 1.7f;
        menu.menuSprites[RETRY_MENU_SPRITE_YES].scaleX = 1.7f;
        menu.menuSprites[RETRY_MENU_SPRITE_NO].scaleY = 1.5f;
        menu.menuSprites[RETRY_MENU_SPRITE_NO].scaleX = 1.5f;
        menu.menuSprites[RETRY_MENU_SPRITE_YES].posOffset = D3DXVECTOR3(-4.0f, -4.0f, 0.0f);
        menu.menuSprites[RETRY_MENU_SPRITE_NO].posOffset = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
    }
    else if (legacyState == RETRY_MENU_CURSOR_NO)
    {
        menu.menuSprites[RETRY_MENU_SPRITE_NO].color = COLOR_LIGHT_RED;
        menu.menuSprites[RETRY_MENU_SPRITE_YES].color = COLOR_SET_ALPHA(COLOR_GREY, 0x80);
        menu.menuSprites[RETRY_MENU_SPRITE_YES].scaleY = 1.5f;
        menu.menuSprites[RETRY_MENU_SPRITE_YES].scaleX = 1.5f;
        menu.menuSprites[RETRY_MENU_SPRITE_NO].scaleY = 1.7f;
        menu.menuSprites[RETRY_MENU_SPRITE_NO].scaleX = 1.7f;
        menu.menuSprites[RETRY_MENU_SPRITE_NO].posOffset = D3DXVECTOR3(-4.0f, -4.0f, 0.0f);
        menu.menuSprites[RETRY_MENU_SPRITE_YES].posOffset = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
    }
    else if (legacyState == RETRY_MENU_SELECTED_YES || legacyState == RETRY_MENU_SELECTED_NO)
    {
        for (int i = RETRY_MENU_SPRITES_START; i < RETRY_MENU_SPRITES_END; ++i)
        {
            if (menu.menuSprites[i].flags.isVisible)
            {
                menu.menuSprites[i].pendingInterrupt = 2;
            }
        }
        if (legacyState == RETRY_MENU_SELECTED_YES)
        {
            menu.menuBackground.pendingInterrupt = 1;
        }
    }

    for (int i = RETRY_MENU_SPRITES_START; i < RETRY_MENU_SPRITES_END; ++i)
    {
        g_AnmManager->ExecuteScript(&menu.menuSprites[i]);
    }
    if (g_Supervisor.lockableBackbuffer)
    {
        g_AnmManager->ExecuteScript(&menu.menuBackground);
    }
    menu.curState = legacyState;
    menu.numFrames = phaseFrames;
}

void FinishSharedShellLocal()
{
    ResetSharedShellStatePreserving(NextShellSerialBase());
}

void CommitPauseEnterHandoff(int frame)
{
    if (g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER || g_GameManager.isInGameMenu)
    {
        return;
    }

    const u16 scheduledHandoffFrame = g_State.shell.authoritativeHandoffFrame;
    g_GameManager.isInGameMenu = 1;
    ApplyMenuViewport();
    g_State.shell.pendingActivation = false;
    g_State.shell.authoritativePhase = SharedShellPhase_Active;
    g_State.shell.authoritativeTransitionReason = SharedShellTransitionReason_None;
    g_State.shell.authoritativePhaseFrames = 0;
    SetLocalPausePresentationHold(false, frame > scheduledHandoffFrame ? "shell-enter-late" : "shell-enter");
    SetSharedShellUiPhaseBarrierBypass(frame, true);
    ResetRollbackEpoch(frame, "shell-enter", frame + 1);
    if (g_State.isHost)
    {
        MarkAuthoritativeShellChanged();
    }
    else
    {
        CopyAuthoritativeToPredicted();
    }
    TraceDiagnostic("shell-enter-committed", "frame=%d serial=%u handoff=%u late=%d", frame,
                    g_State.shell.shellSerial, scheduledHandoffFrame, frame > scheduledHandoffFrame ? 1 : 0);
}

void CommitRetryEnterActivation(int frame)
{
    if (g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER)
    {
        return;
    }

    g_GameManager.isInRetryMenu = 1;
    g_State.shell.pendingActivation = false;
    SetSharedShellUiPhaseBarrierBypass(frame, true);
    ResetRollbackEpoch(frame, "shell-enter-retry", frame + 1);
    TraceDiagnostic("shell-retry-enter-committed", "frame=%d serial=%u node=%u", frame,
                    g_State.shell.shellSerial, (unsigned)g_State.shell.authoritativeNode);
}

void CommitPauseMenuExit(StageMenu &menu, SharedShellNode node)
{
    g_GameManager.isInGameMenu = 0;
    HideMenuSprites(menu, 0, ARRAY_SIZE_SIGNED(menu.menuSprites));
    menu.curState = GAME_MENU_PAUSE_OPENING;
    menu.numFrames = 0;
    if (node == SharedShellNode_PauseClosingQuit)
    {
        g_Supervisor.curState = SUPERVISOR_STATE_MAINMENU;
    }
    FinishSharedShellLocal();
}

void CommitPauseLeaveHandoff(int frame)
{
    StageMenu &menu = g_AsciiManager.gameMenu;
    const u16 serial = g_State.shell.shellSerial;
    const u16 handoffFrame = g_State.shell.authoritativeHandoffFrame;
    const SharedShellNode node = g_State.shell.authoritativeNode;
    const SharedShellTransitionReason reason = g_State.shell.authoritativeTransitionReason;

    CommitPauseMenuExit(menu, node);
    if (reason == SharedShellTransitionReason_Resume)
    {
        PrepareGameplayStart();
        const int resumedFrame = CurrentNetFrame();
        SetSharedShellUiPhaseBarrierBypass(resumedFrame, false);
        ResetRollbackEpoch(resumedFrame, "shell-leave", resumedFrame);
    }
    TraceDiagnostic("shell-leave-committed", "frame=%d serial=%u handoff=%u node=%u reason=%u", frame, serial,
                    handoffFrame, (unsigned)node, (unsigned)reason);
}

void CommitRetryContinue(StageMenu &menu)
{
    const u16 serial = g_State.shell.shellSerial;
    g_GameManager.isInRetryMenu = 0;
    HideMenuSprites(menu, RETRY_MENU_SPRITES_START, RETRY_MENU_SPRITES_END);
    menu.curState = RETRY_MENU_OPENING;
    menu.numFrames = 0;
    g_Supervisor.curState = SUPERVISOR_STATE_RESULTSCREEN_FROMGAME;
    g_GameManager.guiScore = g_GameManager.score;
    FinishSharedShellLocal();
    TraceDiagnostic("shell-retry-leave-committed", "frame=%d serial=%u mode=continue isInUi=%d",
                    CurrentNetFrame(), serial, IsCurrentUiFrame() ? 1 : 0);
}

void CommitRetryRestart(StageMenu &menu)
{
    const u16 serial = g_State.shell.shellSerial;
    g_GameManager.isInRetryMenu = 0;
    HideMenuSprites(menu, RETRY_MENU_SPRITES_START, RETRY_MENU_SPRITES_END);
    menu.curState = RETRY_MENU_OPENING;
    menu.numFrames = 0;
    g_GameManager.numRetries++;
    g_GameManager.guiScore = g_GameManager.numRetries;
    g_GameManager.nextScoreIncrement = 0;
    g_GameManager.score = g_GameManager.guiScore;
    g_GameManager.livesRemaining = g_Supervisor.defaultConfig.lifeCount;
    g_GameManager.bombsRemaining = g_Supervisor.defaultConfig.bombCount;
    g_GameManager.grazeInStage = 0;
    if (Session::IsDualPlayerSession())
    {
        g_GameManager.livesRemaining2 = g_Supervisor.defaultConfig.lifeCount;
        g_GameManager.bombsRemaining2 = g_Supervisor.defaultConfig.bombCount;
        g_GameManager.currentPower = 128;
        g_GameManager.currentPower2 = 128;
    }
    else
    {
        g_GameManager.currentPower = 0;
    }
    g_GameManager.pointItemsCollectedInStage = 0;
    g_GameManager.extraLives = 0;
    g_Gui.flags.flag0 = 2;
    g_Gui.flags.flag1 = 2;
    g_Gui.flags.flag3 = 2;
    g_Gui.flags.flag4 = 2;
    g_Gui.flags.flag2 = 2;
    FinishSharedShellLocal();
    PrepareGameplayStart();
    const int resumedFrame = CurrentNetFrame();
    SetSharedShellUiPhaseBarrierBypass(resumedFrame, false);
    ResetRollbackEpoch(resumedFrame, "shell-leave-retry", resumedFrame);
    TraceDiagnostic("shell-retry-leave-committed", "frame=%d serial=%u mode=restart isInUi=%d", resumedFrame, serial,
                    IsCurrentUiFrame() ? 1 : 0);
}

bool IsNewerShellState(u16 serial, u16 revision, u16 phaseFrames)
{
    if (!g_State.shell.active)
    {
        return true;
    }
    if (serial != g_State.shell.shellSerial)
    {
        return serial > g_State.shell.shellSerial;
    }
    if (revision != g_State.shell.stateRevision)
    {
        return revision > g_State.shell.stateRevision;
    }
    return phaseFrames >= g_State.shell.authoritativePhaseFrames;
}

void QueueShellMarkerForSelection(StageMenu &menu, SharedShellKind kind, int playerIndex, u8 selection, u8 vote)
{
    int spriteIndex = -1;
    if (kind == SharedShell_Pause)
    {
        if (selection == PauseSelection_Unpause)
        {
            spriteIndex = GAME_MENU_SPRITE_CURSOR_UNPAUSE;
        }
        else if (selection == PauseSelection_Quit)
        {
            spriteIndex = GAME_MENU_SPRITE_CURSOR_QUIT;
        }
        else if (selection == PauseSelection_QuitYes)
        {
            spriteIndex = GAME_MENU_SPRITE_CURSOR_YES;
        }
        else if (selection == PauseSelection_QuitNo)
        {
            spriteIndex = GAME_MENU_SPRITE_CURSOR_NO;
        }
    }
    else if (kind == SharedShell_Retry)
    {
        spriteIndex = selection == RetrySelection_Yes ? RETRY_MENU_SPRITE_YES : RETRY_MENU_SPRITE_NO;
    }

    if (spriteIndex < 0 || !menu.menuSprites[spriteIndex].flags.isVisible)
    {
        return;
    }

    D3DXVECTOR3 pos = menu.menuSprites[spriteIndex].pos;
    pos += menu.menuSprites[spriteIndex].posOffset;
    pos.x += 36.0f;
    pos.y -= 6.0f;

    const D3DCOLOR previousColor = g_AsciiManager.color;
    const D3DXVECTOR2 previousScale = g_AsciiManager.scale;
    g_AsciiManager.color = playerIndex == 0 ? COLOR_WHITE : COLOR_LIGHTBLUE;
    g_AsciiManager.scale.x = 0.75f;
    g_AsciiManager.scale.y = 0.75f;
    g_AsciiManager.AddFormatText(&pos, vote != SharedShellVote_None ? "P%d READY" : "P%d", playerIndex + 1);
    g_AsciiManager.color = previousColor;
    g_AsciiManager.scale = previousScale;
}
} // namespace

void ResetSharedShellState()
{
    ResetSharedShellStatePreserving(1);
}

bool IsSharedShellActive(SharedShellKind kind)
{
    if (!Session::IsRemoteNetplaySession() || !g_State.shell.active)
    {
        return false;
    }
    return kind == SharedShell_None || g_State.shell.shellKind == kind;
}

bool IsPausePresentationHoldActive()
{
    return Session::IsRemoteNetplaySession() && g_State.shell.localPausePresentationHold;
}

bool ShouldFreezeSharedShellUiInput(int frame)
{
    if (!Session::IsRemoteNetplaySession() || !g_State.shell.active)
    {
        return false;
    }
    if (g_State.shell.shellKind != SharedShell_Retry || g_State.shell.authoritativePhase != SharedShellPhase_PendingLeave)
    {
        return false;
    }
    if (!g_GameManager.isInRetryMenu)
    {
        return false;
    }

    return frame < g_State.shell.authoritativeHandoffFrame;
}

bool RequestSharedShellEnter(SharedShellKind kind)
{
    if (!Session::IsRemoteNetplaySession() || !g_State.isSessionActive)
    {
        return false;
    }
    if (kind == SharedShell_Pause)
    {
        const char *blockReason = GetPauseShellEntryBlockReason();
        if (blockReason != nullptr)
        {
            TraceDiagnostic("shell-enter-local-blocked",
                            "reason=%s frame=%d confirmed=%d latestRemote=%d pending=%d rollback=%d sync=%d",
                            blockReason, CurrentNetFrame(), g_State.lastConfirmedSyncFrame, LatestRemoteReceivedFrame(),
                            g_State.pendingRollbackFrame, g_State.rollbackActive ? 1 : 0, g_State.isSync ? 1 : 0);
            SetLocalPausePresentationHold(false, "blocked");
            return false;
        }
    }
    if (kind == SharedShell_Pause && g_State.shell.localPausePresentationHold)
    {
        return true;
    }
    if (g_State.shell.active)
    {
        return g_State.shell.shellKind == kind;
    }
    if (g_State.isHost)
    {
        const bool opened = OpenSharedShellOnHost(kind);
        if (opened && kind == SharedShell_Pause)
        {
            SetLocalPausePresentationHold(true, "local-pause-request");
        }
        return opened;
    }

    if (kind == SharedShell_Pause)
    {
        SetLocalPausePresentationHold(true, "local-pause-request");
    }
    SendSharedShellIntent(kind, ShellAction_PauseRequest, 0);
    return true;
}

void ApplyPendingSharedShellActivation()
{
    if (!g_State.shell.active || !g_State.shell.pendingActivation)
    {
        return;
    }
    if (g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER)
    {
        return;
    }

    switch (g_State.shell.shellKind)
    {
    case SharedShell_Pause:
        g_GameManager.isInGameMenu = 1;
        ApplyMenuViewport();
        break;
    case SharedShell_Retry:
        CommitRetryEnterActivation(CurrentNetFrame());
        break;
    default:
        break;
    }

    g_State.shell.pendingActivation = false;
}

bool ConsumeSharedShellUiPhaseBarrierBypass(int frame, bool isInUi)
{
    if (!g_State.shell.suppressUiPhaseBarrier)
    {
        return false;
    }
    if (g_State.shell.suppressUiPhaseBarrierFrame != frame || g_State.shell.suppressUiPhaseBarrierIsInUi != isInUi)
    {
        return false;
    }

    g_State.shell.suppressUiPhaseBarrier = false;
    g_State.shell.suppressUiPhaseBarrierFrame = -1;
    TraceDiagnostic("ui-transition-bypass", "frame=%d isInUi=%d", frame, isInUi ? 1 : 0);
    return true;
}

void DriveSharedShellHandoff(int frame)
{
    if (!g_State.shell.active)
    {
        if (g_State.shell.localPausePresentationHold)
        {
            SetLocalPausePresentationHold(false, "inactive");
        }
        return;
    }

    if (g_State.shell.shellKind != SharedShell_Pause)
    {
        ApplyPendingSharedShellActivation();
        if (g_State.shell.shellKind == SharedShell_Retry &&
            g_State.shell.authoritativePhase == SharedShellPhase_PendingLeave &&
            frame >= g_State.shell.authoritativeHandoffFrame)
        {
            StageMenu &menu = g_AsciiManager.retryMenu;
            if (g_State.shell.authoritativeNode == SharedShellNode_RetryClosingContinue)
            {
                CommitRetryContinue(menu);
            }
            else if (g_State.shell.authoritativeNode == SharedShellNode_RetryClosingRetry)
            {
                CommitRetryRestart(menu);
            }
        }
        return;
    }

    if (g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER)
    {
        return;
    }

    if (g_State.shell.authoritativePhase == SharedShellPhase_PendingEnter)
    {
        if (frame >= g_State.shell.authoritativeHandoffFrame)
        {
            CommitPauseEnterHandoff(frame);
        }
        else
        {
            SetLocalPausePresentationHold(true, "await-authoritative-handoff");
        }
        return;
    }

    if (g_State.shell.authoritativePhase == SharedShellPhase_Active)
    {
        if (!g_GameManager.isInGameMenu)
        {
            if (frame >= g_State.shell.authoritativeHandoffFrame)
            {
                CommitPauseEnterHandoff(frame);
            }
            else
            {
                SetLocalPausePresentationHold(true, "await-authoritative-active");
            }
        }
        else
        {
            SetLocalPausePresentationHold(false, "pause-active");
        }
        return;
    }

    if (g_State.shell.authoritativePhase == SharedShellPhase_PendingLeave && frame >= g_State.shell.authoritativeHandoffFrame)
    {
        CommitPauseLeaveHandoff(frame);
    }
}

void DriveSharedShellStateBroadcast()
{
    if (!g_State.shell.active)
    {
        return;
    }

    const bool freezeAuthoritativePhaseCounter =
        g_State.shell.shellKind == SharedShell_Pause && g_State.shell.authoritativePhase == SharedShellPhase_PendingEnter;
    const bool freezePredictedPhaseCounter =
        g_State.shell.shellKind == SharedShell_Pause && g_State.shell.predictedPhase == SharedShellPhase_PendingEnter;

    if (g_State.isHost && !freezeAuthoritativePhaseCounter && g_State.shell.authoritativePhaseFrames < 0xffffu)
    {
        g_State.shell.authoritativePhaseFrames++;
    }
    if (!freezePredictedPhaseCounter && g_State.shell.predictedPhaseFrames < 0xffffu)
    {
        g_State.shell.predictedPhaseFrames++;
    }

    if (!g_State.isHost)
    {
        return;
    }

    const Uint64 now = SDL_GetTicks64();
    if (g_State.shell.lastShellStateSendTick != 0 && now - g_State.shell.lastShellStateSendTick < kReconnectPingMs)
    {
        return;
    }

    SendSharedShellStateNow();
}

void HandleSharedShellIntentPacket(const CtrlPack &ctrl)
{
    g_State.lastRuntimeReceiveTick = SDL_GetTicks64();
    if (!g_State.isHost)
    {
        return;
    }

    const SharedShellKind kind = (SharedShellKind)ctrl.shellIntent.shellKind;
    const SharedShellAction action = (SharedShellAction)ctrl.shellIntent.action;
    const int playerIndex = ctrl.shellIntent.playerSlot < 2 ? (int)ctrl.shellIntent.playerSlot : -1;
    if (playerIndex < 0)
    {
        return;
    }

    const u16 intentSeq = ctrl.shellIntent.intentSeq;
    if (intentSeq <= g_State.shell.lastProcessedIntentSeq[playerIndex])
    {
        return;
    }
    g_State.shell.lastProcessedIntentSeq[playerIndex] = intentSeq;

    if (action == ShellAction_PauseRequest && !g_State.shell.active)
    {
        OpenSharedShellOnHost(kind);
        return;
    }

    if (!g_State.shell.active || g_State.shell.shellKind != kind || ctrl.shellIntent.shellSerial != g_State.shell.shellSerial)
    {
        return;
    }
    if (kind == SharedShell_Pause && g_State.shell.authoritativePhase != SharedShellPhase_Active)
    {
        return;
    }

    bool changed =
        ApplyActionToModel(g_State.shell.shellKind, g_State.shell.authoritativeNode, g_State.shell.authoritativePhaseFrames,
                           g_State.shell.authoritativeSelection, g_State.shell.authoritativeVotes, playerIndex, action);
    if (changed)
    {
        if (kind == SharedShell_Pause)
        {
            SchedulePauseLeaveOnHost();
        }
        else if (kind == SharedShell_Retry)
        {
            ScheduleRetryLeaveOnHost();
        }
        MarkAuthoritativeShellChanged();
    }
}

void HandleSharedShellStatePacket(const CtrlPack &ctrl)
{
    const Uint64 now = SDL_GetTicks64();
    g_State.lastRuntimeReceiveTick = now;

    const u16 shellSerial = ctrl.shellState.shellSerial;
    if (shellSerial < g_State.shell.nextShellSerial && shellSerial != g_State.shell.shellSerial)
    {
        return;
    }
    if (!IsNewerShellState(shellSerial, ctrl.shellState.stateRevision, ctrl.shellState.phaseFrames))
    {
        return;
    }

    g_State.shell.active = (ctrl.shellState.flags & SharedShellFlag_Active) != 0;
    g_State.shell.pendingActivation = g_State.shell.active && (SharedShellKind)ctrl.shellState.shellKind == SharedShell_Retry;
    g_State.shell.shellSerial = shellSerial;
    g_State.shell.nextShellSerial = std::max<u16>(g_State.shell.nextShellSerial, (u16)(shellSerial + 1));
    g_State.shell.stateRevision = ctrl.shellState.stateRevision;
    g_State.shell.shellKind = (SharedShellKind)ctrl.shellState.shellKind;
    g_State.shell.authoritativePhase = (SharedShellPhase)ctrl.shellState.shellPhase;
    g_State.shell.authoritativeNode = (SharedShellNode)ctrl.shellState.shellNode;
    g_State.shell.authoritativeTransitionReason =
        (SharedShellTransitionReason)ctrl.shellState.transitionReason;
    g_State.shell.authoritativePhaseFrames = ctrl.shellState.phaseFrames;
    g_State.shell.authoritativeHandoffFrame = ctrl.shellState.handoffFrame;
    g_State.shell.authoritativeSelection[0] = ctrl.shellState.selectionP1;
    g_State.shell.authoritativeSelection[1] = ctrl.shellState.selectionP2;
    UnpackVoteMask(ctrl.shellState.voteMask, g_State.shell.authoritativeVotes);
    CopyAuthoritativeToPredicted();
    g_State.shell.lastShellStateRecvTick = now;
    TraceDiagnostic("recv-shell-state", "serial=%u revision=%u kind=%u phase=%u node=%u handoff=%u flags=0x%02x",
                    shellSerial, ctrl.shellState.stateRevision, (unsigned)g_State.shell.shellKind,
                    (unsigned)g_State.shell.authoritativePhase, (unsigned)g_State.shell.authoritativeNode,
                    g_State.shell.authoritativeHandoffFrame, ctrl.shellState.flags);

    if ((g_State.shell.shellKind == SharedShell_Pause && g_GameManager.isInGameMenu) ||
        (g_State.shell.shellKind == SharedShell_Retry && g_GameManager.isInRetryMenu))
    {
        g_State.shell.pendingActivation = false;
    }
    else if (g_State.shell.active && g_State.shell.shellKind == SharedShell_Pause)
    {
        SetLocalPausePresentationHold(true, "recv-shell-state");
    }
    if (!g_State.shell.active)
    {
        SetLocalPausePresentationHold(false, "shell-inactive");
    }
}

bool UpdateSharedShellMenu(StageMenu &menu, SharedShellKind kind)
{
    if (!IsSharedShellActive(kind))
    {
        return false;
    }

    const int localPlayer = LocalPlayerIndex();
    const SharedShellPhase localPhase = g_State.isHost ? g_State.shell.authoritativePhase : g_State.shell.predictedPhase;
    const bool allowMenuInput = localPhase == SharedShellPhase_Active;
    const SharedShellAction action = allowMenuInput ? CaptureLocalShellAction(kind) : ShellAction_None;

    if (g_State.isHost)
    {
        if (action != ShellAction_None)
        {
            bool changed = ApplyActionToModel(kind, g_State.shell.authoritativeNode, g_State.shell.authoritativePhaseFrames,
                                              g_State.shell.authoritativeSelection, g_State.shell.authoritativeVotes,
                                              localPlayer, action);
            if (changed)
            {
                if (kind == SharedShell_Pause)
                {
                    SchedulePauseLeaveOnHost();
                }
                else if (kind == SharedShell_Retry)
                {
                    ScheduleRetryLeaveOnHost();
                }
                MarkAuthoritativeShellChanged();
            }
        }
    }
    else if (action != ShellAction_None)
    {
        SharedShellNode predictedNode = g_State.shell.predictedNode;
        u16 predictedPhaseFrames = g_State.shell.predictedPhaseFrames;
        u8 predictedSelection[2] = {g_State.shell.predictedSelection[0], g_State.shell.predictedSelection[1]};
        u8 predictedVotes[2] = {g_State.shell.predictedVotes[0], g_State.shell.predictedVotes[1]};
        if (ApplyActionToModel(kind, predictedNode, predictedPhaseFrames, predictedSelection, predictedVotes,
                               localPlayer, action))
        {
            g_State.shell.predictedNode = predictedNode;
            g_State.shell.predictedPhaseFrames = predictedPhaseFrames;
            g_State.shell.predictedSelection[0] = predictedSelection[0];
            g_State.shell.predictedSelection[1] = predictedSelection[1];
            g_State.shell.predictedVotes[0] = predictedVotes[0];
            g_State.shell.predictedVotes[1] = predictedVotes[1];
            SendSharedShellIntent(kind, action, g_State.shell.shellSerial);
        }
    }

    if (kind == SharedShell_Pause)
    {
        ApplyMenuViewport();
        ApplyPauseMenuDisplay(menu, PauseLegacyStateForDisplay(g_State.shell.predictedNode,
                                                               g_State.shell.predictedSelection[localPlayer]),
                              g_State.shell.predictedPhaseFrames);
    }
    else if (kind == SharedShell_Retry)
    {
        ApplyRetryMenuDisplay(menu, RetryLegacyStateForDisplay(g_State.shell.predictedNode,
                                                               g_State.shell.predictedSelection[localPlayer]),
                              g_State.shell.predictedPhaseFrames);
    }

    return true;
}

void QueueSharedShellOverlayText()
{
    if (!g_State.shell.active)
    {
        return;
    }

    StageMenu *menu = nullptr;
    if (g_State.shell.shellKind == SharedShell_Pause && g_GameManager.isInGameMenu)
    {
        menu = &g_AsciiManager.gameMenu;
    }
    else if (g_State.shell.shellKind == SharedShell_Retry && g_GameManager.isInRetryMenu)
    {
        menu = &g_AsciiManager.retryMenu;
    }

    if (menu == nullptr)
    {
        return;
    }

    QueueShellMarkerForSelection(*menu, g_State.shell.shellKind, RemotePlayerIndex(),
                                 g_State.shell.authoritativeSelection[RemotePlayerIndex()],
                                 g_State.shell.authoritativeVotes[RemotePlayerIndex()]);
}

} // namespace th06::Netplay
