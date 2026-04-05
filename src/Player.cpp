#include "Player.hpp"

#include "AnmManager.hpp"
#include "AnmVm.hpp"
#include "BombData.hpp"
#include "BulletData.hpp"
#include "BulletManager.hpp"
#include "ChainPriorities.hpp"
#include "EclManager.hpp"
#include "EffectManager.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "NetplayAuthoritativePresentation.hpp"
#include "NetplaySession.hpp"
#include "Rng.hpp"
#include "Session.hpp"
#include "ScreenEffect.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"
#include "ZunBool.hpp"
#include "i18n.hpp"
#include "utils.hpp"
#include "thprac_th06.h"

namespace th06
{
namespace
{
bool HasSecondPlayer()
{
    return Session::IsDualPlayerSession();
}

float PlayerSpawnCenterX(const Player *player)
{
    const float centerX = g_GameManager.arcadeRegionSize.x / 2.0f;
    if (!HasSecondPlayer())
    {
        return centerX;
    }

    return centerX + (player->playerType == 1 ? -32.0f : 32.0f);
}

int CharacterShotTypeForPlayer(const Player *player)
{
    return player->playerType == 1 ? g_GameManager.CharacterShotType() : g_GameManager.CharacterShotType2();
}

int PlayerIdleScript(const Player *player)
{
    return player->playerType == 1 ? ANM_SCRIPT_PLAYER_IDLE : ANM_SCRIPT_PLAYER_IDLE2;
}

int PlayerMoveLeftScript(const Player *player)
{
    return player->playerType == 1 ? ANM_SCRIPT_PLAYER_MOVING_LEFT : ANM_SCRIPT_PLAYER_MOVING_LEFT2;
}

int PlayerStopLeftScript(const Player *player)
{
    return player->playerType == 1 ? ANM_SCRIPT_PLAYER_STOPPING_LEFT : ANM_SCRIPT_PLAYER_STOPPING_LEFT2;
}

int PlayerMoveRightScript(const Player *player)
{
    return player->playerType == 1 ? ANM_SCRIPT_PLAYER_MOVING_RIGHT : ANM_SCRIPT_PLAYER_MOVING_RIGHT2;
}

int PlayerStopRightScript(const Player *player)
{
    return player->playerType == 1 ? ANM_SCRIPT_PLAYER_STOPPING_RIGHT : ANM_SCRIPT_PLAYER_STOPPING_RIGHT2;
}

float MInterpolation(float t, float a, float b)
{
    if (t < 0.0f)
    {
        return a;
    }
    if (t < 0.5f)
    {
        const float k = (b - a) * 2.0f;
        return k * t * t + a;
    }
    if (t < 1.0f)
    {
        const float k = (b - a) * 2.0f;
        t -= 1.0f;
        return -k * t * t + b;
    }
    return b;
}

bool ConfigurePlayerChain(Player *player, u8 unk, i8 playerType)
{
    memset(player, 0, sizeof(Player));
    player->playerType = playerType;
    player->invulnerabilityTimer.InitializeForPopup();
    player->unk_9e1 = unk;
    player->chainCalc = g_Chain.CreateElem((ChainCallback)Player::OnUpdate);
    player->chainDraw1 = g_Chain.CreateElem((ChainCallback)Player::OnDrawHighPrio);
    player->chainDraw2 = g_Chain.CreateElem((ChainCallback)Player::OnDrawLowPrio);
    if (player->chainCalc == NULL || player->chainDraw1 == NULL || player->chainDraw2 == NULL)
    {
        return false;
    }
    player->chainCalc->arg = player;
    player->chainDraw1->arg = player;
    player->chainDraw2->arg = player;
    player->chainCalc->addedCallback = (ChainAddedCallback)Player::AddedCallback;
    player->chainCalc->deletedCallback = (ChainDeletedCallback)Player::DeletedCallback;
    if (g_Chain.AddToCalcChain(player->chainCalc, TH_CHAIN_PRIO_CALC_PLAYER))
    {
        return false;
    }
    g_Chain.AddToDrawChain(player->chainDraw1, TH_CHAIN_PRIO_DRAW_LOW_PRIO_PLAYER);
    g_Chain.AddToDrawChain(player->chainDraw2, TH_CHAIN_PRIO_DRAW_HIGH_PRIO_PLAYER);
    return true;
}
} // namespace

DIFFABLE_STATIC(Player, g_Player2);
DIFFABLE_STATIC(Player, g_Player);

DIFFABLE_STATIC_ARRAY_ASSIGN(CharacterData, 4, g_CharData) = {
    /* ReimuA  */ {4.0, 2.0, 4.0, 2.0, Player::FireBulletReimuA, Player::FireBulletReimuA},
    /* ReimuB  */ {4.0, 2.0, 4.0, 2.0, Player::FireBulletReimuB, Player::FireBulletReimuB},
    /* MarisaA */ {5.0, 2.5, 5.0, 2.5, Player::FireBulletMarisaA, Player::FireBulletMarisaA},
    /* MarisaB */ {5.0, 2.5, 5.0, 2.5, Player::FireBulletMarisaB, Player::FireBulletMarisaB},
};

Player::Player()
{
}

ZunResult Player::RegisterChain(u8 unk)
{
    if (!ConfigurePlayerChain(&g_Player, unk, 1))
    {
        return ZUN_ERROR;
    }
    memset(&g_Player2, 0, sizeof(Player));
    if (HasSecondPlayer() && !ConfigurePlayerChain(&g_Player2, unk, 2))
    {
        return ZUN_ERROR;
    }
    return ZUN_SUCCESS;
}

void Player::CutChain()
{
    if (g_Player.chainCalc != NULL)
    {
        g_Chain.Cut(g_Player.chainCalc);
        g_Player.chainCalc = NULL;
    }
    if (g_Player.chainDraw1 != NULL)
    {
        g_Chain.Cut(g_Player.chainDraw1);
        g_Player.chainDraw1 = NULL;
    }
    if (g_Player.chainDraw2 != NULL)
    {
        g_Chain.Cut(g_Player.chainDraw2);
        g_Player.chainDraw2 = NULL;
    }
    if (g_Player2.chainCalc != NULL)
    {
        g_Chain.Cut(g_Player2.chainCalc);
        g_Player2.chainCalc = NULL;
    }
    if (g_Player2.chainDraw1 != NULL)
    {
        g_Chain.Cut(g_Player2.chainDraw1);
        g_Player2.chainDraw1 = NULL;
    }
    if (g_Player2.chainDraw2 != NULL)
    {
        g_Chain.Cut(g_Player2.chainDraw2);
        g_Player2.chainDraw2 = NULL;
    }
    return;
}

ZunResult Player::AddedCallback(Player *p)
{
    PlayerBullet *curBullet;
    i32 idx;
    if (HasSecondPlayer() && (i32)(g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER_REINIT) &&
        g_AnmManager->LoadAnm(ANM_FILE_MOD_ANM, "data/mod_anm.anm", ANM_OFFSET_MOD_ANM) != ZUN_SUCCESS)
    {
        return ZUN_ERROR;
    }
    if (HasSecondPlayer())
    {
        g_AnmManager->SetAndExecuteScriptIdx(&p->hitboxSprite, ANM_SCRIPT_HITBOX);
    }

    const Character character = p->playerType == 1 ? (Character)g_GameManager.character : (Character)g_GameManager.character2;
    switch (character)
    {
    case CHARA_REIMU:
        if ((i32)(g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER_REINIT))
        {
            if (p->playerType == 1)
            {
                if (g_AnmManager->LoadAnm(ANM_FILE_PLAYER, "data/player00.anm", ANM_OFFSET_PLAYER) != ZUN_SUCCESS)
                {
                    return ZUN_ERROR;
                }
            }
            else
            {
                const bool wantsAltAnm = g_GameManager.character == g_GameManager.character2;
                const char *primaryPath = wantsAltAnm ? "data/player00b.anm" : "data/player00.anm";
                if (g_AnmManager->LoadAnm(ANM_FILE_PLAYER2, (char *)primaryPath, ANM_OFFSET_PLAYER2) != ZUN_SUCCESS)
                {
                    return ZUN_ERROR;
                }
            }
        }
        g_AnmManager->SetAndExecuteScriptIdx(&p->playerSprite, PlayerIdleScript(p));
        break;
    case CHARA_MARISA:
        if ((i32)(g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER_REINIT))
        {
            if (p->playerType == 1)
            {
                if (g_AnmManager->LoadAnm(ANM_FILE_PLAYER, "data/player01.anm", ANM_OFFSET_PLAYER) != ZUN_SUCCESS)
                {
                    return ZUN_ERROR;
                }
            }
            else
            {
                const bool wantsAltAnm = g_GameManager.character == g_GameManager.character2;
                const char *primaryPath = wantsAltAnm ? "data/player01b.anm" : "data/player01.anm";
                if (g_AnmManager->LoadAnm(ANM_FILE_PLAYER2, (char *)primaryPath, ANM_OFFSET_PLAYER2) != ZUN_SUCCESS)
                {
                    return ZUN_ERROR;
                }
            }
        }
        g_AnmManager->SetAndExecuteScriptIdx(&p->playerSprite, PlayerIdleScript(p));
        break;
    }
    p->positionCenter.x = PlayerSpawnCenterX(p);
    p->positionCenter.y = g_GameManager.arcadeRegionSize.y - 64.0f;
    p->positionCenter.z = 0.49;
    p->orbsPosition[0].z = 0.49;
    p->orbsPosition[1].z = 0.49;
    for (idx = 0; idx < ARRAY_SIZE_SIGNED(p->bombRegionSizes); idx++)
    {
        p->bombRegionSizes[idx].x = 0.0;
    }
    p->hitboxSize.x = 1.25;
    p->hitboxSize.y = 1.25;
    p->hitboxSize.z = 5.0;
    p->grabItemSize.x = 12.0;
    p->grabItemSize.y = 12.0;
    p->grabItemSize.z = 5.0;
    p->playerDirection = MOVEMENT_NONE;
    memcpy(&p->characterData, &g_CharData[CharacterShotTypeForPlayer(p)], sizeof(CharacterData));
    p->characterData.diagonalMovementSpeed = p->characterData.orthogonalMovementSpeed / sqrtf(2.0);
    p->characterData.diagonalMovementSpeedFocus = p->characterData.orthogonalMovementSpeedFocus / sqrtf(2.0);
    p->fireBulletCallback = p->characterData.fireBulletCallback;
    p->fireBulletFocusCallback = p->characterData.fireBulletFocusCallback;
    p->playerState = PLAYER_STATE_SPAWNING;
    p->invulnerabilityTimer.SetCurrent(120);
    p->orbState = ORB_HIDDEN;
    g_AnmManager->SetAndExecuteScriptIdx(&p->orbsSprite[0],
                                         p->playerType == 1 ? ANM_SCRIPT_PLAYER_ORB_LEFT : ANM_SCRIPT_PLAYER_ORB_LEFT2);
    g_AnmManager->SetAndExecuteScriptIdx(&p->orbsSprite[1],
                                         p->playerType == 1 ? ANM_SCRIPT_PLAYER_ORB_RIGHT : ANM_SCRIPT_PLAYER_ORB_RIGHT2);
    for (curBullet = &p->bullets[0], idx = 0; idx < ARRAY_SIZE_SIGNED(p->bullets); idx++, curBullet++)
    {
        curBullet->bulletState = 0;
    }
    p->fireBulletTimer.SetCurrent(-1);
    p->bombInfo.calc = g_BombData[CharacterShotTypeForPlayer(p)].calc;
    p->bombInfo.draw = g_BombData[CharacterShotTypeForPlayer(p)].draw;
    p->bombInfo.isInUse = 0;
    for (idx = 0; idx < ARRAY_SIZE_SIGNED(p->laserTimer); idx++)
    {
        p->laserTimer[idx].InitializeForPopup();
    }
    p->verticalMovementSpeedMultiplierDuringBomb = 1.0;
    p->horizontalMovementSpeedMultiplierDuringBomb = 1.0;
    p->respawnTimer = 8;
    p->hitboxTime = 0;
    p->lifegiveTime = 0;
    return ZUN_SUCCESS;
}

ZunResult Player::DeletedCallback(Player *p)
{
    if ((i32)(g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER_REINIT))
    {
        g_AnmManager->ReleaseAnm(p->playerType == 1 ? ANM_FILE_PLAYER : ANM_FILE_PLAYER2);
    }
    return ZUN_SUCCESS;
}

#pragma var_order(idx, scaleFactor1, scaleFactor2, lastEnemyHit)
ChainCallbackResult Player::OnUpdate(Player *p)
{
    f32 scaleFactor1, scaleFactor2;
    i32 idx;
    D3DXVECTOR3 lastEnemyHit;
    const bool hasSecondPlayer = HasSecondPlayer();
    f32 playersDistance = 999.0f;

    if (g_GameManager.isTimeStopped)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    // In netplay, isolate g_Rng consumption from player processing.
    // Player death spawns power items via SpawnItem(state=2) which calls
    // g_Rng, and BombData also calls g_Rng during bombs — both can differ
    // between peers because each player dies/bombs independently.
    const bool isolateRng = Netplay::IsSessionActive();
    Rng savedRng;
    if (isolateRng)
    {
        savedRng = g_Rng;
    }

    if (hasSecondPlayer)
    {
        playersDistance = sqrtf(g_Player.RangeToPlayer(&g_Player2.positionCenter));
    }
    for (idx = 0; idx < ARRAY_SIZE_SIGNED(p->bombRegionSizes); idx++)
    {
        p->bombRegionSizes[idx].x = 0.0;
    }
    for (idx = 0; idx < ARRAY_SIZE_SIGNED(p->bombProjectiles); idx++)
    {
        p->bombProjectiles[idx].sizeX = 0.0;
    }
    if (p->bombInfo.isInUse)
    {
        p->bombInfo.calc(p);
    }
    else if (!g_Gui.HasCurrentMsgIdx() && p->respawnTimer != 0 &&
             ((p->playerType == 1 && 0 < g_GameManager.bombsRemaining && WAS_PRESSED(TH_BUTTON_BOMB)) ||
              (p->playerType == 2 && 0 < g_GameManager.bombsRemaining2 && WAS_PRESSED(TH_BUTTON_BOMB2))) &&
             p->bombInfo.calc != NULL)
    {
        g_GameManager.bombsUsed++;
        i8 &bombsRemaining = p->playerType == 1 ? g_GameManager.bombsRemaining : g_GameManager.bombsRemaining2;
        bombsRemaining--;
        // thprac: InfBombs — undo the decrement
        if (THPrac::TH06::THPracIsInfBombs())
        {
            bombsRemaining++;
        }
        g_Gui.flags.flag1 = 2;
        p->bombInfo.isInUse = 1;
        p->bombInfo.timer.SetCurrent(0);
        p->bombInfo.duration = 999;
        p->bombInfo.calc(p);
        g_EnemyManager.spellcardInfo.isCapturing = false;
        g_GameManager.DecreaseSubrank(200);
        g_EnemyManager.spellcardInfo.usedBomb = g_EnemyManager.spellcardInfo.isActive;
    }
    if (p->playerState == PLAYER_STATE_DEAD)
    {
        if (p->respawnTimer != 0)
        {
            p->respawnTimer--;
            if (p->respawnTimer == 0)
            {
                g_GameManager.powerItemCountForScore = 0;
                i8 &livesRemaining = p->playerType == 1 ? g_GameManager.livesRemaining : g_GameManager.livesRemaining2;
                u16 &currentPower = p->playerType == 1 ? g_GameManager.currentPower : g_GameManager.currentPower2;
                if (livesRemaining > 0)
                {
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_POWER_BIG, 2);
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_POWER_SMALL, 2);
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_POWER_SMALL, 2);
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_POWER_SMALL, 2);
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_POWER_SMALL, 2);
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_POWER_SMALL, 2);
                    // thprac: InfPower — skip power loss on death
                    if (!THPrac::TH06::THPracIsInfPower())
                    {
                        if (currentPower <= 16)
                        {
                            currentPower = 0;
                        }
                        else
                        {
                            currentPower -= 16;
                        }
                    }
                    g_Gui.flags.flag2 = 2;
                }
                else
                {
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_FULL_POWER, 2);
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_FULL_POWER, 2);
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_FULL_POWER, 2);
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_FULL_POWER, 2);
                    g_ItemManager.SpawnItem(&p->positionCenter, ITEM_FULL_POWER, 2);
                    // thprac: InfPower — skip power reset on game over
                    if (!THPrac::TH06::THPracIsInfPower())
                    {
                        currentPower = 0;
                    }
                    g_Gui.flags.flag2 = 2;
                    g_GameManager.extraLives = 255;
                }
                g_GameManager.DecreaseSubrank(1600);
            }
        }
        else
        {
            scaleFactor1 = p->invulnerabilityTimer.AsFramesFloat() / 30.0f;
            p->playerSprite.scaleY = 3.0f * scaleFactor1 + 1.0f;
            p->playerSprite.scaleX = 1.0f - 1.0f * scaleFactor1;
            p->playerSprite.color =
                COLOR_SET_ALPHA(COLOR_WHITE, (u32)(255.0f - p->invulnerabilityTimer.AsFramesFloat() * 255.0f / 30.0f));
            p->playerSprite.flags.blendMode = AnmVmBlendMode_One;
            p->previousHorizontalSpeed = 0.0f;
            p->previousVerticalSpeed = 0.0f;
            if (p->invulnerabilityTimer.AsFrames() >= 30)
            {
                p->playerState = PLAYER_STATE_SPAWNING;
                p->positionCenter.x = PlayerSpawnCenterX(p);
                p->positionCenter.y = g_GameManager.arcadeRegionSize.y - 64.0f;
                p->positionCenter.z = 0.2;
                p->invulnerabilityTimer.SetCurrent(0);
                p->playerSprite.scaleX = 3.0;
                p->playerSprite.scaleY = 3.0;
                g_AnmManager->SetAndExecuteScriptIdx(&p->playerSprite, PlayerIdleScript(p));
                i8 &livesRemaining = p->playerType == 1 ? g_GameManager.livesRemaining : g_GameManager.livesRemaining2;
                i8 &bombsRemaining = p->playerType == 1 ? g_GameManager.bombsRemaining : g_GameManager.bombsRemaining2;
                if (livesRemaining <= 0)
                {
                    // thprac: InfLives — prevent game over
                    if (THPrac::TH06::THPracIsInfLives() && p->playerType == 1)
                    {
                        livesRemaining = 1;
                    }
                    else
                    {
                        if (Session::IsRemoteNetplaySession())
                        {
                            Netplay::RequestSharedShellEnter(Netplay::SharedShell_Retry);
                        }
                        else
                        {
                            g_GameManager.isInRetryMenu = 1;
                        }
                    }
                }
                else
                {
                    livesRemaining--;
                    // thprac: InfLives — undo the decrement
                    if (THPrac::TH06::THPracIsInfLives() && p->playerType == 1)
                    {
                        livesRemaining++;
                    }
                    g_Gui.flags.flag0 = 2;
                    if (g_GameManager.difficulty < 4 && g_GameManager.isInPracticeMode == 0)
                    {
                        bombsRemaining = g_Supervisor.defaultConfig.bombCount;
                    }
                    else
                    {
                        bombsRemaining = 3;
                    }
                    g_Gui.flags.flag1 = 2;
                    goto spawning;
                }
            }
        }
    }
    else if (p->playerState == PLAYER_STATE_SPAWNING)
    {
    spawning:
        p->bulletGracePeriod = 90;
        scaleFactor2 = 1.0f - p->invulnerabilityTimer.AsFramesFloat() / 30.0f;
        p->playerSprite.scaleY = 2.0f * scaleFactor2 + 1.0f;
        p->playerSprite.scaleX = 1.0f - 1.0f * scaleFactor2;
        p->playerSprite.flags.blendMode = AnmVmBlendMode_One;
        p->verticalMovementSpeedMultiplierDuringBomb = 1.0;
        p->horizontalMovementSpeedMultiplierDuringBomb = 1.0;
        p->playerSprite.color = COLOR_SET_ALPHA(COLOR_WHITE, p->invulnerabilityTimer.AsFrames() * 255 / 30);
        p->respawnTimer = 0;
        if (30 <= p->invulnerabilityTimer.AsFrames())
        {
            p->playerState = PLAYER_STATE_INVULNERABLE;
            p->playerSprite.scaleX = 1.0;
            p->playerSprite.scaleY = 1.0;
            p->playerSprite.color = COLOR_WHITE;
            p->playerSprite.flags.blendMode = AnmVmBlendMode_InvSrcAlpha;
            p->invulnerabilityTimer.SetCurrent(240);
            p->respawnTimer = 6;
        }
    }
    if (p->bulletGracePeriod != 0)
    {
        p->bulletGracePeriod--;
        g_BulletManager.RemoveAllBullets(0);
    }
    if (p->playerState == PLAYER_STATE_INVULNERABLE)
    {
        p->invulnerabilityTimer.Decrement(1);
        if (p->invulnerabilityTimer.AsFrames() <= 0)
        {
            p->playerState = PLAYER_STATE_ALIVE;
            p->invulnerabilityTimer.SetCurrent(0);
            p->playerSprite.flags.colorOp = AnmVmColorOp_Modulate;
            p->playerSprite.color = COLOR_WHITE;
        }
        else if (p->invulnerabilityTimer.AsFrames() % 8 < 2)
        {
            p->playerSprite.flags.colorOp = AnmVmColorOp_Add;
            p->playerSprite.color = 0xff404040;
        }
        else
        {
            p->playerSprite.flags.colorOp = AnmVmColorOp_Modulate;
            p->playerSprite.color = COLOR_WHITE;
        }
    }
    else
    {
        p->invulnerabilityTimer.Tick();
    }
    if (p->playerState != PLAYER_STATE_DEAD && p->playerState != PLAYER_STATE_SPAWNING)
    {
        p->HandlePlayerInputs();
    }
    g_AnmManager->ExecuteScript(&p->playerSprite);
    Player::UpdatePlayerBullets(p);
    if (p->orbState != ORB_HIDDEN)
    {
        g_AnmManager->ExecuteScript(&p->orbsSprite[0]);
        g_AnmManager->ExecuteScript(&p->orbsSprite[1]);
    }
    lastEnemyHit.x = -999.0;
    lastEnemyHit.y = -999.0;
    lastEnemyHit.z = 0.0;
    p->positionOfLastEnemyHit = lastEnemyHit;
    Player::UpdateFireBulletsTimer(p);
    if (hasSecondPlayer)
    {
        if (p->isFocus &&
            playersDistance <= 20.0f &&
            ((p->playerType == 1 && !IS_PRESSED(TH_BUTTON_SHOOT)) ||
             (p->playerType != 1 && !IS_PRESSED(TH_BUTTON_SHOOT2))))
        {
            p->lifegiveTime++;
        }
        else
        {
            p->lifegiveTime = 0;
        }

        if (p->lifegiveTime >= 90)
        {
            p->lifegiveTime = 0;
            if (p->playerType == 1 && g_GameManager.livesRemaining >= 1 && g_GameManager.livesRemaining2 < 8)
            {
                g_Gui.flags.flag0 = 2;
                g_GameManager.livesRemaining--;
                D3DXVECTOR3 spawnPos = p->positionCenter;
                g_ItemManager.SpawnItem(&spawnPos, ITEM_LIFE, 3);
            }
            else if (p->playerType != 1 && g_GameManager.livesRemaining2 >= 1 && g_GameManager.livesRemaining < 8)
            {
                g_Gui.flags.flag0 = 2;
                g_GameManager.livesRemaining2--;
                D3DXVECTOR3 spawnPos = p->positionCenter;
                g_ItemManager.SpawnItem(&spawnPos, ITEM_LIFE, 4);
            }
        }

        p->hitboxTime = p->isFocus ? p->hitboxTime + 1 : 0;
    }
    else
    {
        p->lifegiveTime = 0;
        p->hitboxTime = 0;
    }

    if (isolateRng)
    {
        g_Rng = savedRng;
    }

    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

