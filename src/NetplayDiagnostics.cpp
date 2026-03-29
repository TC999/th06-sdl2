#include "NetplayInternal.hpp"
#include "thprac_th06.h"

namespace th06::Netplay
{
const char *ControlToString(Control control)
{
    switch (control)
    {
    case Ctrl_No_Ctrl:
        return "none";
    case Ctrl_Start_Game:
        return "start_game";
    case Ctrl_Key:
        return "key";
    case Ctrl_Set_InitSetting:
        return "init_setting";
    case Ctrl_Try_Resync:
        return "try_resync";
    case Ctrl_Debug_EndingJump:
        return "debug_ending_jump";
    case Ctrl_UiPhase:
        return "ui_phase";
    case Ctrl_ShellIntent:
        return "shell_intent";
    case Ctrl_ShellState:
        return "shell_state";
    default:
        return "unknown";
    }
}

const char *InGameCtrlToString(InGameCtrlType ctrl)
{
    switch (ctrl)
    {
    case Quick_Quit:
        return "quick_quit";
    case Quick_Restart:
        return "quick_restart";
    case Inf_Life:
        return "inf_life";
    case Inf_Bomb:
        return "inf_bomb";
    case Inf_Power:
        return "inf_power";
    case Add_Delay:
        return "add_delay";
    case Dec_Delay:
        return "dec_delay";
    case IGC_NONE:
        return "none";
    default:
        return "unknown";
    }
}

const char *SessionRoleTag()
{
    if (g_State.isHost)
    {
        return "host";
    }
    if (g_State.isGuest)
    {
        return "guest";
    }
    return "idle";
}

void AppendButtonName(char *buffer, size_t size, size_t &offset, bool active, const char *name)
{
    if (!active || buffer == nullptr || size == 0 || offset >= size - 1)
    {
        return;
    }

    const int written =
        std::snprintf(buffer + offset, size - offset, offset == 0 ? "%s" : "|%s", name);
    if (written <= 0)
    {
        return;
    }

    const size_t consumed = (size_t)written;
    offset = consumed >= size - offset ? size - 1 : offset + consumed;
}

std::string FormatInputBits(u16 input)
{
    char names[160] = {};
    size_t offset = 0;
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_SHOOT) != 0, "shoot");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_BOMB) != 0, "bomb");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_FOCUS) != 0, "focus");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_MENU) != 0, "menu");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_UP) != 0, "up");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_DOWN) != 0, "down");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_LEFT) != 0, "left");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_RIGHT) != 0, "right");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_SKIP) != 0, "skip");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_SHOOT2) != 0, "shoot2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_BOMB2) != 0, "bomb2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_FOCUS2) != 0, "focus2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_UP2) != 0, "up2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_DOWN2) != 0, "down2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_LEFT2) != 0, "left2");
    AppendButtonName(names, sizeof(names), offset, (input & TH_BUTTON_RIGHT2) != 0, "right2");
    if (offset == 0)
    {
        std::snprintf(names, sizeof(names), "none");
    }

    char summary[192] = {};
    std::snprintf(summary, sizeof(summary), "0x%04X[%s]", input, names);
    return summary;
}

std::string BuildCtrlPacketWindowSummary(const CtrlPack &ctrl, int count)
{
    std::ostringstream stream;
    const int frameCount = std::max(0, std::min(count, kKeyPackFrameCount));
    for (int i = 0; i < frameCount; ++i)
    {
        if (i != 0)
        {
            stream << ' ';
        }
        stream << (ctrl.frame - i) << ':' << FormatInputBits(WriteToInt(ctrl.keys[i])) << '/'
               << ctrl.rngSeed[i] << '/' << InGameCtrlToString(ctrl.inGameCtrl[i]);
    }
    return stream.str();
}

std::string BuildRuntimeStateSummary()
{
    const bool inGameManager = g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER;
    const bool isInGameMenu = inGameManager && g_GameManager.isInGameMenu;
    const bool isInRetryMenu = inGameManager && g_GameManager.isInRetryMenu;
    const bool isInUi = !inGameManager || isInGameMenu || isInRetryMenu;

    char summary[640] = {};
    std::snprintf(summary, sizeof(summary),
                  "role=%s sup=%d gf=%d ui=%d menu=%d retry=%d net=%d last=%d delay=%d sync=%d conn=%d reconn=%d "
                  "desyncTick=%llu rb=%d pend=%d tgt=%d send=%d stall=%d ctrl=%s lastIn=%s curIn=%s status=%s",
                  SessionRoleTag(), (int)g_Supervisor.curState, inGameManager ? g_GameManager.gameFrames : -1,
                  isInUi ? 1 : 0, isInGameMenu ? 1 : 0, isInRetryMenu ? 1 : 0, g_State.currentNetFrame,
                  g_State.lastFrame, g_State.delay, g_State.isSync ? 1 : 0, g_State.isConnected ? 1 : 0,
                  g_State.isTryingReconnect ? 1 : 0, (unsigned long long)g_State.desyncStartTick,
                  g_State.rollbackActive ? 1 : 0,
                  g_State.pendingRollbackFrame, g_State.rollbackTargetFrame, g_State.rollbackSendFrame,
                  g_State.stallFrameRequested ? 1 : 0, InGameCtrlToString(g_State.currentCtrl),
                  FormatInputBits(g_LastFrameInput).c_str(), FormatInputBits(g_CurFrameInput).c_str(),
                  g_State.statusText.c_str());
    return summary;
}

