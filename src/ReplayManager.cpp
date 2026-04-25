#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Controller.hpp"
#include "BulletManager.hpp"
#include "EnemyManager.hpp"
#include "EffectManager.hpp"
#include "FileSystem.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "GamePaths.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "ReplayManager.hpp"
#include "Rng.hpp"
#include "Session.hpp"
#include "Supervisor.hpp"
#include "thprac_th06.h"
#include "utils.hpp"

namespace th06
{
DIFFABLE_STATIC(ReplayManager *, g_ReplayManager)
namespace
{
constexpr int kReplayStageCount = 7;
constexpr int kReplayInputCapacity = 53998;
constexpr u8 kReplayTraceLoggedPlayer1Miss = 1 << 0;
constexpr u8 kReplayTraceLoggedPlayer2Miss = 1 << 1;
constexpr u8 kReplayTraceLoggedLiveLeak = 1 << 2;
constexpr int kReplayTraceWindowStage = 4;
constexpr int kReplayTraceInitialWindowStart = 0;
constexpr int kReplayTraceInitialWindowEnd = 200;
constexpr int kReplayTraceCriticalWindowStart = 740;
constexpr int kReplayTraceCriticalWindowEnd = 770;

void WriteReplayTraceLine(const char *line)
{
    if (!THPrac::TH06::THPracIsDebugLogEnabled() || line == NULL || line[0] == '\0')
    {
        return;
    }

    char resolvedTracePath[512];
    GamePaths::Resolve(resolvedTracePath, sizeof(resolvedTracePath), "./replay_trace.log");
    GamePaths::EnsureParentDir(resolvedTracePath);

    FILE *traceFile = fopen(resolvedTracePath, "at");
    if (traceFile == NULL)
    {
        return;
    }

    fputs(line, traceFile);
    fclose(traceFile);
}

void ResetReplayTraceLog()
{
    char resolvedTracePath[512];
    GamePaths::Resolve(resolvedTracePath, sizeof(resolvedTracePath), "./replay_trace.log");
    GamePaths::EnsureParentDir(resolvedTracePath);

    if (!THPrac::TH06::THPracIsDebugLogEnabled())
    {
        std::remove(resolvedTracePath);
        return;
    }

    FILE *traceFile = fopen(resolvedTracePath, "wt");
    if (traceFile != NULL)
    {
        fclose(traceFile);
    }
}

void ReplayTraceLog(const char *fmt, ...)
{
    if (!THPrac::TH06::THPracIsDebugLogEnabled())
    {
        return;
    }

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    buffer[sizeof(buffer) - 1] = '\0';
    GameErrorContext::Log(&g_GameErrorContext, "%s", buffer);
    WriteReplayTraceLine(buffer);
}

struct LegacyStageReplayData
{
    i32 score;
    i16 randomSeed;
    i16 pointItemsCollected;
    u8 power;
    i8 livesRemaining;
    i8 bombsRemaining;
    u8 rank;
    i8 powerItemCountForScore;
    i8 padding[3];
    ReplayDataInput replayInputs[53998];
};
ZUN_ASSERT_SIZE(LegacyStageReplayData, 0x69780);

bool IsValidReplayStageIndex(int stageIndex)
{
    return stageIndex >= 0 && stageIndex < kReplayStageCount;
}

template <typename ReplayBlob>
ZunResult DecodeReplayBlob(ReplayBlob *data, i32 fileSize)
{
    u8 *checksumCursor;
    u32 checksum;
    u8 *obfuscateCursor;
    u8 obfOffset;
    i32 idx;

    if (data == NULL)
    {
        return ZUN_ERROR;
    }

    if (memcmp(data->magic, "T6RP", 4) != 0)
    {
        return ZUN_ERROR;
    }

    obfuscateCursor = (u8 *)&data->rngValue3;
    obfOffset = data->key;
    for (idx = 0; idx < fileSize - (i32)offsetof(ReplayBlob, rngValue3); idx += 1, obfuscateCursor += 1)
    {
        *obfuscateCursor -= obfOffset;
        obfOffset += 7;
    }

    checksumCursor = (u8 *)&data->key;
    checksum = 0x3f000318;
    for (idx = 0; idx < fileSize - (i32)offsetof(ReplayBlob, key); idx += 1, checksumCursor += 1)
    {
        checksum += *checksumCursor;
    }

    if (checksum != (u32)data->checksum)
    {
        return ZUN_ERROR;
    }

    if (!IsValidReplayVersion(data->version))
    {
        return ZUN_ERROR;
    }

    return ZUN_SUCCESS;
}

// =============================================================================
//  On-disk replay → in-memory ReplayData translation
//
//  These routines read the wire formats defined in ReplayData.hpp and produce
//  a fully-relocated ReplayData* on heap, with stageReplayData[i] pointing to
//  StageReplayData blocks inside the same allocation. After this returns,
//  callers can treat the result like the in-memory layout would have looked
//  on a 32-bit native build — pointers are real pointers, no fixup needed.
//
//  This makes 64-bit builds bit-compatible with 32-bit replays: the wire
//  format never changes, only the in-memory pointer width.
// =============================================================================

template <typename DiskHdr>
ReplayData *ReifyReplayBlob(const u8 *rawData, i32 fileSize)
{
    static_assert(offsetof(DiskHdr, key) == offsetof(DiskHdr, rngValue3) - 1,
                  "key/rngValue3 expected adjacent");

    if (rawData == NULL || fileSize < (i32)sizeof(DiskHdr))
    {
        return NULL;
    }

    const DiskHdr *disk = reinterpret_cast<const DiskHdr *>(rawData);
    const size_t newHeaderSize = sizeof(ReplayData);
    const size_t oldHeaderSize = sizeof(DiskHdr);
    const size_t payloadSize = (size_t)fileSize - oldHeaderSize;
    const size_t convertedSize = newHeaderSize + payloadSize;

    ReplayData *replayData = reinterpret_cast<ReplayData *>(malloc(convertedSize));
    if (replayData == NULL)
    {
        return NULL;
    }
    memset(replayData, 0, convertedSize);

    // Copy fields that share names. shottypeChara2 only exists in the
    // post-vanilla format; for legacy files we duplicate shottypeChara so the
    // single-player flow keeps working.
    memcpy(replayData->magic, disk->magic, sizeof(replayData->magic));
    replayData->version = disk->version;
    replayData->shottypeChara = disk->shottypeChara;
    if constexpr (offsetof(DiskHdr, stageOffset) == offsetof(OnDiskReplayData, stageOffset))
    {
        replayData->shottypeChara2 = reinterpret_cast<const OnDiskReplayData *>(disk)->shottypeChara2;
    }
    else
    {
        replayData->shottypeChara2 = disk->shottypeChara;
    }
    replayData->difficulty = disk->difficulty;
    replayData->checksum = disk->checksum;
    replayData->rngValue1 = disk->rngValue1;
    replayData->rngValue2 = disk->rngValue2;
    replayData->key = disk->key;
    replayData->rngValue3 = disk->rngValue3;
    memcpy(replayData->date, disk->date, sizeof(replayData->date));
    memcpy(replayData->name, disk->name, sizeof(replayData->name));
    replayData->score = disk->score;
    replayData->slowdownRate2 = disk->slowdownRate2;
    replayData->slowdownRate = disk->slowdownRate;
    replayData->slowdownRate3 = disk->slowdownRate3;

    // Copy payload bytes verbatim (stage blocks).
    if (payloadSize > 0)
    {
        memcpy(reinterpret_cast<u8 *>(replayData) + newHeaderSize,
               rawData + oldHeaderSize, payloadSize);
    }

    // Translate u32 file offsets → real pointers into the new buffer.
    const size_t stageHeaderSize = offsetof(StageReplayData, replayInputs);
    for (i32 idx = 0; idx < kReplayStageCount; idx += 1)
    {
        u32 fileOffset = disk->stageOffset[idx];
        if (fileOffset == 0)
        {
            replayData->stageReplayData[idx] = NULL;
            continue;
        }
        if (fileOffset < oldHeaderSize ||
            (size_t)fileOffset + stageHeaderSize > (size_t)fileSize)
        {
            replayData->stageReplayData[idx] = NULL;
            continue;
        }
        const size_t newStageOffset = (size_t)fileOffset - oldHeaderSize + newHeaderSize;
        StageReplayData *stage =
            reinterpret_cast<StageReplayData *>(reinterpret_cast<u8 *>(replayData) + newStageOffset);
        replayData->stageReplayData[idx] = stage;

        // Legacy format had unused padding[3] where the new format has
        // power2/livesRemaining2/bombsRemaining2; zero those for safety.
        if constexpr (offsetof(DiskHdr, stageOffset) != offsetof(OnDiskReplayData, stageOffset))
        {
            stage->power2 = 0;
            stage->livesRemaining2 = 0;
            stage->bombsRemaining2 = 0;
        }
    }

    g_LastFileSize = (u32)convertedSize;
    return replayData;
}

void CleanupFailedReplayManagerRegistration(ReplayManager *mgr)
{
    if (mgr == NULL)
    {
        return;
    }

    if (mgr->calcChain != NULL)
    {
        g_Chain.Cut(mgr->calcChain);
        return;
    }

    if (mgr->drawChain != NULL)
    {
        g_Chain.Cut(mgr->drawChain);
    }
    if (mgr->calcChainDemoHighPrio != NULL)
    {
        g_Chain.Cut(mgr->calcChainDemoHighPrio);
    }
    if (mgr->replayData != NULL)
    {
        free(mgr->replayData);
        mgr->replayData = NULL;
    }
    if (g_ReplayManager == mgr)
    {
        g_ReplayManager = NULL;
    }
    delete mgr;
}

bool ShouldLogReplayTraceWindow(int stage, int frameId)
{
    if (stage != kReplayTraceWindowStage)
    {
        return false;
    }

    if (frameId >= kReplayTraceInitialWindowStart && frameId <= kReplayTraceInitialWindowEnd)
    {
        return true;
    }

    return frameId >= kReplayTraceCriticalWindowStart && frameId <= kReplayTraceCriticalWindowEnd;
}

const char *ReplayTraceWindowTag(int frameId)
{
    if (frameId <= kReplayTraceInitialWindowEnd)
    {
        return "initial";
    }

    return "critical";
}

int CountActiveLasers()
{
    int activeLasers = 0;

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.lasers); ++idx)
    {
        if (g_BulletManager.lasers[idx].inUse != 0)
        {
            activeLasers += 1;
        }
    }

