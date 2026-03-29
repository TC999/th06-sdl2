#include "NetplayInternal.hpp"

#include "PortableGameplayRestore.hpp"
#include "PortableSnapshotStorage.hpp"
#include "Session.hpp"
#include "WatchdogWin.hpp"
#include "thprac_gui_locale.h"
#include "thprac_th06.h"

#include <memory>

namespace th06::Netplay
{
namespace
{
const char *TrRecovery(const char *zh, const char *en, const char *ja)
{
    switch (THPrac::Gui::LocaleGet())
    {
    case THPrac::Gui::LOCALE_ZH_CN:
        return zh;
    case THPrac::Gui::LOCALE_JA_JP:
        return ja;
    case THPrac::Gui::LOCALE_EN_US:
    default:
        return en;
    }
}

constexpr u8 kSnapshotDatagramMagic[4] = {'T', 'S', 'N', 'P'};
constexpr u8 kSnapshotDatagramVersion = 1;
constexpr int kRecoveryBurstCount = 4;

const char *PhaseToText(AuthoritativeRecoveryPhase phase)
{
    switch (phase)
    {
    case AuthoritativeRecoveryPhase_WaitingAuthority:
        return "waiting";
    case AuthoritativeRecoveryPhase_PendingFreeze:
        return "pending-freeze";
    case AuthoritativeRecoveryPhase_SendingSnapshot:
        return "sending";
    case AuthoritativeRecoveryPhase_ReceivingSnapshot:
        return "receiving";
    case AuthoritativeRecoveryPhase_ApplyingSnapshot:
        return "applying";
    case AuthoritativeRecoveryPhase_PendingResume:
        return "pending-resume";
    case AuthoritativeRecoveryPhase_Failed:
        return "failed";
    default:
        return "inactive";
    }
}

const char *ReasonToText(AuthoritativeRecoveryReason reason)
{
    switch (reason)
    {
    case AuthoritativeRecoveryReason_UnrollbackableDesync:
        return "unrollbackable desync";
    case AuthoritativeRecoveryReason_PredictionTimeout:
        return "prediction timeout";
    case AuthoritativeRecoveryReason_ReconnectTimeout:
        return "reconnect timeout";
    case AuthoritativeRecoveryReason_RemoteRequest:
        return "peer requested recovery";
    case AuthoritativeRecoveryReason_SnapshotCorrupt:
        return "snapshot corrupt";
    case AuthoritativeRecoveryReason_RestoreFailed:
        return "snapshot restore failed";
    default:
        return "recovery";
    }
}

const char *PortablePhaseToText(PortableGameplayRestore::Phase phase)
{
    using Phase = PortableGameplayRestore::Phase;
    switch (phase)
    {
    case Phase::Idle:
        return "idle";
    case Phase::PendingDecode:
        return "pending-decode";
    case Phase::PendingBootstrap:
        return "pending-bootstrap";
    case Phase::WaitingForGameplayShell:
        return "waiting-shell";
    case Phase::Applying:
        return "applying";
    case Phase::SyncingShell:
        return "syncing-shell";
    case Phase::Completed:
        return "completed";
    case Phase::Failed:
        return "failed";
    default:
        return "unknown";
    }
}

const char *PortableSourceToText(PortableGameplayRestore::Source source)
{
    using Source = PortableGameplayRestore::Source;
    switch (source)
    {
    case Source::ManualMemory:
        return "manual-memory";
    case Source::ManualDisk:
        return "manual-disk";
    case Source::AuthoritativeNetplayRecovery:
        return "netplay-authoritative";
    default:
        return "unknown";
    }
}

void ClearBitmap(u8 *bitmap)
{
    if (bitmap != nullptr)
    {
        std::memset(bitmap, 0, kAuthoritativeRecoveryBitmapBytes);
    }
}

void SetBitmapBit(u8 *bitmap, int index)
{
    if (bitmap == nullptr || index < 0 || index >= kAuthoritativeRecoveryMaxChunks)
    {
        return;
    }
    bitmap[index / 8] |= (u8)(1u << (index % 8));
}

bool TestBitmapBit(const u8 *bitmap, int index)
{
    if (bitmap == nullptr || index < 0 || index >= kAuthoritativeRecoveryMaxChunks)
    {
        return false;
    }
    return (bitmap[index / 8] & (u8)(1u << (index % 8))) != 0;
}

int CountBitmapBits(const u8 *bitmap, int chunkCount)
{
    int count = 0;
    for (int i = 0; i < chunkCount; ++i)
    {
        if (TestBitmapBit(bitmap, i))
        {
            ++count;
        }
    }
    return count;
}

bool AllChunksReceived(const u8 *bitmap, int chunkCount)
{
    return chunkCount > 0 && CountBitmapBits(bitmap, chunkCount) >= chunkCount;
}

bool SendSnapshotDatagram(SnapshotDatagramKind kind, u16 recoverySerial, u32 frameValue, u32 rawBytes,
                          u32 compressedBytes, u16 chunkIndex, u16 chunkCount, u16 flags,
                          AuthoritativeRecoveryReason reason, u32 digest32, const void *payload = nullptr,
                          int payloadBytes = 0);

u16 BuildGuestSnapshotAckFlags(const AuthoritativeRecoveryState &recovery)
{
    u16 flags = 0;
    if (recovery.metaReceived)
    {
        flags |= SnapshotAckFlag_MetaReceived;
    }
    if (recovery.chunkCount > 0 && AllChunksReceived(recovery.receivedBitmap, recovery.chunkCount))
    {
        flags |= SnapshotAckFlag_AllChunksReceived;
    }
    if (recovery.restoreApplied)
    {
        flags |= SnapshotAckFlag_RestoreApplied;
    }
    if (recovery.restoreFailed)
    {
        flags |= SnapshotAckFlag_Failed;
    }
    return flags;
}

bool IsSameSnapshotMeta(const AuthoritativeRecoveryState &recovery, const SnapshotDatagramHeader &header)
{
    return recovery.rawBytes == header.rawBytes && recovery.compressedBytes == header.compressedBytes &&
           recovery.chunkCount == header.chunkCount && recovery.digest32 == header.digest32;
}

void TraceRecoveryProgress(const char *trigger)
{
    const AuthoritativeRecoveryState &recovery = g_State.recovery;
    TraceDiagnostic("recovery-progress", "serial=%u phase=%s trigger=%s received=%d/%d acked=%d/%d",
                    recovery.recoverySerial, PhaseToText(recovery.phase), trigger != nullptr ? trigger : "-",
                    CountBitmapBits(recovery.receivedBitmap, recovery.chunkCount), recovery.chunkCount,
                    CountBitmapBits(recovery.peerAckBitmap, recovery.chunkCount), recovery.chunkCount);
}

void MarkRecoveryProgress(const char *trigger)
{
    g_State.recovery.lastProgressTick = SDL_GetTicks64();
    TraceRecoveryProgress(trigger);
}

void MaybeRequestRecoveryAutoDump(const char *trigger)
{
    AuthoritativeRecoveryState &recovery = g_State.recovery;
    if (recovery.autoDumpRequested || !THPrac::TH06::THPracIsRecoveryAutoDumpEnabled())
    {
        return;
    }

    recovery.autoDumpRequested = true;
    const bool requested = th06::WatchdogWin::RequestManualDump();
    TraceDiagnostic("recovery-auto-dump", "serial=%u phase=%s trigger=%s requested=%d",
                    recovery.recoverySerial, PhaseToText(recovery.phase), trigger != nullptr ? trigger : "-",
                    requested ? 1 : 0);
}

void SendGuestSnapshotAck(const AuthoritativeRecoveryState &recovery)
{
    SendSnapshotDatagram(SnapshotDatagram_SnapshotAck, recovery.recoverySerial, 0, 0, 0, 0, recovery.chunkCount,
                         BuildGuestSnapshotAckFlags(recovery), AuthoritativeRecoveryReason_None, 0,
                         recovery.receivedBitmap, kAuthoritativeRecoveryBitmapBytes);
}

u32 ComputeDigest32(const std::vector<u8> &bytes)
{
    return bytes.empty() ? 0u : SDL_crc32(0, bytes.data(), bytes.size());
}

bool SendSnapshotDatagram(SnapshotDatagramKind kind, u16 recoverySerial, u32 frameValue, u32 rawBytes,
                          u32 compressedBytes, u16 chunkIndex, u16 chunkCount, u16 flags,
                          AuthoritativeRecoveryReason reason, u32 digest32, const void *payload, int payloadBytes)
{
    if (payloadBytes < 0 || payloadBytes > kRelayMaxDatagramBytes - (int)sizeof(SnapshotDatagramHeader))
    {
        return false;
    }

    u8 buffer[kRelayMaxDatagramBytes] = {};
    SnapshotDatagramHeader header {};
    std::memcpy(header.magic, kSnapshotDatagramMagic, sizeof(kSnapshotDatagramMagic));
    header.version = kSnapshotDatagramVersion;
    header.kind = (u8)kind;
    header.recoverySerial = recoverySerial;
    header.chunkIndex = chunkIndex;
    header.chunkCount = chunkCount;
    header.payloadBytes = (u16)payloadBytes;
    header.flags = flags;
    header.frameValue = frameValue;
    header.rawBytes = rawBytes;
    header.compressedBytes = compressedBytes;
    header.reason = (u32)reason;
    header.digest32 = digest32;
    std::memcpy(buffer, &header, sizeof(header));
    if (payloadBytes > 0 && payload != nullptr)
    {
        std::memcpy(buffer + sizeof(header), payload, (size_t)payloadBytes);
    }
    return SendDatagramImmediate(buffer, sizeof(header) + (size_t)payloadBytes);
}

void AbortMatchToMainMenu(const char *statusText)
{
    TraceDiagnostic("recovery-abort-local", "status=%s", statusText != nullptr ? statusText : "-");
    ResetLauncherState();
    Session::UseLocalSession();
    g_Supervisor.curState = SUPERVISOR_STATE_MAINMENU;
    g_Supervisor.wantedState = SUPERVISOR_STATE_MAINMENU;
    g_Supervisor.wantedState2 = SUPERVISOR_STATE_MAINMENU;
    SetStatus(statusText != nullptr ? statusText : "recovery failed");
}

void SendAbortBurst(u16 recoverySerial, AuthoritativeRecoveryReason reason)
{
    for (int i = 0; i < kRecoveryBurstCount; ++i)
    {
        SendSnapshotDatagram(SnapshotDatagram_RecoveryAbort, recoverySerial, 0, 0, 0, 0, 0, 0, reason, 0);
    }
}

void SendResumeBurst(u16 recoverySerial, int resumeFrame)
{
    for (int i = 0; i < kRecoveryBurstCount; ++i)
    {
        SendSnapshotDatagram(SnapshotDatagram_RecoveryResume, recoverySerial, (u32)std::max(0, resumeFrame), 0, 0, 0, 0,
                             0, AuthoritativeRecoveryReason_None, 0);
    }
}

void FailAuthoritativeRecovery(AuthoritativeRecoveryReason reason, const char *statusText, bool sendAbort)
{
    if (sendAbort && g_State.recovery.active && g_State.recovery.isHostAuthority)
    {
        SendAbortBurst(g_State.recovery.recoverySerial, reason);
    }
    AbortMatchToMainMenu(statusText);
}

bool StartHostAuthorityRecovery(int frame, AuthoritativeRecoveryReason reason)
{
    TraceDiagnostic("recovery-host-capture-begin", "frame=%d reason=%s", frame, ReasonToText(reason));
    auto portableState = std::make_unique<DGS::PortableGameplayState>();
    DGS::CapturePortableGameplayState(*portableState);
    TraceDiagnostic("recovery-host-capture-end", "frame=%d", frame);
    TraceDiagnostic("recovery-host-encode-begin", "frame=%d", frame);
    std::vector<u8> rawBytes = DGS::EncodePortableGameplayState(*portableState);
    portableState.reset();
    TraceDiagnostic("recovery-host-encode-end", "frame=%d raw=%u", frame, (u32)rawBytes.size());
    if (rawBytes.empty())
    {
        FailAuthoritativeRecovery(AuthoritativeRecoveryReason_SnapshotCorrupt, "snapshot encode failed", false);
        return false;
    }

    std::vector<u8> compressedBytes;
    std::string compressError;
    TraceDiagnostic("recovery-host-compress-begin", "frame=%d raw=%u", frame, (u32)rawBytes.size());
    if (!PortableSnapshotStorage::CompressPortableSnapshotForTransport(rawBytes, compressedBytes, &compressError))
    {
        FailAuthoritativeRecovery(AuthoritativeRecoveryReason_SnapshotCorrupt, compressError.c_str(), false);
        return false;
    }
    TraceDiagnostic("recovery-host-compress-end", "frame=%d compressed=%u", frame, (u32)compressedBytes.size());
    if (compressedBytes.size() > kAuthoritativeRecoveryMaxCompressedBytes)
    {
        FailAuthoritativeRecovery(AuthoritativeRecoveryReason_SnapshotCorrupt, "snapshot too large", false);
        return false;
    }

    const u16 chunkCount =
        (u16)((compressedBytes.size() + (size_t)kAuthoritativeRecoveryChunkPayloadBytes - 1) /
              (size_t)kAuthoritativeRecoveryChunkPayloadBytes);
    if (chunkCount == 0 || chunkCount > kAuthoritativeRecoveryMaxChunks)
    {
        FailAuthoritativeRecovery(AuthoritativeRecoveryReason_SnapshotCorrupt, "snapshot chunk count invalid", false);
        return false;
    }

    AuthoritativeRecoveryState &recovery = g_State.recovery;
    const u16 nextSerial = recovery.nextRecoverySerial;
    ResetAuthoritativeRecoveryState();
    recovery = {};
    recovery.active = true;
    recovery.isHostAuthority = true;
    recovery.recoverySerial = nextSerial;
    recovery.nextRecoverySerial = (u16)std::max<int>(1, nextSerial + 1);
    recovery.freezeFrame = frame;
    recovery.resumeFrame = frame + 1;
    recovery.reason = reason;
    recovery.phase = AuthoritativeRecoveryPhase_SendingSnapshot;
    recovery.startTick = SDL_GetTicks64();
    recovery.lastProgressTick = recovery.startTick;
    recovery.lastRecvTick = recovery.startTick;
    recovery.rawSnapshotBytes = std::move(rawBytes);
    recovery.compressedSnapshotBytes = std::move(compressedBytes);
    recovery.rawBytes = (u32)recovery.rawSnapshotBytes.size();
    recovery.compressedBytes = (u32)recovery.compressedSnapshotBytes.size();
    recovery.chunkCount = chunkCount;
    recovery.digest32 = ComputeDigest32(recovery.compressedSnapshotBytes);
    ClearBitmap(recovery.receivedBitmap);
    ClearBitmap(recovery.peerAckBitmap);
    g_State.stallFrameRequested = true;
    g_State.currentCtrl = IGC_NONE;
    SetStatus("recovering authoritative snapshot");
    TraceDiagnostic("recovery-enter-host",
                    "serial=%u frame=%d reason=%s raw=%u compressed=%u chunks=%u",
                    recovery.recoverySerial, frame, ReasonToText(reason), recovery.rawBytes, recovery.compressedBytes,
                    recovery.chunkCount);
    MaybeRequestRecoveryAutoDump("host-enter");
    return true;
}

void StartGuestRecoveryWait(int frame, AuthoritativeRecoveryReason reason)
{
    AuthoritativeRecoveryState &recovery = g_State.recovery;
    if (recovery.active)
    {
        return;
    }

    const u16 nextSerial = recovery.nextRecoverySerial;
    ResetAuthoritativeRecoveryState();
    recovery = {};
    recovery.active = true;
    recovery.isHostAuthority = false;
    recovery.recoverySerial = 0;
    recovery.nextRecoverySerial = nextSerial;
    recovery.freezeFrame = frame;
    recovery.reason = reason;
    recovery.phase = AuthoritativeRecoveryPhase_WaitingAuthority;
    recovery.startTick = SDL_GetTicks64();
    recovery.lastProgressTick = recovery.startTick;
    recovery.lastRecvTick = recovery.startTick;
    recovery.lastSendTick = 0;
    g_State.stallFrameRequested = true;
    g_State.currentCtrl = IGC_NONE;
    SetStatus("waiting for authority snapshot");
    TraceDiagnostic("recovery-request-local", "frame=%d reason=%s", frame, ReasonToText(reason));
}

void CommitAuthoritativeRecoveryResume()
{
    const int resumeFrame = std::max(0, g_State.recovery.resumeFrame);
    TraceDiagnostic("recovery-resume-commit", "serial=%u resume=%d", g_State.recovery.recoverySerial, resumeFrame);

    ClearRuntimeCaches();
    g_State.lastFrame = -1;
    g_State.currentNetFrame = resumeFrame;
    g_State.lastConfirmedSyncFrame = std::max(0, resumeFrame - 1);
    g_State.lastRuntimeReceiveTick = SDL_GetTicks64();
    g_State.isTryingReconnect = false;
    g_State.reconnectIssued = false;
    g_State.reconnectStartTick = 0;
    g_State.isConnected = true;
    g_State.isSync = true;
    g_State.resyncTriggered = false;
    g_State.resyncTargetFrame = 0;
    g_State.currentCtrl = IGC_NONE;
    ResetRollbackEpoch(std::max(0, resumeFrame - 1), "authoritative-recovery", resumeFrame);
    g_State.seedValidationIgnoreUntilFrame =
        resumeFrame + std::max(kKeyPackFrameCount, std::max(2, g_State.delay + 2));
    g_State.desyncStartTick = 0;
    TraceDiagnostic("recovery-resume-grace", "resume=%d graceUntil=%d delay=%d", resumeFrame,
                    g_State.seedValidationIgnoreUntilFrame, g_State.delay);
    SetStatus("connected");
}

void SendRecoveryRequestIfNeeded()
{
    AuthoritativeRecoveryState &recovery = g_State.recovery;
    const Uint64 now = SDL_GetTicks64();
    if (now - recovery.lastSendTick < kAuthoritativeRecoveryResendMs)
    {
        return;
    }

    recovery.lastSendTick = now;
    SendSnapshotDatagram(SnapshotDatagram_RecoveryRequest, 0, (u32)std::max(0, recovery.freezeFrame), 0, 0, 0, 0, 0,
                         recovery.reason, 0);
}

void SendHostRecoverySnapshotIfNeeded()
{
    AuthoritativeRecoveryState &recovery = g_State.recovery;
    const Uint64 now = SDL_GetTicks64();
    if (now - recovery.lastSendTick < kAuthoritativeRecoveryResendMs)
    {
        return;
    }

    recovery.lastSendTick = now;
    if (!recovery.peerMetaAcked)
    {
        SendSnapshotDatagram(SnapshotDatagram_RecoveryEnter, recovery.recoverySerial,
                             (u32)std::max(0, recovery.freezeFrame), 0, 0, 0, 0, 0, recovery.reason, 0);
    }
    SendSnapshotDatagram(SnapshotDatagram_SnapshotMeta, recovery.recoverySerial, 0, recovery.rawBytes,
                         recovery.compressedBytes, 0, recovery.chunkCount, 0, AuthoritativeRecoveryReason_None,
                         recovery.digest32);

    for (u16 chunkIndex = 0; chunkIndex < recovery.chunkCount; ++chunkIndex)
    {
        if (TestBitmapBit(recovery.peerAckBitmap, chunkIndex))
        {
            continue;
        }

        const size_t offset = (size_t)chunkIndex * (size_t)kAuthoritativeRecoveryChunkPayloadBytes;
        if (offset >= recovery.compressedSnapshotBytes.size())
        {
            break;
        }
        const size_t payloadBytes =
            std::min((size_t)kAuthoritativeRecoveryChunkPayloadBytes, recovery.compressedSnapshotBytes.size() - offset);
        SendSnapshotDatagram(SnapshotDatagram_SnapshotChunk, recovery.recoverySerial, 0, recovery.rawBytes,
                             recovery.compressedBytes, chunkIndex, recovery.chunkCount, 0,
                             AuthoritativeRecoveryReason_None, 0, recovery.compressedSnapshotBytes.data() + offset,
                             (int)payloadBytes);
    }
}

void SyncRecoveryWithPortableRestore()
{
    AuthoritativeRecoveryState &recovery = g_State.recovery;
    if (g_State.isHost || !recovery.active || recovery.phase != AuthoritativeRecoveryPhase_ApplyingSnapshot)
    {
        return;
    }

    PortableGameplayRestore::Phase terminalPhase = PortableGameplayRestore::Phase::Idle;
    PortableGameplayRestore::Source terminalSource = PortableGameplayRestore::Source::ManualMemory;
    std::string terminalLine1;
    std::string terminalLine2;
    if (PortableGameplayRestore::ConsumePortableRestoreTerminalResult(terminalPhase, terminalSource, terminalLine1,
                                                                      terminalLine2) &&
        terminalSource == PortableGameplayRestore::Source::AuthoritativeNetplayRecovery)
    {
        recovery.lastPortableRestorePhase = (u8)terminalPhase;
        switch (terminalPhase)
        {
        case PortableGameplayRestore::Phase::Completed:
            recovery.restoreApplied = true;
            recovery.phase = AuthoritativeRecoveryPhase_PendingResume;
            TraceDiagnostic("recovery-restore-completed", "serial=%u recoveryPhase=%s portablePhase=%s source=%s",
                            recovery.recoverySerial, PhaseToText(recovery.phase), PortablePhaseToText(terminalPhase),
                            PortableSourceToText(terminalSource));
            SendGuestSnapshotAck(recovery);
            MarkRecoveryProgress("restore-applied");
            return;

        case PortableGameplayRestore::Phase::Failed:
            TraceDiagnostic("recovery-restore-failed",
                            "serial=%u recoveryPhase=%s portablePhase=%s source=%s detail=%s",
                            recovery.recoverySerial, PhaseToText(recovery.phase), PortablePhaseToText(terminalPhase),
                            PortableSourceToText(terminalSource),
                            terminalLine2.empty() ? "-" : terminalLine2.c_str());
            recovery.restoreFailed = true;
            recovery.reason = AuthoritativeRecoveryReason_RestoreFailed;
            return;

        default:
            break;
        }
    }

    const PortableGameplayRestore::Source source = PortableGameplayRestore::GetPortableRestoreSource();
    if (source != PortableGameplayRestore::Source::AuthoritativeNetplayRecovery)
    {
        return;
    }

    const PortableGameplayRestore::Phase phase = PortableGameplayRestore::GetPortableRestorePhase();
    if (recovery.lastPortableRestorePhase != (u8)phase)
    {
        recovery.lastPortableRestorePhase = (u8)phase;
        TraceDiagnostic("recovery-restore-phase", "serial=%u recoveryPhase=%s portablePhase=%s source=%s",
                        recovery.recoverySerial, PhaseToText(recovery.phase), PortablePhaseToText(phase),
                        PortableSourceToText(source));
    }

    switch (phase)
    {
    case PortableGameplayRestore::Phase::Completed:
        recovery.restoreApplied = true;
        recovery.phase = AuthoritativeRecoveryPhase_PendingResume;
        TraceDiagnostic("recovery-restore-completed", "serial=%u recoveryPhase=%s portablePhase=%s source=%s",
                        recovery.recoverySerial, PhaseToText(recovery.phase), PortablePhaseToText(phase),
                        PortableSourceToText(source));
        SendGuestSnapshotAck(recovery);
        MarkRecoveryProgress("restore-applied");
        break;

    case PortableGameplayRestore::Phase::Failed:
    {
        std::string line1;
        std::string line2;
        PortableGameplayRestore::GetPortableRestoreStatus(line1, line2);
        TraceDiagnostic("recovery-restore-failed", "serial=%u recoveryPhase=%s portablePhase=%s source=%s detail=%s",
                        recovery.recoverySerial, PhaseToText(recovery.phase), PortablePhaseToText(phase),
                        PortableSourceToText(source), line2.empty() ? "-" : line2.c_str());
        recovery.restoreFailed = true;
        recovery.reason = AuthoritativeRecoveryReason_RestoreFailed;
        break;
    }

    default:
        break;
    }
}
} // namespace

void ResetAuthoritativeRecoveryState()
{
    const u16 nextSerial = std::max<u16>(1, g_State.recovery.nextRecoverySerial);
    g_State.recovery = {};
    g_State.recovery.nextRecoverySerial = nextSerial;
}

bool TryStartAuthoritativeRecovery(int frame, AuthoritativeRecoveryReason reason)
{
    if (Session::GetKind() != SessionKind::Netplay || !g_State.isSessionActive || IsCurrentUiFrame())
    {
        return false;
    }

    if (g_State.recovery.active)
    {
        return false;
    }

    if (g_State.isHost)
    {
        return StartHostAuthorityRecovery(frame, reason);
    }

    StartGuestRecoveryWait(frame, reason);
    return true;
}

bool HandleSnapshotSidebandDatagram(const SnapshotDatagramHeader &header, const u8 *payload, int payloadBytes)
{
    if (Session::GetKind() != SessionKind::Netplay || !g_State.isSessionActive)
    {
        return false;
    }

    AuthoritativeRecoveryState &recovery = g_State.recovery;
    recovery.lastRecvTick = SDL_GetTicks64();

    const SnapshotDatagramKind kind = (SnapshotDatagramKind)header.kind;
    switch (kind)
    {
    case SnapshotDatagram_RecoveryRequest:
        if (!g_State.isHost)
        {
            return false;
        }
        TraceDiagnostic("recovery-request-recv", "frame=%d reason=%u active=%d", CurrentNetFrame(), header.reason,
                        recovery.active ? 1 : 0);
        if (!recovery.active)
        {
            StartHostAuthorityRecovery(CurrentNetFrame(), AuthoritativeRecoveryReason_RemoteRequest);
        }
        return true;

    case SnapshotDatagram_RecoveryEnter:
        if (g_State.isHost || header.recoverySerial == 0)
        {
            return false;
        }
        if (recovery.active && recovery.recoverySerial != 0 && header.recoverySerial < recovery.recoverySerial)
        {
            return true;
        }
        if (recovery.active && recovery.recoverySerial == header.recoverySerial)
        {
            const bool advancedFromWaiting = recovery.phase == AuthoritativeRecoveryPhase_WaitingAuthority;
            recovery.freezeFrame = (int)header.frameValue;
            recovery.reason = (AuthoritativeRecoveryReason)header.reason;
            if (advancedFromWaiting)
            {
                recovery.phase = AuthoritativeRecoveryPhase_ReceivingSnapshot;
                SetStatus("receiving authority snapshot");
                MarkRecoveryProgress("enter-dup-activate");
            }
            else
            {
                TraceDiagnostic("recovery-enter-guest-dup", "serial=%u freeze=%u reason=%u", header.recoverySerial,
                                header.frameValue, header.reason);
            }
            return true;
        }
        TraceDiagnostic("recovery-enter-guest", "serial=%u freeze=%u reason=%u", header.recoverySerial, header.frameValue,
                        header.reason);
        ResetAuthoritativeRecoveryState();
        recovery.active = true;
        recovery.isHostAuthority = false;
        recovery.recoverySerial = header.recoverySerial;
        recovery.freezeFrame = (int)header.frameValue;
        recovery.reason = (AuthoritativeRecoveryReason)header.reason;
        recovery.phase = AuthoritativeRecoveryPhase_ReceivingSnapshot;
        recovery.startTick = SDL_GetTicks64();
        recovery.lastProgressTick = recovery.startTick;
        recovery.lastRecvTick = recovery.startTick;
        SetStatus("receiving authority snapshot");
        MarkRecoveryProgress("enter");
        return true;

    case SnapshotDatagram_SnapshotMeta:
        if (g_State.isHost || !recovery.active || recovery.recoverySerial != header.recoverySerial)
        {
            return false;
        }
        if (header.compressedBytes == 0 || header.compressedBytes > kAuthoritativeRecoveryMaxCompressedBytes ||
            header.chunkCount == 0 || header.chunkCount > kAuthoritativeRecoveryMaxChunks ||
            header.rawBytes == 0)
        {
            recovery.restoreFailed = true;
            recovery.reason = AuthoritativeRecoveryReason_SnapshotCorrupt;
            return true;
        }
        if (recovery.metaReceived)
        {
            if (!IsSameSnapshotMeta(recovery, header))
            {
                recovery.restoreFailed = true;
                recovery.reason = AuthoritativeRecoveryReason_SnapshotCorrupt;
                return true;
            }
            TraceDiagnostic("recovery-meta-dup", "serial=%u raw=%u compressed=%u chunks=%u", header.recoverySerial,
                            header.rawBytes, header.compressedBytes, header.chunkCount);
            SendGuestSnapshotAck(recovery);
            return true;
        }
        recovery.rawBytes = header.rawBytes;
        recovery.compressedBytes = header.compressedBytes;
        recovery.chunkCount = header.chunkCount;
        recovery.digest32 = header.digest32;
        recovery.metaReceived = true;
        recovery.receivedCompressedBytes.assign(recovery.compressedBytes, 0);
        ClearBitmap(recovery.receivedBitmap);
        SendGuestSnapshotAck(recovery);
        MarkRecoveryProgress("meta");
        return true;

    case SnapshotDatagram_SnapshotChunk:
        if (g_State.isHost || !recovery.active || recovery.recoverySerial != header.recoverySerial || !recovery.metaReceived)
        {
            return false;
        }
        if (header.chunkIndex >= recovery.chunkCount || payload == nullptr || payloadBytes <= 0)
        {
            recovery.restoreFailed = true;
            recovery.reason = AuthoritativeRecoveryReason_SnapshotCorrupt;
            return true;
        }
        {
            const size_t offset = (size_t)header.chunkIndex * (size_t)kAuthoritativeRecoveryChunkPayloadBytes;
            if (offset + (size_t)payloadBytes > recovery.receivedCompressedBytes.size())
            {
                recovery.restoreFailed = true;
                recovery.reason = AuthoritativeRecoveryReason_SnapshotCorrupt;
                return true;
            }
            const bool wasNewChunk = !TestBitmapBit(recovery.receivedBitmap, header.chunkIndex);
            std::memcpy(recovery.receivedCompressedBytes.data() + offset, payload, (size_t)payloadBytes);
            SetBitmapBit(recovery.receivedBitmap, header.chunkIndex);
            if (wasNewChunk)
            {
                MarkRecoveryProgress("chunk");
            }
        }
        return true;

    case SnapshotDatagram_SnapshotAck:
    {
        if (!g_State.isHost || !recovery.active || !recovery.isHostAuthority || recovery.recoverySerial != header.recoverySerial)
        {
            return false;
        }
        const bool oldPeerMetaAcked = recovery.peerMetaAcked;
        const int oldAckedChunks = CountBitmapBits(recovery.peerAckBitmap, recovery.chunkCount);
        const bool oldRestoreApplied = recovery.phase == AuthoritativeRecoveryPhase_PendingResume;
        if (payload != nullptr && payloadBytes == kAuthoritativeRecoveryBitmapBytes)
        {
            std::memcpy(recovery.peerAckBitmap, payload, kAuthoritativeRecoveryBitmapBytes);
        }
        if ((header.flags & SnapshotAckFlag_MetaReceived) != 0)
        {
            recovery.peerMetaAcked = true;
        }
        if ((header.flags & SnapshotAckFlag_Failed) != 0)
        {
            FailAuthoritativeRecovery(AuthoritativeRecoveryReason_RestoreFailed, "peer restore failed", true);
            return true;
        }
        if ((header.flags & SnapshotAckFlag_RestoreApplied) != 0)
        {
            recovery.phase = AuthoritativeRecoveryPhase_PendingResume;
            recovery.resumeFrame = recovery.freezeFrame + 1;
        }
        const int newAckedChunks = CountBitmapBits(recovery.peerAckBitmap, recovery.chunkCount);
        const bool newRestoreApplied = recovery.phase == AuthoritativeRecoveryPhase_PendingResume;
        if (recovery.peerMetaAcked != oldPeerMetaAcked || newAckedChunks != oldAckedChunks ||
            newRestoreApplied != oldRestoreApplied)
        {
            MarkRecoveryProgress("ack");
        }
        return true;
    }

    case SnapshotDatagram_RecoveryResume:
    {
        if (g_State.isHost || !recovery.active || recovery.recoverySerial != header.recoverySerial)
        {
            return false;
        }
        const bool progress = recovery.phase != AuthoritativeRecoveryPhase_PendingResume ||
                              recovery.resumeFrame != (int)header.frameValue;
        recovery.resumeFrame = (int)header.frameValue;
        recovery.phase = AuthoritativeRecoveryPhase_PendingResume;
        if (progress)
        {
            MarkRecoveryProgress("resume");
        }
        return true;
    }

    case SnapshotDatagram_RecoveryAbort:
        if (!recovery.active || (recovery.recoverySerial != 0 && recovery.recoverySerial != header.recoverySerial))
        {
            return false;
        }
        AbortMatchToMainMenu("recovery aborted");
        return true;

    default:
        return false;
    }
}

void DriveAuthoritativeRecovery(int frame)
{
    AuthoritativeRecoveryState &recovery = g_State.recovery;
    if (!recovery.active)
    {
        return;
    }

    Session::ApplyLegacyFrameInput(0);
    g_State.currentCtrl = IGC_NONE;
    g_State.stallFrameRequested = true;
    ReceiveRuntimePackets();
    SyncRecoveryWithPortableRestore();

    const Uint64 now = SDL_GetTicks64();
    const Uint64 timeoutBase = recovery.lastProgressTick != 0 ? recovery.lastProgressTick : recovery.startTick;
    if (timeoutBase != 0 && now - timeoutBase >= kAuthoritativeRecoveryTimeoutMs)
    {
        TraceDiagnostic("recovery-timeout-no-progress", "serial=%u phase=%s received=%d/%d acked=%d/%d",
                        recovery.recoverySerial, PhaseToText(recovery.phase),
                        CountBitmapBits(recovery.receivedBitmap, recovery.chunkCount), recovery.chunkCount,
                        CountBitmapBits(recovery.peerAckBitmap, recovery.chunkCount), recovery.chunkCount);
        FailAuthoritativeRecovery(AuthoritativeRecoveryReason_ReconnectTimeout, "recovery timeout", true);
        return;
    }

    if (recovery.restoreFailed)
    {
        if (!g_State.isHost)
        {
            SendSnapshotDatagram(SnapshotDatagram_SnapshotAck, recovery.recoverySerial, 0, 0, 0, 0, recovery.chunkCount,
                                 SnapshotAckFlag_Failed, AuthoritativeRecoveryReason_RestoreFailed, 0,
                                 recovery.receivedBitmap, kAuthoritativeRecoveryBitmapBytes);
        }
        FailAuthoritativeRecovery(recovery.reason, "recovery failed", g_State.isHost && recovery.isHostAuthority);
        return;
    }

    switch (recovery.phase)
    {
    case AuthoritativeRecoveryPhase_WaitingAuthority:
        SendRecoveryRequestIfNeeded();
        break;

    case AuthoritativeRecoveryPhase_SendingSnapshot:
        SendHostRecoverySnapshotIfNeeded();
        break;

    case AuthoritativeRecoveryPhase_ReceivingSnapshot:
        if (recovery.metaReceived && recovery.chunkCount > 0 && AllChunksReceived(recovery.receivedBitmap, recovery.chunkCount) &&
            !recovery.restoreQueued)
        {
            const u32 receivedDigest = ComputeDigest32(recovery.receivedCompressedBytes);
            if (receivedDigest != recovery.digest32)
            {
                recovery.restoreFailed = true;
                recovery.reason = AuthoritativeRecoveryReason_SnapshotCorrupt;
                break;
            }

            std::string decompressError;
            if (!PortableSnapshotStorage::DecompressPortableSnapshotFromTransport(recovery.receivedCompressedBytes,
                                                                                  recovery.rawBytes,
                                                                                  recovery.receivedRawBytes,
                                                                                  &decompressError))
            {
                recovery.restoreFailed = true;
                recovery.reason = AuthoritativeRecoveryReason_SnapshotCorrupt;
                break;
            }

            if (!PortableGameplayRestore::QueuePortableRestoreFromMemory(
                    recovery.receivedRawBytes, PortableGameplayRestore::Source::AuthoritativeNetplayRecovery,
                    "netplay-authoritative"))
            {
                recovery.restoreFailed = true;
                recovery.reason = AuthoritativeRecoveryReason_RestoreFailed;
                break;
            }

            recovery.restoreQueued = true;
            recovery.phase = AuthoritativeRecoveryPhase_ApplyingSnapshot;
            SendGuestSnapshotAck(recovery);
            MaybeRequestRecoveryAutoDump("guest-queue-restore");
            MarkRecoveryProgress("queue-restore");
        }
        else if (recovery.metaReceived && now - recovery.lastSendTick >= kAuthoritativeRecoveryResendMs)
        {
            recovery.lastSendTick = now;
            SendGuestSnapshotAck(recovery);
        }
        break;

    case AuthoritativeRecoveryPhase_ApplyingSnapshot:
        break;

    case AuthoritativeRecoveryPhase_PendingResume:
        if (g_State.isHost && recovery.isHostAuthority)
        {
            SendResumeBurst(recovery.recoverySerial, recovery.resumeFrame);
            CommitAuthoritativeRecoveryResume();
            return;
        }
        if (!g_State.isHost && recovery.resumeFrame >= 0)
        {
            CommitAuthoritativeRecoveryResume();
            return;
        }
        break;

    default:
        break;
    }

    (void)frame;
}

bool IsAuthoritativeRecoveryActive()
{
    return g_State.recovery.active;
}

bool IsAuthoritativeRecoveryFreezeActive()
{
    return g_State.recovery.active;
}

bool GetAuthoritativeRecoveryOverlay(std::string &line1, std::string &line2, int &receivedChunks, int &totalChunks)
{
    line1.clear();
    line2.clear();
    receivedChunks = 0;
    totalChunks = 0;
    if (!g_State.recovery.active)
    {
        return false;
    }

    const AuthoritativeRecoveryState &recovery = g_State.recovery;
    totalChunks = recovery.chunkCount;
    if (g_State.isHost && recovery.isHostAuthority)
    {
        receivedChunks = CountBitmapBits(recovery.peerAckBitmap, recovery.chunkCount);
    }
    else
    {
        receivedChunks = CountBitmapBits(recovery.receivedBitmap, recovery.chunkCount);
    }

    switch (recovery.phase)
    {
    case AuthoritativeRecoveryPhase_WaitingAuthority:
        line1 = TrRecovery("正在重连...", "Reconnecting...", "再接続中...");
        line2 = TrRecovery("等待主机权威快照", "Waiting for host authority snapshot",
                           "ホストの権威スナップショットを待機中");
        break;
    case AuthoritativeRecoveryPhase_SendingSnapshot:
        line1 = TrRecovery("正在同步权威快照...", "Syncing authority snapshot...",
                           "権威スナップショットを同期中...");
        line2 = TrRecovery("主机正在发送快照", "Host is sending the snapshot",
                           "ホストがスナップショットを送信中");
        break;
    case AuthoritativeRecoveryPhase_ReceivingSnapshot:
        line1 = TrRecovery("正在同步权威快照...", "Syncing authority snapshot...",
                           "権威スナップショットを同期中...");
        line2 = TrRecovery("正在接收快照分片", "Receiving snapshot chunks",
                           "スナップショット分割を受信中");
        break;
    case AuthoritativeRecoveryPhase_ApplyingSnapshot:
        line1 = TrRecovery("正在恢复游戏状态...", "Restoring gameplay state...",
                           "ゲーム状態を復元中...");
        line2 = TrRecovery("正在反序列化并重建世界", "Deserializing and rebuilding world",
                           "逆シリアル化してワールドを再構築中");
        break;
    case AuthoritativeRecoveryPhase_PendingResume:
        line1 = TrRecovery("正在恢复游戏状态...", "Restoring gameplay state...",
                           "ゲーム状態を復元中...");
        line2 = TrRecovery("等待恢复完成", "Waiting for restore to complete", "復元完了を待機中");
        break;
    default:
        line1 = TrRecovery("正在重连...", "Reconnecting...", "再接続中...");
        line2 = TrRecovery("正在处理权威恢复", "Processing authority recovery",
                           "権威復旧を処理中");
        break;
    }
    return true;
}
} // namespace th06::Netplay
