#include "SinglePlayerSnapshot.hpp"

#include "AsciiManager.hpp"
#include "Controller.hpp"
#include "GameManager.hpp"
#include "Player.hpp"
#include "GameErrorContext.hpp"
#include "GamePaths.hpp"
#include "GameplayState.hpp"
#include "GameplayStatePortable.hpp"
#include "Gui.hpp"
#include "PortableGameplayRestore.hpp"
#include "PortableSnapshotStorage.hpp"
#include "Session.hpp"
#include "Supervisor.hpp"
#include "thprac_th06.h"

#include <SDL.h>
#include <cstdio>
#include <memory>

namespace th06::SinglePlayerSnapshot
{
namespace
{
constexpr i32 kOverlayLineHeight = 12;
constexpr i32 kOverlayBottomMargin = 8;
constexpr u32 kValidationOverlayDurationMs = 4000;

struct QuickSnapshotState
{
    DGS::DeterministicGameplayState snapshot {};
    bool hasSnapshot = false;
    std::vector<u8> portableSnapshotBytes;
    bool hasPortableSnapshot = false;
    bool portableRestoreTrialEnabled = false;
    bool prevSavePressed = false;
    bool prevLoadPressed = false;
    bool prevDiskLoadPressed = false;
    bool prevBackupLoadPressed = false;
    bool prevPortableValidatePressed = false;
    int savedStage = -1;
    std::string portableStatusLine1;
    std::string portableStatusLine2;
    u32 portableStatusUntilTick = 0;
};

QuickSnapshotState g_QuickSnapshotState;

bool IsPortableValidationHotkeyPressed(const Uint8 *keyboardState);
bool IsBackupLoadHotkeyPressed(const Uint8 *keyboardState);
void SetPortableValidationStatus(const std::string &line1, const std::string &line2);
bool LoadQuickSnapshotThroughDefaultPath();
bool LoadQuickSnapshotThroughState(const DGS::DeterministicGameplayState &state);
void LogQuickSnapshotEvent(const char *tag, const char *detail);
void LogPortableSaveTrialResult(const char *status, size_t rawBytes, size_t bytesWritten, const char *path);
bool WritePortableSnapshotBytesToSaveFile(const std::vector<u8> &bytes, char *outPath, size_t outPathSize,
                                          size_t *outBytesWritten);

bool IsLocalGameplayFrame()
{
    return Session::GetKind() == SessionKind::Local && g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER &&
           g_GameManager.isInReplay == 0;
}

bool HasActiveDialogue()
{
    return g_Gui.impl != nullptr && g_Gui.HasCurrentMsgIdx() != 0;
}

bool CanUseQuickSnapshotHotkeys()
{
    return IsLocalGameplayFrame() && g_GameManager.isInGameMenu == 0 && g_GameManager.isInRetryMenu == 0 &&
           !HasActiveDialogue();
}

bool IsPortableRestoreTrialActive()
{
    return THPrac::TH06::THPracIsDeveloperModeEnabled() && g_QuickSnapshotState.portableRestoreTrialEnabled;
}

bool ShouldEmitDebugLogs()
{
    return THPrac::TH06::THPracIsDebugLogEnabled();
}

void SyncKeyEdges(bool savePressed, bool loadPressed)
{
    g_QuickSnapshotState.prevSavePressed = savePressed;
    g_QuickSnapshotState.prevLoadPressed = loadPressed;
}

void SyncBackupLoadEdge(bool backupLoadPressed)
{
    g_QuickSnapshotState.prevBackupLoadPressed = backupLoadPressed;
}

void SyncDiskLoadEdge(bool diskLoadPressed)
{
    g_QuickSnapshotState.prevDiskLoadPressed = diskLoadPressed;
}

void SyncPortableValidationEdge(bool validatePressed)
{
    g_QuickSnapshotState.prevPortableValidatePressed = validatePressed;
}

void ClearQuickSnapshot()
{
    g_QuickSnapshotState.hasSnapshot = false;
    g_QuickSnapshotState.portableSnapshotBytes.clear();
    g_QuickSnapshotState.hasPortableSnapshot = false;
    g_QuickSnapshotState.savedStage = -1;
}

void InvalidateSnapshotIfOutOfScope()
{
    if (!g_QuickSnapshotState.hasSnapshot)
    {
        return;
    }

    if (!IsLocalGameplayFrame() || g_QuickSnapshotState.savedStage != g_GameManager.currentStage)
    {
        ClearQuickSnapshot();
    }
}

bool HasCompatibleSnapshot()
{
    return g_QuickSnapshotState.hasSnapshot && IsLocalGameplayFrame() &&
           g_QuickSnapshotState.savedStage == g_GameManager.currentStage;
}

bool HasCompatiblePortableSnapshot()
{
    return g_QuickSnapshotState.hasPortableSnapshot && !g_QuickSnapshotState.portableSnapshotBytes.empty() &&
           HasCompatibleSnapshot();
}

bool HasCompatibleSnapshotForActiveLoadMode()
{
    return IsPortableRestoreTrialActive() ? HasCompatiblePortableSnapshot() : HasCompatibleSnapshot();
}

void SaveQuickSnapshot()
{
    DGS::CaptureDeterministicGameplayState(g_QuickSnapshotState.snapshot);
    auto portableState = std::make_unique<DGS::PortableGameplayState>();
    DGS::CapturePortableGameplayState(*portableState);
    g_QuickSnapshotState.portableSnapshotBytes = DGS::EncodePortableGameplayState(*portableState);
    g_QuickSnapshotState.hasSnapshot = true;
    g_QuickSnapshotState.hasPortableSnapshot = !g_QuickSnapshotState.portableSnapshotBytes.empty();
    g_QuickSnapshotState.savedStage = g_GameManager.currentStage;

    if (IsPortableRestoreTrialActive() && g_QuickSnapshotState.hasPortableSnapshot)
    {
        const size_t rawPortableBytes = g_QuickSnapshotState.portableSnapshotBytes.size();
        char dumpPath[512];
        size_t dumpBytesWritten = 0;
        const bool dumpOk = WritePortableSnapshotBytesToSaveFile(g_QuickSnapshotState.portableSnapshotBytes, dumpPath,
                                                                 sizeof(dumpPath), &dumpBytesWritten);
        if (dumpOk)
        {
            char line2[96];
            const unsigned dumpRatio = rawPortableBytes != 0 ? (unsigned)((dumpBytesWritten * 100u) / rawPortableBytes) : 0;
            std::snprintf(line2, sizeof(line2), "dump:%uB zstd:%u%%", (unsigned)dumpBytesWritten, dumpRatio);
            SetPortableValidationStatus("Portable Save OK", line2);
            LogPortableSaveTrialResult("ok", rawPortableBytes, dumpBytesWritten, dumpPath);
        }
        else
        {
            SetPortableValidationStatus("Portable Save FAIL", "dump failed");
            LogPortableSaveTrialResult("fail", rawPortableBytes, 0, dumpPath);
        }
    }

    char detail[128];
    std::snprintf(detail, sizeof(detail), "portableBytes=%u portableDump=%d", (unsigned)g_QuickSnapshotState.portableSnapshotBytes.size(),
                  IsPortableRestoreTrialActive() ? 1 : 0);
    LogQuickSnapshotEvent("save", detail);
}

void ResetInputAfterSnapshotLoad()
{
    Controller::ResetDeviceInputState();
    Session::ResetLegacyInputState();

    const Uint8 *keyboardState = SDL_GetKeyboardState(nullptr);
    SyncKeyEdges(keyboardState[SDL_SCANCODE_S] != 0, keyboardState[SDL_SCANCODE_L] != 0);
    SyncDiskLoadEdge(false);
    SyncBackupLoadEdge(IsBackupLoadHotkeyPressed(keyboardState));
    SyncPortableValidationEdge(THPrac::TH06::THPracIsDeveloperModeEnabled() &&
                               IsPortableValidationHotkeyPressed(keyboardState));
}

void LogPortableLoadTrialResult(const char *status, const DGS::PortableGameplayBuildResult *build,
                                const DGS::PortableRestoreEvaluation *evaluation, bool restored)
{
    if (!ShouldEmitDebugLogs())
    {
        return;
    }

    const char *readiness = evaluation != nullptr ? DGS::PortableRestoreReadinessToString(evaluation->readiness) : "DecodeFailed";
    const char *reason = (evaluation != nullptr && !evaluation->blockingReasons.empty()) ? evaluation->blockingReasons.front().c_str()
                                                                                          : "";

    char line[512];
    std::snprintf(line, sizeof(line),
                  "[PortableLoadTrial] status=%s readiness=%s restored=%d build=%d players=%u bullets=%u lasers=%u items=%u effects=%u stageObjs=%u stageQuads=%u reason=%s\n",
                  status, readiness, restored ? 1 : 0, build != nullptr && build->success ? 1 : 0,
                  build != nullptr ? build->builtPlayerCount : 0, build != nullptr ? build->builtBulletCount : 0,
                  build != nullptr ? build->builtLaserCount : 0, build != nullptr ? build->builtItemCount : 0,
                  build != nullptr ? build->builtEffectCount : 0, build != nullptr ? build->builtStageObjectCount : 0,
                  build != nullptr ? build->builtStageQuadCount : 0, reason);

    GameErrorContext::Log(&g_GameErrorContext, "%s", line);

    char resolvedLogPath[512];
    GamePaths::Resolve(resolvedLogPath, sizeof(resolvedLogPath), "./log.txt");
    GamePaths::EnsureParentDir(resolvedLogPath);

    FILE *file = nullptr;
    file = std::fopen(resolvedLogPath, "a");
    if (file != nullptr)
    {
        std::fprintf(file, "%s", line);
        std::fflush(file);
        std::fclose(file);
    }
}

void LogQuickSnapshotEvent(const char *tag, const char *detail)
{
    if (!ShouldEmitDebugLogs())
    {
        return;
    }

    char line[512];
    std::snprintf(line, sizeof(line),
                  "[QuickSnapshot] tag=%s mode=%s hasDgs=%d hasPortable=%d savedStage=%d currentStage=%d detail=%s\n",
                  tag, IsPortableRestoreTrialActive() ? "portable" : "dgs", g_QuickSnapshotState.hasSnapshot ? 1 : 0,
                  g_QuickSnapshotState.hasPortableSnapshot ? 1 : 0, g_QuickSnapshotState.savedStage,
                  g_GameManager.currentStage, detail != nullptr ? detail : "");

    GameErrorContext::Log(&g_GameErrorContext, "%s", line);

    char resolvedLogPath[512];
    GamePaths::Resolve(resolvedLogPath, sizeof(resolvedLogPath), "./log.txt");
    GamePaths::EnsureParentDir(resolvedLogPath);

    FILE *file = nullptr;
    file = std::fopen(resolvedLogPath, "a");
    if (file != nullptr)
    {
        std::fprintf(file, "%s", line);
        std::fflush(file);
        std::fclose(file);
    }
}

void LogPortableSaveTrialResult(const char *status, size_t rawBytes, size_t bytesWritten, const char *path)
{
    if (!ShouldEmitDebugLogs())
    {
        return;
    }

    char line[512];
    std::snprintf(line, sizeof(line), "[PortableSaveTrial] status=%s raw=%u stored=%u path=%s\n", status,
                  (unsigned)rawBytes, (unsigned)bytesWritten, path != nullptr ? path : "");

    GameErrorContext::Log(&g_GameErrorContext, "%s", line);

    char resolvedLogPath[512];
    GamePaths::Resolve(resolvedLogPath, sizeof(resolvedLogPath), "./log.txt");
    GamePaths::EnsureParentDir(resolvedLogPath);

    FILE *file = nullptr;
    file = std::fopen(resolvedLogPath, "a");
    if (file != nullptr)
    {
        std::fprintf(file, "%s", line);
        std::fflush(file);
        std::fclose(file);
    }
}

bool WritePortableSnapshotBytesToSaveFile(const std::vector<u8> &bytes, char *outPath, size_t outPathSize,
                                          size_t *outBytesWritten)
{
    if (outPath != nullptr && outPathSize != 0)
    {
        outPath[0] = '\0';
    }
    if (outBytesWritten != nullptr)
    {
        *outBytesWritten = 0;
    }

    if (bytes.empty())
    {
        return false;
    }

    std::vector<u8> diskBytes;
    std::string diskError;
    if (!PortableSnapshotStorage::EncodePortableSnapshotForDisk(bytes, diskBytes, nullptr, &diskError))
    {
        if (ShouldEmitDebugLogs())
        {
            GameErrorContext::Log(&g_GameErrorContext, "[PortableSaveTrial] zstd encode failed reason=%s\n",
                                  diskError.c_str());
        }
        return false;
    }

    char resolvedDumpPath[512];
    GamePaths::Resolve(resolvedDumpPath, sizeof(resolvedDumpPath), "./Save/portable_state.bin");
    GamePaths::EnsureParentDir(resolvedDumpPath);

    FILE *file = nullptr;
    file = std::fopen(resolvedDumpPath, "wb");
    if (file == nullptr)
    {
        return false;
    }

    const size_t written = std::fwrite(diskBytes.data(), 1, diskBytes.size(), file);
    std::fflush(file);
    std::fclose(file);

    if (written != diskBytes.size())
    {
        return false;
    }

    if (outPath != nullptr && outPathSize != 0)
    {
        std::snprintf(outPath, outPathSize, "%s", resolvedDumpPath);
    }
    if (outBytesWritten != nullptr)
    {
        *outBytesWritten = written;
    }
    return true;
}

bool ReadPortableSnapshotBytesFromSaveFile(std::vector<u8> &bytes, char *outPath, size_t outPathSize,
                                           size_t *outBytesRead)
{
    bytes.clear();
    if (outPath != nullptr && outPathSize != 0)
    {
        outPath[0] = '\0';
    }
    if (outBytesRead != nullptr)
    {
        *outBytesRead = 0;
    }

    char resolvedDumpPath[512];
    GamePaths::Resolve(resolvedDumpPath, sizeof(resolvedDumpPath), "./Save/portable_state.bin");

    FILE *file = nullptr;
    file = std::fopen(resolvedDumpPath, "rb");
    if (file == nullptr)
    {
        return false;
    }

    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return false;
    }

