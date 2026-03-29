#include "PortableGameplayRestore.hpp"

#include "AnmManager.hpp"
#include "AnmIdx.hpp"
#include "BombData.hpp"
#include "BulletManager.hpp"
#include "Controller.hpp"
#include "EnemyEclInstr.hpp"
#include "EnemyManager.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "GamePaths.hpp"
#include "GameplayState.hpp"
#include "Gui.hpp"
#include "MainMenu.hpp"
#include "NetplayInternal.hpp"
#include "Player.hpp"
#include "ReplayManager.hpp"
#include "PortableSnapshotStorage.hpp"
#include "Session.hpp"
#include "SinglePlayerSnapshot.hpp"
#include "SoundPlayer.hpp"
#include "Stage.hpp"
#include "Supervisor.hpp"
#include "i18n.hpp"
#include "thprac_th06.h"

#include <SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace th06::PortableGameplayRestore
{
namespace
{
constexpr u32 kStatusDurationMs = 5000;
constexpr u32 kBootstrapWaitTimeoutMs = 8000;
constexpr u32 kBootstrapWaitLogIntervalMs = 1000;

struct RuntimeState
{
    Phase phase = Phase::Idle;
    Source source = Source::ManualMemory;
    bool hasTerminalResult = false;
    Phase terminalPhase = Phase::Idle;
    Source terminalSource = Source::ManualMemory;
    u32 phaseSinceTick = 0;
    u32 lastProgressLogTick = 0;
    u32 forcedClearFrames = 0;
    bool forceClearGameplayFrames = false;
    bool fromDisk = false;
    bool prevDiskHotkeyPressed = false;
    bool breakCurrentFrame = false;
    bool pendingMainMenuBootstrap = false;
    bool usedMainMenuBootstrap = false;
    std::vector<u8> queuedBytes;
    std::string queuedPath;
    std::string sourceTag;
    std::unique_ptr<DGS::PortableGameplayState> decodedState;
    std::unique_ptr<DGS::PortableGameplayBuildResult> expectedBuild;
    std::string statusLine1;
    std::string statusLine2;
    std::string terminalLine1;
    std::string terminalLine2;
    u32 statusUntilTick = 0;
};

struct MenuRestoreSnapshot
{
    bool valid = false;
    bool capturedFromMainMenu = false;
    bool restoreOnNextMainMenu = false;
    u8 character = 0;
    u8 shotType = 0;
    u8 character2 = 0;
    u8 shotType2 = 0;
    Difficulty difficulty = NORMAL;
    u8 isInPracticeMode = 0;
    u8 isInReplay = 0;
    u8 demoMode = 0;
    i32 currentStage = 0;
    i32 demoFrames = 0;
    i32 wantedState = 0;
    i32 curState = 0;
    i32 wantedState2 = 0;
    i32 calcCount = 0;
    i32 supervisorLastFrameTime = 0;
    f32 effectiveFramerateMultiplier = 1.0f;
    f32 framerateMultiplier = 1.0f;
    u32 startupTimeBeforeMenuMusic = 0;
    i32 tickCountToEffectiveFramerate = 0;
    double gameWindowLastFrameTime = 0.0;
    u8 gameWindowCurFrame = 0;
    int delay = Netplay::kDefaultDelay;
    int currentDelayCooldown = 0;
    int currentNetFrame = 0;
    Netplay::InGameCtrlType currentCtrl = Netplay::IGC_NONE;
};

RuntimeState g_PortableRestoreState;
MenuRestoreSnapshot g_MenuRestoreSnapshot;

bool IsPortableRestoreDeveloperModeActive()
{
    return THPrac::TH06::THPracIsDeveloperModeEnabled() && SinglePlayerSnapshot::IsPortableRestoreTrialEnabled();
}

bool NearlyEqualFloat(float lhs, float rhs, float epsilon = 0.01f)
{
    return std::fabs(lhs - rhs) <= epsilon;
}

bool NearlyEqualVec3(const D3DXVECTOR3 &lhs, const D3DXVECTOR3 &rhs, float epsilon = 0.01f)
{
    return NearlyEqualFloat(lhs.x, rhs.x, epsilon) && NearlyEqualFloat(lhs.y, rhs.y, epsilon) &&
           NearlyEqualFloat(lhs.z, rhs.z, epsilon);
}

bool IsIntroSpriteHidden(const AnmVm &vm)
{
    return vm.flags.isVisible == 0 || vm.currentInstruction == nullptr;
}

const char *ResolvePortableRestoreBombSpellcardTextFallback()
{
    const Character playerCharacter = (Character)g_GameManager.character;
    const ShotType playerShot = (ShotType)g_GameManager.shotType;

    switch (playerCharacter)
    {
    case CHARA_REIMU:
        return playerShot == SHOT_TYPE_B ? TH_REIMU_B_BOMB_NAME : TH_REIMU_A_BOMB_NAME;
    case CHARA_MARISA:
        return playerShot == SHOT_TYPE_B ? TH_MARISA_B_BOMB_NAME : TH_MARISA_A_BOMB_NAME;
    default:
        return nullptr;
    }
}

std::string ResolvePortableRestoreEnemySpellcardText(const DGS::PortableGuiState &state)
{
    if (!state.enemySpellcardText.empty())
    {
        return state.enemySpellcardText;
    }

    if (g_Gui.impl != nullptr && g_Gui.impl->enemySpellcardName.flags.isVisible && g_EnemyManager.spellcardInfo.idx >= 0 &&
        g_EnemyManager.spellcardInfo.idx < CATK_NUM_CAPTURES)
    {
        return g_GameManager.catk[g_EnemyManager.spellcardInfo.idx].name;
    }

    return {};
}

std::string ResolvePortableRestoreBombSpellcardText(const DGS::PortableGuiState &state)
{
    if (!state.bombSpellcardText.empty())
    {
        return state.bombSpellcardText;
    }

    if (g_Gui.impl != nullptr && g_Gui.impl->bombSpellcardName.flags.isVisible)
    {
        const char *bombName = ResolvePortableRestoreBombSpellcardTextFallback();
        return bombName != nullptr ? bombName : "";
    }

    return {};
}

void RedrawPortableGuiSpellcardTexts(const DGS::PortableGuiState &state)
{
    if (g_AnmManager == nullptr || g_Gui.impl == nullptr)
    {
        return;
    }

    if (g_Gui.impl->enemySpellcardName.flags.isVisible)
    {
        const std::string enemySpellcardText = ResolvePortableRestoreEnemySpellcardText(state);
        AnmManager::DrawStringFormat(g_AnmManager, &g_Gui.impl->enemySpellcardName, 0xfff0f0, COLOR_RGB(COLOR_BLACK),
                                     const_cast<char *>(enemySpellcardText.c_str()));
    }

    if (g_Gui.impl->bombSpellcardName.flags.isVisible)
    {
        const std::string bombSpellcardText = ResolvePortableRestoreBombSpellcardText(state);
        g_AnmManager->DrawVmTextFmt(g_AnmManager, &g_Gui.impl->bombSpellcardName, 0xf0f0ff, 0x0,
                                    const_cast<char *>(bombSpellcardText.c_str()));
    }
}

void SetStatus(const std::string &line1, const std::string &line2)
{
    g_PortableRestoreState.statusLine1 = line1;
    g_PortableRestoreState.statusLine2 = line2;
    g_PortableRestoreState.statusUntilTick = SDL_GetTicks() + kStatusDurationMs;
}

void SetPhase(Phase phase)
{
    g_PortableRestoreState.phase = phase;
    g_PortableRestoreState.phaseSinceTick = SDL_GetTicks();
    g_PortableRestoreState.lastProgressLogTick = 0;
}

void ClearQueuedState()
{
    g_PortableRestoreState.queuedBytes.clear();
    g_PortableRestoreState.queuedPath.clear();
    g_PortableRestoreState.sourceTag.clear();
    g_PortableRestoreState.decodedState.reset();
    g_PortableRestoreState.expectedBuild.reset();
    g_PortableRestoreState.pendingMainMenuBootstrap = false;
    g_PortableRestoreState.usedMainMenuBootstrap = false;
}

void CaptureMenuRestoreSnapshot()
{
    g_MenuRestoreSnapshot.valid = true;
    g_MenuRestoreSnapshot.capturedFromMainMenu = g_Supervisor.curState == SUPERVISOR_STATE_MAINMENU;
    g_MenuRestoreSnapshot.restoreOnNextMainMenu = false;
    g_MenuRestoreSnapshot.character = g_GameManager.character;
    g_MenuRestoreSnapshot.shotType = g_GameManager.shotType;
    g_MenuRestoreSnapshot.character2 = g_GameManager.character2;
    g_MenuRestoreSnapshot.shotType2 = g_GameManager.shotType2;
    g_MenuRestoreSnapshot.difficulty =
        g_Supervisor.cfg.defaultDifficulty < EXTRA ? (Difficulty)g_Supervisor.cfg.defaultDifficulty : NORMAL;
    g_MenuRestoreSnapshot.isInPracticeMode = 0;
    g_MenuRestoreSnapshot.isInReplay = 0;
    g_MenuRestoreSnapshot.demoMode = 0;
    g_MenuRestoreSnapshot.currentStage = 0;
    g_MenuRestoreSnapshot.demoFrames = 0;
    g_MenuRestoreSnapshot.wantedState = SUPERVISOR_STATE_MAINMENU;
    g_MenuRestoreSnapshot.curState = SUPERVISOR_STATE_MAINMENU;
    g_MenuRestoreSnapshot.wantedState2 = SUPERVISOR_STATE_MAINMENU;
    g_MenuRestoreSnapshot.calcCount = g_Supervisor.calcCount;
    g_MenuRestoreSnapshot.supervisorLastFrameTime = 0;
    g_MenuRestoreSnapshot.effectiveFramerateMultiplier = 1.0f;
    g_MenuRestoreSnapshot.framerateMultiplier = 1.0f;
    g_MenuRestoreSnapshot.startupTimeBeforeMenuMusic = 0;
    g_MenuRestoreSnapshot.tickCountToEffectiveFramerate = 0;
    g_MenuRestoreSnapshot.gameWindowLastFrameTime = 0.0;
    g_MenuRestoreSnapshot.gameWindowCurFrame = 0;
    g_MenuRestoreSnapshot.delay = Netplay::kDefaultDelay;
    g_MenuRestoreSnapshot.currentDelayCooldown = 0;
    g_MenuRestoreSnapshot.currentNetFrame = 0;
    g_MenuRestoreSnapshot.currentCtrl = Netplay::IGC_NONE;
}

void RestoreMenuRestoreSnapshotFields()
{
    if (!g_MenuRestoreSnapshot.valid)
    {
        return;
    }

    g_GameManager.character = g_MenuRestoreSnapshot.character;
    g_GameManager.shotType = g_MenuRestoreSnapshot.shotType;
    g_GameManager.character2 = g_MenuRestoreSnapshot.character2;
    g_GameManager.shotType2 = g_MenuRestoreSnapshot.shotType2;
    g_GameManager.difficulty = g_MenuRestoreSnapshot.difficulty;
    g_GameManager.isInPracticeMode = g_MenuRestoreSnapshot.isInPracticeMode;
    g_GameManager.isInReplay = g_MenuRestoreSnapshot.isInReplay;
    g_GameManager.demoMode = g_MenuRestoreSnapshot.demoMode;
    g_GameManager.currentStage = g_MenuRestoreSnapshot.currentStage;
    g_GameManager.demoFrames = g_MenuRestoreSnapshot.demoFrames;
    g_GameManager.menuCursorBackup = 0;
    g_Supervisor.cfg.defaultDifficulty = (u8)g_MenuRestoreSnapshot.difficulty;

    g_Supervisor.calcCount = g_MenuRestoreSnapshot.calcCount;
    g_Supervisor.lastFrameTime = g_MenuRestoreSnapshot.supervisorLastFrameTime;
    g_Supervisor.effectiveFramerateMultiplier = g_MenuRestoreSnapshot.effectiveFramerateMultiplier;
    g_Supervisor.framerateMultiplier = g_MenuRestoreSnapshot.framerateMultiplier;
    g_Supervisor.startupTimeBeforeMenuMusic = g_MenuRestoreSnapshot.startupTimeBeforeMenuMusic;

    g_TickCountToEffectiveFramerate = g_MenuRestoreSnapshot.tickCountToEffectiveFramerate;
    g_LastFrameTime = g_MenuRestoreSnapshot.gameWindowLastFrameTime;
    g_GameWindow.curFrame = g_MenuRestoreSnapshot.gameWindowCurFrame;

    Netplay::g_State.delay = g_MenuRestoreSnapshot.delay;
    Netplay::g_State.currentDelayCooldown = g_MenuRestoreSnapshot.currentDelayCooldown;
    Netplay::g_State.currentNetFrame = g_MenuRestoreSnapshot.currentNetFrame;
    Netplay::g_State.currentCtrl = g_MenuRestoreSnapshot.currentCtrl;

    THPrac::TH06::THPortableResetShellSyncTrackers();
    Controller::ResetDeviceInputState();
    Session::ResetLegacyInputState();
    g_PortableRestoreState.forceClearGameplayFrames = false;
}

void ForceCleanupGameplayShellForMenuReturn()
{
    Stage::CutChain();
    BulletManager::CutChain();
    Player::CutChain();
    EnemyManager::CutChain();
    g_EclManager.Unload();
    EffectManager::CutChain();
    Gui::CutChain();
    ReplayManager::StopRecording();
    GameManager::CutChain();

    g_Supervisor.StopAudio();
    THPrac::TH06::THPortableResetShellSyncTrackers();
    THPrac::TH06::THPracResetParams();
    g_AsciiManager.InitializeVms();

    g_GameManager.isInGameMenu = 0;
    g_GameManager.isInRetryMenu = 0;
    g_GameManager.isInMenu = 0;
    g_GameManager.isTimeStopped = false;
    g_GameManager.isGameCompleted = 0;
    g_GameManager.numRetries = 0;
    g_GameManager.currentStage = 0;
    g_GameManager.demoFrames = 0;
    g_GameManager.score = 0;
    g_GameManager.guiScore = 0;
    g_GameManager.nextScoreIncrement = 0;
    g_GameManager.currentPower = 0;
    g_GameManager.currentPower2 = 0;
    g_GameManager.pointItemsCollectedInStage = 0;
    g_GameManager.grazeInStage = 0;
    g_PortableRestoreState.forceClearGameplayFrames = false;
}

void ResetPostRestorePresentationState()
{
    if (g_AnmManager != nullptr)
    {
        g_AnmManager->SetCurrentBlendMode(0xff);
        g_AnmManager->SetCurrentColorOp(0xff);
        g_AnmManager->SetCurrentVertexShader(0xff);
        g_AnmManager->SetCurrentTexture(0);
        g_AnmManager->SetCurrentSprite(nullptr);
        g_AnmManager->SetCurrentZWriteDisable(0xff);
        g_AnmManager->currentTextureFactor = 0xffffffffu;
    }

    if (g_Gui.impl != nullptr)
    {
        g_Gui.impl->loadingScreenSprite.Initialize();
        g_Gui.impl->loadingScreenSprite.activeSpriteIndex = -1;
        g_Gui.impl->loadingScreenSprite.beginingOfScript = nullptr;
        g_Gui.impl->loadingScreenSprite.currentInstruction = nullptr;
        g_Gui.impl->loadingScreenSprite.sprite = nullptr;
        g_Gui.impl->loadingScreenSprite.pendingInterrupt = 0;
        g_Gui.impl->loadingScreenSprite.flags.isVisible = 0;
        g_Gui.impl->loadingScreenSprite.flags.isStopped = 1;
    }

    g_Stage.skyFogNeedsSetup = 1;
}

void RestoreMenuRestoreSnapshotStateIds()
{
    if (!g_MenuRestoreSnapshot.valid)
    {
        return;
    }

    g_Supervisor.wantedState = g_MenuRestoreSnapshot.wantedState;
    g_Supervisor.curState = g_MenuRestoreSnapshot.curState;
    g_Supervisor.wantedState2 = g_MenuRestoreSnapshot.wantedState2;
}

void Finish(Phase phase, const std::string &line1, const std::string &line2)
{
    SetPhase(phase);
    g_PortableRestoreState.breakCurrentFrame = true;
    if (phase == Phase::Completed)
    {
        g_PortableRestoreState.forcedClearFrames = 3;
        g_PortableRestoreState.forceClearGameplayFrames = g_PortableRestoreState.usedMainMenuBootstrap;
        ResetPostRestorePresentationState();
    }
    THPrac::TH06::THPracResetParams();
    if (phase == Phase::Completed && g_MenuRestoreSnapshot.valid)
    {
        g_MenuRestoreSnapshot.restoreOnNextMainMenu = true;
    }
    SetStatus(line1, line2);
    if (phase == Phase::Completed || phase == Phase::Failed)
    {
        g_PortableRestoreState.hasTerminalResult = true;
        g_PortableRestoreState.terminalPhase = phase;
        g_PortableRestoreState.terminalSource = g_PortableRestoreState.source;
        g_PortableRestoreState.terminalLine1 = line1;
        g_PortableRestoreState.terminalLine2 = line2;
    }
    ClearQueuedState();
    SetPhase(Phase::Idle);
}

void LogLine(const char *line)
{
    if (!THPrac::TH06::THPracIsDebugLogEnabled())
    {
        return;
    }

    GameErrorContext::Log(&g_GameErrorContext, "%s\n", line);

    char resolvedLogPath[512];
    GamePaths::Resolve(resolvedLogPath, sizeof(resolvedLogPath), "./log.txt");
    GamePaths::EnsureParentDir(resolvedLogPath);

    FILE *file = nullptr;
    if (fopen_s(&file, resolvedLogPath, "a") == 0 && file != nullptr)
    {
        std::fprintf(file, "%s\n", line);
        std::fflush(file);
        std::fclose(file);
    }
}

void LogPortableRestoreEvent(const char *event, const char *detail)
{
    char line[768];
    std::snprintf(line, sizeof(line), "[PortableRestore] phase=%d event=%s source=%s detail=%s",
                  (int)g_PortableRestoreState.phase, event,
                  g_PortableRestoreState.sourceTag.empty() ? "" : g_PortableRestoreState.sourceTag.c_str(),
                  detail != nullptr ? detail : "");
    LogLine(line);
}

bool ShouldEmitProgressLog()
{
    const u32 now = SDL_GetTicks();
    if (g_PortableRestoreState.lastProgressLogTick == 0 ||
        now - g_PortableRestoreState.lastProgressLogTick >= kBootstrapWaitLogIntervalMs)
    {
        g_PortableRestoreState.lastProgressLogTick = now;
        return true;
    }
    return false;
}

bool HasPhaseTimedOut()
{
    return SDL_GetTicks() - g_PortableRestoreState.phaseSinceTick >= kBootstrapWaitTimeoutMs;
}

void LogPortablePostVerifyDiffLine(const char *label, const char *detail)
{
    char line[512];
    std::snprintf(line, sizeof(line), "post-verify-diff:%s %s", label != nullptr ? label : "(null)",
                  detail != nullptr ? detail : "");
    LogPortableRestoreEvent("post-verify-diff", line);
}

void NotePostVerifyMismatch(std::string &firstReason, const char *reason)
{
    if (firstReason.empty() && reason != nullptr)
    {
        firstReason = reason;
    }
}

bool ComparePostVerifyU64(std::string &firstReason, const char *label, u64 expected, u64 actual, const char *reason)
{
    if (expected == actual)
    {
        return true;
    }

    NotePostVerifyMismatch(firstReason, reason);
    char detail[256];
    std::snprintf(detail, sizeof(detail), "expected=0x%016llX actual=0x%016llX", (unsigned long long)expected,
                  (unsigned long long)actual);
    LogPortablePostVerifyDiffLine(label, detail);
    return false;
}

bool ComparePostVerifyU32(std::string &firstReason, const char *label, u32 expected, u32 actual, const char *reason)
{
    if (expected == actual)
    {
        return true;
    }

    NotePostVerifyMismatch(firstReason, reason);
    char detail[256];
    std::snprintf(detail, sizeof(detail), "expected=%u actual=%u", expected, actual);
    LogPortablePostVerifyDiffLine(label, detail);
    return false;
}

bool ComparePostVerifyI32(std::string &firstReason, const char *label, i32 expected, i32 actual, const char *reason)
{
    if (expected == actual)
    {
        return true;
    }

    NotePostVerifyMismatch(firstReason, reason);
    char detail[256];
    std::snprintf(detail, sizeof(detail), "expected=%d actual=%d", expected, actual);
    LogPortablePostVerifyDiffLine(label, detail);
    return false;
}

u64 FingerprintPortableExplicitState(const DGS::PortableGameplayState &state)
{
    auto sanitized = std::make_unique<DGS::PortableGameplayState>(state);
    sanitized->shadowFingerprints.dgs = {};
    return DGS::FingerprintPortableGameplayState(*sanitized);
}

bool IsDiskPortableLoadHotkeyPressed(const Uint8 *keyboardState)
{
    const bool ctrlPressed = keyboardState[SDL_SCANCODE_LCTRL] != 0 || keyboardState[SDL_SCANCODE_RCTRL] != 0;
    const bool lPressed = keyboardState[SDL_SCANCODE_L] != 0;
    return IsPortableRestoreDeveloperModeActive() && ctrlPressed && lPressed;
}

bool IsSupportedSupervisorState(i32 state)
{
    return state == SUPERVISOR_STATE_MAINMENU || state == SUPERVISOR_STATE_GAMEMANAGER ||
           state == SUPERVISOR_STATE_GAMEMANAGER_REINIT;
}

bool IsUnsupportedGameplayMode()
{
    if (Session::IsRemoteNetplaySession() && g_PortableRestoreState.source != Source::AuthoritativeNetplayRecovery)
    {
        return true;
    }

    return g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER && g_GameManager.isInReplay != 0;
}

bool IsAuthoritativeNetplayTopologyCompatible(const DGS::PortableGameplayResourceCatalog &catalog)
{
    if (g_PortableRestoreState.source != Source::AuthoritativeNetplayRecovery)
    {
        return false;
    }

    return Session::IsRemoteNetplaySession() && Session::IsDualPlayerSession() && catalog.hasSecondPlayer == 1;
}

bool IsGameplayShellReady()
{
    return g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER && g_Stage.stdData != nullptr &&
           g_EclManager.eclFile != nullptr && g_Stage.objects != nullptr && g_Stage.objectInstances != nullptr &&
           g_Stage.quadVms != nullptr;
}

bool IsMainMenuShellStable()
{
    if (g_Supervisor.curState != SUPERVISOR_STATE_MAINMENU)
    {
        return false;
    }

    if (g_MainMenu.chainCalc == nullptr || g_MainMenu.chainDraw == nullptr)
    {
        return false;
    }

    return g_MainMenu.gameState == STATE_PRE_INPUT || g_MainMenu.gameState == STATE_MAIN_MENU ||
           g_MainMenu.gameState == STATE_ONLINE;
}

bool CurrentCatalogMatches(const DGS::PortableGameplayResourceCatalog &catalog)
{
    return (u32)g_GameManager.currentStage == catalog.currentStage && (u32)g_GameManager.difficulty == catalog.difficulty &&
           g_GameManager.character == catalog.character1 && g_GameManager.shotType == catalog.shotType1 &&
           g_GameManager.character2 == catalog.character2 && g_GameManager.shotType2 == catalog.shotType2 &&
           (Session::IsDualPlayerSession() ? 1u : 0u) == catalog.hasSecondPlayer &&
           (u32)g_GameManager.isInPracticeMode == catalog.isPracticeMode && (u32)g_GameManager.isInReplay == catalog.isReplay;
}

f32 ComputeRefreshRateFromMainMenu()
{
    f32 measuredRefreshRate = 60.0f;
    if (g_MainMenu.timeRelatedArrSize >= 2)
    {
        measuredRefreshRate = 0.0f;
        for (i32 i = 0; i < g_MainMenu.timeRelatedArrSize; ++i)
        {
            measuredRefreshRate += g_MainMenu.timeRelatedArr[i];
        }
        measuredRefreshRate /= (f32)g_MainMenu.timeRelatedArrSize;
    }

    if (measuredRefreshRate >= 155.0f)
        return 60.0f / 160.0f;
    if (measuredRefreshRate >= 135.0f)
        return 60.0f / 150.0f;
    if (measuredRefreshRate >= 110.0f)
        return 60.0f / 120.0f;
    if (measuredRefreshRate >= 95.0f)
        return 60.0f / 100.0f;
    if (measuredRefreshRate >= 87.5f)
        return 60.0f / 90.0f;
    if (measuredRefreshRate >= 82.5f)
        return 60.0f / 85.0f;
    if (measuredRefreshRate >= 77.5f)
        return 60.0f / 80.0f;
    if (measuredRefreshRate >= 73.5f)
        return 60.0f / 75.0f;
    if (measuredRefreshRate >= 68.0f)
        return 60.0f / 70.0f;
    return 1.0f;
}

void ApplyBootstrapBaseline(const DGS::PortableGameplayResourceCatalog &catalog)
{
    g_GameManager.character = (u8)catalog.character1;
    g_GameManager.shotType = (u8)catalog.shotType1;
    g_GameManager.character2 = (u8)catalog.character2;
    g_GameManager.shotType2 = (u8)catalog.shotType2;
    g_GameManager.difficulty = (Difficulty)catalog.difficulty;
    g_GameManager.isInPracticeMode = (u8)catalog.isPracticeMode;
    g_GameManager.isInReplay = 0;
    g_GameManager.demoMode = 0;
    g_GameManager.currentStage = (i32)catalog.currentStage - 1;
}

void ApplyBootstrapStartResources(const DGS::PortableGameplayResourceCatalog &catalog)
{
    ApplyBootstrapBaseline(catalog);

    g_GameManager.livesRemaining = g_Supervisor.cfg.lifeCount;
    g_GameManager.bombsRemaining = g_Supervisor.cfg.bombCount;
    if (Session::IsDualPlayerSession())
    {
        g_GameManager.livesRemaining2 = g_Supervisor.cfg.lifeCount;
        g_GameManager.bombsRemaining2 = g_Supervisor.cfg.bombCount;
    }

    if (g_GameManager.difficulty == EXTRA)
    {
        g_GameManager.livesRemaining = 2;
        g_GameManager.bombsRemaining = 3;
        if (Session::IsDualPlayerSession())
        {
            g_GameManager.livesRemaining2 = 2;
            g_GameManager.bombsRemaining2 = 3;
        }
    }

    if (g_GameManager.isInPracticeMode != 0)
    {
        g_GameManager.livesRemaining = 8;
        g_GameManager.bombsRemaining = 8;
        if (Session::IsDualPlayerSession())
        {
            g_GameManager.livesRemaining2 = 8;
            g_GameManager.bombsRemaining2 = 8;
        }
    }
}

void FailNow(const std::string &line1, const std::string &line2)
{
    LogPortableRestoreEvent("fail", line2.c_str());
    Finish(Phase::Failed, line1, line2);
}

void RequestPortableRestoreFailureReturnToMenu()
{
    ForceCleanupGameplayShellForMenuReturn();
    RestoreMenuRestoreSnapshotFields();
    g_MenuRestoreSnapshot.restoreOnNextMainMenu = false;
    g_MenuRestoreSnapshot.valid = false;
    if (g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER ||
        g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER_REINIT)
    {
        if (g_Supervisor.wantedState != SUPERVISOR_STATE_GAMEMANAGER &&
            g_Supervisor.wantedState != SUPERVISOR_STATE_GAMEMANAGER_REINIT)
        {
            g_Supervisor.wantedState = SUPERVISOR_STATE_GAMEMANAGER;
        }
        g_Supervisor.curState = SUPERVISOR_STATE_MAINMENU;
        return;
    }

    g_Supervisor.curState = SUPERVISOR_STATE_MAINMENU;
    g_Supervisor.wantedState = SUPERVISOR_STATE_MAINMENU;
    g_Supervisor.wantedState2 = SUPERVISOR_STATE_MAINMENU;
}

template <typename T> T *OffsetToPointer(i32 offset, void *base)
{
    if (offset < 0 || base == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<T *>(reinterpret_cast<u8 *>(base) + offset);
}

FireBulletCallback ResolvePlayerFireToken(DGS::PortablePlayerFireFuncToken token)
{
    switch (token)
    {
    case DGS::PortablePlayerFireFuncToken::None:
        return nullptr;
    case DGS::PortablePlayerFireFuncToken::FireBulletReimuA:
        return Player::FireBulletReimuA;
    case DGS::PortablePlayerFireFuncToken::FireBulletReimuB:
        return Player::FireBulletReimuB;
    case DGS::PortablePlayerFireFuncToken::FireBulletMarisaA:
        return Player::FireBulletMarisaA;
    case DGS::PortablePlayerFireFuncToken::FireBulletMarisaB:
        return Player::FireBulletMarisaB;
    default:
        return nullptr;
    }
}

void (*ResolvePlayerBombToken(DGS::PortablePlayerBombFuncToken token))(Player *)
{
    switch (token)
    {
    case DGS::PortablePlayerBombFuncToken::None:
        return nullptr;
    case DGS::PortablePlayerBombFuncToken::BombReimuACalc:
        return BombData::BombReimuACalc;
    case DGS::PortablePlayerBombFuncToken::BombReimuBCalc:
        return BombData::BombReimuBCalc;
    case DGS::PortablePlayerBombFuncToken::BombMarisaACalc:
        return BombData::BombMarisaACalc;
    case DGS::PortablePlayerBombFuncToken::BombMarisaBCalc:
        return BombData::BombMarisaBCalc;
    case DGS::PortablePlayerBombFuncToken::BombReimuADraw:
        return BombData::BombReimuADraw;
    case DGS::PortablePlayerBombFuncToken::BombReimuBDraw:
        return BombData::BombReimuBDraw;
    case DGS::PortablePlayerBombFuncToken::BombMarisaADraw:
        return BombData::BombMarisaADraw;
    case DGS::PortablePlayerBombFuncToken::BombMarisaBDraw:
        return BombData::BombMarisaBDraw;
    case DGS::PortablePlayerBombFuncToken::DarkenViewport:
        return BombData::DarkenViewport;
    default:
        return nullptr;
    }
}

EffectUpdateCallback ResolveEffectUpdateToken(DGS::PortableEffectUpdateToken token)
{
    switch (token)
    {
    case DGS::PortableEffectUpdateToken::None:
        return nullptr;
    case DGS::PortableEffectUpdateToken::EffectCallbackRandomSplash:
        return EffectManager::EffectCallbackRandomSplash;
    case DGS::PortableEffectUpdateToken::EffectCallbackRandomSplashBig:
        return EffectManager::EffectCallbackRandomSplashBig;
    case DGS::PortableEffectUpdateToken::EffectCallbackStill:
        return EffectManager::EffectCallbackStill;
    case DGS::PortableEffectUpdateToken::EffectUpdateCallback4:
        return EffectManager::EffectUpdateCallback4;
    case DGS::PortableEffectUpdateToken::EffectCallbackAttract:
        return EffectManager::EffectCallbackAttract;
    case DGS::PortableEffectUpdateToken::EffectCallbackAttractSlow:
        return EffectManager::EffectCallbackAttractSlow;
    default:
        return nullptr;
    }
}

void (*ResolveEnemyFuncToken(DGS::PortableEnemyFuncToken token))(Enemy *, EclRawInstr *)
{
    switch (token)
    {
    case DGS::PortableEnemyFuncToken::None:
        return nullptr;
    case DGS::PortableEnemyFuncToken::ExInsCirnoRainbowBallJank:
        return EnemyEclInstr::ExInsCirnoRainbowBallJank;
    case DGS::PortableEnemyFuncToken::ExInsShootAtRandomArea:
        return EnemyEclInstr::ExInsShootAtRandomArea;
    case DGS::PortableEnemyFuncToken::ExInsShootStarPattern:
        return EnemyEclInstr::ExInsShootStarPattern;
    case DGS::PortableEnemyFuncToken::ExInsPatchouliShottypeSetVars:
        return EnemyEclInstr::ExInsPatchouliShottypeSetVars;
    case DGS::PortableEnemyFuncToken::ExInsStage56Func4:
        return EnemyEclInstr::ExInsStage56Func4;
    case DGS::PortableEnemyFuncToken::ExInsStage5Func5:
        return EnemyEclInstr::ExInsStage5Func5;
    case DGS::PortableEnemyFuncToken::ExInsStage6XFunc6:
        return EnemyEclInstr::ExInsStage6XFunc6;
    case DGS::PortableEnemyFuncToken::ExInsStage6Func7:
        return EnemyEclInstr::ExInsStage6Func7;
    case DGS::PortableEnemyFuncToken::ExInsStage6Func8:
        return EnemyEclInstr::ExInsStage6Func8;
    case DGS::PortableEnemyFuncToken::ExInsStage6Func9:
        return EnemyEclInstr::ExInsStage6Func9;
    case DGS::PortableEnemyFuncToken::ExInsStage6XFunc10:
        return EnemyEclInstr::ExInsStage6XFunc10;
    case DGS::PortableEnemyFuncToken::ExInsStage6Func11:
        return EnemyEclInstr::ExInsStage6Func11;
    case DGS::PortableEnemyFuncToken::ExInsStage4Func12:
        return EnemyEclInstr::ExInsStage4Func12;
    case DGS::PortableEnemyFuncToken::ExInsStageXFunc13:
        return EnemyEclInstr::ExInsStageXFunc13;
    case DGS::PortableEnemyFuncToken::ExInsStageXFunc14:
        return EnemyEclInstr::ExInsStageXFunc14;
    case DGS::PortableEnemyFuncToken::ExInsStageXFunc15:
        return EnemyEclInstr::ExInsStageXFunc15;
    case DGS::PortableEnemyFuncToken::ExInsStageXFunc16:
        return EnemyEclInstr::ExInsStageXFunc16;
    default:
        return nullptr;
    }
}

void RestorePortableAnmVmState(AnmVm &vm, const DGS::PortableAnmVmState &state)
{
    vm.rotation = state.rotation;
    vm.angleVel = state.angleVel;
    vm.scaleY = state.scaleY;
    vm.scaleX = state.scaleX;
    vm.scaleInterpFinalY = state.scaleInterpFinalY;
    vm.scaleInterpFinalX = state.scaleInterpFinalX;
    vm.uvScrollPos = state.uvScrollPos;
    vm.currentTimeInScript = state.currentTimeInScript;
    vm.matrix = state.matrix;
    vm.color = state.color;
    vm.flags.flags = state.flags;
    vm.alphaInterpEndTime = state.alphaInterpEndTime;
    vm.scaleInterpEndTime = state.scaleInterpEndTime;
    vm.autoRotate = state.autoRotate;
    vm.pendingInterrupt = state.pendingInterrupt;
    vm.posInterpEndTime = state.posInterpEndTime;
    vm.pos = state.pos;
    vm.scaleInterpInitialY = state.scaleInterpInitialY;
    vm.scaleInterpInitialX = state.scaleInterpInitialX;
    vm.scaleInterpTime = state.scaleInterpTime;
    vm.activeSpriteIndex = state.activeSpriteIndex;
    vm.baseSpriteIndex = state.baseSpriteIndex;
    vm.anmFileIndex = state.anmFileIndex;
    vm.alphaInterpInitial = state.alphaInterpInitial;
    vm.alphaInterpFinal = state.alphaInterpFinal;
    vm.posInterpInitial = state.posInterpInitial;
    vm.posInterpFinal = state.posInterpFinal;
    vm.posOffset = state.posOffset;
    vm.posInterpTime = state.posInterpTime;
    vm.timeOfLastSpriteSet = state.timeOfLastSpriteSet;
    vm.alphaInterpTime = state.alphaInterpTime;
    vm.fontWidth = state.fontWidth;
    vm.fontHeight = state.fontHeight;
    DGS::RestoreAnmVmRefs(vm, state.refs);
}

void RestorePortableEnemyFlags(EnemyFlags &flags, const DGS::PortableEnemyFlagsState &state)
{
    flags.unk1 = state.unk1;
    flags.unk2 = state.unk2;
    flags.unk3 = state.unk3;
    flags.unk4 = state.unk4;
    flags.unk5 = state.unk5;
    flags.unk6 = state.unk6;
    flags.unk7 = state.unk7;
    flags.unk8 = state.unk8;
    flags.isBoss = state.isBoss;
    flags.unk10 = state.unk10;
    flags.unk11 = state.unk11;
    flags.shouldClampPos = state.shouldClampPos != 0;
    flags.unk13 = state.unk13;
    flags.unk14 = state.unk14;
    flags.unk15 = state.unk15;
    flags.unk16 = state.unk16;
}

void RestorePortableEnemyContext(EnemyEclContext &context, const DGS::PortableEnemyEclContextState &state)
{
    context.time = state.time;
    context.funcSetFunc = state.hasFuncToken && !state.hasUnknownFuncToken ? ResolveEnemyFuncToken(state.funcToken) : nullptr;
    context.var0 = state.var0;
    context.var1 = state.var1;
    context.var2 = state.var2;
    context.var3 = state.var3;
    context.float0 = state.float0;
    context.float1 = state.float1;
    context.float2 = state.float2;
    context.float3 = state.float3;
    context.var4 = state.var4;
    context.var5 = state.var5;
    context.var6 = state.var6;
    context.var7 = state.var7;
    context.compareRegister = state.compareRegister;
    context.subId = state.subId;
    context.currentInstr = OffsetToPointer<EclRawInstr>(state.scriptOffset, g_EclManager.eclFile);
}

void RestorePortableEnemyBulletShooter(EnemyBulletShooter &dst, const DGS::PortableEnemyBulletShooterState &src)
{
    dst.sprite = src.sprite;
    dst.spriteOffset = src.spriteOffset;
    dst.position = src.position;
    dst.angle1 = src.angle1;
    dst.angle2 = src.angle2;
    dst.speed1 = src.speed1;
    dst.speed2 = src.speed2;
    std::copy(std::begin(src.exFloats), std::end(src.exFloats), std::begin(dst.exFloats));
    std::copy(std::begin(src.exInts), std::end(src.exInts), std::begin(dst.exInts));
    dst.unk_40 = src.unk_40;
    dst.count1 = src.count1;
    dst.count2 = src.count2;
    dst.aimMode = src.aimMode;
    dst.unk_4a = src.unk_4a;
    dst.flags = src.flags;
    dst.provokedPlayer = src.provokedPlayer;
    dst.sfx = src.sfx;
}

void RestorePortableEnemyLaserShooter(EnemyLaserShooter &dst, const DGS::PortableEnemyLaserShooterState &src)
{
    dst.sprite = src.sprite;
    dst.spriteOffset = src.spriteOffset;
    dst.position = src.position;
    dst.angle = src.angle;
    dst.unk_14 = src.unk_14;
    dst.speed = src.speed;
    dst.unk_1c = src.unk_1c;
    dst.startOffset = src.startOffset;
    dst.endOffset = src.endOffset;
    dst.startLength = src.startLength;
    dst.width = src.width;
    dst.startTime = src.startTime;
    dst.duration = src.duration;
    dst.despawnDuration = src.despawnDuration;
    dst.hitboxStartTime = src.hitboxStartTime;
    dst.hitboxEndDelay = src.hitboxEndDelay;
    dst.unk_44 = src.unk_44;
    dst.type = src.type;
    dst.flags = src.flags;
    dst.unk_50 = src.unk_50;
    dst.provokedPlayer = src.provokedPlayer;
}

void RestorePortableEnemyState(Enemy &enemy, const DGS::PortableEnemyState &state)
{
    RestorePortableAnmVmState(enemy.primaryVm, state.primaryVm);
    for (size_t i = 0; i < state.vms.size(); ++i)
    {
        RestorePortableAnmVmState(enemy.vms[i], state.vms[i]);
    }
    RestorePortableEnemyContext(enemy.currentContext, state.currentContext);
    for (size_t i = 0; i < state.savedContextStack.size(); ++i)
    {
        RestorePortableEnemyContext(enemy.savedContextStack[i], state.savedContextStack[i]);
    }
    enemy.stackDepth = state.stackDepth;
    enemy.unk_c40 = state.unk_c40;
    enemy.deathCallbackSub = state.deathCallbackSub;
    std::copy(std::begin(state.interrupts), std::end(state.interrupts), std::begin(enemy.interrupts));
    enemy.runInterrupt = state.runInterrupt;
    enemy.position = state.position;
    enemy.hitboxDimensions = state.hitboxDimensions;
    enemy.axisSpeed = state.axisSpeed;
    enemy.angle = state.angle;
    enemy.angularVelocity = state.angularVelocity;
    enemy.speed = state.speed;
    enemy.acceleration = state.acceleration;
    enemy.shootOffset = state.shootOffset;
    enemy.moveInterp = state.moveInterp;
    enemy.moveInterpStartPos = state.moveInterpStartPos;
    enemy.moveInterpTimer = state.moveInterpTimer;
    enemy.moveInterpStartTime = state.moveInterpStartTime;
    enemy.bulletRankSpeedLow = state.bulletRankSpeedLow;
    enemy.bulletRankSpeedHigh = state.bulletRankSpeedHigh;
    enemy.bulletRankAmount1Low = state.bulletRankAmount1Low;
    enemy.bulletRankAmount1High = state.bulletRankAmount1High;
    enemy.bulletRankAmount2Low = state.bulletRankAmount2Low;
    enemy.bulletRankAmount2High = state.bulletRankAmount2High;
    enemy.life = state.life;
    enemy.maxLife = state.maxLife;
    enemy.score = state.score;
    enemy.bossTimer = state.bossTimer;
    enemy.color = state.color;
    RestorePortableEnemyBulletShooter(enemy.bulletProps, state.bulletProps);
    enemy.shootInterval = state.shootInterval;
    enemy.shootIntervalTimer = state.shootIntervalTimer;
    RestorePortableEnemyLaserShooter(enemy.laserProps, state.laserProps);
    enemy.laserStore = state.laserStore;
    enemy.deathAnm1 = state.deathAnm1;
    enemy.deathAnm2 = state.deathAnm2;
    enemy.deathAnm3 = state.deathAnm3;
    enemy.itemDrop = state.itemDrop;
    enemy.bossId = state.bossId;
    enemy.unk_e41 = state.unk_e41;
    enemy.exInsFunc10Timer = state.exInsFunc10Timer;
    RestorePortableEnemyFlags(enemy.flags, state.flags);
    enemy.anmExFlags = state.anmExFlags;
    enemy.anmExDefaults = state.anmExDefaults;
    enemy.anmExFarLeft = state.anmExFarLeft;
    enemy.anmExFarRight = state.anmExFarRight;
    enemy.anmExLeft = state.anmExLeft;
    enemy.anmExRight = state.anmExRight;
    enemy.lowerMoveLimit = state.lowerMoveLimit;
    enemy.upperMoveLimit = state.upperMoveLimit;
    enemy.effectIdx = state.effectIdx;
    enemy.effectDistance = state.effectDistance;
    enemy.lifeCallbackThreshold = state.lifeCallbackThreshold;
    enemy.lifeCallbackSub = state.lifeCallbackSub;
    enemy.timerCallbackThreshold = state.timerCallbackThreshold;
    enemy.timerCallbackSub = state.timerCallbackSub;
    enemy.exInsFunc6Angle = state.exInsFunc6Angle;
    enemy.exInsFunc6Timer = state.exInsFunc6Timer;
    enemy.provokedPlayer = state.provokedPlayer;
}

void RestorePortableRunningSpellcard(RunningSpellcardInfo &dst, const DGS::PortableRunningSpellcardState &src)
{
    dst.isCapturing = src.isCapturing;
    dst.isActive = src.isActive;
    dst.captureScore = src.captureScore;
    dst.idx = src.idx;
    dst.usedBomb = src.usedBomb;
}

void RestorePortableEnemyManagerState(const DGS::PortableEnemyManagerState &state)
{
    RestorePortableEnemyState(g_EnemyManager.enemyTemplate, state.enemyTemplate);
    const size_t enemyCount = std::min(state.enemies.size(), std::size(g_EnemyManager.enemies));
    for (size_t i = 0; i < enemyCount; ++i)
    {
        RestorePortableEnemyState(g_EnemyManager.enemies[i], state.enemies[i]);
    }
    g_EnemyManager.randomItemSpawnIndex = state.randomItemSpawnIndex;
    g_EnemyManager.randomItemTableIndex = state.randomItemTableIndex;
    g_EnemyManager.enemyCount = state.enemyCount;
    RestorePortableRunningSpellcard(g_EnemyManager.spellcardInfo, state.spellcardInfo);
    g_EnemyManager.unk_ee5d8 = state.unk_ee5d8;
    g_EnemyManager.timelineTime = state.timelineTime;
}

void RestorePortablePlayerBulletState(PlayerBullet &bullet, const DGS::PortablePlayerBulletState &state)
{
    RestorePortableAnmVmState(bullet.sprite, state.sprite);
    bullet.position = state.position;
    bullet.size = state.size;
    bullet.velocity = state.velocity;
    bullet.sidewaysMotion = state.sidewaysMotion;
    bullet.unk_134 = state.unk_134;
    bullet.unk_140 = state.timer;
    bullet.damage = state.damage;
    bullet.bulletState = state.bulletState;
    bullet.bulletType = state.bulletType;
    bullet.unk_152 = state.unk_152;
    bullet.spawnPositionIdx = state.spawnPositionIdx;
}

void RestorePortablePlayerBombState(PlayerBombInfo &bombInfo, const DGS::PortablePlayerBombState &state)
{
    bombInfo.isInUse = state.isInUse;
    bombInfo.duration = state.duration;
    bombInfo.timer = state.timer;
    bombInfo.calc = state.hasUnknownCalcToken ? nullptr : ResolvePlayerBombToken(state.calcToken);
    bombInfo.draw = state.hasUnknownDrawToken ? nullptr : ResolvePlayerBombToken(state.drawToken);
    std::copy(std::begin(state.reimuABombProjectilesState), std::end(state.reimuABombProjectilesState),
              std::begin(bombInfo.reimuABombProjectilesState));
    std::copy(std::begin(state.reimuABombProjectilesRelated), std::end(state.reimuABombProjectilesRelated),
              std::begin(bombInfo.reimuABombProjectilesRelated));
    std::copy(std::begin(state.bombRegionPositions), std::end(state.bombRegionPositions),
              std::begin(bombInfo.bombRegionPositions));
    std::copy(std::begin(state.bombRegionVelocities), std::end(state.bombRegionVelocities),
              std::begin(bombInfo.bombRegionVelocities));
    for (size_t ring = 0; ring < state.sprites.size(); ++ring)
    {
        for (size_t sprite = 0; sprite < state.sprites[ring].size(); ++sprite)
        {
            RestorePortableAnmVmState(bombInfo.sprites[ring][sprite], state.sprites[ring][sprite]);
        }
    }
}

void RestorePortableCharacterData(CharacterData &data, const DGS::PortableCharacterDataState &state)
{
    data.orthogonalMovementSpeed = state.orthogonalMovementSpeed;
    data.orthogonalMovementSpeedFocus = state.orthogonalMovementSpeedFocus;
    data.diagonalMovementSpeed = state.diagonalMovementSpeed;
    data.diagonalMovementSpeedFocus = state.diagonalMovementSpeedFocus;
    data.fireBulletCallback = state.hasUnknownFireToken ? nullptr : ResolvePlayerFireToken(state.fireBulletToken);
    data.fireBulletFocusCallback =
        state.hasUnknownFocusFireToken ? nullptr : ResolvePlayerFireToken(state.fireBulletFocusToken);
}

void RestorePortablePlayerState(Player &player, const DGS::PortablePlayerState &state)
{
    RestorePortableAnmVmState(player.playerSprite, state.playerSprite);
    for (size_t i = 0; i < state.orbsSprite.size(); ++i)
    {
        RestorePortableAnmVmState(player.orbsSprite[i], state.orbsSprite[i]);
    }
    player.positionCenter = state.positionCenter;
    player.unk_44c = state.unk_44c;
    player.hitboxTopLeft = state.hitboxTopLeft;
    player.hitboxBottomRight = state.hitboxBottomRight;
    player.grabItemTopLeft = state.grabItemTopLeft;
    player.grabItemBottomRight = state.grabItemBottomRight;
    player.hitboxSize = state.hitboxSize;
    player.grabItemSize = state.grabItemSize;
    std::copy(std::begin(state.orbsPosition), std::end(state.orbsPosition), std::begin(player.orbsPosition));
    std::copy(std::begin(state.bombRegionPositions), std::end(state.bombRegionPositions), std::begin(player.bombRegionPositions));
    std::copy(std::begin(state.bombRegionSizes), std::end(state.bombRegionSizes), std::begin(player.bombRegionSizes));
    std::copy(std::begin(state.bombRegionDamages), std::end(state.bombRegionDamages), std::begin(player.bombRegionDamages));
    std::copy(std::begin(state.unk_838), std::end(state.unk_838), std::begin(player.unk_838));
    std::copy(std::begin(state.bombProjectiles), std::end(state.bombProjectiles), std::begin(player.bombProjectiles));
    std::copy(std::begin(state.laserTimer), std::end(state.laserTimer), std::begin(player.laserTimer));
    player.horizontalMovementSpeedMultiplierDuringBomb = state.horizontalMovementSpeedMultiplierDuringBomb;
    player.verticalMovementSpeedMultiplierDuringBomb = state.verticalMovementSpeedMultiplierDuringBomb;
    player.respawnTimer = state.respawnTimer;
    player.bulletGracePeriod = state.bulletGracePeriod;
    player.playerState = state.playerState;
    player.playerType = state.playerType;
    player.unk_9e1 = state.unk_9e1;
    player.orbState = state.orbState;
    player.isFocus = state.isFocus;
    player.unk_9e4 = state.unk_9e4;
    player.focusMovementTimer = state.focusMovementTimer;
    RestorePortableCharacterData(player.characterData, state.characterData);
    player.playerDirection = (PlayerDirection)state.playerDirection;
    player.previousHorizontalSpeed = state.previousHorizontalSpeed;
    player.previousVerticalSpeed = state.previousVerticalSpeed;
    player.previousFrameInput = state.previousFrameInput;
    player.positionOfLastEnemyHit = state.positionOfLastEnemyHit;
    for (size_t i = 0; i < state.bullets.size(); ++i)
    {
        RestorePortablePlayerBulletState(player.bullets[i], state.bullets[i]);
    }
    player.fireBulletTimer = state.fireBulletTimer;
    player.invulnerabilityTimer = state.invulnerabilityTimer;
    player.fireBulletCallback = state.hasUnknownFireToken ? nullptr : ResolvePlayerFireToken(state.fireBulletToken);
    player.fireBulletFocusCallback =
        state.hasUnknownFocusFireToken ? nullptr : ResolvePlayerFireToken(state.fireBulletFocusToken);
    RestorePortablePlayerBombState(player.bombInfo, state.bombInfo);
    RestorePortableAnmVmState(player.hitboxSprite, state.hitboxSprite);
    player.hitboxTime = state.hitboxTime;
    player.lifegiveTime = state.lifegiveTime;
}

void RestorePortableBulletTypeSpritesState(BulletTypeSprites &dst, const DGS::PortableBulletTypeSpritesState &src)
{
    RestorePortableAnmVmState(dst.spriteBullet, src.spriteBullet);
    RestorePortableAnmVmState(dst.spriteSpawnEffectFast, src.spriteSpawnEffectFast);
    RestorePortableAnmVmState(dst.spriteSpawnEffectNormal, src.spriteSpawnEffectNormal);
    RestorePortableAnmVmState(dst.spriteSpawnEffectSlow, src.spriteSpawnEffectSlow);
    RestorePortableAnmVmState(dst.spriteSpawnEffectDonut, src.spriteSpawnEffectDonut);
    dst.grazeSize = src.grazeSize;
    dst.unk_55c = src.unk_55c;
    dst.bulletHeight = src.bulletHeight;
}

void RestorePortableBulletState(Bullet &bullet, const DGS::PortableBulletState &state)
{
    RestorePortableBulletTypeSpritesState(bullet.sprites, state.sprites);
    bullet.pos = state.pos;
    bullet.velocity = state.velocity;
    bullet.ex4Acceleration = state.ex4Acceleration;
    bullet.speed = state.speed;
    bullet.ex5Float0 = state.ex5Float0;
    bullet.dirChangeSpeed = state.dirChangeSpeed;
    bullet.angle = state.angle;
    bullet.ex5Float1 = state.ex5Float1;
    bullet.dirChangeRotation = state.dirChangeRotation;
    bullet.timer = state.timer;
    bullet.ex5Int0 = state.ex5Int0;
    bullet.dirChangeInterval = state.dirChangeInterval;
    bullet.dirChangeNumTimes = state.dirChangeNumTimes;
    bullet.dirChangeMaxTimes = state.dirChangeMaxTimes;
    bullet.exFlags = state.exFlags;
    bullet.spriteOffset = state.spriteOffset;
    bullet.unk_5bc = state.unk_5bc;
    bullet.state = state.state;
    bullet.unk_5c0 = state.unk_5c0;
    bullet.unk_5c2 = state.unk_5c2;
    bullet.isGrazed = state.isGrazed;
    bullet.provokedPlayer = state.provokedPlayer;
}

void RestorePortableLaserState(Laser &laser, const DGS::PortableLaserState &state)
{
    RestorePortableAnmVmState(laser.vm0, state.vm0);
    RestorePortableAnmVmState(laser.vm1, state.vm1);
    laser.pos = state.pos;
    laser.angle = state.angle;
    laser.startOffset = state.startOffset;
    laser.endOffset = state.endOffset;
    laser.startLength = state.startLength;
    laser.width = state.width;
    laser.speed = state.speed;
    laser.startTime = state.startTime;
    laser.hitboxStartTime = state.hitboxStartTime;
    laser.duration = state.duration;
    laser.despawnDuration = state.despawnDuration;
    laser.hitboxEndDelay = state.hitboxEndDelay;
    laser.inUse = state.inUse;
    laser.timer = state.timer;
    laser.flags = state.flags;
    laser.color = state.color;
    laser.state = state.state;
    laser.provokedPlayer = state.provokedPlayer;
}

void RestorePortableBulletManagerState(const DGS::PortableBulletManagerState &state)
{
    for (size_t i = 0; i < state.bulletTypeTemplates.size(); ++i)
    {
        RestorePortableBulletTypeSpritesState(g_BulletManager.bulletTypeTemplates[i], state.bulletTypeTemplates[i]);
    }
    for (size_t i = 0; i < state.bullets.size(); ++i)
    {
        RestorePortableBulletState(g_BulletManager.bullets[i], state.bullets[i]);
    }
    for (size_t i = 0; i < state.lasers.size(); ++i)
    {
        RestorePortableLaserState(g_BulletManager.lasers[i], state.lasers[i]);
    }
    g_BulletManager.nextBulletIndex = state.nextBulletIndex;
    g_BulletManager.bulletCount = state.bulletCount;
    g_BulletManager.time = state.time;
}

void RestorePortableItemState(Item &item, const DGS::PortableItemState &state)
{
    RestorePortableAnmVmState(item.sprite, state.sprite);
    item.currentPosition = state.currentPosition;
    item.startPosition = state.startPosition;
    item.targetPosition = state.targetPosition;
    item.timer = state.timer;
    item.itemType = state.itemType;
    item.isInUse = state.isInUse;
    item.unk_142 = state.unk_142;
    item.state = state.state;
}

void RestorePortableItemManagerState(const DGS::PortableItemManagerState &state)
{
    for (size_t i = 0; i < state.items.size(); ++i)
    {
        RestorePortableItemState(g_ItemManager.items[i], state.items[i]);
    }
    g_ItemManager.nextIndex = state.nextIndex;
    g_ItemManager.itemCount = state.itemCount;
}

void RestorePortableEffectState(Effect &effect, const DGS::PortableEffectState &state)
{
    RestorePortableAnmVmState(effect.vm, state.vm);
    effect.pos1 = state.pos1;
    effect.unk_11c = state.unk_11c;
    effect.unk_128 = state.unk_128;
    effect.position = state.position;
    effect.pos2 = state.pos2;
    effect.quaternion = state.quaternion;
    effect.unk_15c = state.unk_15c;
    effect.angleRelated = state.angleRelated;
    effect.timer = state.timer;
    effect.unk_170 = state.unk_170;
    effect.updateCallback = state.hasUnknownUpdateToken ? nullptr : ResolveEffectUpdateToken(state.updateCallbackToken);
    effect.inUseFlag = state.inUseFlag;
    effect.effectId = state.effectId;
    effect.unk_17a = state.unk_17a;
    effect.unk_17b = state.unk_17b;
}

void RestorePortableEffectManagerState(const DGS::PortableEffectManagerState &state)
{
    g_EffectManager.nextIndex = state.nextIndex;
    g_EffectManager.activeEffects = state.activeEffects;
    for (size_t i = 0; i < state.effects.size(); ++i)
    {
        RestorePortableEffectState(g_EffectManager.effects[i], state.effects[i]);
    }
}

void RestorePortableStageCoreState(const DGS::PortableStageCoreState &state)
{
    g_Stage.stage = state.stage;
    g_Stage.objectsCount = state.objectsCount;
    g_Stage.quadCount = state.quadCount;
    g_Stage.scriptTime = state.scriptTime;
    g_Stage.instructionIndex = state.instructionIndex;
    g_Stage.timer = state.timer;
    g_Stage.position = state.position;
    g_Stage.skyFog = state.skyFog;
    g_Stage.skyFogInterpInitial = state.skyFogInterpInitial;
    g_Stage.skyFogInterpFinal = state.skyFogInterpFinal;
    g_Stage.skyFogInterpDuration = state.skyFogInterpDuration;
    g_Stage.skyFogInterpTimer = state.skyFogInterpTimer;
    g_Stage.skyFogNeedsSetup = state.skyFogNeedsSetup;
    g_Stage.spellcardState = state.spellcardState;
    g_Stage.ticksSinceSpellcardStarted = state.ticksSinceSpellcardStarted;
    g_Stage.unpauseFlag = state.unpauseFlag;
    g_Stage.facingDirInterpInitial = state.facingDirInterpInitial;
    g_Stage.facingDirInterpFinal = state.facingDirInterpFinal;
    g_Stage.facingDirInterpDuration = state.facingDirInterpDuration;
    g_Stage.facingDirInterpTimer = state.facingDirInterpTimer;
    g_Stage.positionInterpFinal = state.positionInterpFinal;
    g_Stage.positionInterpEndTime = state.positionInterpEndTime;
    g_Stage.positionInterpInitial = state.positionInterpInitial;
    g_Stage.positionInterpStartTime = state.positionInterpStartTime;
    g_GameManager.stageCameraFacingDir = state.currentCameraFacingDir;

    if (g_Stage.objects != nullptr)
    {
        const size_t objectCount = std::min((size_t)g_Stage.objectsCount, state.objectFlags.size());
        for (size_t i = 0; i < objectCount; ++i)
        {
            if (g_Stage.objects[i] != nullptr)
            {
                g_Stage.objects[i]->flags = state.objectFlags[i];
            }
        }
    }

    if (g_Stage.quadVms != nullptr)
    {
        const size_t quadCount = std::min((size_t)g_Stage.quadCount, state.quadVms.size());
        for (size_t i = 0; i < quadCount; ++i)
        {
            RestorePortableAnmVmState(g_Stage.quadVms[i], state.quadVms[i]);
        }
    }

    RestorePortableAnmVmState(g_Stage.spellcardBackground, state.spellcardBackground);
    RestorePortableAnmVmState(g_Stage.unk2, state.extraBackground);
}

bool RestorePortableGuiState(const DGS::PortableGuiState &state)
{
    g_Gui.flags.flag0 = state.flag0;
    g_Gui.flags.flag1 = state.flag1;
    g_Gui.flags.flag2 = state.flag2;
    g_Gui.flags.flag3 = state.flag3;
    g_Gui.flags.flag4 = state.flag4;
    g_Gui.bossPresent = state.bossPresent;
    g_Gui.bossUIOpacity = state.bossUIOpacity;
    g_Gui.eclSetLives = state.eclSetLives;
    g_Gui.spellcardSecondsRemaining = state.spellcardSecondsRemaining;
    g_Gui.lastSpellcardSecondsRemaining = state.lastSpellcardSecondsRemaining;
    g_Gui.bossHealthBar1 = state.bossHealthBar1;
    g_Gui.bossHealthBar2 = state.bossHealthBar2;
    g_Gui.bombSpellcardBarLength = state.bombSpellcardBarLength;
    g_Gui.blueSpellcardBarLength = state.blueSpellcardBarLength;

    if (!state.hasGuiImpl || g_Gui.impl == nullptr)
    {
        return false;
    }

    g_Gui.impl->bossHealthBarState = state.bossHealthBarState;
    RestorePortableAnmVmState(g_Gui.impl->playerSpellcardPortrait, state.playerSpellcardPortrait);
    RestorePortableAnmVmState(g_Gui.impl->enemySpellcardPortrait, state.enemySpellcardPortrait);
    RestorePortableAnmVmState(g_Gui.impl->bombSpellcardName, state.bombSpellcardName);
    RestorePortableAnmVmState(g_Gui.impl->enemySpellcardName, state.enemySpellcardName);
    RestorePortableAnmVmState(g_Gui.impl->bombSpellcardBackground, state.bombSpellcardBackground);
    RestorePortableAnmVmState(g_Gui.impl->enemySpellcardBackground, state.enemySpellcardBackground);
    RestorePortableAnmVmState(g_Gui.impl->loadingScreenSprite, state.loadingScreenSprite);
    RedrawPortableGuiSpellcardTexts(state);
    return true;
}

void RestorePortableGameplayCoreState(const DGS::PortableGameplayCoreState &core)
{
    g_GameManager.guiScore = core.guiScore;
    g_GameManager.score = core.score;
    g_GameManager.nextScoreIncrement = core.nextScoreIncrement;
    g_GameManager.highScore = core.highScore;
    g_GameManager.difficulty = (Difficulty)core.difficulty;
    g_GameManager.grazeInStage = core.grazeInStage;
    g_GameManager.grazeInTotal = core.grazeInTotal;
    g_GameManager.deaths = core.deaths;
    g_GameManager.bombsUsed = core.bombsUsed;
    g_GameManager.spellcardsCaptured = core.spellcardsCaptured;
    g_GameManager.pointItemsCollectedInStage = (u16)core.pointItemsCollectedInStage;
    g_GameManager.pointItemsCollected = (u16)core.pointItemsCollected;
    g_GameManager.powerItemCountForScore = (i8)core.powerItemCountForScore;
    g_GameManager.extraLives = (i8)core.extraLives;
    g_GameManager.currentStage = core.currentStage;
    g_GameManager.rank = core.rank;
    g_GameManager.maxRank = core.maxRank;
    g_GameManager.minRank = core.minRank;
    g_GameManager.subRank = core.subRank;
    g_GameManager.randomSeed = (u16)core.randomSeed;
    g_GameManager.gameFrames = core.gameFrames;
    g_GameManager.currentPower = core.currentPower1;
    g_GameManager.currentPower2 = core.currentPower2;
    g_GameManager.livesRemaining = (i8)core.livesRemaining1;
    g_GameManager.bombsRemaining = (i8)core.bombsRemaining1;
    g_GameManager.livesRemaining2 = (i8)core.livesRemaining2;
    g_GameManager.bombsRemaining2 = (i8)core.bombsRemaining2;
    g_GameManager.numRetries = (u8)core.numRetries;
    g_GameManager.isTimeStopped = (i8)core.isTimeStopped;
    g_GameManager.isGameCompleted = (u8)core.isGameCompleted;
    g_GameManager.isInPracticeMode = (u8)core.isPracticeMode;
    g_GameManager.demoMode = (u8)core.demoMode;
    g_GameManager.character = core.character1;
    g_GameManager.shotType = core.shotType1;
    g_GameManager.character2 = core.character2;
    g_GameManager.shotType2 = core.shotType2;
    g_GameManager.isInReplay = 0;
    g_GameManager.isInGameMenu = 0;
    g_GameManager.isInRetryMenu = 0;
    g_GameManager.isInMenu = 0;
}

bool RestorePortableRuntimeState(const DGS::PortableGameplayRuntimeState &runtime)
{
    DGS::DgsStageRuntimeState stageRuntimeState;
    stageRuntimeState.objectInstances = runtime.stageObjectInstances;
    return DGS::RestoreDeterministicRuntimeState(runtime.enemyEclRuntimeState, runtime.screenEffectRuntimeState,
                                                 runtime.controllerRuntimeState, runtime.supervisorRuntimeState,
                                                 runtime.gameWindowRuntimeState, stageRuntimeState,
                                                 runtime.soundRuntimeState, runtime.inputRuntimeState);
}

void RestorePortableRngState(const DGS::PortableGameplayState &state)
{
    if (DGS::HasPortableCaptureFlag(state.captureFlags, DGS::PortableCaptureFlag_HasExplicitRng))
    {
        g_Rng.seed = state.rng.seed;
        g_Rng.generationCount = state.rng.generationCount;
    }
    else
    {
        g_Rng.Initialize((u16)state.core.randomSeed);
    }
}

void RestorePortableCatkState(const DGS::PortableGameplayState &state)
{
    if (!DGS::HasPortableCaptureFlag(state.captureFlags, DGS::PortableCaptureFlag_HasExplicitCatk))
    {
        return;
    }

    std::memcpy(g_GameManager.catk, state.catk.data(), sizeof(g_GameManager.catk));
}

bool RestorePortableGameplayStateLive(const DGS::PortableGameplayState &state)
{
    RestorePortableRngState(state);
    RestorePortableGameplayCoreState(state.core);
    RestorePortableCatkState(state);
    RestorePortableStageCoreState(state.stageCore);
    RestorePortableEnemyManagerState(state.enemyActors);
    RestorePortablePlayerState(g_Player, state.players[0]);
    RestorePortablePlayerState(g_Player2, state.players[1]);
    RestorePortableBulletManagerState(state.bulletActors);
    RestorePortableItemManagerState(state.itemActors);
    RestorePortableEffectManagerState(state.effectActors);

    // Clear live device/input edge state before restoring the captured runtime.
    // Doing this afterwards clobbers the snapshot's deterministic input state
    // and makes authority post-verify fail on inputRuntime every time.
    Controller::ResetDeviceInputState();
    Session::ResetLegacyInputState();

    if (!RestorePortableRuntimeState(state.runtime))
    {
        return false;
    }

    DGS::DgsEclManagerRefs eclRefs;
    eclRefs.hasEclFile = state.eclScripts.hasEclFile;
    eclRefs.timelineOffset.value = state.eclScripts.timelineOffset;
    eclRefs.subTableOffset.value = offsetof(EclRawHeader, subOffsets);
    DGS::RestoreEclManagerRefs(g_EclManager, eclRefs);
    DGS::RestoreStageRefs(g_Stage, state.stageRefs);
    DGS::RestoreEnemyManagerRefs(g_EnemyManager, state.enemyRefs);

    Netplay::g_State.currentNetFrame = state.runtime.frame;
    Netplay::g_State.delay = state.runtime.delay;
    Netplay::g_State.currentDelayCooldown = state.runtime.currentDelayCooldown;
    Netplay::g_State.currentCtrl = Netplay::IGC_NONE;

    GameManager::SetupCamera(0.0f);
    GameManager::SetupCameraStageBackground(0.0f);
    g_Renderer->SetFog(1, g_Stage.skyFog.color, g_Stage.skyFog.nearPlane, g_Stage.skyFog.farPlane);

    ResetPostRestorePresentationState();
    if (!RestorePortableGuiState(state.gui))
    {
        return false;
    }
    return true;
}

bool ApplyPortableShellSync(const DGS::PortableGameplayState &state, std::string &detail)
{
    const int shellFrame = state.enemyActors.timelineTime.current;
    THPrac::TH06::THPortableFastForwardStageShell(shellFrame);
    if (!THPrac::TH06::THPortableReloadBossSectionAssets((int)state.shellSync.bossAssetProfile))
    {
        detail = "boss asset reload failed";
        return false;
    }
    if (!THPrac::TH06::THPortableSyncStageBgm(state.shellSync.bgmTrackIndex))
    {
        detail = "BGM sync failed";
        return false;
    }
    if (!THPrac::TH06::THPortableSyncStageIntroSprites(state.shellSync.hideStageNameIntro,
                                                       state.shellSync.hideSongNameIntro))
    {
        detail = "stage intro sync failed";
        return false;
    }

    // thprac-style shell fast-forward establishes the correct shell-side resources,
    // but it also pushes camera/fog/interp state to the shell's final state.
    // Re-apply the captured Stage/GUI core so the live world ends up at the
    // snapshot's exact frame rather than the fast-forward helper's terminal state.
    RestorePortableStageCoreState(state.stageCore);
    if (!RestorePortableGuiState(state.gui))
    {
        detail = "gui restore failed after shell sync";
        return false;
    }

    g_GameManager.counat = shellFrame;
    GameManager::SetupCamera(0.0f);
    GameManager::SetupCameraStageBackground(0.0f);
    g_Renderer->SetFog(1, g_Stage.skyFog.color, g_Stage.skyFog.nearPlane, g_Stage.skyFog.farPlane);

    detail = "ok";
    return true;
}

bool VerifyPortableShellSync(const DGS::PortableGameplayState &state, std::string &detail)
{
    if (g_GameManager.counat != state.enemyActors.timelineTime.current)
    {
        detail = "timeline frame mismatch";
        return false;
    }
    if (!NearlyEqualVec3(g_GameManager.stageCameraFacingDir, state.stageCore.currentCameraFacingDir))
    {
        detail = "camera facing mismatch";
        return false;
    }
    if (!NearlyEqualFloat(g_Stage.skyFog.nearPlane, state.stageCore.skyFog.nearPlane) ||
        !NearlyEqualFloat(g_Stage.skyFog.farPlane, state.stageCore.skyFog.farPlane) ||
        g_Stage.skyFog.color != state.stageCore.skyFog.color)
    {
        detail = "fog mismatch";
        return false;
    }
    if (g_Gui.impl == nullptr)
    {
        detail = "gui shell unavailable";
        return false;
    }
    if (IsIntroSpriteHidden(g_Gui.impl->stageNameSprite) != state.shellSync.hideStageNameIntro)
    {
        detail = "stage intro visibility mismatch";
        return false;
    }
    if (IsIntroSpriteHidden(g_Gui.impl->songNameSprite) != state.shellSync.hideSongNameIntro)
    {
        detail = "song intro visibility mismatch";
        return false;
    }
    if (THPrac::TH06::THPortableGetCurrentBgmTrackIndex() != state.shellSync.bgmTrackIndex)
    {
        detail = "bgm track mismatch";
        return false;
    }
    if (THPrac::TH06::THPortableGetCurrentBossAssetProfile() != (int)state.shellSync.bossAssetProfile)
    {
        detail = "boss asset profile mismatch";
        return false;
    }

    detail = "ok";
    return true;
}

bool CapturePostApplyMatches(const DGS::PortableGameplayState &expectedState,
                             const DGS::PortableGameplayBuildResult &expectedBuild, std::string &detail)
{
    auto captured = std::make_unique<DGS::PortableGameplayState>();
    DGS::CapturePortableGameplayState(*captured);
    auto actualBuild = std::make_unique<DGS::PortableGameplayBuildResult>();
    DGS::BuildPortableGameplayWorldFromState(*captured, *actualBuild);

    if (!actualBuild->success)
    {
        detail = "post-verify shadow build failed";
        char line[256];
        std::snprintf(line, sizeof(line), "readiness=%s reason=%s",
                      DGS::PortableRestoreReadinessToString(actualBuild->evaluation.readiness),
                      actualBuild->evaluation.blockingReasons.empty() ? ""
                                                                      : actualBuild->evaluation.blockingReasons.front().c_str());
        LogPortablePostVerifyDiffLine("build", line);
        return false;
    }

    bool matches = true;
    bool hasDiagnosticDrift = false;
    std::string firstReason;
    const bool isAuthorityRecovery = g_PortableRestoreState.source == Source::AuthoritativeNetplayRecovery;
    std::string diagnosticReason;

    const u64 expectedPortableFingerprint = FingerprintPortableExplicitState(expectedState);
    const u64 actualPortableFingerprint = FingerprintPortableExplicitState(*captured);

    if (expectedPortableFingerprint != actualPortableFingerprint)
    {
        char detailLine[256];
        std::snprintf(detailLine, sizeof(detailLine), "expected=0x%016llX actual=0x%016llX",
                      (unsigned long long)expectedPortableFingerprint, (unsigned long long)actualPortableFingerprint);
        LogPortablePostVerifyDiffLine("portableFingerprint", detailLine);
        hasDiagnosticDrift = true;
    }
    if (expectedBuild.shadowFingerprint != actualBuild->shadowFingerprint)
    {
        char detailLine[256];
        std::snprintf(detailLine, sizeof(detailLine), "expected=0x%016llX actual=0x%016llX",
                      (unsigned long long)expectedBuild.shadowFingerprint,
                      (unsigned long long)actualBuild->shadowFingerprint);
        LogPortablePostVerifyDiffLine("shadowFingerprint", detailLine);
        hasDiagnosticDrift = true;
    }

    matches &= ComparePostVerifyU32(firstReason, "catalogHash", expectedState.catalog.catalogHash,
                                    captured->catalog.catalogHash, "catalog hash mismatch");
    matches &= ComparePostVerifyU32(firstReason, "catalogStage", expectedState.catalog.currentStage,
                                    captured->catalog.currentStage, "catalog stage mismatch");
    matches &= ComparePostVerifyU32(firstReason, "catalogDifficulty", expectedState.catalog.difficulty,
                                    captured->catalog.difficulty, "catalog difficulty mismatch");
    matches &= ComparePostVerifyU32(firstReason, "catalogCharacter1", expectedState.catalog.character1,
                                    captured->catalog.character1, "catalog player mismatch");
    matches &= ComparePostVerifyU32(firstReason, "catalogShotType1", expectedState.catalog.shotType1,
                                    captured->catalog.shotType1, "catalog shot mismatch");
    matches &= ComparePostVerifyU32(firstReason, "catalogPractice", expectedState.catalog.isPracticeMode,
                                    captured->catalog.isPracticeMode, "catalog practice mismatch");
    matches &= ComparePostVerifyU32(firstReason, "catalogReplay", expectedState.catalog.isReplay,
                                    captured->catalog.isReplay, "catalog replay mismatch");

    matches &= ComparePostVerifyU32(firstReason, "builtEnemyCount", expectedBuild.builtEnemyCount,
                                    actualBuild->builtEnemyCount, "enemy count mismatch");
    matches &= ComparePostVerifyU32(firstReason, "builtPlayerCount", expectedBuild.builtPlayerCount,
                                    actualBuild->builtPlayerCount, "player count mismatch");
    matches &= ComparePostVerifyU32(firstReason, "builtPlayerBulletCount", expectedBuild.builtPlayerBulletCount,
                                    actualBuild->builtPlayerBulletCount, "player bullet count mismatch");
    matches &= ComparePostVerifyU32(firstReason, "builtBulletCount", expectedBuild.builtBulletCount,
                                    actualBuild->builtBulletCount, "bullet count mismatch");
    matches &= ComparePostVerifyU32(firstReason, "builtLaserCount", expectedBuild.builtLaserCount,
                                    actualBuild->builtLaserCount, "laser count mismatch");
    matches &= ComparePostVerifyU32(firstReason, "builtItemCount", expectedBuild.builtItemCount,
                                    actualBuild->builtItemCount, "item count mismatch");
    matches &= ComparePostVerifyU32(firstReason, "builtEffectCount", expectedBuild.builtEffectCount,
                                    actualBuild->builtEffectCount, "effect count mismatch");
    matches &= ComparePostVerifyU32(firstReason, "builtEclSubCount", expectedBuild.builtEclSubCount,
                                    actualBuild->builtEclSubCount, "ecl sub count mismatch");
    matches &= ComparePostVerifyI32(firstReason, "builtEclTimelineSlot", expectedBuild.builtEclTimelineSlot,
                                    actualBuild->builtEclTimelineSlot, "ecl timeline slot mismatch");
    matches &= ComparePostVerifyU32(firstReason, "builtStageObjectCount", expectedBuild.builtStageObjectCount,
                                    actualBuild->builtStageObjectCount, "stage object count mismatch");
    matches &= ComparePostVerifyU32(firstReason, "builtStageQuadCount", expectedBuild.builtStageQuadCount,
                                    actualBuild->builtStageQuadCount, "stage quad count mismatch");

    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason, "dgs.gameManager",
                         expectedState.shadowFingerprints.dgs.gameManager,
                         captured->shadowFingerprints.dgs.gameManager, "game manager drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason, "dgs.player1",
                         expectedState.shadowFingerprints.dgs.player1,
                         captured->shadowFingerprints.dgs.player1, "player1 drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason, "dgs.player2",
                         expectedState.shadowFingerprints.dgs.player2,
                         captured->shadowFingerprints.dgs.player2, "player2 drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason,
                         "dgs.bulletManager", expectedState.shadowFingerprints.dgs.bulletManager,
                         captured->shadowFingerprints.dgs.bulletManager, "bullet manager drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason,
                         "dgs.enemyManager", expectedState.shadowFingerprints.dgs.enemyManager,
                         captured->shadowFingerprints.dgs.enemyManager, "enemy manager drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason,
                         "dgs.itemManager", expectedState.shadowFingerprints.dgs.itemManager,
                         captured->shadowFingerprints.dgs.itemManager, "item manager drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason,
                         "dgs.effectManager", expectedState.shadowFingerprints.dgs.effectManager,
                         captured->shadowFingerprints.dgs.effectManager, "effect manager drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason, "dgs.stageState",
                         expectedState.shadowFingerprints.dgs.stageState,
                         captured->shadowFingerprints.dgs.stageState, "stage state drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason, "dgs.eclManager",
                         expectedState.shadowFingerprints.dgs.eclManager,
                         captured->shadowFingerprints.dgs.eclManager, "ecl manager drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason, "dgs.rng",
                         expectedState.shadowFingerprints.dgs.rng,
                         captured->shadowFingerprints.dgs.rng, "rng drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason, "dgs.runtime",
                         expectedState.shadowFingerprints.dgs.runtime,
                         captured->shadowFingerprints.dgs.runtime, "runtime drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason, "dgs.refs",
                         expectedState.shadowFingerprints.dgs.refs,
                         captured->shadowFingerprints.dgs.refs, "ref graph drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason,
                         "dgs.unresolved", expectedState.shadowFingerprints.dgs.unresolved,
                         captured->shadowFingerprints.dgs.unresolved, "unresolved audit drift");
    hasDiagnosticDrift |= !ComparePostVerifyU64(isAuthorityRecovery ? diagnosticReason : firstReason, "dgs.combined",
                         expectedState.shadowFingerprints.dgs.combined,
                         captured->shadowFingerprints.dgs.combined, "combined DGS drift");

    if (isAuthorityRecovery)
    {
        DGS::AuthorityGameplayFingerprint expectedAuthority {};
        DGS::AuthorityGameplayFingerprint actualAuthority {};
        DGS::CapturePortableAuthorityGameplayFingerprint(expectedState, expectedAuthority);
        DGS::CapturePortableAuthorityGameplayFingerprint(*captured, actualAuthority);

        const char *firstAuthorityDiff =
            DGS::FirstDifferentAuthorityGameplaySubsystem(expectedAuthority, actualAuthority);
        if (std::strcmp(firstAuthorityDiff, "none") != 0)
        {
            char detailLine[512];
            std::snprintf(detailLine, sizeof(detailLine),
                          "firstDiff=%s expectedAll=0x%016llX actualAll=0x%016llX game=%016llX/%016llX bullet=%016llX/%016llX input=%016llX/%016llX catk=%016llX/%016llX rng=%016llX/%016llX",
                          firstAuthorityDiff, (unsigned long long)expectedAuthority.allHash,
                          (unsigned long long)actualAuthority.allHash, (unsigned long long)expectedAuthority.gameHash,
                          (unsigned long long)actualAuthority.gameHash, (unsigned long long)expectedAuthority.bulletHash,
                          (unsigned long long)actualAuthority.bulletHash, (unsigned long long)expectedAuthority.inputHash,
                          (unsigned long long)actualAuthority.inputHash, (unsigned long long)expectedAuthority.catkHash,
                          (unsigned long long)actualAuthority.catkHash, (unsigned long long)expectedAuthority.rngHash,
                          (unsigned long long)actualAuthority.rngHash);
            LogPortableRestoreEvent("authority-post-verify-first-diff", detailLine);
            Netplay::TraceDiagnostic("authority-post-verify-first-diff", "serial=%u %s",
                                     Netplay::g_State.recovery.recoverySerial, detailLine);
            Netplay::TraceDiagnostic("authority-post-verify-failed",
                                     "serial=%u expectedAll=%016llx actualAll=%016llx firstDiff=%s",
                                     Netplay::g_State.recovery.recoverySerial,
                                     (unsigned long long)expectedAuthority.allHash,
                                     (unsigned long long)actualAuthority.allHash, firstAuthorityDiff);
            detail = firstAuthorityDiff;
            return false;
        }

        Netplay::TraceDiagnostic("authority-post-verify-ok", "serial=%u all=%016llx",
                                 Netplay::g_State.recovery.recoverySerial,
                                 (unsigned long long)actualAuthority.allHash);
    }

    if (!matches)
    {
        char summary[256];
        std::snprintf(summary, sizeof(summary), "portable=0x%016llX shadow=0x%016llX reason=%s",
                      (unsigned long long)actualPortableFingerprint, (unsigned long long)actualBuild->shadowFingerprint,
                      firstReason.empty() ? "captured state drifted after apply" : firstReason.c_str());
        LogPortablePostVerifyDiffLine("summary", summary);
    }
    else if (hasDiagnosticDrift)
    {
        char summary[256];
        std::snprintf(summary, sizeof(summary), "portable=0x%016llX shadow=0x%016llX reason=diagnostic drift only",
                      (unsigned long long)actualPortableFingerprint, (unsigned long long)actualBuild->shadowFingerprint);
        LogPortablePostVerifyDiffLine("summary", summary);
    }

    detail = firstReason.empty() ? (hasDiagnosticDrift ? "diagnostic drift only" : "ok") : firstReason;
    return matches;
}

bool ReadDiskBytes(const char *path, std::vector<u8> &bytes)
{
    bytes.clear();

    char resolvedPath[512];
    if (path == nullptr || path[0] == '\0')
    {
        GamePaths::Resolve(resolvedPath, sizeof(resolvedPath), "./Save/portable_state.bin");
        path = resolvedPath;
    }

    FILE *file = nullptr;
    if (fopen_s(&file, path, "rb") != 0 || file == nullptr)
    {
        return false;
    }

    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return false;
    }
    const long fileSize = std::ftell(file);
    if (fileSize <= 0 || std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fclose(file);
        return false;
    }

    std::vector<u8> diskBytes((size_t)fileSize);
    const size_t read = std::fread(diskBytes.data(), 1, diskBytes.size(), file);
    std::fclose(file);
    if (read != diskBytes.size())
    {
        diskBytes.clear();
        return false;
    }

    std::string diskError;
    if (!PortableSnapshotStorage::DecodePortableSnapshotFromDisk(diskBytes, bytes, nullptr, &diskError))
    {
        char detail[256];
        std::snprintf(detail, sizeof(detail), "disk-decode-failed reason=%s", diskError.c_str());
        LogPortableRestoreEvent("disk", detail);
        bytes.clear();
        return false;
    }
    return true;
}

void ProcessPendingDecode()
{
    if (g_Supervisor.curState == SUPERVISOR_STATE_MAINMENU && !IsMainMenuShellStable())
    {
        return;
    }

    if (g_PortableRestoreState.fromDisk)
    {
        if (!ReadDiskBytes(g_PortableRestoreState.queuedPath.empty() ? nullptr : g_PortableRestoreState.queuedPath.c_str(),
                           g_PortableRestoreState.queuedBytes))
        {
            FailNow("Portable Restore FAIL", "disk read/decode failed");
            return;
        }
    }

    auto decoded = std::make_unique<DGS::PortableGameplayState>();
    if (!DGS::DecodePortableGameplayState(g_PortableRestoreState.queuedBytes, *decoded))
    {
        FailNow("Portable Restore FAIL", "decode failed");
        return;
    }
    if (g_PortableRestoreState.source == Source::AuthoritativeNetplayRecovery &&
        (!DGS::HasPortableCaptureFlag(decoded->captureFlags, DGS::PortableCaptureFlag_HasExplicitRng) ||
         !DGS::HasPortableCaptureFlag(decoded->captureFlags, DGS::PortableCaptureFlag_HasExplicitCatk)))
    {
        FailNow("Portable Restore FAIL", "authority snapshot missing deterministic rng/catk state");
        return;
    }
    if (decoded->catalog.hasSecondPlayer != 0 && !IsAuthoritativeNetplayTopologyCompatible(decoded->catalog))
    {
        FailNow("Portable Restore FAIL",
                g_PortableRestoreState.source == Source::AuthoritativeNetplayRecovery
                    ? "netplay authority snapshot player topology mismatch"
                    : "Portable 2P restore not supported yet.");
        return;
    }
    if (decoded->catalog.isReplay != 0)
    {
        FailNow("Portable Restore FAIL", "Replay portable restore is not supported.");
        return;
    }
    if (!IsSupportedSupervisorState(g_Supervisor.curState) || IsUnsupportedGameplayMode())
    {
        FailNow("Portable Restore FAIL", "current mode does not support portable restore");
        return;
    }

    g_PortableRestoreState.decodedState = std::move(decoded);
    LogPortableRestoreEvent("decoded", "ok");

    if (CurrentCatalogMatches(g_PortableRestoreState.decodedState->catalog) && IsGameplayShellReady())
    {
        SetPhase(Phase::Applying);
        return;
    }

    SetPhase(Phase::PendingBootstrap);
}

void ProcessPendingBootstrap()
{
    if (!IsSupportedSupervisorState(g_Supervisor.curState) || IsUnsupportedGameplayMode())
    {
        FailNow("Portable Restore FAIL", "bootstrap unavailable in current mode");
        return;
    }

    if (g_Supervisor.curState == SUPERVISOR_STATE_MAINMENU && !IsMainMenuShellStable())
    {
        if (ShouldEmitProgressLog())
        {
            char detail[256];
            std::snprintf(detail, sizeof(detail), "mainmenu-not-stable gameState=%d cur=%d wanted=%d wanted2=%d",
                          g_MainMenu.gameState, g_Supervisor.curState, g_Supervisor.wantedState,
                          g_Supervisor.wantedState2);
            LogPortableRestoreEvent("bootstrap-wait", detail);
        }
        if (HasPhaseTimedOut())
        {
            FailNow("Portable Restore FAIL", "mainmenu bootstrap timed out");
        }
        return;
    }

    if (g_Supervisor.curState == SUPERVISOR_STATE_MAINMENU)
    {
        g_PortableRestoreState.pendingMainMenuBootstrap = true;
    }
    else
    {
        ApplyBootstrapBaseline(g_PortableRestoreState.decodedState->catalog);
        g_Supervisor.wantedState = SUPERVISOR_STATE_GAMEMANAGER;
        g_Supervisor.wantedState2 = SUPERVISOR_STATE_GAMEMANAGER;
        g_Supervisor.curState = SUPERVISOR_STATE_GAMEMANAGER_REINIT;
    }
    LogPortableRestoreEvent("bootstrap", g_Supervisor.curState == SUPERVISOR_STATE_MAINMENU ? "queued-mainmenu"
                                                                                           : "queued-gameplay");
    SetPhase(Phase::WaitingForGameplayShell);
}

void ProcessWaitingForGameplayShell()
{
    if (!IsSupportedSupervisorState(g_Supervisor.curState) || IsUnsupportedGameplayMode())
    {
        FailNow("Portable Restore FAIL", "bootstrap lost supported gameplay shell");
        return;
    }

    if (g_Supervisor.curState == SUPERVISOR_STATE_MAINMENU)
    {
        if (ShouldEmitProgressLog())
        {
            char detail[256];
            std::snprintf(detail, sizeof(detail),
                          "waiting-mainmenu pending=%d stable=%d gameState=%d cur=%d wanted=%d wanted2=%d",
                          g_PortableRestoreState.pendingMainMenuBootstrap ? 1 : 0, IsMainMenuShellStable() ? 1 : 0,
                          g_MainMenu.gameState, g_Supervisor.curState, g_Supervisor.wantedState,
                          g_Supervisor.wantedState2);
            LogPortableRestoreEvent("waiting", detail);
        }
        if (HasPhaseTimedOut())
        {
            FailNow("Portable Restore FAIL", "waiting for mainmenu bootstrap timed out");
        }
        return;
    }

    const bool catalogMatches = CurrentCatalogMatches(g_PortableRestoreState.decodedState->catalog);
    const bool shellReady = IsGameplayShellReady();
    if (!catalogMatches || !shellReady)
    {
        if (ShouldEmitProgressLog())
        {
            char detail[384];
            std::snprintf(detail, sizeof(detail),
                          "waiting-shell cur=%d wanted=%d wanted2=%d stage=%d expectedStage=%u diff=%d expectedDiff=%u "
                          "practice=%d expectedPractice=%u replay=%d expectedReplay=%u ready=%d std=%d ecl=%d objs=%d "
                          "inst=%d quads=%d",
                          g_Supervisor.curState, g_Supervisor.wantedState, g_Supervisor.wantedState2,
                          g_GameManager.currentStage, g_PortableRestoreState.decodedState->catalog.currentStage,
                          g_GameManager.difficulty, g_PortableRestoreState.decodedState->catalog.difficulty,
                          g_GameManager.isInPracticeMode, g_PortableRestoreState.decodedState->catalog.isPracticeMode,
                          g_GameManager.isInReplay, g_PortableRestoreState.decodedState->catalog.isReplay,
                          shellReady ? 1 : 0, g_Stage.stdData != nullptr ? 1 : 0, g_EclManager.eclFile != nullptr ? 1 : 0,
                          g_Stage.objects != nullptr ? 1 : 0, g_Stage.objectInstances != nullptr ? 1 : 0,
                          g_Stage.quadVms != nullptr ? 1 : 0);
            LogPortableRestoreEvent("waiting", detail);
        }
        if (HasPhaseTimedOut())
        {
            FailNow("Portable Restore FAIL", catalogMatches ? "waiting for gameplay shell timed out"
                                                            : "waiting for catalog match timed out");
        }
        return;
    }

    LogPortableRestoreEvent("waiting", "gameplay-shell-ready");
    SetPhase(Phase::Applying);
}

void ProcessApplying()
{
    auto build = std::make_unique<DGS::PortableGameplayBuildResult>();
    DGS::BuildPortableGameplayWorldFromState(*g_PortableRestoreState.decodedState, *build);
    if (build->evaluation.readiness != DGS::PortableRestoreReadiness::Ready || !build->success)
    {
        const std::string reason =
            !build->evaluation.blockingReasons.empty() ? build->evaluation.blockingReasons.front() : "portable state not ready";
        FailNow("Portable Restore FAIL", reason);
        return;
    }

    if (!RestorePortableGameplayStateLive(*g_PortableRestoreState.decodedState))
    {
        FailNow("Portable Restore FAIL", "live apply failed");
        return;
    }

    g_PortableRestoreState.expectedBuild = std::move(build);
    SetPhase(Phase::SyncingShell);
}

void ProcessSyncingShell()
{
    std::string detail;
    if (!ApplyPortableShellSync(*g_PortableRestoreState.decodedState, detail))
    {
        LogPortableRestoreEvent("shell-sync-failed", detail.c_str());
        RequestPortableRestoreFailureReturnToMenu();
        FailNow("Portable Restore FAIL", detail);
        return;
    }
    LogPortableRestoreEvent("shell-sync", "ok");

    if (!VerifyPortableShellSync(*g_PortableRestoreState.decodedState, detail))
    {
        LogPortableRestoreEvent("shell-sync-failed", detail.c_str());
        RequestPortableRestoreFailureReturnToMenu();
        FailNow("Portable Restore FAIL", detail);
        return;
    }
    LogPortableRestoreEvent("shell-verify", "ok");

    if (!CapturePostApplyMatches(*g_PortableRestoreState.decodedState, *g_PortableRestoreState.expectedBuild, detail))
    {
        LogPortableRestoreEvent("post-verify-failed", detail.c_str());
        RequestPortableRestoreFailureReturnToMenu();
        FailNow("Portable Restore FAIL", "post-verify failed");
        return;
    }

    LogPortableRestoreEvent("apply", "ok");
    Finish(Phase::Completed, "Portable Restore OK",
           g_PortableRestoreState.fromDisk ? "disk restore applied" : "memory restore applied");
}
} // namespace