class DiagnosticHashBuilder
{
public:
    void AddBytes(const void *data, size_t size)
    {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i)
        {
            m_value ^= (uint64_t)bytes[i];
            m_value *= 1099511628211ull;
        }
    }

    template <typename T> void AddRaw(const T &value)
    {
        AddBytes(&value, sizeof(value));
    }

    void AddBool(bool value)
    {
        const unsigned char raw = value ? 1 : 0;
        AddRaw(raw);
    }

    void AddFloat(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        AddRaw(bits);
    }

    void AddDouble(double value)
    {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        AddRaw(bits);
    }

    uint64_t Finish() const
    {
        return m_value;
    }

private:
    uint64_t m_value = 1469598103934665603ull;
};

template <typename T> void HashArray(DiagnosticHashBuilder &hash, const T *data, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        hash.AddRaw(data[i]);
    }
}

template <typename T> int NormalizeArrayIndex(const T *ptr, const T *base, size_t count)
{
    if (ptr == nullptr)
    {
        return -1;
    }
    if (base == nullptr || count == 0)
    {
        return -2;
    }

    const uintptr_t ptrValue = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t baseValue = reinterpret_cast<uintptr_t>(base);
    const uintptr_t span = count * sizeof(T);
    if (ptrValue < baseValue || ptrValue >= baseValue + span)
    {
        return -2;
    }

    const uintptr_t delta = ptrValue - baseValue;
    if (delta % sizeof(T) != 0)
    {
        return -2;
    }

    return (int)(delta / sizeof(T));
}

std::intptr_t NormalizeByteOffset(const void *base, const void *ptr)
{
    if (ptr == nullptr)
    {
        return -1;
    }
    if (base == nullptr)
    {
        return -2;
    }

    const uintptr_t ptrValue = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t baseValue = reinterpret_cast<uintptr_t>(base);
    if (ptrValue < baseValue)
    {
        return -2;
    }

    return (std::intptr_t)(ptrValue - baseValue);
}

void HashTimer(DiagnosticHashBuilder &hash, const ZunTimer &timer)
{
    hash.AddRaw(timer.previous);
    hash.AddFloat(timer.subFrame);
    hash.AddRaw(timer.current);
}

void HashVec2(DiagnosticHashBuilder &hash, const D3DXVECTOR2 &vec)
{
    hash.AddFloat(vec.x);
    hash.AddFloat(vec.y);
}

void HashVec3(DiagnosticHashBuilder &hash, const D3DXVECTOR3 &vec)
{
    hash.AddFloat(vec.x);
    hash.AddFloat(vec.y);
    hash.AddFloat(vec.z);
}

void HashQuaternion(DiagnosticHashBuilder &hash, const D3DXQUATERNION &quat)
{
    hash.AddFloat(quat.x);
    hash.AddFloat(quat.y);
    hash.AddFloat(quat.z);
    hash.AddFloat(quat.w);
}

void HashAnmVm(DiagnosticHashBuilder &hash, const AnmVm &vm)
{
    HashVec3(hash, vm.rotation);
    HashVec3(hash, vm.angleVel);
    hash.AddFloat(vm.scaleY);
    hash.AddFloat(vm.scaleX);
    hash.AddFloat(vm.scaleInterpFinalY);
    hash.AddFloat(vm.scaleInterpFinalX);
    HashVec2(hash, vm.uvScrollPos);
    HashTimer(hash, vm.currentTimeInScript);
    hash.AddBytes(&vm.matrix, sizeof(vm.matrix));
    hash.AddRaw(vm.color);
    hash.AddRaw(vm.flags.flags);
    hash.AddRaw(vm.alphaInterpEndTime);
    hash.AddRaw(vm.scaleInterpEndTime);
    hash.AddRaw(vm.autoRotate);
    hash.AddRaw(vm.pendingInterrupt);
    hash.AddRaw(vm.posInterpEndTime);
    HashVec3(hash, vm.pos);
    hash.AddFloat(vm.scaleInterpInitialY);
    hash.AddFloat(vm.scaleInterpInitialX);
    HashTimer(hash, vm.scaleInterpTime);
    hash.AddRaw(vm.activeSpriteIndex);
    hash.AddRaw(vm.baseSpriteIndex);
    hash.AddRaw(vm.anmFileIndex);
    hash.AddRaw((int64_t)NormalizeByteOffset(vm.beginingOfScript, vm.currentInstruction));
    hash.AddRaw(vm.sprite != nullptr ? vm.sprite->spriteId : -1);
    hash.AddRaw(vm.alphaInterpInitial);
    hash.AddRaw(vm.alphaInterpFinal);
    HashVec3(hash, vm.posInterpInitial);
    HashVec3(hash, vm.posInterpFinal);
    HashVec3(hash, vm.posOffset);
    HashTimer(hash, vm.posInterpTime);
    hash.AddRaw(vm.timeOfLastSpriteSet);
    HashTimer(hash, vm.alphaInterpTime);
    hash.AddRaw(vm.fontWidth);
    hash.AddRaw(vm.fontHeight);
}

void HashPlayerBullet(DiagnosticHashBuilder &hash, const PlayerBullet &bullet)
{
    hash.AddRaw(bullet.bulletState);
    if (bullet.bulletState == BULLET_STATE_UNUSED)
    {
        return;
    }

    HashAnmVm(hash, bullet.sprite);
    HashVec3(hash, bullet.position);
    HashVec3(hash, bullet.size);
    HashVec2(hash, bullet.velocity);
    hash.AddFloat(bullet.sidewaysMotion);
    HashVec3(hash, bullet.unk_134);
    HashTimer(hash, bullet.unk_140);
    hash.AddRaw(bullet.damage);
    hash.AddRaw(bullet.bulletType);
    hash.AddRaw(bullet.unk_152);
    hash.AddRaw(bullet.spawnPositionIdx);
}