#pragma var_order(bullet, idx, enemyBottomRight, bulletBottomRight, enemyTopLeft, damage, bulletTopLeft)
i32 Player::CalcDamageToEnemy(D3DXVECTOR3 *enemyPos, D3DXVECTOR3 *enemyHitboxSize, ZunBool *hitWithLazerDuringBomb)
{
    ZunVec3 bulletTopLeft;
    i32 damage;
    ZunVec3 enemyTopLeft;
    i32 idx;
    PlayerBullet *bullet;

    ZunVec3 bulletBottomRight;
    ZunVec3 enemyBottomRight;

    damage = 0;

    ZunVec3::SetVecCorners(&enemyTopLeft, &enemyBottomRight, enemyPos, enemyHitboxSize);
    bullet = &this->bullets[0];
    if (hitWithLazerDuringBomb)
    {
        *hitWithLazerDuringBomb = false;
    }
    for (idx = 0; idx < ARRAY_SIZE_SIGNED(this->bullets); idx++, bullet++)
    {
        if (bullet->bulletState == BULLET_STATE_UNUSED ||
            bullet->bulletState != BULLET_STATE_FIRED && bullet->bulletType != BULLET_TYPE_2)
        {
            continue;
        }

        ZunVec3::SetVecCorners(&bulletTopLeft, &bulletBottomRight, &bullet->position, &bullet->size);

        if (bulletTopLeft.y > enemyBottomRight.y || bulletTopLeft.x > enemyBottomRight.x ||
            bulletBottomRight.y < enemyTopLeft.y || bulletBottomRight.x < enemyTopLeft.x)
        {
            continue;
        }
        /* Bullet is hitting the enemy */
        if (!this->bombInfo.isInUse)
        {
            damage += bullet->damage;
        }
        else
        {
            damage += bullet->damage / 3 != 0 ? bullet->damage / 3 : 1;
        }

        if (bullet->bulletType == BULLET_TYPE_2)
        {
            bullet->damage = bullet->damage / 4;
            if (bullet->damage == 0)
            {
                bullet->damage = 1;
            }
            switch (bullet->sprite.anmFileIndex)
            {
            case ANM_SCRIPT_PLAYER_MARISA_A_ORB_BULLET_1:
                bullet->size.x = 32.0f;
                bullet->size.y = 32.0f;
                break;
            case ANM_SCRIPT_PLAYER_MARISA_A_ORB_BULLET_2:
                bullet->size.x = 42.0f;
                bullet->size.y = 42.0f;
                break;
            case ANM_SCRIPT_PLAYER_MARISA_A_ORB_BULLET_3:
                bullet->size.x = 48.0f;
                bullet->size.y = 48.0f;
                break;
            case ANM_SCRIPT_PLAYER_MARISA_A_ORB_BULLET_4:
                bullet->size.x = 48.0f;
                bullet->size.y = 48.0f;
            }
            if (bullet->unk_140.AsFrames() % 6 == 0)
            {
                g_EffectManager.SpawnParticles(PARTICLE_EFFECT_UNK_5, &bullet->position, 1, COLOR_WHITE);
            }
        }

        if (bullet->bulletType != BULLET_TYPE_LASER)
        {
            if (bullet->bulletState == BULLET_STATE_FIRED)
            {
                g_AnmManager->SetAndExecuteScriptIdx(&bullet->sprite, bullet->sprite.anmFileIndex + 0x20);
                g_EffectManager.SpawnParticles(PARTICLE_EFFECT_UNK_5, &bullet->position, 1, COLOR_WHITE);
                bullet->position.z = 0.1;
            }
            bullet->bulletState = BULLET_STATE_COLLIDED;
            bullet->velocity.x /= 8.0f;
            bullet->velocity.y /= 8.0f;
        }
        else
        {
            this->unk_9e4++;
            if (this->unk_9e4 % 8 == 0)
            {
                *bulletTopLeft.AsD3dXVec() = *enemyPos;
                bulletTopLeft.x = bullet->position.x;

                g_EffectManager.SpawnParticles(PARTICLE_EFFECT_UNK_5, bulletTopLeft.AsD3dXVec(), 1, COLOR_WHITE);
            }
        }
    }
    for (idx = 0; idx < ARRAY_SIZE_SIGNED(this->bombRegionSizes); idx++)
    {
        if (this->bombRegionSizes[idx].x <= 0.0f)
        {
            continue;
        }

        *bulletTopLeft.AsD3dXVec() = this->bombRegionPositions[idx] - this->bombRegionSizes[idx] / 2.0f;
        *bulletBottomRight.AsD3dXVec() = this->bombRegionPositions[idx] + this->bombRegionSizes[idx] / 2.0f;
        if (bulletTopLeft.x > enemyBottomRight.x || bulletBottomRight.x < enemyTopLeft.x ||
            bulletTopLeft.y > enemyBottomRight.y || bulletBottomRight.y < enemyTopLeft.y)
        {
            continue;
        }
        damage += this->bombRegionDamages[idx];
        this->unk_838[idx] += this->bombRegionDamages[idx];
        this->unk_9e4++;
        if (this->unk_9e4 % 4 == 0)
        {
            g_EffectManager.SpawnParticles(PARTICLE_EFFECT_UNK_3, enemyPos, 1, COLOR_WHITE);
        }
        if (this->bombInfo.isInUse && hitWithLazerDuringBomb)
        {
            *hitWithLazerDuringBomb = true;
        }
    }
    return damage;
}