bool QueuePortableRestoreFromDisk(const char *path)
{
    if (!IsPortableRestoreDeveloperModeActive())
    {
        return false;
    }

    CaptureMenuRestoreSnapshot();
    THPrac::TH06::THPracResetParams();
    SetPhase(Phase::PendingDecode);
    g_PortableRestoreState.source = Source::ManualDisk;
    g_PortableRestoreState.fromDisk = true;
    g_PortableRestoreState.queuedPath = path != nullptr && path[0] != '\0' ? path : "";
    g_PortableRestoreState.queuedBytes.clear();
    g_PortableRestoreState.sourceTag = "disk";
    g_PortableRestoreState.decodedState.reset();
    g_PortableRestoreState.expectedBuild.reset();
    g_PortableRestoreState.usedMainMenuBootstrap = false;
    g_PortableRestoreState.forceClearGameplayFrames = false;
    g_PortableRestoreState.breakCurrentFrame = true;
    SetStatus("Portable Disk QUEUED", "waiting for restore");
    LogPortableRestoreEvent("queue", "disk");
    return true;
}

bool QueuePortableRestoreFromMemory(const std::vector<u8> &bytes, Source source, const char *sourceTag)
{
    const bool allowWithoutDeveloperMode = source == Source::AuthoritativeNetplayRecovery;
    if ((!IsPortableRestoreDeveloperModeActive() && !allowWithoutDeveloperMode) || bytes.empty())
    {
        return false;
    }

    CaptureMenuRestoreSnapshot();
    THPrac::TH06::THPracResetParams();
    SetPhase(Phase::PendingDecode);
    g_PortableRestoreState.source = source;
    g_PortableRestoreState.fromDisk = false;
    g_PortableRestoreState.queuedBytes = bytes;
    g_PortableRestoreState.queuedPath.clear();
    g_PortableRestoreState.sourceTag = sourceTag != nullptr ? sourceTag : "memory";
    g_PortableRestoreState.decodedState.reset();
    g_PortableRestoreState.expectedBuild.reset();
    g_PortableRestoreState.usedMainMenuBootstrap = false;
    g_PortableRestoreState.forceClearGameplayFrames = false;
    g_PortableRestoreState.breakCurrentFrame = true;
    SetStatus(source == Source::AuthoritativeNetplayRecovery ? "Netplay Restore QUEUED" : "Portable Restore QUEUED",
              "waiting for restore");
    LogPortableRestoreEvent("queue", g_PortableRestoreState.sourceTag.c_str());
    return true;
}

