#include "GameplayState.hpp"

#include "NetplayInternal.hpp"

#include <algorithm>
#include <iterator>
#include <memory>

namespace th06::DGS
{
namespace
{
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void HashBytes(uint64_t &hash, const void *data, size_t size)
{
    const u8 *bytes = reinterpret_cast<const u8 *>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

template <typename T> void HashValue(uint64_t &hash, const T &value)
{
    HashBytes(hash, &value, sizeof(T));
}

template <typename T> void HashVector(uint64_t &hash, const std::vector<T> &values)
{
    const u32 count = (u32)values.size();
    HashValue(hash, count);
    if (!values.empty())
    {
        HashBytes(hash, values.data(), values.size() * sizeof(T));
    }
}

void HashString(uint64_t &hash, const std::string &value)
{
    const u32 size = (u32)value.size();
    HashValue(hash, size);
    if (!value.empty())
    {
        HashBytes(hash, value.data(), value.size());
    }
}

void SanitizeAnmVm(AnmVm &vm)
{
    vm.beginingOfScript = nullptr;
    vm.currentInstruction = nullptr;
    vm.sprite = nullptr;
}

void SanitizePlayer(Player &player)
{
    SanitizeAnmVm(player.playerSprite);
    for (AnmVm &orb : player.orbsSprite)
    {
        SanitizeAnmVm(orb);
    }
    for (PlayerBullet &bullet : player.bullets)
    {
        SanitizeAnmVm(bullet.sprite);
    }
    player.fireBulletCallback = nullptr;
    player.fireBulletFocusCallback = nullptr;
    player.characterData.fireBulletCallback = nullptr;
    player.characterData.fireBulletFocusCallback = nullptr;
    player.bombInfo.calc = nullptr;
    player.bombInfo.draw = nullptr;
    for (auto &spriteRow : player.bombInfo.sprites)
    {
        for (AnmVm &vm : spriteRow)
        {
            SanitizeAnmVm(vm);
        }
    }
    player.chainCalc = nullptr;
    player.chainDraw1 = nullptr;
    player.chainDraw2 = nullptr;
    SanitizeAnmVm(player.hitboxSprite);
}

void SanitizeBulletManager(BulletManager &bulletManager)
{
    bulletManager.bulletAnmPath = nullptr;
    for (Laser &laser : bulletManager.lasers)
    {
        SanitizeAnmVm(laser.vm0);
        SanitizeAnmVm(laser.vm1);
    }
}

void SanitizeEnemyContext(EnemyEclContext &context)
{
    context.currentInstr = nullptr;
    context.funcSetFunc = nullptr;
}

void SanitizeEnemy(Enemy &enemy)
{
    SanitizeAnmVm(enemy.primaryVm);
    for (AnmVm &vm : enemy.vms)
    {
        SanitizeAnmVm(vm);
    }
    SanitizeEnemyContext(enemy.currentContext);
    for (EnemyEclContext &saved : enemy.savedContextStack)
    {
        SanitizeEnemyContext(saved);
    }
    std::fill(std::begin(enemy.lasers), std::end(enemy.lasers), nullptr);
    std::fill(std::begin(enemy.effectArray), std::end(enemy.effectArray), nullptr);
}

void SanitizeEnemyManager(EnemyManager &enemyManager)
{
    enemyManager.stgEnmAnmFilename = nullptr;
    enemyManager.stgEnm2AnmFilename = nullptr;
    SanitizeEnemy(enemyManager.enemyTemplate);
    for (Enemy &enemy : enemyManager.enemies)
    {
        SanitizeEnemy(enemy);
    }
    std::fill(std::begin(enemyManager.bosses), std::end(enemyManager.bosses), nullptr);
    enemyManager.timelineInstr = nullptr;
}

void SanitizeItemManager(ItemManager &itemManager)
{
    for (Item &item : itemManager.items)
    {
        SanitizeAnmVm(item.sprite);
    }
}

void SanitizeEffect(Effect &effect)
{
    SanitizeAnmVm(effect.vm);
    effect.updateCallback = nullptr;
}

void SanitizeEffectManager(EffectManager &effectManager)
{
    for (Effect &effect : effectManager.effects)
    {
        SanitizeEffect(effect);
    }
}

void SanitizeStage(Stage &stage)
{
    stage.quadVms = nullptr;
    stage.stdData = nullptr;
    stage.objects = nullptr;
    stage.objectInstances = nullptr;
    stage.beginningOfScript = nullptr;
    SanitizeAnmVm(stage.spellcardBackground);
    SanitizeAnmVm(stage.unk2);
}

void SanitizeEclManager(EclManager &eclManager)
{
    eclManager.eclFile = nullptr;
    eclManager.subTable = nullptr;
    eclManager.timeline = nullptr;
}

template <typename T> uint64_t FingerprintOpaque(const DgsOpaqueState<T> &state)
{
    uint64_t hash = kFnvOffset;
    HashBytes(hash, state.bytes.data(), state.bytes.size());
    return hash;
}

template <> uint64_t FingerprintOpaque<Player>(const DgsOpaqueState<Player> &state)
{
    auto value = std::make_unique<Player>();
    std::memcpy(value.get(), state.bytes.data(), sizeof(Player));
    SanitizePlayer(*value);
    uint64_t hash = kFnvOffset;
    HashBytes(hash, value.get(), sizeof(Player));
    return hash;
}

template <> uint64_t FingerprintOpaque<BulletManager>(const DgsOpaqueState<BulletManager> &state)
{
    auto value = std::make_unique<BulletManager>();
    std::memcpy(value.get(), state.bytes.data(), sizeof(BulletManager));
    SanitizeBulletManager(*value);
    uint64_t hash = kFnvOffset;
    HashBytes(hash, value.get(), sizeof(BulletManager));
    return hash;
}

template <> uint64_t FingerprintOpaque<EnemyManager>(const DgsOpaqueState<EnemyManager> &state)
{
    auto value = std::make_unique<EnemyManager>();
    std::memcpy(value.get(), state.bytes.data(), sizeof(EnemyManager));
    SanitizeEnemyManager(*value);
    uint64_t hash = kFnvOffset;
    HashBytes(hash, value.get(), sizeof(EnemyManager));
    return hash;
}

template <> uint64_t FingerprintOpaque<ItemManager>(const DgsOpaqueState<ItemManager> &state)
{
    auto value = std::make_unique<ItemManager>();
    std::memcpy(value.get(), state.bytes.data(), sizeof(ItemManager));
    SanitizeItemManager(*value);
    uint64_t hash = kFnvOffset;
    HashBytes(hash, value.get(), sizeof(ItemManager));
    return hash;
}

template <> uint64_t FingerprintOpaque<EffectManager>(const DgsOpaqueState<EffectManager> &state)
{
    auto value = std::make_unique<EffectManager>();
    std::memcpy(value.get(), state.bytes.data(), sizeof(EffectManager));
    SanitizeEffectManager(*value);
    uint64_t hash = kFnvOffset;
    HashBytes(hash, value.get(), sizeof(EffectManager));
    return hash;
}

template <> uint64_t FingerprintOpaque<Stage>(const DgsOpaqueState<Stage> &state)
{
    auto value = std::make_unique<Stage>();
    std::memcpy(value.get(), state.bytes.data(), sizeof(Stage));
    SanitizeStage(*value);
    uint64_t hash = kFnvOffset;
    HashBytes(hash, value.get(), sizeof(Stage));
    return hash;
}

template <> uint64_t FingerprintOpaque<EclManager>(const DgsOpaqueState<EclManager> &state)
{
    auto value = std::make_unique<EclManager>();
    std::memcpy(value.get(), state.bytes.data(), sizeof(EclManager));
    SanitizeEclManager(*value);
    uint64_t hash = kFnvOffset;
    HashBytes(hash, value.get(), sizeof(EclManager));
    return hash;
}

void HashAnmVmRefs(uint64_t &hash, const DgsAnmVmRefs &refs)
{
    HashValue(hash, refs.scriptSlot);
    HashValue(hash, refs.beginningOfScript.value);
    HashValue(hash, refs.currentInstruction.value);
    HashValue(hash, refs.spriteIndex.value);
}

void HashEnemyContextRefs(uint64_t &hash, const DgsEnemyContextRefs &refs)
{
    HashValue(hash, refs.currentInstruction.value);
    HashValue(hash, refs.hasFuncSetFunc);
    HashValue(hash, refs.unresolvedFuncSetFunc);
}

uint64_t FingerprintStageRefs(const DgsStageRefs &refs)
{
    uint64_t hash = kFnvOffset;
    HashValue(hash, refs.beginningOfScript.value);
    for (const auto &entry : refs.objectOffsets)
    {
        HashValue(hash, entry.value);
    }
    for (const DgsAnmVmRefs &vmRefs : refs.quadVmRefs)
    {
        HashAnmVmRefs(hash, vmRefs);
    }
    HashAnmVmRefs(hash, refs.spellcardBackground);
    HashAnmVmRefs(hash, refs.extraBackground);
    return hash;
}

uint64_t FingerprintEnemyRefs(const DgsEnemyManagerRefs &refs)
{
    uint64_t hash = kFnvOffset;
    for (const auto &bossIndex : refs.bossIndices)
    {
        HashValue(hash, bossIndex.value);
    }
    HashValue(hash, refs.timelineOffset.value);
    for (const DgsEnemyRefs &enemyRefs : refs.enemyRefs)
    {
        HashAnmVmRefs(hash, enemyRefs.primaryVm);
        for (const DgsAnmVmRefs &vmRefs : enemyRefs.vms)
        {
            HashAnmVmRefs(hash, vmRefs);
        }
        HashEnemyContextRefs(hash, enemyRefs.currentContext);
        for (const DgsEnemyContextRefs &saved : enemyRefs.savedContextStack)
        {
            HashEnemyContextRefs(hash, saved);
        }
        for (const auto &laserIndex : enemyRefs.laserIndices)
        {
            HashValue(hash, laserIndex.value);
        }
        for (const auto &effectIndex : enemyRefs.effectIndices)
        {
            HashValue(hash, effectIndex.value);
        }
    }
    return hash;
}

uint64_t FingerprintEclRefs(const DgsEclManagerRefs &refs)
{
    uint64_t hash = kFnvOffset;
    HashValue(hash, refs.hasEclFile);
    HashValue(hash, refs.timelineOffset.value);
    HashValue(hash, refs.subTableOffset.value);
    return hash;
}

template <typename T> uint64_t FingerprintRawStruct(const T &value)
{
    uint64_t hash = kFnvOffset;
    HashBytes(hash, &value, sizeof(T));
    return hash;
}

uint64_t FingerprintStageRuntime(const DgsStageRuntimeState &state)
{
    uint64_t hash = kFnvOffset;
    HashVector(hash, state.objectInstances);
    HashVector(hash, state.quadVms);
    return hash;
}

struct DgsFingerprintSummary
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

    uint64_t Combined() const
    {
        uint64_t hash = kFnvOffset;
        HashValue(hash, gameManager);
        HashValue(hash, player1);
        HashValue(hash, player2);
        HashValue(hash, bulletManager);
        HashValue(hash, enemyManager);
        HashValue(hash, itemManager);
        HashValue(hash, effectManager);
        HashValue(hash, stageState);
        HashValue(hash, eclManager);
        HashValue(hash, rng);
        HashValue(hash, runtime);
        HashValue(hash, refs);
        HashValue(hash, unresolved);
        return hash;
    }
};

DgsFingerprintSummary BuildFingerprintSummary(const DeterministicGameplayState &state)
{
    DgsFingerprintSummary summary;
    summary.gameManager = FingerprintOpaque(state.gameManager);
    summary.player1 = FingerprintOpaque(state.player1);
    summary.player2 = FingerprintOpaque(state.player2);
    summary.bulletManager = FingerprintOpaque(state.bulletManager);
    summary.enemyManager = FingerprintOpaque(state.enemyManager);
    summary.itemManager = FingerprintOpaque(state.itemManager);
    summary.effectManager = FingerprintOpaque(state.effectManager);
    summary.stageState = FingerprintOpaque(state.stageState);
    summary.eclManager = FingerprintOpaque(state.eclManager);
    summary.rng = FingerprintOpaque(state.rng);

    uint64_t runtimeHash = kFnvOffset;
    HashValue(runtimeHash, state.frame);
    HashValue(runtimeHash, state.stage);
    HashValue(runtimeHash, state.delay);
    HashValue(runtimeHash, state.currentDelayCooldown);
    HashValue(runtimeHash, state.enemyEclRuntimeState);
    HashValue(runtimeHash, state.screenEffectRuntimeState);
    HashValue(runtimeHash, state.controllerRuntimeState);
    HashValue(runtimeHash, state.supervisorRuntimeState);
    HashValue(runtimeHash, state.gameWindowRuntimeState);
    HashValue(runtimeHash, state.soundRuntimeState);
    HashValue(runtimeHash, state.inputRuntimeState);
    HashValue(runtimeHash, FingerprintStageRuntime(state.stageRuntimeState));
    summary.runtime = runtimeHash;

    uint64_t refsHash = kFnvOffset;
    HashValue(refsHash, FingerprintStageRefs(state.stageRefs));
    HashValue(refsHash, FingerprintEnemyRefs(state.enemyManagerRefs));
    HashValue(refsHash, FingerprintEclRefs(state.eclManagerRefs));
    summary.refs = refsHash;

    uint64_t unresolvedHash = kFnvOffset;
    for (const std::string &entry : state.unresolvedFields)
    {
        HashString(unresolvedHash, entry);
    }
    summary.unresolved = unresolvedHash;
    return summary;
}
} // namespace

DgsCoverageReport AuditDeterministicGameplayStateCoverage()
{
    DgsCoverageReport report;
    report.entries.push_back({DgsCoverageKind::Included, "GameManager", "Opaque deterministic core state."});
    report.entries.push_back({DgsCoverageKind::Included, "Player1/Player2", "Gameplay player state captured through opaque bridge."});
    report.entries.push_back({DgsCoverageKind::Included, "BulletManager", "Gameplay bullets and lasers captured through opaque bridge."});
    report.entries.push_back({DgsCoverageKind::Included, "EnemyManager", "Enemy arrays and deterministic manager state captured through opaque bridge."});
    report.entries.push_back({DgsCoverageKind::Included, "ItemManager", "Active items captured through opaque bridge."});
    report.entries.push_back({DgsCoverageKind::Included, "EffectManager", "Active effects captured through opaque bridge."});
    report.entries.push_back({DgsCoverageKind::Included, "Stage", "Gameplay stage state plus normalized stage refs."});
    report.entries.push_back({DgsCoverageKind::Included, "EclManager", "ECL runtime state plus normalized base relations."});
    report.entries.push_back({DgsCoverageKind::Included, "EnemyEclInstr::RuntimeState", "Runtime-only deterministic enemy ECL globals."});
    report.entries.push_back({DgsCoverageKind::Included, "ScreenEffect::RuntimeState", "Runtime-only deterministic screen effect globals."});
    report.entries.push_back({DgsCoverageKind::Included, "Controller::RuntimeState", "Deterministic controller runtime fields already used by rollback."});
    report.entries.push_back({DgsCoverageKind::Included, "Supervisor/GameWindow timing fields", "Current rollback deterministic timing fields."});
    report.entries.push_back({DgsCoverageKind::Included, "Sound queue state", "Gameplay-adjacent queued sound values without device handles."});

    report.entries.push_back({DgsCoverageKind::Excluded, "MainMenu/OnlineMenu", "UI state intentionally excluded from DGS phase 1."});
    report.entries.push_back({DgsCoverageKind::Excluded, "Gui presentation state", "Pure UI display state remains outside deterministic gameplay."});
    report.entries.push_back({DgsCoverageKind::Excluded, "ED/Staff/Result", "Post-game UI paths are excluded."});
    report.entries.push_back({DgsCoverageKind::Excluded, "Renderer/GLES/screenshot textures", "Render pipeline resources are excluded."});
    report.entries.push_back({DgsCoverageKind::Excluded, "Window/input raw device state", "Platform input and window state are excluded."});
    report.entries.push_back({DgsCoverageKind::Excluded, "Relay/socket/debug network", "Transport state is outside deterministic gameplay."});
    report.entries.push_back({DgsCoverageKind::Excluded, "Chain graph", "Callback graph ownership is intentionally excluded."});
    report.entries.push_back({DgsCoverageKind::Excluded, "Replay input stream", "Input-based replay stays unchanged in phase 1."});

    report.entries.push_back({DgsCoverageKind::Unresolved, "Player function and chain pointers", "Stored in opaque state for in-process restore only."});
    report.entries.push_back({DgsCoverageKind::Unresolved, "Effect update callbacks", "Stored in opaque state for in-process restore only."});
    report.entries.push_back({DgsCoverageKind::Unresolved, "EnemyEclContext.funcSetFunc", "Only audited, not normalized yet."});
    report.entries.push_back({DgsCoverageKind::Unresolved, "BulletManager laser VM script refs", "Still rely on opaque in-process state."});
    report.entries.push_back({DgsCoverageKind::Unresolved, "Ecl sub-table entries", "Only the base relation is normalized in phase 1."});
    return report;
}

uint64_t FingerprintDeterministicGameplayState(const DeterministicGameplayState &state)
{
    return BuildFingerprintSummary(state).Combined();
}

DgsSubsystemFingerprints FingerprintDeterministicGameplaySubsystems(const DeterministicGameplayState &state)
{
    const DgsFingerprintSummary summary = BuildFingerprintSummary(state);

    DgsSubsystemFingerprints result;
    result.gameManager = summary.gameManager;
    result.player1 = summary.player1;
    result.player2 = summary.player2;
    result.bulletManager = summary.bulletManager;
    result.enemyManager = summary.enemyManager;
    result.itemManager = summary.itemManager;
    result.effectManager = summary.effectManager;
    result.stageState = summary.stageState;
    result.eclManager = summary.eclManager;
    result.rng = summary.rng;
    result.runtime = summary.runtime;
    result.refs = summary.refs;
    result.unresolved = summary.unresolved;
    result.combined = summary.Combined();
    return result;
}

uint64_t FingerprintGameplaySnapshotOverlap(const Netplay::GameplaySnapshot &snapshot)
{
    DeterministicGameplayState state;
    CaptureDeterministicGameplayStateFromGameplaySnapshot(snapshot, state);
    return FingerprintDeterministicGameplayState(state);
}

DgsComparisonResult CompareDeterministicGameplayStateToLiveWorld(const DeterministicGameplayState &state)
{
    DeterministicGameplayState liveState;
    CaptureDeterministicGameplayState(liveState);

    const DgsFingerprintSummary expected = BuildFingerprintSummary(state);
    const DgsFingerprintSummary live = BuildFingerprintSummary(liveState);

    DgsComparisonResult result;
    result.expectedFingerprint = expected.Combined();
    result.liveFingerprint = live.Combined();
    result.matches = result.expectedFingerprint == result.liveFingerprint;

    if (expected.gameManager != live.gameManager)
    {
        result.mismatchedSubsystems.emplace_back("GameManager");
    }
    if (expected.player1 != live.player1 || expected.player2 != live.player2)
    {
        result.mismatchedSubsystems.emplace_back("Player");
    }
    if (expected.bulletManager != live.bulletManager)
    {
        result.mismatchedSubsystems.emplace_back("BulletManager");
    }
    if (expected.enemyManager != live.enemyManager)
    {
        result.mismatchedSubsystems.emplace_back("EnemyManager");
    }
    if (expected.itemManager != live.itemManager)
    {
        result.mismatchedSubsystems.emplace_back("ItemManager");
    }
    if (expected.effectManager != live.effectManager)
    {
        result.mismatchedSubsystems.emplace_back("EffectManager");
    }
    if (expected.stageState != live.stageState)
    {
        result.mismatchedSubsystems.emplace_back("Stage");
    }
    if (expected.eclManager != live.eclManager)
    {
        result.mismatchedSubsystems.emplace_back("EclManager");
    }
    if (expected.rng != live.rng)
    {
        result.mismatchedSubsystems.emplace_back("Rng");
    }
    if (expected.runtime != live.runtime)
    {
        result.mismatchedSubsystems.emplace_back("Runtime");
    }
    if (expected.refs != live.refs)
    {
        result.mismatchedSubsystems.emplace_back("Refs");
    }
    if (expected.unresolved != live.unresolved)
    {
        result.mismatchedSubsystems.emplace_back("Unresolved");
    }

    return result;
}

bool RoundTripDeterministicGameplayStateInProcess(uint64_t *outBefore, uint64_t *outAfter)
{
    DeterministicGameplayState original;
    CaptureDeterministicGameplayState(original);
    const uint64_t before = FingerprintDeterministicGameplayState(original);
    if (outBefore != nullptr)
    {
        *outBefore = before;
    }

    if (!RestoreDeterministicGameplayState(original))
    {
        if (outAfter != nullptr)
        {
            *outAfter = 0;
        }
        return false;
    }

    DeterministicGameplayState roundTripped;
    CaptureDeterministicGameplayState(roundTripped);
    const uint64_t after = FingerprintDeterministicGameplayState(roundTripped);
    if (outAfter != nullptr)
    {
        *outAfter = after;
    }

    return before == after;
}
} // namespace th06::DGS
