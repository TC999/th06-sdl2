#pragma once

#include <string>
#include <vector>

#include "GameplayStatePortable.hpp"

namespace th06
{
struct MainMenu;
}

namespace th06::PortableGameplayRestore
{
enum class Source
{
    ManualMemory,
    ManualDisk,
    AuthoritativeNetplayRecovery,
};

enum class Phase
{
    Idle,
    PendingDecode,
    PendingBootstrap,
    WaitingForGameplayShell,
    Applying,
    SyncingShell,
    Completed,
    Failed,
};

bool QueuePortableRestoreFromDisk(const char *path = nullptr);
bool QueuePortableRestoreFromMemory(const std::vector<u8> &bytes, Source source = Source::ManualMemory,
                                    const char *sourceTag = "memory");
void TickPortableRestore();
bool ConsumeForcedClearFrameRequested();
bool ShouldForceClearGameplayFrame();
bool ConsumeFrameBreakRequested();
bool TryConsumePendingMainMenuBootstrap(::th06::MainMenu *menu);
bool IsBootstrapOrApplyActive();
bool ShouldAdvanceSupervisorTransitionWhileStalled();
void OnMainMenuEntered();
void ResetPortableRestoreState();
Phase GetPortableRestorePhase();
Source GetPortableRestoreSource();
bool HasPortableRestoreStatus();
void GetPortableRestoreStatus(std::string &line1, std::string &line2);
bool ConsumePortableRestoreTerminalResult(Phase &phase, Source &source, std::string &line1, std::string &line2);
} // namespace th06::PortableGameplayRestore