#pragma var_order(vector, idx, vecLength, bullet)
void Player::UpdatePlayerBullets(Player *player)
{
    ZunVec2 vector;
    PlayerBullet *bullet;
    f32 vecLength;
    i32 idx;

    for (idx = 0; idx < ARRAY_SIZE_SIGNED(player->laserTimer); idx++)
    {
        if (player->laserTimer[idx].AsFrames() != 0)
        {
            player->laserTimer[idx].Decrement(1);
        }
    }
    bullet = &player->bullets[0];
    for (idx = 0; idx < ARRAY_SIZE_SIGNED(player->bullets); idx++, bullet++)
    {
        if (bullet->bulletState == BULLET_STATE_UNUSED)
        {
            continue;
        }

        switch (bullet->bulletType)
        {
        case BULLET_TYPE_1:
            if (bullet->bulletState == BULLET_STATE_FIRED)
            {
                if (player->positionOfLastEnemyHit.x > -100.0f && bullet->unk_140.AsFrames() < 40 &&
                    bullet->unk_140.HasTicked())
                {
                    vector.x = player->positionOfLastEnemyHit.x - bullet->position.x;
                    vector.y = player->positionOfLastEnemyHit.y - bullet->position.y;

                    vecLength = vector.VectorLength() / (bullet->unk_134.y / 4.0f);
                    if (vecLength < 1.0f)
                    {
                        vecLength = 1.0f;
                    }

                    vector.x = vector.x / vecLength + bullet->velocity.x;
                    vector.y = vector.y / vecLength + bullet->velocity.y;

                    vecLength = vector.VectorLengthF64();

                    bullet->unk_134.y = ZUN_MIN(vecLength, 10.0f);

                    if (bullet->unk_134.y < 1.0f)
                    {
                        bullet->unk_134.y = 1.0f;
                    }

                    bullet->velocity.x = (vector.x * bullet->unk_134.y) / vecLength;
                    bullet->velocity.y = (vector.y * bullet->unk_134.y) / vecLength;
                }
                else
                {
                    if (bullet->unk_134.y < 10.0f)
                    {
                        bullet->unk_134.y += 0.33333333f;
                        vector.x = bullet->velocity.x;
                        vector.y = bullet->velocity.y;
                        vecLength = vector.VectorLengthF64();
                        bullet->velocity.x = vector.x * bullet->unk_134.y / vecLength;
                        bullet->velocity.y = vector.y * bullet->unk_134.y / vecLength;
                    }
                }
            }

            break;

        case BULLET_TYPE_2:
            if (bullet->bulletState == BULLET_STATE_FIRED)
            {
                bullet->velocity.y -= 0.3f;
            }
            break;
        case BULLET_TYPE_LASER:

            if (player->laserTimer[bullet->unk_152] == 70)
            {
                bullet->sprite.pendingInterrupt = 1;
            }
            else if (player->laserTimer[bullet->unk_152] == 1)
            {
                bullet->sprite.pendingInterrupt = 1;
            }

            bullet->position = player->orbsPosition[bullet->spawnPositionIdx - 1];

            bullet->position.x += bullet->sidewaysMotion;
            bullet->position.y /= 2.0f;
            bullet->position.z = 0.44f;

            bullet->sprite.scaleY = (bullet->position.y * 2) / 14.0f;

            bullet->size.y = bullet->position.y * 2;
            break;
        }

        bullet->sprite.pos.x = bullet->position[0] += bullet->velocity.x * g_Supervisor.effectiveFramerateMultiplier;

        bullet->sprite.pos.y = bullet->position[1] += bullet->velocity.y * g_Supervisor.effectiveFramerateMultiplier;

        bullet->sprite.pos.z = bullet->position.z;
        if (bullet->bulletType != BULLET_TYPE_LASER &&
            !g_GameManager.IsInBounds(bullet->position.x, bullet->position.y, bullet->sprite.sprite->widthPx,
                                      bullet->sprite.sprite->heightPx))
        {
            bullet->bulletState = BULLET_STATE_UNUSED;
        }

        if (g_AnmManager->ExecuteScript(&bullet->sprite))
        {
            bullet->bulletState = BULLET_STATE_UNUSED;
        }
        bullet->unk_140.Tick();
    }
}