    const long fileSize = std::ftell(file);
    if (fileSize <= 0 || std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fclose(file);
        return false;
    }

    std::vector<u8> diskBytes((size_t)fileSize);
    const size_t read = std::fread(diskBytes.data(), 1, diskBytes.size(), file);
    std::fclose(file);

    if (read != diskBytes.size())
    {
        diskBytes.clear();
        return false;
    }

    std::string diskError;
    if (!PortableSnapshotStorage::DecodePortableSnapshotFromDisk(diskBytes, bytes, nullptr, &diskError))
    {
        if (ShouldEmitDebugLogs())
        {
            GameErrorContext::Log(&g_GameErrorContext, "[PortableLoadTrial] zstd decode failed reason=%s\n",
                                  diskError.c_str());
        }
        bytes.clear();
        return false;
    }

    if (outPath != nullptr && outPathSize != 0)
    {
        std::snprintf(outPath, outPathSize, "%s", resolvedDumpPath);
    }
    if (outBytesRead != nullptr)
    {
        *outBytesRead = read;
    }
    return true;
}

bool LoadPortableSnapshotFromDiskTrial()
{
    const bool queued = PortableGameplayRestore::QueuePortableRestoreFromDisk(nullptr);
    LogQuickSnapshotEvent("disk-load-attempt", queued ? "queued" : "queue-failed");
    return queued;
}

bool LoadQuickSnapshotThroughPortableTrial()
{
    if (!HasCompatiblePortableSnapshot())
    {
        const bool fallbackOk = HasCompatibleSnapshot() && LoadQuickSnapshotThroughDefaultPath();
        SetPortableValidationStatus(fallbackOk ? "Portable Load FALLBACK" : "Portable Load EMPTY",
                                    fallbackOk ? "slot unavailable -> DGS" : "save first (S)");
        LogPortableLoadTrialResult(fallbackOk ? "slot-unavailable-fallback" : "slot-unavailable", nullptr, nullptr,
                                   fallbackOk);
        return fallbackOk;
    }

    const bool queued =
        PortableGameplayRestore::QueuePortableRestoreFromMemory(g_QuickSnapshotState.portableSnapshotBytes,
                                                                PortableGameplayRestore::Source::ManualMemory,
                                                                "memory-slot");
    if (!queued)
    {
        SetPortableValidationStatus("Portable Load FAIL", "queue failed");
        LogPortableLoadTrialResult("queue-failed", nullptr, nullptr, false);
        return false;
    }

    SetPortableValidationStatus("Portable Load QUEUED", "pure portable backend");
    LogPortableLoadTrialResult("queued", nullptr, nullptr, false);
    return true;
}

bool LoadQuickSnapshotThroughDefaultPath()
{
    if (!HasCompatibleSnapshot())
    {
        return false;
    }

    return LoadQuickSnapshotThroughState(g_QuickSnapshotState.snapshot);
}

bool LoadQuickSnapshotThroughState(const DGS::DeterministicGameplayState &state)
{
    if (!DGS::RestoreDeterministicGameplayState(state))
    {
        return false;
    }

    ResetInputAfterSnapshotLoad();
    return true;
}

bool LoadQuickSnapshot()
{
    LogQuickSnapshotEvent("load-attempt", HasCompatibleSnapshotForActiveLoadMode() ? "slot-ok" : "slot-missing");

    if (IsPortableRestoreTrialActive())
    {
        return LoadQuickSnapshotThroughPortableTrial();
    }

    return LoadQuickSnapshotThroughDefaultPath();
}

bool IsPortableValidationHotkeyPressed(const Uint8 *keyboardState)
{
    const bool ctrlPressed = keyboardState[SDL_SCANCODE_LCTRL] != 0 || keyboardState[SDL_SCANCODE_RCTRL] != 0;
    const bool shiftPressed = keyboardState[SDL_SCANCODE_LSHIFT] != 0 || keyboardState[SDL_SCANCODE_RSHIFT] != 0;
    const bool pPressed = keyboardState[SDL_SCANCODE_P] != 0;
    return ctrlPressed && shiftPressed && pPressed;
}

bool IsBackupLoadHotkeyPressed(const Uint8 *keyboardState)
{
    const bool ctrlPressed = keyboardState[SDL_SCANCODE_LCTRL] != 0 || keyboardState[SDL_SCANCODE_RCTRL] != 0;
    const bool oPressed = keyboardState[SDL_SCANCODE_O] != 0;
    return THPrac::TH06::THPracIsDeveloperModeEnabled() && ctrlPressed && oPressed;
}

bool IsDiskPortableLoadHotkeyPressed(const Uint8 *keyboardState)
{
    (void)keyboardState;
    return false;
}

void SetPortableValidationStatus(const std::string &line1, const std::string &line2)
{
    g_QuickSnapshotState.portableStatusLine1 = line1;
    g_QuickSnapshotState.portableStatusLine2 = line2;
    g_QuickSnapshotState.portableStatusUntilTick = SDL_GetTicks() + kValidationOverlayDurationMs;
}

void LogPortableValidationResult(const char *status, uint64_t before, uint64_t after,
                                 const DGS::PortableGameplayBuildResult &build,
                                 const DGS::PortableRestoreEvaluation &evaluation)
{
    if (!ShouldEmitDebugLogs())
    {
        return;
    }

    char line[768];
    int written =
        std::snprintf(line, sizeof(line),
                      "[PortableValidation] status=%s readiness=%s success=%d before=%llu after=%llu enemies=%u players=%u playerBullets=%u bullets=%u lasers=%u items=%u effects=%u eclSubs=%u eclTimeline=%d stageObjs=%u stageQuads=%u",
                      status, DGS::PortableRestoreReadinessToString(evaluation.readiness), build.success ? 1 : 0,
                      (unsigned long long)before, (unsigned long long)after, build.builtEnemyCount,
                      build.builtPlayerCount, build.builtPlayerBulletCount, build.builtBulletCount,
                      build.builtLaserCount, build.builtItemCount, build.builtEffectCount, build.builtEclSubCount,
                      build.builtEclTimelineSlot, build.builtStageObjectCount, build.builtStageQuadCount);
    if (written < 0)
    {
        return;
    }

    size_t cursor = (size_t)std::min<int>(written, (int)sizeof(line) - 1);

    if (!evaluation.blockingReasons.empty() && cursor < sizeof(line) - 1)
    {
        int count = std::snprintf(line + cursor, sizeof(line) - cursor, " reasons=");
        if (count > 0)
        {
            cursor += (size_t)std::min<int>(count, (int)sizeof(line) - (int)cursor - 1);
        }

        for (size_t i = 0; i < evaluation.blockingReasons.size() && cursor < sizeof(line) - 1; ++i)
        {
            count = std::snprintf(line + cursor, sizeof(line) - cursor, "%s%s", i != 0 ? " | " : "",
                                  evaluation.blockingReasons[i].c_str());
            if (count > 0)
            {
                cursor += (size_t)std::min<int>(count, (int)sizeof(line) - (int)cursor - 1);
            }
        }
    }

    if (!build.notes.empty() && cursor < sizeof(line) - 1)
    {
        int count = std::snprintf(line + cursor, sizeof(line) - cursor, " notes=");
        if (count > 0)
        {
            cursor += (size_t)std::min<int>(count, (int)sizeof(line) - (int)cursor - 1);
        }

        for (size_t i = 0; i < build.notes.size() && cursor < sizeof(line) - 1; ++i)
        {
            count =
                std::snprintf(line + cursor, sizeof(line) - cursor, "%s%s", i != 0 ? " | " : "", build.notes[i].c_str());
            if (count > 0)
            {
                cursor += (size_t)std::min<int>(count, (int)sizeof(line) - (int)cursor - 1);
            }
        }
    }

    if (cursor < sizeof(line) - 2)
    {
        line[cursor++] = '\n';
        line[cursor] = '\0';
    }

    char loggerLine[480];
    std::snprintf(loggerLine, sizeof(loggerLine), "%s", line);
    GameErrorContext::Log(&g_GameErrorContext, "%s", loggerLine);

    char resolvedLogPath[512];
    GamePaths::Resolve(resolvedLogPath, sizeof(resolvedLogPath), "./log.txt");
    GamePaths::EnsureParentDir(resolvedLogPath);

    FILE *file = nullptr;
    file = std::fopen(resolvedLogPath, "a");
    if (file == nullptr)
    {
        return;
    }

    std::fprintf(file, "%s", line);
    std::fflush(file);
    std::fclose(file);
}

bool WritePortableValidationDump(char *outPath, size_t outPathSize, size_t *outBytesWritten)
{
    if (outPath != nullptr && outPathSize != 0)
    {
        outPath[0] = '\0';
    }
    if (outBytesWritten != nullptr)
    {
        *outBytesWritten = 0;
    }

    auto state = std::make_unique<DGS::PortableGameplayState>();
    DGS::CapturePortableGameplayState(*state);
    const std::vector<u8> bytes = DGS::EncodePortableGameplayState(*state);
    return WritePortableSnapshotBytesToSaveFile(bytes, outPath, outPathSize, outBytesWritten);
}

void RunPortableValidation()
{
    auto build = std::make_unique<DGS::PortableGameplayBuildResult>();
    auto evaluation = std::make_unique<DGS::PortableRestoreEvaluation>();
    uint64_t before = 0;
    uint64_t after = 0;
    const bool ok = DGS::RunPortableGameplayDebugValidation(build.get(), evaluation.get(), &before, &after);
    char dumpPath[512];
    size_t dumpBytesWritten = 0;
    const bool dumpOk = WritePortableValidationDump(dumpPath, sizeof(dumpPath), &dumpBytesWritten);

    char line1[96];
    std::snprintf(line1, sizeof(line1), "Portable %s (%s)", ok ? "OK" : "FAIL",
                  DGS::PortableRestoreReadinessToString(evaluation->readiness));

    char line2[128];
    if (!dumpOk)
    {
        std::snprintf(line2, sizeof(line2), "dump failed");
    }
    else if (evaluation->blockingReasons.empty())
    {
        std::snprintf(line2, sizeof(line2), "dump:%uB p:%u b:%u l:%u i:%u e:%u", (unsigned)dumpBytesWritten,
                      build->builtPlayerCount, build->builtBulletCount, build->builtLaserCount,
                      build->builtItemCount, build->builtEffectCount);
    }
    else
    {
        std::snprintf(line2, sizeof(line2), "%s", evaluation->blockingReasons.front().c_str());
    }

    SetPortableValidationStatus(line1, line2);
    LogPortableValidationResult(ok ? "ok" : "fail", before, after, *build, *evaluation);

    if (dumpOk)
    {
        if (ShouldEmitDebugLogs())
        {
            GameErrorContext::Log(&g_GameErrorContext, "[PortableValidationDump] path=%s bytes=%u\n", dumpPath,
                                  (unsigned)dumpBytesWritten);
        }
    }
}

bool ShouldDrawPortableValidationStatus()
{
    return SDL_GetTicks() < g_QuickSnapshotState.portableStatusUntilTick &&
           (!g_QuickSnapshotState.portableStatusLine1.empty() || !g_QuickSnapshotState.portableStatusLine2.empty());
}

int CountPortableValidationStatusLines()
{
    if (!ShouldDrawPortableValidationStatus())
    {
        return 0;
    }

    int lines = 0;
    if (!g_QuickSnapshotState.portableStatusLine1.empty())
    {
        ++lines;
    }
    if (!g_QuickSnapshotState.portableStatusLine2.empty())
    {
        ++lines;
    }
    return lines;
}

int CountPortableRestoreStatusLines()
{
    std::string line1;
    std::string line2;
    PortableGameplayRestore::GetPortableRestoreStatus(line1, line2);
    int lines = 0;
    if (!line1.empty())
    {
        ++lines;
    }
    if (!line2.empty())
    {
        ++lines;
    }
    return lines;
}

i32 ComputeOverlayBaseY(i32 totalLines)
{
    const i32 bottomAlignedY =
        (i32)GAME_REGION_BOTTOM - kOverlayBottomMargin - (totalLines > 0 ? (totalLines - 1) * kOverlayLineHeight : 0);
    const i32 topClampedY = (i32)GAME_REGION_TOP + 8;
    return std::max(bottomAlignedY, topClampedY);
}

void DrawPortableValidationStatus(i32 baseY, i32 *line)
{
    if (!ShouldDrawPortableValidationStatus())
    {
        return;
    }

    D3DXVECTOR3 pos((f32)GAME_REGION_LEFT, (f32)(baseY + (*line * kOverlayLineHeight)), 0.0f);
    if (!g_QuickSnapshotState.portableStatusLine1.empty())
    {
        g_AsciiManager.AddFormatText(&pos, g_QuickSnapshotState.portableStatusLine1.c_str());
        ++(*line);
        pos.x = (f32)GAME_REGION_LEFT;
        pos.y = (f32)(baseY + (*line * kOverlayLineHeight));
    }
    if (!g_QuickSnapshotState.portableStatusLine2.empty())
    {
        g_AsciiManager.AddFormatText(&pos, g_QuickSnapshotState.portableStatusLine2.c_str());
        ++(*line);
    }
}

void DrawPortableRestoreStatus(i32 baseY, i32 *line)
{
    std::string line1;
    std::string line2;
    PortableGameplayRestore::GetPortableRestoreStatus(line1, line2);
    if (line1.empty() && line2.empty())
    {
        return;
    }

    D3DXVECTOR3 pos((f32)GAME_REGION_LEFT, (f32)(baseY + (*line * kOverlayLineHeight)), 0.0f);
    if (!line1.empty())
    {
        g_AsciiManager.AddFormatText(&pos, line1.c_str());
        ++(*line);
        pos.y = (f32)(baseY + (*line * kOverlayLineHeight));
    }
    if (!line2.empty())
    {
        pos.x = (f32)GAME_REGION_LEFT;
        g_AsciiManager.AddFormatText(&pos, line2.c_str());
        ++(*line);
    }
}
} // namespace