    return activeLasers;
}

int CountActivePlayerBullets(const Player &player)
{
    int activeBullets = 0;

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(player.bullets); ++idx)
    {
        if (player.bullets[idx].bulletState != BULLET_STATE_UNUSED)
        {
            activeBullets += 1;
        }
    }

    return activeBullets;
}

void LogReplayBoot(const ReplayManager *mgr, const StageReplayData *stageReplayData)
{
    if (mgr == NULL || mgr->replayData == NULL || stageReplayData == NULL)
    {
        return;
    }

    const ReplayDataInput *firstInput = stageReplayData->replayInputs;
    const ReplayDataInput *secondInput = firstInput + 1;
    ReplayTraceLog(
        "[ReplayBoot] stage=%d demo=%d diff=%d shot=%d seed=%d lives=%d bombs=%d power=%u "
        "frame0=%d input0=0x%04x frame1=%d input1=0x%04x extra={shot2:%d lives2:%d bombs2:%d power2:%u}\n",
        g_GameManager.currentStage, mgr->isDemo, mgr->replayData->difficulty, mgr->replayData->shottypeChara,
        stageReplayData->randomSeed, stageReplayData->livesRemaining, stageReplayData->bombsRemaining,
        stageReplayData->power, firstInput->frameNum, firstInput->inputKey, secondInput->frameNum,
        secondInput->inputKey, mgr->replayData->shottypeChara2, stageReplayData->livesRemaining2,
        stageReplayData->bombsRemaining2, stageReplayData->power2);
}

void LogReplayMiss(ReplayManager *mgr, const Player &player)
{
    if (mgr == NULL)
    {
        return;
    }

    const u8 playerBit = player.playerType == 2 ? kReplayTraceLoggedPlayer2Miss : kReplayTraceLoggedPlayer1Miss;
    if ((mgr->replayTraceFlags & playerBit) != 0 || player.playerState != PLAYER_STATE_DEAD)
    {
        return;
    }

    mgr->replayTraceFlags |= playerBit;
    ReplayTraceLog("[ReplayTrace] event=miss player=%d stage=%d frameId=%d gameFrames=%u score=%u pos=(%.3f,%.3f,%.3f) "
                   "lives=%d bombs=%d power=%u input=0x%04x last=0x%04x rngSeed=%u rngGen=%u\n",
                   player.playerType, g_GameManager.currentStage, mgr->frameId, g_GameManager.gameFrames,
                   g_GameManager.score, player.positionCenter.x, player.positionCenter.y, player.positionCenter.z,
                   player.playerType == 1 ? g_GameManager.livesRemaining : g_GameManager.livesRemaining2,
                   player.playerType == 1 ? g_GameManager.bombsRemaining : g_GameManager.bombsRemaining2,
                   player.playerType == 1 ? g_GameManager.currentPower : g_GameManager.currentPower2, g_CurFrameInput,
                   g_LastFrameInput, g_Rng.seed, g_Rng.generationCount);
}