void HashPlayerBombInfo(DiagnosticHashBuilder &hash, const PlayerBombInfo &bombInfo)
{
    hash.AddRaw(bombInfo.isInUse);
    if (bombInfo.isInUse == 0)
    {
        return;
    }

    hash.AddRaw(bombInfo.duration);
    HashTimer(hash, bombInfo.timer);
    HashArray(hash, bombInfo.reimuABombProjectilesState, std::size(bombInfo.reimuABombProjectilesState));
    for (float value : bombInfo.reimuABombProjectilesRelated)
    {
        hash.AddFloat(value);
    }
    for (const D3DXVECTOR3 &vec : bombInfo.bombRegionPositions)
    {
        HashVec3(hash, vec);
    }
    for (const D3DXVECTOR3 &vec : bombInfo.bombRegionVelocities)
    {
        HashVec3(hash, vec);
    }
    for (const auto &spriteRow : bombInfo.sprites)
    {
        for (const AnmVm &vm : spriteRow)
        {
            HashAnmVm(hash, vm);
        }
    }
}

uint64_t HashPlayerState(const Player &player)
{
    DiagnosticHashBuilder hash;
    HashAnmVm(hash, player.playerSprite);
    for (const AnmVm &vm : player.orbsSprite)
    {
        HashAnmVm(hash, vm);
    }
    HashVec3(hash, player.positionCenter);
    HashVec3(hash, player.unk_44c);
    HashVec3(hash, player.hitboxTopLeft);
    HashVec3(hash, player.hitboxBottomRight);
    HashVec3(hash, player.grabItemTopLeft);
    HashVec3(hash, player.grabItemBottomRight);
    HashVec3(hash, player.hitboxSize);
    HashVec3(hash, player.grabItemSize);
    for (const D3DXVECTOR3 &vec : player.orbsPosition)
    {
        HashVec3(hash, vec);
    }
    for (const D3DXVECTOR3 &vec : player.bombRegionPositions)
    {
        HashVec3(hash, vec);
    }
    for (const D3DXVECTOR3 &vec : player.bombRegionSizes)
    {
        HashVec3(hash, vec);
    }
    HashArray(hash, player.bombRegionDamages, std::size(player.bombRegionDamages));
    HashArray(hash, player.unk_838, std::size(player.unk_838));
    hash.AddBytes(player.bombProjectiles, sizeof(player.bombProjectiles));
    for (const ZunTimer &timer : player.laserTimer)
    {
        HashTimer(hash, timer);
    }
    hash.AddFloat(player.horizontalMovementSpeedMultiplierDuringBomb);
    hash.AddFloat(player.verticalMovementSpeedMultiplierDuringBomb);
    hash.AddRaw(player.respawnTimer);
    hash.AddRaw(player.bulletGracePeriod);
    hash.AddRaw(player.playerState);
    hash.AddRaw(player.playerType);
    hash.AddRaw(player.unk_9e1);
    hash.AddRaw(player.orbState);
    hash.AddRaw(player.isFocus);
    hash.AddRaw(player.unk_9e4);
    HashTimer(hash, player.focusMovementTimer);
    hash.AddFloat(player.characterData.orthogonalMovementSpeed);
    hash.AddFloat(player.characterData.orthogonalMovementSpeedFocus);
    hash.AddFloat(player.characterData.diagonalMovementSpeed);
    hash.AddFloat(player.characterData.diagonalMovementSpeedFocus);
    hash.AddRaw(player.playerDirection);
    hash.AddFloat(player.previousHorizontalSpeed);
    hash.AddFloat(player.previousVerticalSpeed);
    hash.AddRaw(player.previousFrameInput);
    HashVec3(hash, player.positionOfLastEnemyHit);
    for (const PlayerBullet &bullet : player.bullets)
    {
        HashPlayerBullet(hash, bullet);
    }
    HashTimer(hash, player.fireBulletTimer);
    HashTimer(hash, player.invulnerabilityTimer);
    HashPlayerBombInfo(hash, player.bombInfo);
    HashAnmVm(hash, player.hitboxSprite);
    hash.AddRaw(player.hitboxTime);
    hash.AddRaw(player.lifegiveTime);
    return hash.Finish();
}

void HashBulletTypeSprites(DiagnosticHashBuilder &hash, const BulletTypeSprites &sprites)
{
    HashAnmVm(hash, sprites.spriteBullet);
    HashAnmVm(hash, sprites.spriteSpawnEffectFast);
    HashAnmVm(hash, sprites.spriteSpawnEffectNormal);
    HashAnmVm(hash, sprites.spriteSpawnEffectSlow);
    HashAnmVm(hash, sprites.spriteSpawnEffectDonut);
    HashVec3(hash, sprites.grazeSize);
    hash.AddRaw(sprites.unk_55c);
    hash.AddRaw(sprites.bulletHeight);
}

void HashBullet(DiagnosticHashBuilder &hash, const Bullet &bullet)
{
    hash.AddRaw(bullet.state);
    if (bullet.state == 0)
    {
        return;
    }

    HashBulletTypeSprites(hash, bullet.sprites);
    HashVec3(hash, bullet.pos);
    HashVec3(hash, bullet.velocity);
    HashVec3(hash, bullet.ex4Acceleration);
    hash.AddFloat(bullet.speed);
    hash.AddFloat(bullet.ex5Float0);
    hash.AddFloat(bullet.dirChangeSpeed);
    hash.AddFloat(bullet.angle);
    hash.AddFloat(bullet.ex5Float1);
    hash.AddFloat(bullet.dirChangeRotation);
    HashTimer(hash, bullet.timer);
    hash.AddRaw(bullet.ex5Int0);
    hash.AddRaw(bullet.dirChangeInterval);
    hash.AddRaw(bullet.dirChangeNumTimes);
    hash.AddRaw(bullet.dirChangeMaxTimes);
    hash.AddRaw(bullet.exFlags);
    hash.AddRaw(bullet.spriteOffset);
    hash.AddRaw(bullet.unk_5bc);
    hash.AddRaw(bullet.unk_5c0);
    hash.AddRaw(bullet.unk_5c2);
    hash.AddRaw(bullet.isGrazed);
    hash.AddRaw(bullet.provokedPlayer);
}

