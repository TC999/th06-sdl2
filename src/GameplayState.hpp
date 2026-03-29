#pragma once

#include "AnmManager.hpp"
#include "BulletManager.hpp"
#include "Controller.hpp"
#include "EclManager.hpp"
#include "EffectManager.hpp"
#include "EnemyEclInstr.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "Rng.hpp"
#include "ScreenEffect.hpp"
#include "SoundPlayer.hpp"
#include "Stage.hpp"
#include "Supervisor.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace th06
{
namespace Netplay
{
struct GameplaySnapshot;
}

namespace DGS
{
constexpr u32 kDgsMagic = 0x31475344u; // "DGS1"

enum class DgsSchemaVersion : u32
{
    V1 = 1
};

enum class DgsSectionId : u16
{
    Envelope = 0,
    CoreState = 1,
    RuntimeState = 2,
    StageRefs = 3,
    EnemyRefs = 4,
    EclRefs = 5,
    Audit = 6
};

template <typename Tag> struct DgsIndexRef
{
    i32 value = -1;

    bool HasValue() const
    {
        return value >= 0;
    }
};

template <typename Tag> struct DgsOffsetRef
{
    i32 value = -1;

    bool HasValue() const
    {
        return value >= 0;
    }
};

template <typename T> struct DgsOpaqueState
{
    std::array<u8, sizeof(T)> bytes {};

    void CaptureFrom(const T &value)
    {
        std::memcpy(bytes.data(), &value, sizeof(T));
    }

    T Load() const
    {
        T value {};
        std::memcpy(&value, bytes.data(), sizeof(T));
        return value;
    }

    void RestoreTo(T &value) const
    {
        std::memcpy(&value, bytes.data(), sizeof(T));
    }
};

struct DgsEnvelopeHeader
{
    u32 magic = kDgsMagic;
    u32 version = (u32)DgsSchemaVersion::V1;
    u32 sectionCount = 0;
    u32 reserved = 0;
};

struct DgsSupervisorRuntimeState
{
    i32 calcCount = 0;
    i32 wantedState = 0;
    i32 curState = 0;
    i32 wantedState2 = 0;
    i32 unk194 = 0;
    i32 unk198 = 0;
    ZunBool isInEnding = false;
    i32 vsyncEnabled = 0;
    i32 lastFrameTime = 0;
    f32 effectiveFramerateMultiplier = 1.0f;
    f32 framerateMultiplier = 1.0f;
    f32 unk1b4 = 0.0f;
    f32 unk1b8 = 0.0f;
    u32 startupTimeBeforeMenuMusic = 0;
};

struct DgsGameWindowRuntimeState
{
    i32 tickCountToEffectiveFramerate = 0;
    double lastFrameTime = 0.0;
    u8 curFrame = 0;
};

struct DgsStageRuntimeState
{
    std::vector<RawStageObjectInstance> objectInstances;
    std::vector<AnmVm> quadVms;
};

struct DgsSoundRuntimeState
{
    i32 soundBuffersToPlay[3] {};
    i32 queuedSfxState[128] {};
    i32 isLooping = 0;
};

struct DgsInputRuntimeState
{
    u16 lastFrameInput = 0;
    u16 curFrameInput = 0;
    u16 eighthFrameHeldInput = 0;
    u16 heldInputFrames = 0;
};

struct DgsAnmScriptTag;
struct DgsAnmSpriteTag;
struct DgsStageScriptTag;
struct DgsStageObjectTag;
struct DgsEnemyTag;
struct DgsLaserTag;
struct DgsEffectTag;
struct DgsEclInstrTag;
struct DgsEclTimelineTag;
struct DgsEclSubTableTag;

struct DgsAnmVmRefs
{
    i32 scriptSlot = -1;
    DgsOffsetRef<DgsAnmScriptTag> beginningOfScript;
    DgsOffsetRef<DgsAnmScriptTag> currentInstruction;
    DgsIndexRef<DgsAnmSpriteTag> spriteIndex;
};

struct DgsEnemyContextRefs
{
    DgsOffsetRef<DgsEclInstrTag> currentInstruction;
    bool hasFuncSetFunc = false;
    bool unresolvedFuncSetFunc = false;
};

struct DgsEnemyRefs
{
    DgsAnmVmRefs primaryVm;
    std::array<DgsAnmVmRefs, 8> vms {};
    DgsEnemyContextRefs currentContext;
    std::array<DgsEnemyContextRefs, 8> savedContextStack {};
    std::array<DgsIndexRef<DgsLaserTag>, 32> laserIndices {};
    std::array<DgsIndexRef<DgsEffectTag>, 12> effectIndices {};
};

struct DgsEnemyManagerRefs
{
    std::array<DgsIndexRef<DgsEnemyTag>, 8> bossIndices {};
    DgsOffsetRef<DgsEclTimelineTag> timelineOffset;
    std::vector<DgsEnemyRefs> enemyRefs;
};

struct DgsStageRefs
{
    DgsOffsetRef<DgsStageScriptTag> beginningOfScript;
    std::vector<DgsOffsetRef<DgsStageObjectTag>> objectOffsets;
    std::vector<DgsAnmVmRefs> quadVmRefs;
    DgsAnmVmRefs spellcardBackground;
    DgsAnmVmRefs extraBackground;
};

struct DgsEclManagerRefs
{
    bool hasEclFile = false;
    DgsOffsetRef<DgsEclTimelineTag> timelineOffset;
    DgsOffsetRef<DgsEclSubTableTag> subTableOffset;
};

enum class DgsCoverageKind
{
    Included,
    Excluded,
    Unresolved
};

struct DgsCoverageEntry
{
    DgsCoverageKind kind = DgsCoverageKind::Included;
    std::string name;
    std::string detail;
};

struct DgsCoverageReport
{
    std::vector<DgsCoverageEntry> entries;
};

struct DgsComparisonResult
{
    bool matches = false;
    uint64_t expectedFingerprint = 0;
    uint64_t liveFingerprint = 0;
    std::vector<std::string> mismatchedSubsystems;
};

struct DgsSubsystemFingerprints
{
    uint64_t gameManager = 0;
    uint64_t player1 = 0;
    uint64_t player2 = 0;
    uint64_t bulletManager = 0;
    uint64_t enemyManager = 0;
    uint64_t itemManager = 0;
    uint64_t effectManager = 0;
    uint64_t stageState = 0;
    uint64_t eclManager = 0;
    uint64_t rng = 0;
    uint64_t runtime = 0;
    uint64_t refs = 0;
    uint64_t unresolved = 0;
    uint64_t combined = 0;
};

struct DeterministicGameplayState
{
    DgsEnvelopeHeader header {};
    i32 frame = 0;
    i32 stage = 0;
    i32 delay = 1;
    i32 currentDelayCooldown = 0;

    DgsOpaqueState<GameManager> gameManager;
    DgsOpaqueState<Player> player1;
    DgsOpaqueState<Player> player2;
    DgsOpaqueState<BulletManager> bulletManager;
    DgsOpaqueState<EnemyManager> enemyManager;
    DgsOpaqueState<ItemManager> itemManager;
    DgsOpaqueState<EffectManager> effectManager;
    DgsOpaqueState<Stage> stageState;
    DgsOpaqueState<EclManager> eclManager;
    DgsOpaqueState<Rng> rng;

    EnemyEclInstr::RuntimeState enemyEclRuntimeState {};
    ScreenEffect::RuntimeState screenEffectRuntimeState {};
    Controller::RuntimeState controllerRuntimeState {};
    DgsSupervisorRuntimeState supervisorRuntimeState {};
    DgsGameWindowRuntimeState gameWindowRuntimeState {};
    DgsStageRuntimeState stageRuntimeState {};
    DgsSoundRuntimeState soundRuntimeState {};
    DgsInputRuntimeState inputRuntimeState {};

    DgsStageRefs stageRefs {};
    DgsEnemyManagerRefs enemyManagerRefs {};
    DgsEclManagerRefs eclManagerRefs {};

    std::vector<std::string> unresolvedFields;
};

void CaptureSharedGameplaySnapshotState(Netplay::GameplaySnapshot &snapshot);
bool RestoreSharedGameplaySnapshotState(const Netplay::GameplaySnapshot &snapshot);

void RestoreAnmVmRefs(AnmVm &vm, const DgsAnmVmRefs &refs);
void RestoreEnemyContextRefs(EnemyEclContext &context, const DgsEnemyContextRefs &refs);
void RestoreStageRefs(Stage &stage, const DgsStageRefs &refs);
void RestoreEnemyManagerRefs(EnemyManager &enemyManager, const DgsEnemyManagerRefs &refs);
void RestoreEclManagerRefs(EclManager &eclManager, const DgsEclManagerRefs &refs);
bool RestoreDeterministicRuntimeState(const EnemyEclInstr::RuntimeState &enemyEclRuntimeState,
                                      const ScreenEffect::RuntimeState &screenEffectRuntimeState,
                                      const Controller::RuntimeState &controllerRuntimeState,
                                      const DgsSupervisorRuntimeState &supervisorRuntimeState,
                                      const DgsGameWindowRuntimeState &gameWindowRuntimeState,
                                      const DgsStageRuntimeState &stageRuntimeState,
                                      const DgsSoundRuntimeState &soundRuntimeState,
                                      const DgsInputRuntimeState &inputRuntimeState);

void CaptureDeterministicGameplayState(DeterministicGameplayState &state);
void CaptureDeterministicGameplayStateFromGameplaySnapshot(const Netplay::GameplaySnapshot &snapshot,
                                                          DeterministicGameplayState &state);
bool RestoreDeterministicGameplayState(const DeterministicGameplayState &state);

DgsCoverageReport AuditDeterministicGameplayStateCoverage();
uint64_t FingerprintDeterministicGameplayState(const DeterministicGameplayState &state);
DgsSubsystemFingerprints FingerprintDeterministicGameplaySubsystems(const DeterministicGameplayState &state);
uint64_t FingerprintGameplaySnapshotOverlap(const Netplay::GameplaySnapshot &snapshot);
DgsComparisonResult CompareDeterministicGameplayStateToLiveWorld(const DeterministicGameplayState &state);
bool RoundTripDeterministicGameplayStateInProcess(uint64_t *outBefore = nullptr, uint64_t *outAfter = nullptr);

const char *DgsSectionIdToString(DgsSectionId id);
const char *DgsCoverageKindToString(DgsCoverageKind kind);

} // namespace DGS
} // namespace th06
