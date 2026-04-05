#include "NetplayAuthoritativeTransport.hpp"

namespace th06::Netplay
{
namespace
{
constexpr u8 kAuthoritativeStateDatagramMagic[4] = {'T', 'A', 'U', 'T'};
constexpr u8 kAuthoritativeStateDatagramVersion = 1;
constexpr u8 kAuthoritativeExpectedActorMask = 0x0F;

void FillPrefix(AuthoritativeDatagramPrefix &prefix, AuthoritativeStateDatagramKind kind)
{
    std::memcpy(prefix.magic, kAuthoritativeStateDatagramMagic, sizeof(kAuthoritativeStateDatagramMagic));
    prefix.version = kAuthoritativeStateDatagramVersion;
    prefix.kind = static_cast<u8>(kind);
    prefix.reserved = 0;
}

template <typename TEntry, typename TDatagram>
bool SendActorChunks(const AuthoritativeReplicator::ReplicatedWorldState &state, const TEntry *entries, int entryCount,
                     AuthoritativeStateDatagramKind kind, int chunkCapacity, const char *label)
{
    if (entryCount <= 0)
    {
        return true;
    }

    const int chunkCount = (entryCount + chunkCapacity - 1) / chunkCapacity;
    for (int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
    {
        TDatagram datagram {};
        FillPrefix(datagram.header.prefix, kind);
        datagram.header.serverFrame = state.serverFrame;
        datagram.header.chunkIndex = (u16)chunkIndex;
        datagram.header.chunkCount = (u16)chunkCount;
        const int start = chunkIndex * chunkCapacity;
        const int count = std::min(chunkCapacity, entryCount - start);
        datagram.header.entryCount = (u16)count;
        if (count > 0)
        {
            std::memcpy(datagram.entries, entries + start, sizeof(TEntry) * (size_t)count);
        }

        const size_t bytes = sizeof(datagram.header) + sizeof(TEntry) * (size_t)count;
        if (!SendDatagramImmediate(&datagram, bytes))
        {
            TraceDiagnostic("authoritative-state-send-chunk-fail",
                            "frame=%d type=%s chunk=%d/%d count=%d", state.serverFrame, label, chunkIndex,
                            chunkCount, count);
            return false;
        }
    }
    return true;
}

bool ValidatePrefix(const AuthoritativeDatagramPrefix &prefix)
{
    return std::memcmp(prefix.magic, kAuthoritativeStateDatagramMagic, sizeof(kAuthoritativeStateDatagramMagic)) == 0 &&
           prefix.version == kAuthoritativeStateDatagramVersion;
}

bool CanConsumeAuthoritativeDatagram()
{
    return g_State.isSessionActive && g_State.authoritativeModeEnabled && !g_State.isHost;
}

bool BeginChunkApply(int serverFrame, u8 actorBit, const char *label, int chunkIndex, int chunkCount)
{
    if (!CanConsumeAuthoritativeDatagram() || !g_State.latestAuthoritativeFrameState.valid)
    {
        return false;
    }

    if (serverFrame < g_State.latestAuthoritativeFrameState.serverFrame)
    {
        TraceDiagnostic("authoritative-state-drop-stale-chunk", "type=%s frame=%d latest=%d", label, serverFrame,
                        g_State.latestAuthoritativeFrameState.serverFrame);
        return false;
    }
    if (serverFrame != g_State.latestAuthoritativeFrameState.serverFrame)
    {
        TraceDiagnostic("authoritative-state-drop-orphan-chunk", "type=%s frame=%d latest=%d", label, serverFrame,
                        g_State.latestAuthoritativeFrameState.serverFrame);
        return false;
    }
    if (chunkIndex == 0)
    {
        g_State.latestAuthoritativeFrameState.receivedActorMask &= (u8)~actorBit;
    }
    if (chunkIndex + 1 >= chunkCount)
    {
        g_State.latestAuthoritativeFrameState.receivedActorMask |= actorBit;
        g_State.authoritativeHashCheckEnabled = (g_State.latestAuthoritativeFrameState.receivedActorMask &
                                                g_State.latestAuthoritativeFrameState.expectedActorMask) ==
                                               g_State.latestAuthoritativeFrameState.expectedActorMask;
    }
    return true;
}

template <typename TEntry>
void AppendEntries(TEntry *dest, u16 &destCount, int destMax, const TEntry *src, int entryCount)
{
    if (entryCount <= 0)
    {
        return;
    }

    const int writable = std::min(entryCount, destMax - (int)destCount);
    if (writable <= 0)
    {
        return;
    }

    std::memcpy(dest + destCount, src, sizeof(TEntry) * (size_t)writable);
    destCount = (u16)(destCount + writable);
}
} // namespace

bool SendAuthoritativeFrameStateDatagram(const AuthoritativeReplicator::ReplicatedWorldState &state)
{
    if (!g_State.isSessionActive || !g_State.isConnected || !g_State.isHost || !g_State.authoritativeModeEnabled)
    {
        return false;
    }

    AuthoritativeStateDatagramHeader header {};
    FillPrefix(header.prefix, AuthoritativeStateDatagram_Frame);
    header.serverFrame = state.serverFrame;
    header.ackedInputFrameP1 = state.ackedInputFrameP1;
    header.ackedInputFrameP2 = state.ackedInputFrameP2;
    header.worldHash = state.worldHash;
    header.flags = state.flags;
    header.bgmCue = state.bgmCue;
    header.player1 = state.player1;
    header.player2 = state.player2;
    header.hud = state.hud;
    header.ui = state.ui;
    header.bulletCount = state.bulletCount;
    header.laserCount = state.laserCount;
    header.enemyCount = state.enemyCount;
    header.itemCount = state.itemCount;
    std::memcpy(header.bgmPath, state.bgmPath, sizeof(header.bgmPath));
    std::memcpy(header.posPath, state.posPath, sizeof(header.posPath));
    header.bgmIsLooping = state.bgmIsLooping;

    if (!SendDatagramImmediate(&header, sizeof(header)))
    {
        return false;
    }

    const bool bulletsOk = SendActorChunks<ReplicatedBulletState, AuthoritativeBulletChunkDatagram>(
        state, state.bullets, state.bulletCount, AuthoritativeStateDatagram_BulletChunk,
        kAuthoritativeBulletChunkCapacity, "bullet");
    const bool lasersOk = SendActorChunks<ReplicatedLaserState, AuthoritativeLaserChunkDatagram>(
        state, state.lasers, state.laserCount, AuthoritativeStateDatagram_LaserChunk, kAuthoritativeLaserChunkCapacity,
        "laser");
    const bool enemiesOk = SendActorChunks<ReplicatedEnemyState, AuthoritativeEnemyChunkDatagram>(
        state, state.enemies, state.enemyCount, AuthoritativeStateDatagram_EnemyChunk, kAuthoritativeEnemyChunkCapacity,
        "enemy");
    const bool itemsOk = SendActorChunks<ReplicatedItemState, AuthoritativeItemChunkDatagram>(
        state, state.items, state.itemCount, AuthoritativeStateDatagram_ItemChunk, kAuthoritativeItemChunkCapacity,
        "item");

    TraceDiagnostic("authoritative-state-send",
                    "frame=%d ackP1=%d ackP2=%d hash=%llu flags=%u bullets=%u lasers=%u enemies=%u items=%u",
                    state.serverFrame, state.ackedInputFrameP1, state.ackedInputFrameP2,
                    (unsigned long long)state.worldHash, state.flags, state.bulletCount, state.laserCount,
                    state.enemyCount, state.itemCount);
    return bulletsOk && lasersOk && enemiesOk && itemsOk;
}

bool HandleAuthoritativeStateDatagram(const AuthoritativeStateDatagramHeader &header)
{
    if (!CanConsumeAuthoritativeDatagram() || !ValidatePrefix(header.prefix) ||
        header.prefix.kind != static_cast<u8>(AuthoritativeStateDatagram_Frame))
    {
        return false;
    }

    g_State.lastRuntimeReceiveTick = SDL_GetTicks64();
    g_State.latestAuthoritativeFrameState = {};
    g_State.latestAuthoritativeFrameState.valid = true;
    g_State.latestAuthoritativeFrameState.serverFrame = header.serverFrame;
    g_State.latestAuthoritativeFrameState.ackedInputFrameP1 = header.ackedInputFrameP1;
    g_State.latestAuthoritativeFrameState.ackedInputFrameP2 = header.ackedInputFrameP2;
    g_State.latestAuthoritativeFrameState.worldHash = header.worldHash;
    g_State.latestAuthoritativeFrameState.flags = header.flags;
    g_State.latestAuthoritativeFrameState.bgmCue = header.bgmCue;
    g_State.latestAuthoritativeFrameState.player1 = header.player1;
    g_State.latestAuthoritativeFrameState.player2 = header.player2;
    g_State.latestAuthoritativeFrameState.hud = header.hud;
    g_State.latestAuthoritativeFrameState.ui = header.ui;
    g_State.latestAuthoritativeFrameState.bulletCount = 0;
    g_State.latestAuthoritativeFrameState.laserCount = 0;
    g_State.latestAuthoritativeFrameState.enemyCount = 0;
    g_State.latestAuthoritativeFrameState.itemCount = 0;
    std::memcpy(g_State.latestAuthoritativeFrameState.bgmPath, header.bgmPath,
                sizeof(g_State.latestAuthoritativeFrameState.bgmPath));
    std::memcpy(g_State.latestAuthoritativeFrameState.posPath, header.posPath,
                sizeof(g_State.latestAuthoritativeFrameState.posPath));
    g_State.latestAuthoritativeFrameState.bgmIsLooping = header.bgmIsLooping;
    g_State.latestAuthoritativeFrameState.receivedActorMask = 0;
    g_State.latestAuthoritativeFrameState.expectedActorMask = kAuthoritativeExpectedActorMask;
    if (header.bulletCount == 0)
    {
        g_State.latestAuthoritativeFrameState.receivedActorMask |= 1 << 0;
    }
    if (header.laserCount == 0)
    {
        g_State.latestAuthoritativeFrameState.receivedActorMask |= 1 << 1;
    }
    if (header.enemyCount == 0)
    {
        g_State.latestAuthoritativeFrameState.receivedActorMask |= 1 << 2;
    }
    if (header.itemCount == 0)
    {
        g_State.latestAuthoritativeFrameState.receivedActorMask |= 1 << 3;
    }
    g_State.authoritativeHashCheckEnabled = (g_State.latestAuthoritativeFrameState.receivedActorMask &
                                            g_State.latestAuthoritativeFrameState.expectedActorMask) ==
                                           g_State.latestAuthoritativeFrameState.expectedActorMask;
    TraceDiagnostic("authoritative-state-recv",
                    "frame=%d ackP1=%d ackP2=%d hash=%llu flags=%u bullets=%u lasers=%u enemies=%u items=%u",
                    header.serverFrame, header.ackedInputFrameP1, header.ackedInputFrameP2,
                    (unsigned long long)header.worldHash, header.flags, header.bulletCount, header.laserCount,
                    header.enemyCount, header.itemCount);
    return true;
}

bool HandleAuthoritativeStateDatagramBytes(const void *bytes, int size)
{
    if (bytes == nullptr || size < (int)sizeof(AuthoritativeDatagramPrefix))
    {
        return false;
    }

    const AuthoritativeDatagramPrefix *prefix = (const AuthoritativeDatagramPrefix *)bytes;
    if (!ValidatePrefix(*prefix))
    {
        return false;
    }

    switch ((AuthoritativeStateDatagramKind)prefix->kind)
    {
    case AuthoritativeStateDatagram_Frame:
        if (size < (int)sizeof(AuthoritativeStateDatagramHeader))
        {
            return false;
        }
        return HandleAuthoritativeStateDatagram(*(const AuthoritativeStateDatagramHeader *)bytes);

    case AuthoritativeStateDatagram_BulletChunk:
    {
        if (size < (int)sizeof(AuthoritativeActorChunkHeader))
        {
            return false;
        }
        const AuthoritativeBulletChunkDatagram *chunk = (const AuthoritativeBulletChunkDatagram *)bytes;
        const int expectedBytes = (int)(sizeof(chunk->header) + sizeof(chunk->entries[0]) * chunk->header.entryCount);
        if (chunk->header.entryCount > kAuthoritativeBulletChunkCapacity)
        {
            return false;
        }
        if (size < expectedBytes ||
            !BeginChunkApply(chunk->header.serverFrame, 1 << 0, "bullet", chunk->header.chunkIndex,
                             chunk->header.chunkCount))
        {
            return false;
        }
        if (chunk->header.chunkIndex == 0)
        {
            g_State.latestAuthoritativeFrameState.bulletCount = 0;
        }
        AppendEntries(g_State.latestAuthoritativeFrameState.bullets, g_State.latestAuthoritativeFrameState.bulletCount,
                      kAuthoritativeMaxReplicatedBullets, chunk->entries, chunk->header.entryCount);
        return true;
    }

    case AuthoritativeStateDatagram_LaserChunk:
    {
        if (size < (int)sizeof(AuthoritativeActorChunkHeader))
        {
            return false;
        }
        const AuthoritativeLaserChunkDatagram *chunk = (const AuthoritativeLaserChunkDatagram *)bytes;
        const int expectedBytes = (int)(sizeof(chunk->header) + sizeof(chunk->entries[0]) * chunk->header.entryCount);
        if (chunk->header.entryCount > kAuthoritativeLaserChunkCapacity)
        {
            return false;
        }
        if (size < expectedBytes ||
            !BeginChunkApply(chunk->header.serverFrame, 1 << 1, "laser", chunk->header.chunkIndex,
                             chunk->header.chunkCount))
        {
            return false;
        }
        if (chunk->header.chunkIndex == 0)
        {
            g_State.latestAuthoritativeFrameState.laserCount = 0;
        }
        AppendEntries(g_State.latestAuthoritativeFrameState.lasers, g_State.latestAuthoritativeFrameState.laserCount,
                      kAuthoritativeMaxReplicatedLasers, chunk->entries, chunk->header.entryCount);
        return true;
    }

    case AuthoritativeStateDatagram_EnemyChunk:
    {
        if (size < (int)sizeof(AuthoritativeActorChunkHeader))
        {
            return false;
        }
        const AuthoritativeEnemyChunkDatagram *chunk = (const AuthoritativeEnemyChunkDatagram *)bytes;
        const int expectedBytes = (int)(sizeof(chunk->header) + sizeof(chunk->entries[0]) * chunk->header.entryCount);
        if (chunk->header.entryCount > kAuthoritativeEnemyChunkCapacity)
        {
            return false;
        }
        if (size < expectedBytes ||
            !BeginChunkApply(chunk->header.serverFrame, 1 << 2, "enemy", chunk->header.chunkIndex,
                             chunk->header.chunkCount))
        {
            return false;
        }
        if (chunk->header.chunkIndex == 0)
        {
            g_State.latestAuthoritativeFrameState.enemyCount = 0;
        }
        AppendEntries(g_State.latestAuthoritativeFrameState.enemies, g_State.latestAuthoritativeFrameState.enemyCount,
                      kAuthoritativeMaxReplicatedEnemies, chunk->entries, chunk->header.entryCount);
        return true;
    }

    case AuthoritativeStateDatagram_ItemChunk:
    {
        if (size < (int)sizeof(AuthoritativeActorChunkHeader))
        {
            return false;
        }
        const AuthoritativeItemChunkDatagram *chunk = (const AuthoritativeItemChunkDatagram *)bytes;
        const int expectedBytes = (int)(sizeof(chunk->header) + sizeof(chunk->entries[0]) * chunk->header.entryCount);
        if (chunk->header.entryCount > kAuthoritativeItemChunkCapacity)
        {
            return false;
        }
        if (size < expectedBytes ||
            !BeginChunkApply(chunk->header.serverFrame, 1 << 3, "item", chunk->header.chunkIndex,
                             chunk->header.chunkCount))
        {
            return false;
        }
        if (chunk->header.chunkIndex == 0)
        {
            g_State.latestAuthoritativeFrameState.itemCount = 0;
        }
        AppendEntries(g_State.latestAuthoritativeFrameState.items, g_State.latestAuthoritativeFrameState.itemCount,
                      kAuthoritativeMaxReplicatedItems, chunk->entries, chunk->header.entryCount);
        return true;
    }

    default:
        return false;
    }
}
} // namespace th06::Netplay