void HashLaser(DiagnosticHashBuilder &hash, const Laser &laser)
{
    hash.AddRaw(laser.inUse);
    if (laser.inUse == 0)
    {
        return;
    }

    HashAnmVm(hash, laser.vm0);
    HashAnmVm(hash, laser.vm1);
    HashVec3(hash, laser.pos);
    hash.AddFloat(laser.angle);
    hash.AddFloat(laser.startOffset);
    hash.AddFloat(laser.endOffset);
    hash.AddFloat(laser.startLength);
    hash.AddFloat(laser.width);
    hash.AddFloat(laser.speed);
    hash.AddRaw(laser.startTime);
    hash.AddRaw(laser.hitboxStartTime);
    hash.AddRaw(laser.duration);
    hash.AddRaw(laser.despawnDuration);
    hash.AddRaw(laser.hitboxEndDelay);
    HashTimer(hash, laser.timer);
    hash.AddRaw(laser.flags);
    hash.AddRaw(laser.color);
    hash.AddRaw(laser.state);
    hash.AddRaw(laser.provokedPlayer);
}

uint64_t HashBulletManagerState()
{
    DiagnosticHashBuilder hash;
    hash.AddRaw(g_BulletManager.nextBulletIndex);
    hash.AddRaw(g_BulletManager.bulletCount);
    HashTimer(hash, g_BulletManager.time);
    for (const Bullet &bullet : g_BulletManager.bullets)
    {
        HashBullet(hash, bullet);
    }
    for (const Laser &laser : g_BulletManager.lasers)
    {
        HashLaser(hash, laser);
    }
    return hash.Finish();
}

void HashEnemyContext(DiagnosticHashBuilder &hash, const EnemyEclContext &context)
{
    hash.AddRaw((int64_t)NormalizeByteOffset(g_EclManager.eclFile, context.currentInstr));
    HashTimer(hash, context.time);
    hash.AddRaw(context.var0);
    hash.AddRaw(context.var1);
    hash.AddRaw(context.var2);
    hash.AddRaw(context.var3);
    hash.AddFloat(context.float0);
    hash.AddFloat(context.float1);
    hash.AddFloat(context.float2);
    hash.AddFloat(context.float3);
    hash.AddRaw(context.var4);
    hash.AddRaw(context.var5);
    hash.AddRaw(context.var6);
    hash.AddRaw(context.var7);
    hash.AddRaw(context.compareRegister);
    hash.AddRaw(context.subId);
}

uint64_t HashEnemyState(const Enemy &enemy)
{
    DiagnosticHashBuilder hash;
    hash.AddBytes(&enemy.flags, sizeof(enemy.flags));
    if (!enemy.flags.unk5)
    {
        return hash.Finish();
    }

    HashAnmVm(hash, enemy.primaryVm);
    for (const AnmVm &vm : enemy.vms)
    {
        HashAnmVm(hash, vm);
    }
    HashEnemyContext(hash, enemy.currentContext);
    for (const EnemyEclContext &context : enemy.savedContextStack)
    {
        HashEnemyContext(hash, context);
    }
    hash.AddRaw(enemy.stackDepth);
    hash.AddRaw(enemy.unk_c40);
    hash.AddRaw(enemy.deathCallbackSub);
    HashArray(hash, enemy.interrupts, std::size(enemy.interrupts));
    hash.AddRaw(enemy.runInterrupt);
    HashVec3(hash, enemy.position);
    HashVec3(hash, enemy.hitboxDimensions);
    HashVec3(hash, enemy.axisSpeed);
    hash.AddFloat(enemy.angle);
    hash.AddFloat(enemy.angularVelocity);
    hash.AddFloat(enemy.speed);
    hash.AddFloat(enemy.acceleration);
    HashVec3(hash, enemy.shootOffset);
    HashVec3(hash, enemy.moveInterp);
    HashVec3(hash, enemy.moveInterpStartPos);
    HashTimer(hash, enemy.moveInterpTimer);
    hash.AddRaw(enemy.moveInterpStartTime);
    hash.AddFloat(enemy.bulletRankSpeedLow);
    hash.AddFloat(enemy.bulletRankSpeedHigh);
    hash.AddRaw(enemy.bulletRankAmount1Low);
    hash.AddRaw(enemy.bulletRankAmount1High);
    hash.AddRaw(enemy.bulletRankAmount2Low);
    hash.AddRaw(enemy.bulletRankAmount2High);
    hash.AddRaw(enemy.life);
    hash.AddRaw(enemy.maxLife);
    hash.AddRaw(enemy.score);
    HashTimer(hash, enemy.bossTimer);
    hash.AddRaw(enemy.color);
    hash.AddBytes(&enemy.bulletProps, sizeof(enemy.bulletProps));
    hash.AddRaw(enemy.shootInterval);
    HashTimer(hash, enemy.shootIntervalTimer);
    hash.AddBytes(&enemy.laserProps, sizeof(enemy.laserProps));
    for (const Laser *laser : enemy.lasers)
    {
        hash.AddRaw(NormalizeArrayIndex(laser, g_BulletManager.lasers, std::size(g_BulletManager.lasers)));
    }
    hash.AddRaw(enemy.laserStore);
    hash.AddRaw(enemy.deathAnm1);
    hash.AddRaw(enemy.deathAnm2);
    hash.AddRaw(enemy.deathAnm3);
    hash.AddRaw(enemy.itemDrop);
    hash.AddRaw(enemy.bossId);
    hash.AddRaw(enemy.unk_e41);
    HashTimer(hash, enemy.exInsFunc10Timer);
    hash.AddRaw(enemy.anmExFlags);
    hash.AddRaw(enemy.anmExDefaults);
    hash.AddRaw(enemy.anmExFarLeft);
    hash.AddRaw(enemy.anmExFarRight);
    hash.AddRaw(enemy.anmExLeft);
    hash.AddRaw(enemy.anmExRight);
    HashVec2(hash, enemy.lowerMoveLimit);
    HashVec2(hash, enemy.upperMoveLimit);
    for (const Effect *effect : enemy.effectArray)
    {
        hash.AddRaw(NormalizeArrayIndex(effect, g_EffectManager.effects, std::size(g_EffectManager.effects)));
    }
    hash.AddRaw(enemy.effectIdx);
    hash.AddFloat(enemy.effectDistance);
    hash.AddRaw(enemy.lifeCallbackThreshold);
    hash.AddRaw(enemy.lifeCallbackSub);
    hash.AddRaw(enemy.timerCallbackThreshold);
    hash.AddRaw(enemy.timerCallbackSub);
    hash.AddFloat(enemy.exInsFunc6Angle);
    HashTimer(hash, enemy.exInsFunc6Timer);
    hash.AddRaw(enemy.provokedPlayer);
    return hash.Finish();
}

