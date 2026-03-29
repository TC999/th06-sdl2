#include "GameplayStatePortable.hpp"
#include "FileSystem.hpp"
#include "GameErrorContext.hpp"
#include "GamePaths.hpp"
#include "thprac_th06.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
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
bool g_PortableValidationTraceEnabled = false;

void AppendPortableValidationTraceLine(const char *phase)
{
    if (!THPrac::TH06::THPracIsDebugLogEnabled())
    {
        return;
    }

    char line[256];
    std::snprintf(line, sizeof(line), "[PortableValidationTrace] %s\n", phase != nullptr ? phase : "(null)");
    GameErrorContext::Log(&g_GameErrorContext, "%s", line);

    char resolvedLogPath[512];
    GamePaths::Resolve(resolvedLogPath, sizeof(resolvedLogPath), "./log.txt");
    GamePaths::EnsureParentDir(resolvedLogPath);

    FILE *file = nullptr;
    if (fopen_s(&file, resolvedLogPath, "a") != 0 || file == nullptr)
    {
        return;
    }

    std::fprintf(file, "%s", line);
    std::fflush(file);
    std::fclose(file);
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

template <typename T, size_t N> void HashArray(u64 &hash, const std::array<T, N> &values)
{
    for (const auto &value : values)
    {
        HashValue(hash, value);
    }
}

bool EqualAnmVmRefs(const DgsAnmVmRefs &lhs, const DgsAnmVmRefs &rhs)
{
    return lhs.scriptSlot == rhs.scriptSlot && lhs.beginningOfScript.value == rhs.beginningOfScript.value &&
           lhs.currentInstruction.value == rhs.currentInstruction.value && lhs.spriteIndex.value == rhs.spriteIndex.value;
}

void HashAnmVmRefs(u64 &hash, const DgsAnmVmRefs &refs)
{
    HashValue(hash, refs.scriptSlot);
    HashValue(hash, refs.beginningOfScript.value);
    HashValue(hash, refs.currentInstruction.value);
    HashValue(hash, refs.spriteIndex.value);
}

void HashEnemyContextRefs(u64 &hash, const DgsEnemyContextRefs &refs)
{
    HashValue(hash, refs.currentInstruction.value);
    HashValue(hash, refs.hasFuncSetFunc);
    HashValue(hash, refs.unresolvedFuncSetFunc);
}

void HashDgsEnemyRefs(u64 &hash, const DgsEnemyManagerRefs &refs)
{
    for (const auto &bossIndex : refs.bossIndices)
    {
        HashValue(hash, bossIndex.value);
    }
    HashValue(hash, refs.timelineOffset.value);
    const u32 enemyCount = (u32)refs.enemyRefs.size();
    HashValue(hash, enemyCount);
    for (const auto &enemyRefs : refs.enemyRefs)
    {
        HashAnmVmRefs(hash, enemyRefs.primaryVm);
        for (const auto &vmRefs : enemyRefs.vms)
        {
            HashAnmVmRefs(hash, vmRefs);
        }
        HashEnemyContextRefs(hash, enemyRefs.currentContext);
        for (const auto &contextRefs : enemyRefs.savedContextStack)
        {
            HashEnemyContextRefs(hash, contextRefs);
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
}

void HashDgsStageRefs(u64 &hash, const DgsStageRefs &refs)
{
    HashValue(hash, refs.beginningOfScript.value);
    const u32 objectCount = (u32)refs.objectOffsets.size();
    HashValue(hash, objectCount);
    for (const auto &objectOffset : refs.objectOffsets)
    {
        HashValue(hash, objectOffset.value);
    }
    const u32 quadCount = (u32)refs.quadVmRefs.size();
    HashValue(hash, quadCount);
    for (const auto &vmRefs : refs.quadVmRefs)
    {
        HashAnmVmRefs(hash, vmRefs);
    }
    HashAnmVmRefs(hash, refs.spellcardBackground);
    HashAnmVmRefs(hash, refs.extraBackground);
}

void HashPortableEnemyFlags(u64 &hash, const PortableEnemyFlagsState &flags)
{
    HashValue(hash, flags.unk1);
    HashValue(hash, flags.unk2);
    HashValue(hash, flags.unk3);
    HashValue(hash, flags.unk4);
    HashValue(hash, flags.unk5);
    HashValue(hash, flags.unk6);
    HashValue(hash, flags.unk7);
    HashValue(hash, flags.unk8);
    HashValue(hash, flags.isBoss);
    HashValue(hash, flags.unk10);
    HashValue(hash, flags.unk11);
    HashValue(hash, flags.shouldClampPos);
    HashValue(hash, flags.unk13);
    HashValue(hash, flags.unk14);
    HashValue(hash, flags.unk15);
    HashValue(hash, flags.unk16);
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
    HashAnmVmRefs(hash, state.refs);
}

void HashPortableEnemyContext(u64 &hash, const PortableEnemyEclContextState &state)
{
    HashValue(hash, state.scriptOffset);
    HashValue(hash, state.time);
    HashValue(hash, (u16)state.funcToken);
    HashValue(hash, state.hasFuncToken);
    HashValue(hash, state.hasUnknownFuncToken);
    HashValue(hash, state.var0);
    HashValue(hash, state.var1);
    HashValue(hash, state.var2);
    HashValue(hash, state.var3);
    HashValue(hash, state.float0);
    HashValue(hash, state.float1);
    HashValue(hash, state.float2);
    HashValue(hash, state.float3);
    HashValue(hash, state.var4);
    HashValue(hash, state.var5);
    HashValue(hash, state.var6);
    HashValue(hash, state.var7);
    HashValue(hash, state.compareRegister);
    HashValue(hash, state.subId);
}

void HashPortableEnemyBulletShooter(u64 &hash, const PortableEnemyBulletShooterState &state)
{
    HashValue(hash, state.sprite);
    HashValue(hash, state.spriteOffset);
    HashValue(hash, state.position);
    HashValue(hash, state.angle1);
    HashValue(hash, state.angle2);
    HashValue(hash, state.speed1);
    HashValue(hash, state.speed2);
    for (f32 value : state.exFloats)
    {
        HashValue(hash, value);
    }
    for (i32 value : state.exInts)
    {
        HashValue(hash, value);
    }
    HashValue(hash, state.unk_40);
    HashValue(hash, state.count1);
    HashValue(hash, state.count2);
    HashValue(hash, state.aimMode);
    HashValue(hash, state.unk_4a);
    HashValue(hash, state.flags);
    HashValue(hash, state.provokedPlayer);
    HashValue(hash, (i32)state.sfx);
}

void HashPortableEnemyLaserShooter(u64 &hash, const PortableEnemyLaserShooterState &state)
{
    HashValue(hash, state.sprite);
    HashValue(hash, state.spriteOffset);
    HashValue(hash, state.position);
    HashValue(hash, state.angle);
    HashValue(hash, state.unk_14);
    HashValue(hash, state.speed);
    HashValue(hash, state.unk_1c);
    HashValue(hash, state.startOffset);
    HashValue(hash, state.endOffset);
    HashValue(hash, state.startLength);
    HashValue(hash, state.width);
    HashValue(hash, state.startTime);
    HashValue(hash, state.duration);
    HashValue(hash, state.despawnDuration);
    HashValue(hash, state.hitboxStartTime);
    HashValue(hash, state.hitboxEndDelay);
    HashValue(hash, state.unk_44);
    HashValue(hash, state.type);
    HashValue(hash, state.flags);
    HashValue(hash, state.unk_50);
    HashValue(hash, state.provokedPlayer);
}

void HashPortableEnemyState(u64 &hash, const PortableEnemyState &state)
{
    HashPortableAnmVmState(hash, state.primaryVm);
    for (const auto &vm : state.vms)
    {
        HashPortableAnmVmState(hash, vm);
    }
    HashPortableEnemyContext(hash, state.currentContext);
    for (const auto &context : state.savedContextStack)
    {
        HashPortableEnemyContext(hash, context);
    }
    HashValue(hash, state.stackDepth);
    HashValue(hash, state.unk_c40);
    HashValue(hash, state.deathCallbackSub);
    for (i32 value : state.interrupts)
    {
        HashValue(hash, value);
    }
    HashValue(hash, state.runInterrupt);
    HashValue(hash, state.position);
    HashValue(hash, state.hitboxDimensions);
    HashValue(hash, state.axisSpeed);
    HashValue(hash, state.angle);
    HashValue(hash, state.angularVelocity);
    HashValue(hash, state.speed);
    HashValue(hash, state.acceleration);
    HashValue(hash, state.shootOffset);
    HashValue(hash, state.moveInterp);
    HashValue(hash, state.moveInterpStartPos);
    HashValue(hash, state.moveInterpTimer);
    HashValue(hash, state.moveInterpStartTime);
    HashValue(hash, state.bulletRankSpeedLow);
    HashValue(hash, state.bulletRankSpeedHigh);
    HashValue(hash, state.bulletRankAmount1Low);
    HashValue(hash, state.bulletRankAmount1High);
    HashValue(hash, state.bulletRankAmount2Low);
    HashValue(hash, state.bulletRankAmount2High);
    HashValue(hash, state.life);
    HashValue(hash, state.maxLife);
    HashValue(hash, state.score);
    HashValue(hash, state.bossTimer);
    HashValue(hash, state.color);
    HashPortableEnemyBulletShooter(hash, state.bulletProps);
    HashValue(hash, state.shootInterval);
    HashValue(hash, state.shootIntervalTimer);
    HashPortableEnemyLaserShooter(hash, state.laserProps);
    HashArray(hash, state.laserIndices);
    HashValue(hash, state.laserStore);
    HashValue(hash, state.deathAnm1);
    HashValue(hash, state.deathAnm2);
    HashValue(hash, state.deathAnm3);
    HashValue(hash, state.itemDrop);
    HashValue(hash, state.bossId);
    HashValue(hash, state.unk_e41);
    HashValue(hash, state.exInsFunc10Timer);
    HashPortableEnemyFlags(hash, state.flags);
    HashValue(hash, state.anmExFlags);
    HashValue(hash, state.anmExDefaults);
    HashValue(hash, state.anmExFarLeft);
    HashValue(hash, state.anmExFarRight);
    HashValue(hash, state.anmExLeft);
    HashValue(hash, state.anmExRight);
    HashValue(hash, state.lowerMoveLimit);
    HashValue(hash, state.upperMoveLimit);
    HashArray(hash, state.effectIndices);
    HashValue(hash, state.effectIdx);
    HashValue(hash, state.effectDistance);
    HashValue(hash, state.lifeCallbackThreshold);
    HashValue(hash, state.lifeCallbackSub);
    HashValue(hash, state.timerCallbackThreshold);
    HashValue(hash, state.timerCallbackSub);
    HashValue(hash, state.exInsFunc6Angle);
    HashValue(hash, state.exInsFunc6Timer);
    HashValue(hash, state.provokedPlayer);
}

void HashPortableEnemyManagerState(u64 &hash, const PortableEnemyManagerState &state)
{
    HashString(hash, state.stgEnmAnmFilename);
    HashString(hash, state.stgEnm2AnmFilename);
    HashPortableEnemyState(hash, state.enemyTemplate);
    const u32 enemyCount = (u32)state.enemies.size();
    HashValue(hash, enemyCount);
    for (const auto &enemy : state.enemies)
    {
        HashPortableEnemyState(hash, enemy);
    }
    HashArray(hash, state.bossIndices);
    HashValue(hash, state.randomItemSpawnIndex);
    HashValue(hash, state.randomItemTableIndex);
    HashValue(hash, state.enemyCount);
    HashValue(hash, state.spellcardInfo.isCapturing);
    HashValue(hash, state.spellcardInfo.isActive);
    HashValue(hash, state.spellcardInfo.captureScore);
    HashValue(hash, state.spellcardInfo.idx);
    HashValue(hash, state.spellcardInfo.usedBomb);
    HashValue(hash, state.unk_ee5d8);
    HashValue(hash, state.timelineOffset);
    HashValue(hash, state.timelineTime);
}

void HashPlayerRect(u64 &hash, const PlayerRect &rect)
{
    HashValue(hash, rect.posX);
    HashValue(hash, rect.posY);
    HashValue(hash, rect.sizeX);
    HashValue(hash, rect.sizeY);
}

void HashPortablePlayerBulletState(u64 &hash, const PortablePlayerBulletState &state)
{
    HashPortableAnmVmState(hash, state.sprite);
    HashValue(hash, state.position);
    HashValue(hash, state.size);
    HashValue(hash, state.velocity);
    HashValue(hash, state.sidewaysMotion);
    HashValue(hash, state.unk_134);
    HashValue(hash, state.timer);
    HashValue(hash, state.damage);
    HashValue(hash, state.bulletState);
    HashValue(hash, state.bulletType);
    HashValue(hash, state.unk_152);
    HashValue(hash, state.spawnPositionIdx);
}

void HashPortablePlayerBombState(u64 &hash, const PortablePlayerBombState &state)
{
    HashValue(hash, state.isInUse);
    HashValue(hash, state.duration);
    HashValue(hash, state.timer);
    HashValue(hash, (u16)state.calcToken);
    HashValue(hash, (u16)state.drawToken);
    HashValue(hash, state.hasUnknownCalcToken);
    HashValue(hash, state.hasUnknownDrawToken);
    HashArray(hash, state.reimuABombProjectilesState);
    for (f32 value : state.reimuABombProjectilesRelated)
    {
        HashValue(hash, value);
    }
    for (const auto &value : state.bombRegionPositions)
    {
        HashValue(hash, value);
    }
    for (const auto &value : state.bombRegionVelocities)
    {
        HashValue(hash, value);
    }
    for (const auto &ring : state.sprites)
    {
        for (const auto &sprite : ring)
        {
            HashPortableAnmVmState(hash, sprite);
        }
    }
}

void HashPortableCharacterDataState(u64 &hash, const PortableCharacterDataState &state)
{
    HashValue(hash, state.orthogonalMovementSpeed);
    HashValue(hash, state.orthogonalMovementSpeedFocus);
    HashValue(hash, state.diagonalMovementSpeed);
    HashValue(hash, state.diagonalMovementSpeedFocus);
    HashValue(hash, (u16)state.fireBulletToken);
    HashValue(hash, (u16)state.fireBulletFocusToken);
    HashValue(hash, state.hasUnknownFireToken);
    HashValue(hash, state.hasUnknownFocusFireToken);
}

void HashPortablePlayerState(u64 &hash, const PortablePlayerState &state)
{
    HashValue(hash, state.isPresent);
    HashPortableAnmVmState(hash, state.playerSprite);
    for (const auto &sprite : state.orbsSprite)
    {
        HashPortableAnmVmState(hash, sprite);
    }
    HashValue(hash, state.positionCenter);
    HashValue(hash, state.unk_44c);
    HashValue(hash, state.hitboxTopLeft);
    HashValue(hash, state.hitboxBottomRight);
    HashValue(hash, state.grabItemTopLeft);
    HashValue(hash, state.grabItemBottomRight);
    HashValue(hash, state.hitboxSize);
    HashValue(hash, state.grabItemSize);
    for (const auto &value : state.orbsPosition)
    {
        HashValue(hash, value);
    }
    for (const auto &value : state.bombRegionPositions)
    {
        HashValue(hash, value);
    }
    for (const auto &value : state.bombRegionSizes)
    {
        HashValue(hash, value);
    }
    HashArray(hash, state.bombRegionDamages);
    HashArray(hash, state.unk_838);
    for (const auto &rect : state.bombProjectiles)
    {
        HashPlayerRect(hash, rect);
    }
    for (const auto &timer : state.laserTimer)
    {
        HashValue(hash, timer);
    }
    HashValue(hash, state.horizontalMovementSpeedMultiplierDuringBomb);
    HashValue(hash, state.verticalMovementSpeedMultiplierDuringBomb);
    HashValue(hash, state.respawnTimer);
    HashValue(hash, state.bulletGracePeriod);
    HashValue(hash, state.playerState);
    HashValue(hash, state.playerType);
    HashValue(hash, state.unk_9e1);
    HashValue(hash, state.orbState);
    HashValue(hash, state.isFocus);
    HashValue(hash, state.unk_9e4);
    HashValue(hash, state.focusMovementTimer);
    HashPortableCharacterDataState(hash, state.characterData);
    HashValue(hash, state.playerDirection);
    HashValue(hash, state.previousHorizontalSpeed);
    HashValue(hash, state.previousVerticalSpeed);
    HashValue(hash, state.previousFrameInput);
    HashValue(hash, state.positionOfLastEnemyHit);
    for (const auto &bullet : state.bullets)
    {
        HashPortablePlayerBulletState(hash, bullet);
    }
    HashValue(hash, state.fireBulletTimer);
    HashValue(hash, state.invulnerabilityTimer);
    HashValue(hash, (u16)state.fireBulletToken);
    HashValue(hash, (u16)state.fireBulletFocusToken);
    HashValue(hash, state.hasUnknownFireToken);
    HashValue(hash, state.hasUnknownFocusFireToken);
    HashPortablePlayerBombState(hash, state.bombInfo);
    HashPortableAnmVmState(hash, state.hitboxSprite);
    HashValue(hash, state.hitboxTime);
    HashValue(hash, state.lifegiveTime);
}

void HashPortableBulletTypeSpritesState(u64 &hash, const PortableBulletTypeSpritesState &state)
{
    HashPortableAnmVmState(hash, state.spriteBullet);
    HashPortableAnmVmState(hash, state.spriteSpawnEffectFast);
    HashPortableAnmVmState(hash, state.spriteSpawnEffectNormal);
    HashPortableAnmVmState(hash, state.spriteSpawnEffectSlow);
    HashPortableAnmVmState(hash, state.spriteSpawnEffectDonut);
    HashValue(hash, state.grazeSize);
    HashValue(hash, state.unk_55c);
    HashValue(hash, state.bulletHeight);
}

void HashPortableBulletState(u64 &hash, const PortableBulletState &state)
{
    HashPortableBulletTypeSpritesState(hash, state.sprites);
    HashValue(hash, state.pos);
    HashValue(hash, state.velocity);
    HashValue(hash, state.ex4Acceleration);
    HashValue(hash, state.speed);
    HashValue(hash, state.ex5Float0);
    HashValue(hash, state.dirChangeSpeed);
    HashValue(hash, state.angle);
    HashValue(hash, state.ex5Float1);
    HashValue(hash, state.dirChangeRotation);
    HashValue(hash, state.timer);
    HashValue(hash, state.ex5Int0);
    HashValue(hash, state.dirChangeInterval);
    HashValue(hash, state.dirChangeNumTimes);
    HashValue(hash, state.dirChangeMaxTimes);
    HashValue(hash, state.exFlags);
    HashValue(hash, state.spriteOffset);
    HashValue(hash, state.unk_5bc);
    HashValue(hash, state.state);
    HashValue(hash, state.unk_5c0);
    HashValue(hash, state.unk_5c2);
    HashValue(hash, state.isGrazed);
    HashValue(hash, state.provokedPlayer);
}

void HashPortableLaserState(u64 &hash, const PortableLaserState &state)
{
    HashPortableAnmVmState(hash, state.vm0);
    HashPortableAnmVmState(hash, state.vm1);
    HashValue(hash, state.pos);
    HashValue(hash, state.angle);
    HashValue(hash, state.startOffset);
    HashValue(hash, state.endOffset);
    HashValue(hash, state.startLength);
    HashValue(hash, state.width);
    HashValue(hash, state.speed);
    HashValue(hash, state.startTime);
    HashValue(hash, state.hitboxStartTime);
    HashValue(hash, state.duration);
    HashValue(hash, state.despawnDuration);
    HashValue(hash, state.hitboxEndDelay);
    HashValue(hash, state.inUse);
    HashValue(hash, state.timer);
    HashValue(hash, state.flags);
    HashValue(hash, state.color);
    HashValue(hash, state.state);
    HashValue(hash, state.provokedPlayer);
}

void HashPortableBulletManagerState(u64 &hash, const PortableBulletManagerState &state)
{
    HashString(hash, state.bulletAnmPath);
    for (const auto &templateState : state.bulletTypeTemplates)
    {
        HashPortableBulletTypeSpritesState(hash, templateState);
    }
    for (const auto &bullet : state.bullets)
    {
        HashPortableBulletState(hash, bullet);
    }
    for (const auto &laser : state.lasers)
    {
        HashPortableLaserState(hash, laser);
    }
    HashValue(hash, state.nextBulletIndex);
    HashValue(hash, state.bulletCount);
    HashValue(hash, state.time);
}

void HashPortableItemState(u64 &hash, const PortableItemState &state)
{
    HashPortableAnmVmState(hash, state.sprite);
    HashValue(hash, state.currentPosition);
    HashValue(hash, state.startPosition);
    HashValue(hash, state.targetPosition);
    HashValue(hash, state.timer);
    HashValue(hash, state.itemType);
    HashValue(hash, state.isInUse);
    HashValue(hash, state.unk_142);
    HashValue(hash, state.state);
}

void HashPortableItemManagerState(u64 &hash, const PortableItemManagerState &state)
{
    for (const auto &item : state.items)
    {
        HashPortableItemState(hash, item);
    }
    HashValue(hash, state.nextIndex);
    HashValue(hash, state.itemCount);
}

void HashPortableEffectState(u64 &hash, const PortableEffectState &state)
{
    HashPortableAnmVmState(hash, state.vm);
    HashValue(hash, state.pos1);
    HashValue(hash, state.unk_11c);
    HashValue(hash, state.unk_128);
    HashValue(hash, state.position);
    HashValue(hash, state.pos2);
    HashValue(hash, state.quaternion);
    HashValue(hash, state.unk_15c);
    HashValue(hash, state.angleRelated);
    HashValue(hash, state.timer);
    HashValue(hash, state.unk_170);
    HashValue(hash, (u16)state.updateCallbackToken);
    HashValue(hash, state.hasUnknownUpdateToken);
    HashValue(hash, state.inUseFlag);
    HashValue(hash, state.effectId);
    HashValue(hash, state.unk_17a);
    HashValue(hash, state.unk_17b);
}

void HashPortableEffectManagerState(u64 &hash, const PortableEffectManagerState &state)
{
    HashValue(hash, state.nextIndex);
    HashValue(hash, state.activeEffects);
    for (const auto &effect : state.effects)
    {
        HashPortableEffectState(hash, effect);
    }
}

void HashPortableEclScripts(u64 &hash, const PortableEclScriptState &state)
{
    HashValue(hash, state.hasEclFile);
    HashValue(hash, state.timelineOffset);
    const u32 count = (u32)state.subTableEntryOffsets.size();
    HashValue(hash, count);
    for (i32 offset : state.subTableEntryOffsets)
    {
        HashValue(hash, offset);
    }
}

void HashPortableEclCore(u64 &hash, const PortableEclManagerCoreState &state)
{
    HashValue(hash, state.hasEclFile);
    HashString(hash, state.resource.resourcePath);
    HashValue(hash, state.resource.resourceContentHash);
    HashValue(hash, state.resource.resourceSizeBytes);
    HashValue(hash, state.subCount);
    HashValue(hash, state.mainCount);
    HashArray(hash, state.timelineOffsets);
    HashValue(hash, state.activeTimelineSlot);
    HashValue(hash, state.activeTimelineOffset);
    const u32 count = (u32)state.subTableEntryOffsets.size();
    HashValue(hash, count);
    for (i32 offset : state.subTableEntryOffsets)
    {
        HashValue(hash, offset);
    }
}

void HashPortableStageCore(u64 &hash, const PortableStageCoreState &state)
{
    HashValue(hash, state.hasStageData);
    HashString(hash, state.resource.stdPath);
    HashValue(hash, state.resource.stdContentHash);
    HashValue(hash, state.resource.stdSizeBytes);
    HashString(hash, state.resource.anmPath);
    HashValue(hash, state.resource.anmContentHash);
    HashValue(hash, state.resource.anmSizeBytes);
    HashValue(hash, state.stage);
    HashValue(hash, state.objectsCount);
    HashValue(hash, state.quadCount);
    HashValue(hash, state.scriptTime);
    HashValue(hash, state.instructionIndex);
    HashValue(hash, state.timer);
    HashValue(hash, state.position);
    HashValue(hash, state.skyFog.nearPlane);
    HashValue(hash, state.skyFog.farPlane);
    HashValue(hash, state.skyFog.color);
    HashValue(hash, state.skyFogInterpInitial.nearPlane);
    HashValue(hash, state.skyFogInterpInitial.farPlane);
    HashValue(hash, state.skyFogInterpInitial.color);
    HashValue(hash, state.skyFogInterpFinal.nearPlane);
    HashValue(hash, state.skyFogInterpFinal.farPlane);
    HashValue(hash, state.skyFogInterpFinal.color);
    HashValue(hash, state.skyFogInterpDuration);
    HashValue(hash, state.skyFogInterpTimer);
    HashValue(hash, state.skyFogNeedsSetup);
    HashValue(hash, (i32)state.spellcardState);
    HashValue(hash, state.ticksSinceSpellcardStarted);
    HashValue(hash, state.unpauseFlag);
    HashValue(hash, state.facingDirInterpInitial);
    HashValue(hash, state.facingDirInterpFinal);
    HashValue(hash, state.facingDirInterpDuration);
    HashValue(hash, state.facingDirInterpTimer);
    HashValue(hash, state.positionInterpFinal);
    HashValue(hash, state.positionInterpEndTime);
    HashValue(hash, state.positionInterpInitial);
    HashValue(hash, state.positionInterpStartTime);
    HashValue(hash, state.currentCameraFacingDir);
    const u32 objectFlagCount = (u32)state.objectFlags.size();
    HashValue(hash, objectFlagCount);
    for (u8 value : state.objectFlags)
    {
        HashValue(hash, value);
    }
    const u32 quadCount = (u32)state.quadVms.size();
    HashValue(hash, quadCount);
    for (const auto &vm : state.quadVms)
    {
        HashPortableAnmVmState(hash, vm);
    }
    HashPortableAnmVmState(hash, state.spellcardBackground);
    HashPortableAnmVmState(hash, state.extraBackground);
}

void HashPortableShellSyncState(u64 &hash, const PortableShellSyncState &state)
{
    HashValue(hash, state.bgmTrackIndex);
    HashValue(hash, (u32)state.bossAssetProfile);
    HashValue(hash, state.hideStageNameIntro);
    HashValue(hash, state.hideSongNameIntro);
}

void HashPortableGuiState(u64 &hash, const PortableGuiState &state)
{
    HashValue(hash, state.hasGuiImpl);
    HashValue(hash, state.flag0);
    HashValue(hash, state.flag1);
    HashValue(hash, state.flag2);
    HashValue(hash, state.flag3);
    HashValue(hash, state.flag4);
    HashValue(hash, state.bossHealthBarState);
    HashValue(hash, state.bossPresent);
    HashValue(hash, state.bossUIOpacity);
    HashValue(hash, state.eclSetLives);
    HashValue(hash, state.spellcardSecondsRemaining);
    HashValue(hash, state.lastSpellcardSecondsRemaining);
    HashValue(hash, state.bossHealthBar1);
    HashValue(hash, state.bossHealthBar2);
    HashValue(hash, state.bombSpellcardBarLength);
    HashValue(hash, state.blueSpellcardBarLength);
    HashPortableAnmVmState(hash, state.playerSpellcardPortrait);
    HashPortableAnmVmState(hash, state.enemySpellcardPortrait);
    HashPortableAnmVmState(hash, state.bombSpellcardName);
    HashPortableAnmVmState(hash, state.enemySpellcardName);
    HashPortableAnmVmState(hash, state.bombSpellcardBackground);
    HashPortableAnmVmState(hash, state.enemySpellcardBackground);
    HashPortableAnmVmState(hash, state.loadingScreenSprite);
    HashString(hash, state.bombSpellcardText);
    HashString(hash, state.enemySpellcardText);
}

bool NearlyEqualVec3(const D3DXVECTOR3 &lhs, const D3DXVECTOR3 &rhs, float epsilon = 0.001f)
{
    return std::fabs(lhs.x - rhs.x) <= epsilon && std::fabs(lhs.y - rhs.y) <= epsilon &&
           std::fabs(lhs.z - rhs.z) <= epsilon;
}

D3DXVECTOR3 ComputePortableStageFacingDir(const PortableStageCoreState &state)
{
    if (state.facingDirInterpDuration <= 0)
    {
        return state.facingDirInterpFinal;
    }

    float ratio =
        ((float)state.facingDirInterpTimer.current + state.facingDirInterpTimer.subFrame) / (float)state.facingDirInterpDuration;
    if (ratio < 0.0f)
    {
        ratio = 0.0f;
    }
    if (ratio > 1.0f)
    {
        ratio = 1.0f;
    }

    D3DXVECTOR3 result;
    result.x = (state.facingDirInterpFinal.x - state.facingDirInterpInitial.x) * ratio + state.facingDirInterpInitial.x;
    result.y = (state.facingDirInterpFinal.y - state.facingDirInterpInitial.y) * ratio + state.facingDirInterpInitial.y;
    result.z = (state.facingDirInterpFinal.z - state.facingDirInterpInitial.z) * ratio + state.facingDirInterpInitial.z;
    return result;
}

PortableGameplayResourceCatalog CaptureLiveCatalogForValidation()
{
    auto state = std::make_unique<PortableGameplayState>();
    CapturePortableGameplayState(*state);
    return state->catalog;
}

void AppendCoverageEntries(PortableGameplayCoverageReport &report, DgsCoverageKind kind,
                           const std::vector<std::string> &values)
{
    for (const auto &value : values)
    {
        report.entries.push_back({kind, value, value});
    }
}

void AppendBlockingReason(std::vector<std::string> &reasons, const std::string &reason)
{
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end())
    {
        reasons.push_back(reason);
    }
}

bool ValidatePortableEnemyContext(const PortableEnemyEclContextState &context, const char *label,
                                  std::vector<std::string> &reasons)
{
    bool ok = true;
    if (context.scriptOffset < -1)
    {
        AppendBlockingReason(reasons, std::string(label) + ": script offset is out of range.");
        ok = false;
    }
    if (context.hasUnknownFuncToken || (context.hasFuncToken && context.funcToken == PortableEnemyFuncToken::Unknown))
    {
        AppendBlockingReason(reasons, std::string(label) + ": funcSetFunc still maps to an unknown token.");
        ok = false;
    }
    return ok;
}

bool ValidatePortableEnemyState(const PortableEnemyState &enemy, size_t enemyIndex, std::vector<std::string> &reasons)
{
    bool ok = true;
    const std::string prefix = "Portable enemy[" + std::to_string(enemyIndex) + "]";
    ok = ValidatePortableEnemyContext(enemy.currentContext, (prefix + " currentContext").c_str(), reasons) && ok;
    for (size_t i = 0; i < enemy.savedContextStack.size(); ++i)
    {
        ok = ValidatePortableEnemyContext(enemy.savedContextStack[i],
                                          (prefix + " savedContext[" + std::to_string(i) + "]").c_str(), reasons) &&
             ok;
    }
    for (i32 laserIndex : enemy.laserIndices)
    {
        if (laserIndex < -1)
        {
            AppendBlockingReason(reasons, prefix + ": laser index is out of range.");
            ok = false;
            break;
        }
    }
    for (i32 effectIndex : enemy.effectIndices)
    {
        if (effectIndex < -1)
        {
            AppendBlockingReason(reasons, prefix + ": effect index is out of range.");
            ok = false;
            break;
        }
    }
    return ok;
}

bool ValidatePortableEnemyActors(const PortableEnemyManagerState &actors, std::vector<std::string> &reasons)
{
    bool ok = true;
    if (actors.timelineOffset < -1)
    {
        AppendBlockingReason(reasons, "Portable enemy manager timeline offset is out of range.");
        ok = false;
    }
    if (actors.enemyCount < 0 || actors.enemyCount > (i32)actors.enemies.size())
    {
        AppendBlockingReason(reasons, "Portable enemy count does not match encoded enemy array.");
        ok = false;
    }
    ok = ValidatePortableEnemyState(actors.enemyTemplate, (size_t)-1, reasons) && ok;
    for (size_t i = 0; i < actors.enemies.size(); ++i)
    {
        ok = ValidatePortableEnemyState(actors.enemies[i], i, reasons) && ok;
    }
    for (i32 bossIndex : actors.bossIndices)
    {
        if (bossIndex < -1 || bossIndex >= (i32)actors.enemies.size())
        {
            AppendBlockingReason(reasons, "Portable enemy manager boss index is out of range.");
            ok = false;
            break;
        }
    }
    return ok;
}

bool ValidatePortableEclScripts(const PortableEclScriptState &scripts, std::vector<std::string> &reasons)
{
    bool ok = true;
    if (scripts.timelineOffset < -1)
    {
        AppendBlockingReason(reasons, "Portable ECL timeline offset is out of range.");
        ok = false;
    }
    for (i32 offset : scripts.subTableEntryOffsets)
    {
        if (offset < -1)
        {
            AppendBlockingReason(reasons, "Portable ECL subtable entry offset is out of range.");
            ok = false;
            break;
        }
    }
    return ok;
}

bool IsPortableEclOffsetInRange(i32 offset, u32 resourceSizeBytes)
{
    return offset == -1 || (offset >= 0 && (u32)offset < resourceSizeBytes);
}

bool ValidatePortableEclManagerCoreState(const PortableGameplayState &state, std::vector<std::string> &reasons)
{
    TracePortableValidationPhase("validate-ecl-core-start");

    bool ok = true;
    const auto &core = state.eclCore;
    const char *expectedPath = ResolvePortableEclPathForStage(g_GameManager.currentStage);

    if (!core.hasEclFile)
    {
        AppendBlockingReason(reasons, "Portable ECL core is missing a loaded ECL resource.");
        TracePortableValidationPhase("validate-ecl-core-end");
        return false;
    }

    if (expectedPath == nullptr || core.resource.resourcePath != expectedPath)
    {
        AppendBlockingReason(reasons, "Portable ECL resource path does not match the current stage ECL.");
        ok = false;
    }

    std::vector<u8> rawBytes;
    if (!ReadPortableResourceBytes(core.resource.resourcePath.c_str(), rawBytes))
    {
        AppendBlockingReason(reasons, "Portable ECL resource path could not be loaded for validation.");
        TracePortableValidationPhase("validate-ecl-core-end");
        return false;
    }

    const u64 fileHash = HashByteSpan(rawBytes.data(), rawBytes.size());
    if (core.resource.resourceSizeBytes != rawBytes.size())
    {
        AppendBlockingReason(reasons, "Portable ECL resource size does not match the loaded stage ECL.");
        ok = false;
    }
    if (core.resource.resourceContentHash != fileHash)
    {
        AppendBlockingReason(reasons, "Portable ECL resource hash does not match the loaded stage ECL.");
        ok = false;
    }

    if (core.subCount < 0 || core.mainCount < 0 || (size_t)core.subCount != core.subTableEntryOffsets.size())
    {
        AppendBlockingReason(reasons, "Portable ECL subCount/mainCount do not match the encoded ECL core layout.");
        ok = false;
    }

    if (core.activeTimelineSlot < 0 || core.activeTimelineSlot >= (i32)core.timelineOffsets.size())
    {
        AppendBlockingReason(reasons, "Portable ECL active timeline slot is out of range.");
        ok = false;
    }
    else if (core.timelineOffsets[(size_t)core.activeTimelineSlot] != core.activeTimelineOffset)
    {
        AppendBlockingReason(reasons, "Portable ECL active timeline slot does not match the captured timeline offset.");
        ok = false;
    }

    for (i32 offset : core.timelineOffsets)
    {
        if (!IsPortableEclOffsetInRange(offset, core.resource.resourceSizeBytes))
        {
            AppendBlockingReason(reasons, "Portable ECL timeline offsets are out of range.");
            ok = false;
            break;
        }
    }
    for (i32 offset : core.subTableEntryOffsets)
    {
        if (!IsPortableEclOffsetInRange(offset, core.resource.resourceSizeBytes))
        {
            AppendBlockingReason(reasons, "Portable ECL subtable entries are out of range.");
            ok = false;
            break;
        }
    }

    if (!IsPortableEclOffsetInRange(state.eclScripts.timelineOffset, core.resource.resourceSizeBytes))
    {
        AppendBlockingReason(reasons, "Portable ECL runtime timeline offset is out of range.");
        ok = false;
    }
    if (state.eclScripts.timelineOffset != -1 && state.eclScripts.timelineOffset != core.activeTimelineOffset)
    {
        AppendBlockingReason(reasons, "Portable ECL runtime timeline offset does not match the active ECL core timeline.");
        ok = false;
    }
    if (!IsPortableEclOffsetInRange(state.enemyActors.timelineOffset, core.resource.resourceSizeBytes))
    {
        AppendBlockingReason(reasons, "Portable enemy manager timeline offset is out of the portable ECL resource range.");
        ok = false;
    }
    for (i32 offset : state.eclScripts.subTableEntryOffsets)
    {
        if (!IsPortableEclOffsetInRange(offset, core.resource.resourceSizeBytes))
        {
            AppendBlockingReason(reasons, "Portable runtime ECL subtable entries are out of range.");
            ok = false;
            break;
        }
    }
    if (!state.eclScripts.subTableEntryOffsets.empty() && state.eclScripts.subTableEntryOffsets != core.subTableEntryOffsets)
    {
        AppendBlockingReason(reasons, "Portable runtime ECL subtable entries do not match the encoded ECL core.");
        ok = false;
    }

    auto validateContextOffset = [&](i32 scriptOffset, const std::string &label) {
        if (!IsPortableEclOffsetInRange(scriptOffset, core.resource.resourceSizeBytes))
        {
            AppendBlockingReason(reasons, label + " is out of the portable ECL resource range.");
            ok = false;
        }
    };

    validateContextOffset(state.enemyActors.enemyTemplate.currentContext.scriptOffset,
                          "Portable enemy template current script offset");
    for (size_t i = 0; i < state.enemyActors.enemyTemplate.savedContextStack.size(); ++i)
    {
        validateContextOffset(state.enemyActors.enemyTemplate.savedContextStack[i].scriptOffset,
                              "Portable enemy template savedContext[" + std::to_string(i) + "] script offset");
    }
    for (size_t enemyIndex = 0; enemyIndex < state.enemyActors.enemies.size(); ++enemyIndex)
    {
        const auto &enemy = state.enemyActors.enemies[enemyIndex];
        validateContextOffset(enemy.currentContext.scriptOffset,
                              "Portable enemy[" + std::to_string(enemyIndex) + "] current script offset");
        for (size_t i = 0; i < enemy.savedContextStack.size(); ++i)
        {
            validateContextOffset(enemy.savedContextStack[i].scriptOffset,
                                  "Portable enemy[" + std::to_string(enemyIndex) + "] savedContext[" +
                                      std::to_string(i) + "] script offset");
        }
    }

    TracePortableValidationPhase("validate-ecl-core-end");
    return ok;
}

bool IsPortableStageStdOffsetInRange(i32 offset, u32 resourceSizeBytes)
{
    return offset >= 0 && (u32)offset < resourceSizeBytes;
}

bool ValidatePortableStageCoreState(const PortableGameplayState &state, std::vector<std::string> &reasons)
{
    TracePortableValidationPhase("validate-stage-core-start");

    bool ok = true;
    const auto &core = state.stageCore;
    const char *expectedStdPath = ResolvePortableStageStdPathForStage((i32)state.catalog.currentStage);
    const char *expectedAnmPath = ResolvePortableStageAnmPathForStage((i32)state.catalog.currentStage);

    if (!core.hasStageData)
    {
        AppendBlockingReason(reasons, "Portable stage core is missing loaded stage data.");
        TracePortableValidationPhase("validate-stage-core-end");
        return false;
    }

    if (core.stage != state.catalog.currentStage)
    {
        AppendBlockingReason(reasons, "Portable stage core stage does not match the gameplay catalog stage.");
        ok = false;
    }
    if (expectedStdPath == nullptr || core.resource.stdPath != expectedStdPath)
    {
        AppendBlockingReason(reasons, "Portable stage std resource path does not match the current stage STD.");
        ok = false;
    }
    if (expectedAnmPath == nullptr || core.resource.anmPath != expectedAnmPath)
    {
        AppendBlockingReason(reasons, "Portable stage anm resource path does not match the current stage ANM.");
        ok = false;
    }

    std::vector<u8> stdBytes;
    if (!ReadPortableResourceBytes(core.resource.stdPath.c_str(), stdBytes))
    {
        AppendBlockingReason(reasons, "Portable stage std resource path could not be loaded for validation.");
        TracePortableValidationPhase("validate-stage-core-end");
        return false;
    }
    std::vector<u8> anmBytes;
    if (!ReadPortableResourceBytes(core.resource.anmPath.c_str(), anmBytes))
    {
        AppendBlockingReason(reasons, "Portable stage anm resource path could not be loaded for validation.");
        TracePortableValidationPhase("validate-stage-core-end");
        return false;
    }

    if (core.resource.stdSizeBytes != stdBytes.size())
    {
        AppendBlockingReason(reasons, "Portable stage std resource size does not match the loaded stage STD.");
        ok = false;
    }
    if (core.resource.stdContentHash != HashByteSpan(stdBytes.data(), stdBytes.size()))
    {
        AppendBlockingReason(reasons, "Portable stage std resource hash does not match the loaded stage STD.");
        ok = false;
    }
    if (core.resource.anmSizeBytes != anmBytes.size())
    {
        AppendBlockingReason(reasons, "Portable stage anm resource size does not match the loaded stage ANM.");
        ok = false;
    }
    if (core.resource.anmContentHash != HashByteSpan(anmBytes.data(), anmBytes.size()))
    {
        AppendBlockingReason(reasons, "Portable stage anm resource hash does not match the loaded stage ANM.");
        ok = false;
    }

    if (stdBytes.size() < sizeof(RawStageHeader))
    {
        AppendBlockingReason(reasons, "Portable stage std resource is too small to contain a valid stage header.");
        TracePortableValidationPhase("validate-stage-core-end");
        return false;
    }

    const auto *rawHeader = reinterpret_cast<const RawStageHeader *>(stdBytes.data());
    if (core.objectsCount < 0 || core.quadCount < 0)
    {
        AppendBlockingReason(reasons, "Portable stage object or quad count is negative.");
        ok = false;
    }
    if (core.objectsCount != rawHeader->nbObjects)
    {
        AppendBlockingReason(reasons, "Portable stage object count does not match the loaded stage STD.");
        ok = false;
    }
    if (core.quadCount != rawHeader->nbFaces)
    {
        AppendBlockingReason(reasons, "Portable stage quad count does not match the loaded stage STD.");
        ok = false;
    }

    if ((int)core.objectFlags.size() != core.objectsCount)
    {
        AppendBlockingReason(reasons, "Portable stage object flag count does not match the captured object count.");
        ok = false;
    }
    if ((int)core.quadVms.size() != core.quadCount)
    {
        AppendBlockingReason(reasons, "Portable stage quad VM count does not match the captured quad count.");
        ok = false;
    }
    if ((int)state.runtime.stageObjectInstances.size() != core.objectsCount)
    {
        AppendBlockingReason(reasons, "Portable stage runtime object instance count does not match the captured object count.");
        ok = false;
    }
    if ((int)state.stageRefs.objectOffsets.size() != core.objectsCount)
    {
        AppendBlockingReason(reasons, "Portable stage object refs do not match the captured object count.");
        ok = false;
    }
    if ((int)state.stageRefs.quadVmRefs.size() != core.quadCount)
    {
        AppendBlockingReason(reasons, "Portable stage quad VM refs do not match the captured quad count.");
        ok = false;
    }
    if (!EqualAnmVmRefs(core.spellcardBackground.refs, state.stageRefs.spellcardBackground))
    {
        AppendBlockingReason(reasons, "Portable stage spellcard background refs do not match the captured stage refs.");
        ok = false;
    }
    if (!EqualAnmVmRefs(core.extraBackground.refs, state.stageRefs.extraBackground))
    {
        AppendBlockingReason(reasons, "Portable stage extra background refs do not match the captured stage refs.");
        ok = false;
    }
    for (size_t i = 0; i < core.quadVms.size() && i < state.stageRefs.quadVmRefs.size(); ++i)
    {
        if (!EqualAnmVmRefs(core.quadVms[i].refs, state.stageRefs.quadVmRefs[i]))
        {
            AppendBlockingReason(reasons, "Portable stage quad VM refs do not match the captured stage refs.");
            ok = false;
            break;
        }
    }

    if (!IsPortableStageStdOffsetInRange(state.stageRefs.beginningOfScript.value, core.resource.stdSizeBytes) ||
        state.stageRefs.beginningOfScript.value != rawHeader->scriptOffset)
    {
        AppendBlockingReason(reasons, "Portable stage beginning-of-script offset does not match the loaded stage STD.");
        ok = false;
    }

    const u8 *rawBase = stdBytes.data();
    const auto *rawObjectTable = reinterpret_cast<const RawStageObject *const *>(rawBase + sizeof(RawStageHeader));
    for (size_t i = 0; i < state.stageRefs.objectOffsets.size(); ++i)
    {
        const i32 actualOffset = (i32)(uintptr_t)rawObjectTable[i];
        const i32 capturedOffset = state.stageRefs.objectOffsets[i].value;
        if (!IsPortableStageStdOffsetInRange(capturedOffset, core.resource.stdSizeBytes) || capturedOffset != actualOffset)
        {
            AppendBlockingReason(reasons, "Portable stage object refs do not match the loaded stage STD.");
            ok = false;
            break;
        }
    }

    const D3DXVECTOR3 expectedFacingDir = ComputePortableStageFacingDir(core);
    if (!NearlyEqualVec3(core.currentCameraFacingDir, expectedFacingDir))
    {
        AppendBlockingReason(reasons, "Portable stage current camera facing direction is inconsistent with the captured stage interpolation state.");
        ok = false;
    }

    TracePortableValidationPhase("validate-stage-core-end");
    return ok;
}

bool ValidatePortablePlayerState(const PortablePlayerState &player, size_t playerIndex, std::vector<std::string> &reasons)
{
    if (!player.isPresent)
    {
        return true;
    }

    bool ok = true;
    const std::string prefix = "Portable player[" + std::to_string(playerIndex) + "]";
    if (player.characterData.hasUnknownFireToken || player.characterData.hasUnknownFocusFireToken ||
        player.hasUnknownFireToken || player.hasUnknownFocusFireToken ||
        player.fireBulletToken == PortablePlayerFireFuncToken::Unknown ||
        player.fireBulletFocusToken == PortablePlayerFireFuncToken::Unknown)
    {
        AppendBlockingReason(reasons, prefix + ": fire-bullet callback token is still unknown.");
        ok = false;
    }
    if (player.bombInfo.hasUnknownCalcToken || player.bombInfo.hasUnknownDrawToken ||
        player.bombInfo.calcToken == PortablePlayerBombFuncToken::Unknown ||
        player.bombInfo.drawToken == PortablePlayerBombFuncToken::Unknown)
    {
        AppendBlockingReason(reasons, prefix + ": bomb callback token is still unknown.");
        ok = false;
    }
    return ok;
}

bool ValidatePortableBulletManagerState(const PortableBulletManagerState &state, std::vector<std::string> &reasons)
{
    bool ok = true;
    if (state.nextBulletIndex < 0 || state.nextBulletIndex > (i32)state.bullets.size())
    {
        AppendBlockingReason(reasons, "Portable bullet manager nextBulletIndex is out of range.");
        ok = false;
    }
    if (state.bulletCount < 0 || state.bulletCount > (i32)state.bullets.size())
    {
        AppendBlockingReason(reasons, "Portable bullet manager bulletCount is out of range.");
        ok = false;
    }
    return ok;
}

bool ValidatePortableItemManagerState(const PortableItemManagerState &state, std::vector<std::string> &reasons)
{
    bool ok = true;
    if (state.nextIndex < 0 || state.nextIndex > (i32)state.items.size())
    {
        AppendBlockingReason(reasons, "Portable item manager nextIndex is out of range.");
        ok = false;
    }
    if (state.itemCount > state.items.size())
    {
        AppendBlockingReason(reasons, "Portable item manager itemCount is out of range.");
        ok = false;
    }
    return ok;
}

bool ValidatePortableEffectManagerState(const PortableEffectManagerState &state, std::vector<std::string> &reasons)
{
    bool ok = true;
    if (state.nextIndex < 0 || state.nextIndex > (i32)state.effects.size())
    {
        AppendBlockingReason(reasons, "Portable effect manager nextIndex is out of range.");
        ok = false;
    }
    if (state.activeEffects < 0 || state.activeEffects > (i32)state.effects.size())
    {
        AppendBlockingReason(reasons, "Portable effect manager activeEffects is out of range.");
        ok = false;
    }
    for (size_t i = 0; i < state.effects.size(); ++i)
    {
        const auto &effect = state.effects[i];
        if (effect.hasUnknownUpdateToken || effect.updateCallbackToken == PortableEffectUpdateToken::Unknown)
        {
            AppendBlockingReason(reasons,
                                 "Portable effect[" + std::to_string(i) + "] update callback token is still unknown.");
            ok = false;
            break;
        }
    }
    return ok;
}

bool ValidatePortableShellSyncState(const PortableGameplayState &state, std::vector<std::string> &reasons)
{
    bool ok = true;
    if (state.shellSync.bgmTrackIndex < 0 || state.shellSync.bgmTrackIndex > 1)
    {
        AppendBlockingReason(reasons, "Portable shell sync BGM track index is out of range.");
        ok = false;
    }

    switch (state.shellSync.bossAssetProfile)
    {
    case PortableBossAssetProfile::None:
    case PortableBossAssetProfile::Stage6BossEffects:
    case PortableBossAssetProfile::Stage7EndEffects:
        break;
    default:
        AppendBlockingReason(reasons, "Portable shell sync boss asset profile is unknown.");
        ok = false;
        break;
    }
    return ok;
}

bool ValidatePortableGuiState(const PortableGameplayState &state, std::vector<std::string> &reasons)
{
    bool ok = true;
    if (!state.gui.hasGuiImpl)
    {
        AppendBlockingReason(reasons, "Portable GUI spellcard HUD state is missing; resave snapshot with schema V5.");
        ok = false;
    }
    if (state.gui.bossHealthBarState > 3)
    {
        AppendBlockingReason(reasons, "Portable GUI boss health bar state is out of range.");
        ok = false;
    }
    return ok;
}
} // namespace

uint64_t FingerprintPortableGameplayState(const PortableGameplayState &state)
{
    u64 hash = kFnvOffset;
    HashValue(hash, state.header.magic);
    HashValue(hash, state.header.version);
    HashValue(hash, state.captureFlags);

    HashValue(hash, state.catalog.catalogHash);
    HashValue(hash, state.catalog.currentStage);
    HashValue(hash, state.catalog.difficulty);
    HashValue(hash, state.catalog.character1);
    HashValue(hash, state.catalog.shotType1);
    HashValue(hash, state.catalog.character2);
    HashValue(hash, state.catalog.shotType2);
    HashValue(hash, state.catalog.hasSecondPlayer);
    HashValue(hash, state.catalog.isPracticeMode);
    HashValue(hash, state.catalog.isReplay);

    HashValue(hash, state.core.guiScore);
    HashValue(hash, state.core.score);
    HashValue(hash, state.core.nextScoreIncrement);
    HashValue(hash, state.core.highScore);
    HashValue(hash, state.core.difficulty);
    HashValue(hash, state.core.grazeInStage);
    HashValue(hash, state.core.grazeInTotal);
    HashValue(hash, state.core.deaths);
    HashValue(hash, state.core.bombsUsed);
    HashValue(hash, state.core.spellcardsCaptured);
    HashValue(hash, state.core.pointItemsCollectedInStage);
    HashValue(hash, state.core.pointItemsCollected);
    HashValue(hash, state.core.powerItemCountForScore);
    HashValue(hash, state.core.extraLives);
    HashValue(hash, state.core.currentStage);
    HashValue(hash, state.core.rank);
    HashValue(hash, state.core.maxRank);
    HashValue(hash, state.core.minRank);
    HashValue(hash, state.core.subRank);
    HashValue(hash, state.core.randomSeed);
    HashValue(hash, state.core.gameFrames);
    HashValue(hash, state.core.currentPower1);
    HashValue(hash, state.core.currentPower2);
    HashValue(hash, state.core.livesRemaining1);
    HashValue(hash, state.core.bombsRemaining1);
    HashValue(hash, state.core.livesRemaining2);
    HashValue(hash, state.core.bombsRemaining2);
    HashValue(hash, state.core.numRetries);
    HashValue(hash, state.core.isTimeStopped);
    HashValue(hash, state.core.isGameCompleted);
    HashValue(hash, state.core.isPracticeMode);
    HashValue(hash, state.core.demoMode);
    HashValue(hash, state.core.character1);
    HashValue(hash, state.core.shotType1);
    HashValue(hash, state.core.character2);
    HashValue(hash, state.core.shotType2);

    HashValue(hash, state.runtime.frame);
    HashValue(hash, state.runtime.stage);
    HashValue(hash, state.runtime.delay);
    HashValue(hash, state.runtime.currentDelayCooldown);
    HashValue(hash, state.rng.seed);
    HashValue(hash, state.rng.generationCount);
    HashValue(hash, HashByteSpan(state.catk.data(), sizeof(Catk) * state.catk.size()));
    HashValue(hash, state.shadowFingerprints.dgs.combined);

    for (const auto &object : state.runtime.stageObjectInstances)
    {
        HashValue(hash, object.id);
        HashValue(hash, object.unk2);
        HashValue(hash, object.position);
    }

    HashDgsStageRefs(hash, state.stageRefs);
    HashDgsEnemyRefs(hash, state.enemyRefs);
    HashPortableEclScripts(hash, state.eclScripts);
    HashPortableEclCore(hash, state.eclCore);
    HashPortableStageCore(hash, state.stageCore);
    HashPortableShellSyncState(hash, state.shellSync);
    HashPortableGuiState(hash, state.gui);
    HashPortableEnemyManagerState(hash, state.enemyActors);
    for (const auto &player : state.players)
    {
        HashPortablePlayerState(hash, player);
    }
    HashPortableBulletManagerState(hash, state.bulletActors);
    HashPortableItemManagerState(hash, state.itemActors);
    HashPortableEffectManagerState(hash, state.effectActors);

    for (const auto &entry : state.mustNormalizeBeforeRestore)
    {
        HashString(hash, entry);
    }
    for (const auto &entry : state.excludedByDesign)
    {
        HashString(hash, entry);
    }
    return hash;
}

namespace
{
void HashPortableAuthorityGameState(u64 &hash, const PortableGameplayState &state)
{
    HashValue(hash, state.core.guiScore);
    HashValue(hash, state.core.score);
    HashValue(hash, state.core.nextScoreIncrement);
    HashValue(hash, state.core.highScore);
    HashValue(hash, state.core.difficulty);
    HashValue(hash, state.core.grazeInStage);
    HashValue(hash, state.core.grazeInTotal);
    HashValue(hash, state.core.deaths);
    HashValue(hash, state.core.bombsUsed);
    HashValue(hash, state.core.spellcardsCaptured);
    HashValue(hash, state.core.pointItemsCollectedInStage);
    HashValue(hash, state.core.pointItemsCollected);
    HashValue(hash, state.core.powerItemCountForScore);
    HashValue(hash, state.core.extraLives);
    HashValue(hash, state.core.currentStage);
    HashValue(hash, state.core.rank);
    HashValue(hash, state.core.maxRank);
    HashValue(hash, state.core.minRank);
    HashValue(hash, state.core.subRank);
    HashValue(hash, state.core.gameFrames);
    HashValue(hash, state.core.currentPower1);
    HashValue(hash, state.core.currentPower2);
    HashValue(hash, state.core.livesRemaining1);
    HashValue(hash, state.core.bombsRemaining1);
    HashValue(hash, state.core.livesRemaining2);
    HashValue(hash, state.core.bombsRemaining2);
    HashValue(hash, state.core.numRetries);
    HashValue(hash, state.core.isTimeStopped);
    HashValue(hash, state.core.isGameCompleted);
    HashValue(hash, state.core.isPracticeMode);
    HashValue(hash, state.core.demoMode);
    HashValue(hash, state.core.character1);
    HashValue(hash, state.core.shotType1);
    HashValue(hash, state.core.character2);
    HashValue(hash, state.core.shotType2);
}

void HashPortableAuthorityEnemyEclRuntime(u64 &hash, const PortableGameplayRuntimeState &runtime)
{
    HashValue(hash, runtime.enemyEclRuntimeState.playerShot);
    HashValue(hash, runtime.enemyEclRuntimeState.playerDistance);
    HashValue(hash, runtime.enemyEclRuntimeState.playerAngle);
    for (f32 angle : runtime.enemyEclRuntimeState.starAngleTable)
    {
        HashValue(hash, angle);
    }
    HashValue(hash, runtime.enemyEclRuntimeState.enemyPosVector);
    HashValue(hash, runtime.enemyEclRuntimeState.playerPosVector);
    for (i32 value : runtime.enemyEclRuntimeState.eclLiteralInts)
    {
        HashValue(hash, value);
    }
    for (f32 value : runtime.enemyEclRuntimeState.eclLiteralFloats)
    {
        HashValue(hash, value);
    }
    HashValue(hash, runtime.enemyEclRuntimeState.eclLiteralIntCursor);
    HashValue(hash, runtime.enemyEclRuntimeState.eclLiteralFloatCursor);
}

void HashPortableAuthorityScreenRuntime(u64 &hash, const PortableGameplayRuntimeState &runtime)
{
    HashValue(hash, (u32)runtime.screenEffectRuntimeState.activeEffects.size());
    for (const auto &effect : runtime.screenEffectRuntimeState.activeEffects)
    {
        HashValue(hash, effect.usedEffect);
        HashValue(hash, effect.fadeAlpha);
        HashValue(hash, effect.effectLength);
        HashValue(hash, effect.genericParam);
        HashValue(hash, effect.shakinessParam);
        HashValue(hash, effect.unusedParam);
        HashValue(hash, effect.timer.previous);
        HashValue(hash, effect.timer.subFrame);
        HashValue(hash, effect.timer.current);
    }
}

void HashPortableAuthorityInputRuntime(u64 &hash, const PortableGameplayRuntimeState &runtime)
{
    HashValue(hash, runtime.frame);
    HashValue(hash, runtime.delay);
    HashValue(hash, runtime.currentDelayCooldown);
    HashValue(hash, runtime.controllerRuntimeState.focusButtonConflictState);
    HashValue(hash, runtime.inputRuntimeState.lastFrameInput);
    HashValue(hash, runtime.inputRuntimeState.curFrameInput);
    HashValue(hash, runtime.inputRuntimeState.eighthFrameHeldInput);
    HashValue(hash, runtime.inputRuntimeState.heldInputFrames);
}
} // namespace

bool CapturePortableAuthorityGameplayFingerprint(const PortableGameplayState &state, AuthorityGameplayFingerprint &out)
{
    out = {};

    u64 hash = kFnvOffset;
    HashPortableAuthorityGameState(hash, state);
    out.gameHash = hash;

    hash = kFnvOffset;
    HashPortablePlayerState(hash, state.players[0]);
    out.player1Hash = hash;

    hash = kFnvOffset;
    HashPortablePlayerState(hash, state.players[1]);
    out.player2Hash = hash;

    hash = kFnvOffset;
    HashPortableBulletManagerState(hash, state.bulletActors);
    out.bulletHash = hash;

    hash = kFnvOffset;
    HashPortableEnemyManagerState(hash, state.enemyActors);
    out.enemyHash = hash;

    hash = kFnvOffset;
    HashPortableItemManagerState(hash, state.itemActors);
    out.itemHash = hash;

    hash = kFnvOffset;
    HashPortableEffectManagerState(hash, state.effectActors);
    out.effectHash = hash;

    hash = kFnvOffset;
    HashPortableStageCore(hash, state.stageCore);
    for (const auto &object : state.runtime.stageObjectInstances)
    {
        HashValue(hash, object.id);
        HashValue(hash, object.unk2);
        HashValue(hash, object.position);
    }
    out.stageHash = hash;

    hash = kFnvOffset;
    HashPortableEclScripts(hash, state.eclScripts);
    HashPortableEclCore(hash, state.eclCore);
    out.eclHash = hash;

    hash = kFnvOffset;
    HashPortableAuthorityEnemyEclRuntime(hash, state.runtime);
    out.enemyEclHash = hash;

    hash = kFnvOffset;
    HashPortableAuthorityScreenRuntime(hash, state.runtime);
    out.screenHash = hash;

    hash = kFnvOffset;
    HashPortableAuthorityInputRuntime(hash, state.runtime);
    out.inputHash = hash;

    out.catkHash = HashByteSpan(state.catk.data(), sizeof(Catk) * state.catk.size());

    hash = kFnvOffset;
    HashValue(hash, state.rng.seed);
    HashValue(hash, state.rng.generationCount);
    out.rngHash = hash;

    hash = kFnvOffset;
    HashValue(hash, out.gameHash);
    HashValue(hash, out.player1Hash);
    HashValue(hash, out.player2Hash);
    HashValue(hash, out.bulletHash);
    HashValue(hash, out.enemyHash);
    HashValue(hash, out.itemHash);
    HashValue(hash, out.effectHash);
    HashValue(hash, out.stageHash);
    HashValue(hash, out.eclHash);
    HashValue(hash, out.enemyEclHash);
    HashValue(hash, out.screenHash);
    HashValue(hash, out.inputHash);
    HashValue(hash, out.catkHash);
    HashValue(hash, out.rngHash);
    out.allHash = hash;
    return true;
}

const char *FirstDifferentAuthorityGameplaySubsystem(const AuthorityGameplayFingerprint &lhs,
                                                     const AuthorityGameplayFingerprint &rhs)
{
    if (lhs.gameHash != rhs.gameHash)
    {
        return "gameManager";
    }
    if (lhs.player1Hash != rhs.player1Hash)
    {
        return "player1";
    }
    if (lhs.player2Hash != rhs.player2Hash)
    {
        return "player2";
    }
    if (lhs.bulletHash != rhs.bulletHash)
    {
        return "bulletManager";
    }
    if (lhs.enemyHash != rhs.enemyHash)
    {
        return "enemyManager";
    }
    if (lhs.itemHash != rhs.itemHash)
    {
        return "itemManager";
    }
    if (lhs.effectHash != rhs.effectHash)
    {
        return "effectManager";
    }
    if (lhs.stageHash != rhs.stageHash)
    {
        return "stage";
    }
    if (lhs.eclHash != rhs.eclHash)
    {
        return "ecl";
    }
    if (lhs.enemyEclHash != rhs.enemyEclHash)
    {
        return "enemyEcl";
    }
    if (lhs.screenHash != rhs.screenHash)
    {
        return "screen";
    }
    if (lhs.inputHash != rhs.inputHash)
    {
        return "inputRuntime";
    }
    if (lhs.catkHash != rhs.catkHash)
    {
        return "catk";
    }
    if (lhs.rngHash != rhs.rngHash)
    {
        return "rng";
    }
    if (lhs.allHash != rhs.allHash)
    {
        return "all";
    }
    return "none";
}

PortableGameplayCoverageReport AuditPortableGameplayStateCoverage(const PortableGameplayState &state)
{
    PortableGameplayCoverageReport report;
    report.entries.push_back({DgsCoverageKind::Included, "Portable resource catalog",
                              "Stable gameplay resource identity and catalog hash."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable core",
                              "Explicit scalar gameplay core fields without raw struct dumps."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable runtime",
                              "Explicit deterministic runtime subset and stage object instances."});
    report.entries.push_back({DgsCoverageKind::Included, "Stage refs", "Stage script/object/VM refs stored as offsets and indices."});
    report.entries.push_back({DgsCoverageKind::Included, "Enemy refs",
                              "Enemy cross-object refs stored as normalized indices and offsets."});
    report.entries.push_back({DgsCoverageKind::Included, "ECL scripts",
                              "Timeline and explicit subtable entry offsets stored without raw pointers."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable ECL core",
                              "Explicit ECL resource identity, file layout, timeline slots, and subtable offsets."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable stage core",
                              "Explicit stage resource identity, object flags, stage interpolation state, and quad/background VM runtime."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable actors",
                              "EnemyManager actor bodies, contexts, tokens, and cross-object indices are explicit."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable players",
                              "Player actor bodies, player bullets, bomb state, and callback tokens are explicit."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable bullet manager",
                              "BulletManager bullets, lasers, template VMs, and manager runtime are explicit."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable item manager",
                              "ItemManager items, sprite VMs, timers, and manager runtime are explicit."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable effect manager",
                              "EffectManager effects, VMs, update-callback tokens, and manager runtime are explicit."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable shell sync",
                              "Explicit BGM track, boss asset profile, and intro sprite visibility hints."});
    report.entries.push_back({DgsCoverageKind::Included, "Portable GUI state",
                              "Explicit spellcard HUD state, boss UI fields, and GUI ANM VMs."});
    report.entries.push_back({DgsCoverageKind::Included, "Shadow fingerprints",
                              "Parallel DGS fingerprints retained for migration-time validation."});

    AppendCoverageEntries(report, DgsCoverageKind::Unresolved, state.mustNormalizeBeforeRestore);
    AppendCoverageEntries(report, DgsCoverageKind::Excluded, state.excludedByDesign);
    return report;
}

void SetPortableValidationTraceEnabled(bool enabled)
{
    g_PortableValidationTraceEnabled = enabled;
}

bool IsPortableValidationTraceEnabled()
{
    return g_PortableValidationTraceEnabled;
}

void TracePortableValidationPhase(const char *phase)
{
    if (!g_PortableValidationTraceEnabled)
    {
        return;
    }
    AppendPortableValidationTraceLine(phase);
}

void EvaluatePortableRestoreReadiness(const PortableGameplayState &state, PortableRestoreEvaluation &evaluation)
{
    TracePortableValidationPhase("readiness-start");
    evaluation = {};
    evaluation.schemaMatches = state.header.magic == kPortableGameplayMagic &&
                               (state.header.version == (u32)PortableGameplaySchemaVersion::V1 ||
                                state.header.version == (u32)PortableGameplaySchemaVersion::V2 ||
                                state.header.version == (u32)PortableGameplaySchemaVersion::V3 ||
                                state.header.version == (u32)PortableGameplaySchemaVersion::V4 ||
                                state.header.version == (u32)PortableGameplaySchemaVersion::V5 ||
                                state.header.version == (u32)PortableGameplaySchemaVersion::V6);
    if (!evaluation.schemaMatches)
    {
        evaluation.readiness = PortableRestoreReadiness::SchemaMismatch;
        evaluation.blockingReasons.emplace_back("Portable schema magic/version mismatch.");
        return;
    }

    const PortableGameplayResourceCatalog liveCatalog = CaptureLiveCatalogForValidation();
    evaluation.catalogMatches = liveCatalog.catalogHash == state.catalog.catalogHash &&
                                liveCatalog.currentStage == state.catalog.currentStage &&
                                liveCatalog.difficulty == state.catalog.difficulty &&
                                liveCatalog.character1 == state.catalog.character1 &&
                                liveCatalog.shotType1 == state.catalog.shotType1 &&
                                liveCatalog.character2 == state.catalog.character2 &&
                                liveCatalog.shotType2 == state.catalog.shotType2;
    if (!evaluation.catalogMatches)
    {
        evaluation.readiness = PortableRestoreReadiness::CatalogMismatch;
        evaluation.blockingReasons.emplace_back("Portable resource catalog does not match the current gameplay context.");
        return;
    }

    const u32 requiredFlags = PortableCaptureFlag_HasResourceCatalog | PortableCaptureFlag_HasExplicitCore |
                              PortableCaptureFlag_HasExplicitRuntime | PortableCaptureFlag_HasStageRefs |
                              PortableCaptureFlag_HasEnemyRefs | PortableCaptureFlag_HasEclRefs |
                              PortableCaptureFlag_HasExplicitEclCore | PortableCaptureFlag_HasExplicitStageCore |
                              PortableCaptureFlag_HasShadowFingerprints | PortableCaptureFlag_HasExplicitActors |
                              PortableCaptureFlag_HasExplicitPlayers | PortableCaptureFlag_HasExplicitBullets |
                              PortableCaptureFlag_HasExplicitItems | PortableCaptureFlag_HasExplicitEffects |
                              PortableCaptureFlag_HasShellSyncHints | PortableCaptureFlag_HasExplicitGui;
    evaluation.missingCaptureFlags = requiredFlags & ~state.captureFlags;
    if (evaluation.missingCaptureFlags != 0)
    {
        evaluation.readiness = PortableRestoreReadiness::ShadowOnly;
        evaluation.blockingReasons.emplace_back("Portable capture is missing required explicit sections.");
    }
    if (state.header.version < (u32)PortableGameplaySchemaVersion::V4 ||
        !HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasShellSyncHints))
    {
        evaluation.readiness = PortableRestoreReadiness::ShadowOnly;
        evaluation.blockingReasons.emplace_back("Portable shell sync hints are missing; resave snapshot with schema V4.");
    }
    if (state.header.version < (u32)PortableGameplaySchemaVersion::V5 ||
        !HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitGui))
    {
        evaluation.readiness = PortableRestoreReadiness::ShadowOnly;
        evaluation.blockingReasons.emplace_back("Portable GUI spellcard HUD state is missing; resave snapshot with schema V5.");
    }

    bool eclCoreOk = true;
    if (HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitEclCore))
    {
        eclCoreOk = ValidatePortableEclManagerCoreState(state, evaluation.blockingReasons);
    }
    bool stageCoreOk = true;
    if (HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitStageCore))
    {
        stageCoreOk = ValidatePortableStageCoreState(state, evaluation.blockingReasons);
    }

    if (!ValidatePortableEclScripts(state.eclScripts, evaluation.blockingReasons) || !eclCoreOk ||
        !stageCoreOk || !ValidatePortableEnemyActors(state.enemyActors, evaluation.blockingReasons))
    {
        evaluation.readiness = PortableRestoreReadiness::NeedsNormalization;
    }
    for (size_t i = 0; i < state.players.size(); ++i)
    {
        if (!ValidatePortablePlayerState(state.players[i], i, evaluation.blockingReasons))
        {
            evaluation.readiness = PortableRestoreReadiness::NeedsNormalization;
        }
    }
    if (!ValidatePortableBulletManagerState(state.bulletActors, evaluation.blockingReasons))
    {
        evaluation.readiness = PortableRestoreReadiness::NeedsNormalization;
    }
    if (!ValidatePortableItemManagerState(state.itemActors, evaluation.blockingReasons))
    {
        evaluation.readiness = PortableRestoreReadiness::NeedsNormalization;
    }
    if (!ValidatePortableEffectManagerState(state.effectActors, evaluation.blockingReasons))
    {
        evaluation.readiness = PortableRestoreReadiness::NeedsNormalization;
    }
    if (HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasShellSyncHints) &&
        !ValidatePortableShellSyncState(state, evaluation.blockingReasons))
    {
        evaluation.readiness = PortableRestoreReadiness::NeedsNormalization;
    }
    if (HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitGui) &&
        !ValidatePortableGuiState(state, evaluation.blockingReasons))
    {
        evaluation.readiness = PortableRestoreReadiness::NeedsNormalization;
    }

    if (!state.mustNormalizeBeforeRestore.empty())
    {
        evaluation.readiness = PortableRestoreReadiness::NeedsNormalization;
        for (const auto &entry : state.mustNormalizeBeforeRestore)
        {
            AppendBlockingReason(evaluation.blockingReasons, entry);
        }
    }

    if (!HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasIndependentRestore))
    {
        evaluation.readiness = PortableRestoreReadiness::NotBuildableYet;
        evaluation.blockingReasons.emplace_back("Independent gameplay world builder is not enabled yet.");
    }

    if (evaluation.blockingReasons.empty())
    {
        evaluation.readiness = PortableRestoreReadiness::Ready;
    }
    TracePortableValidationPhase("readiness-end");
}

