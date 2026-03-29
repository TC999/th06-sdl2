#pragma once

#include <string>

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
std::string LocalizeNetplayStatusText(const std::string &raw);
std::string LocalizeRelayProbeStatusText(const std::string &raw);
}; // namespace th06::OnlineMenu