void TickPortableRestore()
{
    const Uint8 *keyboardState = SDL_GetKeyboardState(nullptr);
    const bool diskHotkeyPressed = IsDiskPortableLoadHotkeyPressed(keyboardState);
    if (diskHotkeyPressed && !g_PortableRestoreState.prevDiskHotkeyPressed && g_PortableRestoreState.phase == Phase::Idle)
    {
        QueuePortableRestoreFromDisk(nullptr);
    }
    g_PortableRestoreState.prevDiskHotkeyPressed = diskHotkeyPressed;

    for (int i = 0; i < 8; ++i)
    {
        const Phase before = g_PortableRestoreState.phase;
        switch (g_PortableRestoreState.phase)
        {
        case Phase::Idle:
        case Phase::Completed:
        case Phase::Failed:
            return;
        case Phase::PendingDecode:
            ProcessPendingDecode();
            break;
        case Phase::PendingBootstrap:
            ProcessPendingBootstrap();
            break;
        case Phase::WaitingForGameplayShell:
            ProcessWaitingForGameplayShell();
            break;
        case Phase::Applying:
            ProcessApplying();
            break;
        case Phase::SyncingShell:
            ProcessSyncingShell();
            break;
        default:
            return;
        }

        if (g_PortableRestoreState.phase == before)
        {
            return;
        }
    }
}

