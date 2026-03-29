#include "GameplayState.hpp"

#include "NetplayInternal.hpp"

#include <algorithm>
#include <iterator>

namespace th06::DGS
{
namespace
{
template <typename T> i32 PointerToIndex(const T *ptr, const T *base, size_t count)
{
    if (ptr == nullptr || base == nullptr)
    {
        return -1;
    }

    if (ptr < base || ptr >= base + count)
    {
        return -1;
    }

    return (i32)(ptr - base);
}

template <typename T> i32 PointerToOffset(const T *ptr, const void *base)
{
    if (ptr == nullptr || base == nullptr)
    {
        return -1;
    }

    const u8 *rawPtr = reinterpret_cast<const u8 *>(ptr);
    const u8 *rawBase = reinterpret_cast<const u8 *>(base);
    if (rawPtr < rawBase)
    {
        return -1;
    }

    return (i32)(rawPtr - rawBase);
}

template <typename T> T *OffsetToPointer(i32 offset, void *base)
{
    if (offset < 0 || base == nullptr)
    {
        return nullptr;
    }

    return reinterpret_cast<T *>(reinterpret_cast<u8 *>(base) + offset);
}

template <typename T> const T *OffsetToPointer(i32 offset, const void *base)
{
    if (offset < 0 || base == nullptr)
    {
        return nullptr;
    }

    return reinterpret_cast<const T *>(reinterpret_cast<const u8 *>(base) + offset);
}

void InitializeHeader(DeterministicGameplayState &state)
{
    state.header.magic = kDgsMagic;
    state.header.version = (u32)DgsSchemaVersion::V1;
    state.header.sectionCount = 6;
    state.header.reserved = 0;
}

DgsAnmVmRefs CaptureAnmVmRefs(const AnmVm &vm)
{
    DgsAnmVmRefs refs;
    refs.scriptSlot = vm.anmFileIndex;

    AnmRawInstr *baseScript = nullptr;
    if (g_AnmManager != nullptr && refs.scriptSlot >= 0 && refs.scriptSlot < (i32)std::size(g_AnmManager->scripts))
    {
        baseScript = g_AnmManager->scripts[refs.scriptSlot];
    }

    refs.beginningOfScript.value = PointerToOffset(vm.beginingOfScript, baseScript != nullptr ? baseScript : vm.beginingOfScript);
    refs.currentInstruction.value = PointerToOffset(vm.currentInstruction, baseScript != nullptr ? baseScript : vm.beginingOfScript);
    refs.spriteIndex.value =
        g_AnmManager != nullptr ? PointerToIndex(vm.sprite, g_AnmManager->sprites, std::size(g_AnmManager->sprites)) : -1;
    return refs;
}

} // namespace

void RestoreAnmVmRefs(AnmVm &vm, const DgsAnmVmRefs &refs)
{
    int scriptSlot = refs.scriptSlot >= 0 ? refs.scriptSlot : vm.anmFileIndex;
    AnmRawInstr *baseScript = nullptr;
    if (g_AnmManager != nullptr && scriptSlot >= 0 && scriptSlot < (i32)std::size(g_AnmManager->scripts))
    {
        baseScript = g_AnmManager->scripts[scriptSlot];
    }

    vm.beginingOfScript = OffsetToPointer<AnmRawInstr>(refs.beginningOfScript.value, baseScript);
    vm.currentInstruction = OffsetToPointer<AnmRawInstr>(refs.currentInstruction.value, baseScript);
    vm.sprite = (g_AnmManager != nullptr && refs.spriteIndex.HasValue() && refs.spriteIndex.value < (i32)std::size(g_AnmManager->sprites))
                    ? &g_AnmManager->sprites[refs.spriteIndex.value]
                    : nullptr;
}

DgsEnemyContextRefs CaptureEnemyContextRefs(const EnemyEclContext &context)
{
    DgsEnemyContextRefs refs;
    refs.currentInstruction.value = PointerToOffset(context.currentInstr, g_EclManager.eclFile);
    refs.hasFuncSetFunc = context.funcSetFunc != nullptr;
    refs.unresolvedFuncSetFunc = context.funcSetFunc != nullptr;
    return refs;
}

void RestoreEnemyContextRefs(EnemyEclContext &context, const DgsEnemyContextRefs &refs)
{
    context.currentInstr = OffsetToPointer<EclRawInstr>(refs.currentInstruction.value, g_EclManager.eclFile);
}

void CaptureStageRefs(const Stage &stage, DgsStageRefs &refs)
{
    refs.objectOffsets.clear();
    refs.quadVmRefs.clear();
    refs.beginningOfScript.value = PointerToOffset(stage.beginningOfScript, stage.stdData);

    if (stage.objects != nullptr && stage.objectsCount > 0)
    {
        refs.objectOffsets.reserve((size_t)stage.objectsCount);
        for (int i = 0; i < stage.objectsCount; ++i)
        {
            DgsOffsetRef<DgsStageObjectTag> entry;
            entry.value = PointerToOffset(stage.objects[i], stage.stdData);
            refs.objectOffsets.push_back(entry);
        }
    }

    if (stage.quadVms != nullptr && stage.quadCount > 0)
    {
        refs.quadVmRefs.reserve((size_t)stage.quadCount);
        for (int i = 0; i < stage.quadCount; ++i)
        {
            refs.quadVmRefs.push_back(CaptureAnmVmRefs(stage.quadVms[i]));
        }
    }

    refs.spellcardBackground = CaptureAnmVmRefs(stage.spellcardBackground);
    refs.extraBackground = CaptureAnmVmRefs(stage.unk2);
}

void RestoreStageRefs(Stage &stage, const DgsStageRefs &refs)
{
    stage.beginningOfScript = OffsetToPointer<RawStageInstr>(refs.beginningOfScript.value, stage.stdData);

    if (stage.objects != nullptr && (int)refs.objectOffsets.size() == stage.objectsCount)
    {
        for (int i = 0; i < stage.objectsCount; ++i)
        {
            stage.objects[i] = OffsetToPointer<RawStageObject>(refs.objectOffsets[(size_t)i].value, stage.stdData);
        }
    }

    if (stage.quadVms != nullptr && (int)refs.quadVmRefs.size() == stage.quadCount)
    {
        for (int i = 0; i < stage.quadCount; ++i)
        {
            RestoreAnmVmRefs(stage.quadVms[i], refs.quadVmRefs[(size_t)i]);
        }
    }

    RestoreAnmVmRefs(stage.spellcardBackground, refs.spellcardBackground);
    RestoreAnmVmRefs(stage.unk2, refs.extraBackground);
}

void CaptureEnemyManagerRefs(const EnemyManager &enemyManager, DgsEnemyManagerRefs &refs)
{
    for (size_t i = 0; i < refs.bossIndices.size(); ++i)
    {
        refs.bossIndices[i].value = PointerToIndex(enemyManager.bosses[i], enemyManager.enemies, std::size(enemyManager.enemies));
    }

    refs.timelineOffset.value = PointerToOffset(enemyManager.timelineInstr, g_EclManager.eclFile);
    refs.enemyRefs.clear();
    refs.enemyRefs.resize(std::size(enemyManager.enemies));

    for (size_t i = 0; i < std::size(enemyManager.enemies); ++i)
    {
        const Enemy &enemy = enemyManager.enemies[i];
        DgsEnemyRefs &enemyRefs = refs.enemyRefs[i];
        enemyRefs.primaryVm = CaptureAnmVmRefs(enemy.primaryVm);
        for (size_t vmIdx = 0; vmIdx < enemyRefs.vms.size(); ++vmIdx)
        {
            enemyRefs.vms[vmIdx] = CaptureAnmVmRefs(enemy.vms[vmIdx]);
        }

        enemyRefs.currentContext = CaptureEnemyContextRefs(enemy.currentContext);
        for (size_t ctxIdx = 0; ctxIdx < enemyRefs.savedContextStack.size(); ++ctxIdx)
        {
            enemyRefs.savedContextStack[ctxIdx] = CaptureEnemyContextRefs(enemy.savedContextStack[ctxIdx]);
        }

        for (size_t laserIdx = 0; laserIdx < enemyRefs.laserIndices.size(); ++laserIdx)
        {
            enemyRefs.laserIndices[laserIdx].value =
                PointerToIndex(enemy.lasers[laserIdx], g_BulletManager.lasers, std::size(g_BulletManager.lasers));
        }

        for (size_t effectIdx = 0; effectIdx < enemyRefs.effectIndices.size(); ++effectIdx)
        {
            enemyRefs.effectIndices[effectIdx].value =
                PointerToIndex(enemy.effectArray[effectIdx], g_EffectManager.effects, std::size(g_EffectManager.effects));
        }
    }
}

void RestoreEnemyManagerRefs(EnemyManager &enemyManager, const DgsEnemyManagerRefs &refs)
{
    for (size_t i = 0; i < refs.bossIndices.size(); ++i)
    {
        enemyManager.bosses[i] = (refs.bossIndices[i].HasValue() && refs.bossIndices[i].value < (i32)std::size(enemyManager.enemies))
                                     ? &enemyManager.enemies[(size_t)refs.bossIndices[i].value]
                                     : nullptr;
    }

    enemyManager.timelineInstr = OffsetToPointer<EclTimelineInstr>(refs.timelineOffset.value, g_EclManager.eclFile);
    const size_t enemyCount = std::min(refs.enemyRefs.size(), std::size(enemyManager.enemies));
    for (size_t i = 0; i < enemyCount; ++i)
    {
        Enemy &enemy = enemyManager.enemies[i];
        const DgsEnemyRefs &enemyRefs = refs.enemyRefs[i];
        RestoreAnmVmRefs(enemy.primaryVm, enemyRefs.primaryVm);
        for (size_t vmIdx = 0; vmIdx < enemyRefs.vms.size(); ++vmIdx)
        {
            RestoreAnmVmRefs(enemy.vms[vmIdx], enemyRefs.vms[vmIdx]);
        }

        RestoreEnemyContextRefs(enemy.currentContext, enemyRefs.currentContext);
        for (size_t ctxIdx = 0; ctxIdx < enemyRefs.savedContextStack.size(); ++ctxIdx)
        {
            RestoreEnemyContextRefs(enemy.savedContextStack[ctxIdx], enemyRefs.savedContextStack[ctxIdx]);
        }

        for (size_t laserIdx = 0; laserIdx < enemyRefs.laserIndices.size(); ++laserIdx)
        {
            enemy.lasers[laserIdx] = (enemyRefs.laserIndices[laserIdx].HasValue() &&
                                      enemyRefs.laserIndices[laserIdx].value < (i32)std::size(g_BulletManager.lasers))
                                         ? &g_BulletManager.lasers[(size_t)enemyRefs.laserIndices[laserIdx].value]
                                         : nullptr;
        }

        for (size_t effectIdx = 0; effectIdx < enemyRefs.effectIndices.size(); ++effectIdx)
        {
            enemy.effectArray[effectIdx] = (enemyRefs.effectIndices[effectIdx].HasValue() &&
                                            enemyRefs.effectIndices[effectIdx].value < (i32)std::size(g_EffectManager.effects))
                                               ? &g_EffectManager.effects[(size_t)enemyRefs.effectIndices[effectIdx].value]
                                               : nullptr;
        }
    }
}

void CaptureEclManagerRefs(const EclManager &eclManager, DgsEclManagerRefs &refs)
{
    refs.hasEclFile = eclManager.eclFile != nullptr;
    refs.timelineOffset.value = PointerToOffset(eclManager.timeline, eclManager.eclFile);
    refs.subTableOffset.value = PointerToOffset(eclManager.subTable, eclManager.eclFile);
}

void RestoreEclManagerRefs(EclManager &eclManager, const DgsEclManagerRefs &refs)
{
    if (!refs.hasEclFile)
    {
        eclManager.eclFile = nullptr;
    }

    eclManager.timeline = OffsetToPointer<EclTimelineInstr>(refs.timelineOffset.value, eclManager.eclFile);
    eclManager.subTable = OffsetToPointer<EclRawInstr *>(refs.subTableOffset.value, eclManager.eclFile);
}

void CaptureDeterministicRuntimeState(EnemyEclInstr::RuntimeState &enemyEclRuntimeState,
                                      ScreenEffect::RuntimeState &screenEffectRuntimeState,
                                      Controller::RuntimeState &controllerRuntimeState,
                                      DgsSupervisorRuntimeState &supervisorRuntimeState,
                                      DgsGameWindowRuntimeState &gameWindowRuntimeState,
                                      DgsStageRuntimeState &stageRuntimeState,
                                      DgsSoundRuntimeState &soundRuntimeState,
                                      DgsInputRuntimeState &inputRuntimeState)
{
    enemyEclRuntimeState = EnemyEclInstr::CaptureRuntimeState();
    screenEffectRuntimeState = ScreenEffect::CaptureRuntimeState();
    controllerRuntimeState = Controller::CaptureRuntimeState();

    supervisorRuntimeState.calcCount = g_Supervisor.calcCount;
    supervisorRuntimeState.wantedState = g_Supervisor.wantedState;
    supervisorRuntimeState.curState = g_Supervisor.curState;
    supervisorRuntimeState.wantedState2 = g_Supervisor.wantedState2;
    supervisorRuntimeState.unk194 = g_Supervisor.unk194;
    supervisorRuntimeState.unk198 = g_Supervisor.unk198;
    supervisorRuntimeState.isInEnding = g_Supervisor.isInEnding;
    supervisorRuntimeState.vsyncEnabled = g_Supervisor.vsyncEnabled;
    supervisorRuntimeState.lastFrameTime = g_Supervisor.lastFrameTime;
    supervisorRuntimeState.effectiveFramerateMultiplier = g_Supervisor.effectiveFramerateMultiplier;
    supervisorRuntimeState.framerateMultiplier = g_Supervisor.framerateMultiplier;
    supervisorRuntimeState.unk1b4 = g_Supervisor.unk1b4;
    supervisorRuntimeState.unk1b8 = g_Supervisor.unk1b8;
    supervisorRuntimeState.startupTimeBeforeMenuMusic = g_Supervisor.startupTimeBeforeMenuMusic;

    gameWindowRuntimeState.tickCountToEffectiveFramerate = g_TickCountToEffectiveFramerate;
    gameWindowRuntimeState.lastFrameTime = g_LastFrameTime;
    gameWindowRuntimeState.curFrame = g_GameWindow.curFrame;

    stageRuntimeState.objectInstances.clear();
    stageRuntimeState.quadVms.clear();
    if (g_Stage.objectInstances != nullptr && g_Stage.objectsCount > 0)
    {
        stageRuntimeState.objectInstances.assign(g_Stage.objectInstances, g_Stage.objectInstances + g_Stage.objectsCount);
    }
    if (g_Stage.quadVms != nullptr && g_Stage.quadCount > 0)
    {
        stageRuntimeState.quadVms.assign(g_Stage.quadVms, g_Stage.quadVms + g_Stage.quadCount);
    }

    std::memcpy(soundRuntimeState.soundBuffersToPlay, g_SoundPlayer.soundBuffersToPlay,
                sizeof(soundRuntimeState.soundBuffersToPlay));
    std::memcpy(soundRuntimeState.queuedSfxState, g_SoundPlayer.unk408, sizeof(soundRuntimeState.queuedSfxState));
    soundRuntimeState.isLooping = g_SoundPlayer.isLooping;

    inputRuntimeState.lastFrameInput = g_LastFrameInput;
    inputRuntimeState.curFrameInput = g_CurFrameInput;
    inputRuntimeState.eighthFrameHeldInput = g_IsEigthFrameOfHeldInput;
    inputRuntimeState.heldInputFrames = g_NumOfFramesInputsWereHeld;
}

bool RestoreDeterministicRuntimeState(const EnemyEclInstr::RuntimeState &enemyEclRuntimeState,
                                      const ScreenEffect::RuntimeState &screenEffectRuntimeState,
                                      const Controller::RuntimeState &controllerRuntimeState,
                                      const DgsSupervisorRuntimeState &supervisorRuntimeState,
                                      const DgsGameWindowRuntimeState &gameWindowRuntimeState,
                                      const DgsStageRuntimeState &stageRuntimeState,
                                      const DgsSoundRuntimeState &soundRuntimeState,
                                      const DgsInputRuntimeState &inputRuntimeState)
{
    if (!stageRuntimeState.objectInstances.empty())
    {
        if (g_Stage.objectInstances == nullptr || (int)stageRuntimeState.objectInstances.size() != g_Stage.objectsCount)
        {
            return false;
        }

        std::memcpy(g_Stage.objectInstances, stageRuntimeState.objectInstances.data(),
                    stageRuntimeState.objectInstances.size() * sizeof(RawStageObjectInstance));
    }

    if (!stageRuntimeState.quadVms.empty())
    {
        if (g_Stage.quadVms == nullptr || (int)stageRuntimeState.quadVms.size() != g_Stage.quadCount)
        {
            return false;
        }

        std::memcpy(g_Stage.quadVms, stageRuntimeState.quadVms.data(),
                    stageRuntimeState.quadVms.size() * sizeof(AnmVm));
    }

    EnemyEclInstr::RestoreRuntimeState(enemyEclRuntimeState);
    ScreenEffect::RestoreRuntimeState(screenEffectRuntimeState);
    Controller::RestoreRuntimeState(controllerRuntimeState);

    g_Supervisor.calcCount = supervisorRuntimeState.calcCount;
    g_Supervisor.wantedState = supervisorRuntimeState.wantedState;
    g_Supervisor.curState = supervisorRuntimeState.curState;
    g_Supervisor.wantedState2 = supervisorRuntimeState.wantedState2;
    g_Supervisor.unk194 = supervisorRuntimeState.unk194;
    g_Supervisor.unk198 = supervisorRuntimeState.unk198;
    g_Supervisor.isInEnding = supervisorRuntimeState.isInEnding;
    g_Supervisor.vsyncEnabled = supervisorRuntimeState.vsyncEnabled;
    g_Supervisor.lastFrameTime = supervisorRuntimeState.lastFrameTime;
    g_Supervisor.effectiveFramerateMultiplier = supervisorRuntimeState.effectiveFramerateMultiplier;
    g_Supervisor.framerateMultiplier = supervisorRuntimeState.framerateMultiplier;
    g_Supervisor.unk1b4 = supervisorRuntimeState.unk1b4;
    g_Supervisor.unk1b8 = supervisorRuntimeState.unk1b8;
    g_Supervisor.startupTimeBeforeMenuMusic = supervisorRuntimeState.startupTimeBeforeMenuMusic;

    g_TickCountToEffectiveFramerate = gameWindowRuntimeState.tickCountToEffectiveFramerate;
    g_LastFrameTime = gameWindowRuntimeState.lastFrameTime;
    g_GameWindow.curFrame = gameWindowRuntimeState.curFrame;

    std::memcpy(g_SoundPlayer.soundBuffersToPlay, soundRuntimeState.soundBuffersToPlay,
                sizeof(g_SoundPlayer.soundBuffersToPlay));
    std::memcpy(g_SoundPlayer.unk408, soundRuntimeState.queuedSfxState, sizeof(g_SoundPlayer.unk408));
    g_SoundPlayer.isLooping = soundRuntimeState.isLooping;

    g_LastFrameInput = inputRuntimeState.lastFrameInput;
    g_CurFrameInput = inputRuntimeState.curFrameInput;
    g_IsEigthFrameOfHeldInput = inputRuntimeState.eighthFrameHeldInput;
    g_NumOfFramesInputsWereHeld = inputRuntimeState.heldInputFrames;
    return true;
}

void PopulateUnresolvedFields(DeterministicGameplayState &state)
{
    state.unresolvedFields.clear();
    state.unresolvedFields.emplace_back("Player.calc/draw/bullets/chain pointers remain in-process only.");
    state.unresolvedFields.emplace_back("BulletManager laser AnmVm script pointers are not normalized in phase 1.");
    state.unresolvedFields.emplace_back("EffectManager effect update callbacks remain in-process only.");
    state.unresolvedFields.emplace_back("EnemyEclContext.funcSetFunc remains unresolved and in-process only.");
    state.unresolvedFields.emplace_back("EnemyManager enemyTemplate pointer-bearing fields remain unresolved.");
    state.unresolvedFields.emplace_back("EclManager subTable contents are only normalized at the base offset level.");
}

template <typename T> void CaptureOpaqueState(DgsOpaqueState<T> &dst, const T &src)
{
    dst.CaptureFrom(src);
}

void CaptureCoreState(DeterministicGameplayState &state, const GameManager &gameManager, const Player &player1,
                      const Player &player2, const BulletManager &bulletManager, const EnemyManager &enemyManager,
                      const ItemManager &itemManager, const EffectManager &effectManager, const Stage &stageState,
                      const EclManager &eclManager, const Rng &rng)
{
    CaptureOpaqueState(state.gameManager, gameManager);
    CaptureOpaqueState(state.player1, player1);
    CaptureOpaqueState(state.player2, player2);
    CaptureOpaqueState(state.bulletManager, bulletManager);
    CaptureOpaqueState(state.enemyManager, enemyManager);
    CaptureOpaqueState(state.itemManager, itemManager);
    CaptureOpaqueState(state.effectManager, effectManager);
    CaptureOpaqueState(state.stageState, stageState);
    CaptureOpaqueState(state.eclManager, eclManager);
    CaptureOpaqueState(state.rng, rng);

    CaptureStageRefs(stageState, state.stageRefs);
    CaptureEnemyManagerRefs(enemyManager, state.enemyManagerRefs);
    CaptureEclManagerRefs(eclManager, state.eclManagerRefs);
}

void CaptureSharedGameplaySnapshotState(Netplay::GameplaySnapshot &snapshot)
{
    snapshot.gameManager = g_GameManager;
    snapshot.player1 = g_Player;
    snapshot.player2 = g_Player2;
    snapshot.bulletManager = g_BulletManager;
    snapshot.enemyManager = g_EnemyManager;
    snapshot.itemManager = g_ItemManager;
    snapshot.effectManager = g_EffectManager;
    snapshot.stageState = g_Stage;
    snapshot.eclManager = g_EclManager;
    snapshot.rng = g_Rng;

    CaptureDeterministicRuntimeState(snapshot.enemyEclRuntimeState, snapshot.screenEffectRuntimeState,
                                     snapshot.controllerRuntimeState, snapshot.supervisorRuntimeState,
                                     snapshot.gameWindowRuntimeState, snapshot.stageRuntimeState,
                                     snapshot.soundRuntimeState, snapshot.inputRuntimeState);
}

bool RestoreSharedGameplaySnapshotState(const Netplay::GameplaySnapshot &snapshot)
{
    g_GameManager = snapshot.gameManager;
    g_Player = snapshot.player1;
    g_Player2 = snapshot.player2;
    g_BulletManager = snapshot.bulletManager;
    g_EnemyManager = snapshot.enemyManager;
    g_ItemManager = snapshot.itemManager;
    g_EffectManager = snapshot.effectManager;
    g_Stage = snapshot.stageState;
    g_EclManager = snapshot.eclManager;
    g_Rng = snapshot.rng;

    return RestoreDeterministicRuntimeState(snapshot.enemyEclRuntimeState, snapshot.screenEffectRuntimeState,
                                            snapshot.controllerRuntimeState, snapshot.supervisorRuntimeState,
                                            snapshot.gameWindowRuntimeState, snapshot.stageRuntimeState,
                                            snapshot.soundRuntimeState, snapshot.inputRuntimeState);
}

void CaptureDeterministicGameplayState(DeterministicGameplayState &state)
{
    InitializeHeader(state);
    state.frame = Netplay::g_State.currentNetFrame;
    state.stage = g_GameManager.currentStage;
    state.delay = Netplay::g_State.delay;
    state.currentDelayCooldown = Netplay::g_State.currentDelayCooldown;

    CaptureCoreState(state, g_GameManager, g_Player, g_Player2, g_BulletManager, g_EnemyManager, g_ItemManager,
                     g_EffectManager, g_Stage, g_EclManager, g_Rng);
    CaptureDeterministicRuntimeState(state.enemyEclRuntimeState, state.screenEffectRuntimeState,
                                     state.controllerRuntimeState, state.supervisorRuntimeState,
                                     state.gameWindowRuntimeState, state.stageRuntimeState, state.soundRuntimeState,
                                     state.inputRuntimeState);
    PopulateUnresolvedFields(state);
}

void CaptureDeterministicGameplayStateFromGameplaySnapshot(const Netplay::GameplaySnapshot &snapshot,
                                                          DeterministicGameplayState &state)
{
    InitializeHeader(state);
    state.frame = snapshot.frame;
    state.stage = snapshot.stage;
    state.delay = snapshot.delay;
    state.currentDelayCooldown = snapshot.currentDelayCooldown;

    CaptureCoreState(state, snapshot.gameManager, snapshot.player1, snapshot.player2, snapshot.bulletManager,
                     snapshot.enemyManager, snapshot.itemManager, snapshot.effectManager, snapshot.stageState,
                     snapshot.eclManager, snapshot.rng);
    state.enemyEclRuntimeState = snapshot.enemyEclRuntimeState;
    state.screenEffectRuntimeState = snapshot.screenEffectRuntimeState;
    state.controllerRuntimeState = snapshot.controllerRuntimeState;
    state.supervisorRuntimeState = snapshot.supervisorRuntimeState;
    state.gameWindowRuntimeState = snapshot.gameWindowRuntimeState;
    state.stageRuntimeState = snapshot.stageRuntimeState;
    state.soundRuntimeState = snapshot.soundRuntimeState;
    state.inputRuntimeState = snapshot.inputRuntimeState;
    PopulateUnresolvedFields(state);
}

bool RestoreDeterministicGameplayState(const DeterministicGameplayState &state)
{
    if (state.stage != g_GameManager.currentStage)
    {
        return false;
    }

    state.gameManager.RestoreTo(g_GameManager);
    state.player1.RestoreTo(g_Player);
    state.player2.RestoreTo(g_Player2);
    state.bulletManager.RestoreTo(g_BulletManager);
    state.enemyManager.RestoreTo(g_EnemyManager);
    state.itemManager.RestoreTo(g_ItemManager);
    state.effectManager.RestoreTo(g_EffectManager);
    state.stageState.RestoreTo(g_Stage);
    state.eclManager.RestoreTo(g_EclManager);
    state.rng.RestoreTo(g_Rng);

    if (!RestoreDeterministicRuntimeState(state.enemyEclRuntimeState, state.screenEffectRuntimeState,
                                          state.controllerRuntimeState, state.supervisorRuntimeState,
                                          state.gameWindowRuntimeState, state.stageRuntimeState,
                                          state.soundRuntimeState, state.inputRuntimeState))
    {
        return false;
    }

    RestoreEclManagerRefs(g_EclManager, state.eclManagerRefs);
    RestoreStageRefs(g_Stage, state.stageRefs);
    RestoreEnemyManagerRefs(g_EnemyManager, state.enemyManagerRefs);

    Netplay::g_State.delay = state.delay;
    Netplay::g_State.currentDelayCooldown = state.currentDelayCooldown;
    Netplay::g_State.currentCtrl = Netplay::IGC_NONE;
    return true;
}
} // namespace th06::DGS