u16 ProcessLocalGameplayInput(u16 nextInput)
{
    const Uint8 *keyboardState = SDL_GetKeyboardState(nullptr);
    const bool ctrlPressed = keyboardState[SDL_SCANCODE_LCTRL] != 0 || keyboardState[SDL_SCANCODE_RCTRL] != 0;
    const bool savePressed = keyboardState[SDL_SCANCODE_S] != 0;
    const bool loadPressed = keyboardState[SDL_SCANCODE_L] != 0 && !ctrlPressed;
    const bool backupLoadPressed = IsBackupLoadHotkeyPressed(keyboardState);
    const bool portableValidatePressed =
        THPrac::TH06::THPracIsDeveloperModeEnabled() && IsPortableValidationHotkeyPressed(keyboardState);
    const bool portableRestoreBusy = PortableGameplayRestore::GetPortableRestorePhase() != PortableGameplayRestore::Phase::Idle;

    InvalidateSnapshotIfOutOfScope();

    if (loadPressed != g_QuickSnapshotState.prevLoadPressed)
    {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "rawL=%d canUse=%d", loadPressed ? 1 : 0, CanUseQuickSnapshotHotkeys() ? 1 : 0);
        LogQuickSnapshotEvent("raw-load-key", detail);
    }
    if (backupLoadPressed != g_QuickSnapshotState.prevBackupLoadPressed)
    {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "rawCtrlO=%d canUse=%d", backupLoadPressed ? 1 : 0,
                      CanUseQuickSnapshotHotkeys() ? 1 : 0);
        LogQuickSnapshotEvent("raw-backup-load-key", detail);
    }
    if (!CanUseQuickSnapshotHotkeys())
    {
        SyncKeyEdges(savePressed, loadPressed);
        SyncDiskLoadEdge(false);
        SyncBackupLoadEdge(backupLoadPressed);
        SyncPortableValidationEdge(portableValidatePressed);
        return nextInput;
    }

    const bool saveEdge = savePressed && !g_QuickSnapshotState.prevSavePressed;
    const bool loadEdge = loadPressed && !g_QuickSnapshotState.prevLoadPressed;
    const bool backupLoadEdge = backupLoadPressed && !g_QuickSnapshotState.prevBackupLoadPressed;
    const bool portableValidateEdge =
        portableValidatePressed && !g_QuickSnapshotState.prevPortableValidatePressed;

    SyncKeyEdges(savePressed, loadPressed);
    SyncDiskLoadEdge(false);
    SyncBackupLoadEdge(backupLoadPressed);
    SyncPortableValidationEdge(portableValidatePressed);

    if (portableRestoreBusy)
    {
        if (loadEdge || backupLoadEdge)
        {
            LogQuickSnapshotEvent(backupLoadEdge ? "backup-load-ignored" : "load-ignored", "portable-restore-busy");
        }
        if (saveEdge)
        {
            SaveQuickSnapshot();
        }
        if (portableValidateEdge)
        {
            RunPortableValidation();
        }
        return nextInput;
    }

    if ((loadEdge || backupLoadEdge) && LoadQuickSnapshot())
    {
        return 0;
    }
    else if (loadEdge || backupLoadEdge)
    {
        const bool slotOk = HasCompatibleSnapshotForActiveLoadMode();
        SetPortableValidationStatus(IsPortableRestoreTrialActive() ? "Portable Load FAIL" : "Load FAIL",
                                    slotOk ? "handler rejected" : "slot unavailable");
        LogQuickSnapshotEvent(backupLoadEdge ? "backup-load-rejected" : "load-rejected",
                              slotOk ? "handler-returned-false" : "slot-unavailable");
    }

    if (saveEdge)
    {
        SaveQuickSnapshot();
    }

    if (portableValidateEdge)
    {
        RunPortableValidation();
    }

    return nextInput;
}