bool ConsumeFrameBreakRequested()
{
    const bool requested = g_PortableRestoreState.breakCurrentFrame;
    g_PortableRestoreState.breakCurrentFrame = false;
    return requested;
}

bool ConsumeForcedClearFrameRequested()
{
    if (g_PortableRestoreState.forcedClearFrames == 0)
    {
        return false;
    }

    g_PortableRestoreState.forcedClearFrames -= 1;
    return true;
}

bool ShouldForceClearGameplayFrame()
{
    return g_PortableRestoreState.forceClearGameplayFrames && g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER;
}

bool TryConsumePendingMainMenuBootstrap(::th06::MainMenu *menu)
{
    if (menu == nullptr || !g_PortableRestoreState.pendingMainMenuBootstrap || g_PortableRestoreState.decodedState == nullptr)
    {
        return false;
    }

    if (!IsMainMenuShellStable())
    {
        if (ShouldEmitProgressLog())
        {
            char detail[128];
            std::snprintf(detail, sizeof(detail), "mainmenu-bootstrap-not-stable gameState=%d", g_MainMenu.gameState);
            LogPortableRestoreEvent("bootstrap-consume-wait", detail);
        }
        return false;
    }

    THPrac::TH06::THPracResetParams();
    ApplyBootstrapStartResources(g_PortableRestoreState.decodedState->catalog);

    menu->idleFrames = 0;
    menu->stateTimer = 0;

    const f32 refreshRate = ComputeRefreshRateFromMainMenu();
    g_Supervisor.framerateMultiplier = refreshRate;
    g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);
    g_Supervisor.StopAudio();
    g_PortableRestoreState.pendingMainMenuBootstrap = false;
    g_PortableRestoreState.usedMainMenuBootstrap = true;
    g_Supervisor.curState = SUPERVISOR_STATE_GAMEMANAGER;
    LogPortableRestoreEvent("bootstrap-consumed", "mainmenu");
    return true;
}

