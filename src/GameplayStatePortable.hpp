#pragma once

#include "GameplayState.hpp"

#include <string>
#include <vector>

namespace th06::DGS
{
constexpr u32 kPortableGameplayMagic = 0x31534750u; // "PGS1"

enum class PortableGameplaySchemaVersion : u32
{
    V1 = 1,
    V2 = 2,
    V3 = 3,
    V4 = 4,
    V5 = 5,
    V6 = 6,
};

enum class PortableGameplaySectionId : u16
{
    Envelope = 0,
    ResourceCatalog = 1,
    Core = 2,
    Runtime = 3,
    StageRefs = 4,
    EnemyRefs = 5,
    EclRefs = 6,
    Actors = 7,
    Fingerprints = 8,
    Audit = 9,
    EclCore = 10,
    StageCore = 11,
    ShellSync = 12,
    GuiState = 13,
    RngState = 14,
    CatkState = 15,
};

enum PortableGameplayCaptureFlags : u32
{
    PortableCaptureFlag_None = 0,
    PortableCaptureFlag_HasResourceCatalog = 1u << 0,
    PortableCaptureFlag_HasExplicitCore = 1u << 1,
    PortableCaptureFlag_HasExplicitRuntime = 1u << 2,
    PortableCaptureFlag_HasStageRefs = 1u << 3,
    PortableCaptureFlag_HasEnemyRefs = 1u << 4,
    PortableCaptureFlag_HasEclRefs = 1u << 5,
    PortableCaptureFlag_HasShadowFingerprints = 1u << 6,
    PortableCaptureFlag_HasExplicitActors = 1u << 7,
    PortableCaptureFlag_HasIndependentRestore = 1u << 8,
    PortableCaptureFlag_HasExplicitPlayers = 1u << 9,
    PortableCaptureFlag_HasExplicitBullets = 1u << 10,
    PortableCaptureFlag_HasExplicitItems = 1u << 11,
    PortableCaptureFlag_HasExplicitEffects = 1u << 12,
    PortableCaptureFlag_HasExplicitEclCore = 1u << 13,
    PortableCaptureFlag_HasExplicitStageCore = 1u << 14,
    PortableCaptureFlag_HasShellSyncHints = 1u << 15,
    PortableCaptureFlag_HasExplicitGui = 1u << 16,
    PortableCaptureFlag_HasExplicitRng = 1u << 17,
    PortableCaptureFlag_HasExplicitCatk = 1u << 18,
};

inline bool HasPortableCaptureFlag(u32 flags, PortableGameplayCaptureFlags flag)
{
    return (flags & (u32)flag) != 0;
}

enum class PortableRestoreReadiness : u32
{
    ShadowOnly = 0,
    SchemaMismatch = 1,
    CatalogMismatch = 2,
    NeedsNormalization = 3,
    NotBuildableYet = 4,
    Ready = 5
};

struct PortableGameplayEnvelopeHeader
{
    u32 magic = kPortableGameplayMagic;
    u32 version = (u32)PortableGameplaySchemaVersion::V6;
    u32 sectionCount = 0;
    u32 totalBytes = 0;
};

struct PortableGameplaySectionHeader
{
    u16 id = 0;
    u16 version = 1;
    u32 sizeBytes = 0;
};

struct PortableGameplayResourceCatalog
{
    u64 catalogHash = 0;
    u32 currentStage = 0;
    u32 difficulty = 0;
    u32 character1 = 0;
    u32 shotType1 = 0;
    u32 character2 = 0;
    u32 shotType2 = 0;
    u32 hasSecondPlayer = 0;
    u32 isPracticeMode = 0;
    u32 isReplay = 0;
};

struct PortableGameplayCoreState
{
    u32 guiScore = 0;
    u32 score = 0;
    u32 nextScoreIncrement = 0;
    u32 highScore = 0;
    u32 difficulty = 0;
    i32 grazeInStage = 0;
    i32 grazeInTotal = 0;
    i32 deaths = 0;
    i32 bombsUsed = 0;
    i32 spellcardsCaptured = 0;
    i32 pointItemsCollectedInStage = 0;
    i32 pointItemsCollected = 0;
    i32 powerItemCountForScore = 0;
    i32 extraLives = 0;
    i32 currentStage = 0;
    i32 rank = 0;
    i32 maxRank = 0;
    i32 minRank = 0;
    i32 subRank = 0;
    u32 randomSeed = 0;
    u32 gameFrames = 0;
    u16 currentPower1 = 0;
    u16 currentPower2 = 0;
    i16 livesRemaining1 = 0;
    i16 bombsRemaining1 = 0;
    i16 livesRemaining2 = 0;
    i16 bombsRemaining2 = 0;
    u16 numRetries = 0;
    u16 isTimeStopped = 0;
    u16 isGameCompleted = 0;
    u16 isPracticeMode = 0;
    u16 demoMode = 0;
    u8 character1 = 0;
    u8 shotType1 = 0;
    u8 character2 = 0;
    u8 shotType2 = 0;
};

struct PortableGameplayRuntimeState
{
    i32 frame = 0;
    i32 stage = 0;
    i32 delay = 0;
    i32 currentDelayCooldown = 0;
    EnemyEclInstr::RuntimeState enemyEclRuntimeState {};
    ScreenEffect::RuntimeState screenEffectRuntimeState {};
    Controller::RuntimeState controllerRuntimeState {};
    DgsSupervisorRuntimeState supervisorRuntimeState {};
    DgsGameWindowRuntimeState gameWindowRuntimeState {};
    DgsSoundRuntimeState soundRuntimeState {};
    DgsInputRuntimeState inputRuntimeState {};
    std::vector<RawStageObjectInstance> stageObjectInstances;
};

struct PortableRngState
{
    u16 seed = 0;
    u32 generationCount = 0;
};

struct AuthorityGameplayFingerprint
{
    u64 allHash = 0;
    u64 gameHash = 0;
    u64 player1Hash = 0;
    u64 player2Hash = 0;
    u64 bulletHash = 0;
    u64 enemyHash = 0;
    u64 itemHash = 0;
    u64 effectHash = 0;
    u64 stageHash = 0;
    u64 eclHash = 0;
    u64 enemyEclHash = 0;
    u64 screenHash = 0;
    u64 inputHash = 0;
    u64 catkHash = 0;
    u64 rngHash = 0;
};

enum class PortableEnemyFuncToken : u16
{
    None = 0,
    ExInsCirnoRainbowBallJank = 1,
    ExInsShootAtRandomArea = 2,
    ExInsShootStarPattern = 3,
    ExInsPatchouliShottypeSetVars = 4,
    ExInsStage56Func4 = 5,
    ExInsStage5Func5 = 6,
    ExInsStage6XFunc6 = 7,
    ExInsStage6Func7 = 8,
    ExInsStage6Func8 = 9,
    ExInsStage6Func9 = 10,
    ExInsStage6XFunc10 = 11,
    ExInsStage6Func11 = 12,
    ExInsStage4Func12 = 13,
    ExInsStageXFunc13 = 14,
    ExInsStageXFunc14 = 15,
    ExInsStageXFunc15 = 16,
    ExInsStageXFunc16 = 17,
    Unknown = 0xffff,
};

enum class PortablePlayerFireFuncToken : u16
{
    None = 0,
    FireBulletReimuA = 1,
    FireBulletReimuB = 2,
    FireBulletMarisaA = 3,
    FireBulletMarisaB = 4,
    Unknown = 0xffff,
};

enum class PortablePlayerBombFuncToken : u16
{
    None = 0,
    BombReimuACalc = 1,
    BombReimuBCalc = 2,
    BombMarisaACalc = 3,
    BombMarisaBCalc = 4,
    BombReimuADraw = 5,
    BombReimuBDraw = 6,
    BombMarisaADraw = 7,
    BombMarisaBDraw = 8,
    DarkenViewport = 9,
    Unknown = 0xffff,
};

enum class PortableEffectUpdateToken : u16
{
    None = 0,
    EffectCallbackRandomSplash = 1,
    EffectCallbackRandomSplashBig = 2,
    EffectCallbackStill = 3,
    EffectUpdateCallback4 = 4,
    EffectCallbackAttract = 5,
    EffectCallbackAttractSlow = 6,
    Unknown = 0xffff,
};

enum class PortableBossAssetProfile : u32
{
    None = 0,
    Stage6BossEffects = 1,
    Stage7EndEffects = 2,
};

struct PortableEnemyFlagsState
{
    u8 unk1 = 0;
    u8 unk2 = 0;
    u8 unk3 = 0;
    u8 unk4 = 0;
    u8 unk5 = 0;
    u8 unk6 = 0;
    u8 unk7 = 0;
    u8 unk8 = 0;
    u8 isBoss = 0;
    u8 unk10 = 0;
    u8 unk11 = 0;
    u8 shouldClampPos = 0;
    u8 unk13 = 0;
    u8 unk14 = 0;
    u8 unk15 = 0;
    u8 unk16 = 0;
};

struct PortableAnmVmState
{
    D3DXVECTOR3 rotation {};
    D3DXVECTOR3 angleVel {};
    f32 scaleY = 1.0f;
    f32 scaleX = 1.0f;
    f32 scaleInterpFinalY = 0.0f;
    f32 scaleInterpFinalX = 0.0f;
    D3DXVECTOR2 uvScrollPos {};
    ZunTimer currentTimeInScript {};
    D3DXMATRIX matrix {};
    ZunColor color = 0;
    u16 flags = 0;
    i16 alphaInterpEndTime = 0;
    i16 scaleInterpEndTime = 0;
    i16 autoRotate = 0;
    i16 pendingInterrupt = 0;
    i16 posInterpEndTime = 0;
    D3DXVECTOR3 pos {};
    f32 scaleInterpInitialY = 0.0f;
    f32 scaleInterpInitialX = 0.0f;
    ZunTimer scaleInterpTime {};
    i16 activeSpriteIndex = 0;
    i16 baseSpriteIndex = 0;
    i16 anmFileIndex = 0;
    D3DCOLOR alphaInterpInitial = 0;
    D3DCOLOR alphaInterpFinal = 0;
    D3DXVECTOR3 posInterpInitial {};
    D3DXVECTOR3 posInterpFinal {};
    D3DXVECTOR3 posOffset {};
    ZunTimer posInterpTime {};
    i32 timeOfLastSpriteSet = 0;
    ZunTimer alphaInterpTime {};
    u8 fontWidth = 0;
    u8 fontHeight = 0;
    DgsAnmVmRefs refs {};
};

struct PortableEnemyBulletShooterState
{
    i16 sprite = 0;
    i16 spriteOffset = 0;
    D3DXVECTOR3 position {};
    f32 angle1 = 0.0f;
    f32 angle2 = 0.0f;
    f32 speed1 = 0.0f;
    f32 speed2 = 0.0f;
    f32 exFloats[4] {};
    i32 exInts[4] {};
    i32 unk_40 = 0;
    i16 count1 = 0;
    i16 count2 = 0;
    u16 aimMode = 0;
    u16 unk_4a = 0;
    u32 flags = 0;
    u8 provokedPlayer = 0;
    SoundIdx sfx = NO_SOUND;
};

struct PortableEnemyLaserShooterState
{
    i16 sprite = 0;
    i16 spriteOffset = 0;
    D3DXVECTOR3 position {};
    f32 angle = 0.0f;
    u32 unk_14 = 0;
    f32 speed = 0.0f;
    u32 unk_1c = 0;
    f32 startOffset = 0.0f;
    f32 endOffset = 0.0f;
    f32 startLength = 0.0f;
    f32 width = 0.0f;
    i32 startTime = 0;
    i32 duration = 0;
    i32 despawnDuration = 0;
    i32 hitboxStartTime = 0;
    i32 hitboxEndDelay = 0;
    u32 unk_44 = 0;
    u16 type = 0;
    u32 flags = 0;
    u32 unk_50 = 0;
    u8 provokedPlayer = 0;
};

struct PortableEnemyEclContextState
{
    i32 scriptOffset = -1;
    ZunTimer time {};
    PortableEnemyFuncToken funcToken = PortableEnemyFuncToken::None;
    bool hasFuncToken = false;
    bool hasUnknownFuncToken = false;
    i32 var0 = 0;
    i32 var1 = 0;
    i32 var2 = 0;
    i32 var3 = 0;
    f32 float0 = 0.0f;
    f32 float1 = 0.0f;
    f32 float2 = 0.0f;
    f32 float3 = 0.0f;
    i32 var4 = 0;
    i32 var5 = 0;
    i32 var6 = 0;
    i32 var7 = 0;
    i32 compareRegister = 0;
    u16 subId = 0;
};

struct PortableEnemyState
{
    PortableAnmVmState primaryVm {};
    std::array<PortableAnmVmState, 8> vms {};
    PortableEnemyEclContextState currentContext {};
    std::array<PortableEnemyEclContextState, 8> savedContextStack {};
    i32 stackDepth = 0;
    i32 unk_c40 = 0;
    i32 deathCallbackSub = 0;
    i32 interrupts[8] {};
    i32 runInterrupt = 0;
    D3DXVECTOR3 position {};
    D3DXVECTOR3 hitboxDimensions {};
    D3DXVECTOR3 axisSpeed {};
    f32 angle = 0.0f;
    f32 angularVelocity = 0.0f;
    f32 speed = 0.0f;
    f32 acceleration = 0.0f;
    D3DXVECTOR3 shootOffset {};
    D3DXVECTOR3 moveInterp {};
    D3DXVECTOR3 moveInterpStartPos {};
    ZunTimer moveInterpTimer {};
    i32 moveInterpStartTime = 0;
    f32 bulletRankSpeedLow = 0.0f;
    f32 bulletRankSpeedHigh = 0.0f;
    i16 bulletRankAmount1Low = 0;
    i16 bulletRankAmount1High = 0;
    i16 bulletRankAmount2Low = 0;
    i16 bulletRankAmount2High = 0;
    i32 life = 0;
    i32 maxLife = 0;
    i32 score = 0;
    ZunTimer bossTimer {};
    ZunColor color = 0;
    PortableEnemyBulletShooterState bulletProps {};
    i32 shootInterval = 0;
    ZunTimer shootIntervalTimer {};
    PortableEnemyLaserShooterState laserProps {};
    std::array<i32, 32> laserIndices {};
    i32 laserStore = 0;
    u8 deathAnm1 = 0;
    u8 deathAnm2 = 0;
    u8 deathAnm3 = 0;
    i8 itemDrop = 0;
    u8 bossId = 0;
    u8 unk_e41 = 0;
    ZunTimer exInsFunc10Timer {};
    PortableEnemyFlagsState flags {};
    u8 anmExFlags = 0;
    i16 anmExDefaults = 0;
    i16 anmExFarLeft = 0;
    i16 anmExFarRight = 0;
    i16 anmExLeft = 0;
    i16 anmExRight = 0;
    D3DXVECTOR2 lowerMoveLimit {};
    D3DXVECTOR2 upperMoveLimit {};
    std::array<i32, 12> effectIndices {};
    i32 effectIdx = 0;
    f32 effectDistance = 0.0f;
    i32 lifeCallbackThreshold = 0;
    i32 lifeCallbackSub = 0;
    i32 timerCallbackThreshold = 0;
    i32 timerCallbackSub = 0;
    f32 exInsFunc6Angle = 0.0f;
    ZunTimer exInsFunc6Timer {};
    u8 provokedPlayer = 0;
};

using PortableEnemyTemplateState = PortableEnemyState;

struct PortableRunningSpellcardState
{
    bool isCapturing = false;
    bool isActive = false;
    i32 captureScore = 0;
    u32 idx = 0;
    bool usedBomb = false;
};

struct PortableEnemyManagerState
{
    std::string stgEnmAnmFilename;
    std::string stgEnm2AnmFilename;
    PortableEnemyTemplateState enemyTemplate {};
    std::vector<PortableEnemyState> enemies;
    std::array<i32, 8> bossIndices {};
    u16 randomItemSpawnIndex = 0;
    u16 randomItemTableIndex = 0;
    i32 enemyCount = 0;
    PortableRunningSpellcardState spellcardInfo {};
    i32 unk_ee5d8 = 0;
    i32 timelineOffset = -1;
    ZunTimer timelineTime {};
};

struct PortablePlayerBulletState
{
    PortableAnmVmState sprite {};
    D3DXVECTOR3 position {};
    D3DXVECTOR3 size {};
    D3DXVECTOR2 velocity {};
    f32 sidewaysMotion = 0.0f;
    D3DXVECTOR3 unk_134 {};
    ZunTimer timer {};
    i16 damage = 0;
    i16 bulletState = 0;
    i16 bulletType = 0;
    i16 unk_152 = 0;
    i16 spawnPositionIdx = 0;
};

struct PortablePlayerBombState
{
    u32 isInUse = 0;
    i32 duration = 0;
    ZunTimer timer {};
    PortablePlayerBombFuncToken calcToken = PortablePlayerBombFuncToken::None;
    PortablePlayerBombFuncToken drawToken = PortablePlayerBombFuncToken::None;
    bool hasUnknownCalcToken = false;
    bool hasUnknownDrawToken = false;
    std::array<i32, 8> reimuABombProjectilesState {};
    std::array<f32, 8> reimuABombProjectilesRelated {};
    std::array<D3DXVECTOR3, 8> bombRegionPositions {};
    std::array<D3DXVECTOR3, 8> bombRegionVelocities {};
    std::array<std::array<PortableAnmVmState, 4>, 8> sprites {};
};

struct PortableCharacterDataState
{
    f32 orthogonalMovementSpeed = 0.0f;
    f32 orthogonalMovementSpeedFocus = 0.0f;
    f32 diagonalMovementSpeed = 0.0f;
    f32 diagonalMovementSpeedFocus = 0.0f;
    PortablePlayerFireFuncToken fireBulletToken = PortablePlayerFireFuncToken::None;
    PortablePlayerFireFuncToken fireBulletFocusToken = PortablePlayerFireFuncToken::None;
    bool hasUnknownFireToken = false;
    bool hasUnknownFocusFireToken = false;
};

struct PortablePlayerState
{
    bool isPresent = false;
    PortableAnmVmState playerSprite {};
    std::array<PortableAnmVmState, 3> orbsSprite {};
    D3DXVECTOR3 positionCenter {};
    D3DXVECTOR3 unk_44c {};
    D3DXVECTOR3 hitboxTopLeft {};
    D3DXVECTOR3 hitboxBottomRight {};
    D3DXVECTOR3 grabItemTopLeft {};
    D3DXVECTOR3 grabItemBottomRight {};
    D3DXVECTOR3 hitboxSize {};
    D3DXVECTOR3 grabItemSize {};
    std::array<D3DXVECTOR3, 2> orbsPosition {};
    std::array<D3DXVECTOR3, 32> bombRegionPositions {};
    std::array<D3DXVECTOR3, 32> bombRegionSizes {};
    std::array<i32, 32> bombRegionDamages {};
    std::array<i32, 32> unk_838 {};
    std::array<PlayerRect, 16> bombProjectiles {};
    std::array<ZunTimer, 2> laserTimer {};
    f32 horizontalMovementSpeedMultiplierDuringBomb = 0.0f;
    f32 verticalMovementSpeedMultiplierDuringBomb = 0.0f;
    i32 respawnTimer = 0;
    i32 bulletGracePeriod = 0;
    i8 playerState = 0;
    i8 playerType = 0;
    u8 unk_9e1 = 0;
    i8 orbState = 0;
    i8 isFocus = 0;
    u8 unk_9e4 = 0;
    ZunTimer focusMovementTimer {};
    PortableCharacterDataState characterData {};
    i32 playerDirection = 0;
    f32 previousHorizontalSpeed = 0.0f;
    f32 previousVerticalSpeed = 0.0f;
    i16 previousFrameInput = 0;
    D3DXVECTOR3 positionOfLastEnemyHit {};
    std::array<PortablePlayerBulletState, 80> bullets {};
    ZunTimer fireBulletTimer {};
    ZunTimer invulnerabilityTimer {};
    PortablePlayerFireFuncToken fireBulletToken = PortablePlayerFireFuncToken::None;
    PortablePlayerFireFuncToken fireBulletFocusToken = PortablePlayerFireFuncToken::None;
    bool hasUnknownFireToken = false;
    bool hasUnknownFocusFireToken = false;
    PortablePlayerBombState bombInfo {};
    PortableAnmVmState hitboxSprite {};
    int hitboxTime = 0;
    int lifegiveTime = 0;
};

struct PortableBulletTypeSpritesState
{
    PortableAnmVmState spriteBullet {};
    PortableAnmVmState spriteSpawnEffectFast {};
    PortableAnmVmState spriteSpawnEffectNormal {};
    PortableAnmVmState spriteSpawnEffectSlow {};
    PortableAnmVmState spriteSpawnEffectDonut {};
    D3DXVECTOR3 grazeSize {};
    u8 unk_55c = 0;
    u8 bulletHeight = 0;
};

struct PortableBulletState
{
    PortableBulletTypeSpritesState sprites {};
    D3DXVECTOR3 pos {};
    D3DXVECTOR3 velocity {};
    D3DXVECTOR3 ex4Acceleration {};
    f32 speed = 0.0f;
    f32 ex5Float0 = 0.0f;
    f32 dirChangeSpeed = 0.0f;
    f32 angle = 0.0f;
    f32 ex5Float1 = 0.0f;
    f32 dirChangeRotation = 0.0f;
    ZunTimer timer {};
    i32 ex5Int0 = 0;
    i32 dirChangeInterval = 0;
    i32 dirChangeNumTimes = 0;
    i32 dirChangeMaxTimes = 0;
    u16 exFlags = 0;
    i16 spriteOffset = 0;
    u16 unk_5bc = 0;
    u16 state = 0;
    u16 unk_5c0 = 0;
    u8 unk_5c2 = 0;
    u8 isGrazed = 0;
    u8 provokedPlayer = 0;
};

struct PortableLaserState
{
    PortableAnmVmState vm0 {};
    PortableAnmVmState vm1 {};
    D3DXVECTOR3 pos {};
    f32 angle = 0.0f;
    f32 startOffset = 0.0f;
    f32 endOffset = 0.0f;
    f32 startLength = 0.0f;
    f32 width = 0.0f;
    f32 speed = 0.0f;
    i32 startTime = 0;
    i32 hitboxStartTime = 0;
    i32 duration = 0;
    i32 despawnDuration = 0;
    i32 hitboxEndDelay = 0;
    i32 inUse = 0;
    ZunTimer timer {};
    u16 flags = 0;
    i16 color = 0;
    u8 state = 0;
    u8 provokedPlayer = 0;
};

struct PortableBulletManagerState
{
    std::string bulletAnmPath;
    std::array<PortableBulletTypeSpritesState, 16> bulletTypeTemplates {};
    std::array<PortableBulletState, 640> bullets {};
    std::array<PortableLaserState, 64> lasers {};
    i32 nextBulletIndex = 0;
    i32 bulletCount = 0;
    ZunTimer time {};
};

struct PortableItemState
{
    PortableAnmVmState sprite {};
    D3DXVECTOR3 currentPosition {};
    D3DXVECTOR3 startPosition {};
    D3DXVECTOR3 targetPosition {};
    ZunTimer timer {};
    i8 itemType = 0;
    i8 isInUse = 0;
    i8 unk_142 = 0;
    i8 state = 0;
};

struct PortableItemManagerState
{
    std::array<PortableItemState, 513> items {};
    i32 nextIndex = 0;
    u32 itemCount = 0;
};

struct PortableEffectState
{
    PortableAnmVmState vm {};
    D3DXVECTOR3 pos1 {};
    D3DXVECTOR3 unk_11c {};
    D3DXVECTOR3 unk_128 {};
    D3DXVECTOR3 position {};
    D3DXVECTOR3 pos2 {};
    D3DXQUATERNION quaternion {};
    f32 unk_15c = 0.0f;
    f32 angleRelated = 0.0f;
    ZunTimer timer {};
    i32 unk_170 = 0;
    PortableEffectUpdateToken updateCallbackToken = PortableEffectUpdateToken::None;
    bool hasUnknownUpdateToken = false;
    i8 inUseFlag = 0;
    i8 effectId = 0;
    i8 unk_17a = 0;
    i8 unk_17b = 0;
};

struct PortableEffectManagerState
{
    i32 nextIndex = 0;
    i32 activeEffects = 0;
    std::array<PortableEffectState, 513> effects {};
};

struct PortableEclResourceIdentity
{
    std::string resourcePath;
    u64 resourceContentHash = 0;
    u32 resourceSizeBytes = 0;
};

struct PortableEclManagerCoreState
{
    bool hasEclFile = false;
    PortableEclResourceIdentity resource {};
    i32 subCount = 0;
    i32 mainCount = 0;
    std::array<i32, 3> timelineOffsets {{-1, -1, -1}};
    i32 activeTimelineSlot = -1;
    i32 activeTimelineOffset = -1;
    std::vector<i32> subTableEntryOffsets;
};

struct PortableStageResourceIdentity
{
    std::string stdPath;
    u64 stdContentHash = 0;
    u32 stdSizeBytes = 0;
    std::string anmPath;
    u64 anmContentHash = 0;
    u32 anmSizeBytes = 0;
};

struct PortableStageCoreState
{
    bool hasStageData = false;
    PortableStageResourceIdentity resource {};
    u32 stage = 0;
    i32 objectsCount = 0;
    i32 quadCount = 0;
    ZunTimer scriptTime {};
    i32 instructionIndex = 0;
    ZunTimer timer {};
    D3DXVECTOR3 position {};
    StageCameraSky skyFog {};
    StageCameraSky skyFogInterpInitial {};
    StageCameraSky skyFogInterpFinal {};
    i32 skyFogInterpDuration = 0;
    ZunTimer skyFogInterpTimer {};
    u8 skyFogNeedsSetup = 0;
    SpellcardState spellcardState = NOT_RUNNING;
    i32 ticksSinceSpellcardStarted = 0;
    u8 unpauseFlag = 0;
    D3DXVECTOR3 facingDirInterpInitial {};
    D3DXVECTOR3 facingDirInterpFinal {};
    i32 facingDirInterpDuration = 0;
    ZunTimer facingDirInterpTimer {};
    D3DXVECTOR3 positionInterpFinal {};
    i32 positionInterpEndTime = 0;
    D3DXVECTOR3 positionInterpInitial {};
    i32 positionInterpStartTime = 0;
    D3DXVECTOR3 currentCameraFacingDir {};
    std::vector<u8> objectFlags;
    std::vector<PortableAnmVmState> quadVms;
    PortableAnmVmState spellcardBackground {};
    PortableAnmVmState extraBackground {};
};

struct PortableShellSyncState
{
    i32 bgmTrackIndex = -1;
    PortableBossAssetProfile bossAssetProfile = PortableBossAssetProfile::None;
    bool hideStageNameIntro = false;
    bool hideSongNameIntro = false;
};

struct PortableGuiState
{
    bool hasGuiImpl = false;
    u8 flag0 = 0;
    u8 flag1 = 0;
    u8 flag2 = 0;
    u8 flag3 = 0;
    u8 flag4 = 0;
    u8 bossHealthBarState = 0;
    bool bossPresent = false;
    u32 bossUIOpacity = 0;
    i32 eclSetLives = 0;
    i32 spellcardSecondsRemaining = 0;
    i32 lastSpellcardSecondsRemaining = 0;
    f32 bossHealthBar1 = 0.0f;
    f32 bossHealthBar2 = 0.0f;
    f32 bombSpellcardBarLength = 0.0f;
    f32 blueSpellcardBarLength = 0.0f;
    PortableAnmVmState playerSpellcardPortrait {};
    PortableAnmVmState enemySpellcardPortrait {};
    PortableAnmVmState bombSpellcardName {};
    PortableAnmVmState enemySpellcardName {};
    PortableAnmVmState bombSpellcardBackground {};
    PortableAnmVmState enemySpellcardBackground {};
    PortableAnmVmState loadingScreenSprite {};
    std::string bombSpellcardText;
    std::string enemySpellcardText;
};

struct PortableEclScriptState
{
    bool hasEclFile = false;
    i32 timelineOffset = -1;
    std::vector<i32> subTableEntryOffsets;
};

struct PortableGameplayShadowFingerprints
{
    DgsSubsystemFingerprints dgs {};
};

struct PortableGameplayCoverageEntry
{
    DgsCoverageKind kind = DgsCoverageKind::Included;
    std::string name;
    std::string detail;
};

struct PortableGameplayCoverageReport
{
    std::vector<PortableGameplayCoverageEntry> entries;
};

struct PortableRestoreEvaluation
{
    PortableRestoreReadiness readiness = PortableRestoreReadiness::ShadowOnly;
    bool schemaMatches = false;
    bool catalogMatches = false;
    u32 missingCaptureFlags = 0;
    std::vector<std::string> blockingReasons;
};

struct PortableGameplayState
{
    PortableGameplayEnvelopeHeader header {};
    u32 captureFlags = PortableCaptureFlag_None;
    PortableGameplayResourceCatalog catalog {};
    PortableGameplayCoreState core {};
    PortableGameplayRuntimeState runtime {};
    PortableRngState rng {};
    DgsStageRefs stageRefs {};
    DgsEnemyManagerRefs enemyRefs {};
    PortableEclScriptState eclScripts {};
    PortableEclManagerCoreState eclCore {};
    PortableStageCoreState stageCore {};
    PortableShellSyncState shellSync {};
    PortableGuiState gui {};
    PortableEnemyManagerState enemyActors {};
    std::array<PortablePlayerState, 2> players {};
    PortableBulletManagerState bulletActors {};
    PortableItemManagerState itemActors {};
    PortableEffectManagerState effectActors {};
    std::array<Catk, CATK_NUM_CAPTURES> catk {};
    PortableGameplayShadowFingerprints shadowFingerprints {};
    std::vector<std::string> mustNormalizeBeforeRestore;
    std::vector<std::string> excludedByDesign;
};

struct PortableGameplayBuildResult
{
    bool success = false;
    PortableRestoreEvaluation evaluation {};
    u64 shadowFingerprint = 0;
    u32 builtEnemyCount = 0;
    u32 builtPlayerCount = 0;
    u32 builtPlayerBulletCount = 0;
    u32 builtBulletCount = 0;
    u32 builtLaserCount = 0;
    u32 builtItemCount = 0;
    u32 builtEffectCount = 0;
    u32 builtEclSubCount = 0;
    i32 builtEclTimelineSlot = -1;
    u32 builtStageObjectCount = 0;
    u32 builtStageQuadCount = 0;
    std::vector<std::string> notes;
};

void CapturePortableGameplayState(PortableGameplayState &state);
uint64_t FingerprintPortableGameplayState(const PortableGameplayState &state);
PortableGameplayCoverageReport AuditPortableGameplayStateCoverage(const PortableGameplayState &state);
void EvaluatePortableRestoreReadiness(const PortableGameplayState &state, PortableRestoreEvaluation &evaluation);
PortableRestoreEvaluation EvaluatePortableRestoreReadiness(const PortableGameplayState &state);
bool CapturePortableAuthorityGameplayFingerprint(const PortableGameplayState &state, AuthorityGameplayFingerprint &out);
const char *FirstDifferentAuthorityGameplaySubsystem(const AuthorityGameplayFingerprint &lhs,
                                                     const AuthorityGameplayFingerprint &rhs);
std::vector<u8> EncodePortableGameplayState(const PortableGameplayState &state);
bool DecodePortableGameplayState(const std::vector<u8> &bytes, PortableGameplayState &state);
bool RoundTripPortableGameplayStateBinary(const PortableGameplayState &state, uint64_t *outBefore = nullptr,
                                          uint64_t *outAfter = nullptr);
void BuildPortableGameplayWorldFromState(const PortableGameplayState &state, PortableGameplayBuildResult &result);
PortableGameplayBuildResult BuildPortableGameplayWorldFromState(const PortableGameplayState &state);
bool RunPortableGameplayDebugValidation(PortableGameplayBuildResult *outBuild = nullptr,
                                        PortableRestoreEvaluation *outEvaluation = nullptr,
                                        uint64_t *outBefore = nullptr, uint64_t *outAfter = nullptr);
void SetPortableValidationTraceEnabled(bool enabled);
bool IsPortableValidationTraceEnabled();
void TracePortableValidationPhase(const char *phase);

const char *PortableGameplaySectionIdToString(PortableGameplaySectionId id);
const char *PortableRestoreReadinessToString(PortableRestoreReadiness readiness);
} // namespace th06::DGS