ChainCallbackResult Player::OnDrawHighPrio(Player *p)
{
    D3DXVECTOR3 drawPosition = p->positionCenter;
    D3DXVECTOR3 drawOrbs[2] = {p->orbsPosition[0], p->orbsPosition[1]};
    Netplay::AuthoritativePresentation::TryGetRenderOverride(p, drawPosition, drawOrbs);

    Player::DrawBullets(p);
    if (p->bombInfo.isInUse != 0 && p->bombInfo.draw != NULL)
    {
        p->bombInfo.draw(p);
    }
    p->playerSprite.pos.x = g_GameManager.arcadeRegionTopLeftPos.x + drawPosition.x;
    p->playerSprite.pos.y = g_GameManager.arcadeRegionTopLeftPos.y + drawPosition.y;
    p->playerSprite.pos.z = 0.49;
    if (!g_GameManager.isInRetryMenu)
    {
        u32 savedColor = p->playerSprite.color;
        if (HasSecondPlayer() && Session::IsRemoteNetplaySession())
        {
            Player *remotePlayer = Netplay::IsLocalPlayer1() ? (&g_Player2) : (&g_Player);
            if (p == remotePlayer)
            {
                f32 alphaDistance = sqrtf(g_Player.RangeToPlayer(&g_Player2.positionCenter));
                if (alphaDistance < 50.0f)
                {
                    alphaDistance = 50.0f;
                }
                if (alphaDistance < 100.0f)
                {
                    int alpha = (int)(((alphaDistance - 50.0f) / 50.0f) * 200.0f + 55.0f);
                    if (alpha > 255)
                    {
                        alpha = 255;
                    }
                    if (alpha < 0)
                    {
                        alpha = 0;
                    }
                    p->playerSprite.color = COLOR_SET_ALPHA(p->playerSprite.color, alpha);
                }
            }
        }
        g_AnmManager->DrawNoRotation(&p->playerSprite);
        p->playerSprite.color = savedColor;
        if (HasSecondPlayer() && p->hitboxTime != 0)
        {
            float hitboxScale1 = MInterpolation(p->hitboxTime / 18.0f, 1.5f, 1.0f);
            float hitboxScale2 = MInterpolation(p->hitboxTime / 12.0f, 0.3f, 1.0f);
            float hitboxAngle1;
            float hitboxAngle2;
            int hitboxAlpha = (int)((p->hitboxTime < 6.0f ? p->hitboxTime / 6.0f : 1.0f) * 255.0f);
            if (hitboxAlpha > 255)
            {
                hitboxAlpha = 255;
            }

            if (p->hitboxTime < 18.0f)
            {
                hitboxAngle1 = MInterpolation(p->hitboxTime / 18.0f, 3.14159f, -3.14159f);
                hitboxAngle2 = -hitboxAngle1;
            }
            else
            {
                hitboxAngle1 = -3.14159f + p->hitboxTime * 0.05235988f;
                hitboxAngle2 = 3.14159f - p->hitboxTime * 0.05235988f;
            }

            p->hitboxSprite.pos.x = g_GameManager.arcadeRegionTopLeftPos.x + drawPosition.x;
            p->hitboxSprite.pos.y = g_GameManager.arcadeRegionTopLeftPos.y + drawPosition.y;
            p->hitboxSprite.pos.z = 0.49f;
            p->hitboxSprite.color = COLOR_SET_ALPHA(p->hitboxSprite.color, hitboxAlpha);

            if (Session::IsRemoteNetplaySession())
            {
                Player *localPlayer = Netplay::IsLocalPlayer1() ? (&g_Player) : (&g_Player2);
                Player *remotePlayer = Netplay::IsLocalPlayer1() ? (&g_Player2) : (&g_Player);
                if (p == remotePlayer)
                {
                    f32 alphaDistance = sqrtf(remotePlayer->RangeToPlayer(&localPlayer->positionCenter));
                    if (alphaDistance < 50.0f)
                    {
                        alphaDistance = 50.0f;
                    }
                    if (alphaDistance < 100.0f)
                    {
                        int alpha = (int)(((alphaDistance - 50.0f) / 50.0f) * 220.0f + 35.0f);
                        if (alpha > 255)
                        {
                            alpha = 255;
                        }
                        if (alpha < 0)
                        {
                            alpha = 0;
                        }
                        if (alpha < (int)(p->hitboxSprite.color >> 24))
                        {
                            p->hitboxSprite.color = COLOR_SET_ALPHA(p->hitboxSprite.color, alpha);
                        }
                    }
                }
            }

            p->hitboxSprite.rotation.z = hitboxAngle1;
            p->hitboxSprite.scaleX = hitboxScale1;
            p->hitboxSprite.scaleY = hitboxScale1;
            g_AnmManager->Draw(&p->hitboxSprite);
            p->hitboxSprite.rotation.z = hitboxAngle2;
            p->hitboxSprite.scaleX = hitboxScale2;
            p->hitboxSprite.scaleY = hitboxScale2;
            g_AnmManager->Draw(&p->hitboxSprite);
        }
        if (p->orbState != ORB_HIDDEN &&
            (p->playerState == PLAYER_STATE_ALIVE || p->playerState == PLAYER_STATE_INVULNERABLE))
        {
            p->orbsSprite[0].pos = drawOrbs[0];
            p->orbsSprite[1].pos = drawOrbs[1];
            p->orbsSprite[0].pos[0] += g_GameManager.arcadeRegionTopLeftPos.x;
            p->orbsSprite[0].pos[1] += g_GameManager.arcadeRegionTopLeftPos.y;
            p->orbsSprite[1].pos[0] += g_GameManager.arcadeRegionTopLeftPos.x;
            p->orbsSprite[1].pos[1] += g_GameManager.arcadeRegionTopLeftPos.y;
            p->orbsSprite[0].pos.z = 0.491;
            p->orbsSprite[1].pos.z = 0.491;
            g_AnmManager->Draw(&p->orbsSprite[0]);
            g_AnmManager->Draw(&p->orbsSprite[1]);
        }
    }
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult Player::OnDrawLowPrio(Player *p)
{
    Player::DrawBulletExplosions(p);
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

#pragma var_order(playerDirection, verticalSpeed, horizontalSpeed, verticalOrbOffset, horizontalOrbOffset,             \
                  intermediateFloat)
ZunResult Player::HandlePlayerInputs()
{
    float intermediateFloat;

    float horizontalOrbOffset;
    float verticalOrbOffset;

    float horizontalSpeed = 0.0;
    float verticalSpeed = 0.0;
    PlayerDirection playerDirection = this->playerDirection;

    this->playerDirection = MOVEMENT_NONE;
    const bool upPressed = this->playerType == 1 ? IS_PRESSED(TH_BUTTON_UP) : IS_PRESSED(TH_BUTTON_UP2);
    const bool downPressed = this->playerType == 1 ? IS_PRESSED(TH_BUTTON_DOWN) : IS_PRESSED(TH_BUTTON_DOWN2);
    const bool leftPressed = this->playerType == 1 ? IS_PRESSED(TH_BUTTON_LEFT) : IS_PRESSED(TH_BUTTON_LEFT2);
    const bool rightPressed = this->playerType == 1 ? IS_PRESSED(TH_BUTTON_RIGHT) : IS_PRESSED(TH_BUTTON_RIGHT2);
    const bool focusPressed = this->playerType == 1 ? IS_PRESSED(TH_BUTTON_FOCUS) : IS_PRESSED(TH_BUTTON_FOCUS2);
    const bool shootPressed = this->playerType == 1 ? IS_PRESSED(TH_BUTTON_SHOOT) : IS_PRESSED(TH_BUTTON_SHOOT2);

    if (upPressed)
    {
        this->playerDirection = MOVEMENT_UP;
        if (leftPressed)
        {
            this->playerDirection = MOVEMENT_UP_LEFT;
        }
        if (rightPressed)
        {
            this->playerDirection = MOVEMENT_UP_RIGHT;
        }
    }
    else
    {
        if (downPressed)
        {
            this->playerDirection = MOVEMENT_DOWN;
            if (leftPressed)
            {
                this->playerDirection = MOVEMENT_DOWN_LEFT;
            }
            if (rightPressed)
            {
                this->playerDirection = MOVEMENT_DOWN_RIGHT;
            }
        }
        else
        {
            if (leftPressed)
            {
                this->playerDirection = MOVEMENT_LEFT;
            }
            if (rightPressed)
            {
                this->playerDirection = MOVEMENT_RIGHT;
            }
        }
    }
    if (focusPressed)
    {
        this->isFocus = true;
    }
    else
    {
        this->isFocus = false;
    }

    switch (this->playerDirection)
    {
    case MOVEMENT_RIGHT:
        if (focusPressed)
        {
            horizontalSpeed = this->characterData.orthogonalMovementSpeedFocus;
        }
        else
        {
            horizontalSpeed = this->characterData.orthogonalMovementSpeed;
        }
        break;
    case MOVEMENT_LEFT:
        if (focusPressed)
        {
            horizontalSpeed = -this->characterData.orthogonalMovementSpeedFocus;
        }
        else
        {
            horizontalSpeed = -this->characterData.orthogonalMovementSpeed;
        }
        break;
    case MOVEMENT_UP:
        if (focusPressed)
        {
            verticalSpeed = -this->characterData.orthogonalMovementSpeedFocus;
        }
        else
        {
            verticalSpeed = -this->characterData.orthogonalMovementSpeed;
        }
        break;
    case MOVEMENT_DOWN:
        if (focusPressed)
        {
            verticalSpeed = this->characterData.orthogonalMovementSpeedFocus;
        }
        else
        {
            verticalSpeed = this->characterData.orthogonalMovementSpeed;
        }
        break;
    case MOVEMENT_UP_LEFT:
        if (focusPressed)
        {
            horizontalSpeed = -this->characterData.diagonalMovementSpeedFocus;
        }
        else
        {
            horizontalSpeed = -this->characterData.diagonalMovementSpeed;
        }
        verticalSpeed = horizontalSpeed;
        break;
    case MOVEMENT_DOWN_LEFT:
        if (focusPressed)
        {
            horizontalSpeed = -this->characterData.diagonalMovementSpeedFocus;
        }
        else
        {
            horizontalSpeed = -this->characterData.diagonalMovementSpeed;
        }
        verticalSpeed = -horizontalSpeed;
        break;
    case MOVEMENT_UP_RIGHT:
        if (focusPressed)
        {
            horizontalSpeed = this->characterData.diagonalMovementSpeedFocus;
        }
        else
        {
            horizontalSpeed = this->characterData.diagonalMovementSpeed;
        }
        verticalSpeed = -horizontalSpeed;
        break;
    case MOVEMENT_DOWN_RIGHT:
        if (focusPressed)
        {
            horizontalSpeed = this->characterData.diagonalMovementSpeedFocus;
        }
        else
        {
            horizontalSpeed = this->characterData.diagonalMovementSpeed;
        }
        verticalSpeed = horizontalSpeed;
    }

    if (horizontalSpeed < 0.0f && this->previousHorizontalSpeed >= 0.0f)
    {
        g_AnmManager->SetAndExecuteScriptIdx(&this->playerSprite, PlayerMoveLeftScript(this));
    }
    else if (!horizontalSpeed && this->previousHorizontalSpeed < 0.0f)
    {
        g_AnmManager->SetAndExecuteScriptIdx(&this->playerSprite, PlayerStopLeftScript(this));
    }

    if (horizontalSpeed > 0.0f && this->previousHorizontalSpeed <= 0.0f)
    {
        g_AnmManager->SetAndExecuteScriptIdx(&this->playerSprite, PlayerMoveRightScript(this));
    }
    else if (!horizontalSpeed && this->previousHorizontalSpeed > 0.0f)
    {
        g_AnmManager->SetAndExecuteScriptIdx(&this->playerSprite, PlayerStopRightScript(this));
    }

    this->previousHorizontalSpeed = horizontalSpeed;
    this->previousVerticalSpeed = verticalSpeed;

    // TODO: Match stack variables here
    this->positionCenter[0] +=
        horizontalSpeed * this->horizontalMovementSpeedMultiplierDuringBomb * g_Supervisor.effectiveFramerateMultiplier;
    this->positionCenter[1] +=
        verticalSpeed * this->verticalMovementSpeedMultiplierDuringBomb * g_Supervisor.effectiveFramerateMultiplier;

    if (this->positionCenter.x < g_GameManager.playerMovementAreaTopLeftPos.x)
    {
        this->positionCenter.x = g_GameManager.playerMovementAreaTopLeftPos.x;
    }
    else if (g_GameManager.playerMovementAreaTopLeftPos.x + g_GameManager.playerMovementAreaSize.x <
             this->positionCenter.x)
    {
        this->positionCenter.x = g_GameManager.playerMovementAreaTopLeftPos.x + g_GameManager.playerMovementAreaSize.x;
    }

    if (this->positionCenter.y < g_GameManager.playerMovementAreaTopLeftPos.y)
    {
        this->positionCenter.y = g_GameManager.playerMovementAreaTopLeftPos.y;
    }
    else if (g_GameManager.playerMovementAreaTopLeftPos.y + g_GameManager.playerMovementAreaSize.y <
             this->positionCenter.y)
    {
        this->positionCenter.y = g_GameManager.playerMovementAreaTopLeftPos.y + g_GameManager.playerMovementAreaSize.y;
    }

    this->hitboxTopLeft = this->positionCenter - this->hitboxSize;

    this->hitboxBottomRight = this->positionCenter + this->hitboxSize;

    this->grabItemTopLeft = this->positionCenter - this->grabItemSize;

    this->grabItemBottomRight = this->positionCenter + this->grabItemSize;

    this->orbsPosition[0] = this->positionCenter;
    this->orbsPosition[1] = this->positionCenter;

    verticalOrbOffset = 0.0;
    horizontalOrbOffset = verticalOrbOffset;

    const u16 currentPower = this->playerType == 1 ? g_GameManager.currentPower : g_GameManager.currentPower2;
    if (currentPower < 8)
    {
        this->orbState = ORB_HIDDEN;
    }
    else if (this->orbState == ORB_HIDDEN)
    {
        this->orbState = ORB_UNFOCUSED;
    }

    switch (this->orbState)
    {
    case ORB_HIDDEN:
        this->focusMovementTimer.InitializeForPopup();
        break;

    case ORB_UNFOCUSED:
        horizontalOrbOffset = 24.0;
        this->focusMovementTimer.InitializeForPopup();
        if (this->isFocus)
        {
            this->orbState = ORB_FOCUSING;
        }
        else
        {
            break;
        }

    CASE_ORB_FOCUSING:
    case ORB_FOCUSING:
        this->focusMovementTimer.Tick();

        intermediateFloat = this->focusMovementTimer.AsFramesFloat() / 8.0f;
        verticalOrbOffset = (1.0f - intermediateFloat) * 32.0f + -32.0f;
        intermediateFloat *= intermediateFloat;
        horizontalOrbOffset = -16.0f * intermediateFloat + 24.0f;

        if ((ZunBool)(this->focusMovementTimer.current >= 8))
        {
            this->orbState = ORB_FOCUSED;
        }
        if (!this->isFocus)
        {

            this->orbState = ORB_UNFOCUSING;
            this->focusMovementTimer.SetCurrent(8 - this->focusMovementTimer.AsFrames());

            goto CASE_ORB_UNFOCUSING;
        }
        else
        {
            break;
        }

    case ORB_FOCUSED:
        horizontalOrbOffset = 8.0;
        verticalOrbOffset = -32.0;
        this->focusMovementTimer.InitializeForPopup();
        if (!this->isFocus)
        {
            this->orbState = ORB_UNFOCUSING;
        }
        else
        {
            break;
        }

    CASE_ORB_UNFOCUSING:
    case ORB_UNFOCUSING:
        this->focusMovementTimer.Tick();

        intermediateFloat = this->focusMovementTimer.AsFramesFloat() / 8.0f;
        verticalOrbOffset = (32.0f * intermediateFloat) + -32.0f;
        intermediateFloat *= intermediateFloat;
        intermediateFloat = 1.0f - intermediateFloat;
        horizontalOrbOffset = -16.0f * intermediateFloat + 24.0f;
        if ((ZunBool)(this->focusMovementTimer.current >= 8))
        {
            this->orbState = ORB_UNFOCUSED;
        }
        if (this->isFocus)
        {
            this->orbState = ORB_FOCUSING;
            this->focusMovementTimer.SetCurrent(8 - this->focusMovementTimer.AsFrames());
            goto CASE_ORB_FOCUSING;
        }
    }

    this->orbsPosition[0].x -= horizontalOrbOffset;
    this->orbsPosition[1].x += horizontalOrbOffset;
    this->orbsPosition[0].y += verticalOrbOffset;
    this->orbsPosition[1].y += verticalOrbOffset;
    if (shootPressed && !g_Gui.HasCurrentMsgIdx())
    {
        this->StartFireBulletTimer(this);
    }
    this->previousFrameInput = g_CurFrameInput;
    return ZUN_SUCCESS;
}

#pragma var_order(bulletIdx, bullets)
void Player::DrawBullets(Player *p)
{
    i32 bulletIdx;
    PlayerBullet *bullets;

    bullets = p->bullets;
    for (bulletIdx = 0; bulletIdx < ARRAY_SIZE_SIGNED(p->bullets); bulletIdx++, bullets++)
    {
        if (bullets->bulletState != BULLET_STATE_FIRED)
        {
            continue;
        }
        if (bullets->sprite.autoRotate)
        {
            bullets->sprite.rotation.z = ZUN_PI / 2 - utils::AddNormalizeAngle(bullets->unk_134.z, ZUN_PI);
        }
        g_AnmManager->Draw2(&bullets->sprite);
    }
}

#pragma var_order(bulletIdx, bullets)
void Player::DrawBulletExplosions(Player *p)
{
    i32 bulletIdx;
    PlayerBullet *bullets;

    bullets = p->bullets;
    for (bulletIdx = 0; bulletIdx < ARRAY_SIZE_SIGNED(p->bullets); bulletIdx++, bullets++)
    {
        if (bullets->bulletState != BULLET_STATE_COLLIDED)
        {
            continue;
        }
        if (bullets->sprite.autoRotate)
        {
            bullets->sprite.rotation.z = ZUN_PI / 2 - utils::AddNormalizeAngle(bullets->unk_134.z, ZUN_PI);
        }
        bullets->sprite.pos.z = 0.4f;
        g_AnmManager->Draw2(&bullets->sprite);
    }
}

void Player::StartFireBulletTimer(Player *p)
{
    if (p->fireBulletTimer.AsFrames() < 0)
    {
        p->fireBulletTimer.InitializeForPopup();
    }
}

ZunResult Player::UpdateFireBulletsTimer(Player *p)
{
    if (p->fireBulletTimer.AsFrames() < 0)
    {
        return ZUN_SUCCESS;
    }

    if (p->playerType == 1)
    {
        if (p->fireBulletTimer.HasTicked() && (!g_Player.bombInfo.isInUse || g_GameManager.character != CHARA_MARISA ||
                                               g_GameManager.shotType != SHOT_TYPE_B))
        {
            p->SpawnBullets(p, p->fireBulletTimer.AsFrames());
        }
    }
    else
    {
        if (p->fireBulletTimer.HasTicked() &&
            (!g_Player2.bombInfo.isInUse || g_GameManager.character2 != CHARA_MARISA ||
             g_GameManager.shotType2 != SHOT_TYPE_B))
        {
            p->SpawnBullets(p, p->fireBulletTimer.AsFrames());
        }
    }

    p->fireBulletTimer.Tick();

    if (p->fireBulletTimer.AsFrames() >= 30 || p->playerState == PLAYER_STATE_DEAD ||
        p->playerState == PLAYER_STATE_SPAWNING)
    {
        p->fireBulletTimer.SetCurrent(-1);
    }
    return ZUN_SUCCESS;
}

#pragma var_order(relY, relX)
f32 Player::AngleFromPlayer(D3DXVECTOR3 *pos)
{
    f32 relX;
    f32 relY;

    relX = pos->x - this->positionCenter.x;
    relY = pos->y - this->positionCenter.y;
    if (relY == 0.0f && relX == 0.0f)
    {
        return ZUN_PI / 2;
    }
    return atan2f(relY, relX);
}

#pragma var_order(relY, relX)
f32 Player::AngleToPlayer(D3DXVECTOR3 *pos)
{
    f32 relX;
    f32 relY;

    relX = this->positionCenter.x - pos->x;
    relY = this->positionCenter.y - pos->y;
    if (relY == 0.0f && relX == 0.0f)
    {
        // Shoot down. An angle of 0 means to the right, and the angle goes
        // clockwise.
        return RADIANS(90.0f);
    }
    return atan2f(relY, relX);
}

f32 Player::RangeToPlayer(D3DXVECTOR3 *pos)
{
    const D3DXVECTOR3 vecToPlayer(pos->x - this->positionCenter.x, pos->y - this->positionCenter.y, 0.0f);
    return D3DXVec3LengthSq(&vecToPlayer);
}

#pragma var_order(idx, curBulletIdx, curBullet, bulletResult)
void Player::SpawnBullets(Player *p, u32 timer)
{
    FireBulletResult bulletResult;
    PlayerBullet *curBullet;
    i32 curBulletIdx;
    u32 idx;

    idx = 0;
    curBullet = p->bullets;

    for (curBulletIdx = 0; curBulletIdx < ARRAY_SIZE_SIGNED(p->bullets); curBulletIdx++, curBullet++)
    {
        if (curBullet->bulletState != BULLET_STATE_UNUSED)
        {
            continue;
        }
    WHILE_LOOP:
        if (!p->isFocus)
        {
            bulletResult = (*p->fireBulletCallback)(p, curBullet, idx, timer);
        }
        else
        {
            bulletResult = (*p->fireBulletFocusCallback)(p, curBullet, idx, timer);
        }
        if (bulletResult >= 0)
        {
            curBullet->sprite.pos.x = curBullet->position.x;
            curBullet->sprite.pos.y = curBullet->position.y;
            curBullet->sprite.pos.z = 0.495;
            curBullet->bulletState = BULLET_STATE_FIRED;
        }
        if (bulletResult == FBR_STOP_SPAWNING)
        {
            return;
        }
        if (bulletResult > 0)
        {
            return;
        }
        idx++;
        if (bulletResult == FBR_SPAWN_MORE)
        {
            goto WHILE_LOOP;
        }
    }
}

#pragma var_order(bulletData, bulletFrame, unused3, unused, unused2)
FireBulletResult Player::FireSingleBullet(Player *player, PlayerBullet *bullet, i32 bulletIdx,
                                          i32 framesSinceLastBullet, CharacterPowerData *powerData)
{
    CharacterPowerBulletData *bulletData;
    i32 bulletFrame;
    i32 unused;
    i32 unused2;
    i32 unused3;

    while ((player->playerType == 1 ? g_GameManager.currentPower : g_GameManager.currentPower2) >= powerData->power)
    {
        powerData++;
    }

    bulletData = powerData->bullets + bulletIdx;

    if (bulletData->bulletType == BULLET_TYPE_LASER)
    {
        bulletFrame = bulletData->bulletFrame;
        if (!player->laserTimer[bulletFrame].AsFrames())
        {
            player->laserTimer[bulletFrame].SetCurrent(bulletData->waitBetweenBullets);

            bullet->unk_152 = bulletFrame;
            bullet->spawnPositionIdx = bulletData->spawnPositionIdx;
            bullet->sidewaysMotion = bulletData->motion.x;
            bullet->unk_134.x = bulletData->motion.y;
            goto SHOOT_BULLET;
        }
    }
    else if (framesSinceLastBullet % bulletData->waitBetweenBullets == bulletData->bulletFrame)
    {
    SHOOT_BULLET:

        g_AnmManager->SetAndExecuteScriptIdx(
            &bullet->sprite, player->playerType == 1 ? bulletData->anmFileIdx : ANM_OFFSET_PLAYER_DIFFERENCE + bulletData->anmFileIdx);
        if (!bulletData->spawnPositionIdx)
        {
            bullet->position = player->positionCenter;
        }
        else
        {
            bullet->position = player->orbsPosition[bulletData->spawnPositionIdx - 1];
        }
        bullet->position[0] += bulletData->motion.x;
        bullet->position[1] += bulletData->motion.y;

        bullet->position.z = 0.495f;

        bullet->size.x = bulletData->size.x;
        bullet->size.y = bulletData->size.y;
        bullet->size.z = 1.0f;
        bullet->unk_134.z = bulletData->direction;
        bullet->unk_134.y = bulletData->velocity;

        bullet->velocity.x = cosf(bulletData->direction) * bulletData->velocity;

        bullet->velocity.y = sinf(bulletData->direction) * bulletData->velocity;

        bullet->unk_140.InitializeForPopup();

        bullet->bulletType = bulletData->bulletType;
        bullet->damage = bulletData->damage;
        if (bulletData->bulletSoundIdx >= 0)
        {
            g_SoundPlayer.PlaySoundByIdx((SoundIdx)bulletData->bulletSoundIdx, 0);
        }

        return bulletIdx >= powerData->numBullets - 1;
    }

    if (bulletIdx >= powerData->numBullets - 1)
    {
        return FBR_STOP_SPAWNING;
    }
    else
    {
        return FBR_SPAWN_MORE;
    }
}

FireBulletResult Player::FireBulletReimuA(Player *player, PlayerBullet *bullet, u32 bulletIdx,
                                          u32 framesSinceLastBullet)
{
    return player->FireSingleBullet(player, bullet, bulletIdx, framesSinceLastBullet, g_CharacterPowerDataReimuA);
}

FireBulletResult Player::FireBulletReimuB(Player *player, PlayerBullet *bullet, u32 bulletIdx,
                                          u32 framesSinceLastBullet)
{
    return player->FireSingleBullet(player, bullet, bulletIdx, framesSinceLastBullet, g_CharacterPowerDataReimuB);
}

FireBulletResult Player::FireBulletMarisaA(Player *player, PlayerBullet *bullet, u32 bulletIdx,
                                           u32 framesSinceLastBullet)
{
    return player->FireSingleBullet(player, bullet, bulletIdx, framesSinceLastBullet, g_CharacterPowerDataMarisaA);
}

FireBulletResult Player::FireBulletMarisaB(Player *player, PlayerBullet *bullet, u32 bulletIdx,
                                           u32 framesSinceLastBullet)
{
    return player->FireSingleBullet(player, bullet, bulletIdx, framesSinceLastBullet, g_CharacterPowerDataMarisaB);
}

#pragma var_order(bombTopLeft, i, bulletBottomRight, bulletTopLeft, bombProjectile, bombBottomRight)
i32 Player::CheckGraze(D3DXVECTOR3 *center, D3DXVECTOR3 *size)
{
    D3DXVECTOR3 bombBottomRight;
    PlayerRect *bombProjectile;
    D3DXVECTOR3 bombTopLeft;
    D3DXVECTOR3 bulletBottomRight;
    D3DXVECTOR3 bulletTopLeft;
    i32 i;

    bulletTopLeft.x = center->x - size->x / 2.0f - 20.0f;
    bulletTopLeft.y = center->y - size->y / 2.0f - 20.0f;
    bulletBottomRight.x = center->x + size->x / 2.0f + 20.0f;
    bulletBottomRight.y = center->y + size->y / 2.0f + 20.0f;
    bombProjectile = this->bombProjectiles;

    for (i = 0; i < ARRAY_SIZE_SIGNED(this->bombProjectiles); i++, bombProjectile++)
    {
        if (bombProjectile->sizeX == 0.0f)
        {
            continue;
        }

        bombTopLeft.x = bombProjectile->posX - bombProjectile->sizeX / 2.0f;
        bombTopLeft.y = bombProjectile->posY - bombProjectile->sizeY / 2.0f;
        bombBottomRight.x = bombProjectile->sizeX / 2.0f + bombProjectile->posX;
        bombBottomRight.y = bombProjectile->sizeY / 2.0f + bombProjectile->posY;

        // Bomb clips bullet's hitbox, destroys bullet upon return
        if (!(bombTopLeft.x > bulletBottomRight.x || bombBottomRight.x < bulletTopLeft.x ||
              bombTopLeft.y > bulletBottomRight.y || bombBottomRight.y < bulletTopLeft.y))
        {
            return 2;
        }
    }

    if (this->playerState == PLAYER_STATE_DEAD || this->playerState == PLAYER_STATE_SPAWNING)
    {
        return 0;
    }
    if (this->hitboxTopLeft.x > bulletBottomRight.x || this->hitboxBottomRight.x < bulletTopLeft.x ||
        this->hitboxTopLeft.y > bulletBottomRight.y || this->hitboxBottomRight.y < bulletTopLeft.y)
    {
        return 0;
    }

    // Bullet clips player's graze hitbox, add score and check for death upon return
    this->ScoreGraze(center);
    return 1;
}

#pragma var_order(padding1, bombProjectileTop, bombProjectileLeft, curBombIdx, padding2, bulletBottom, bulletRight,    \
                  padding3, bulletTop, bulletLeft, curBombProjectile, padding4, bombProjectileBottom,                  \
                  bombProjectileRight)