PortableRestoreEvaluation EvaluatePortableRestoreReadiness(const PortableGameplayState &state)
{
    PortableRestoreEvaluation evaluation;
    EvaluatePortableRestoreReadiness(state, evaluation);
    return evaluation;
}

bool RunPortableGameplayDebugValidation(PortableGameplayBuildResult *outBuild, PortableRestoreEvaluation *outEvaluation,
                                        uint64_t *outBefore, uint64_t *outAfter)
{
    struct TraceGuard
    {
        TraceGuard()
        {
            SetPortableValidationTraceEnabled(true);
            TracePortableValidationPhase("validation-start");
        }
        ~TraceGuard()
        {
            TracePortableValidationPhase("validation-end");
            SetPortableValidationTraceEnabled(false);
        }
    } traceGuard;

    auto captured = std::make_unique<PortableGameplayState>();
    TracePortableValidationPhase("capture-start");
    CapturePortableGameplayState(*captured);
    TracePortableValidationPhase("capture-end");

    TracePortableValidationPhase("fingerprint-before-start");
    const u64 before = FingerprintPortableGameplayState(*captured);
    TracePortableValidationPhase("fingerprint-before-end");
    if (outBefore != nullptr)
    {
        *outBefore = before;
    }

    TracePortableValidationPhase("encode-start");
    const std::vector<u8> bytes = EncodePortableGameplayState(*captured);
    TracePortableValidationPhase("encode-end");
    auto decoded = std::make_unique<PortableGameplayState>();
    TracePortableValidationPhase("decode-start");
    if (!DecodePortableGameplayState(bytes, *decoded))
    {
        TracePortableValidationPhase("decode-fail");
        if (outAfter != nullptr)
        {
            *outAfter = 0;
        }
        if (outEvaluation != nullptr)
        {
            outEvaluation->blockingReasons.clear();
            outEvaluation->schemaMatches = false;
            outEvaluation->catalogMatches = false;
            outEvaluation->missingCaptureFlags = 0;
            outEvaluation->readiness = PortableRestoreReadiness::ShadowOnly;
            outEvaluation->blockingReasons.emplace_back("DecodePortableGameplayState failed during debug validation.");
        }
        if (outBuild != nullptr)
        {
            outBuild->notes.clear();
            outBuild->success = false;
            outBuild->evaluation = {};
            outBuild->shadowFingerprint = 0;
            outBuild->builtEnemyCount = 0;
            outBuild->notes.emplace_back("Portable gameplay debug validation failed during decode.");
        }
        return false;
    }
    TracePortableValidationPhase("decode-end");

    TracePortableValidationPhase("fingerprint-after-start");
    const u64 after = FingerprintPortableGameplayState(*decoded);
    TracePortableValidationPhase("fingerprint-after-end");
    if (outAfter != nullptr)
    {
        *outAfter = after;
    }

    auto build = std::make_unique<PortableGameplayBuildResult>();
    TracePortableValidationPhase("build-start");
    BuildPortableGameplayWorldFromState(*decoded, *build);
    TracePortableValidationPhase("build-end");
    if (outEvaluation != nullptr)
    {
        *outEvaluation = build->evaluation;
    }
    if (outBuild != nullptr)
    {
        *outBuild = *build;
    }
    if (before == after && build->success)
    {
        TracePortableValidationPhase("validation-success");
        return true;
    }

    TracePortableValidationPhase("validation-fail");
    return false;
}
} // namespace th06::DGS
