#include "AstroBotEclForecast.hpp"

#include <algorithm>
#include <cmath>

#include "EclManager.hpp"
#include "EnemyEclInstr.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "utils.hpp"

namespace th06::AstroBotEclForecast
{
namespace
{
constexpr int kForecastMaxFrames = 45;
constexpr int kForecastMaxInstructions = 24;
constexpr int kForecastMaxInstructionStep = 0x400;

float AngleToPlayer(const Player &player, const D3DXVECTOR3 &origin)
{
    return std::atan2(player.positionCenter.y - origin.y, player.positionCenter.x - origin.x);
}

bool ShouldProcessInstruction(const EclRawInstr &instr)
{
    return (instr.skipForDifficulty & (1 << g_GameManager.difficulty)) != 0;
}

bool IsStopOpcode(i16 opcode)
{
    switch (opcode)
    {
    case ECL_OPCODE_JUMP:
    case ECL_OPCODE_JUMPDEC:
    case ECL_OPCODE_JUMPLSS:
    case ECL_OPCODE_JUMPLEQ:
    case ECL_OPCODE_JUMPEQU:
    case ECL_OPCODE_JUMPGRE:
    case ECL_OPCODE_JUMPGEQ:
    case ECL_OPCODE_JUMPNEQ:
    case ECL_OPCODE_CALL:
    case ECL_OPCODE_RET:
    case ECL_OPCODE_CALLLSS:
    case ECL_OPCODE_CALLLEQ:
    case ECL_OPCODE_CALLEQU:
    case ECL_OPCODE_CALLGRE:
    case ECL_OPCODE_CALLGEQ:
    case ECL_OPCODE_CALLNEQ:
    case ECL_OPCODE_EXINSCALL:
    case ECL_OPCODE_EXINSREPEAT:
    case ECL_OPCODE_ENEMYINTERRUPT:
    case ECL_OPCODE_ENEMYINTERRUPTSET:
        return true;
    default:
        return false;
    }
}

template <typename T>
bool TryReadStruct(const T *ptr, T *out)
{
#ifdef _MSC_VER
    __try
    {
        *out = *ptr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    if (ptr == nullptr || out == nullptr)
    {
        return false;
    }
    *out = *ptr;
    return true;
#endif
}

bool TryAdvanceInstr(const EclRawInstr *instr, int offsetToNext, const EclRawInstr **nextInstr)
{
    if (instr == nullptr || nextInstr == nullptr || offsetToNext <= 0 || offsetToNext > kForecastMaxInstructionStep)
    {
        return false;
    }

    const uintptr_t current = reinterpret_cast<uintptr_t>(instr);
    const uintptr_t next = current + static_cast<uintptr_t>(offsetToNext);
    if (next <= current)
    {
        return false;
    }

    *nextInstr = reinterpret_cast<const EclRawInstr *>(next);
    return true;
}

template <typename T>
T *RawPtr(const T *ptr)
{
    return const_cast<T *>(ptr);
}

void AppendThreat(ForecastSnapshot &snapshot, const ForecastThreat &threat)
{
    if (!threat.valid || snapshot.count >= ARRAY_SIZE_SIGNED(snapshot.threats))
    {
        return;
    }
    snapshot.threats[snapshot.count++] = threat;
}

float ReadFloat(Enemy *enemy, const float *value)
{
    return *EnemyEclInstr::GetVarFloat(enemy, RawPtr(value), nullptr);
}

int ReadInt(Enemy *enemy, const EclVarId *value)
{
    return *EnemyEclInstr::GetVar(enemy, RawPtr(value), nullptr);
}

void AppendBulletThreat(ForecastSnapshot &snapshot, Enemy *enemy, const Player &player, const EclRawInstr &instr, int deltaFrames)
{
    const auto &args = instr.args.bullet;
    const int aimMode = instr.opCode - ECL_OPCODE_BULLETFANAIMED;
    int count1 = ReadInt(enemy, &args.count1);
    int count2 = ReadInt(enemy, &args.count2);
    count1 += enemy->BulletRankAmount1(g_GameManager.rank);
    count2 += enemy->BulletRankAmount2(g_GameManager.rank);
    count1 = std::max(count1, 1);
    count2 = std::max(count2, 1);

    const D3DXVECTOR3 origin = enemy->position + enemy->shootOffset;
    const bool aimed = aimMode == 0 || aimMode == 2 || aimMode == 4;
    float angleCenter = ReadFloat(enemy, &args.angle1);
    if (aimed)
    {
        angleCenter += AngleToPlayer(player, origin);
    }

    const float angleStep = std::fabs(ReadFloat(enemy, &args.angle2));
    float halfRange = 0.0f;
    ForecastThreatType type = ForecastThreatType::BulletCone;
    switch (instr.opCode)
    {
    case ECL_OPCODE_BULLETCIRCLEAIMED:
    case ECL_OPCODE_BULLETCIRCLE:
    case ECL_OPCODE_BULLETOFFSETCIRCLEAIMED:
    case ECL_OPCODE_BULLETOFFSETCIRCLE:
        type = ForecastThreatType::BulletRing;
        halfRange = ZUN_PI;
        break;
    default:
        halfRange = angleStep * std::max(count1 - 1, 0) * 0.5f + angleStep * 0.5f;
        break;
    }

    float speed1 = ReadFloat(enemy, &args.speed1);
    float speed2 = ReadFloat(enemy, &args.speed2);
    if (speed1 != 0.0f)
    {
        speed1 += enemy->BulletRankSpeed(g_GameManager.rank);
        speed1 = std::max(speed1, 0.3f);
    }
    speed2 += enemy->BulletRankSpeed(g_GameManager.rank) * 0.5f;
    speed2 = std::max(speed2, 0.3f);

    const float speedMin = std::max(0.3f, std::min(speed1, speed1 + speed2 * std::max(count2 - 1, 0)));
    const float speedMax = std::max(speed1, speed1 + std::fabs(speed2) * std::max(count2 - 1, 0));
    const int spawnOffset = std::max(deltaFrames, 0);

    ForecastThreat threat {};
    threat.valid = true;
    threat.type = type;
    threat.isLaser = false;
    threat.spawnFrameOffset = spawnOffset;
    threat.origin = origin;
    threat.angleCenter = angleCenter;
    threat.angleHalfRange = halfRange;
    threat.rangeMin = speedMin * static_cast<float>(spawnOffset);
    threat.rangeMax = speedMax * static_cast<float>(spawnOffset + 12);
    threat.width = 10.0f + static_cast<float>(std::min(count1 * count2, 24));
    threat.weight = 0.9f + static_cast<float>(std::min(count1 * count2, 32)) * 0.18f;
    if (type == ForecastThreatType::BulletRing)
    {
        threat.weight *= 1.15f;
    }
    AppendThreat(snapshot, threat);
}

void AppendLaserThreat(ForecastSnapshot &snapshot, Enemy *enemy, const Player &player, const EclRawInstr &instr, int deltaFrames)
{
    const auto &args = instr.args.laser;
    const D3DXVECTOR3 origin = enemy->position + enemy->shootOffset;
    float angleCenter = ReadFloat(enemy, &args.angle);
    if (instr.opCode == ECL_OPCODE_LASERCREATEAIMED)
    {
        angleCenter += AngleToPlayer(player, origin);
    }

    const float speed = std::max(ReadFloat(enemy, &args.speed), 0.0f);
    const float startOffset = std::max(ReadFloat(enemy, &args.startOffset), 0.0f);
    const float endOffset = std::max(ReadFloat(enemy, &args.endOffset), startOffset);
    const float width = std::max(ReadFloat(enemy, &args.width), 1.0f);
    const int activationOffset = std::max(deltaFrames + std::max(args.hitboxStartTime, 0), 0);

    ForecastThreat threat {};
    threat.valid = true;
    threat.type = ForecastThreatType::LaserBeam;
    threat.isLaser = true;
    threat.spawnFrameOffset = activationOffset;
    threat.origin = origin;
    threat.angleCenter = angleCenter;
    threat.angleHalfRange = std::atan2(std::max(width, 8.0f), std::max(endOffset, 24.0f));
    threat.rangeMin = startOffset + speed * static_cast<float>(activationOffset);
    threat.rangeMax = endOffset + speed * static_cast<float>(activationOffset + 8);
    threat.width = width;
    threat.weight = 3.2f + width * 0.08f;
    AppendThreat(snapshot, threat);
}
} // namespace

ForecastSnapshot BuildForecastSnapshot(const Player &player)
{
    ForecastSnapshot snapshot {};

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_EnemyManager.enemies) - 1; ++idx)
    {
        Enemy &enemy = g_EnemyManager.enemies[idx];
        if (enemy.life <= 0 || enemy.currentContext.currentInstr == nullptr)
        {
            continue;
        }

        const EnemyEclInstr::RuntimeState savedRuntime = EnemyEclInstr::CaptureRuntimeState();

        const EclRawInstr *instr = enemy.currentContext.currentInstr;
        const int currentTime = enemy.currentContext.time.current;
        int previousTime = currentTime;
        for (int scanned = 0; instr != nullptr && scanned < kForecastMaxInstructions; ++scanned)
        {
            EclRawInstr instrCopy {};
            if (!TryReadStruct(instr, &instrCopy))
            {
                break;
            }

            if (instrCopy.offsetToNext <= 0 || instrCopy.offsetToNext > kForecastMaxInstructionStep)
            {
                break;
            }

            const int deltaFrames = std::max(instrCopy.time - currentTime, 0);
            if (instrCopy.time < previousTime || deltaFrames > kForecastMaxFrames)
            {
                break;
            }
            previousTime = instrCopy.time;

            bool stopScan = false;
            if (ShouldProcessInstruction(instrCopy))
            {
                switch (instrCopy.opCode)
                {
                case ECL_OPCODE_BULLETFANAIMED:
                case ECL_OPCODE_BULLETFAN:
                case ECL_OPCODE_BULLETCIRCLEAIMED:
                case ECL_OPCODE_BULLETCIRCLE:
                case ECL_OPCODE_BULLETOFFSETCIRCLEAIMED:
                case ECL_OPCODE_BULLETOFFSETCIRCLE:
                    AppendBulletThreat(snapshot, &enemy, player, instrCopy, deltaFrames);
                    break;
                case ECL_OPCODE_LASERCREATE:
                case ECL_OPCODE_LASERCREATEAIMED:
                    AppendLaserThreat(snapshot, &enemy, player, instrCopy, deltaFrames);
                    break;
                default:
                    if (IsStopOpcode(instrCopy.opCode))
                    {
                        stopScan = true;
                    }
                    break;
                }
            }

            if (stopScan)
            {
                break;
            }

            const EclRawInstr *nextInstr = nullptr;
            if (!TryAdvanceInstr(instr, instrCopy.offsetToNext, &nextInstr))
            {
                break;
            }
            instr = nextInstr;
        }

        EnemyEclInstr::RestoreRuntimeState(savedRuntime);
    }

    return snapshot;
}
} // namespace th06::AstroBotEclForecast