uint64_t HashEnemyManagerState()
{
    DiagnosticHashBuilder hash;
    hash.AddRaw(g_EnemyManager.randomItemSpawnIndex);
    hash.AddRaw(g_EnemyManager.randomItemTableIndex);
    hash.AddRaw(g_EnemyManager.enemyCount);
    hash.AddBytes(&g_EnemyManager.spellcardInfo, sizeof(g_EnemyManager.spellcardInfo));
    hash.AddRaw(g_EnemyManager.unk_ee5d8);
    hash.AddRaw((int64_t)NormalizeByteOffset(g_EclManager.timeline, g_EnemyManager.timelineInstr));
    HashTimer(hash, g_EnemyManager.timelineTime);
    hash.AddRaw(HashEnemyState(g_EnemyManager.enemyTemplate));
    for (const Enemy &enemy : g_EnemyManager.enemies)
    {
        hash.AddRaw(HashEnemyState(enemy));
    }
    for (const Enemy *boss : g_EnemyManager.bosses)
    {
        hash.AddRaw(NormalizeArrayIndex(boss, g_EnemyManager.enemies, std::size(g_EnemyManager.enemies)));
    }
    return hash.Finish();
}

void HashItem(DiagnosticHashBuilder &hash, const Item &item)
{
    hash.AddRaw(item.isInUse);
    if (!item.isInUse)
    {
        return;
    }

    HashAnmVm(hash, item.sprite);
    HashVec3(hash, item.currentPosition);
    HashVec3(hash, item.startPosition);
    HashVec3(hash, item.targetPosition);
    HashTimer(hash, item.timer);
    hash.AddRaw(item.itemType);
    hash.AddRaw(item.unk_142);
    hash.AddRaw(item.state);
}

uint64_t HashItemManagerState()
{
    DiagnosticHashBuilder hash;
    hash.AddRaw(g_ItemManager.nextIndex);
    hash.AddRaw(g_ItemManager.itemCount);
    for (const Item &item : g_ItemManager.items)
    {
        HashItem(hash, item);
    }
    return hash.Finish();
}

void HashEffect(DiagnosticHashBuilder &hash, const Effect &effect)
{
    hash.AddRaw(effect.inUseFlag);
    if (!effect.inUseFlag)
    {
        return;
    }

    HashAnmVm(hash, effect.vm);
    HashVec3(hash, effect.pos1);
    HashVec3(hash, effect.unk_11c);
    HashVec3(hash, effect.unk_128);
    HashVec3(hash, effect.position);
    HashVec3(hash, effect.pos2);
    HashQuaternion(hash, effect.quaternion);
    hash.AddFloat(effect.unk_15c);
    hash.AddFloat(effect.angleRelated);
    HashTimer(hash, effect.timer);
    hash.AddRaw(effect.unk_170);
    hash.AddRaw(effect.effectId);
    hash.AddRaw(effect.unk_17a);
    hash.AddRaw(effect.unk_17b);
}

uint64_t HashEffectManagerState()
{
    DiagnosticHashBuilder hash;
    hash.AddRaw(g_EffectManager.nextIndex);
    hash.AddRaw(g_EffectManager.activeEffects);
    for (const Effect &effect : g_EffectManager.effects)
    {
        HashEffect(hash, effect);
    }
    return hash.Finish();
}