void LogReplayLiveInputLeak(ReplayManager *mgr, u16 liveUncapturedInput)
{
    if (mgr == NULL || liveUncapturedInput == 0 || (mgr->replayTraceFlags & kReplayTraceLoggedLiveLeak) != 0)
    {
        return;
    }

    mgr->replayTraceFlags |= kReplayTraceLoggedLiveLeak;
    ReplayTraceLog("[ReplayTrace] event=live-input-leak stage=%d frameId=%d gameFrames=%u live=0x%04x replay=0x%04x\n",
                   g_GameManager.currentStage, mgr->frameId, g_GameManager.gameFrames, liveUncapturedInput,
                   mgr->replayInputs != NULL ? mgr->replayInputs->inputKey : 0);
}

float DistSq(const D3DXVECTOR3 &a, const D3DXVECTOR3 &b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

void LogReplayTraceWindow(ReplayManager *mgr)
{
    if (mgr == NULL || !ShouldLogReplayTraceWindow(g_GameManager.currentStage, mgr->frameId))
    {
        return;
    }

    const Player &player = g_Player;
    StageReplayData *stageReplayData = nullptr;
    ReplayDataInput *replayInputBegin = nullptr;
    ReplayDataInput *replayInputEnd = nullptr;

    if (mgr->replayData != NULL)
    {
        const int stageIndex = g_GameManager.currentStage - 1;
        if (IsValidReplayStageIndex(stageIndex))
        {
            stageReplayData = mgr->replayData->stageReplayData[stageIndex];
        }
    }
    if (stageReplayData != NULL)
    {
        replayInputBegin = stageReplayData->replayInputs;
        replayInputEnd = replayInputBegin + kReplayInputCapacity;
    }

    const Bullet *nearestBullet = nullptr;
    int nearestBulletIdx = -1;
    float nearestBulletDistSq = 0.0f;
    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.bullets); ++idx)
    {
        const Bullet &bullet = g_BulletManager.bullets[idx];
        if (bullet.state == 0)
        {
            continue;
        }

        const float distSq = DistSq(player.positionCenter, bullet.pos);
        if (nearestBullet == nullptr || distSq < nearestBulletDistSq)
        {
            nearestBullet = &bullet;
            nearestBulletIdx = idx;
            nearestBulletDistSq = distSq;
        }
    }

    const Enemy *nearestEnemy = nullptr;
    int nearestEnemyIdx = -1;
    float nearestEnemyDistSq = 0.0f;
    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_EnemyManager.enemies) - 1; ++idx)
    {
        const Enemy &enemy = g_EnemyManager.enemies[idx];
        if (!enemy.flags.unk5)
        {
            continue;
        }

        const float distSq = DistSq(player.positionCenter, enemy.position);
        if (nearestEnemy == nullptr || distSq < nearestEnemyDistSq)
        {
            nearestEnemy = &enemy;
            nearestEnemyIdx = idx;
            nearestEnemyDistSq = distSq;
        }
    }

    const ReplayDataInput *currentReplayInput =
        (replayInputBegin != nullptr && mgr->replayInputs != nullptr &&
         mgr->replayInputs >= replayInputBegin && mgr->replayInputs < replayInputEnd)
            ? mgr->replayInputs
            : nullptr;
    const ReplayDataInput *nextReplayInput =
        (currentReplayInput != nullptr && currentReplayInput + 1 < replayInputEnd) ? currentReplayInput + 1 : nullptr;
    const RunningSpellcardInfo &spellInfo = g_EnemyManager.spellcardInfo;

    ReplayTraceLog(
        "[ReplayTrace] event=window stage=%d frameId=%d gameFrames=%u eff=%.3f rank=%d score=%u rngSeed=%u rngGen=%u "
        "input=0x%04x last=0x%04x player={state:%d pos:(%.3f,%.3f,%.3f) respawn:%d inv:%d grace:%d} "
        "bullet={idx:%d dist2:%.3f state:%u grazed:%u pos:(%.3f,%.3f,%.3f) vel:(%.3f,%.3f,%.3f) speed:%.3f angle:%.3f} "
        "enemy={idx:%d dist2:%.3f life:%d boss:%d pos:(%.3f,%.3f,%.3f) speed:%.3f angle:%.3f} "
        "world={bullets:%d enemies:%d timeline:%d spell:%d} "
        "extra={window:%s guiScore:%u grazeStage:%d grazeTotal:%d items:%u effects:%d lasers:%d p1Bullets:%d "
        "p2={state:%d pos:(%.3f,%.3f,%.3f) respawn:%d inv:%d} "
        "replay={frame:%d input:0x%04x nextFrame:%d nextInput:0x%04x held:%u} "
        "spellInfo={active:%d capture:%d usedBomb:%d idx:%u score:%d}}\n",
        g_GameManager.currentStage, mgr->frameId, g_GameManager.gameFrames, g_Supervisor.effectiveFramerateMultiplier,
        g_GameManager.rank, g_GameManager.score, g_Rng.seed, g_Rng.generationCount, g_CurFrameInput, g_LastFrameInput,
        player.playerState, player.positionCenter.x, player.positionCenter.y, player.positionCenter.z, player.respawnTimer,
        player.invulnerabilityTimer.current, player.bulletGracePeriod, nearestBulletIdx, nearestBulletDistSq,
        nearestBullet != nullptr ? nearestBullet->state : 0, nearestBullet != nullptr ? nearestBullet->isGrazed : 0,
        nearestBullet != nullptr ? nearestBullet->pos.x : 0.0f, nearestBullet != nullptr ? nearestBullet->pos.y : 0.0f,
        nearestBullet != nullptr ? nearestBullet->pos.z : 0.0f,
        nearestBullet != nullptr ? nearestBullet->velocity.x : 0.0f,
        nearestBullet != nullptr ? nearestBullet->velocity.y : 0.0f,
        nearestBullet != nullptr ? nearestBullet->velocity.z : 0.0f, nearestBullet != nullptr ? nearestBullet->speed : 0.0f,
        nearestBullet != nullptr ? nearestBullet->angle : 0.0f, nearestEnemyIdx, nearestEnemyDistSq,
        nearestEnemy != nullptr ? nearestEnemy->life : 0, nearestEnemy != nullptr ? (nearestEnemy->flags.isBoss ? 1 : 0) : 0,
        nearestEnemy != nullptr ? nearestEnemy->position.x : 0.0f, nearestEnemy != nullptr ? nearestEnemy->position.y : 0.0f,
        nearestEnemy != nullptr ? nearestEnemy->position.z : 0.0f, nearestEnemy != nullptr ? nearestEnemy->speed : 0.0f,
        nearestEnemy != nullptr ? nearestEnemy->angle : 0.0f, g_BulletManager.bulletCount, g_EnemyManager.enemyCount,
        g_EnemyManager.timelineTime.current, g_Gui.SpellcardSecondsRemaining(), ReplayTraceWindowTag(mgr->frameId),
        g_GameManager.guiScore, g_GameManager.grazeInStage, g_GameManager.grazeInTotal, g_ItemManager.itemCount,
        g_EffectManager.activeEffects, CountActiveLasers(), CountActivePlayerBullets(g_Player), g_Player2.playerState,
        g_Player2.positionCenter.x, g_Player2.positionCenter.y, g_Player2.positionCenter.z, g_Player2.respawnTimer,
        g_Player2.invulnerabilityTimer.current, currentReplayInput != nullptr ? currentReplayInput->frameNum : -1,
        currentReplayInput != nullptr ? currentReplayInput->inputKey : 0,
        nextReplayInput != nullptr ? nextReplayInput->frameNum : -1,
        nextReplayInput != nullptr ? nextReplayInput->inputKey : 0, mgr->replayHeldFrames,
        spellInfo.isActive ? 1 : 0, spellInfo.isCapturing ? 1 : 0, spellInfo.usedBomb ? 1 : 0, spellInfo.idx,
        spellInfo.captureScore);
}
} // namespace