void OnMainMenuEntered()
{
    if (!g_MenuRestoreSnapshot.valid || !g_MenuRestoreSnapshot.restoreOnNextMainMenu)
    {
        return;
    }

    ForceCleanupGameplayShellForMenuReturn();
    RestoreMenuRestoreSnapshotFields();
    RestoreMenuRestoreSnapshotStateIds();
    g_MenuRestoreSnapshot.restoreOnNextMainMenu = false;
    g_MenuRestoreSnapshot.valid = false;
}

bool IsBootstrapOrApplyActive()
{
    switch (g_PortableRestoreState.phase)
    {
    case Phase::PendingDecode:
    case Phase::PendingBootstrap:
    case Phase::WaitingForGameplayShell:
    case Phase::Applying:
    case Phase::SyncingShell:
        return true;
    default:
        return false;
    }
}

bool ShouldAdvanceSupervisorTransitionWhileStalled()
{
    if (g_PortableRestoreState.source != Source::AuthoritativeNetplayRecovery)
    {
        return false;
    }

    switch (g_PortableRestoreState.phase)
    {
    case Phase::PendingBootstrap:
    case Phase::WaitingForGameplayShell:
        return g_Supervisor.wantedState != g_Supervisor.curState;

    default:
        return false;
    }
}

void ResetPortableRestoreState()
{
    g_PortableRestoreState = {};
    g_MenuRestoreSnapshot = {};
}