uint64_t HashStageState()
{
    DiagnosticHashBuilder hash;
    hash.AddRaw(g_Stage.quadCount);
    hash.AddRaw(g_Stage.objectsCount);
    HashTimer(hash, g_Stage.scriptTime);
    hash.AddRaw(g_Stage.instructionIndex);
    HashTimer(hash, g_Stage.timer);
    hash.AddRaw(g_Stage.stage);
    HashVec3(hash, g_Stage.position);
    hash.AddBytes(&g_Stage.skyFog, sizeof(g_Stage.skyFog));
    hash.AddBytes(&g_Stage.skyFogInterpInitial, sizeof(g_Stage.skyFogInterpInitial));
    hash.AddBytes(&g_Stage.skyFogInterpFinal, sizeof(g_Stage.skyFogInterpFinal));
    hash.AddRaw(g_Stage.skyFogInterpDuration);
    HashTimer(hash, g_Stage.skyFogInterpTimer);
    hash.AddRaw(g_Stage.skyFogNeedsSetup);
    hash.AddRaw(g_Stage.spellcardState);
    hash.AddRaw(g_Stage.ticksSinceSpellcardStarted);
    HashAnmVm(hash, g_Stage.spellcardBackground);
    HashAnmVm(hash, g_Stage.unk2);
    hash.AddRaw(g_Stage.unpauseFlag);
    HashVec3(hash, g_Stage.facingDirInterpInitial);
    HashVec3(hash, g_Stage.facingDirInterpFinal);
    hash.AddRaw(g_Stage.facingDirInterpDuration);
    HashTimer(hash, g_Stage.facingDirInterpTimer);
    HashVec3(hash, g_Stage.positionInterpFinal);
    hash.AddRaw(g_Stage.positionInterpEndTime);
    HashVec3(hash, g_Stage.positionInterpInitial);
    hash.AddRaw(g_Stage.positionInterpStartTime);

    if (g_Stage.objectInstances != nullptr && g_Stage.objectsCount > 0)
    {
        hash.AddBytes(g_Stage.objectInstances, (size_t)g_Stage.objectsCount * sizeof(RawStageObjectInstance));
    }
    if (g_Stage.quadVms != nullptr && g_Stage.quadCount > 0)
    {
        for (int i = 0; i < g_Stage.quadCount; ++i)
        {
            HashAnmVm(hash, g_Stage.quadVms[i]);
        }
    }
    return hash.Finish();
}

uint64_t HashGameManagerState()
{
    DiagnosticHashBuilder hash;
    hash.AddRaw(g_GameManager.guiScore);
    hash.AddRaw(g_GameManager.score);
    hash.AddRaw(g_GameManager.nextScoreIncrement);
    hash.AddRaw(g_GameManager.highScore);
    hash.AddRaw(g_GameManager.difficulty);
    hash.AddRaw(g_GameManager.grazeInStage);
    hash.AddRaw(g_GameManager.grazeInTotal);
    hash.AddRaw(g_GameManager.deaths);
    hash.AddRaw(g_GameManager.bombsUsed);
    hash.AddRaw(g_GameManager.spellcardsCaptured);
    hash.AddRaw(g_GameManager.isTimeStopped);
    hash.AddRaw(g_GameManager.currentPower);
    hash.AddRaw(g_GameManager.pointItemsCollectedInStage);
    hash.AddRaw(g_GameManager.pointItemsCollected);
    hash.AddRaw(g_GameManager.numRetries);
    hash.AddRaw(g_GameManager.powerItemCountForScore);
    hash.AddRaw(g_GameManager.livesRemaining);
    hash.AddRaw(g_GameManager.bombsRemaining);
    hash.AddRaw(g_GameManager.extraLives);
    hash.AddRaw(g_GameManager.character);
    hash.AddRaw(g_GameManager.character2);
    hash.AddRaw(g_GameManager.shotType);
    hash.AddRaw(g_GameManager.shotType2);
    hash.AddRaw(g_GameManager.isGameCompleted);
    hash.AddRaw(g_GameManager.isInPracticeMode);
    hash.AddRaw(g_GameManager.demoMode);
    hash.AddRaw(g_GameManager.gameFrames);
    hash.AddRaw(g_GameManager.currentStage);
    hash.AddRaw(g_GameManager.rank);
    hash.AddRaw(g_GameManager.maxRank);
    hash.AddRaw(g_GameManager.minRank);
    hash.AddRaw(g_GameManager.subRank);
    hash.AddRaw(g_GameManager.livesRemaining2);
    hash.AddRaw(g_GameManager.bombsRemaining2);
    hash.AddRaw(g_GameManager.currentPower2);
    return hash.Finish();
}

uint64_t HashInputRuntimeState()
{
    const Controller::RuntimeState state = Controller::CaptureRuntimeState();
    DiagnosticHashBuilder hash;
    hash.AddRaw(g_State.currentNetFrame);
    hash.AddRaw(g_State.delay);
    hash.AddRaw(g_State.currentDelayCooldown);
    hash.AddRaw(state.focusButtonConflictState);
    hash.AddRaw(g_LastFrameInput);
    hash.AddRaw(g_CurFrameInput);
    hash.AddRaw(g_IsEigthFrameOfHeldInput);
    hash.AddRaw(g_NumOfFramesInputsWereHeld);
    return hash.Finish();
}

uint64_t HashCatkState()
{
    DiagnosticHashBuilder hash;
    hash.AddBytes(g_GameManager.catk, sizeof(g_GameManager.catk));
    return hash.Finish();
}

uint64_t HashRngState()
{
    DiagnosticHashBuilder hash;
    hash.AddRaw(g_Rng.seed);
    hash.AddRaw(g_Rng.generationCount);
    return hash.Finish();
}

uint64_t HashEclManagerState()
{
    DiagnosticHashBuilder hash;
    hash.AddRaw((int64_t)NormalizeByteOffset(g_EclManager.eclFile, g_EclManager.eclFile));
    hash.AddRaw((int64_t)NormalizeByteOffset(g_EclManager.eclFile, g_EclManager.timeline));
    if (g_EclManager.subTable != nullptr && g_EclManager.eclFile != nullptr)
    {
        const int subCount = g_EclManager.eclFile->subCount;
        hash.AddRaw(subCount);
        for (int i = 0; i < subCount; ++i)
        {
            hash.AddRaw((int64_t)NormalizeByteOffset(g_EclManager.eclFile, g_EclManager.subTable[i]));
        }
    }
    else
    {
        hash.AddRaw(0);
    }
    return hash.Finish();
}