i32 Player::CalcKillBoxCollision(D3DXVECTOR3 *bulletCenter, D3DXVECTOR3 *bulletSize)
{
    PlayerRect *curBombProjectile;
    f32 bulletLeft, bulletTop, bulletRight, bulletBottom;
    f32 bombProjectileLeft, bombProjectileTop, bombProjectileRight, bombProjectileBottom;
    i32 curBombIdx;
    i32 padding1, padding2, padding3, padding4;

    curBombProjectile = this->bombProjectiles;
    bulletLeft = bulletCenter->x - bulletSize->x / 2.0f;
    bulletTop = bulletCenter->y - bulletSize->y / 2.0f;
    bulletRight = bulletCenter->x + bulletSize->x / 2.0f;
    bulletBottom = bulletCenter->y + bulletSize->y / 2.0f;
    for (curBombIdx = 0; curBombIdx < ARRAY_SIZE_SIGNED(this->bombProjectiles); curBombIdx++, curBombProjectile++)
    {
        if (curBombProjectile->sizeX == 0.0f)
        {
            continue;
        }
        bombProjectileLeft = curBombProjectile->posX - curBombProjectile->sizeX / 2.0f;
        bombProjectileTop = curBombProjectile->posY - curBombProjectile->sizeY / 2.0f;
        bombProjectileRight = curBombProjectile->posX + curBombProjectile->sizeX / 2.0f;
        bombProjectileBottom = curBombProjectile->posY + curBombProjectile->sizeY / 2.0f;
        if (!(bombProjectileLeft > bulletRight || bombProjectileRight < bulletLeft ||
              bombProjectileTop > bulletBottom || bombProjectileBottom < bulletTop))
        {
            return 2;
        }
    }
    if (this->hitboxTopLeft.x > bulletRight || this->hitboxTopLeft.y > bulletBottom ||
        this->hitboxBottomRight.x < bulletLeft || this->hitboxBottomRight.y < bulletTop)
    {
        return 0;
    }
    else if (this->playerState != PLAYER_STATE_ALIVE)
    {
        return 1;
    }
    else
    {
        this->Die();
        return 1;
    }
}

