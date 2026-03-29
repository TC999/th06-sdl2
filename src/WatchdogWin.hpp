#pragma once

namespace th06::WatchdogWin
{
void Init();
void Shutdown();
void TickHeartbeat();
bool RequestManualDump();
} // namespace th06::WatchdogWin
