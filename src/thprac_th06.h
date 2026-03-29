#pragma once

#include "thprac_games.h"
#include "GameManager.hpp"
#include <stdint.h>

namespace THPrac {
namespace TH06 {

    // Struct definition and constants - macros already defined in GameManager.hpp

    struct Th6k {
        uint32_t magic;
        uint16_t th6kLen;
        uint16_t unkLen;
        uint8_t version;
        uint8_t unk_9;
    };

    struct Catk {
        Th6k base;
        int32_t captureScore;
        uint16_t idx;
        uint8_t nameCsum;
        uint8_t characterShotType;
        uint32_t unk_14;
        char name[32];
        uint32_t unk_38;
        uint16_t numAttempts;
        uint16_t numSuccess;
    };

    struct Clrd {
        Th6k base;
        uint8_t difficultyClearedWithRetries[5];
        uint8_t difficultyClearedWithoutRetries[5];
        uint8_t characterShotType;
    };

    struct Pscr {
        Th6k base;
        int32_t score;
        uint8_t character;
        uint8_t difficulty;
        uint8_t stage;
    };

    struct GameManager {
        uint32_t guiScore;
        uint32_t score;
        uint32_t nextScoreIncrement;
        uint32_t highScore;
        uint32_t difficulty;
        int32_t grazeInStage;
        int32_t grazeInTotal;
        uint32_t isInReplay;
        int32_t deaths;
        int32_t bombsUsed;
        int32_t spellcardsCaptured;
        int8_t isTimeStopped;
        Catk catk[CATK_NUM_CAPTURES];
        Clrd clrd[CLRD_NUM_CHARACTERS];
        Pscr pscr[PSCR_NUM_CHARS_SHOTTYPES][PSCR_NUM_STAGES][PSCR_NUM_DIFFICULTIES];
        uint16_t currentPower;
        int8_t unk_1812;
        int8_t unk_1813;
        uint16_t pointItemsCollectedInStage;
        uint16_t pointItemsCollected;
        uint8_t numRetries;
        int8_t powerItemCountForScore;
        int8_t livesRemaining;
        int8_t bombsRemaining;
        int8_t extraLives;
        uint8_t character;
        uint8_t shotType;
        uint8_t isInGameMenu;
        uint8_t isInRetryMenu;
        uint8_t isInMenu;
        uint8_t isGameCompleted;
        uint8_t isInPracticeMode;
        uint8_t demoMode;
        int8_t unk_1825;
        int8_t unk_1826;
        int8_t unk_1827;
        int32_t demoFrames;
        int8_t replayFile[256];
        int8_t unk_192c[256];
        uint16_t randomSeed;
        uint32_t gameFrames;
        int32_t currentStage;
        uint32_t menuCursorBackup;
        Float2 arcadeRegionTopLeftPos;
        Float2 arcadeRegionSize;
        Float2 playerMovementAreaTopLeftPos;
        Float2 playerMovementAreaSize;
        float cameraDistance;
        Float3 stageCameraFacingDir;
        int32_t counat;
        int32_t rank;
        int32_t maxRank;
        int32_t minRank;
        int32_t subRank;
    };

    // Called by integration module — creates GUI singletons.
    void THGuiCreate();
    // Per-frame GUI update (extracted from th06_update hook body).
    void THPracUpdate();
    // Per-frame GUI render (extracted from th06_render hook body).
    void THPracRender();

    // Practice menu integration — called from MainMenu.cpp
    void THPracMenuOpen();    // Open practice overlay (State 1)
    void THPracMenuConfirm(); // Fill params and close (State 3)
    void THPracMenuCancel();  // Close without filling (State 4)
    // Apply thPracParam overrides: sets stage and difficulty on the real GameManager.
    // Returns the stage value that was applied.
    int THPracMenuApply();
    // Returns true if thprac practice mode is active (mode != 0).
    bool THPracIsActive();
    bool THPracIsManualDumpHotkeyEnabled();
    bool THPracIsRecoveryAutoDumpEnabled();
    bool THPracIsDebugLogEnabled();
    bool THPracIsDeveloperModeEnabled();
    bool THPracConsumeEndingShortcut();
    void THPracResetEndingShortcut();
    void THPracPrepareDebugEndingJump();
    bool THPracIsDebugEndingJumpActive();
    void THPracClearDebugEndingJump();
    // Apply practice parameters (lives, bombs, power, etc.) at stage start.
    // Called from GameManager::AddedCallback after normal initialization.
    void THPracApplyStageParams();
    // Fix RNG seed — called AFTER mgr->randomSeed = g_Rng.seed (matches original hook timing).
    void THPracFixSeed();
    // Apply ECL patches after ECL file is loaded
    void THPracPostEclLoad();
    // Suppress stage title / song name sprites after GUI init during warp
    void THPracPostGuiInit();
    // Returns true if practice warp targets a boss section (needs boss BGM).
    bool THPracShouldPlayBossBGM();
    void THPortableFastForwardStageShell(int targetFrame);
    bool THPortableReloadBossSectionAssets(int profile);
    bool THPortableSyncStageIntroSprites(bool hideStageNameIntro, bool hideSongNameIntro);
    bool THPortableSyncStageBgm(int trackIndex);
    void THPortableSetCurrentBgmTrackIndex(int trackIndex);
    int THPortableGetCurrentBgmTrackIndex();
    void THPortableSetCurrentBossAssetProfile(int profile);
    int THPortableGetCurrentBossAssetProfile();
    void THPortableResetShellSyncTrackers();

    // Overlay cheat queries — return true when the toggle is active
    bool THPracIsMuteki();    // F1: Invincibility
    bool THPracIsInfLives();  // F2: Infinite lives
    bool THPracIsInfBombs();  // F3: Infinite bombs
    bool THPracIsInfPower();  // F4: Infinite power
    bool THPracIsTimeLock();  // F5: Freeze ECL timeline
    bool THPracIsAutoBomb();  // F6: Auto-bomb on hit

    void THPracCountMiss();   // Increment miss counter
    bool THPracIsRankLockDown(); // F11: Prevent rank decrease on miss/bomb
    int32_t THPracGetRepSeed();  // Get last captured replay seed
    void THPracLockTimerTick();  // Increment lock timer (called each boss timer tick frame)
    void THPracLockTimerReset(); // Reset lock timer (called on new spell/boss phase)
    void THPracCaptureRepSeed(); // Capture current RNG seed for replay display

    void THPracSpellAttempt();   // Record spell attempt (called at SPELLCARDSTART)
    void THPracSpellCapture();   // Record spell capture (called at SPELLCARDEND when captured)
    void THPracSpellTimeout();   // Record spell timeout (called when boss timer finishes)
    void THPracSaveData();       // Persist TH06Save data to disk
    void THPracResetParams();    // Reset practice params (called on main menu return)
}
}
