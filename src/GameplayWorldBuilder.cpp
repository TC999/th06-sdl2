#include "GameplayStatePortable.hpp"
#include "FileSystem.hpp"

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

template <typename T> void HashValue(u64 &hash, const T &value)
{
    const u8 *bytes = reinterpret_cast<const u8 *>(&value);
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
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

struct PortableEnemyShadowNode
{
    i32 bossRef = -1;
    i32 laserRefs[32] {};
    i32 effectRefs[12] {};
    i32 currentScriptOffset = -1;
    PortableEnemyFuncToken funcToken = PortableEnemyFuncToken::None;
    i32 life = 0;
    i32 maxLife = 0;
    i32 score = 0;
    D3DXVECTOR3 position {};
};

struct PortableEnemyShadowWorld
{
    i32 timelineOffset = -1;
    ZunTimer timelineTime {};
    std::vector<PortableEnemyShadowNode> enemies;
    std::vector<i32> subTableEntryOffsets;
};

struct PortablePlayerShadowNode
{
    i32 playerType = 0;
    i32 playerState = 0;
    i32 orbState = 0;
    bool isFocus = false;
    D3DXVECTOR3 positionCenter {};
    i16 previousFrameInput = 0;
    PortablePlayerFireFuncToken fireToken = PortablePlayerFireFuncToken::None;
    PortablePlayerFireFuncToken focusFireToken = PortablePlayerFireFuncToken::None;
    PortablePlayerBombFuncToken bombCalcToken = PortablePlayerBombFuncToken::None;
    PortablePlayerBombFuncToken bombDrawToken = PortablePlayerBombFuncToken::None;
    u32 activeBulletCount = 0;
};

struct PortablePlayerShadowWorld
{
    std::vector<PortablePlayerShadowNode> players;
    u32 totalPlayerBulletCount = 0;
};

struct PortableBulletShadowWorld
{
    i32 nextBulletIndex = 0;
    i32 encodedBulletCount = 0;
    ZunTimer time {};
    u32 activeBulletCount = 0;
    u32 activeLaserCount = 0;
};

struct PortableItemShadowWorld
{
    i32 nextIndex = 0;
    u32 encodedItemCount = 0;
    u32 activeItemCount = 0;
};

struct PortableEffectShadowWorld
{
    i32 nextIndex = 0;
    i32 encodedActiveEffects = 0;
    u32 activeEffectCount = 0;
};

struct PortableEclShadowCore
{
    std::string resourcePath;
    u64 resourceContentHash = 0;
    u32 resourceSizeBytes = 0;
    i32 subCount = 0;
    i32 mainCount = 0;
    std::array<i32, 3> timelineOffsets {{-1, -1, -1}};
    i32 activeTimelineSlot = -1;
    i32 activeTimelineOffset = -1;
    std::vector<i32> subTableEntryOffsets;
};

struct PortableStageShadowCore
{
    std::string stdPath;
    u64 stdContentHash = 0;
    u32 stdSizeBytes = 0;
    std::string anmPath;
    u64 anmContentHash = 0;
    u32 anmSizeBytes = 0;
    u32 stage = 0;
    i32 objectsCount = 0;
    i32 quadCount = 0;
    i32 beginningOfScriptOffset = -1;
    std::vector<i32> objectOffsets;
    D3DXVECTOR3 currentCameraFacingDir {};
    std::vector<u8> objectFlags;
    std::vector<PortableAnmVmState> quadVms;
    PortableAnmVmState spellcardBackground {};
    PortableAnmVmState extraBackground {};
};

void HashShadowWorld(u64 &hash, const PortableEnemyShadowWorld &world)
{
    HashValue(hash, world.timelineOffset);
    HashValue(hash, world.timelineTime);
    const u32 enemyCount = (u32)world.enemies.size();
    HashValue(hash, enemyCount);
    for (const auto &enemy : world.enemies)
    {
        HashValue(hash, enemy.bossRef);
        for (i32 value : enemy.laserRefs)
        {
            HashValue(hash, value);
        }
        for (i32 value : enemy.effectRefs)
        {
            HashValue(hash, value);
        }
        HashValue(hash, enemy.currentScriptOffset);
        HashValue(hash, (u16)enemy.funcToken);
        HashValue(hash, enemy.life);
        HashValue(hash, enemy.maxLife);
        HashValue(hash, enemy.score);
        HashValue(hash, enemy.position);
    }
    const u32 subCount = (u32)world.subTableEntryOffsets.size();
    HashValue(hash, subCount);
    for (i32 value : world.subTableEntryOffsets)
    {
        HashValue(hash, value);
    }
}

void HashShadowWorld(u64 &hash, const PortablePlayerShadowWorld &world)
{
    const u32 playerCount = (u32)world.players.size();
    HashValue(hash, playerCount);
    HashValue(hash, world.totalPlayerBulletCount);
    for (const auto &player : world.players)
    {
        HashValue(hash, player.playerType);
        HashValue(hash, player.playerState);
        HashValue(hash, player.orbState);
        HashValue(hash, player.isFocus);
        HashValue(hash, player.positionCenter);
        HashValue(hash, player.previousFrameInput);
        HashValue(hash, (u16)player.fireToken);
        HashValue(hash, (u16)player.focusFireToken);
        HashValue(hash, (u16)player.bombCalcToken);
        HashValue(hash, (u16)player.bombDrawToken);
        HashValue(hash, player.activeBulletCount);
    }
}

void HashShadowWorld(u64 &hash, const PortableBulletShadowWorld &world)
{
    HashValue(hash, world.nextBulletIndex);
    HashValue(hash, world.encodedBulletCount);
    HashValue(hash, world.time);
    HashValue(hash, world.activeBulletCount);
    HashValue(hash, world.activeLaserCount);
}

void HashShadowWorld(u64 &hash, const PortableItemShadowWorld &world)
{
    HashValue(hash, world.nextIndex);
    HashValue(hash, world.encodedItemCount);
    HashValue(hash, world.activeItemCount);
}

void HashShadowWorld(u64 &hash, const PortableEffectShadowWorld &world)
{
    HashValue(hash, world.nextIndex);
    HashValue(hash, world.encodedActiveEffects);
    HashValue(hash, world.activeEffectCount);
}

void HashShadowWorld(u64 &hash, const PortableEclShadowCore &world)
{
    HashString(hash, world.resourcePath);
    HashValue(hash, world.resourceContentHash);
    HashValue(hash, world.resourceSizeBytes);
    HashValue(hash, world.subCount);
    HashValue(hash, world.mainCount);
    for (i32 offset : world.timelineOffsets)
    {
        HashValue(hash, offset);
    }
    HashValue(hash, world.activeTimelineSlot);
    HashValue(hash, world.activeTimelineOffset);
    const u32 subCount = (u32)world.subTableEntryOffsets.size();
    HashValue(hash, subCount);
    for (i32 value : world.subTableEntryOffsets)
    {
        HashValue(hash, value);
    }
}

bool EqualAnmVmRefs(const DgsAnmVmRefs &lhs, const DgsAnmVmRefs &rhs)
{
    return lhs.scriptSlot == rhs.scriptSlot && lhs.beginningOfScript.value == rhs.beginningOfScript.value &&
           lhs.currentInstruction.value == rhs.currentInstruction.value && lhs.spriteIndex.value == rhs.spriteIndex.value;
}

void HashPortableAnmVmState(u64 &hash, const PortableAnmVmState &state)
{
    HashValue(hash, state.rotation);
    HashValue(hash, state.angleVel);
    HashValue(hash, state.scaleY);
    HashValue(hash, state.scaleX);
    HashValue(hash, state.scaleInterpFinalY);
    HashValue(hash, state.scaleInterpFinalX);
    HashValue(hash, state.uvScrollPos);
    HashValue(hash, state.currentTimeInScript);
    HashValue(hash, state.matrix);
    HashValue(hash, state.color);
    HashValue(hash, state.flags);
    HashValue(hash, state.alphaInterpEndTime);
    HashValue(hash, state.scaleInterpEndTime);
    HashValue(hash, state.autoRotate);
    HashValue(hash, state.pendingInterrupt);
    HashValue(hash, state.posInterpEndTime);
    HashValue(hash, state.pos);
    HashValue(hash, state.scaleInterpInitialY);
    HashValue(hash, state.scaleInterpInitialX);
    HashValue(hash, state.scaleInterpTime);
    HashValue(hash, state.activeSpriteIndex);
    HashValue(hash, state.baseSpriteIndex);
    HashValue(hash, state.anmFileIndex);
    HashValue(hash, state.alphaInterpInitial);
    HashValue(hash, state.alphaInterpFinal);
    HashValue(hash, state.posInterpInitial);
    HashValue(hash, state.posInterpFinal);
    HashValue(hash, state.posOffset);
    HashValue(hash, state.posInterpTime);
    HashValue(hash, state.timeOfLastSpriteSet);
    HashValue(hash, state.alphaInterpTime);
    HashValue(hash, state.fontWidth);
    HashValue(hash, state.fontHeight);
    HashValue(hash, state.refs.scriptSlot);
    HashValue(hash, state.refs.beginningOfScript.value);
    HashValue(hash, state.refs.currentInstruction.value);
    HashValue(hash, state.refs.spriteIndex.value);
}

void HashShadowWorld(u64 &hash, const PortableStageShadowCore &world)
{
    HashString(hash, world.stdPath);
    HashValue(hash, world.stdContentHash);
    HashValue(hash, world.stdSizeBytes);
    HashString(hash, world.anmPath);
    HashValue(hash, world.anmContentHash);
    HashValue(hash, world.anmSizeBytes);
    HashValue(hash, world.stage);
    HashValue(hash, world.objectsCount);
    HashValue(hash, world.quadCount);
    HashValue(hash, world.beginningOfScriptOffset);
    const u32 objectCount = (u32)world.objectOffsets.size();
    HashValue(hash, objectCount);
    for (i32 offset : world.objectOffsets)
    {
        HashValue(hash, offset);
    }
    HashValue(hash, world.currentCameraFacingDir);
    const u32 objectFlagCount = (u32)world.objectFlags.size();
    HashValue(hash, objectFlagCount);
    for (u8 flags : world.objectFlags)
    {
        HashValue(hash, flags);
    }
    const u32 quadCount = (u32)world.quadVms.size();
    HashValue(hash, quadCount);
    for (const auto &vm : world.quadVms)
    {
        HashPortableAnmVmState(hash, vm);
    }
    HashPortableAnmVmState(hash, world.spellcardBackground);
    HashPortableAnmVmState(hash, world.extraBackground);
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

bool BuildShadowStageCore(const PortableGameplayState &state, PortableStageShadowCore &core, std::vector<std::string> &notes)
{
    core.stdPath = state.stageCore.resource.stdPath;
    core.stdContentHash = state.stageCore.resource.stdContentHash;
    core.stdSizeBytes = state.stageCore.resource.stdSizeBytes;
    core.anmPath = state.stageCore.resource.anmPath;
    core.anmContentHash = state.stageCore.resource.anmContentHash;
    core.anmSizeBytes = state.stageCore.resource.anmSizeBytes;
    core.stage = state.stageCore.stage;
    core.objectsCount = 0;
    core.quadCount = 0;
    core.beginningOfScriptOffset = -1;
    core.objectOffsets.clear();
    core.currentCameraFacingDir = state.stageCore.currentCameraFacingDir;
    core.objectFlags = state.stageCore.objectFlags;
    core.quadVms = state.stageCore.quadVms;
    core.spellcardBackground = state.stageCore.spellcardBackground;
    core.extraBackground = state.stageCore.extraBackground;

    if (!state.stageCore.hasStageData)
    {
        notes.emplace_back("Shadow build could not reconstruct stage core because no stage data was captured.");
        return false;
    }
    if (state.stageCore.resource.stdPath.empty() || state.stageCore.resource.anmPath.empty())
    {
        notes.emplace_back("Shadow build stage core is missing canonical stage resource paths.");
        return false;
    }

    std::vector<u8> stdBytes;
    if (!ReadPortableResourceBytes(state.stageCore.resource.stdPath.c_str(), stdBytes))
    {
        notes.emplace_back("Shadow build could not load the portable stage STD resource.");
        return false;
    }
    std::vector<u8> anmBytes;
    if (!ReadPortableResourceBytes(state.stageCore.resource.anmPath.c_str(), anmBytes))
    {
        notes.emplace_back("Shadow build could not load the portable stage ANM resource.");
        return false;
    }

    if (stdBytes.size() < sizeof(RawStageHeader))
    {
        notes.emplace_back("Shadow build found an invalid portable stage STD header.");
        return false;
    }
    if (HashByteSpan(stdBytes.data(), stdBytes.size()) != state.stageCore.resource.stdContentHash ||
        (u32)stdBytes.size() != state.stageCore.resource.stdSizeBytes)
    {
        notes.emplace_back("Shadow build stage STD resource identity does not match the captured portable state.");
        return false;
    }
    if (HashByteSpan(anmBytes.data(), anmBytes.size()) != state.stageCore.resource.anmContentHash ||
        (u32)anmBytes.size() != state.stageCore.resource.anmSizeBytes)
    {
        notes.emplace_back("Shadow build stage ANM resource identity does not match the captured portable state.");
        return false;
    }

    const auto *rawHeader = reinterpret_cast<const RawStageHeader *>(stdBytes.data());
    core.objectsCount = rawHeader->nbObjects;
    core.quadCount = rawHeader->nbFaces;
    core.beginningOfScriptOffset = rawHeader->scriptOffset;
    const auto *objectTable = reinterpret_cast<const RawStageObject *const *>(stdBytes.data() + sizeof(RawStageHeader));
    core.objectOffsets.reserve((size_t)std::max(core.objectsCount, 0));
    for (i32 i = 0; i < core.objectsCount; ++i)
    {
        core.objectOffsets.push_back(RawPointerFieldToOffset(objectTable[i]));
    }

    if (core.stage != state.catalog.currentStage || core.stage != state.stageCore.stage)
    {
        notes.emplace_back("Shadow build stage core stage does not match the captured portable state.");
        return false;
    }
    if (core.objectsCount != state.stageCore.objectsCount || core.quadCount != state.stageCore.quadCount)
    {
        notes.emplace_back("Shadow build stage STD counts do not match the captured portable state.");
        return false;
    }
    if (core.beginningOfScriptOffset != state.stageRefs.beginningOfScript.value)
    {
        notes.emplace_back("Shadow build stage script offset does not match the captured stage refs.");
        return false;
    }
    if (core.objectOffsets.size() != state.stageRefs.objectOffsets.size())
    {
        notes.emplace_back("Shadow build stage object refs do not match the captured stage refs.");
        return false;
    }
    for (size_t i = 0; i < core.objectOffsets.size(); ++i)
    {
        if (core.objectOffsets[i] != state.stageRefs.objectOffsets[i].value)
        {
            notes.emplace_back("Shadow build stage object refs do not match the captured stage refs.");
            return false;
        }
    }
    if ((int)core.objectFlags.size() != state.stageCore.objectsCount ||
        (int)core.quadVms.size() != state.stageCore.quadCount)
    {
        notes.emplace_back("Shadow build stage runtime counts do not match the captured stage core.");
        return false;
    }
    if ((int)state.runtime.stageObjectInstances.size() != state.stageCore.objectsCount)
    {
        notes.emplace_back("Shadow build stage object instances do not match the captured stage core.");
        return false;
    }
    if (!EqualAnmVmRefs(core.spellcardBackground.refs, state.stageRefs.spellcardBackground) ||
        !EqualAnmVmRefs(core.extraBackground.refs, state.stageRefs.extraBackground))
    {
        notes.emplace_back("Shadow build stage background VM refs do not match the captured stage refs.");
        return false;
    }
    if (core.quadVms.size() != state.stageRefs.quadVmRefs.size())
    {
        notes.emplace_back("Shadow build stage quad VM refs do not match the captured stage refs.");
        return false;
    }
    for (size_t i = 0; i < core.quadVms.size(); ++i)
    {
        if (!EqualAnmVmRefs(core.quadVms[i].refs, state.stageRefs.quadVmRefs[i]))
        {
            notes.emplace_back("Shadow build stage quad VM refs do not match the captured stage refs.");
            return false;
        }
    }

    return true;
}

bool BuildShadowEclCore(const PortableGameplayState &state, PortableEclShadowCore &core, std::vector<std::string> &notes)
{
    core.resourcePath = state.eclCore.resource.resourcePath;
    core.resourceContentHash = state.eclCore.resource.resourceContentHash;
    core.resourceSizeBytes = state.eclCore.resource.resourceSizeBytes;
    core.subCount = 0;
    core.mainCount = 0;
    core.timelineOffsets = {{-1, -1, -1}};
    core.activeTimelineSlot = -1;
    core.activeTimelineOffset = -1;
    core.subTableEntryOffsets.clear();

    if (!state.eclCore.hasEclFile)
    {
        notes.emplace_back("Shadow build could not reconstruct ECL core because no ECL resource was captured.");
        return false;
    }
    if (state.eclCore.resource.resourcePath.empty())
    {
        notes.emplace_back("Shadow build could not reconstruct ECL core because the resource path is empty.");
        return false;
    }

    std::vector<u8> rawBytes;
    if (!ReadPortableResourceBytes(state.eclCore.resource.resourcePath.c_str(), rawBytes))
    {
        notes.emplace_back("Shadow build could not load the portable ECL resource.");
        return false;
    }

    if (rawBytes.size() < sizeof(EclRawHeader))
    {
        notes.emplace_back("Shadow build found an invalid portable ECL resource header.");
        return false;
    }

    const u64 contentHash = HashByteSpan(rawBytes.data(), rawBytes.size());
    if (contentHash != state.eclCore.resource.resourceContentHash)
    {
        notes.emplace_back("Shadow build ECL resource hash does not match the captured portable state.");
        return false;
    }

    if ((u32)rawBytes.size() != state.eclCore.resource.resourceSizeBytes)
    {
        notes.emplace_back("Shadow build ECL resource size does not match the captured portable state.");
        return false;
    }

    const auto *rawHeader = reinterpret_cast<const EclRawHeader *>(rawBytes.data());
    core.subCount = rawHeader->subCount;
    core.mainCount = rawHeader->mainCount;
    for (size_t i = 0; i < core.timelineOffsets.size(); ++i)
    {
        core.timelineOffsets[i] = RawPointerFieldToOffset(rawHeader->timelineOffsets[i]);
    }
    core.activeTimelineOffset = state.eclCore.activeTimelineOffset;
    for (size_t i = 0; i < core.timelineOffsets.size(); ++i)
    {
        if (core.timelineOffsets[i] == core.activeTimelineOffset)
        {
            core.activeTimelineSlot = (i32)i;
            break;
        }
    }

    core.subTableEntryOffsets.reserve((size_t)std::max(core.subCount, 0));
    for (i32 i = 0; i < core.subCount; ++i)
    {
        core.subTableEntryOffsets.push_back(RawPointerFieldToOffset(rawHeader->subOffsets[i]));
    }

    if (core.resourcePath != state.eclCore.resource.resourcePath)
    {
        notes.emplace_back("Shadow build ECL resource path does not match the captured portable state.");
        return false;
    }
    if (core.subCount != state.eclCore.subCount || core.mainCount != state.eclCore.mainCount)
    {
        notes.emplace_back("Shadow build ECL header counts do not match the captured portable state.");
        return false;
    }
    if (core.timelineOffsets != state.eclCore.timelineOffsets)
    {
        notes.emplace_back("Shadow build ECL timeline offsets do not match the captured portable state.");
        return false;
    }
    if (core.activeTimelineSlot != state.eclCore.activeTimelineSlot)
    {
        notes.emplace_back("Shadow build ECL active timeline slot does not match the captured portable state.");
        return false;
    }
    if (core.activeTimelineOffset != state.eclCore.activeTimelineOffset)
    {
        notes.emplace_back("Shadow build ECL active timeline offset does not match the captured portable state.");
        return false;
    }
    if (core.subTableEntryOffsets != state.eclCore.subTableEntryOffsets)
    {
        notes.emplace_back("Shadow build ECL subtable entries do not match the captured portable state.");
        return false;
    }

    return true;
}

bool BuildShadowEnemyWorld(const PortableGameplayState &state, PortableEnemyShadowWorld &world,
                           std::vector<std::string> &notes)
{
    const PortableEnemyManagerState &actors = state.enemyActors;
    world.timelineOffset = actors.timelineOffset;
    world.timelineTime = actors.timelineTime;
    world.subTableEntryOffsets = state.eclCore.subTableEntryOffsets;
    world.enemies.resize(actors.enemies.size());

    for (size_t i = 0; i < actors.enemies.size(); ++i)
    {
        const PortableEnemyState &enemy = actors.enemies[i];
        PortableEnemyShadowNode &node = world.enemies[i];
        node.currentScriptOffset = enemy.currentContext.scriptOffset;
        node.funcToken = enemy.currentContext.funcToken;
        node.life = enemy.life;
        node.maxLife = enemy.maxLife;
        node.score = enemy.score;
        node.position = enemy.position;
        for (size_t laserIdx = 0; laserIdx < std::size(node.laserRefs); ++laserIdx)
        {
            node.laserRefs[laserIdx] = enemy.laserIndices[laserIdx];
        }
        for (size_t effectIdx = 0; effectIdx < std::size(node.effectRefs); ++effectIdx)
        {
            node.effectRefs[effectIdx] = enemy.effectIndices[effectIdx];
        }

        bool isBoss = false;
        for (size_t bossSlot = 0; bossSlot < actors.bossIndices.size(); ++bossSlot)
        {
            if (actors.bossIndices[bossSlot] == (i32)i)
            {
                node.bossRef = (i32)bossSlot;
                isBoss = true;
                break;
            }
        }
        if (!isBoss)
        {
            node.bossRef = -1;
        }

        if (enemy.currentContext.hasUnknownFuncToken)
        {
            notes.emplace_back("Shadow build encountered an enemy with an unknown func token.");
            return false;
        }
    }

    return true;
}

bool BuildShadowPlayerWorld(const PortableGameplayState &state, PortablePlayerShadowWorld &world,
                            std::vector<std::string> &notes)
{
    world.players.clear();
    world.totalPlayerBulletCount = 0;
    for (const auto &player : state.players)
    {
        if (!player.isPresent)
        {
            continue;
        }

        PortablePlayerShadowNode node;
        node.playerType = player.playerType;
        node.playerState = player.playerState;
        node.orbState = player.orbState;
        node.isFocus = player.isFocus != 0;
        node.positionCenter = player.positionCenter;
        node.previousFrameInput = player.previousFrameInput;
        node.fireToken = player.fireBulletToken;
        node.focusFireToken = player.fireBulletFocusToken;
        node.bombCalcToken = player.bombInfo.calcToken;
        node.bombDrawToken = player.bombInfo.drawToken;
        for (const auto &bullet : player.bullets)
        {
            if (bullet.bulletState != BULLET_STATE_UNUSED)
            {
                ++node.activeBulletCount;
                ++world.totalPlayerBulletCount;
            }
        }

        if (player.hasUnknownFireToken || player.hasUnknownFocusFireToken || player.bombInfo.hasUnknownCalcToken ||
            player.bombInfo.hasUnknownDrawToken)
        {
            notes.emplace_back("Shadow build encountered a player with an unknown fire or bomb token.");
            return false;
        }

        world.players.push_back(node);
    }
    return true;
}

bool BuildShadowBulletWorld(const PortableGameplayState &state, PortableBulletShadowWorld &world,
                            std::vector<std::string> &notes)
{
    world.nextBulletIndex = state.bulletActors.nextBulletIndex;
    world.encodedBulletCount = state.bulletActors.bulletCount;
    world.time = state.bulletActors.time;
    world.activeBulletCount = 0;
    world.activeLaserCount = 0;

    if (world.nextBulletIndex < 0 || world.nextBulletIndex > (i32)state.bulletActors.bullets.size())
    {
        notes.emplace_back("Shadow build encountered an out-of-range bullet manager nextBulletIndex.");
        return false;
    }

    for (const auto &bullet : state.bulletActors.bullets)
    {
        if (bullet.state != 0)
        {
            ++world.activeBulletCount;
        }
    }
    for (const auto &laser : state.bulletActors.lasers)
    {
        if (laser.inUse != 0)
        {
            ++world.activeLaserCount;
        }
    }
    return true;
}

bool BuildShadowItemWorld(const PortableGameplayState &state, PortableItemShadowWorld &world, std::vector<std::string> &notes)
{
    world.nextIndex = state.itemActors.nextIndex;
    world.encodedItemCount = state.itemActors.itemCount;
    world.activeItemCount = 0;

    if (world.nextIndex < 0 || world.nextIndex > (i32)state.itemActors.items.size())
    {
        notes.emplace_back("Shadow build encountered an out-of-range item manager nextIndex.");
        return false;
    }

    for (const auto &item : state.itemActors.items)
    {
        if (item.isInUse != 0)
        {
            ++world.activeItemCount;
        }
    }
    return true;
}

bool BuildShadowEffectWorld(const PortableGameplayState &state, PortableEffectShadowWorld &world,
                            std::vector<std::string> &notes)
{
    world.nextIndex = state.effectActors.nextIndex;
    world.encodedActiveEffects = state.effectActors.activeEffects;
    world.activeEffectCount = 0;

    if (world.nextIndex < 0 || world.nextIndex > (i32)state.effectActors.effects.size())
    {
        notes.emplace_back("Shadow build encountered an out-of-range effect manager nextIndex.");
        return false;
    }

    for (const auto &effect : state.effectActors.effects)
    {
        if (effect.hasUnknownUpdateToken)
        {
            notes.emplace_back("Shadow build encountered an effect with an unknown update token.");
            return false;
        }
        if (effect.inUseFlag != 0)
        {
            ++world.activeEffectCount;
        }
    }
    return true;
}

} // namespace

void BuildPortableGameplayWorldFromState(const PortableGameplayState &state, PortableGameplayBuildResult &result)
{
    TracePortableValidationPhase("build-readiness-start");
    result.notes.clear();
    EvaluatePortableRestoreReadiness(state, result.evaluation);
    TracePortableValidationPhase("build-readiness-end");
    result.success = false;
    result.shadowFingerprint = 0;
    result.builtEnemyCount = 0;
    result.builtPlayerCount = 0;
    result.builtPlayerBulletCount = 0;
    result.builtBulletCount = 0;
    result.builtLaserCount = 0;
    result.builtItemCount = 0;
    result.builtEffectCount = 0;
    result.builtEclSubCount = 0;
    result.builtEclTimelineSlot = -1;
    result.builtStageObjectCount = 0;
    result.builtStageQuadCount = 0;

    if (!HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitActors) ||
        !HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitEclCore) ||
        !HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitStageCore) ||
        !HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitPlayers) ||
        !HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitBullets) ||
        !HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitItems) ||
        !HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitEffects))
    {
        result.notes.emplace_back("Portable actor, ECL-core, or Stage-core sections are missing.");
        return;
    }

    PortableEnemyShadowWorld shadowWorld;
    PortablePlayerShadowWorld playerShadowWorld;
    PortableBulletShadowWorld bulletShadowWorld;
    PortableItemShadowWorld itemShadowWorld;
    PortableEffectShadowWorld effectShadowWorld;
    PortableEclShadowCore eclShadowCore;
    PortableStageShadowCore stageShadowCore;
    TracePortableValidationPhase("build-ecl-core-start");
    if (!BuildShadowEclCore(state, eclShadowCore, result.notes))
    {
        TracePortableValidationPhase("build-ecl-core-fail");
        if (result.notes.empty())
        {
            result.notes.emplace_back("Portable ECL core shadow build failed.");
        }
        return;
    }
    TracePortableValidationPhase("build-ecl-core-end");
    TracePortableValidationPhase("build-stage-core-start");
    if (!BuildShadowStageCore(state, stageShadowCore, result.notes))
    {
        TracePortableValidationPhase("build-stage-core-fail");
        if (result.notes.empty())
        {
            result.notes.emplace_back("Portable stage core shadow build failed.");
        }
        return;
    }
    TracePortableValidationPhase("build-stage-core-end");

    TracePortableValidationPhase("build-shadow-start");
    if (!BuildShadowEnemyWorld(state, shadowWorld, result.notes))
    {
        TracePortableValidationPhase("build-shadow-fail");
        if (result.notes.empty())
        {
            result.notes.emplace_back("Portable enemy shadow world build failed.");
        }
        return;
    }
    if (!BuildShadowPlayerWorld(state, playerShadowWorld, result.notes))
    {
        TracePortableValidationPhase("build-shadow-fail");
        if (result.notes.empty())
        {
            result.notes.emplace_back("Portable player shadow world build failed.");
        }
        return;
    }
    if (!BuildShadowBulletWorld(state, bulletShadowWorld, result.notes))
    {
        TracePortableValidationPhase("build-shadow-fail");
        if (result.notes.empty())
        {
            result.notes.emplace_back("Portable bullet shadow world build failed.");
        }
        return;
    }
    if (!BuildShadowItemWorld(state, itemShadowWorld, result.notes))
    {
        TracePortableValidationPhase("build-shadow-fail");
        if (result.notes.empty())
        {
            result.notes.emplace_back("Portable item shadow world build failed.");
        }
        return;
    }
    if (!BuildShadowEffectWorld(state, effectShadowWorld, result.notes))
    {
        TracePortableValidationPhase("build-shadow-fail");
        if (result.notes.empty())
        {
            result.notes.emplace_back("Portable effect shadow world build failed.");
        }
        return;
    }
    TracePortableValidationPhase("build-shadow-end");

    TracePortableValidationPhase("build-hash-start");
    u64 hash = kFnvOffset;
    HashShadowWorld(hash, eclShadowCore);
    HashShadowWorld(hash, stageShadowCore);
    HashShadowWorld(hash, shadowWorld);
    HashShadowWorld(hash, playerShadowWorld);
    HashShadowWorld(hash, bulletShadowWorld);
    HashShadowWorld(hash, itemShadowWorld);
    HashShadowWorld(hash, effectShadowWorld);
    HashString(hash, state.enemyActors.stgEnmAnmFilename);
    HashString(hash, state.enemyActors.stgEnm2AnmFilename);
    HashString(hash, state.bulletActors.bulletAnmPath);
    HashValue(hash, state.enemyActors.enemyCount);
    HashValue(hash, state.enemyActors.randomItemSpawnIndex);
    HashValue(hash, state.enemyActors.randomItemTableIndex);
    HashValue(hash, state.enemyActors.spellcardInfo.isCapturing);
    HashValue(hash, state.enemyActors.spellcardInfo.isActive);
    HashValue(hash, state.enemyActors.spellcardInfo.captureScore);
    HashValue(hash, state.enemyActors.spellcardInfo.idx);
    HashValue(hash, state.enemyActors.spellcardInfo.usedBomb);
    HashValue(hash, state.enemyActors.unk_ee5d8);
    TracePortableValidationPhase("build-hash-end");

    result.shadowFingerprint = hash;
    result.builtEnemyCount = (u32)shadowWorld.enemies.size();
    result.builtPlayerCount = (u32)playerShadowWorld.players.size();
    result.builtPlayerBulletCount = playerShadowWorld.totalPlayerBulletCount;
    result.builtBulletCount = bulletShadowWorld.activeBulletCount;
    result.builtLaserCount = bulletShadowWorld.activeLaserCount;
    result.builtItemCount = itemShadowWorld.activeItemCount;
    result.builtEffectCount = effectShadowWorld.activeEffectCount;
    result.builtEclSubCount = (u32)eclShadowCore.subTableEntryOffsets.size();
    result.builtEclTimelineSlot = eclShadowCore.activeTimelineSlot;
    result.builtStageObjectCount = (u32)stageShadowCore.objectOffsets.size();
    result.builtStageQuadCount = (u32)stageShadowCore.quadVms.size();
    result.success = true;
    result.notes.emplace_back("Built portable enemy/player/bullet/item/effect/ECL-core/Stage-core shadow worlds for validation only.");

    if (result.evaluation.readiness != PortableRestoreReadiness::Ready)
    {
        result.notes.emplace_back("Full portable restore is still blocked outside the currently normalized chains.");
    }
}

PortableGameplayBuildResult BuildPortableGameplayWorldFromState(const PortableGameplayState &state)
{
    auto result = std::make_unique<PortableGameplayBuildResult>();
    BuildPortableGameplayWorldFromState(state, *result);
    return *result;
}
} // namespace th06::DGS