void DrawQuickSnapshotOverlay()
{
    InvalidateSnapshotIfOutOfScope();

    if (!CanUseQuickSnapshotHotkeys())
    {
        return;
    }

    if (!IsPortableRestoreTrialActive())
    {
        return;
    }

    i32 totalLines = 2;
    if (THPrac::TH06::THPracIsDeveloperModeEnabled())
    {
        totalLines += IsPortableRestoreTrialActive() ? 4 : 3;
        if (IsPortableRestoreTrialActive() && HasCompatibleSnapshot() && !HasCompatiblePortableSnapshot())
        {
            ++totalLines;
        }
        totalLines += CountPortableRestoreStatusLines();
        totalLines += CountPortableValidationStatusLines();
    }

    const i32 baseY = ComputeOverlayBaseY(totalLines);

    // Compute player proximity to overlay region and fade opacity to 10% when close
    const f32 overlayLeft = (f32)GAME_REGION_LEFT;
    const f32 overlayTop = (f32)baseY;
    const f32 overlayRight = overlayLeft + 200.0f; // approximate text width
    const f32 overlayBottom = (f32)(baseY + totalLines * kOverlayLineHeight);
    const f32 fadeMargin = 48.0f; // start fading within this distance
    const f32 px = g_Player.positionCenter.x;
    const f32 py = g_Player.positionCenter.y;
    // Distance from player to overlay rect (0 if inside)
    const f32 dx = (px < overlayLeft) ? (overlayLeft - px) : (px > overlayRight) ? (px - overlayRight) : 0.0f;
    const f32 dy = (py < overlayTop) ? (overlayTop - py) : (py > overlayBottom) ? (py - overlayBottom) : 0.0f;
    const f32 dist = std::sqrt(dx * dx + dy * dy);
    // Lerp alpha: full (0xFF) at fadeMargin distance, 10% (0x1A) at 0 distance
    const f32 t = (dist >= fadeMargin) ? 1.0f : (dist / fadeMargin);
    const u8 alpha = (u8)(0x1A + (u8)((0xFF - 0x1A) * t));
    const D3DCOLOR savedColor = g_AsciiManager.color;
    // Apply alpha to existing color (preserve RGB, replace alpha channel)
    g_AsciiManager.SetColor((savedColor & 0x00FFFFFF) | ((D3DCOLOR)alpha << 24));

    i32 line = 0;
    D3DXVECTOR3 pos((f32)GAME_REGION_LEFT, (f32)(baseY + (line * kOverlayLineHeight)), 0.0f);
    g_AsciiManager.AddFormatText(&pos, "Save(S)");
    ++line;

    pos.x = (f32)GAME_REGION_LEFT;
    pos.y = (f32)(baseY + (line * kOverlayLineHeight));
    if (HasCompatibleSnapshotForActiveLoadMode())
    {
        g_AsciiManager.AddFormatText(&pos, IsPortableRestoreTrialActive() ? "Load(L)[P]" : "Load(L)");
    }
    else
    {
        g_AsciiManager.AddFormatText(&pos, IsPortableRestoreTrialActive() ? "Load(L)[P] --" : "Load(L) --");
    }
    ++line;

    if (THPrac::TH06::THPracIsDeveloperModeEnabled())
    {
        pos.x = (f32)GAME_REGION_LEFT;
        pos.y = (f32)(baseY + (line * kOverlayLineHeight));
        g_AsciiManager.AddFormatText(&pos, IsPortableRestoreTrialActive() ? "LoadMode: Portable" : "LoadMode: DGS");
        ++line;
        pos.x = (f32)GAME_REGION_LEFT;
        pos.y = (f32)(baseY + (line * kOverlayLineHeight));
        if (IsPortableRestoreTrialActive() && HasCompatibleSnapshot() && !HasCompatiblePortableSnapshot())
        {
            g_AsciiManager.AddFormatText(&pos, "PortableSlot: Resave(S)");
            ++line;
            pos.x = (f32)GAME_REGION_LEFT;
            pos.y = (f32)(baseY + (line * kOverlayLineHeight));
        }
        pos.x = (f32)GAME_REGION_LEFT;
        pos.y = (f32)(baseY + (line * kOverlayLineHeight));
        g_AsciiManager.AddFormatText(&pos, "LoadAlt(Ctrl+O)");
        ++line;
        if (IsPortableRestoreTrialActive())
        {
            pos.x = (f32)GAME_REGION_LEFT;
            pos.y = (f32)(baseY + (line * kOverlayLineHeight));
            g_AsciiManager.AddFormatText(&pos, "LoadDisk(Ctrl+L)");
            ++line;
        }
        pos.x = (f32)GAME_REGION_LEFT;
        pos.y = (f32)(baseY + (line * kOverlayLineHeight));
        g_AsciiManager.AddFormatText(&pos, "Portable(Ctrl+Shift+P)");
        ++line;
        DrawPortableRestoreStatus(baseY, &line);
        DrawPortableValidationStatus(baseY, &line);
    }

    // Restore original color
    g_AsciiManager.SetColor(savedColor);
}

void ResetQuickSnapshotState()
{
    g_QuickSnapshotState.prevSavePressed = false;
    g_QuickSnapshotState.prevLoadPressed = false;
    g_QuickSnapshotState.prevDiskLoadPressed = false;
    g_QuickSnapshotState.prevBackupLoadPressed = false;
    g_QuickSnapshotState.prevPortableValidatePressed = false;
    g_QuickSnapshotState.portableStatusLine1.clear();
    g_QuickSnapshotState.portableStatusLine2.clear();
    g_QuickSnapshotState.portableStatusUntilTick = 0;
}

bool IsPortableRestoreTrialEnabled()
{
    return g_QuickSnapshotState.portableRestoreTrialEnabled;
}

void SetPortableRestoreTrialEnabled(bool enabled)
{
    g_QuickSnapshotState.portableRestoreTrialEnabled = enabled;
}
} // namespace th06::SinglePlayerSnapshot