#pragma var_order(idx, decryptedData, obfOffset, obfuscateCursor, checksum, checksumCursor)
ZunResult ReplayManager::ValidateReplayData(ReplayData *data, i32 fileSize)
{
    // Legacy entry point: kept for ABI but no longer used by LoadReplayData.
    // Operates on an OnDiskReplayData-shaped header laid into the same memory.
    return DecodeReplayBlob(reinterpret_cast<OnDiskReplayData *>(data), fileSize);
}

ReplayData *ReplayManager::LoadReplayData(char *replayFile, int isExternalResource)
{
    u8 *rawData = FileSystem::OpenPath(replayFile, isExternalResource);
    if (rawData == NULL)
    {
        return NULL;
    }

    ResetReplayTraceLog();

    const i32 fileSize = g_LastFileSize;

    // Try the current 84-byte wire format first. DecodeReplayBlob does
    // in-place decrypt on rawData, so we make a working copy to retry
    // with the legacy format if validation fails.
    if (fileSize >= (i32)sizeof(OnDiskReplayData))
    {
        u8 *workCopy = reinterpret_cast<u8 *>(malloc((size_t)fileSize));
        if (workCopy != NULL)
        {
            memcpy(workCopy, rawData, (size_t)fileSize);
            if (DecodeReplayBlob(reinterpret_cast<OnDiskReplayData *>(workCopy), fileSize) == ZUN_SUCCESS)
            {
                ReplayData *replayData = ReifyReplayBlob<OnDiskReplayData>(workCopy, fileSize);
                free(workCopy);
                if (replayData != NULL)
                {
                    ReplayTraceLog("[ReplayLoad] path=%s format=current size=%d\n",
                                   replayFile != NULL ? replayFile : "<null>", fileSize);
                    free(rawData);
                    return replayData;
                }
            }
            free(workCopy);
        }
    }

    // Try legacy 80-byte format (original ZUN files).
    if (fileSize >= (i32)sizeof(OnDiskLegacyReplayData))
    {
        if (DecodeReplayBlob(reinterpret_cast<OnDiskLegacyReplayData *>(rawData), fileSize) == ZUN_SUCCESS)
        {
            ReplayData *replayData = ReifyReplayBlob<OnDiskLegacyReplayData>(rawData, fileSize);
            if (replayData != NULL)
            {
                ReplayTraceLog("[ReplayLoad] path=%s format=legacy size=%d\n",
                               replayFile != NULL ? replayFile : "<null>", fileSize);
            }
            free(rawData);
            return replayData;
        }
    }

    free(rawData);
    return NULL;
}

ZunResult ReplayManager::RegisterChain(i32 isDemo, char *replayFile)
{
    ReplayManager *replayMgr;

    if (g_Supervisor.framerateMultiplier < 0.99f && !isDemo)
    {
        return ZUN_SUCCESS;
    }
    g_Supervisor.framerateMultiplier = 1.0f;
    if (g_ReplayManager == NULL)
    {
        replayMgr = new ReplayManager();
        g_ReplayManager = replayMgr;
        replayMgr->replayData = NULL;
        replayMgr->isDemo = isDemo;
        replayMgr->replayFile = replayFile;
        switch (isDemo)
        {
        case false:
            replayMgr->calcChain = g_Chain.CreateElem((ChainCallback)ReplayManager::OnUpdate);
            replayMgr->calcChain->addedCallback = (ChainAddedCallback)AddedCallback;
            replayMgr->calcChain->deletedCallback = (ChainDeletedCallback)DeletedCallback;
            replayMgr->drawChain = g_Chain.CreateElem((ChainCallback)ReplayManager::OnDraw);
            replayMgr->calcChain->arg = replayMgr;
            if (g_Chain.AddToCalcChain(replayMgr->calcChain, TH_CHAIN_PRIO_CALC_REPLAYMANAGER))
            {
                CleanupFailedReplayManagerRegistration(replayMgr);
                return ZUN_ERROR;
            }
            replayMgr->calcChainDemoHighPrio = NULL;
            break;
        case true:
            replayMgr->calcChain = g_Chain.CreateElem((ChainCallback)ReplayManager::OnUpdateDemoHighPrio);
            replayMgr->calcChain->addedCallback = (ChainAddedCallback)AddedCallbackDemo;
            replayMgr->calcChain->deletedCallback = (ChainDeletedCallback)DeletedCallback;
            replayMgr->drawChain = g_Chain.CreateElem((ChainCallback)ReplayManager::OnDraw);
            replayMgr->calcChain->arg = replayMgr;
            if (g_Chain.AddToCalcChain(replayMgr->calcChain, TH_CHAIN_PRIO_CALC_LOW_PRIO_REPLAYMANAGER_DEMO))
            {
                CleanupFailedReplayManagerRegistration(replayMgr);
                return ZUN_ERROR;
            }
            replayMgr->calcChainDemoHighPrio = g_Chain.CreateElem((ChainCallback)ReplayManager::OnUpdateDemoLowPrio);
            replayMgr->calcChainDemoHighPrio->arg = replayMgr;
            if (g_Chain.AddToCalcChain(replayMgr->calcChainDemoHighPrio, TH_CHAIN_PRIO_CALC_HIGH_PRIO_REPLAYMANAGER_DEMO))
            {
                CleanupFailedReplayManagerRegistration(replayMgr);
                return ZUN_ERROR;
            }
            break;
        }
        replayMgr->drawChain->arg = replayMgr;
        if (g_Chain.AddToDrawChain(replayMgr->drawChain, TH_CHAIN_PRIO_DRAW_REPLAYMANAGER))
        {
            CleanupFailedReplayManagerRegistration(replayMgr);
            return ZUN_ERROR;
        }
    }
    else
    {
        switch (isDemo)
        {
        case false:
            if (AddedCallback(g_ReplayManager) != ZUN_SUCCESS)
            {
                CleanupFailedReplayManagerRegistration(g_ReplayManager);
                return ZUN_ERROR;
            }
            break;
        case true:
            if (AddedCallbackDemo(g_ReplayManager) != ZUN_SUCCESS)
            {
                CleanupFailedReplayManagerRegistration(g_ReplayManager);
                return ZUN_ERROR;
            }
            return ZUN_SUCCESS;
            break;
        }
    }
    return ZUN_SUCCESS;
}

