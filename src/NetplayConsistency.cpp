#include "NetplayInternal.hpp"

namespace th06::Netplay
{
namespace
{
constexpr u8 kConsistencyDatagramMagic[4] = {'T', 'D', 'H', 'G'};
constexpr u8 kConsistencyDatagramVersion = 1;
constexpr int kConsistencyLiteCadence = 30;
constexpr std::size_t kConsistencySampleCacheSize = 32;

uint64_t MakeConsistencyKey(int stage, int frame, int rollbackEpochStartFrame)
{
    uint64_t hash = 1469598103934665603ull;
    const uint32_t values[3] = {(uint32_t)stage, (uint32_t)frame, (uint32_t)rollbackEpochStartFrame};
    for (uint32_t value : values)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool IsConsistencyEnabled()
{
    return THPrac::TH06::THPracIsDebugLogEnabled() && Session::IsRemoteNetplaySession() && g_State.isSessionActive;
}

bool IsConsistencySampleEligible(const FrameSubsystemHashes &hashes)
{
    if (!IsConsistencyEnabled() || !g_State.isConnected || g_State.isTryingReconnect || IsAuthoritativeRecoveryActive())
    {
        return false;
    }
    if (hashes.stage < 0 || hashes.frame < 0)
    {
        return false;
    }
    return !IsCurrentUiFrame();
}

const FrameSubsystemHashes *FindConsistencySample(const std::deque<FrameSubsystemHashes> &samples, int stage, int frame,
                                                  int rollbackEpochStartFrame)
{
    for (const FrameSubsystemHashes &sample : samples)
    {
        if (sample.stage == stage && sample.frame == frame && sample.rollbackEpochStartFrame == rollbackEpochStartFrame)
        {
            return &sample;
        }
    }
    return nullptr;
}

FrameSubsystemHashes *FindConsistencySample(std::deque<FrameSubsystemHashes> &samples, int stage, int frame,
                                            int rollbackEpochStartFrame)
{
    for (FrameSubsystemHashes &sample : samples)
    {
        if (sample.stage == stage && sample.frame == frame && sample.rollbackEpochStartFrame == rollbackEpochStartFrame)
        {
            return &sample;
        }
    }
    return nullptr;
}

bool UpsertConsistencySample(const FrameSubsystemHashes &hashes)
{
    FrameSubsystemHashes *existing =
        FindConsistencySample(g_State.consistencySamples, hashes.stage, hashes.frame, hashes.rollbackEpochStartFrame);
    if (existing != nullptr)
    {
        *existing = hashes;
        return true;
    }

    if (g_State.consistencySamples.size() >= kConsistencySampleCacheSize)
    {
        const FrameSubsystemHashes &evicted = g_State.consistencySamples.front();
        g_State.consistencyDetailRequestedKeys.erase(
            MakeConsistencyKey(evicted.stage, evicted.frame, evicted.rollbackEpochStartFrame));
        g_State.consistencySamples.pop_front();
    }

    g_State.consistencySamples.push_back(hashes);
    return false;
}

bool SendConsistencyDatagram(ConsistencyDatagramKind kind, int stage, int frame, int rollbackEpochStartFrame,
                             const void *payload, std::size_t payloadBytes)
{
    if (!IsConsistencyEnabled())
    {
        return false;
    }

    u8 buffer[kRelayMaxDatagramBytes] = {};
    if (sizeof(ConsistencyDatagramHeader) + payloadBytes > sizeof(buffer))
    {
        return false;
    }

    ConsistencyDatagramHeader header {};
    std::memcpy(header.magic, kConsistencyDatagramMagic, sizeof(header.magic));
    header.version = kConsistencyDatagramVersion;
    header.kind = (u8)kind;
    header.reserved = 0;
    header.stage = stage;
    header.frame = frame;
    header.rollbackEpochStartFrame = rollbackEpochStartFrame;

    std::memcpy(buffer, &header, sizeof(header));
    if (payload != nullptr && payloadBytes > 0)
    {
        std::memcpy(buffer + sizeof(header), payload, payloadBytes);
    }
    return SendDatagramImmediate(buffer, sizeof(header) + payloadBytes);
}

const char *FirstDifferentSubsystem(const FrameSubsystemHashes &local, const FrameSubsystemHashes &remote)
{
    if (local.gameHash != remote.gameHash)
    {
        return "game";
    }
    if (local.player1Hash != remote.player1Hash)
    {
        return "p1";
    }
    if (local.player2Hash != remote.player2Hash)
    {
        return "p2";
    }
    if (local.bulletHash != remote.bulletHash)
    {
        return "bullet";
    }
    if (local.enemyHash != remote.enemyHash)
    {
        return "enemy";
    }
    if (local.itemHash != remote.itemHash)
    {
        return "item";
    }
    if (local.effectHash != remote.effectHash)
    {
        return "effect";
    }
    if (local.stageHash != remote.stageHash)
    {
        return "stage";
    }
    if (local.eclHash != remote.eclHash)
    {
        return "ecl";
    }
    if (local.enemyEclHash != remote.enemyEclHash)
    {
        return "enemyEcl";
    }
    if (local.screenHash != remote.screenHash)
    {
        return "screen";
    }
    if (local.inputHash != remote.inputHash)
    {
        return "input";
    }
    if (local.catkHash != remote.catkHash)
    {
        return "catk";
    }
    if (local.rngHash != remote.rngHash)
    {
        return "rng";
    }
    if (local.allHash != remote.allHash)
    {
        return "all";
    }
    return "none";
}
} // namespace

void ResetConsistencyDebugState()
{
    g_State.consistencySamples.clear();
    g_State.consistencyDetailRequestedKeys.clear();
}

void RecordConsistencyHashSample(const FrameSubsystemHashes &hashes)
{
    if (!IsConsistencySampleEligible(hashes) || hashes.frame % kConsistencyLiteCadence != 0)
    {
        return;
    }

    const bool existed = UpsertConsistencySample(hashes);
    if (!g_State.isHost || existed)
    {
        return;
    }

    ConsistencyLiteHashPayload payload {};
    payload.allHash = hashes.allHash;
    payload.rngHash = hashes.rngHash;
    SendConsistencyDatagram(ConsistencyDatagram_LiteHash, hashes.stage, hashes.frame, hashes.rollbackEpochStartFrame,
                            &payload, sizeof(payload));
}

bool HandleConsistencySidebandDatagram(const u8 *bytes, int size)
{
    if (!IsConsistencyEnabled() || bytes == nullptr || size < (int)sizeof(ConsistencyDatagramHeader))
    {
        return false;
    }

    const ConsistencyDatagramHeader *header = (const ConsistencyDatagramHeader *)bytes;
    if (std::memcmp(header->magic, kConsistencyDatagramMagic, sizeof(header->magic)) != 0 ||
        header->version != kConsistencyDatagramVersion)
    {
        return false;
    }

    const int payloadBytes = size - (int)sizeof(ConsistencyDatagramHeader);
    const u8 *payload = bytes + sizeof(ConsistencyDatagramHeader);
    const int stage = header->stage;
    const int frame = header->frame;
    const int rollbackEpochStartFrame = header->rollbackEpochStartFrame;
    const uint64_t key = MakeConsistencyKey(stage, frame, rollbackEpochStartFrame);

    switch ((ConsistencyDatagramKind)header->kind)
    {
    case ConsistencyDatagram_LiteHash:
    {
        if (g_State.isHost || payloadBytes != (int)sizeof(ConsistencyLiteHashPayload))
        {
            return true;
        }

        const FrameSubsystemHashes *local =
            FindConsistencySample(g_State.consistencySamples, stage, frame, rollbackEpochStartFrame);
        if (local == nullptr)
        {
            TraceDiagnostic("consistency-miss-cache", "kind=lite stage=%d frame=%d epoch=%d", stage, frame,
                            rollbackEpochStartFrame);
            return true;
        }

        const ConsistencyLiteHashPayload *remote = (const ConsistencyLiteHashPayload *)payload;
        if (local->allHash == remote->allHash && local->rngHash == remote->rngHash)
        {
            return true;
        }

        TraceDiagnostic("consistency-mismatch-lite",
                        "stage=%d frame=%d epoch=%d remoteAll=%016llx remoteRng=%016llx localAll=%016llx localRng=%016llx",
                        stage, frame, rollbackEpochStartFrame, (unsigned long long)remote->allHash,
                        (unsigned long long)remote->rngHash, (unsigned long long)local->allHash,
                        (unsigned long long)local->rngHash);

        if (g_State.consistencyDetailRequestedKeys.insert(key).second)
        {
            SendConsistencyDatagram(ConsistencyDatagram_DetailRequest, stage, frame, rollbackEpochStartFrame, nullptr,
                                    0);
        }
        return true;
    }

    case ConsistencyDatagram_DetailRequest:
    {
        if (!g_State.isHost || payloadBytes != 0)
        {
            return true;
        }

        const FrameSubsystemHashes *local =
            FindConsistencySample(g_State.consistencySamples, stage, frame, rollbackEpochStartFrame);
        if (local == nullptr)
        {
            TraceDiagnostic("consistency-detail-miss-host-cache", "stage=%d frame=%d epoch=%d", stage, frame,
                            rollbackEpochStartFrame);
            return true;
        }

        ConsistencyDetailHashPayload detail {};
        detail.allHash = local->allHash;
        detail.gameHash = local->gameHash;
        detail.player1Hash = local->player1Hash;
        detail.player2Hash = local->player2Hash;
        detail.bulletHash = local->bulletHash;
        detail.enemyHash = local->enemyHash;
        detail.itemHash = local->itemHash;
        detail.effectHash = local->effectHash;
        detail.stageHash = local->stageHash;
        detail.eclHash = local->eclHash;
        detail.enemyEclHash = local->enemyEclHash;
        detail.screenHash = local->screenHash;
        detail.inputHash = local->inputHash;
        detail.catkHash = local->catkHash;
        detail.rngHash = local->rngHash;
        SendConsistencyDatagram(ConsistencyDatagram_DetailHash, stage, frame, rollbackEpochStartFrame, &detail,
                                sizeof(detail));
        return true;
    }

    case ConsistencyDatagram_DetailHash:
    {
        if (g_State.isHost || payloadBytes != (int)sizeof(ConsistencyDetailHashPayload))
        {
            return true;
        }

        const FrameSubsystemHashes *local =
            FindConsistencySample(g_State.consistencySamples, stage, frame, rollbackEpochStartFrame);
        if (local == nullptr)
        {
            TraceDiagnostic("consistency-miss-cache", "kind=detail stage=%d frame=%d epoch=%d", stage, frame,
                            rollbackEpochStartFrame);
            return true;
        }

        const ConsistencyDetailHashPayload *remotePayload = (const ConsistencyDetailHashPayload *)payload;
        FrameSubsystemHashes remote {};
        remote.stage = stage;
        remote.frame = frame;
        remote.rollbackEpochStartFrame = rollbackEpochStartFrame;
        remote.allHash = remotePayload->allHash;
        remote.gameHash = remotePayload->gameHash;
        remote.player1Hash = remotePayload->player1Hash;
        remote.player2Hash = remotePayload->player2Hash;
        remote.bulletHash = remotePayload->bulletHash;
        remote.enemyHash = remotePayload->enemyHash;
        remote.itemHash = remotePayload->itemHash;
        remote.effectHash = remotePayload->effectHash;
        remote.stageHash = remotePayload->stageHash;
        remote.eclHash = remotePayload->eclHash;
        remote.enemyEclHash = remotePayload->enemyEclHash;
        remote.screenHash = remotePayload->screenHash;
        remote.inputHash = remotePayload->inputHash;
        remote.catkHash = remotePayload->catkHash;
        remote.rngHash = remotePayload->rngHash;

        TraceDiagnostic(
            "consistency-mismatch-detail",
            "stage=%d frame=%d epoch=%d firstDiff=%s localAll=%016llx remoteAll=%016llx game=%016llx/%016llx p1=%016llx/%016llx p2=%016llx/%016llx bullet=%016llx/%016llx enemy=%016llx/%016llx item=%016llx/%016llx effect=%016llx/%016llx stageHash=%016llx/%016llx ecl=%016llx/%016llx enemyEcl=%016llx/%016llx screen=%016llx/%016llx input=%016llx/%016llx catk=%016llx/%016llx rng=%016llx/%016llx",
            stage, frame, rollbackEpochStartFrame, FirstDifferentSubsystem(*local, remote),
            (unsigned long long)local->allHash, (unsigned long long)remote.allHash,
            (unsigned long long)local->gameHash, (unsigned long long)remote.gameHash,
            (unsigned long long)local->player1Hash, (unsigned long long)remote.player1Hash,
            (unsigned long long)local->player2Hash, (unsigned long long)remote.player2Hash,
            (unsigned long long)local->bulletHash, (unsigned long long)remote.bulletHash,
            (unsigned long long)local->enemyHash, (unsigned long long)remote.enemyHash,
            (unsigned long long)local->itemHash, (unsigned long long)remote.itemHash,
            (unsigned long long)local->effectHash, (unsigned long long)remote.effectHash,
            (unsigned long long)local->stageHash, (unsigned long long)remote.stageHash,
            (unsigned long long)local->eclHash, (unsigned long long)remote.eclHash,
            (unsigned long long)local->enemyEclHash, (unsigned long long)remote.enemyEclHash,
            (unsigned long long)local->screenHash, (unsigned long long)remote.screenHash,
            (unsigned long long)local->inputHash, (unsigned long long)remote.inputHash,
            (unsigned long long)local->catkHash, (unsigned long long)remote.catkHash,
            (unsigned long long)local->rngHash, (unsigned long long)remote.rngHash);
        return true;
    }

    default:
        break;
    }

    return false;
}
} // namespace th06::Netplay
