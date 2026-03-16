#pragma once
// thprac_bridge.h - Maps thprac's hardcoded memory addresses to th06 source variables
//
// In the original thprac, game state was accessed via hardcoded addresses like:
//   *(int8_t*)(0x69d4ba) = lives;
// In the source build, we access the actual game state structs instead:
//   g_GameManager.livesRemaining = lives;
//
// This header provides named references for every address thprac_th06.cpp uses.

#include "GameManager.hpp"
#include "Player.hpp"
#include "EnemyManager.hpp"
#include "BulletManager.hpp"
#include "Supervisor.hpp"
#include "Rng.hpp"
#include "SoundPlayer.hpp"

namespace THPrac { namespace TH06 { namespace Bridge {

// ============================================================================
// GameManager fields (original base: 0x69bca0)
// ============================================================================
// 0x69bca0 → guiScore, 0x69bca4 → score
inline auto& GM_GuiScore()      { return th06::g_GameManager.guiScore; }
inline auto& GM_Score()          { return th06::g_GameManager.score; }
// 0x69bcb0 → difficulty (Difficulty enum, treated as int8 in thprac)
inline auto& GM_Difficulty()     { return th06::g_GameManager.difficulty; }
// 0x69bcb4 → grazeInStage, 0x69bcb8 → grazeInTotal
inline auto& GM_GrazeInStage()   { return th06::g_GameManager.grazeInStage; }
inline auto& GM_GrazeInTotal()   { return th06::g_GameManager.grazeInTotal; }
// 0x69bcbc → isInReplay
inline auto& GM_IsInReplay()     { return th06::g_GameManager.isInReplay; }
// 0x69bcc4 → bombsUsed
inline auto& GM_BombsUsed()      { return th06::g_GameManager.bombsUsed; }
// 0x69bccc → isTimeStopped
inline auto& GM_IsTimeStopped()  { return th06::g_GameManager.isTimeStopped; }
// 0x69d4b0 → currentPower
inline auto& GM_Power()          { return th06::g_GameManager.currentPower; }
// 0x69d4b4 → pointItemsCollectedInStage, 0x69d4b6 → pointItemsCollected
inline auto& GM_PointInStage()   { return th06::g_GameManager.pointItemsCollectedInStage; }
inline auto& GM_PointTotal()     { return th06::g_GameManager.pointItemsCollected; }
// 0x69d4ba → livesRemaining, 0x69d4bb → bombsRemaining
inline auto& GM_Lives()          { return th06::g_GameManager.livesRemaining; }
inline auto& GM_Bombs()          { return th06::g_GameManager.bombsRemaining; }
// 0x69d4bc → extraLives
inline auto& GM_ExtraLives()     { return th06::g_GameManager.extraLives; }
// 0x69d4bd → character, 0x69d4be → shotType
inline auto& GM_Character()      { return th06::g_GameManager.character; }
inline auto& GM_ShotType()       { return th06::g_GameManager.shotType; }
// 0x69d4bf → isInGameMenu
inline auto& GM_IsInGameMenu()   { return th06::g_GameManager.isInGameMenu; }
// 0x69d4c3 → isInPracticeMode (approx)
inline auto& GM_IsPracticeMode() { return th06::g_GameManager.isInPracticeMode; }
// 0x69d6d4 → currentStage
inline auto& GM_Stage()          { return th06::g_GameManager.currentStage; }
// 0x69d6d8 → menuCursorBackup
inline auto& GM_MenuCursor()     { return th06::g_GameManager.menuCursorBackup; }
// 0x69d710..0x69d71c → rank, maxRank, minRank, subRank
inline auto& GM_Rank()           { return th06::g_GameManager.rank; }
inline auto& GM_MaxRank()        { return th06::g_GameManager.maxRank; }
inline auto& GM_MinRank()        { return th06::g_GameManager.minRank; }
inline auto& GM_SubRank()        { return th06::g_GameManager.subRank; }
// 0x69d8f8 → g_Rng.seed
inline auto& GM_RngSeed()        { return th06::g_Rng.seed; }
// 0x69D904 → g_CurFrameInput, 0x69D908 → g_LastFrameInput
inline auto& GM_CurInput()       { return th06::g_CurFrameInput; }
inline auto& GM_LastInput()      { return th06::g_LastFrameInput; }
// GameManager gameFrames
inline auto& GM_GameFrames()     { return th06::g_GameManager.gameFrames; }
// isGameCompleted
inline auto& GM_IsCompleted()    { return th06::g_GameManager.isGameCompleted; }

// ============================================================================
// EnemyManager fields (original base: 0x4B79C8)
// ============================================================================
// 0x5A5F8C → spellcardInfo.isCapturing
inline auto& EM_IsCapturing()    { return th06::g_EnemyManager.spellcardInfo.isCapturing; }
// 0x5A5F90 → spellcardInfo.isActive
inline auto& EM_SpellActive()    { return th06::g_EnemyManager.spellcardInfo.isActive; }
// 0x5A5F98 → spellcardInfo.idx
inline auto& EM_SpellIdx()       { return th06::g_EnemyManager.spellcardInfo.idx; }
// 0x5a5fb0 → timelineTime.current
inline auto& EM_TimelineCurrent(){ return th06::g_EnemyManager.timelineTime.current; }

// ============================================================================
// Player fields (original base: 0x6CA628)
// ============================================================================
// 0x6CAA68 → positionCenter.x, 0x6CAA6C → positionCenter.y
inline auto& PL_PosX()           { return th06::g_Player.positionCenter.x; }
inline auto& PL_PosY()           { return th06::g_Player.positionCenter.y; }
// hitbox corners
inline auto& PL_HitboxTL()      { return th06::g_Player.hitboxTopLeft; }
inline auto& PL_HitboxBR()      { return th06::g_Player.hitboxBottomRight; }
// invulnerability timer
inline auto& PL_InvulTimer()     { return th06::g_Player.invulnerabilityTimer; }
// player state
inline auto& PL_State()          { return th06::g_Player.playerState; }

// ============================================================================
// Supervisor fields
// ============================================================================
// 0x6C6EA4 → curState
inline auto& SV_CurState()      { return th06::g_Supervisor.curState; }
// 0x6C6EB0 → unk198
inline auto& SV_Unk198()        { return th06::g_Supervisor.unk198; }
// framerateMultiplier → game speed control
inline auto& SV_FramerateMultiplier() { return th06::g_Supervisor.framerateMultiplier; }

// ============================================================================
// BulletManager
// ============================================================================
inline auto& BM()               { return th06::g_BulletManager; }

// ============================================================================
// EclManager
// ============================================================================
inline auto& ECL_File()          { return th06::g_EclManager.eclFile; }

// ============================================================================
// Gui (include Gui.hpp separately due to dependency chain)
// ============================================================================

}}} // namespace THPrac::TH06::Bridge
