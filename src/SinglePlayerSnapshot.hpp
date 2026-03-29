#pragma once

#include "inttypes.hpp"

namespace th06::SinglePlayerSnapshot
{
u16 ProcessLocalGameplayInput(u16 nextInput);
void DrawQuickSnapshotOverlay();
void ResetQuickSnapshotState();
bool IsPortableRestoreTrialEnabled();
void SetPortableRestoreTrialEnabled(bool enabled);
} // namespace th06::SinglePlayerSnapshot