i32 Player::ProbeKillBoxCollision(const D3DXVECTOR3 *bulletCenter, const D3DXVECTOR3 *bulletSize) const
{
    const f32 bulletLeft = bulletCenter->x - bulletSize->x / 2.0f;
    const f32 bulletTop = bulletCenter->y - bulletSize->y / 2.0f;
    const f32 bulletRight = bulletCenter->x + bulletSize->x / 2.0f;
    const f32 bulletBottom = bulletCenter->y + bulletSize->y / 2.0f;

    for (int curBombIdx = 0; curBombIdx < ARRAY_SIZE_SIGNED(this->bombProjectiles); ++curBombIdx)
    {
        const PlayerRect &projectile = this->bombProjectiles[curBombIdx];
        if (projectile.sizeX == 0.0f)
        {
            continue;
        }
        const f32 bombProjectileLeft = projectile.posX - projectile.sizeX / 2.0f;
        const f32 bombProjectileTop = projectile.posY - projectile.sizeY / 2.0f;
        const f32 bombProjectileRight = projectile.posX + projectile.sizeX / 2.0f;
        const f32 bombProjectileBottom = projectile.posY + projectile.sizeY / 2.0f;
        if (!(bombProjectileLeft > bulletRight || bombProjectileRight < bulletLeft ||
              bombProjectileTop > bulletBottom || bombProjectileBottom < bulletTop))
        {
            return PROBE_COLLISION_GRAZE_OR_BOMB;
        }
    }

    if (this->hitboxTopLeft.x > bulletRight || this->hitboxTopLeft.y > bulletBottom ||
        this->hitboxBottomRight.x < bulletLeft || this->hitboxBottomRight.y < bulletTop)
    {
        return PROBE_COLLISION_NONE;
    }

    return PROBE_COLLISION_HIT;
}

