#pragma once

namespace th06::OnlineMenu
{
void Open();
void Close();
void Reset();
bool IsOpen();
bool ShouldForceRunInBackground();
bool ConsumeCloseRequested();
bool AllowsBackShortcut();
void UpdateImGui();
}; // namespace th06::OnlineMenu