Phase GetPortableRestorePhase()
{
    return g_PortableRestoreState.phase;
}

Source GetPortableRestoreSource()
{
    return g_PortableRestoreState.source;
}

bool HasPortableRestoreStatus()
{
    return SDL_GetTicks() < g_PortableRestoreState.statusUntilTick &&
           (!g_PortableRestoreState.statusLine1.empty() || !g_PortableRestoreState.statusLine2.empty());
}

void GetPortableRestoreStatus(std::string &line1, std::string &line2)
{
    if (!HasPortableRestoreStatus())
    {
        line1.clear();
        line2.clear();
        return;
    }

    line1 = g_PortableRestoreState.statusLine1;
    line2 = g_PortableRestoreState.statusLine2;
}

bool ConsumePortableRestoreTerminalResult(Phase &phase, Source &source, std::string &line1, std::string &line2)
{
    if (!g_PortableRestoreState.hasTerminalResult)
    {
        return false;
    }

    phase = g_PortableRestoreState.terminalPhase;
    source = g_PortableRestoreState.terminalSource;
    line1 = g_PortableRestoreState.terminalLine1;
    line2 = g_PortableRestoreState.terminalLine2;
    g_PortableRestoreState.hasTerminalResult = false;
    g_PortableRestoreState.terminalPhase = Phase::Idle;
    g_PortableRestoreState.terminalSource = Source::ManualMemory;
    g_PortableRestoreState.terminalLine1.clear();
    g_PortableRestoreState.terminalLine2.clear();
    return true;
}
} // namespace th06::PortableGameplayRestore