#define TH_BUTTON_REPLAY_CAPTURE                                                                                       \
    (TH_BUTTON_SHOOT | TH_BUTTON_BOMB | TH_BUTTON_FOCUS | TH_BUTTON_SKIP | TH_BUTTON_DIRECTION | TH_BUTTON_SHOOT2 |   \
     TH_BUTTON_BOMB2 | TH_BUTTON_FOCUS2 | TH_BUTTON_DIRECTION2)

ChainCallbackResult ReplayManager::OnUpdate(ReplayManager *mgr)
{
    u16 inputs;

    if (!g_GameManager.isInMenu || mgr == NULL || mgr->replayInputs == NULL)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    inputs = IS_PRESSED(TH_BUTTON_REPLAY_CAPTURE);

    // Encode current analog state as signed bytes for the replay stream.
    // Two modes determined by AnalogMode:
    //   Direction:    standard encoding — direction × magnitude scaled to [-127, 127]
    //   Displacement: pixel delta per frame — already integer, stored raw
    // Displacement mode sets bit 3 (TH_BUTTON_MENU) in inputKey as a per-frame
    // flag so playback knows how to interpret the analog fields.
    const AnalogInput &analog = Controller::GetAnalogInput();
    i8 aX = 0, aY = 0;
    if (analog.active)
    {
        if (analog.mode == AnalogMode::Displacement)
        {
            // Pixel displacement: already clamped to ±127 and rounded in AndroidTouchInput.
            aX = static_cast<i8>(analog.x);
            aY = static_cast<i8>(analog.y);
            inputs |= TH_BUTTON_MENU; // per-frame displacement flag
        }
        else
        {
            float clampedX = analog.x < -1.0f ? -1.0f : (analog.x > 1.0f ? 1.0f : analog.x);
            float clampedY = analog.y < -1.0f ? -1.0f : (analog.y > 1.0f ? 1.0f : analog.y);
            aX = static_cast<i8>(clampedX * 127.0f);
            aY = static_cast<i8>(clampedY * 127.0f);
        }
    }

    if (inputs != mgr->replayInputs->inputKey ||
        aX != mgr->replayInputs->analogX ||
        aY != mgr->replayInputs->analogY)
    {
        mgr->replayInputs += 1;
        mgr->replayInputStageBookmarks[g_GameManager.currentStage - 1] = mgr->replayInputs + 1;
        mgr->replayInputs->frameNum = mgr->frameId;
        mgr->replayInputs->inputKey = inputs;
        mgr->replayInputs->analogX = aX;
        mgr->replayInputs->analogY = aY;
    }
    mgr->frameId += 1;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult ReplayManager::OnUpdateDemoLowPrio(ReplayManager *mgr)
{
    if (!g_GameManager.isInMenu)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    if (g_Gui.HasCurrentMsgIdx() && g_Gui.IsDialogueSkippable() && mgr->frameId % 3 != 2)
    {
        return CHAIN_CALLBACK_RESULT_RESTART_FROM_FIRST_JOB;
    }
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult ReplayManager::OnUpdateDemoHighPrio(ReplayManager *mgr)
{
    if (!g_GameManager.isInMenu || mgr == NULL || mgr->replayData == NULL)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    const int stageIndex = g_GameManager.currentStage - 1;
    if (!IsValidReplayStageIndex(stageIndex))
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    StageReplayData *replayData = mgr->replayData->stageReplayData[stageIndex];
    if (replayData == NULL)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    ReplayDataInput *const replayInputBegin = replayData->replayInputs;
    ReplayDataInput *const replayInputEnd = replayData->replayInputs + kReplayInputCapacity;
    const std::uintptr_t replayInputAddr = reinterpret_cast<std::uintptr_t>(mgr->replayInputs);
    const std::uintptr_t replayInputBeginAddr = reinterpret_cast<std::uintptr_t>(replayInputBegin);
    const std::uintptr_t replayInputEndAddr = reinterpret_cast<std::uintptr_t>(replayInputEnd);
    if (replayInputAddr < replayInputBeginAddr || replayInputAddr >= replayInputEndAddr)
    {
        mgr->replayInputs = replayInputBegin;
    }

    while (mgr->replayInputs + 1 < replayInputEnd && mgr->frameId >= mgr->replayInputs[1].frameNum)
    {
        mgr->replayInputs += 1;
    }

    // Replay/demo must override only the current frame's input. `Session::AdvanceFrameInput()`
    // has already advanced g_LastFrameInput at the start of the frame, and calling
    // ApplyLegacyFrameInput() again here would incorrectly replace it with the live SDL input
    // sampled earlier in the same frame, breaking replay edge/held semantics.
    const u16 previousHeldFrames = mgr->replayHeldFrames;
    const u16 liveUncapturedInput = IS_PRESSED(0xFFFFFFFF & ~TH_BUTTON_REPLAY_CAPTURE);
    LogReplayLiveInputLeak(mgr, liveUncapturedInput);

    // Check per-frame displacement flag (bit 3 = TH_BUTTON_MENU).
    const u16 replayInputKey = mgr->replayInputs->inputKey;
    const bool isDisplacementFrame = (replayInputKey & TH_BUTTON_MENU) != 0;
    // Strip the displacement flag before applying to game input.
    g_CurFrameInput = liveUncapturedInput | (replayInputKey & ~TH_BUTTON_MENU);

    // Restore analog direction from replay data (version >= 0x103 only).
    // For old replays (0x102), the padding field may contain garbage,
    // so we must NOT read it — the player will use discrete-only movement.
    if (mgr->replayData->version >= REPLAY_VERSION_ANALOG)
    {
        i8 aX = mgr->replayInputs->analogX;
        i8 aY = mgr->replayInputs->analogY;
        if (isDisplacementFrame)
        {
            // Displacement mode: analog fields contain pixel delta (raw i8).
            AnalogInput replayAnalog = {};
            replayAnalog.x = static_cast<float>(aX);
            replayAnalog.y = static_cast<float>(aY);
            replayAnalog.active = true;
            replayAnalog.mode = AnalogMode::Displacement;
            Controller::SetAnalogInput(replayAnalog);
        }
        else if (aX != 0 || aY != 0)
        {
            AnalogInput replayAnalog;
            replayAnalog.x = aX / 127.0f;
            replayAnalog.y = aY / 127.0f;
            replayAnalog.active = true;
            Controller::SetAnalogInput(replayAnalog);
        }
        else
        {
            // Analog (0,0) from replay: no analog source was active during recording.
            // Synthesize from button flags (same as live keyboard behavior).
            AnalogInput noAnalog = {};
            float dx = 0.0f, dy = 0.0f;
            if (g_CurFrameInput & TH_BUTTON_LEFT)  dx -= 1.0f;
            if (g_CurFrameInput & TH_BUTTON_RIGHT) dx += 1.0f;
            if (g_CurFrameInput & TH_BUTTON_UP)    dy -= 1.0f;
            if (g_CurFrameInput & TH_BUTTON_DOWN)  dy += 1.0f;
            if (dx != 0.0f && dy != 0.0f)
            {
                constexpr float kInvSqrt2 = 0.70710678118f;
                dx *= kInvSqrt2;
                dy *= kInvSqrt2;
            }
            noAnalog.x = dx;
            noAnalog.y = dy;
            noAnalog.active = false;
            Controller::SetAnalogInput(noAnalog);
        }
    }
    else
    {
        // Old replay format (0x102): no analog data in the file.
        // CRITICAL: Clear live analog state to prevent leak.
        AnalogInput noAnalog = {};
        float dx = 0.0f, dy = 0.0f;
        if (g_CurFrameInput & TH_BUTTON_LEFT)  dx -= 1.0f;
        if (g_CurFrameInput & TH_BUTTON_RIGHT) dx += 1.0f;
        if (g_CurFrameInput & TH_BUTTON_UP)    dy -= 1.0f;
        if (g_CurFrameInput & TH_BUTTON_DOWN)  dy += 1.0f;
        if (dx != 0.0f && dy != 0.0f)
        {
            constexpr float kInvSqrt2 = 0.70710678118f;
            dx *= kInvSqrt2;
            dy *= kInvSqrt2;
        }
        noAnalog.x = dx;
        noAnalog.y = dy;
        noAnalog.active = false;
        Controller::SetAnalogInput(noAnalog);
    }

    g_IsEigthFrameOfHeldInput = 0;
    if (g_LastFrameInput == g_CurFrameInput)
    {
        u16 heldFrames = previousHeldFrames;
        if (30 <= heldFrames)
        {
            if (heldFrames % 8 == 0)
            {
                g_IsEigthFrameOfHeldInput = 1;
            }
            if (38 <= heldFrames)
            {
                heldFrames = 30;
            }
        }
        heldFrames++;
        g_NumOfFramesInputsWereHeld = heldFrames;
    }
    else
    {
        g_NumOfFramesInputsWereHeld = 0;
    }
    mgr->replayHeldFrames = g_NumOfFramesInputsWereHeld;
    LogReplayTraceWindow(mgr);
    LogReplayMiss(mgr, g_Player);
    LogReplayMiss(mgr, g_Player2);
    mgr->frameId += 1;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult ReplayManager::OnDraw(ReplayManager *mgr)
{
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

__inline StageReplayData *AllocateStageReplayData(i32 size)
{
    return (StageReplayData *)malloc(size);
}

__inline void ReleaseReplayData(void *data)
{
    return free(data);
}

__inline void ReleaseStageReplayData(void *data)
{
    return free(data);
}

#pragma var_order(stageReplayData, idx, oldStageReplayData)
ZunResult ReplayManager::AddedCallback(ReplayManager *mgr)
{
    StageReplayData *stageReplayData;
    StageReplayData *oldStageReplayData;
    i32 idx;

    mgr->frameId = 0;
    if (mgr->replayData == NULL)
    {
        mgr->replayData = new ReplayData();
        memcpy(&mgr->replayData->magic[0], "T6RP", 4);
        mgr->replayData->shottypeChara = g_GameManager.character * 2 + g_GameManager.shotType;
        mgr->replayData->shottypeChara2 = g_GameManager.character2 * 2 + g_GameManager.shotType2;
        mgr->replayData->version = REPLAY_VERSION_ANALOG;
        mgr->replayData->difficulty = g_GameManager.difficulty;
        utils::CopyStringToFixedField(mgr->replayData->name, sizeof(mgr->replayData->name), "NO NAME");
        for (idx = 0; idx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData); idx += 1)
        {
            mgr->replayData->stageReplayData[idx] = NULL;
        }
    }
    else
    {
        oldStageReplayData = mgr->replayData->stageReplayData[g_GameManager.currentStage - 2];
        if (oldStageReplayData == NULL)
        {
            return ZUN_ERROR;
        }
        oldStageReplayData->score = g_GameManager.score;
    }
    if (mgr->replayData->stageReplayData[g_GameManager.currentStage - 1] != NULL)
    {
        utils::DebugPrint2("error : replay.cpp");
    }
    mgr->replayData->stageReplayData[g_GameManager.currentStage - 1] = AllocateStageReplayData(sizeof(StageReplayData));
    stageReplayData = mgr->replayData->stageReplayData[g_GameManager.currentStage - 1];
    stageReplayData->bombsRemaining = g_GameManager.bombsRemaining;
    stageReplayData->livesRemaining = g_GameManager.livesRemaining;
    stageReplayData->power = g_GameManager.currentPower;
    stageReplayData->bombsRemaining2 = g_GameManager.bombsRemaining2;
    stageReplayData->livesRemaining2 = g_GameManager.livesRemaining2;
    stageReplayData->power2 = (u8)g_GameManager.currentPower2;
    stageReplayData->rank = g_GameManager.rank;
    stageReplayData->pointItemsCollected = g_GameManager.pointItemsCollected;
    stageReplayData->randomSeed = g_GameManager.randomSeed;
    stageReplayData->powerItemCountForScore = g_GameManager.powerItemCountForScore;
    mgr->replayInputs = stageReplayData->replayInputs;
    mgr->replayInputs->frameNum = 0;
    mgr->replayInputs->inputKey = 0;
    mgr->replayInputs->padding = 0;  // analogX = analogY = 0 (no analog input)
    mgr->replayHeldFrames = 0;
    mgr->replayTraceFlags = 0;
    mgr->unk44 = 0;
    return ZUN_SUCCESS;
}

ZunResult ReplayManager::AddedCallbackDemo(ReplayManager *mgr)
{
    i32 idx;
    StageReplayData *replayData;

    mgr->frameId = 0;
    mgr->replayInputs = NULL;
    if (mgr->replayData == NULL)
    {
        if (mgr->replayFile == NULL)
        {
            return ZUN_ERROR;
        }

        mgr->replayData = ReplayManager::LoadReplayData(mgr->replayFile, g_GameManager.demoMode == 0);
        if (mgr->replayData == NULL)
        {
            return ZUN_ERROR;
        }
        // LoadReplayData returns a buffer with stageReplayData[i] already
        // pointing to real StageReplayData blocks (or NULL). No fixup needed.
    }

    const int stageIndex = g_GameManager.currentStage - 1;
    if (!IsValidReplayStageIndex(stageIndex))
    {
        return ZUN_ERROR;
    }

    if (mgr->replayData->stageReplayData[stageIndex] == NULL)
    {
        return ZUN_ERROR;
    }
    replayData = mgr->replayData->stageReplayData[stageIndex];
    g_GameManager.character = mgr->replayData->shottypeChara / 2;
    g_GameManager.shotType = mgr->replayData->shottypeChara % 2;
    g_GameManager.character2 = mgr->replayData->shottypeChara2 / 2;
    g_GameManager.shotType2 = mgr->replayData->shottypeChara2 % 2;
    g_GameManager.difficulty = (Difficulty)mgr->replayData->difficulty;
    g_GameManager.pointItemsCollected = replayData->pointItemsCollected;
    g_Rng.Initialize(replayData->randomSeed);
    g_GameManager.rank = replayData->rank;
    g_GameManager.livesRemaining = replayData->livesRemaining;
    g_GameManager.bombsRemaining = replayData->bombsRemaining;
    g_GameManager.currentPower = replayData->power;
    g_GameManager.livesRemaining2 = replayData->livesRemaining2;
    g_GameManager.bombsRemaining2 = replayData->bombsRemaining2;
    g_GameManager.currentPower2 = replayData->power2;
    mgr->replayInputs = replayData->replayInputs;
    mgr->replayHeldFrames = 0;
    mgr->replayTraceFlags = 0;
    g_GameManager.powerItemCountForScore = replayData->powerItemCountForScore;
    LogReplayBoot(mgr, replayData);
    if (2 <= g_GameManager.currentStage && mgr->replayData->stageReplayData[g_GameManager.currentStage - 2] != NULL)
    {
        g_GameManager.guiScore = mgr->replayData->stageReplayData[g_GameManager.currentStage - 2]->score;
        g_GameManager.score = g_GameManager.guiScore;
    }
    return ZUN_SUCCESS;
}

ZunResult ReplayManager::DeletedCallback(ReplayManager *mgr)
{
    g_Chain.Cut(mgr->drawChain);
    mgr->drawChain = NULL;
    if (mgr->calcChainDemoHighPrio != NULL)
    {
        g_Chain.Cut(mgr->calcChainDemoHighPrio);
        mgr->calcChainDemoHighPrio = NULL;
    }
    ReleaseReplayData(g_ReplayManager->replayData);
    delete g_ReplayManager;
    g_ReplayManager = NULL;
    g_ReplayManager = NULL;
    return ZUN_SUCCESS;
}

void ReplayManager::StopRecording()
{
    ReplayManager *mgr = g_ReplayManager;
    if (mgr != NULL)
    {
        mgr->replayInputs += 1;
        mgr->replayInputs->frameNum = mgr->frameId;
        mgr->replayInputs->inputKey = 0;
        mgr->replayInputs += 1;
        mgr->replayInputs->frameNum = 9999999;
        mgr->replayInputs->inputKey = 0;
        mgr->replayInputStageBookmarks[g_GameManager.currentStage - 1] = mgr->replayInputs + 1;
    }
}

#pragma var_order(stageIdx, mgr, slowDown, diskHdr, stageReplayPos, file, csumStagePos, checksum, checksumCursor,    \
                  obfOffset, obfStagePos, obfuscateCursor)
void ReplayManager::SaveReplay(char *replayPath, char *replayName)
{
    ReplayManager *mgr;
    FILE *file;
    u8 *checksumCursor;
    OnDiskReplayData diskHdr;
    u8 *obfuscateCursor;
    i32 obfStagePos;
    u8 obfOffset;
    u32 checksum;
    i32 csumStagePos;
    size_t stageReplayPos;
    f32 slowDown;
    i32 stageIdx;

    if (g_ReplayManager != NULL)
    {
        mgr = g_ReplayManager;
        if (!mgr->IsDemo())
        {
            if (replayPath != NULL)
            {
                // Build the on-disk header from the in-memory ReplayData.
                // This produces a byte-identical layout to what 32-bit TH06
                // builds wrote: 84-byte header, u32 stage offsets, no
                // architecture-dependent fields.
                memset(&diskHdr, 0, sizeof(diskHdr));
                memcpy(diskHdr.magic, mgr->replayData->magic, sizeof(diskHdr.magic));
                diskHdr.version = mgr->replayData->version;
                diskHdr.shottypeChara = mgr->replayData->shottypeChara;
                diskHdr.shottypeChara2 = mgr->replayData->shottypeChara2;
                diskHdr.difficulty = mgr->replayData->difficulty;
                diskHdr.rngValue1 = mgr->replayData->rngValue1;
                diskHdr.rngValue2 = mgr->replayData->rngValue2;
                diskHdr.key = mgr->replayData->key;
                diskHdr.rngValue3 = mgr->replayData->rngValue3;
                memcpy(diskHdr.date, mgr->replayData->date, sizeof(diskHdr.date));
                memcpy(diskHdr.name, mgr->replayData->name, sizeof(diskHdr.name));
                diskHdr.score = mgr->replayData->score;
                diskHdr.slowdownRate2 = mgr->replayData->slowdownRate2;
                diskHdr.slowdownRate = mgr->replayData->slowdownRate;
                diskHdr.slowdownRate3 = mgr->replayData->slowdownRate3;

                ReplayManager::StopRecording();
                stageReplayPos = sizeof(OnDiskReplayData);
                for (stageIdx = 0; stageIdx < ARRAY_SIZE_SIGNED(g_ReplayManager->replayData->stageReplayData);
                     stageIdx += 1)
                {
                    if (mgr->replayData->stageReplayData[stageIdx] != NULL)
                    {
                        diskHdr.stageOffset[stageIdx] = (u32)stageReplayPos;
                        stageReplayPos += (size_t)mgr->replayInputStageBookmarks[stageIdx] -
                                          (size_t)mgr->replayData->stageReplayData[stageIdx];
                    }
                }
                utils::DebugPrint2("%s write ...\n", replayPath);
                diskHdr.score = g_GameManager.guiScore;
                slowDown = (g_Supervisor.unk1b4 / g_Supervisor.unk1b8 - 0.5f) * 2.0f;
                if (slowDown < 0.0f)
                {
                    slowDown = 0.0f;
                }
                else if (slowDown >= 1.0f)
                {
                    slowDown = 1.0f;
                }
                diskHdr.slowdownRate = (1.0f - slowDown) * 100.0f;
                diskHdr.slowdownRate2 = diskHdr.slowdownRate + 1.12f;
                diskHdr.slowdownRate3 = diskHdr.slowdownRate + 2.34f;
                mgr->replayData->stageReplayData[g_GameManager.currentStage - 1]->score = g_GameManager.score;
                utils::CopyStringToFixedField(diskHdr.name, sizeof(diskHdr.name), replayName);
#ifdef _WIN32
                _strdate(diskHdr.date);
#else
                {
                    time_t t = time(NULL);
                    struct tm *tm = localtime(&t);
                    strftime(diskHdr.date, sizeof(diskHdr.date), "%m/%d/%y", tm);
                }
#endif
                diskHdr.key = g_Rng.GetRandomU16InRange(128) + 64;
                diskHdr.rngValue3 = g_Rng.GetRandomU16InRange(256);
                diskHdr.rngValue1 = g_Rng.GetRandomU16InRange(256);
                diskHdr.rngValue2 = g_Rng.GetRandomU16InRange(256);

                // Calculate the checksum (over header from `key` onward + every stage payload).
                checksumCursor = (u8 *)&diskHdr.key;
                checksum = 0x3f000318;
                for (stageIdx = 0; stageIdx < (i32)(sizeof(OnDiskReplayData) - offsetof(OnDiskReplayData, key));
                     stageIdx += 1, checksumCursor += 1)
                {
                    checksum += *checksumCursor;
                }
                for (stageIdx = 0; stageIdx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData); stageIdx += 1)
                {
                    if (mgr->replayData->stageReplayData[stageIdx] != NULL)
                    {
                        checksumCursor = (u8 *)mgr->replayData->stageReplayData[stageIdx];
                        for (csumStagePos = 0; csumStagePos < (u8 *)mgr->replayInputStageBookmarks[stageIdx] -
                                                                  (u8 *)mgr->replayData->stageReplayData[stageIdx];
                             csumStagePos += 1, checksumCursor += 1)
                        {
                            checksum += *checksumCursor;
                        }
                    }
                }
                diskHdr.checksum = (i32)checksum;

                // Obfuscate the data (over header from `rngValue3` onward + every stage payload).
                obfuscateCursor = (u8 *)&diskHdr.rngValue3;
                obfOffset = diskHdr.key;
                for (stageIdx = 0; stageIdx < (i32)(sizeof(OnDiskReplayData) - offsetof(OnDiskReplayData, rngValue3));
                     stageIdx += 1, obfuscateCursor += 1)
                {
                    *obfuscateCursor += obfOffset;
                    obfOffset += 7;
                }
                for (stageIdx = 0; stageIdx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData); stageIdx += 1)
                {
                    if (mgr->replayData->stageReplayData[stageIdx] != NULL)
                    {
                        obfuscateCursor = (u8 *)mgr->replayData->stageReplayData[stageIdx];
                        for (obfStagePos = 0; obfStagePos < (u8 *)mgr->replayInputStageBookmarks[stageIdx] -
                                                                (u8 *)mgr->replayData->stageReplayData[stageIdx];
                             obfStagePos += 1, obfuscateCursor += 1)
                        {
                            *obfuscateCursor += obfOffset;
                            obfOffset += 7;
                        }
                    }
                }

                // Write the data to the replay file.
                char resolvedReplayPath[512];
                GamePaths::Resolve(resolvedReplayPath, sizeof(resolvedReplayPath), replayPath);
                GamePaths::EnsureParentDir(resolvedReplayPath);
                file = fopen(resolvedReplayPath, "wb");
                fwrite(&diskHdr, sizeof(OnDiskReplayData), 1, file);
                for (stageIdx = 0; stageIdx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData); stageIdx += 1)
                {
                    if (mgr->replayData->stageReplayData[stageIdx] != NULL)
                    {
                        fwrite(mgr->replayData->stageReplayData[stageIdx], 1,
                               (u8 *)mgr->replayInputStageBookmarks[stageIdx] -
                                   (u8 *)mgr->replayData->stageReplayData[stageIdx],
                               file);
                    }
                }
                fclose(file);
            }
            for (stageIdx = 0; stageIdx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData); stageIdx += 1)
            {
                if (g_ReplayManager->replayData->stageReplayData[stageIdx] != NULL)
                {
                    utils::DebugPrint2("Replay Size %d\n", (int)((u8 *)mgr->replayInputStageBookmarks[stageIdx] -
                                                               (u8 *)mgr->replayData->stageReplayData[stageIdx]));
                    ReleaseStageReplayData(g_ReplayManager->replayData->stageReplayData[stageIdx]);
                }
            }
        }
        g_Chain.Cut(g_ReplayManager->calcChain);
    }
    return;
}
}; // namespace th06