#pragma var_order(playerRelativeTopLeft, laserBottomRight, laserTopLeft, playerRelativeBottomRight)
i32 Player::CalcLaserHitbox(D3DXVECTOR3 *laserCenter, D3DXVECTOR3 *laserSize, D3DXVECTOR3 *rotation, f32 angle,
                            i32 canGraze)
{
    D3DXVECTOR3 laserTopLeft;
    D3DXVECTOR3 laserBottomRight;
    D3DXVECTOR3 playerRelativeTopLeft;
    D3DXVECTOR3 playerRelativeBottomRight;

    laserTopLeft = this->positionCenter - *rotation;
    utils::Rotate(&laserBottomRight, &laserTopLeft, angle);
    laserBottomRight.z = 0;
    laserTopLeft = laserBottomRight + *rotation;
    playerRelativeTopLeft = laserTopLeft - this->hitboxSize;
    playerRelativeBottomRight = laserTopLeft + this->hitboxSize;

    laserTopLeft = *laserCenter - *laserSize / 2.0f;
    laserBottomRight = *laserCenter + *laserSize / 2.0f;

    if (!(playerRelativeTopLeft.x > laserBottomRight.x || playerRelativeBottomRight.x < laserTopLeft.x ||
          playerRelativeTopLeft.y > laserBottomRight.y || playerRelativeBottomRight.y < laserTopLeft.y))
    {
        goto LASER_COLLISION;
    }
    if (canGraze == 0)
    {
        return 0;
    }

    laserTopLeft.x -= 48.0f;
    laserTopLeft.y -= 48.0f;
    laserBottomRight.x += 48.0f;
    laserBottomRight.y += 48.0f;

    if (playerRelativeTopLeft.x > laserBottomRight.x || playerRelativeBottomRight.x < laserTopLeft.x ||
        playerRelativeTopLeft.y > laserBottomRight.y || playerRelativeBottomRight.y < laserTopLeft.y)
    {
        return 0;
    }
    if (this->playerState == PLAYER_STATE_DEAD || this->playerState == PLAYER_STATE_SPAWNING)
    {
        return 0;
    }

    this->ScoreGraze(&this->positionCenter);
    return 2;

LASER_COLLISION:
    if (this->playerState != PLAYER_STATE_ALIVE)
    {
        return 0;
    }

    this->Die();
    return 1;
}