uint64_t HashEnemyEclRuntimeState()
{
    const EnemyEclInstr::RuntimeState state = EnemyEclInstr::CaptureRuntimeState();
    DiagnosticHashBuilder hash;
    hash.AddRaw(state.playerShot);
    hash.AddFloat(state.playerDistance);
    hash.AddFloat(state.playerAngle);
    for (float angle : state.starAngleTable)
    {
        hash.AddFloat(angle);
    }
    HashVec3(hash, state.enemyPosVector);
    HashVec3(hash, state.playerPosVector);
    HashArray(hash, state.eclLiteralInts, std::size(state.eclLiteralInts));
    for (float value : state.eclLiteralFloats)
    {
        hash.AddFloat(value);
    }
    hash.AddRaw(state.eclLiteralIntCursor);
    hash.AddRaw(state.eclLiteralFloatCursor);
    return hash.Finish();
}

uint64_t HashScreenEffectRuntimeState()
{
    const ScreenEffect::RuntimeState state = ScreenEffect::CaptureRuntimeState();
    DiagnosticHashBuilder hash;
    hash.AddRaw((int)state.activeEffects.size());
    for (const ScreenEffect::RuntimeEffectState &effect : state.activeEffects)
    {
        hash.AddRaw(effect.usedEffect);
        hash.AddRaw(effect.fadeAlpha);
        hash.AddRaw(effect.effectLength);
        hash.AddRaw(effect.genericParam);
        hash.AddRaw(effect.shakinessParam);
        hash.AddRaw(effect.unusedParam);
        HashTimer(hash, effect.timer);
    }
    return hash.Finish();
}

uint64_t HashControllerRuntimeState()
{
    const Controller::RuntimeState state = Controller::CaptureRuntimeState();
    DiagnosticHashBuilder hash;
    hash.AddRaw(state.focusButtonConflictState);
    return hash.Finish();
}

uint64_t HashSupervisorRuntimeState()
{
    DiagnosticHashBuilder hash;
    hash.AddRaw(g_Supervisor.calcCount);
    hash.AddRaw(g_Supervisor.wantedState);
    hash.AddRaw(g_Supervisor.curState);
    hash.AddRaw(g_Supervisor.wantedState2);
    hash.AddRaw(g_Supervisor.unk194);
    hash.AddRaw(g_Supervisor.unk198);
    hash.AddBool(g_Supervisor.isInEnding);
    hash.AddRaw(g_Supervisor.vsyncEnabled);
    hash.AddRaw(g_Supervisor.lastFrameTime);
    hash.AddFloat(g_Supervisor.effectiveFramerateMultiplier);
    hash.AddFloat(g_Supervisor.framerateMultiplier);
    hash.AddFloat(g_Supervisor.unk1b4);
    hash.AddFloat(g_Supervisor.unk1b8);
    hash.AddRaw(g_Supervisor.startupTimeBeforeMenuMusic);
    hash.AddRaw(g_TickCountToEffectiveFramerate);
    hash.AddDouble(g_LastFrameTime);
    hash.AddRaw(g_GameWindow.curFrame);
    return hash.Finish();
}

uint64_t HashSoundRuntimeState()
{
    DiagnosticHashBuilder hash;
    HashArray(hash, g_SoundPlayer.soundBuffersToPlay, std::size(g_SoundPlayer.soundBuffersToPlay));
    HashArray(hash, g_SoundPlayer.unk408, std::size(g_SoundPlayer.unk408));
    hash.AddRaw(g_SoundPlayer.isLooping);
    return hash.Finish();
}

bool CaptureFrameSubsystemHashes(int frame, FrameSubsystemHashes &outHashes)
{
    if (!g_State.isSessionActive || !g_State.isConnected || frame < 0)
    {
        return false;
    }

    outHashes = {};
    outHashes.stage = g_GameManager.currentStage;
    outHashes.frame = frame;
    outHashes.rollbackEpochStartFrame = g_State.rollbackEpochStartFrame;
    outHashes.gameHash = HashGameManagerState();
    outHashes.player1Hash = HashPlayerState(g_Player);
    outHashes.player2Hash = HashPlayerState(g_Player2);
    outHashes.bulletHash = HashBulletManagerState();
    outHashes.enemyHash = HashEnemyManagerState();
    outHashes.itemHash = HashItemManagerState();
    outHashes.effectHash = HashEffectManagerState();
    outHashes.stageHash = HashStageState();
    outHashes.eclHash = HashEclManagerState();
    outHashes.enemyEclHash = HashEnemyEclRuntimeState();
    outHashes.screenHash = HashScreenEffectRuntimeState();
    outHashes.inputHash = HashInputRuntimeState();
    outHashes.catkHash = HashCatkState();
    outHashes.rngHash = HashRngState();

    DiagnosticHashBuilder all;
    all.AddRaw(outHashes.gameHash);
    all.AddRaw(outHashes.player1Hash);
    all.AddRaw(outHashes.player2Hash);
    all.AddRaw(outHashes.bulletHash);
    all.AddRaw(outHashes.enemyHash);
    all.AddRaw(outHashes.itemHash);
    all.AddRaw(outHashes.effectHash);
    all.AddRaw(outHashes.stageHash);
    all.AddRaw(outHashes.eclHash);
    all.AddRaw(outHashes.enemyEclHash);
    all.AddRaw(outHashes.screenHash);
    all.AddRaw(outHashes.inputHash);
    all.AddRaw(outHashes.catkHash);
    all.AddRaw(outHashes.rngHash);
    outHashes.allHash = all.Finish();
    return true;
}

