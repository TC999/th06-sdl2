#include "GameplayStatePortable.hpp"

#include "BombData.hpp"
#include "EnemyEclInstr.hpp"
#include "FileSystem.hpp"
#include "Gui.hpp"
#include "i18n.hpp"
#include "Session.hpp"
#include "thprac_th06.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>

namespace th06::DGS
{
namespace
{
constexpr u64 kFnvOffset = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;
constexpr const char *kPortableEclFiles[] = {"dummy",
                                             "data/ecldata1.ecl",
                                             "data/ecldata2.ecl",
                                             "data/ecldata3.ecl",
                                             "data/ecldata4.ecl",
                                             "data/ecldata5.ecl",
                                             "data/ecldata6.ecl",
                                             "data/ecldata7.ecl",
                                             nullptr};
constexpr const char *kPortableStageStdFiles[] = {"dummy",
                                                  "data/stage1.std",
                                                  "data/stage2.std",
                                                  "data/stage3.std",
                                                  "data/stage4.std",
                                                  "data/stage5.std",
                                                  "data/stage6.std",
                                                  "data/stage7.std",
                                                  nullptr};
constexpr const char *kPortableStageAnmFiles[] = {"dummy",
                                                  "data/stg1bg.anm",
                                                  "data/stg2bg.anm",
                                                  "data/stg3bg.anm",
                                                  "data/stg4bg.anm",
                                                  "data/stg5bg.anm",
                                                  "data/stg6bg.anm",
                                                  "data/stg7bg.anm",
                                                  nullptr};

const char *ResolvePortableBombSpellcardName()
{
    const Character character = Session::IsDualPlayerSession() ? (Character)g_GameManager.character : CHARA_REIMU;
    const ShotType shotType = Session::IsDualPlayerSession() ? (ShotType)g_GameManager.shotType : SHOT_TYPE_A;
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

template <typename T> void HashValue(u64 &hash, const T &value)
{
    const u8 *bytes = reinterpret_cast<const u8 *>(&value);
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

template <typename T> i32 PointerToIndex(const T *ptr, const T *base, size_t count)
{
    if (ptr == nullptr || base == nullptr || ptr < base || ptr >= base + count)
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

void HashString(u64 &hash, const std::string &value)
{
    const u32 size = (u32)value.size();
    HashValue(hash, size);
    for (char ch : value)
    {
        hash ^= (u8)ch;
        hash *= kFnvPrime;
    }
}

u64 HashByteSpan(const void *data, size_t size)
{
    u64 hash = kFnvOffset;
    const u8 *bytes = reinterpret_cast<const u8 *>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

template <typename T> i32 RawPointerFieldToOffset(T *value)
{
    return (i32)(uintptr_t)value;
}

const char *ResolvePortableEclPathForStage(i32 stage)
{
    if (stage < 0 || stage >= (i32)std::size(kPortableEclFiles))
    {
        return nullptr;
    }
    return kPortableEclFiles[stage];
}

const char *ResolvePortableStageStdPathForStage(i32 stage)
{
    if (stage < 0 || stage >= (i32)std::size(kPortableStageStdFiles))
    {
        return nullptr;
    }
    return kPortableStageStdFiles[stage];
}

const char *ResolvePortableStageAnmPathForStage(i32 stage)
{
    if (stage < 0 || stage >= (i32)std::size(kPortableStageAnmFiles))
    {
        return nullptr;
    }
    return kPortableStageAnmFiles[stage];
}

bool IsPortableIntroSpriteHidden(const AnmVm &vm)
{
    return vm.flags.isVisible == 0 || vm.currentInstruction == nullptr;
}

bool ReadPortableResourceBytes(const char *path, std::vector<u8> &outBytes)
{
    outBytes.clear();
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    u8 *rawData = FileSystem::OpenPath((char *)path, false);
    if (rawData == nullptr)
    {
        return false;
    }

    const u32 sizeBytes = g_LastFileSize;
    outBytes.assign(rawData, rawData + sizeBytes);
    std::free(rawData);
    return true;
}

PortableGameplayResourceCatalog CapturePortableGameplayResourceCatalog()
{
    PortableGameplayResourceCatalog catalog;
    catalog.currentStage = (u32)g_GameManager.currentStage;
    catalog.difficulty = (u32)g_GameManager.difficulty;
    catalog.character1 = g_GameManager.character;
    catalog.shotType1 = g_GameManager.shotType;
    catalog.character2 = g_GameManager.character2;
    catalog.shotType2 = g_GameManager.shotType2;
    catalog.hasSecondPlayer = Session::IsDualPlayerSession() ? 1u : 0u;
    catalog.isPracticeMode = g_GameManager.isInPracticeMode;
    catalog.isReplay = g_GameManager.isInReplay;

    u64 hash = kFnvOffset;
    HashValue(hash, catalog.currentStage);
    HashValue(hash, catalog.difficulty);
    HashValue(hash, catalog.character1);
    HashValue(hash, catalog.shotType1);
    HashValue(hash, catalog.character2);
    HashValue(hash, catalog.shotType2);
    HashValue(hash, catalog.hasSecondPlayer);
    HashValue(hash, catalog.isPracticeMode);
    HashValue(hash, catalog.isReplay);
    catalog.catalogHash = hash;
    return catalog;
}

PortableGameplayCoreState CapturePortableGameplayCoreState()
{
    PortableGameplayCoreState core;
    core.guiScore = g_GameManager.guiScore;
    core.score = g_GameManager.score;
    core.nextScoreIncrement = g_GameManager.nextScoreIncrement;
    core.highScore = g_GameManager.highScore;
    core.difficulty = (u32)g_GameManager.difficulty;
    core.grazeInStage = g_GameManager.grazeInStage;
    core.grazeInTotal = g_GameManager.grazeInTotal;
    core.deaths = g_GameManager.deaths;
    core.bombsUsed = g_GameManager.bombsUsed;
    core.spellcardsCaptured = g_GameManager.spellcardsCaptured;
    core.pointItemsCollectedInStage = g_GameManager.pointItemsCollectedInStage;
    core.pointItemsCollected = g_GameManager.pointItemsCollected;
    core.powerItemCountForScore = g_GameManager.powerItemCountForScore;
    core.extraLives = g_GameManager.extraLives;
    core.currentStage = g_GameManager.currentStage;
    core.rank = g_GameManager.rank;
    core.maxRank = g_GameManager.maxRank;
    core.minRank = g_GameManager.minRank;
    core.subRank = g_GameManager.subRank;
    core.randomSeed = g_GameManager.randomSeed;
    core.gameFrames = g_GameManager.gameFrames;
    core.currentPower1 = g_GameManager.currentPower;
    core.currentPower2 = g_GameManager.currentPower2;
    core.livesRemaining1 = g_GameManager.livesRemaining;
    core.bombsRemaining1 = g_GameManager.bombsRemaining;
    core.livesRemaining2 = g_GameManager.livesRemaining2;
    core.bombsRemaining2 = g_GameManager.bombsRemaining2;
    core.numRetries = g_GameManager.numRetries;
    core.isTimeStopped = g_GameManager.isTimeStopped;
    core.isGameCompleted = g_GameManager.isGameCompleted;
    core.isPracticeMode = g_GameManager.isInPracticeMode;
    core.demoMode = g_GameManager.demoMode;
    core.character1 = g_GameManager.character;
    core.shotType1 = g_GameManager.shotType;
    core.character2 = g_GameManager.character2;
    core.shotType2 = g_GameManager.shotType2;
    return core;
}

void CapturePortableGameplayRuntimeState(const DeterministicGameplayState &dgsState, PortableGameplayRuntimeState &runtime)
{
    runtime.stageObjectInstances.clear();
    runtime.frame = dgsState.frame;
    runtime.stage = dgsState.stage;
    runtime.delay = dgsState.delay;
    runtime.currentDelayCooldown = dgsState.currentDelayCooldown;
    runtime.enemyEclRuntimeState = dgsState.enemyEclRuntimeState;
    runtime.screenEffectRuntimeState = dgsState.screenEffectRuntimeState;
    runtime.controllerRuntimeState = dgsState.controllerRuntimeState;
    runtime.supervisorRuntimeState = dgsState.supervisorRuntimeState;
    runtime.gameWindowRuntimeState = dgsState.gameWindowRuntimeState;
    runtime.soundRuntimeState = dgsState.soundRuntimeState;
    runtime.inputRuntimeState = dgsState.inputRuntimeState;
    runtime.stageObjectInstances = dgsState.stageRuntimeState.objectInstances;
}

PortableEnemyFlagsState CapturePortableEnemyFlags(const EnemyFlags &flags)
{
    PortableEnemyFlagsState out;
    out.unk1 = flags.unk1;
    out.unk2 = flags.unk2;
    out.unk3 = flags.unk3;
    out.unk4 = flags.unk4;
    out.unk5 = flags.unk5;
    out.unk6 = flags.unk6;
    out.unk7 = flags.unk7;
    out.unk8 = flags.unk8;
    out.isBoss = flags.isBoss;
    out.unk10 = flags.unk10;
    out.unk11 = flags.unk11;
    out.shouldClampPos = flags.shouldClampPos;
    out.unk13 = flags.unk13;
    out.unk14 = flags.unk14;
    out.unk15 = flags.unk15;
    out.unk16 = flags.unk16;
    return out;
}

DgsAnmVmRefs CapturePortableAnmVmRefs(const AnmVm &vm)
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

void CapturePortableAnmVmState(const AnmVm &vm, PortableAnmVmState &out)
{
    out.rotation = vm.rotation;
    out.angleVel = vm.angleVel;
    out.scaleY = vm.scaleY;
    out.scaleX = vm.scaleX;
    out.scaleInterpFinalY = vm.scaleInterpFinalY;
    out.scaleInterpFinalX = vm.scaleInterpFinalX;
    out.uvScrollPos = vm.uvScrollPos;
    out.currentTimeInScript = vm.currentTimeInScript;
    out.matrix = vm.matrix;
    out.color = vm.color;
    out.flags = vm.flags.flags;
    out.alphaInterpEndTime = vm.alphaInterpEndTime;
    out.scaleInterpEndTime = vm.scaleInterpEndTime;
    out.autoRotate = vm.autoRotate;
    out.pendingInterrupt = vm.pendingInterrupt;
    out.posInterpEndTime = vm.posInterpEndTime;
    out.pos = vm.pos;
    out.scaleInterpInitialY = vm.scaleInterpInitialY;
    out.scaleInterpInitialX = vm.scaleInterpInitialX;
    out.scaleInterpTime = vm.scaleInterpTime;
    out.activeSpriteIndex = vm.activeSpriteIndex;
    out.baseSpriteIndex = vm.baseSpriteIndex;
    out.anmFileIndex = vm.anmFileIndex;
    out.alphaInterpInitial = vm.alphaInterpInitial;
    out.alphaInterpFinal = vm.alphaInterpFinal;
    out.posInterpInitial = vm.posInterpInitial;
    out.posInterpFinal = vm.posInterpFinal;
    out.posOffset = vm.posOffset;
    out.posInterpTime = vm.posInterpTime;
    out.timeOfLastSpriteSet = vm.timeOfLastSpriteSet;
    out.alphaInterpTime = vm.alphaInterpTime;
    out.fontWidth = vm.fontWidth;
    out.fontHeight = vm.fontHeight;
    out.refs = CapturePortableAnmVmRefs(vm);
}

PortableEnemyFuncToken CapturePortableEnemyFuncToken(void (*func)(Enemy *, EclRawInstr *))
{
    if (func == nullptr)
    {
        return PortableEnemyFuncToken::None;
    }
    if (func == EnemyEclInstr::ExInsCirnoRainbowBallJank)
        return PortableEnemyFuncToken::ExInsCirnoRainbowBallJank;
    if (func == EnemyEclInstr::ExInsShootAtRandomArea)
        return PortableEnemyFuncToken::ExInsShootAtRandomArea;
    if (func == EnemyEclInstr::ExInsShootStarPattern)
        return PortableEnemyFuncToken::ExInsShootStarPattern;
    if (func == EnemyEclInstr::ExInsPatchouliShottypeSetVars)
        return PortableEnemyFuncToken::ExInsPatchouliShottypeSetVars;
    if (func == EnemyEclInstr::ExInsStage56Func4)
        return PortableEnemyFuncToken::ExInsStage56Func4;
    if (func == EnemyEclInstr::ExInsStage5Func5)
        return PortableEnemyFuncToken::ExInsStage5Func5;
    if (func == EnemyEclInstr::ExInsStage6XFunc6)
        return PortableEnemyFuncToken::ExInsStage6XFunc6;
    if (func == EnemyEclInstr::ExInsStage6Func7)
        return PortableEnemyFuncToken::ExInsStage6Func7;
    if (func == EnemyEclInstr::ExInsStage6Func8)
        return PortableEnemyFuncToken::ExInsStage6Func8;
    if (func == EnemyEclInstr::ExInsStage6Func9)
        return PortableEnemyFuncToken::ExInsStage6Func9;
    if (func == EnemyEclInstr::ExInsStage6XFunc10)
        return PortableEnemyFuncToken::ExInsStage6XFunc10;
    if (func == EnemyEclInstr::ExInsStage6Func11)
        return PortableEnemyFuncToken::ExInsStage6Func11;
    if (func == EnemyEclInstr::ExInsStage4Func12)
        return PortableEnemyFuncToken::ExInsStage4Func12;
    if (func == EnemyEclInstr::ExInsStageXFunc13)
        return PortableEnemyFuncToken::ExInsStageXFunc13;
    if (func == EnemyEclInstr::ExInsStageXFunc14)
        return PortableEnemyFuncToken::ExInsStageXFunc14;
    if (func == EnemyEclInstr::ExInsStageXFunc15)
        return PortableEnemyFuncToken::ExInsStageXFunc15;
    if (func == EnemyEclInstr::ExInsStageXFunc16)
        return PortableEnemyFuncToken::ExInsStageXFunc16;
    return PortableEnemyFuncToken::Unknown;
}

PortablePlayerFireFuncToken CapturePortablePlayerFireFuncToken(FireBulletCallback func)
{
    if (func == nullptr)
    {
        return PortablePlayerFireFuncToken::None;
    }
    if (func == Player::FireBulletReimuA)
        return PortablePlayerFireFuncToken::FireBulletReimuA;
    if (func == Player::FireBulletReimuB)
        return PortablePlayerFireFuncToken::FireBulletReimuB;
    if (func == Player::FireBulletMarisaA)
        return PortablePlayerFireFuncToken::FireBulletMarisaA;
    if (func == Player::FireBulletMarisaB)
        return PortablePlayerFireFuncToken::FireBulletMarisaB;
    return PortablePlayerFireFuncToken::Unknown;
}

PortablePlayerBombFuncToken CapturePortablePlayerBombFuncToken(void (*func)(Player *))
{
    if (func == nullptr)
    {
        return PortablePlayerBombFuncToken::None;
    }
    if (func == BombData::BombReimuACalc)
        return PortablePlayerBombFuncToken::BombReimuACalc;
    if (func == BombData::BombReimuBCalc)
        return PortablePlayerBombFuncToken::BombReimuBCalc;
    if (func == BombData::BombMarisaACalc)
        return PortablePlayerBombFuncToken::BombMarisaACalc;
    if (func == BombData::BombMarisaBCalc)
        return PortablePlayerBombFuncToken::BombMarisaBCalc;
    if (func == BombData::BombReimuADraw)
        return PortablePlayerBombFuncToken::BombReimuADraw;
    if (func == BombData::BombReimuBDraw)
        return PortablePlayerBombFuncToken::BombReimuBDraw;
    if (func == BombData::BombMarisaADraw)
        return PortablePlayerBombFuncToken::BombMarisaADraw;
    if (func == BombData::BombMarisaBDraw)
        return PortablePlayerBombFuncToken::BombMarisaBDraw;
    if (func == BombData::DarkenViewport)
        return PortablePlayerBombFuncToken::DarkenViewport;
    return PortablePlayerBombFuncToken::Unknown;
}

PortableEffectUpdateToken CapturePortableEffectUpdateToken(EffectUpdateCallback func)
{
    if (func == nullptr)
    {
        return PortableEffectUpdateToken::None;
    }
    if (func == EffectManager::EffectCallbackRandomSplash)
        return PortableEffectUpdateToken::EffectCallbackRandomSplash;
    if (func == EffectManager::EffectCallbackRandomSplashBig)
        return PortableEffectUpdateToken::EffectCallbackRandomSplashBig;
    if (func == EffectManager::EffectCallbackStill)
        return PortableEffectUpdateToken::EffectCallbackStill;
    if (func == EffectManager::EffectUpdateCallback4)
        return PortableEffectUpdateToken::EffectUpdateCallback4;
    if (func == EffectManager::EffectCallbackAttract)
        return PortableEffectUpdateToken::EffectCallbackAttract;
    if (func == EffectManager::EffectCallbackAttractSlow)
        return PortableEffectUpdateToken::EffectCallbackAttractSlow;
    return PortableEffectUpdateToken::Unknown;
}

void CapturePortableEnemyEclContextState(const EnemyEclContext &context, PortableEnemyEclContextState &out)
{
    out.scriptOffset = PointerToOffset(context.currentInstr, g_EclManager.eclFile);
    out.time = context.time;
    out.funcToken = CapturePortableEnemyFuncToken(context.funcSetFunc);
    out.hasFuncToken = context.funcSetFunc != nullptr;
    out.hasUnknownFuncToken = out.hasFuncToken && out.funcToken == PortableEnemyFuncToken::Unknown;
    out.var0 = context.var0;
    out.var1 = context.var1;
    out.var2 = context.var2;
    out.var3 = context.var3;
    out.float0 = context.float0;
    out.float1 = context.float1;
    out.float2 = context.float2;
    out.float3 = context.float3;
    out.var4 = context.var4;
    out.var5 = context.var5;
    out.var6 = context.var6;
    out.var7 = context.var7;
    out.compareRegister = context.compareRegister;
    out.subId = context.subId;
}

void CapturePortableEnemyBulletShooterState(const EnemyBulletShooter &shooter, PortableEnemyBulletShooterState &out)
{
    out.sprite = shooter.sprite;
    out.spriteOffset = shooter.spriteOffset;
    out.position = shooter.position;
    out.angle1 = shooter.angle1;
    out.angle2 = shooter.angle2;
    out.speed1 = shooter.speed1;
    out.speed2 = shooter.speed2;
    std::copy(std::begin(shooter.exFloats), std::end(shooter.exFloats), std::begin(out.exFloats));
    std::copy(std::begin(shooter.exInts), std::end(shooter.exInts), std::begin(out.exInts));
    out.unk_40 = shooter.unk_40;
    out.count1 = shooter.count1;
    out.count2 = shooter.count2;
    out.aimMode = shooter.aimMode;
    out.unk_4a = shooter.unk_4a;
    out.flags = shooter.flags;
    out.provokedPlayer = shooter.provokedPlayer;
    out.sfx = shooter.sfx;
}

void CapturePortableEnemyLaserShooterState(const EnemyLaserShooter &shooter, PortableEnemyLaserShooterState &out)
{
    out.sprite = shooter.sprite;
    out.spriteOffset = shooter.spriteOffset;
    out.position = shooter.position;
    out.angle = shooter.angle;
    out.unk_14 = shooter.unk_14;
    out.speed = shooter.speed;
    out.unk_1c = shooter.unk_1c;
    out.startOffset = shooter.startOffset;
    out.endOffset = shooter.endOffset;
    out.startLength = shooter.startLength;
    out.width = shooter.width;
    out.startTime = shooter.startTime;
    out.duration = shooter.duration;
    out.despawnDuration = shooter.despawnDuration;
    out.hitboxStartTime = shooter.hitboxStartTime;
    out.hitboxEndDelay = shooter.hitboxEndDelay;
    out.unk_44 = shooter.unk_44;
    out.type = shooter.type;
    out.flags = shooter.flags;
    out.unk_50 = shooter.unk_50;
    out.provokedPlayer = shooter.provokedPlayer;
}

void CapturePortableEnemyState(const Enemy &enemy, PortableEnemyState &out)
{
    CapturePortableAnmVmState(enemy.primaryVm, out.primaryVm);
    for (size_t i = 0; i < out.vms.size(); ++i)
    {
        CapturePortableAnmVmState(enemy.vms[i], out.vms[i]);
    }
    CapturePortableEnemyEclContextState(enemy.currentContext, out.currentContext);
    for (size_t i = 0; i < out.savedContextStack.size(); ++i)
    {
        CapturePortableEnemyEclContextState(enemy.savedContextStack[i], out.savedContextStack[i]);
    }
    out.stackDepth = enemy.stackDepth;
    out.unk_c40 = enemy.unk_c40;
    out.deathCallbackSub = enemy.deathCallbackSub;
    std::copy(std::begin(enemy.interrupts), std::end(enemy.interrupts), std::begin(out.interrupts));
    out.runInterrupt = enemy.runInterrupt;
    out.position = enemy.position;
    out.hitboxDimensions = enemy.hitboxDimensions;
    out.axisSpeed = enemy.axisSpeed;
    out.angle = enemy.angle;
    out.angularVelocity = enemy.angularVelocity;
    out.speed = enemy.speed;
    out.acceleration = enemy.acceleration;
    out.shootOffset = enemy.shootOffset;
    out.moveInterp = enemy.moveInterp;
    out.moveInterpStartPos = enemy.moveInterpStartPos;
    out.moveInterpTimer = enemy.moveInterpTimer;
    out.moveInterpStartTime = enemy.moveInterpStartTime;
    out.bulletRankSpeedLow = enemy.bulletRankSpeedLow;
    out.bulletRankSpeedHigh = enemy.bulletRankSpeedHigh;
    out.bulletRankAmount1Low = enemy.bulletRankAmount1Low;
    out.bulletRankAmount1High = enemy.bulletRankAmount1High;
    out.bulletRankAmount2Low = enemy.bulletRankAmount2Low;
    out.bulletRankAmount2High = enemy.bulletRankAmount2High;
    out.life = enemy.life;
    out.maxLife = enemy.maxLife;
    out.score = enemy.score;
    out.bossTimer = enemy.bossTimer;
    out.color = enemy.color;
    CapturePortableEnemyBulletShooterState(enemy.bulletProps, out.bulletProps);
    out.shootInterval = enemy.shootInterval;
    out.shootIntervalTimer = enemy.shootIntervalTimer;
    CapturePortableEnemyLaserShooterState(enemy.laserProps, out.laserProps);
    for (size_t i = 0; i < out.laserIndices.size(); ++i)
    {
        out.laserIndices[i] = PointerToIndex(enemy.lasers[i], g_BulletManager.lasers, std::size(g_BulletManager.lasers));
    }
    out.laserStore = enemy.laserStore;
    out.deathAnm1 = enemy.deathAnm1;
    out.deathAnm2 = enemy.deathAnm2;
    out.deathAnm3 = enemy.deathAnm3;
    out.itemDrop = enemy.itemDrop;
    out.bossId = enemy.bossId;
    out.unk_e41 = enemy.unk_e41;
    out.exInsFunc10Timer = enemy.exInsFunc10Timer;
    out.flags = CapturePortableEnemyFlags(enemy.flags);
    out.anmExFlags = enemy.anmExFlags;
    out.anmExDefaults = enemy.anmExDefaults;
    out.anmExFarLeft = enemy.anmExFarLeft;
    out.anmExFarRight = enemy.anmExFarRight;
    out.anmExLeft = enemy.anmExLeft;
    out.anmExRight = enemy.anmExRight;
    out.lowerMoveLimit = enemy.lowerMoveLimit;
    out.upperMoveLimit = enemy.upperMoveLimit;
    for (size_t i = 0; i < out.effectIndices.size(); ++i)
    {
        out.effectIndices[i] = PointerToIndex(enemy.effectArray[i], g_EffectManager.effects, std::size(g_EffectManager.effects));
    }
    out.effectIdx = enemy.effectIdx;
    out.effectDistance = enemy.effectDistance;
    out.lifeCallbackThreshold = enemy.lifeCallbackThreshold;
    out.lifeCallbackSub = enemy.lifeCallbackSub;
    out.timerCallbackThreshold = enemy.timerCallbackThreshold;
    out.timerCallbackSub = enemy.timerCallbackSub;
    out.exInsFunc6Angle = enemy.exInsFunc6Angle;
    out.exInsFunc6Timer = enemy.exInsFunc6Timer;
    out.provokedPlayer = enemy.provokedPlayer;
}

PortableRunningSpellcardState CapturePortableRunningSpellcardState(const RunningSpellcardInfo &spellcardInfo)
{
    PortableRunningSpellcardState out;
    out.isCapturing = spellcardInfo.isCapturing != 0;
    out.isActive = spellcardInfo.isActive != 0;
    out.captureScore = spellcardInfo.captureScore;
    out.idx = spellcardInfo.idx;
    out.usedBomb = spellcardInfo.usedBomb != 0;
    return out;
}

void CapturePortableEnemyManagerState(PortableEnemyManagerState &out)
{
    out.stgEnmAnmFilename.clear();
    out.stgEnm2AnmFilename.clear();
    out.enemies.clear();
    out.stgEnmAnmFilename = g_EnemyManager.stgEnmAnmFilename != nullptr ? g_EnemyManager.stgEnmAnmFilename : "";
    out.stgEnm2AnmFilename = g_EnemyManager.stgEnm2AnmFilename != nullptr ? g_EnemyManager.stgEnm2AnmFilename : "";
    CapturePortableEnemyState(g_EnemyManager.enemyTemplate, out.enemyTemplate);
    out.enemies.reserve(std::size(g_EnemyManager.enemies));
    for (const Enemy &enemy : g_EnemyManager.enemies)
    {
        out.enemies.emplace_back();
        CapturePortableEnemyState(enemy, out.enemies.back());
    }
    for (size_t i = 0; i < out.bossIndices.size(); ++i)
    {
        out.bossIndices[i] = PointerToIndex(g_EnemyManager.bosses[i], g_EnemyManager.enemies, std::size(g_EnemyManager.enemies));
    }
    out.randomItemSpawnIndex = g_EnemyManager.randomItemSpawnIndex;
    out.randomItemTableIndex = g_EnemyManager.randomItemTableIndex;
    out.enemyCount = g_EnemyManager.enemyCount;
    out.spellcardInfo = CapturePortableRunningSpellcardState(g_EnemyManager.spellcardInfo);
    out.unk_ee5d8 = g_EnemyManager.unk_ee5d8;
    out.timelineOffset = PointerToOffset(g_EnemyManager.timelineInstr, g_EclManager.eclFile);
    out.timelineTime = g_EnemyManager.timelineTime;
}

void CapturePortablePlayerBulletState(const PlayerBullet &bullet, PortablePlayerBulletState &out)
{
    CapturePortableAnmVmState(bullet.sprite, out.sprite);
    out.position = bullet.position;
    out.size = bullet.size;
    out.velocity = bullet.velocity;
    out.sidewaysMotion = bullet.sidewaysMotion;
    out.unk_134 = bullet.unk_134;
    out.timer = bullet.unk_140;
    out.damage = bullet.damage;
    out.bulletState = bullet.bulletState;
    out.bulletType = bullet.bulletType;
    out.unk_152 = bullet.unk_152;
    out.spawnPositionIdx = bullet.spawnPositionIdx;
}

void CapturePortablePlayerBombState(const PlayerBombInfo &bombInfo, PortablePlayerBombState &out)
{
    out.isInUse = bombInfo.isInUse;
    out.duration = bombInfo.duration;
    out.timer = bombInfo.timer;
    out.calcToken = CapturePortablePlayerBombFuncToken(bombInfo.calc);
    out.drawToken = CapturePortablePlayerBombFuncToken(bombInfo.draw);
    out.hasUnknownCalcToken = bombInfo.calc != nullptr && out.calcToken == PortablePlayerBombFuncToken::Unknown;
    out.hasUnknownDrawToken = bombInfo.draw != nullptr && out.drawToken == PortablePlayerBombFuncToken::Unknown;
    std::copy(std::begin(bombInfo.reimuABombProjectilesState), std::end(bombInfo.reimuABombProjectilesState),
              std::begin(out.reimuABombProjectilesState));
    std::copy(std::begin(bombInfo.reimuABombProjectilesRelated), std::end(bombInfo.reimuABombProjectilesRelated),
              std::begin(out.reimuABombProjectilesRelated));
    std::copy(std::begin(bombInfo.bombRegionPositions), std::end(bombInfo.bombRegionPositions),
              std::begin(out.bombRegionPositions));
    std::copy(std::begin(bombInfo.bombRegionVelocities), std::end(bombInfo.bombRegionVelocities),
              std::begin(out.bombRegionVelocities));
    for (size_t ring = 0; ring < out.sprites.size(); ++ring)
    {
        for (size_t sprite = 0; sprite < out.sprites[ring].size(); ++sprite)
        {
            CapturePortableAnmVmState(bombInfo.sprites[ring][sprite], out.sprites[ring][sprite]);
        }
    }
}

void CapturePortableCharacterDataState(const CharacterData &data, PortableCharacterDataState &out)
{
    out.orthogonalMovementSpeed = data.orthogonalMovementSpeed;
    out.orthogonalMovementSpeedFocus = data.orthogonalMovementSpeedFocus;
    out.diagonalMovementSpeed = data.diagonalMovementSpeed;
    out.diagonalMovementSpeedFocus = data.diagonalMovementSpeedFocus;
    out.fireBulletToken = CapturePortablePlayerFireFuncToken(data.fireBulletCallback);
    out.fireBulletFocusToken = CapturePortablePlayerFireFuncToken(data.fireBulletFocusCallback);
    out.hasUnknownFireToken =
        data.fireBulletCallback != nullptr && out.fireBulletToken == PortablePlayerFireFuncToken::Unknown;
    out.hasUnknownFocusFireToken = data.fireBulletFocusCallback != nullptr &&
                                   out.fireBulletFocusToken == PortablePlayerFireFuncToken::Unknown;
}

void CapturePortablePlayerState(const Player &player, bool isPresent, PortablePlayerState &out)
{
    out.isPresent = isPresent;
    CapturePortableAnmVmState(player.playerSprite, out.playerSprite);
    for (size_t i = 0; i < out.orbsSprite.size(); ++i)
    {
        CapturePortableAnmVmState(player.orbsSprite[i], out.orbsSprite[i]);
    }
    out.positionCenter = player.positionCenter;
    out.unk_44c = player.unk_44c;
    out.hitboxTopLeft = player.hitboxTopLeft;
    out.hitboxBottomRight = player.hitboxBottomRight;
    out.grabItemTopLeft = player.grabItemTopLeft;
    out.grabItemBottomRight = player.grabItemBottomRight;
    out.hitboxSize = player.hitboxSize;
    out.grabItemSize = player.grabItemSize;
    std::copy(std::begin(player.orbsPosition), std::end(player.orbsPosition), std::begin(out.orbsPosition));
    std::copy(std::begin(player.bombRegionPositions), std::end(player.bombRegionPositions),
              std::begin(out.bombRegionPositions));
    std::copy(std::begin(player.bombRegionSizes), std::end(player.bombRegionSizes), std::begin(out.bombRegionSizes));
    std::copy(std::begin(player.bombRegionDamages), std::end(player.bombRegionDamages),
              std::begin(out.bombRegionDamages));
    std::copy(std::begin(player.unk_838), std::end(player.unk_838), std::begin(out.unk_838));
    std::copy(std::begin(player.bombProjectiles), std::end(player.bombProjectiles), std::begin(out.bombProjectiles));
    std::copy(std::begin(player.laserTimer), std::end(player.laserTimer), std::begin(out.laserTimer));
    out.horizontalMovementSpeedMultiplierDuringBomb = player.horizontalMovementSpeedMultiplierDuringBomb;
    out.verticalMovementSpeedMultiplierDuringBomb = player.verticalMovementSpeedMultiplierDuringBomb;
    out.respawnTimer = player.respawnTimer;
    out.bulletGracePeriod = player.bulletGracePeriod;
    out.playerState = player.playerState;
    out.playerType = player.playerType;
    out.unk_9e1 = player.unk_9e1;
    out.orbState = player.orbState;
    out.isFocus = player.isFocus;
    out.unk_9e4 = player.unk_9e4;
    out.focusMovementTimer = player.focusMovementTimer;
    CapturePortableCharacterDataState(player.characterData, out.characterData);
    out.playerDirection = (i32)player.playerDirection;
    out.previousHorizontalSpeed = player.previousHorizontalSpeed;
    out.previousVerticalSpeed = player.previousVerticalSpeed;
    out.previousFrameInput = player.previousFrameInput;
    out.positionOfLastEnemyHit = player.positionOfLastEnemyHit;
    for (size_t i = 0; i < out.bullets.size(); ++i)
    {
        CapturePortablePlayerBulletState(player.bullets[i], out.bullets[i]);
    }
    out.fireBulletTimer = player.fireBulletTimer;
    out.invulnerabilityTimer = player.invulnerabilityTimer;
    out.fireBulletToken = CapturePortablePlayerFireFuncToken(player.fireBulletCallback);
    out.fireBulletFocusToken = CapturePortablePlayerFireFuncToken(player.fireBulletFocusCallback);
    out.hasUnknownFireToken =
        player.fireBulletCallback != nullptr && out.fireBulletToken == PortablePlayerFireFuncToken::Unknown;
    out.hasUnknownFocusFireToken = player.fireBulletFocusCallback != nullptr &&
                                   out.fireBulletFocusToken == PortablePlayerFireFuncToken::Unknown;
    CapturePortablePlayerBombState(player.bombInfo, out.bombInfo);
    CapturePortableAnmVmState(player.hitboxSprite, out.hitboxSprite);
    out.hitboxTime = player.hitboxTime;
    out.lifegiveTime = player.lifegiveTime;
}

void CapturePortableBulletTypeSpritesState(const BulletTypeSprites &sprites, PortableBulletTypeSpritesState &out)
{
    CapturePortableAnmVmState(sprites.spriteBullet, out.spriteBullet);
    CapturePortableAnmVmState(sprites.spriteSpawnEffectFast, out.spriteSpawnEffectFast);
    CapturePortableAnmVmState(sprites.spriteSpawnEffectNormal, out.spriteSpawnEffectNormal);
    CapturePortableAnmVmState(sprites.spriteSpawnEffectSlow, out.spriteSpawnEffectSlow);
    CapturePortableAnmVmState(sprites.spriteSpawnEffectDonut, out.spriteSpawnEffectDonut);
    out.grazeSize = sprites.grazeSize;
    out.unk_55c = sprites.unk_55c;
    out.bulletHeight = sprites.bulletHeight;
}

void CapturePortableBulletState(const Bullet &bullet, PortableBulletState &out)
{
    CapturePortableBulletTypeSpritesState(bullet.sprites, out.sprites);
    out.pos = bullet.pos;
    out.velocity = bullet.velocity;
    out.ex4Acceleration = bullet.ex4Acceleration;
    out.speed = bullet.speed;
    out.ex5Float0 = bullet.ex5Float0;
    out.dirChangeSpeed = bullet.dirChangeSpeed;
    out.angle = bullet.angle;
    out.ex5Float1 = bullet.ex5Float1;
    out.dirChangeRotation = bullet.dirChangeRotation;
    out.timer = bullet.timer;
    out.ex5Int0 = bullet.ex5Int0;
    out.dirChangeInterval = bullet.dirChangeInterval;
    out.dirChangeNumTimes = bullet.dirChangeNumTimes;
    out.dirChangeMaxTimes = bullet.dirChangeMaxTimes;
    out.exFlags = bullet.exFlags;
    out.spriteOffset = bullet.spriteOffset;
    out.unk_5bc = bullet.unk_5bc;
    out.state = bullet.state;
    out.unk_5c0 = bullet.unk_5c0;
    out.unk_5c2 = bullet.unk_5c2;
    out.isGrazed = bullet.isGrazed;
    out.provokedPlayer = bullet.provokedPlayer;
}

void CapturePortableLaserState(const Laser &laser, PortableLaserState &out)
{
    CapturePortableAnmVmState(laser.vm0, out.vm0);
    CapturePortableAnmVmState(laser.vm1, out.vm1);
    out.pos = laser.pos;
    out.angle = laser.angle;
    out.startOffset = laser.startOffset;
    out.endOffset = laser.endOffset;
    out.startLength = laser.startLength;
    out.width = laser.width;
    out.speed = laser.speed;
    out.startTime = laser.startTime;
    out.hitboxStartTime = laser.hitboxStartTime;
    out.duration = laser.duration;
    out.despawnDuration = laser.despawnDuration;
    out.hitboxEndDelay = laser.hitboxEndDelay;
    out.inUse = laser.inUse;
    out.timer = laser.timer;
    out.flags = laser.flags;
    out.color = laser.color;
    out.state = laser.state;
    out.provokedPlayer = laser.provokedPlayer;
}

void CapturePortableBulletManagerState(PortableBulletManagerState &out)
{
    out.bulletAnmPath = g_BulletManager.bulletAnmPath != nullptr ? g_BulletManager.bulletAnmPath : "";
    for (size_t i = 0; i < out.bulletTypeTemplates.size(); ++i)
    {
        CapturePortableBulletTypeSpritesState(g_BulletManager.bulletTypeTemplates[i], out.bulletTypeTemplates[i]);
    }
    for (size_t i = 0; i < out.bullets.size(); ++i)
    {
        CapturePortableBulletState(g_BulletManager.bullets[i], out.bullets[i]);
    }
    for (size_t i = 0; i < out.lasers.size(); ++i)
    {
        CapturePortableLaserState(g_BulletManager.lasers[i], out.lasers[i]);
    }
    out.nextBulletIndex = g_BulletManager.nextBulletIndex;
    out.bulletCount = g_BulletManager.bulletCount;
    out.time = g_BulletManager.time;
}

void CapturePortableItemState(const Item &item, PortableItemState &out)
{
    CapturePortableAnmVmState(item.sprite, out.sprite);
    out.currentPosition = item.currentPosition;
    out.startPosition = item.startPosition;
    out.targetPosition = item.targetPosition;
    out.timer = item.timer;
    out.itemType = item.itemType;
    out.isInUse = item.isInUse;
    out.unk_142 = item.unk_142;
    out.state = item.state;
}

void CapturePortableItemManagerState(PortableItemManagerState &out)
{
    for (size_t i = 0; i < out.items.size(); ++i)
    {
        CapturePortableItemState(g_ItemManager.items[i], out.items[i]);
    }
    out.nextIndex = g_ItemManager.nextIndex;
    out.itemCount = g_ItemManager.itemCount;
}

void CapturePortableEffectState(const Effect &effect, PortableEffectState &out)
{
    CapturePortableAnmVmState(effect.vm, out.vm);
    out.pos1 = effect.pos1;
    out.unk_11c = effect.unk_11c;
    out.unk_128 = effect.unk_128;
    out.position = effect.position;
    out.pos2 = effect.pos2;
    out.quaternion = effect.quaternion;
    out.unk_15c = effect.unk_15c;
    out.angleRelated = effect.angleRelated;
    out.timer = effect.timer;
    out.unk_170 = effect.unk_170;
    out.updateCallbackToken = CapturePortableEffectUpdateToken(effect.updateCallback);
    out.hasUnknownUpdateToken =
        effect.updateCallback != nullptr && out.updateCallbackToken == PortableEffectUpdateToken::Unknown;
    out.inUseFlag = effect.inUseFlag;
    out.effectId = effect.effectId;
    out.unk_17a = effect.unk_17a;
    out.unk_17b = effect.unk_17b;
}

void CapturePortableEffectManagerState(PortableEffectManagerState &out)
{
    out.nextIndex = g_EffectManager.nextIndex;
    out.activeEffects = g_EffectManager.activeEffects;
    for (size_t i = 0; i < out.effects.size(); ++i)
    {
        CapturePortableEffectState(g_EffectManager.effects[i], out.effects[i]);
    }
}

void CapturePortableEclScriptState(PortableEclScriptState &out)
{
    out.subTableEntryOffsets.clear();
    out.hasEclFile = g_EclManager.eclFile != nullptr;
    out.timelineOffset = PointerToOffset(g_EclManager.timeline, g_EclManager.eclFile);
    if (g_EclManager.eclFile != nullptr)
    {
        out.subTableEntryOffsets.reserve((size_t)g_EclManager.eclFile->subCount);
        for (int i = 0; i < g_EclManager.eclFile->subCount; ++i)
        {
            out.subTableEntryOffsets.push_back(PointerToOffset(g_EclManager.subTable[i], g_EclManager.eclFile));
        }
    }
}

void CapturePortableEclManagerCoreState(PortableEclManagerCoreState &out)
{
    out.subTableEntryOffsets.clear();
    out.hasEclFile = g_EclManager.eclFile != nullptr;
    out.resource.resourcePath.clear();
    out.resource.resourceContentHash = 0;
    out.resource.resourceSizeBytes = 0;
    out.subCount = 0;
    out.mainCount = 0;
    out.timelineOffsets = {{-1, -1, -1}};
    out.activeTimelineSlot = -1;
    out.activeTimelineOffset = PointerToOffset(g_EclManager.timeline, g_EclManager.eclFile);

    const char *resourcePath = ResolvePortableEclPathForStage(g_GameManager.currentStage);
    if (resourcePath != nullptr)
    {
        out.resource.resourcePath = resourcePath;
    }

    std::vector<u8> rawBytes;
    if (!out.hasEclFile || resourcePath == nullptr || !ReadPortableResourceBytes(resourcePath, rawBytes))
    {
        return;
    }

    out.resource.resourceSizeBytes = (u32)rawBytes.size();
    out.resource.resourceContentHash = HashByteSpan(rawBytes.data(), rawBytes.size());

    if (rawBytes.size() < sizeof(EclRawHeader))
    {
        return;
    }

    const auto *rawHeader = reinterpret_cast<const EclRawHeader *>(rawBytes.data());
    out.subCount = rawHeader->subCount;
    out.mainCount = rawHeader->mainCount;
    for (size_t i = 0; i < out.timelineOffsets.size(); ++i)
    {
        out.timelineOffsets[i] = RawPointerFieldToOffset(rawHeader->timelineOffsets[i]);
        if (out.timelineOffsets[i] == out.activeTimelineOffset)
        {
            out.activeTimelineSlot = (i32)i;
        }
    }

    out.subTableEntryOffsets.reserve((size_t)std::max(out.subCount, 0));
    for (i32 i = 0; i < out.subCount; ++i)
    {
        out.subTableEntryOffsets.push_back(RawPointerFieldToOffset(rawHeader->subOffsets[i]));
    }
}

void CapturePortableStageCoreState(PortableStageCoreState &out)
{
    out.hasStageData = g_Stage.stdData != nullptr;
    out.resource.stdPath.clear();
    out.resource.stdContentHash = 0;
    out.resource.stdSizeBytes = 0;
    out.resource.anmPath.clear();
    out.resource.anmContentHash = 0;
    out.resource.anmSizeBytes = 0;
    out.stage = g_Stage.stage;
    out.objectsCount = g_Stage.objectsCount;
    out.quadCount = g_Stage.quadCount;
    out.scriptTime = g_Stage.scriptTime;
    out.instructionIndex = g_Stage.instructionIndex;
    out.timer = g_Stage.timer;
    out.position = g_Stage.position;
    out.skyFog = g_Stage.skyFog;
    out.skyFogInterpInitial = g_Stage.skyFogInterpInitial;
    out.skyFogInterpFinal = g_Stage.skyFogInterpFinal;
    out.skyFogInterpDuration = g_Stage.skyFogInterpDuration;
    out.skyFogInterpTimer = g_Stage.skyFogInterpTimer;
    out.skyFogNeedsSetup = g_Stage.skyFogNeedsSetup;
    out.spellcardState = g_Stage.spellcardState;
    out.ticksSinceSpellcardStarted = g_Stage.ticksSinceSpellcardStarted;
    out.unpauseFlag = g_Stage.unpauseFlag;
    out.facingDirInterpInitial = g_Stage.facingDirInterpInitial;
    out.facingDirInterpFinal = g_Stage.facingDirInterpFinal;
    out.facingDirInterpDuration = g_Stage.facingDirInterpDuration;
    out.facingDirInterpTimer = g_Stage.facingDirInterpTimer;
    out.positionInterpFinal = g_Stage.positionInterpFinal;
    out.positionInterpEndTime = g_Stage.positionInterpEndTime;
    out.positionInterpInitial = g_Stage.positionInterpInitial;
    out.positionInterpStartTime = g_Stage.positionInterpStartTime;
    out.currentCameraFacingDir = g_GameManager.stageCameraFacingDir;
    out.objectFlags.clear();
    out.quadVms.clear();
    CapturePortableAnmVmState(g_Stage.spellcardBackground, out.spellcardBackground);
    CapturePortableAnmVmState(g_Stage.unk2, out.extraBackground);

    const char *stdPath = ResolvePortableStageStdPathForStage(g_GameManager.currentStage);
    const char *anmPath = ResolvePortableStageAnmPathForStage(g_GameManager.currentStage);
    if (stdPath != nullptr)
    {
        out.resource.stdPath = stdPath;
    }
    if (anmPath != nullptr)
    {
        out.resource.anmPath = anmPath;
    }

    std::vector<u8> stdBytes;
    if (stdPath != nullptr && ReadPortableResourceBytes(stdPath, stdBytes))
    {
        out.resource.stdSizeBytes = (u32)stdBytes.size();
        out.resource.stdContentHash = HashByteSpan(stdBytes.data(), stdBytes.size());
    }

    std::vector<u8> anmBytes;
    if (anmPath != nullptr && ReadPortableResourceBytes(anmPath, anmBytes))
    {
        out.resource.anmSizeBytes = (u32)anmBytes.size();
        out.resource.anmContentHash = HashByteSpan(anmBytes.data(), anmBytes.size());
    }

    if (g_Stage.objects != nullptr && g_Stage.objectsCount > 0)
    {
        out.objectFlags.reserve((size_t)g_Stage.objectsCount);
        for (i32 i = 0; i < g_Stage.objectsCount; ++i)
        {
            out.objectFlags.push_back((u8)g_Stage.objects[i]->flags);
        }
    }
    if (g_Stage.quadVms != nullptr && g_Stage.quadCount > 0)
    {
        out.quadVms.resize((size_t)g_Stage.quadCount);
        for (i32 i = 0; i < g_Stage.quadCount; ++i)
        {
            CapturePortableAnmVmState(g_Stage.quadVms[i], out.quadVms[(size_t)i]);
        }
    }
}

void CapturePortableShellSyncState(PortableShellSyncState &out)
{
    out.bgmTrackIndex = THPrac::TH06::THPortableGetCurrentBgmTrackIndex();
    out.bossAssetProfile = (PortableBossAssetProfile)THPrac::TH06::THPortableGetCurrentBossAssetProfile();
    out.hideStageNameIntro = g_Gui.impl != nullptr ? IsPortableIntroSpriteHidden(g_Gui.impl->stageNameSprite) : false;
    out.hideSongNameIntro = g_Gui.impl != nullptr ? IsPortableIntroSpriteHidden(g_Gui.impl->songNameSprite) : false;
}

void CapturePortableGuiState(PortableGuiState &out)
{
    out.hasGuiImpl = g_Gui.impl != nullptr;
    out.flag0 = g_Gui.flags.flag0;
    out.flag1 = g_Gui.flags.flag1;
    out.flag2 = g_Gui.flags.flag2;
    out.flag3 = g_Gui.flags.flag3;
    out.flag4 = g_Gui.flags.flag4;
    out.bossPresent = g_Gui.bossPresent;
    out.bossUIOpacity = g_Gui.bossUIOpacity;
    out.eclSetLives = g_Gui.eclSetLives;
    out.spellcardSecondsRemaining = g_Gui.spellcardSecondsRemaining;
    out.lastSpellcardSecondsRemaining = g_Gui.lastSpellcardSecondsRemaining;
    out.bossHealthBar1 = g_Gui.bossHealthBar1;
    out.bossHealthBar2 = g_Gui.bossHealthBar2;
    out.bombSpellcardBarLength = g_Gui.bombSpellcardBarLength;
    out.blueSpellcardBarLength = g_Gui.blueSpellcardBarLength;

    if (g_Gui.impl == nullptr)
    {
        return;
    }

    out.bossHealthBarState = g_Gui.impl->bossHealthBarState;
    CapturePortableAnmVmState(g_Gui.impl->playerSpellcardPortrait, out.playerSpellcardPortrait);
    CapturePortableAnmVmState(g_Gui.impl->enemySpellcardPortrait, out.enemySpellcardPortrait);
    CapturePortableAnmVmState(g_Gui.impl->bombSpellcardName, out.bombSpellcardName);
    CapturePortableAnmVmState(g_Gui.impl->enemySpellcardName, out.enemySpellcardName);
    CapturePortableAnmVmState(g_Gui.impl->bombSpellcardBackground, out.bombSpellcardBackground);
    CapturePortableAnmVmState(g_Gui.impl->enemySpellcardBackground, out.enemySpellcardBackground);
    CapturePortableAnmVmState(g_Gui.impl->loadingScreenSprite, out.loadingScreenSprite);

    if (g_Gui.impl->enemySpellcardName.flags.isVisible && g_EnemyManager.spellcardInfo.idx < CATK_NUM_CAPTURES)
    {
        out.enemySpellcardText = g_GameManager.catk[g_EnemyManager.spellcardInfo.idx].name;
    }
    else
    {
        out.enemySpellcardText.clear();
    }

    if (g_Gui.impl->bombSpellcardName.flags.isVisible)
    {
        const char *bombName = ResolvePortableBombSpellcardName();
        out.bombSpellcardText = bombName != nullptr ? bombName : "";
    }
    else
    {
        out.bombSpellcardText.clear();
    }
}

void AppendUnique(std::vector<std::string> &entries, const std::string &value)
{
    if (std::find(entries.begin(), entries.end(), value) == entries.end())
    {
        entries.push_back(value);
    }
}
} // namespace

void CapturePortableGameplayState(PortableGameplayState &state)
{
    TracePortableValidationPhase("capture-dgs-start");
    auto dgsState = std::make_unique<DeterministicGameplayState>();
    CaptureDeterministicGameplayState(*dgsState);
    TracePortableValidationPhase("capture-dgs-end");

    state.header.magic = kPortableGameplayMagic;
    state.header.version = (u32)PortableGameplaySchemaVersion::V6;
    state.header.sectionCount = 15;
    state.header.totalBytes = 0;

    state.captureFlags = PortableCaptureFlag_HasResourceCatalog | PortableCaptureFlag_HasExplicitCore |
                         PortableCaptureFlag_HasExplicitRuntime | PortableCaptureFlag_HasStageRefs |
                         PortableCaptureFlag_HasEnemyRefs | PortableCaptureFlag_HasEclRefs |
                         PortableCaptureFlag_HasExplicitEclCore | PortableCaptureFlag_HasExplicitStageCore |
                         PortableCaptureFlag_HasShadowFingerprints | PortableCaptureFlag_HasExplicitActors |
                         PortableCaptureFlag_HasIndependentRestore | PortableCaptureFlag_HasExplicitPlayers |
                         PortableCaptureFlag_HasExplicitBullets | PortableCaptureFlag_HasExplicitItems |
                         PortableCaptureFlag_HasExplicitEffects | PortableCaptureFlag_HasShellSyncHints |
                         PortableCaptureFlag_HasExplicitGui | PortableCaptureFlag_HasExplicitRng |
                         PortableCaptureFlag_HasExplicitCatk;

    TracePortableValidationPhase("capture-catalog");
    state.catalog = CapturePortableGameplayResourceCatalog();
    TracePortableValidationPhase("capture-core");
    state.core = CapturePortableGameplayCoreState();
    TracePortableValidationPhase("capture-runtime");
    CapturePortableGameplayRuntimeState(*dgsState, state.runtime);
    state.rng.seed = g_Rng.seed;
    state.rng.generationCount = g_Rng.generationCount;
    std::memcpy(state.catk.data(), g_GameManager.catk, sizeof(g_GameManager.catk));
    TracePortableValidationPhase("capture-stage-refs");
    state.stageRefs = dgsState->stageRefs;
    TracePortableValidationPhase("capture-enemy-refs");
    state.enemyRefs = dgsState->enemyManagerRefs;
    TracePortableValidationPhase("capture-ecl-scripts");
    CapturePortableEclScriptState(state.eclScripts);
    TracePortableValidationPhase("capture-ecl-core");
    CapturePortableEclManagerCoreState(state.eclCore);
    TracePortableValidationPhase("capture-stage-core");
    CapturePortableStageCoreState(state.stageCore);
    TracePortableValidationPhase("capture-shell-sync");
    CapturePortableShellSyncState(state.shellSync);
    TracePortableValidationPhase("capture-gui");
    CapturePortableGuiState(state.gui);
    TracePortableValidationPhase("capture-enemy-actors");
    CapturePortableEnemyManagerState(state.enemyActors);
    TracePortableValidationPhase("capture-player-actors");
    CapturePortablePlayerState(g_Player, true, state.players[0]);
    CapturePortablePlayerState(g_Player2, Session::IsDualPlayerSession(), state.players[1]);
    TracePortableValidationPhase("capture-bullet-actors");
    CapturePortableBulletManagerState(state.bulletActors);
    TracePortableValidationPhase("capture-item-actors");
    CapturePortableItemManagerState(state.itemActors);
    TracePortableValidationPhase("capture-effect-actors");
    CapturePortableEffectManagerState(state.effectActors);
    TracePortableValidationPhase("capture-shadow-fingerprints");
    state.shadowFingerprints.dgs = FingerprintDeterministicGameplaySubsystems(*dgsState);

    state.mustNormalizeBeforeRestore.clear();
    for (const std::string &entry : dgsState->unresolvedFields)
    {
        if (entry.find("EnemyEclContext.funcSetFunc") != std::string::npos ||
            entry.find("EnemyManager enemyTemplate") != std::string::npos ||
            entry.find("EclManager subTable") != std::string::npos ||
            entry.find("Player.calc/draw/bullets/chain pointers") != std::string::npos ||
            entry.find("BulletManager laser AnmVm script pointers") != std::string::npos ||
            entry.find("EffectManager effect update callbacks") != std::string::npos)
        {
            continue;
        }
        AppendUnique(state.mustNormalizeBeforeRestore, entry);
    }
    for (const auto &player : state.players)
    {
        if (!player.isPresent)
        {
            continue;
        }
        if (player.characterData.hasUnknownFireToken || player.characterData.hasUnknownFocusFireToken ||
            player.hasUnknownFireToken || player.hasUnknownFocusFireToken)
        {
            AppendUnique(state.mustNormalizeBeforeRestore,
                         "Portable player fire-bullet callbacks still include unknown tokens.");
        }
        if (player.bombInfo.hasUnknownCalcToken || player.bombInfo.hasUnknownDrawToken)
        {
            AppendUnique(state.mustNormalizeBeforeRestore,
                         "Portable player bomb callbacks still include unknown tokens.");
        }
    }
    for (const auto &effect : state.effectActors.effects)
    {
        if (effect.hasUnknownUpdateToken)
        {
            AppendUnique(state.mustNormalizeBeforeRestore,
                         "Portable effect update callbacks still include unknown tokens.");
            break;
        }
    }
    if (state.shellSync.bgmTrackIndex < 0 || state.shellSync.bgmTrackIndex > 1)
    {
        AppendUnique(state.mustNormalizeBeforeRestore,
                     "Portable shell sync hints are missing a valid BGM track index.");
    }
    if (state.shellSync.bossAssetProfile != PortableBossAssetProfile::None &&
        state.shellSync.bossAssetProfile != PortableBossAssetProfile::Stage6BossEffects &&
        state.shellSync.bossAssetProfile != PortableBossAssetProfile::Stage7EndEffects)
    {
        AppendUnique(state.mustNormalizeBeforeRestore,
                     "Portable shell sync hints include an unknown boss asset profile.");
    }
    state.excludedByDesign.clear();
    state.excludedByDesign.emplace_back("MainMenu / OnlineMenu");
    state.excludedByDesign.emplace_back("Gui dialogue/result presentation state");
    state.excludedByDesign.emplace_back("ED / Staff / Result");
    state.excludedByDesign.emplace_back("Renderer / GLES / screenshot textures");
    state.excludedByDesign.emplace_back("Window / input raw device state");
    state.excludedByDesign.emplace_back("Relay / socket / debug network");
    state.excludedByDesign.emplace_back("Chain callback graph");
    state.excludedByDesign.emplace_back("SoundPlayer device / thread / handles");
    state.excludedByDesign.emplace_back("Input-based replay stream");
}

const char *PortableGameplaySectionIdToString(PortableGameplaySectionId id)
{
    switch (id)
    {
    case PortableGameplaySectionId::Envelope:
        return "envelope";
    case PortableGameplaySectionId::ResourceCatalog:
        return "resource_catalog";
    case PortableGameplaySectionId::Core:
        return "core";
    case PortableGameplaySectionId::Runtime:
        return "runtime";
    case PortableGameplaySectionId::StageRefs:
        return "stage_refs";
    case PortableGameplaySectionId::EnemyRefs:
        return "enemy_refs";
    case PortableGameplaySectionId::EclRefs:
        return "ecl_refs";
    case PortableGameplaySectionId::EclCore:
        return "ecl_core";
    case PortableGameplaySectionId::StageCore:
        return "stage_core";
    case PortableGameplaySectionId::ShellSync:
        return "shell_sync";
    case PortableGameplaySectionId::GuiState:
        return "gui_state";
    case PortableGameplaySectionId::RngState:
        return "rng_state";
    case PortableGameplaySectionId::CatkState:
        return "catk_state";
    case PortableGameplaySectionId::Actors:
        return "actors";
    case PortableGameplaySectionId::Fingerprints:
        return "fingerprints";
    case PortableGameplaySectionId::Audit:
        return "audit";
    default:
        return "unknown";
    }
}

const char *PortableRestoreReadinessToString(PortableRestoreReadiness readiness)
{
    switch (readiness)
    {
    case PortableRestoreReadiness::ShadowOnly:
        return "shadow-only";
    case PortableRestoreReadiness::SchemaMismatch:
        return "schema-mismatch";
    case PortableRestoreReadiness::CatalogMismatch:
        return "catalog-mismatch";
    case PortableRestoreReadiness::NeedsNormalization:
        return "needs-normalization";
    case PortableRestoreReadiness::NotBuildableYet:
        return "not-buildable-yet";
    case PortableRestoreReadiness::Ready:
        return "ready";
    default:
        return "unknown";
    }
}
} // namespace th06::DGS