i32 Player::ProbeLaserHitbox(const D3DXVECTOR3 *laserCenter, const D3DXVECTOR3 *laserSize, const D3DXVECTOR3 *rotation,
                             f32 angle, i32 canGraze) const
{
    D3DXVECTOR3 laserTopLeft;
    D3DXVECTOR3 laserBottomRight;
    D3DXVECTOR3 playerRelativeTopLeft;
    D3DXVECTOR3 playerRelativeBottomRight;

    laserTopLeft = this->positionCenter - *rotation;
    utils::Rotate(&laserBottomRight, &laserTopLeft, angle);
    laserBottomRight.z = 0;
    laserTopLeft = laserBottomRight + *rotation;
    playerRelativeTopLeft = laserTopLeft - this->hitboxSize;
    playerRelativeBottomRight = laserTopLeft + this->hitboxSize;

    laserTopLeft = *laserCenter - *laserSize / 2.0f;
    laserBottomRight = *laserCenter + *laserSize / 2.0f;

    if (!(playerRelativeTopLeft.x > laserBottomRight.x || playerRelativeBottomRight.x < laserTopLeft.x ||
          playerRelativeTopLeft.y > laserBottomRight.y || playerRelativeBottomRight.y < laserTopLeft.y))
    {
        return PROBE_COLLISION_HIT;
    }
    if (canGraze == 0)
    {
        return PROBE_COLLISION_NONE;
    }

    laserTopLeft.x -= 48.0f;
    laserTopLeft.y -= 48.0f;
    laserBottomRight.x += 48.0f;
    laserBottomRight.y += 48.0f;

    if (playerRelativeTopLeft.x > laserBottomRight.x || playerRelativeBottomRight.x < laserTopLeft.x ||
        playerRelativeTopLeft.y > laserBottomRight.y || playerRelativeBottomRight.y < laserTopLeft.y)
    {
        return PROBE_COLLISION_NONE;
    }
    if (this->playerState == PLAYER_STATE_DEAD || this->playerState == PLAYER_STATE_SPAWNING)
    {
        return PROBE_COLLISION_NONE;
    }

    return PROBE_COLLISION_GRAZE_OR_BOMB;
}

#pragma var_order(itemBottomRight, itemTopLeft)
i32 Player::CalcItemBoxCollision(D3DXVECTOR3 *itemCenter, D3DXVECTOR3 *itemSize)
{
    if (this->playerState != PLAYER_STATE_ALIVE && this->playerState != PLAYER_STATE_INVULNERABLE)
    {
        return 0;
    }
    D3DXVECTOR3 itemTopLeft;
    memcpy(&itemTopLeft, &(*itemCenter - *itemSize / 2.0f), sizeof(D3DXVECTOR3));
    D3DXVECTOR3 itemBottomRight;
    memcpy(&itemBottomRight, &(*itemCenter + *itemSize / 2.0f), sizeof(D3DXVECTOR3));

    if (this->grabItemTopLeft.x > itemBottomRight.x || this->grabItemBottomRight.x < itemTopLeft.x ||
        this->grabItemTopLeft.y > itemBottomRight.y || this->grabItemBottomRight.y < itemTopLeft.y)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

void Player::ScoreGraze(D3DXVECTOR3 *center)
{
    D3DXVECTOR3 particlePosition;

    if (g_Player.bombInfo.isInUse == 0)
    {
        if (g_GameManager.grazeInStage < 9999)
        {
            g_GameManager.grazeInStage++;
        }
        if (g_GameManager.grazeInTotal < 999999)
        {
            g_GameManager.grazeInTotal++;
        }
    }

    particlePosition = (this->positionCenter + *center) / 2.0f;
    g_EffectManager.SpawnParticles(PARTICLE_EFFECT_UNK_8, &particlePosition, 1, COLOR_WHITE);
    g_GameManager.AddScore(500);
    g_GameManager.IncreaseSubrank(6);
    g_Gui.flags.flag3 = 2;
    g_SoundPlayer.PlaySoundByIdx(SOUND_GRAZE, 0);
}

#pragma var_order(curLaserTimerIdx)
void Player::Die()
{
    int curLaserTimerIdx;
    i8 &bombsRemaining = this->playerType == 1 ? g_GameManager.bombsRemaining : g_GameManager.bombsRemaining2;

    // thprac: AutoBomb — automatically use a bomb instead of dying
    if (THPrac::TH06::THPracIsAutoBomb() && this->playerType == 1 && bombsRemaining > 0 && this->bombInfo.calc != NULL)
    {
        g_GameManager.bombsUsed++;
        bombsRemaining--;
        g_Gui.flags.flag1 = 2;
        this->bombInfo.isInUse = 1;
        this->bombInfo.timer.SetCurrent(0);
        this->bombInfo.duration = 999;
        this->bombInfo.calc(this);
        g_EnemyManager.spellcardInfo.isCapturing = false;
        g_GameManager.DecreaseSubrank(200);
        g_EnemyManager.spellcardInfo.usedBomb = g_EnemyManager.spellcardInfo.isActive;
        return;
    }

    // thprac: Muteki — skip death entirely
    if (THPrac::TH06::THPracIsMuteki())
        return;

    // thprac: count miss
    THPrac::TH06::THPracCountMiss();

    g_EnemyManager.spellcardInfo.isCapturing = 0;
    g_EffectManager.SpawnParticles(PARTICLE_EFFECT_UNK_12, &this->positionCenter, 1, COLOR_NEONBLUE);
    g_EffectManager.SpawnParticles(PARTICLE_EFFECT_UNK_6, &this->positionCenter, 16, COLOR_WHITE);
    this->playerState = PLAYER_STATE_DEAD;
    this->invulnerabilityTimer.InitializeForPopup();
    g_SoundPlayer.PlaySoundByIdx(SOUND_PICHUN, 0);
    g_GameManager.deaths++;
    for (curLaserTimerIdx = 0; curLaserTimerIdx < ARRAY_SIZE_SIGNED(this->laserTimer); curLaserTimerIdx++)
    {
        this->laserTimer[curLaserTimerIdx].SetCurrent(2);
    }
    return;
}
}; // namespace th06