void TraceFrameSubsystemHashes(int frame, const char *phase)
{
    if (!THPrac::TH06::THPracIsDebugLogEnabled())
    {
        return;
    }

    FrameSubsystemHashes hashes {};
    if (!CaptureFrameSubsystemHashes(frame, hashes))
    {
        return;
    }

    TraceDiagnostic(
        "frame-subsystem-hash",
        "phase=%s frame=%d all=%016llx game=%016llx p1=%016llx p2=%016llx bullet=%016llx enemy=%016llx item=%016llx effect=%016llx stage=%016llx ecl=%016llx enemyEcl=%016llx screen=%016llx input=%016llx catk=%016llx rng=%016llx",
        phase != nullptr ? phase : "unknown", frame, (unsigned long long)hashes.allHash,
        (unsigned long long)hashes.gameHash, (unsigned long long)hashes.player1Hash,
        (unsigned long long)hashes.player2Hash, (unsigned long long)hashes.bulletHash,
        (unsigned long long)hashes.enemyHash, (unsigned long long)hashes.itemHash,
        (unsigned long long)hashes.effectHash, (unsigned long long)hashes.stageHash,
        (unsigned long long)hashes.eclHash, (unsigned long long)hashes.enemyEclHash,
        (unsigned long long)hashes.screenHash, (unsigned long long)hashes.inputHash,
        (unsigned long long)hashes.catkHash, (unsigned long long)hashes.rngHash);
    RecordConsistencyHashSample(hashes);
}

FILE *OpenDiagnosticLogFile()
{
    static FILE *file = nullptr;
    static bool firstOpen = true;
    static char resolvedPath[512] = {};

    if (!THPrac::TH06::THPracIsDebugLogEnabled())
    {
        return nullptr;
    }

    if (file != nullptr)
    {
        return file;
    }

    if (resolvedPath[0] == '\0')
    {
        char relativePath[128] = {};
        std::snprintf(relativePath, sizeof(relativePath), "netplay_diag_%lu.log", GetDiagnosticProcessId());
        GamePaths::Resolve(resolvedPath, sizeof(resolvedPath), relativePath);
    }

    GamePaths::EnsureParentDir(resolvedPath);
    file = std::fopen(resolvedPath, firstOpen ? "wt" : "at");
    if (file == nullptr)
    {
        return nullptr;
    }

    firstOpen = false;
    std::fprintf(file, "==== netplay diagnostic log pid=%lu ====\n", GetDiagnosticProcessId());
    std::fflush(file);
    return file;
}

void TraceDiagnostic(const char *event, const char *fmt, ...)
{
    FILE *file = OpenDiagnosticLogFile();
    if (file == nullptr)
    {
        return;
    }

    char message[1536] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    static Uint64 eventCounter = 0;
    std::fprintf(file, "#%llu t=%llu %s event=%s %s\n", (unsigned long long)++eventCounter,
                 (unsigned long long)SDL_GetTicks64(), BuildRuntimeStateSummary().c_str(), event,
                 message[0] != '\0' ? message : "-");
    std::fflush(file);
}

std::string BuildPacketSummary(const Pack &pack)
{
    char bytes[128] = {};
    char *cursor = bytes;
    const unsigned char *raw = reinterpret_cast<const unsigned char *>(&pack);
    const int dumpCount = std::min(g_State.lastPacketBytes > 0 ? g_State.lastPacketBytes : 16, 16);
    for (int i = 0; i < dumpCount; ++i)
    {
        const ptrdiff_t remaining = (bytes + sizeof(bytes)) - cursor;
        if (remaining <= 1)
        {
            break;
        }

        const int written = std::snprintf(cursor, (size_t)remaining, "%s%02X", i == 0 ? "" : " ", raw[i]);
        if (written <= 0 || written >= remaining)
        {
            break;
        }
        cursor += written;
    }

    char summary[384] = {};
    std::snprintf(summary, sizeof(summary),
                  "from=%s:%d bytes=%d type=%d ctrl=%d delay=%d ver=%d flags=%d seq=%u raw=%s",
                  g_State.lastPacketFromIp.empty() ? "?" : g_State.lastPacketFromIp.c_str(), g_State.lastPacketFromPort,
                  g_State.lastPacketBytes, pack.type, (int)pack.ctrl.ctrlType, pack.ctrl.initSetting.delay,
                  pack.ctrl.initSetting.ver, pack.ctrl.initSetting.flags, pack.seq, bytes);
    return summary;
}

void TraceLauncherPacket(const char *phase, const Pack &pack)
{
    if (!THPrac::TH06::THPracIsDebugLogEnabled())
    {
        return;
    }

    FILE *file = std::fopen("netplay_trace.log", "a");
    if (file == nullptr)
    {
        TraceDiagnostic("launcher-packet", "phase=%s summary=%s", phase, BuildPacketSummary(pack).c_str());
        return;
    }

    std::fprintf(file, "[%s] %s\n", phase, BuildPacketSummary(pack).c_str());
    std::fclose(file);
    TraceDiagnostic("launcher-packet", "phase=%s summary=%s", phase, BuildPacketSummary(pack).c_str());
}

void SetRelayStatus(const std::string &text)
{
    g_Relay.statusText = text;
}

void CloseRelayProbeSocket()
{
    if (g_Relay.socket != kInvalidSocket)
    {
        CloseSocketHandle(g_Relay.socket);
        g_Relay.socket = kInvalidSocket;
        SocketSystem::Release();
    }
    g_Relay.family = AF_UNSPEC;
    g_Relay.isConnecting = false;
    g_Relay.isReachable = false;
    g_Relay.lastRttMs = -1;
    g_Relay.lastProbeTick = 0;
    g_Relay.lastProbeSendTick = 0;
    g_Relay.pendingNonce.clear();
    g_Relay.resolvedAddr = {};
    g_Relay.resolvedAddrLen = 0;
    g_Relay.resolvedAddrValid = false;
}


} // namespace th06::Netplay
