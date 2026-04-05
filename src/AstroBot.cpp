#include "AstroBot.hpp"
#include "AstroBotEclForecast.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "BulletManager.hpp"
#include "Enemy.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "PortableGameplayRestore.hpp"
#include "Session.hpp"
#include "Supervisor.hpp"
#include "NetplayInternal.hpp"
#include "thprac_th06.h"
#include "utils.hpp"

namespace th06::AstroBot
{
namespace
{
constexpr int kDangerProspects = 3;
constexpr int kDangerWeights[kDangerProspects] = {15000, 10000, 5000};
constexpr float kWarningRange = 100.0f;
constexpr float kDynamicRangeFactor = 40.0f;
constexpr float kEnemyForceFactor = 5.0f;
constexpr float kBulletWallGap = 12.0f;
constexpr float kDestGridStep = 30.0f;
constexpr int kDestGridCount = 9;
constexpr float kDirectionThreshold = 30.0f;
constexpr int kSpeedSwitchValue = 250;
constexpr int kBombCooldownFrames = 15;
constexpr float kFatalDanger = 1000000.0f;
constexpr float kWallDanger = 8000.0f;
constexpr float kBoundaryMargin = 36.0f;
constexpr float kBoundaryForce = 220.0f;
constexpr float kEnemyDangerBase = 1800.0f;
constexpr float kDestAttractionScale = 18.0f;
constexpr float kAlignAttractionScale = 26.0f;
constexpr float kItemAttractionScale = 54.0f;
constexpr float kSafeItemDangerThreshold = 760.0f;
constexpr float kBossAlignRewardScale = 136.0f;
constexpr float kEnemyChaseRewardScale = 92.0f;
constexpr float kItemRewardScale = 180.0f;
constexpr float kFocusDangerThreshold = 180.0f;
constexpr float kEmergencyBombDanger = 2600.0f;
constexpr float kDangerBombThreshold = 3200.0f;
constexpr int kNearestBulletCap = 48;
constexpr int kNearestLaserCap = 16;
constexpr int kSurvivalCheckFrames = 5;
constexpr float kBottomComfortBand = 52.0f;
constexpr float kEdgeOutwardGuard = 10.0f;
constexpr float kSqueezeDangerThreshold = 520.0f;
constexpr float kSqueezeDensityThreshold = 180.0f;
constexpr float kSqueezeCenterBiasScale = 42.0f;
constexpr float kSqueezeBoundaryPenaltyScale = 480.0f;
constexpr float kFastModeTieSlack = 22.0f;
constexpr float kActionSwitchPenalty = 10.0f;
constexpr float kThreadFastPenalty = 34.0f;
constexpr float kThreadFocusBonus = 12.0f;
constexpr float kEscapeSlowPenalty = 16.0f;
constexpr float kEscapeFastBonus = 8.0f;
constexpr float kThreadDangerThreshold = 420.0f;
constexpr float kEscapeDangerThreshold = 210.0f;
constexpr int kMaxVirtualWalls = 48;
constexpr float kWallRecognitionRange = 150.0f;
constexpr float kWallAngleTolerance = 0.22f;
constexpr float kWallDangerScale = 3400.0f;
constexpr float kTrajectoryNearMissScale = 220.0f;
constexpr float kTrajectoryGrazeScale = 150.0f;
constexpr int kDensityGridSize = 11;
constexpr float kDensityCellSize = 48.0f;
constexpr float kDensityMinRange = 80.0f;
constexpr float kDensityMaxRange = 440.0f;
constexpr int kDensityProjectionFrames = 10;
constexpr float kDensityForceScale = 58.0f;
constexpr float kDensityPenaltyScale = 40.0f;
constexpr int kFarSectorCount = 8;
constexpr float kFarSenseMinRange = 220.0f;
constexpr float kFarSenseMaxRange = 880.0f;
constexpr int kFarSenseProjectionFrames = 18;
constexpr float kFarDensityForceScale = 22.0f;
constexpr float kFarDensityPenaltyScale = 14.0f;
constexpr int kGlobalLaneCount = 9;
constexpr float kGlobalLaneAttractionScale = 30.0f;
constexpr float kGlobalLanePenaltyScale = 18.0f;
constexpr float kLaserLanePenaltyScale = 42.0f;
constexpr float kCornerTrapPenaltyScale = 36.0f;
constexpr float kTopCurtainGapAttractionScale = 46.0f;
constexpr float kTopCurtainPenaltyScale = 32.0f;
constexpr int kEscapeLaneCount = 7;
constexpr float kEscapeLaneAttractionScale = 34.0f;
constexpr float kEscapeLanePenaltyScale = 24.0f;
constexpr float kIdleCalmDangerThreshold = 120.0f;
constexpr float kIdleCalmTrajectoryThreshold = 90.0f;
constexpr float kIdleReward = 18.0f;
constexpr float kSameActionReward = 12.0f;
constexpr int kLaserWarningLeadFrames = 18;

struct BotVec2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct EnemyTarget
{
    bool valid = false;
    BotVec2 pos {};
    bool isBoss = false;
};

struct ItemSample
{
    bool valid = false;
    BotVec2 pos {};
    float priority = 0.0f;
};

struct BotContext
{
    Target target = Target::Off;
    const Player *player = nullptr;
    float minX = GAME_REGION_LEFT;
    float maxX = GAME_REGION_RIGHT;
    float minY = GAME_REGION_TOP;
    float maxY = GAME_REGION_BOTTOM;
    float effectiveFramerateMultiplier = 1.0f;
    bool bossOnly = true;
    bool autoShoot = true;
    bool autoBomb = false;
    int bombsRemaining = 0;
    EnemyTarget enemy {};
    int itemCount = 0;
    ItemSample items[96] {};
    AstroBotEclForecast::ForecastSnapshot forecast {};
};

struct InputOwnership
{
    Target controlledTarget = Target::Off;
    bool remoteSession = false;
    bool emitCanonicalLocalInput = false;
    bool localIsPlayer1 = true;
};

struct ThreatList
{
    const Bullet *bullets[kNearestBulletCap] {};
    float bulletDistSq[kNearestBulletCap] {};
    int bulletCount = 0;
    const Laser *lasers[kNearestLaserCap] {};
    float laserDistSq[kNearestLaserCap] {};
    int laserCount = 0;
};

struct MovementProfile
{
    ModeHint mode = ModeHint::Force;
    int closeThreats = 0;
    int chaseThreats = 0;
};

struct VirtualWall
{
    bool valid = false;
    BotVec2 start {};
    BotVec2 end {};
    float radius = 0.0f;
};

struct DensityField
{
    float originX = 0.0f;
    float originY = 0.0f;
    float cellSize = kDensityCellSize;
    float values[kDensityGridSize][kDensityGridSize] {};
    bool hasEscapeLane = false;
    float escapeX = 0.0f;
    float escapeY = 0.0f;
    float escapePressure = 0.0f;
    float localPressure = 0.0f;
    float farSectorPressure[kFarSectorCount] {};
    float globalLanePressure[kGlobalLaneCount] {};
    float laserLanePressure[kGlobalLaneCount] {};
    float topCurtainLanePressure[kGlobalLaneCount] {};
    bool hasStrategicLane = false;
    float strategicLaneX = 0.0f;
    float strategicLanePressure = 0.0f;
    bool hasTopCurtainGap = false;
    float topCurtainGapX = 0.0f;
    float topCurtainGapPressure = 0.0f;
};

constexpr Action kCandidateActions[] = {Action::Idle,      Action::Left,    Action::Right,   Action::Up,      Action::Down,
                                        Action::UpLeft,    Action::UpRight, Action::DownLeft, Action::DownRight};

struct RuntimeState
{
    StatusSnapshot status {};
    Action lastAction = Action::Idle;
    bool lastFocus = true;
    int bombCooldown = 0;
};

RuntimeState g_RuntimeState;

bool IsLaserActiveAt(const Laser &laser, int futureStep);
bool IsLaserThreateningSoon(const Laser &laser, int futureStep);
void GetLaserSegment(const Laser &laser, float multiplier, int futureStep, BotVec2 &start, BotVec2 &end, float &halfWidth);
void BuildLaserProbeShape(const Laser &laser, float multiplier, int futureStep, D3DXVECTOR3 &laserCenter, D3DXVECTOR3 &laserSize,
                          D3DXVECTOR3 &rotation);
void DepositForecastThreats(const BotContext &ctx, DensityField &field);
float ComputeForecastDangerAt(const BotContext &ctx, float x, float y);
BotVec2 GenerateForecastForce(const BotContext &ctx);
void FinalizeEscapeLane(const BotContext &ctx, DensityField &field);
float ComputeObjectiveRewardAt(const BotContext &ctx, float x, float y, float localDanger);
bool ActionPressesOutwardBoundary(const BotContext &ctx, Action action);

InputOwnership ResolveInputOwnership(Target requestedTarget)
{
    InputOwnership ownership {};
    ownership.remoteSession = Session::IsRemoteNetplaySession();
    if (requestedTarget == Target::Off)
    {
        return ownership;
    }

    if (!ownership.remoteSession)
    {
        ownership.controlledTarget = requestedTarget;
        return ownership;
    }

    ownership.emitCanonicalLocalInput = true;
    ownership.localIsPlayer1 =
        (Netplay::g_State.isHost && Netplay::g_State.hostIsPlayer1) || (Netplay::g_State.isGuest && !Netplay::g_State.hostIsPlayer1);
    ownership.controlledTarget = ownership.localIsPlayer1 ? Target::P1 : Target::P2;
    return ownership;
}

const char *OwnershipTag(const InputOwnership &ownership)
{
    if (!ownership.remoteSession)
    {
        return "local";
    }
    return ownership.localIsPlayer1 ? "local-p1" : "local-p2";
}

float DistanceSq2D(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

float Dot2D(float ax, float ay, float bx, float by)
{
    return ax * bx + ay * by;
}

float DistanceSqPointToSegment(float px, float py, float ax, float ay, float bx, float by)
{
    const float abx = bx - ax;
    const float aby = by - ay;
    const float lenSq = abx * abx + aby * aby;
    if (lenSq <= 0.0001f)
    {
        return DistanceSq2D(px, py, ax, ay);
    }
    const float t = std::clamp(Dot2D(px - ax, py - ay, abx, aby) / lenSq, 0.0f, 1.0f);
    const float nx = ax + abx * t;
    const float ny = ay + aby * t;
    return DistanceSq2D(px, py, nx, ny);
}

float WrapAngleDiff(float a, float b)
{
    return std::fabs(std::remainder(a - b, 2.0f * ZUN_PI));
}

float Clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

BotVec2 NormalizeOrZero(float x, float y)
{
    const float lenSq = x * x + y * y;
    if (lenSq <= 0.0001f)
    {
        return {};
    }
    const float invLen = 1.0f / std::sqrt(lenSq);
    return {x * invLen, y * invLen};
}

void Accumulate(BotVec2 &dst, const BotVec2 &src, float scale = 1.0f)
{
    dst.x += src.x * scale;
    dst.y += src.y * scale;
}

float ComputeWallDangerAt(float x, float y, const VirtualWall *walls, int wallCount)
{
    float danger = 0.0f;
    for (int idx = 0; idx < wallCount; ++idx)
    {
        const VirtualWall &wall = walls[idx];
        if (!wall.valid)
        {
            continue;
        }
        const float distSq =
            DistanceSqPointToSegment(x, y, wall.start.x, wall.start.y, wall.end.x, wall.end.y);
        const float effectiveSq = std::max(distSq - wall.radius * wall.radius, 9.0f);
        danger += kWallDangerScale / effectiveSq;
    }
    return danger;
}

float BulletEffectiveRadius(const Bullet &bullet)
{
    return std::max(std::max(bullet.sprites.grazeSize.x, bullet.sprites.grazeSize.y) * 0.5f, 4.0f);
}

void DepositDensity(DensityField &field, float x, float y, float weight)
{
    const float half = (static_cast<float>(kDensityGridSize) - 1.0f) * 0.5f;
    const float fx = (x - field.originX) / field.cellSize + half;
    const float fy = (y - field.originY) / field.cellSize + half;
    const int ix = static_cast<int>(std::floor(fx));
    const int iy = static_cast<int>(std::floor(fy));
    for (int oy = 0; oy <= 1; ++oy)
    {
        for (int ox = 0; ox <= 1; ++ox)
        {
            const int gx = ix + ox;
            const int gy = iy + oy;
            if (gx < 0 || gx >= kDensityGridSize || gy < 0 || gy >= kDensityGridSize)
            {
                continue;
            }
            const float wx = 1.0f - std::fabs(fx - static_cast<float>(gx));
            const float wy = 1.0f - std::fabs(fy - static_cast<float>(gy));
            const float blend = Clamp01(wx) * Clamp01(wy);
            field.values[gy][gx] += weight * blend;
        }
    }
}

void DepositFarSectorPressure(DensityField &field, float x, float y, float weight)
{
    const float dx = x - field.originX;
    const float dy = y - field.originY;
    const float distSq = dx * dx + dy * dy;
    if (distSq < kFarSenseMinRange * kFarSenseMinRange || distSq > kFarSenseMaxRange * kFarSenseMaxRange)
    {
        return;
    }

    float angle = std::atan2(dy, dx);
    if (angle < 0.0f)
    {
        angle += 2.0f * ZUN_PI;
    }
    const float sectorFloat = angle / (2.0f * ZUN_PI) * static_cast<float>(kFarSectorCount);
    const int sector = static_cast<int>(std::floor(sectorFloat)) % kFarSectorCount;
    const int nextSector = (sector + 1) % kFarSectorCount;
    const float blend = sectorFloat - std::floor(sectorFloat);
    field.farSectorPressure[sector] += weight * (1.0f - blend);
    field.farSectorPressure[nextSector] += weight * blend;
}

void DepositGlobalLanePressure(const BotContext &ctx, DensityField &field, float x, float width, float weight)
{
    const float left = ctx.minX;
    const float right = ctx.maxX;
    const float laneWidth = (right - left) / static_cast<float>(kGlobalLaneCount);
    const float halfWidth = std::max(width, 6.0f) * 0.5f;
    const float minX = x - halfWidth;
    const float maxX = x + halfWidth;
    for (int lane = 0; lane < kGlobalLaneCount; ++lane)
    {
        const float laneCenter = left + (static_cast<float>(lane) + 0.5f) * laneWidth;
        const float dist = std::max(std::fabs(laneCenter - x) - halfWidth, 0.0f);
        const float laneInfluence = 1.0f - Clamp01(dist / std::max(laneWidth * 1.3f, 1.0f));
        if (laneInfluence <= 0.0f)
        {
            continue;
        }
        if (laneCenter < minX - laneWidth || laneCenter > maxX + laneWidth)
        {
            continue;
        }
        field.globalLanePressure[lane] += weight * laneInfluence;
    }
}

void DepositLaserLanePressure(const BotContext &ctx, DensityField &field, float x, float width, float weight)
{
    const float left = ctx.minX;
    const float right = ctx.maxX;
    const float laneWidth = (right - left) / static_cast<float>(kGlobalLaneCount);
    const float halfWidth = std::max(width, laneWidth * 0.6f) * 0.5f;
    for (int lane = 0; lane < kGlobalLaneCount; ++lane)
    {
        const float laneCenter = left + (static_cast<float>(lane) + 0.5f) * laneWidth;
        const float dist = std::max(std::fabs(laneCenter - x) - halfWidth, 0.0f);
        const float laneInfluence = 1.0f - Clamp01(dist / std::max(laneWidth * 1.4f, 1.0f));
        if (laneInfluence <= 0.0f)
        {
            continue;
        }
        field.laserLanePressure[lane] += weight * laneInfluence;
        field.globalLanePressure[lane] += weight * laneInfluence * 0.7f;
    }
}

void DepositTopCurtainLanePressure(const BotContext &ctx, DensityField &field, float x, float width, float weight)
{
    const float left = ctx.minX;
    const float right = ctx.maxX;
    const float laneWidth = (right - left) / static_cast<float>(kGlobalLaneCount);
    const float halfWidth = std::max(width, laneWidth * 0.7f) * 0.5f;
    for (int lane = 0; lane < kGlobalLaneCount; ++lane)
    {
        const float laneCenter = left + (static_cast<float>(lane) + 0.5f) * laneWidth;
        const float dist = std::max(std::fabs(laneCenter - x) - halfWidth, 0.0f);
        const float laneInfluence = 1.0f - Clamp01(dist / std::max(laneWidth * 1.6f, 1.0f));
        if (laneInfluence <= 0.0f)
        {
            continue;
        }
        field.topCurtainLanePressure[lane] += weight * laneInfluence;
        field.globalLanePressure[lane] += weight * laneInfluence * 0.45f;
    }
}

DensityField BuildDensityField(const BotContext &ctx)
{
    DensityField field {};
    if (ctx.player == nullptr)
    {
        return field;
    }

    field.originX = ctx.player->positionCenter.x;
    field.originY = ctx.player->positionCenter.y;

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.bullets) - 1; ++idx)
    {
        const Bullet &bullet = g_BulletManager.bullets[idx];
        if (bullet.state == 0)
        {
            continue;
        }
        const float dist = std::sqrt(DistanceSq2D(field.originX, field.originY, bullet.pos.x, bullet.pos.y));
        if (dist < kDensityMinRange || dist > kDensityMaxRange)
        {
            continue;
        }
        const float ageWeight = 1.0f - ((dist - kDensityMinRange) / std::max(kDensityMaxRange - kDensityMinRange, 1.0f)) * 0.35f;
        const float speed =
            std::sqrt(bullet.velocity.x * bullet.velocity.x + bullet.velocity.y * bullet.velocity.y) * ctx.effectiveFramerateMultiplier;
        const float speedWeight = 1.0f + std::min(speed * 0.12f, 1.8f);
        const float sizeWeight = 1.0f + std::min(BulletEffectiveRadius(bullet) * 0.08f, 2.4f);
        const float weight = ageWeight * speedWeight * sizeWeight;
        DepositDensity(field, bullet.pos.x, bullet.pos.y, weight);
        DepositFarSectorPressure(field, bullet.pos.x, bullet.pos.y, weight * 0.85f);
        DepositGlobalLanePressure(ctx, field, bullet.pos.x, BulletEffectiveRadius(bullet) * 2.2f, weight * 0.65f);
        if (bullet.pos.y < field.originY - 18.0f && bullet.velocity.y > 0.25f)
        {
            DepositTopCurtainLanePressure(ctx, field, bullet.pos.x, BulletEffectiveRadius(bullet) * 3.2f, weight * 0.95f);
        }

        D3DXVECTOR3 projected = bullet.pos;
        projected.x += bullet.velocity.x * ctx.effectiveFramerateMultiplier * static_cast<float>(kDensityProjectionFrames);
        projected.y += bullet.velocity.y * ctx.effectiveFramerateMultiplier * static_cast<float>(kDensityProjectionFrames);
        DepositDensity(field, projected.x, projected.y, weight * 0.9f);

        D3DXVECTOR3 farProjected = bullet.pos;
        farProjected.x += bullet.velocity.x * ctx.effectiveFramerateMultiplier * static_cast<float>(kFarSenseProjectionFrames);
        farProjected.y += bullet.velocity.y * ctx.effectiveFramerateMultiplier * static_cast<float>(kFarSenseProjectionFrames);
        DepositFarSectorPressure(field, farProjected.x, farProjected.y, weight * 1.1f);
        DepositGlobalLanePressure(ctx, field, farProjected.x, BulletEffectiveRadius(bullet) * 2.8f, weight * 0.9f);
        if (farProjected.y < field.originY + 24.0f && bullet.velocity.y > 0.25f)
        {
            DepositTopCurtainLanePressure(ctx, field, farProjected.x, BulletEffectiveRadius(bullet) * 4.0f, weight * 1.2f);
        }
    }

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.lasers) - 1; ++idx)
    {
        const Laser &laser = g_BulletManager.lasers[idx];
        if (!IsLaserThreateningSoon(laser, 0))
        {
            continue;
        }

        BotVec2 a0 {};
        BotVec2 b0 {};
        BotVec2 a1 {};
        BotVec2 b1 {};
        float halfWidth0 = 0.0f;
        float halfWidth1 = 0.0f;
        GetLaserSegment(laser, ctx.effectiveFramerateMultiplier, 0, a0, b0, halfWidth0);
        GetLaserSegment(laser, ctx.effectiveFramerateMultiplier, kDensityProjectionFrames, a1, b1, halfWidth1);

        const float segDistSq = DistanceSqPointToSegment(field.originX, field.originY, a0.x, a0.y, b0.x, b0.y);
        if (segDistSq > (kDensityMaxRange + 96.0f) * (kDensityMaxRange + 96.0f))
        {
            continue;
        }

        constexpr int kLaserSamples = 8;
        for (int s = 0; s < kLaserSamples; ++s)
        {
            const float t = static_cast<float>(s) / static_cast<float>(kLaserSamples - 1);
            const float x0 = a0.x + (b0.x - a0.x) * t;
            const float y0 = a0.y + (b0.y - a0.y) * t;
            const float x1 = a1.x + (b1.x - a1.x) * t;
            const float y1 = a1.y + (b1.y - a1.y) * t;
            const bool activeLaser = IsLaserActiveAt(laser, 0);
            const float warningScale = activeLaser ? 1.0f : 0.55f;
            DepositDensity(field, x0, y0, (3.4f + halfWidth0 * 0.16f) * warningScale);
            DepositDensity(field, x1, y1, (4.0f + halfWidth1 * 0.18f) * warningScale);
            DepositFarSectorPressure(field, x1, y1, (2.8f + halfWidth1 * 0.10f) * warningScale);
            DepositGlobalLanePressure(ctx, field, x1, halfWidth1 * 3.0f, (3.2f + halfWidth1 * 0.14f) * warningScale);
            DepositLaserLanePressure(ctx, field, x0, halfWidth0 * 3.8f,
                                     (activeLaser ? 7.0f : 4.2f) + halfWidth0 * (activeLaser ? 0.24f : 0.16f));
            DepositLaserLanePressure(ctx, field, x1, halfWidth1 * 4.4f,
                                     (activeLaser ? 8.0f : 5.0f) + halfWidth1 * (activeLaser ? 0.28f : 0.18f));
        }
    }

    DepositForecastThreats(ctx, field);
    FinalizeEscapeLane(ctx, field);

    return field;
}

BotVec2 GenerateDensityForce(const BotContext &ctx, const DensityField &field)
{
    if (ctx.player == nullptr)
    {
        return {};
    }

    BotVec2 force {};
    const float half = (static_cast<float>(kDensityGridSize) - 1.0f) * 0.5f;
    for (int gy = 0; gy < kDensityGridSize; ++gy)
    {
        for (int gx = 0; gx < kDensityGridSize; ++gx)
        {
            const float density = field.values[gy][gx];
            if (density <= 0.01f)
            {
                continue;
            }
            const float cellX = field.originX + (static_cast<float>(gx) - half) * field.cellSize;
            const float cellY = field.originY + (static_cast<float>(gy) - half) * field.cellSize;
            const float distSq = std::max(DistanceSq2D(ctx.player->positionCenter.x, ctx.player->positionCenter.y, cellX, cellY), 64.0f);
            const BotVec2 away = NormalizeOrZero(ctx.player->positionCenter.x - cellX, ctx.player->positionCenter.y - cellY);
            Accumulate(force, away, density * kDensityForceScale / distSq);
        }
    }
    if (field.hasEscapeLane)
    {
        const float pressureGap = std::max(field.localPressure - field.escapePressure, 0.0f);
        if (pressureGap > 0.25f)
        {
            const BotVec2 toward = NormalizeOrZero(field.escapeX - ctx.player->positionCenter.x, field.escapeY - ctx.player->positionCenter.y);
            Accumulate(force, toward, pressureGap * kEscapeLaneAttractionScale);
        }
    }

    for (int sector = 0; sector < kFarSectorCount; ++sector)
    {
        const float pressure = field.farSectorPressure[sector];
        if (pressure <= 0.01f)
        {
            continue;
        }
        const float angle = ((static_cast<float>(sector) + 0.5f) / static_cast<float>(kFarSectorCount)) * (2.0f * ZUN_PI);
        const BotVec2 away = NormalizeOrZero(-std::cos(angle), -std::sin(angle));
        Accumulate(force, away, pressure * kFarDensityForceScale);
    }
    if (field.hasStrategicLane)
    {
        const float laneDelta = std::fabs(ctx.player->positionCenter.x - field.strategicLaneX) /
                                std::max((ctx.maxX - ctx.minX) / static_cast<float>(kGlobalLaneCount), 1.0f);
        const BotVec2 toward = NormalizeOrZero(field.strategicLaneX - ctx.player->positionCenter.x, 0.0f);
        Accumulate(force, toward, std::max(laneDelta, 0.15f) * kGlobalLaneAttractionScale);
    }
    if (field.hasTopCurtainGap)
    {
        const float laneWidth = (ctx.maxX - ctx.minX) / static_cast<float>(kGlobalLaneCount);
        const float gapDelta = std::fabs(ctx.player->positionCenter.x - field.topCurtainGapX) / std::max(laneWidth, 1.0f);
        const BotVec2 toward = NormalizeOrZero(field.topCurtainGapX - ctx.player->positionCenter.x, 0.0f);
        Accumulate(force, toward, std::max(gapDelta, 0.12f) * kTopCurtainGapAttractionScale);
    }
    return force;
}

float ComputeDensityPenaltyAt(const DensityField &field, float x, float y)
{
    const float half = (static_cast<float>(kDensityGridSize) - 1.0f) * 0.5f;
    const float fx = (x - field.originX) / field.cellSize + half;
    const float fy = (y - field.originY) / field.cellSize + half;
    const int ix = static_cast<int>(std::floor(fx));
    const int iy = static_cast<int>(std::floor(fy));
    float penalty = 0.0f;
    for (int oy = -1; oy <= 1; ++oy)
    {
        for (int ox = -1; ox <= 1; ++ox)
        {
            const int gx = ix + ox;
            const int gy = iy + oy;
            if (gx < 0 || gx >= kDensityGridSize || gy < 0 || gy >= kDensityGridSize)
            {
                continue;
            }
            const float localX = field.originX + (static_cast<float>(gx) - half) * field.cellSize;
            const float localY = field.originY + (static_cast<float>(gy) - half) * field.cellSize;
            const float distSq = std::max(DistanceSq2D(x, y, localX, localY), 36.0f);
            penalty += field.values[gy][gx] / std::sqrt(distSq);
        }
    }
    return penalty * kDensityPenaltyScale;
}

float SampleDensityAt(const DensityField &field, float x, float y)
{
    const float half = (static_cast<float>(kDensityGridSize) - 1.0f) * 0.5f;
    const float fx = (x - field.originX) / field.cellSize + half;
    const float fy = (y - field.originY) / field.cellSize + half;
    const int ix = static_cast<int>(std::floor(fx));
    const int iy = static_cast<int>(std::floor(fy));
    float sample = 0.0f;
    for (int oy = 0; oy <= 1; ++oy)
    {
        for (int ox = 0; ox <= 1; ++ox)
        {
            const int gx = ix + ox;
            const int gy = iy + oy;
            if (gx < 0 || gx >= kDensityGridSize || gy < 0 || gy >= kDensityGridSize)
            {
                continue;
            }
            const float wx = 1.0f - std::fabs(fx - static_cast<float>(gx));
            const float wy = 1.0f - std::fabs(fy - static_cast<float>(gy));
            sample += field.values[gy][gx] * Clamp01(wx) * Clamp01(wy);
        }
    }
    return sample;
}

float ComputeEscapeLanePathPressure(const DensityField &field, float fromX, float fromY, float toX, float toY)
{
    float pressure = 0.0f;
    constexpr int kPathSamples = 6;
    for (int idx = 0; idx < kPathSamples; ++idx)
    {
        const float t = kPathSamples == 1 ? 1.0f : static_cast<float>(idx) / static_cast<float>(kPathSamples - 1);
        const float x = fromX + (toX - fromX) * t;
        const float y = fromY + (toY - fromY) * t;
        pressure += SampleDensityAt(field, x, y);
    }
    return pressure;
}

void FinalizeEscapeLane(const BotContext &ctx, DensityField &field)
{
    if (ctx.player == nullptr)
    {
        return;
    }

    const float px = ctx.player->positionCenter.x;
    const float py = ctx.player->positionCenter.y;
    const float baselineY =
        std::clamp(std::max(py + 72.0f, ctx.minY + (ctx.maxY - ctx.minY) * 0.68f), ctx.minY + 24.0f, ctx.maxY - 24.0f);
    field.localPressure = ComputeEscapeLanePathPressure(field, px, py, px, baselineY);

    float bestPressure = std::numeric_limits<float>::infinity();
    float bestX = px;
    const float left = ctx.minX + 20.0f;
    const float right = ctx.maxX - 20.0f;
    for (int lane = 0; lane < kEscapeLaneCount; ++lane)
    {
        const float t = kEscapeLaneCount == 1 ? 0.5f : static_cast<float>(lane) / static_cast<float>(kEscapeLaneCount - 1);
        const float laneX = left + (right - left) * t;
        float lanePressure = ComputeEscapeLanePathPressure(field, px, py, laneX, baselineY);
        lanePressure += std::fabs(laneX - (left + right) * 0.5f) * 0.015f;
        if (lanePressure < bestPressure)
        {
            bestPressure = lanePressure;
            bestX = laneX;
        }
    }

    if (std::isfinite(bestPressure))
    {
        field.hasEscapeLane = true;
        field.escapeX = bestX;
        field.escapeY = baselineY;
        field.escapePressure = bestPressure;
    }

    float bestLanePressure = std::numeric_limits<float>::infinity();
    float bestLaneX = px;
    const float laneWidth = (ctx.maxX - ctx.minX) / static_cast<float>(kGlobalLaneCount);
    for (int lane = 0; lane < kGlobalLaneCount; ++lane)
    {
        const float laneCenter = ctx.minX + (static_cast<float>(lane) + 0.5f) * laneWidth;
        float pressure = field.globalLanePressure[lane] + field.laserLanePressure[lane] * 1.6f;
        pressure += std::fabs(laneCenter - (ctx.minX + ctx.maxX) * 0.5f) * 0.02f;
        if (pressure < bestLanePressure)
        {
            bestLanePressure = pressure;
            bestLaneX = laneCenter;
        }
    }
    if (std::isfinite(bestLanePressure))
    {
        field.hasStrategicLane = true;
        field.strategicLaneX = bestLaneX;
        field.strategicLanePressure = bestLanePressure;
    }

    float curtainTotal = 0.0f;
    for (int lane = 0; lane < kGlobalLaneCount; ++lane)
    {
        curtainTotal += field.topCurtainLanePressure[lane];
    }
    if (curtainTotal > 8.0f)
    {
        float bestGapPressure = std::numeric_limits<float>::infinity();
        float bestGapX = px;
        for (int lane = 0; lane < kGlobalLaneCount; ++lane)
        {
            const float laneCenter = ctx.minX + (static_cast<float>(lane) + 0.5f) * laneWidth;
            float pressure = field.topCurtainLanePressure[lane];
            pressure += std::fabs(laneCenter - px) * 0.012f;
            if (pressure < bestGapPressure)
            {
                bestGapPressure = pressure;
                bestGapX = laneCenter;
            }
        }
        field.hasTopCurtainGap = true;
        field.topCurtainGapX = bestGapX;
        field.topCurtainGapPressure = bestGapPressure;
    }
}

float ComputeEscapeLanePenaltyAt(const DensityField &field, float x, float y)
{
    if (!field.hasEscapeLane)
    {
        return 0.0f;
    }

    const float pressureGap = std::max(field.localPressure - field.escapePressure, 0.0f);
    if (pressureGap <= 0.25f)
    {
        return 0.0f;
    }

    const float dist = std::sqrt(std::max(DistanceSq2D(x, y, field.escapeX, field.escapeY), 1.0f));
    const float strength = Clamp01(pressureGap / 18.0f);
    return dist * kEscapeLanePenaltyScale * 0.018f * strength;
}

float ComputeStrategicLanePenaltyAt(const BotContext &ctx, const DensityField &field, float x)
{
    if (!field.hasStrategicLane)
    {
        return 0.0f;
    }
    const float laneWidth = (ctx.maxX - ctx.minX) / static_cast<float>(kGlobalLaneCount);
    const float dist = std::fabs(x - field.strategicLaneX);
    const float laneDelta = dist / std::max(laneWidth, 1.0f);
    return laneDelta * kGlobalLanePenaltyScale;
}

float ComputeTopCurtainPenaltyAt(const BotContext &ctx, const DensityField &field, float x)
{
    if (!field.hasTopCurtainGap)
    {
        return 0.0f;
    }
    const float laneWidth = (ctx.maxX - ctx.minX) / static_cast<float>(kGlobalLaneCount);
    const float dist = std::fabs(x - field.topCurtainGapX);
    return (dist / std::max(laneWidth, 1.0f)) * kTopCurtainPenaltyScale;
}

float ComputeLaserLanePenaltyAt(const BotContext &ctx, const DensityField &field, float x)
{
    const float laneWidth = (ctx.maxX - ctx.minX) / static_cast<float>(kGlobalLaneCount);
    const float laneFloat = (x - ctx.minX) / std::max(laneWidth, 1.0f);
    const int lane = std::clamp(static_cast<int>(std::floor(laneFloat)), 0, kGlobalLaneCount - 1);
    const int nextLane = std::min(lane + 1, kGlobalLaneCount - 1);
    const float blend = Clamp01(laneFloat - std::floor(laneFloat));
    const float pressure = field.laserLanePressure[lane] * (1.0f - blend) + field.laserLanePressure[nextLane] * blend;
    return pressure * kLaserLanePenaltyScale;
}

float ComputeCornerTrapPenaltyAt(const BotContext &ctx, const DensityField &field, float x, float y)
{
    const float leftTrap = Clamp01((ctx.minX + 42.0f - x) / 42.0f);
    const float rightTrap = Clamp01((x - (ctx.maxX - 42.0f)) / 42.0f);
    const float topTrap = Clamp01((ctx.minY + 42.0f - y) / 42.0f);
    const float bottomTrap = Clamp01((y - (ctx.maxY - 42.0f)) / 42.0f);
    const float horizontalTrap = std::max(leftTrap, rightTrap);
    const float verticalTrap = std::max(topTrap, bottomTrap);
    if (horizontalTrap <= 0.0f && verticalTrap <= 0.0f)
    {
        return 0.0f;
    }

    const float localPressure = ComputeDensityPenaltyAt(field, x, y);
    const float lanePenalty = ComputeLaserLanePenaltyAt(ctx, field, x);
    const float trapStrength = std::max(horizontalTrap + verticalTrap, horizontalTrap * 1.3f + bottomTrap * 1.4f);
    return trapStrength * (localPressure * 0.18f + lanePenalty * 0.25f + kCornerTrapPenaltyScale);
}

bool IsSqueezedAt(const BotContext &ctx, const DensityField &field, float x, float y, float localDanger)
{
    const bool nearHorizontalEdge = x <= ctx.minX + 54.0f || x >= ctx.maxX - 54.0f;
    const bool nearVerticalEdge = y <= ctx.minY + 54.0f || y >= ctx.maxY - 54.0f;
    if (!nearHorizontalEdge && !nearVerticalEdge)
    {
        return false;
    }

    const float density = ComputeDensityPenaltyAt(field, x, y);
    const float cornerPenalty = ComputeCornerTrapPenaltyAt(ctx, field, x, y);
    return localDanger >= kSqueezeDangerThreshold || density >= kSqueezeDensityThreshold || cornerPenalty >= kCornerTrapPenaltyScale;
}

bool ActionPushesTowardNearestBoundary(const BotContext &ctx, float x, float y, Action action)
{
    const float leftDist = x - ctx.minX;
    const float rightDist = ctx.maxX - x;
    const float topDist = y - ctx.minY;
    const float bottomDist = ctx.maxY - y;

    if (leftDist <= rightDist && leftDist <= 54.0f)
    {
        return action == Action::Left || action == Action::UpLeft || action == Action::DownLeft;
    }
    if (rightDist < leftDist && rightDist <= 54.0f)
    {
        return action == Action::Right || action == Action::UpRight || action == Action::DownRight;
    }
    if (bottomDist <= topDist && bottomDist <= 54.0f)
    {
        return action == Action::Down || action == Action::DownLeft || action == Action::DownRight;
    }
    if (topDist < bottomDist && topDist <= 54.0f)
    {
        return action == Action::Up || action == Action::UpLeft || action == Action::UpRight;
    }
    return false;
}

bool ActionMovesAwayFromTopCurtainGap(const DensityField &field, float x, Action action)
{
    if (!field.hasTopCurtainGap)
    {
        return false;
    }
    const float dx = field.topCurtainGapX - x;
    if (std::fabs(dx) < 18.0f)
    {
        return false;
    }
    if (dx > 0.0f)
    {
        return action == Action::Left || action == Action::UpLeft || action == Action::DownLeft;
    }
    return action == Action::Right || action == Action::UpRight || action == Action::DownRight;
}

bool ShouldHardRejectAction(const BotContext &ctx, const DensityField &densityField, float x, float y, float localDanger, Action action)
{
    if (ActionPressesOutwardBoundary(ctx, action))
    {
        return true;
    }
    if (IsSqueezedAt(ctx, densityField, x, y, localDanger) && ActionPushesTowardNearestBoundary(ctx, x, y, action))
    {
        return true;
    }
    if (densityField.hasTopCurtainGap && ActionMovesAwayFromTopCurtainGap(densityField, x, action))
    {
        return true;
    }
    return false;
}

float ComputeFarSectorPenaltyAt(const DensityField &field, float x, float y)
{
    const float dx = x - field.originX;
    const float dy = y - field.originY;
    if ((dx * dx + dy * dy) <= 1.0f)
    {
        return 0.0f;
    }

    float angle = std::atan2(dy, dx);
    if (angle < 0.0f)
    {
        angle += 2.0f * ZUN_PI;
    }
    const float sectorFloat = angle / (2.0f * ZUN_PI) * static_cast<float>(kFarSectorCount);
    const int sector = static_cast<int>(std::floor(sectorFloat)) % kFarSectorCount;
    const int nextSector = (sector + 1) % kFarSectorCount;
    const float blend = sectorFloat - std::floor(sectorFloat);
    const float pressure =
        field.farSectorPressure[sector] * (1.0f - blend) + field.farSectorPressure[nextSector] * blend;
    return pressure * kFarDensityPenaltyScale;
}

float ComputeForecastConeInfluence(const AstroBotEclForecast::ForecastThreat &threat, float x, float y)
{
    const float dx = x - threat.origin.x;
    const float dy = y - threat.origin.y;
    const float dist = std::sqrt(std::max(dx * dx + dy * dy, 1.0f));
    const float pointAngle = std::atan2(dy, dx);
    const float angleDiff = WrapAngleDiff(pointAngle, threat.angleCenter);
    const float coneSlack = threat.angleHalfRange + std::atan2(std::max(threat.width, 6.0f), std::max(dist, 12.0f));
    if (angleDiff > coneSlack)
    {
        return 0.0f;
    }

    const float rangeMin = std::max(0.0f, threat.rangeMin - threat.width * 1.2f);
    const float rangeMax = std::max(rangeMin + 1.0f, threat.rangeMax + threat.width * 1.2f);
    if (dist < rangeMin || dist > rangeMax)
    {
        const float edgeDist = dist < rangeMin ? (rangeMin - dist) : (dist - rangeMax);
        if (edgeDist > threat.width * 2.5f)
        {
            return 0.0f;
        }
    }

    const float angularFactor = 1.0f - Clamp01(angleDiff / std::max(coneSlack, 0.001f));
    const float radialCenter = (rangeMin + rangeMax) * 0.5f;
    const float radialHalf = std::max((rangeMax - rangeMin) * 0.5f, threat.width);
    const float radialFactor = 1.0f - Clamp01(std::fabs(dist - radialCenter) / std::max(radialHalf * 1.4f, 1.0f));
    return threat.weight * std::max(angularFactor * 0.75f + radialFactor * 0.25f, 0.0f);
}

void DepositForecastThreats(const BotContext &ctx, DensityField &field)
{
    constexpr int kForecastSamples = 7;
    for (int idx = 0; idx < ctx.forecast.count; ++idx)
    {
        const auto &threat = ctx.forecast.threats[idx];
        if (!threat.valid)
        {
            continue;
        }

        const float rangeMid = (threat.rangeMin + threat.rangeMax) * 0.5f;
        const float baseWeight = threat.weight * (threat.isLaser ? 1.6f : 1.0f);
        for (int s = 0; s < kForecastSamples; ++s)
        {
            const float t = kForecastSamples == 1 ? 0.5f : static_cast<float>(s) / static_cast<float>(kForecastSamples - 1);
            const float angle = threat.angleCenter - threat.angleHalfRange + (threat.angleHalfRange * 2.0f) * t;
            const float radius = rangeMid;
            DepositDensity(field, threat.origin.x + std::cos(angle) * radius, threat.origin.y + std::sin(angle) * radius, baseWeight);
            if (threat.isLaser)
            {
                DepositDensity(field, threat.origin.x + std::cos(angle) * threat.rangeMax,
                               threat.origin.y + std::sin(angle) * threat.rangeMax, baseWeight * 1.15f);
            }
        }
    }
}

float ComputeForecastDangerAt(const BotContext &ctx, float x, float y)
{
    float danger = 0.0f;
    for (int idx = 0; idx < ctx.forecast.count; ++idx)
    {
        const auto &threat = ctx.forecast.threats[idx];
        if (!threat.valid)
        {
            continue;
        }
        const float influence = ComputeForecastConeInfluence(threat, x, y);
        if (influence <= 0.0f)
        {
            continue;
        }
        danger += influence * (threat.isLaser ? 220.0f : 120.0f);
    }
    return danger;
}

BotVec2 GenerateForecastForce(const BotContext &ctx)
{
    if (ctx.player == nullptr)
    {
        return {};
    }

    BotVec2 force {};
    const float px = ctx.player->positionCenter.x;
    const float py = ctx.player->positionCenter.y;
    for (int idx = 0; idx < ctx.forecast.count; ++idx)
    {
        const auto &threat = ctx.forecast.threats[idx];
        if (!threat.valid)
        {
            continue;
        }
        const float influence = ComputeForecastConeInfluence(threat, px, py);
        if (influence <= 0.0f)
        {
            continue;
        }
        const float rangeMid = (threat.rangeMin + threat.rangeMax) * 0.5f;
        const float cx = threat.origin.x + std::cos(threat.angleCenter) * rangeMid;
        const float cy = threat.origin.y + std::sin(threat.angleCenter) * rangeMid;
        const BotVec2 away = NormalizeOrZero(px - cx, py - cy);
        Accumulate(force, away, influence * (threat.isLaser ? 14.0f : 8.0f));
    }
    return force;
}

float BulletRadiusForWall(const Bullet &bullet)
{
    return std::max(std::max(bullet.sprites.grazeSize.x, bullet.sprites.grazeSize.y) * 0.5f, 4.0f);
}

int BuildVirtualWalls(const BotContext &ctx, VirtualWall *outWalls)
{
    if (ctx.player == nullptr)
    {
        return 0;
    }

    const float px = ctx.player->positionCenter.x;
    const float py = ctx.player->positionCenter.y;
    const float rangeSq = kWallRecognitionRange * kWallRecognitionRange;
    int wallCount = 0;

    auto addWall = [&](BotVec2 start, BotVec2 end, float radius) {
        if (wallCount >= kMaxVirtualWalls)
        {
            return;
        }
        outWalls[wallCount++] = {true, start, end, std::max(radius, 2.0f)};
    };

    for (int i = 0; i < ARRAY_SIZE_SIGNED(g_BulletManager.bullets) - 1; ++i)
    {
        const Bullet &a = g_BulletManager.bullets[i];
        if (a.state == 0 || DistanceSq2D(px, py, a.pos.x, a.pos.y) > rangeSq)
        {
            continue;
        }
        const float aRadius = BulletRadiusForWall(a);
        for (int j = i + 1; j < ARRAY_SIZE_SIGNED(g_BulletManager.bullets) - 1; ++j)
        {
            const Bullet &b = g_BulletManager.bullets[j];
            if (b.state == 0 || DistanceSq2D(px, py, b.pos.x, b.pos.y) > rangeSq)
            {
                continue;
            }
            if (WrapAngleDiff(a.angle, b.angle) > kWallAngleTolerance)
            {
                continue;
            }
            const float bRadius = BulletRadiusForWall(b);
            const float pairDistSq = DistanceSq2D(a.pos.x, a.pos.y, b.pos.x, b.pos.y);
            const float allowed = aRadius + bRadius + kBulletWallGap;
            if (pairDistSq > allowed * allowed * 2.25f)
            {
                continue;
            }
            addWall({a.pos.x, a.pos.y}, {b.pos.x, b.pos.y}, std::max(aRadius, bRadius) + kBulletWallGap * 0.5f);
        }
    }

    for (int i = 0; i < wallCount; ++i)
    {
        if (!outWalls[i].valid)
        {
            continue;
        }
        for (int j = i + 1; j < wallCount; ++j)
        {
            if (!outWalls[j].valid)
            {
                continue;
            }
            const BotVec2 aDir = NormalizeOrZero(outWalls[i].end.x - outWalls[i].start.x, outWalls[i].end.y - outWalls[i].start.y);
            const BotVec2 bDir = NormalizeOrZero(outWalls[j].end.x - outWalls[j].start.x, outWalls[j].end.y - outWalls[j].start.y);
            if (Dot2D(aDir.x, aDir.y, bDir.x, bDir.y) < 0.94f)
            {
                continue;
            }
            const float linkDistSq = std::min(
                std::min(DistanceSq2D(outWalls[i].start.x, outWalls[i].start.y, outWalls[j].start.x, outWalls[j].start.y),
                         DistanceSq2D(outWalls[i].start.x, outWalls[i].start.y, outWalls[j].end.x, outWalls[j].end.y)),
                std::min(DistanceSq2D(outWalls[i].end.x, outWalls[i].end.y, outWalls[j].start.x, outWalls[j].start.y),
                         DistanceSq2D(outWalls[i].end.x, outWalls[i].end.y, outWalls[j].end.x, outWalls[j].end.y)));
            if (linkDistSq > (kBulletWallGap * 3.0f) * (kBulletWallGap * 3.0f))
            {
                continue;
            }

            BotVec2 pts[4] = {outWalls[i].start, outWalls[i].end, outWalls[j].start, outWalls[j].end};
            int minIdx = 0;
            int maxIdx = 0;
            float minProj = Dot2D(pts[0].x, pts[0].y, aDir.x, aDir.y);
            float maxProj = minProj;
            for (int k = 1; k < 4; ++k)
            {
                const float proj = Dot2D(pts[k].x, pts[k].y, aDir.x, aDir.y);
                if (proj < minProj)
                {
                    minProj = proj;
                    minIdx = k;
                }
                if (proj > maxProj)
                {
                    maxProj = proj;
                    maxIdx = k;
                }
            }
            outWalls[i].start = pts[minIdx];
            outWalls[i].end = pts[maxIdx];
            outWalls[i].radius = std::max(outWalls[i].radius, outWalls[j].radius);
            outWalls[j].valid = false;
        }
    }

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.bullets) - 1 && wallCount < kMaxVirtualWalls; ++idx)
    {
        const Bullet &bullet = g_BulletManager.bullets[idx];
        if (bullet.state == 0 || DistanceSq2D(px, py, bullet.pos.x, bullet.pos.y) > rangeSq)
        {
            continue;
        }
        const float radius = BulletRadiusForWall(bullet);
        const float cosA = std::cos(bullet.angle);
        const float sinA = std::sin(bullet.angle);
        if (bullet.pos.x - ctx.minX <= radius + kBulletWallGap && std::fabs(sinA) > 0.72f)
        {
            addWall({ctx.minX, bullet.pos.y - radius - kBulletWallGap}, {ctx.minX, bullet.pos.y + radius + kBulletWallGap}, radius);
        }
        if (ctx.maxX - bullet.pos.x <= radius + kBulletWallGap && std::fabs(sinA) > 0.72f)
        {
            addWall({ctx.maxX, bullet.pos.y - radius - kBulletWallGap}, {ctx.maxX, bullet.pos.y + radius + kBulletWallGap}, radius);
        }
        if (bullet.pos.y - ctx.minY <= radius + kBulletWallGap && std::fabs(cosA) > 0.72f)
        {
            addWall({bullet.pos.x - radius - kBulletWallGap, ctx.minY}, {bullet.pos.x + radius + kBulletWallGap, ctx.minY}, radius);
        }
        if (ctx.maxY - bullet.pos.y <= radius + kBulletWallGap && std::fabs(cosA) > 0.72f)
        {
            addWall({bullet.pos.x - radius - kBulletWallGap, ctx.maxY}, {bullet.pos.x + radius + kBulletWallGap, ctx.maxY}, radius);
        }
    }

    return wallCount;
}

float ComputeVerticalSafeZoneBias(const BotContext &ctx, float y)
{
    const float height = std::max(ctx.maxY - ctx.minY, 1.0f);
    const float safeCenter = ctx.minY + height * 0.78f;
    const float safeHalfSpan = height * 0.16f;
    const float dist = std::fabs(y - safeCenter);
    float bias = dist * 0.10f;
    if (y > ctx.maxY - kBottomComfortBand)
    {
        bias += (y - (ctx.maxY - kBottomComfortBand)) * 0.85f;
    }
    return bias;
}

bool ActionPressesOutwardBoundary(const BotContext &ctx, Action action)
{
    if (ctx.player == nullptr)
    {
        return false;
    }

    const float x = ctx.player->positionCenter.x;
    const float y = ctx.player->positionCenter.y;
    const bool nearLeft = x <= ctx.minX + kEdgeOutwardGuard;
    const bool nearRight = x >= ctx.maxX - kEdgeOutwardGuard;
    const bool nearTop = y <= ctx.minY + kEdgeOutwardGuard;
    const bool nearBottom = y >= ctx.maxY - kEdgeOutwardGuard;

    switch (action)
    {
    case Action::Left: return nearLeft;
    case Action::Right: return nearRight;
    case Action::Up: return nearTop;
    case Action::Down: return nearBottom;
    case Action::UpLeft: return nearTop || nearLeft;
    case Action::UpRight: return nearTop || nearRight;
    case Action::DownLeft: return nearBottom || nearLeft;
    case Action::DownRight: return nearBottom || nearRight;
    case Action::Idle:
    default: return false;
    }
}

const Player *GetTargetPlayer(Target target)
{
    switch (target)
    {
    case Target::P1: return &g_Player;
    case Target::P2: return Session::IsDualPlayerSession() ? &g_Player2 : nullptr;
    case Target::Off:
    default: return nullptr;
    }
}

bool IsDeathbombWindow(const Player &player)
{
    return player.playerState == PLAYER_STATE_DEAD && player.respawnTimer > 0;
}

bool CanOperateOnPlayer(const Player &player)
{
    return player.playerState == PLAYER_STATE_ALIVE || player.playerState == PLAYER_STATE_INVULNERABLE ||
           IsDeathbombWindow(player);
}

bool IsPortableRestoreBusy()
{
    using Phase = PortableGameplayRestore::Phase;
    switch (PortableGameplayRestore::GetPortableRestorePhase())
    {
    case Phase::PendingDecode:
    case Phase::PendingBootstrap:
    case Phase::WaitingForGameplayShell:
    case Phase::Applying:
    case Phase::SyncingShell: return true;
    case Phase::Idle:
    case Phase::Completed:
    case Phase::Failed:
    default: return false;
    }
}

bool IsEligible(Target requestedTarget, const InputOwnership &ownership, BypassReason &reason)
{
    if (!THPrac::TH06::THPracIsDeveloperModeEnabled() || !THPrac::TH06::THPracIsAstroBotEnabled())
    {
        reason = BypassReason::Disabled;
        return false;
    }
    if (requestedTarget == Target::Off)
    {
        reason = BypassReason::NoTarget;
        return false;
    }
    if (g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER)
    {
        reason = BypassReason::NotGameplay;
        return false;
    }
    if (ownership.remoteSession && (!Netplay::g_State.isSessionActive || !Netplay::g_State.isConnected))
    {
        reason = BypassReason::NetplayInactive;
        return false;
    }
    if (g_GameManager.isInReplay || g_GameManager.demoMode)
    {
        reason = BypassReason::ReplayOrDemo;
        return false;
    }
    if (g_GameManager.isInGameMenu || g_GameManager.isInRetryMenu)
    {
        reason = BypassReason::SharedShell;
        return false;
    }
    if (IsPortableRestoreBusy())
    {
        reason = BypassReason::PortableRestoreBusy;
        return false;
    }
    const Player *player = GetTargetPlayer(ownership.controlledTarget);
    if (player == nullptr)
    {
        reason = BypassReason::NoPlayer;
        return false;
    }
    if (!CanOperateOnPlayer(*player))
    {
        reason = BypassReason::PlayerInactive;
        return false;
    }
    reason = BypassReason::None;
    return true;
}

float ItemPriorityValue(i8 itemType)
{
    switch (itemType)
    {
    case ITEM_LIFE: return 12.0f;
    case ITEM_BOMB: return 10.0f;
    case ITEM_FULL_POWER: return 8.0f;
    case ITEM_POWER_BIG: return 5.0f;
    case ITEM_POINT_BULLET: return 4.0f;
    case ITEM_POWER_SMALL: return 3.0f;
    case ITEM_POINT: return 2.0f;
    default: return 0.0f;
    }
}

const Enemy *FindTrackedEnemy(const Player &player, bool bossOnly)
{
    const Enemy *best = nullptr;
    float bestDist = 0.0f;
    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_EnemyManager.enemies) - 1; ++idx)
    {
        const Enemy &enemy = g_EnemyManager.enemies[idx];
        if (enemy.life <= 0)
        {
            continue;
        }
        if (bossOnly && !enemy.flags.isBoss)
        {
            continue;
        }
        const float dist = DistanceSq2D(player.positionCenter.x, player.positionCenter.y, enemy.position.x, enemy.position.y);
        if (best == nullptr || dist < bestDist)
        {
            best = &enemy;
            bestDist = dist;
        }
    }
    return best;
}

void CaptureTrackedItems(const Player &player, BotContext &ctx)
{
    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_ItemManager.items) - 1; ++idx)
    {
        const Item &item = g_ItemManager.items[idx];
        if (item.isInUse == 0 || ctx.itemCount >= ARRAY_SIZE_SIGNED(ctx.items))
        {
            continue;
        }
        const float priority = ItemPriorityValue(item.itemType);
        if (priority <= 0.0f)
        {
            continue;
        }
        const float distSq =
            DistanceSq2D(player.positionCenter.x, player.positionCenter.y, item.currentPosition.x, item.currentPosition.y);
        if (distSq > GAME_REGION_HEIGHT * GAME_REGION_HEIGHT * 0.75f)
        {
            continue;
        }
        ItemSample &sample = ctx.items[ctx.itemCount++];
        sample.valid = true;
        sample.pos = {item.currentPosition.x, item.currentPosition.y};
        sample.priority = priority;
    }
}

BotContext CaptureContext(const InputOwnership &ownership)
{
    BotContext ctx;
    ctx.target = ownership.controlledTarget;
    ctx.player = GetTargetPlayer(ownership.controlledTarget);
    ctx.bossOnly = THPrac::TH06::THPracIsAstroBotBossOnlyEnabled();
    ctx.autoShoot = THPrac::TH06::THPracIsAstroBotAutoShootEnabled();
    ctx.autoBomb = THPrac::TH06::THPracIsAstroBotAutoBombEnabled();
    ctx.effectiveFramerateMultiplier = g_Supervisor.effectiveFramerateMultiplier;
    ctx.minX = g_GameManager.playerMovementAreaTopLeftPos.x;
    ctx.maxX = g_GameManager.playerMovementAreaTopLeftPos.x + g_GameManager.playerMovementAreaSize.x;
    ctx.minY = g_GameManager.playerMovementAreaTopLeftPos.y;
    ctx.maxY = g_GameManager.playerMovementAreaTopLeftPos.y + g_GameManager.playerMovementAreaSize.y;
    if (ctx.player != nullptr)
    {
        ctx.bombsRemaining = ownership.controlledTarget == Target::P2 ? g_GameManager.bombsRemaining2 : g_GameManager.bombsRemaining;
        const Enemy *enemy = FindTrackedEnemy(*ctx.player, ctx.bossOnly);
        if (enemy != nullptr)
        {
            ctx.enemy.valid = true;
            ctx.enemy.pos = {enemy->position.x, enemy->position.y};
            ctx.enemy.isBoss = enemy->flags.isBoss != 0;
        }
        CaptureTrackedItems(*ctx.player, ctx);
        if (!ownership.remoteSession)
        {
            ctx.forecast = AstroBotEclForecast::BuildForecastSnapshot(*ctx.player);
        }
    }
    return ctx;
}

void ApplyActionVelocity(const Player &player, Action action, bool focus, float multiplier, float &dx, float &dy)
{
    dx = 0.0f;
    dy = 0.0f;
    const float ortho = focus ? player.characterData.orthogonalMovementSpeedFocus : player.characterData.orthogonalMovementSpeed;
    const float diagonal = focus ? player.characterData.diagonalMovementSpeedFocus : player.characterData.diagonalMovementSpeed;
    switch (action)
    {
    case Action::Up: dy = -ortho; break;
    case Action::Down: dy = ortho; break;
    case Action::Left: dx = -ortho; break;
    case Action::Right: dx = ortho; break;
    case Action::UpLeft:
        dx = -diagonal;
        dy = -diagonal;
        break;
    case Action::UpRight:
        dx = diagonal;
        dy = -diagonal;
        break;
    case Action::DownLeft:
        dx = -diagonal;
        dy = diagonal;
        break;
    case Action::DownRight:
        dx = diagonal;
        dy = diagonal;
        break;
    case Action::Idle:
    default: break;
    }
    dx *= multiplier * player.horizontalMovementSpeedMultiplierDuringBomb;
    dy *= multiplier * player.verticalMovementSpeedMultiplierDuringBomb;
}

bool IsLaserActiveAt(const Laser &laser, int futureStep)
{
    if (laser.inUse == 0 || laser.state > 2)
    {
        return false;
    }
    const int frame = laser.timer.current + futureStep;
    if (frame < laser.hitboxStartTime)
    {
        return false;
    }
    if (laser.duration > 0 && frame > laser.duration + laser.hitboxEndDelay)
    {
        return false;
    }
    return true;
}

bool IsLaserThreateningSoon(const Laser &laser, int futureStep)
{
    if (laser.inUse == 0 || laser.state > 2)
    {
        return false;
    }
    if (IsLaserActiveAt(laser, futureStep))
    {
        return true;
    }
    const int frame = laser.timer.current + futureStep;
    return frame < laser.hitboxStartTime && (laser.hitboxStartTime - frame) <= kLaserWarningLeadFrames;
}

void GetLaserSegment(const Laser &laser, float multiplier, int futureStep, BotVec2 &start, BotVec2 &end, float &halfWidth)
{
    if (!std::isfinite(laser.pos.x) || !std::isfinite(laser.pos.y) || !std::isfinite(laser.angle) ||
        !std::isfinite(laser.startOffset) || !std::isfinite(laser.endOffset) || !std::isfinite(laser.speed) ||
        !std::isfinite(laser.width))
    {
        start = {};
        end = {};
        halfWidth = 1.0f;
        return;
    }
    const float advance = laser.speed * multiplier * static_cast<float>(futureStep);
    const float cosA = std::cos(laser.angle);
    const float sinA = std::sin(laser.angle);
    const float startOffset = std::max(0.0f, laser.startOffset + advance);
    const float endOffset = std::max(startOffset, laser.endOffset + advance);
    start.x = laser.pos.x + cosA * startOffset;
    start.y = laser.pos.y + sinA * startOffset;
    end.x = laser.pos.x + cosA * endOffset;
    end.y = laser.pos.y + sinA * endOffset;
    halfWidth = std::max(1.0f, laser.width * 0.5f);
}

void BuildLaserProbeShape(const Laser &laser, float multiplier, int futureStep, D3DXVECTOR3 &laserCenter, D3DXVECTOR3 &laserSize,
                          D3DXVECTOR3 &rotation)
{
    if (!std::isfinite(laser.pos.x) || !std::isfinite(laser.pos.y) || !std::isfinite(laser.angle) ||
        !std::isfinite(laser.startOffset) || !std::isfinite(laser.endOffset) || !std::isfinite(laser.startLength) ||
        !std::isfinite(laser.width) || !std::isfinite(laser.speed))
    {
        laserCenter = {};
        laserSize = {0.6f, 1.0f, 0.0f};
        rotation = {};
        return;
    }
    const float advance = laser.speed * multiplier * static_cast<float>(futureStep);
    const float advancedEnd = laser.endOffset + advance;
    float advancedStart = laser.startOffset;
    if (laser.startLength < advancedEnd - advancedStart)
    {
        advancedStart = advancedEnd - laser.startLength;
    }
    if (advancedStart < 0.0f)
    {
        advancedStart = 0.0f;
    }

    laserCenter = {};
    laserSize = {};
    rotation = laser.pos;
    laserSize.y = laser.width * 0.5f;
    laserSize.x = std::max(0.0f, advancedEnd - advancedStart);
    laserCenter.x = (laserSize.x * 0.5f) + advancedStart + laser.pos.x;
    laserCenter.y = laser.pos.y;

    const int probeFrame = laser.timer.current + futureStep;
    if (laser.state == 0 && !(laser.flags & 1))
    {
        const int res = ZUN_MIN(laser.startTime, 30);
        if (laser.startTime > 0 && laser.startTime - res < probeFrame)
        {
            const float local14 = (static_cast<float>(probeFrame) * laser.width) / laser.startTime;
            laserSize.x = local14 * 0.5f;
        }
        else
        {
            laserSize.x = 0.6f;
        }
    }
    else if (laser.state == 2 && !(laser.flags & 1) && laser.despawnDuration > 0)
    {
        const float local14 = laser.width - ((static_cast<float>(probeFrame) * laser.width) / laser.despawnDuration);
        laserSize.x = std::max(0.0f, local14 * 0.5f);
    }
    laserSize.x = std::max(laserSize.x, 0.6f);
    laserSize.y = std::max(laserSize.y, 1.0f);
}

BotVec2 GenerateForce(float srcX, float srcY, float dstX, float dstY, float strength)
{
    const BotVec2 dir = NormalizeOrZero(dstX - srcX, dstY - srcY);
    return {dir.x * strength, dir.y * strength};
}

BotVec2 GenerateForce2(float srcX, float srcY, float dstX, float dstY, float strength)
{
    return GenerateForce(srcX, srcY, dstX, dstY, strength);
}

void ApplyBulletWallRecognition(const BotContext &ctx, const VirtualWall *walls, int wallCount, BotVec2 &force)
{
    if (ctx.player == nullptr)
    {
        return;
    }
    const float px = ctx.player->positionCenter.x;
    const float py = ctx.player->positionCenter.y;
    for (int idx = 0; idx < wallCount; ++idx)
    {
        const VirtualWall &wall = walls[idx];
        if (!wall.valid)
        {
            continue;
        }
        const float distSq =
            std::max(DistanceSqPointToSegment(px, py, wall.start.x, wall.start.y, wall.end.x, wall.end.y), 9.0f);
        const float lenSq = std::max(DistanceSq2D(wall.start.x, wall.start.y, wall.end.x, wall.end.y), 0.0001f);
        const float t = std::clamp(((px - wall.start.x) * (wall.end.x - wall.start.x) + (py - wall.start.y) * (wall.end.y - wall.start.y)) /
                                       lenSq,
                                   0.0f, 1.0f);
        const float nearestX = wall.start.x + (wall.end.x - wall.start.x) * t;
        const float nearestY = wall.start.y + (wall.end.y - wall.start.y) * t;
        const BotVec2 away = NormalizeOrZero(px - nearestX, py - nearestY);
        Accumulate(force, away, kWallDangerScale / distSq);
    }
}

float ComputeBoundaryDanger(const BotContext &ctx, float x, float y)
{
    float danger = 0.0f;
    if (x < ctx.minX + kBoundaryMargin)
    {
        danger += kWallDanger / std::max(x - ctx.minX, 1.0f);
    }
    if (x > ctx.maxX - kBoundaryMargin)
    {
        danger += kWallDanger / std::max(ctx.maxX - x, 1.0f);
    }
    if (y < ctx.minY + kBoundaryMargin)
    {
        danger += kWallDanger / std::max(y - ctx.minY, 1.0f);
    }
    if (y > ctx.maxY - kBoundaryMargin)
    {
        danger += kWallDanger / std::max(ctx.maxY - y, 1.0f);
    }
    return danger;
}

float GenerateDangerAt(const BotContext &ctx, const VirtualWall *walls, int wallCount, float x, float y)
{
    if (ctx.player == nullptr)
    {
        return 0.0f;
    }

    const float dynamicRange = kWarningRange + kDynamicRangeFactor * ctx.player->characterData.orthogonalMovementSpeed;
    const float playerRadius = std::max(ctx.player->hitboxSize.x, ctx.player->hitboxSize.y) + 4.0f;
    const float rangeSqBase = dynamicRange * dynamicRange;
    float danger = ComputeBoundaryDanger(ctx, x, y) + ComputeWallDangerAt(x, y, walls, wallCount);

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.bullets) - 1; ++idx)
    {
        const Bullet &bullet = g_BulletManager.bullets[idx];
        if (bullet.state == 0)
        {
            continue;
        }
        for (int step = 0; step < kDangerProspects; ++step)
        {
            const float bx = bullet.pos.x + bullet.velocity.x * ctx.effectiveFramerateMultiplier * static_cast<float>(step);
            const float by = bullet.pos.y + bullet.velocity.y * ctx.effectiveFramerateMultiplier * static_cast<float>(step);
            const float bulletRadius = BulletEffectiveRadius(bullet);
            const float distSq = DistanceSq2D(x, y, bx, by);
            const float hitRadius = playerRadius + bulletRadius;
            const float rangeSq = (rangeSqBase + bulletRadius * bulletRadius) * (1.0f + 0.45f * static_cast<float>(step));
            if (distSq > rangeSq)
            {
                continue;
            }
            if (distSq <= hitRadius * hitRadius)
            {
                return kFatalDanger;
            }
            const float effectiveSq = std::max(distSq - hitRadius * hitRadius, 9.0f);
            const float sizeFactor = 1.0f + std::min(bulletRadius * 0.09f, 2.6f);
            danger += static_cast<float>(kDangerWeights[step]) * sizeFactor / effectiveSq;
        }
    }

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.lasers) - 1; ++idx)
    {
        const Laser &laser = g_BulletManager.lasers[idx];
        if (!IsLaserThreateningSoon(laser, 0))
        {
            continue;
        }
        for (int step = 0; step < kDangerProspects; ++step)
        {
            if (!IsLaserThreateningSoon(laser, step))
            {
                continue;
            }
            BotVec2 a {};
            BotVec2 b {};
            float halfWidth = 1.0f;
            GetLaserSegment(laser, ctx.effectiveFramerateMultiplier, step, a, b, halfWidth);
            const float distSq = DistanceSqPointToSegment(x, y, a.x, a.y, b.x, b.y);
            const float hitRadius = halfWidth + playerRadius;
            const bool activeLaser = IsLaserActiveAt(laser, step);
            if (activeLaser && distSq <= hitRadius * hitRadius)
            {
                return kFatalDanger;
            }
            const float leadFactor = activeLaser ? 1.0f : 0.7f;
            const float warnRadius = hitRadius + (kWarningRange + halfWidth * 0.35f) * (1.0f + (1.0f - leadFactor));
            if (distSq <= warnRadius * warnRadius)
            {
                const float effectiveSq = std::max(distSq - hitRadius * hitRadius, 9.0f);
                const float laserFactor = (1.55f + std::min(halfWidth * 0.04f, 1.5f)) * leadFactor;
                danger += static_cast<float>(kDangerWeights[step]) * laserFactor / effectiveSq;
            }
        }
    }

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_EnemyManager.enemies) - 1; ++idx)
    {
        const Enemy &enemy = g_EnemyManager.enemies[idx];
        if (enemy.life <= 0)
        {
            continue;
        }
        const float distSq = DistanceSq2D(x, y, enemy.position.x, enemy.position.y);
        if (distSq <= std::max(enemy.hitboxDimensions.x, enemy.hitboxDimensions.y) * std::max(enemy.hitboxDimensions.x, enemy.hitboxDimensions.y))
        {
            return kFatalDanger;
        }
        if (distSq <= rangeSqBase * 2.0f)
        {
            danger += (kEnemyDangerBase * kEnemyForceFactor) / std::max(distSq, 32.0f);
        }
    }

    danger += ComputeForecastDangerAt(ctx, x, y);
    return danger;
}

BotVec2 GenerateDestPoint(const BotContext &ctx, const VirtualWall *walls, int wallCount, const DensityField &densityField, float div,
                          int count)
{
    const float originX = ctx.player->positionCenter.x;
    const float originY = ctx.player->positionCenter.y;
    BotVec2 best {originX, originY};
    float bestDanger = GenerateDangerAt(ctx, walls, wallCount, originX, originY);
    const int radius = count / 2;
    for (int yIdx = -radius; yIdx <= radius; ++yIdx)
    {
        for (int xIdx = -radius; xIdx <= radius; ++xIdx)
        {
            const float x = std::clamp(originX + static_cast<float>(xIdx) * div, ctx.minX, ctx.maxX);
            const float y = std::clamp(originY + static_cast<float>(yIdx) * div, ctx.minY, ctx.maxY);
            float danger = GenerateDangerAt(ctx, walls, wallCount, x, y);
            danger += std::fabs((ctx.minX + ctx.maxX) * 0.5f - x) * 0.06f;
            danger += ComputeVerticalSafeZoneBias(ctx, y);
            danger += ComputeEscapeLanePenaltyAt(densityField, x, y);
            danger -= ComputeObjectiveRewardAt(ctx, x, y, danger);
            if (danger < bestDanger)
            {
                bestDanger = danger;
                best = {x, y};
            }
        }
    }
    return best;
}

BotVec2 GenerateEnemyAlignForce(const BotContext &ctx)
{
    if (!ctx.enemy.valid || ctx.player == nullptr)
    {
        return {};
    }
    if (ctx.enemy.isBoss)
    {
        return GenerateForce(ctx.player->positionCenter.x, ctx.player->positionCenter.y, ctx.enemy.pos.x,
                             ctx.player->positionCenter.y, kAlignAttractionScale);
    }
    return GenerateForce(ctx.player->positionCenter.x, ctx.player->positionCenter.y, ctx.enemy.pos.x, ctx.enemy.pos.y,
                         kAlignAttractionScale * 0.8f);
}

BotVec2 GenerateItemForce(const BotContext &ctx, float localDanger)
{
    if (ctx.player == nullptr)
    {
        return {};
    }

    BotVec2 total {};
    const float safetyFactor = Clamp01(1.25f - (localDanger / std::max(kSafeItemDangerThreshold, 1.0f)));
    if (safetyFactor <= 0.1f)
    {
        return {};
    }
    for (int idx = 0; idx < ctx.itemCount; ++idx)
    {
        const ItemSample &item = ctx.items[idx];
        if (!item.valid)
        {
            continue;
        }
        const float distSq =
            std::max(DistanceSq2D(ctx.player->positionCenter.x, ctx.player->positionCenter.y, item.pos.x, item.pos.y), 36.0f);
        const BotVec2 toward = NormalizeOrZero(item.pos.x - ctx.player->positionCenter.x, item.pos.y - ctx.player->positionCenter.y);
        Accumulate(total, toward, (item.priority * kItemAttractionScale * safetyFactor) / std::sqrt(distSq));
    }
    return total;
}

float ComputeObjectiveRewardAt(const BotContext &ctx, float x, float y, float localDanger)
{
    if (ctx.player == nullptr)
    {
        return 0.0f;
    }

    const float pressureSafety = Clamp01(1.35f - (localDanger / std::max(kSqueezeDangerThreshold, 1.0f)));
    float reward = 0.0f;
    if (ctx.enemy.valid)
    {
        if (ctx.enemy.isBoss)
        {
            const float dx = std::fabs(x - ctx.enemy.pos.x);
            reward += (kBossAlignRewardScale * pressureSafety) / std::max(dx + 18.0f, 18.0f);
        }
        else
        {
            const float dist = std::sqrt(std::max(DistanceSq2D(x, y, ctx.enemy.pos.x, ctx.enemy.pos.y), 36.0f));
            reward += (kEnemyChaseRewardScale * pressureSafety) / std::max(dist, 18.0f);
        }
    }

    const float itemSafetyFactor = Clamp01(1.4f - (localDanger / std::max(kSafeItemDangerThreshold, 1.0f))) * pressureSafety;
    if (itemSafetyFactor > 0.05f)
    {
        for (int idx = 0; idx < ctx.itemCount; ++idx)
        {
            const ItemSample &item = ctx.items[idx];
            if (!item.valid)
            {
                continue;
            }
            const float dist = std::sqrt(std::max(DistanceSq2D(x, y, item.pos.x, item.pos.y), 25.0f));
            reward += ((item.priority * kItemRewardScale) * itemSafetyFactor) / std::max(dist, 12.0f);
        }
    }

    return reward;
}

BotVec2 GenerateMoveForce(const BotContext &ctx, const VirtualWall *walls, int wallCount, const DensityField &densityField)
{
    if (ctx.player == nullptr)
    {
        return {};
    }

    const float px = ctx.player->positionCenter.x;
    const float py = ctx.player->positionCenter.y;
    const float dynamicRange = kWarningRange + kDynamicRangeFactor * ctx.player->characterData.orthogonalMovementSpeed;
    const float rangeSqBase = dynamicRange * dynamicRange;

    BotVec2 force {};
    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.bullets) - 1; ++idx)
    {
        const Bullet &bullet = g_BulletManager.bullets[idx];
        if (bullet.state == 0)
        {
            continue;
        }
        for (int step = 0; step < kDangerProspects; ++step)
        {
            const float bx = bullet.pos.x + bullet.velocity.x * ctx.effectiveFramerateMultiplier * static_cast<float>(step);
            const float by = bullet.pos.y + bullet.velocity.y * ctx.effectiveFramerateMultiplier * static_cast<float>(step);
            const float bulletRadius = BulletEffectiveRadius(bullet);
            const float distSq = DistanceSq2D(px, py, bx, by);
            const float hitRadius = std::max(ctx.player->hitboxSize.x, ctx.player->hitboxSize.y) + bulletRadius + 4.0f;
            const float rangeSq = (rangeSqBase + bulletRadius * bulletRadius) * (1.0f + 0.45f * static_cast<float>(step));
            if (distSq > rangeSq)
            {
                continue;
            }
            const BotVec2 away = NormalizeOrZero(px - bx, py - by);
            const float effectiveSq = std::max(distSq - hitRadius * hitRadius, 9.0f);
            const float sizeFactor = 1.0f + std::min(bulletRadius * 0.09f, 2.6f);
            Accumulate(force, away, (static_cast<float>(kDangerWeights[step]) * sizeFactor) / effectiveSq);
        }
    }

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.lasers) - 1; ++idx)
    {
        const Laser &laser = g_BulletManager.lasers[idx];
        if (!IsLaserThreateningSoon(laser, 0))
        {
            continue;
        }
        for (int step = 0; step < kDangerProspects; ++step)
        {
            if (!IsLaserThreateningSoon(laser, step))
            {
                continue;
            }
            BotVec2 a {};
            BotVec2 b {};
            float halfWidth = 1.0f;
            GetLaserSegment(laser, ctx.effectiveFramerateMultiplier, step, a, b, halfWidth);
            const float lenSq = std::max(DistanceSq2D(a.x, a.y, b.x, b.y), 0.0001f);
            const float t = std::clamp(((px - a.x) * (b.x - a.x) + (py - a.y) * (b.y - a.y)) / lenSq, 0.0f, 1.0f);
            const float nearestX = a.x + (b.x - a.x) * t;
            const float nearestY = a.y + (b.y - a.y) * t;
            const float distSq = DistanceSq2D(px, py, nearestX, nearestY);
            const bool activeLaser = IsLaserActiveAt(laser, step);
            const float leadFactor = activeLaser ? 1.0f : 0.7f;
            const float warnRadius = halfWidth + (kWarningRange + halfWidth * 0.4f) * (1.0f + (1.0f - leadFactor));
            if (distSq > warnRadius * warnRadius)
            {
                continue;
            }
            const BotVec2 away = NormalizeOrZero(px - nearestX, py - nearestY);
            const float laserFactor = (1.8f + std::min(halfWidth * 0.05f, 1.8f)) * leadFactor;
            const float effectiveSq = std::max(distSq - halfWidth * halfWidth, 9.0f);
            Accumulate(force, away, (static_cast<float>(kDangerWeights[step]) * laserFactor) / effectiveSq);
        }
    }

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_EnemyManager.enemies) - 1; ++idx)
    {
        const Enemy &enemy = g_EnemyManager.enemies[idx];
        if (enemy.life <= 0)
        {
            continue;
        }
        const float distSq = DistanceSq2D(px, py, enemy.position.x, enemy.position.y);
        if (distSq > rangeSqBase * 2.0f)
        {
            continue;
        }
        const BotVec2 away = NormalizeOrZero(px - enemy.position.x, py - enemy.position.y);
        Accumulate(force, away, (kEnemyDangerBase * kEnemyForceFactor) / std::max(distSq, 32.0f));
    }

    if (px < ctx.minX + kBoundaryMargin)
    {
        force.x += kBoundaryForce / std::max(px - ctx.minX, 1.0f);
    }
    if (px > ctx.maxX - kBoundaryMargin)
    {
        force.x -= kBoundaryForce / std::max(ctx.maxX - px, 1.0f);
    }
    if (py < ctx.minY + kBoundaryMargin)
    {
        force.y += kBoundaryForce / std::max(py - ctx.minY, 1.0f);
    }
    if (py > ctx.maxY - kBoundaryMargin)
    {
        force.y -= kBoundaryForce / std::max(ctx.maxY - py, 1.0f);
    }
    if (py > ctx.maxY - kBottomComfortBand)
    {
        force.y -= (py - (ctx.maxY - kBottomComfortBand)) * 2.6f;
    }

    const float localDanger = GenerateDangerAt(ctx, walls, wallCount, px, py);
    if (IsSqueezedAt(ctx, densityField, px, py, localDanger))
    {
        const float safeCenterY = ctx.minY + (ctx.maxY - ctx.minY) * 0.74f;
        Accumulate(force, GenerateForce2(px, py, (ctx.minX + ctx.maxX) * 0.5f, safeCenterY, kSqueezeCenterBiasScale));
    }

    ApplyBulletWallRecognition(ctx, walls, wallCount, force);
    Accumulate(force, GenerateDensityForce(ctx, densityField));
    Accumulate(force, GenerateForecastForce(ctx));

    const BotVec2 dest = GenerateDestPoint(ctx, walls, wallCount, densityField, kDestGridStep, kDestGridCount);
    Accumulate(force, GenerateForce2(px, py, dest.x, dest.y, kDestAttractionScale));

    Accumulate(force, GenerateEnemyAlignForce(ctx));
    Accumulate(force, GenerateItemForce(ctx, localDanger));

    return force;
}

ThreatList GenerateNextNearestThreatList(const BotContext &ctx)
{
    ThreatList list {};
    if (ctx.player == nullptr)
    {
        return list;
    }

    const float px = ctx.player->positionCenter.x;
    const float py = ctx.player->positionCenter.y;

    auto insertBullet = [&](const Bullet *bullet, float distSq) {
        int pos = list.bulletCount;
        if (pos < kNearestBulletCap)
        {
            ++list.bulletCount;
        }
        else if (distSq >= list.bulletDistSq[pos - 1])
        {
            return;
        }
        else
        {
            pos = kNearestBulletCap - 1;
        }
        while (pos > 0 && distSq < list.bulletDistSq[pos - 1])
        {
            if (pos < kNearestBulletCap)
            {
                list.bullets[pos] = list.bullets[pos - 1];
                list.bulletDistSq[pos] = list.bulletDistSq[pos - 1];
            }
            --pos;
        }
        list.bullets[pos] = bullet;
        list.bulletDistSq[pos] = distSq;
    };

    auto insertLaser = [&](const Laser *laser, float distSq) {
        int pos = list.laserCount;
        if (pos < kNearestLaserCap)
        {
            ++list.laserCount;
        }
        else if (distSq >= list.laserDistSq[pos - 1])
        {
            return;
        }
        else
        {
            pos = kNearestLaserCap - 1;
        }
        while (pos > 0 && distSq < list.laserDistSq[pos - 1])
        {
            if (pos < kNearestLaserCap)
            {
                list.lasers[pos] = list.lasers[pos - 1];
                list.laserDistSq[pos] = list.laserDistSq[pos - 1];
            }
            --pos;
        }
        list.lasers[pos] = laser;
        list.laserDistSq[pos] = distSq;
    };

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.bullets) - 1; ++idx)
    {
        const Bullet &bullet = g_BulletManager.bullets[idx];
        if (bullet.state != 0)
        {
            insertBullet(&bullet, DistanceSq2D(px, py, bullet.pos.x, bullet.pos.y));
        }
    }

    for (int idx = 0; idx < ARRAY_SIZE_SIGNED(g_BulletManager.lasers) - 1; ++idx)
    {
        const Laser &laser = g_BulletManager.lasers[idx];
        if (!IsLaserThreateningSoon(laser, 0))
        {
            continue;
        }
        BotVec2 a {};
        BotVec2 b {};
        float halfWidth = 0.0f;
        GetLaserSegment(laser, ctx.effectiveFramerateMultiplier, 0, a, b, halfWidth);
        insertLaser(&laser, DistanceSqPointToSegment(px, py, a.x, a.y, b.x, b.y));
    }

    return list;
}

float EvaluateTrajectoryRisk(const BotContext &ctx, const ThreatList &list, const VirtualWall *walls, int wallCount,
                             const DensityField &densityField, Action action, bool focus)
{
    if (ctx.player == nullptr)
    {
        return std::numeric_limits<float>::infinity();
    }

    Player probe = *ctx.player;
    probe.playerState = PLAYER_STATE_INVULNERABLE;
    float x = ctx.player->positionCenter.x;
    float y = ctx.player->positionCenter.y;
    float risk = 0.0f;
    for (int frame = 1; frame <= kSurvivalCheckFrames; ++frame)
    {
        float dx = 0.0f;
        float dy = 0.0f;
        ApplyActionVelocity(*ctx.player, action, focus, ctx.effectiveFramerateMultiplier, dx, dy);
        x = std::clamp(x + dx, ctx.minX, ctx.maxX);
        y = std::clamp(y + dy, ctx.minY, ctx.maxY);
        probe.positionCenter.x = x;
        probe.positionCenter.y = y;
        probe.hitboxTopLeft.x = x - probe.hitboxSize.x;
        probe.hitboxTopLeft.y = y - probe.hitboxSize.y;
        probe.hitboxBottomRight.x = x + probe.hitboxSize.x;
        probe.hitboxBottomRight.y = y + probe.hitboxSize.y;
        risk += ComputeBoundaryDanger(ctx, x, y) * 0.12f;
        risk += ComputeWallDangerAt(x, y, walls, wallCount) * 0.10f;
        risk += ComputeDensityPenaltyAt(densityField, x, y) * 0.12f;

        for (int idx = 0; idx < list.bulletCount; ++idx)
        {
            const Bullet *bullet = list.bullets[idx];
            if (bullet == nullptr)
            {
                continue;
            }
            D3DXVECTOR3 bulletCenter = bullet->pos;
            bulletCenter.x += bullet->velocity.x * ctx.effectiveFramerateMultiplier * static_cast<float>(frame);
            bulletCenter.y += bullet->velocity.y * ctx.effectiveFramerateMultiplier * static_cast<float>(frame);
            const i32 probeResult = probe.ProbeKillBoxCollision(&bulletCenter, &bullet->sprites.grazeSize);
            if (probeResult == PROBE_COLLISION_HIT)
            {
                return std::numeric_limits<float>::infinity();
            }
            const float bulletRadius = BulletEffectiveRadius(*bullet);
            const float hitRadius = bulletRadius + std::max(probe.hitboxSize.x, probe.hitboxSize.y);
            const float nearDistSq = DistanceSq2D(x, y, bulletCenter.x, bulletCenter.y);
            const float effectiveSq = std::max(nearDistSq - hitRadius * hitRadius, 9.0f);
            const float sizeFactor = 1.0f + std::min(bulletRadius * 0.1f, 2.8f);
            risk += (kTrajectoryNearMissScale * sizeFactor) / effectiveSq;
            if (probeResult == PROBE_COLLISION_GRAZE_OR_BOMB)
            {
                risk += 110.0f + bulletRadius * 4.0f;
            }
        }

        for (int idx = 0; idx < list.laserCount; ++idx)
        {
            const Laser *laser = list.lasers[idx];
            if (laser == nullptr || !IsLaserThreateningSoon(*laser, frame))
            {
                continue;
            }
            const float advancedEnd = laser->endOffset + ctx.effectiveFramerateMultiplier * laser->speed * static_cast<float>(frame);
            float advancedStart = laser->startOffset;
            if (laser->startLength < advancedEnd - advancedStart)
            {
                advancedStart = advancedEnd - laser->startLength;
            }
            if (advancedStart < 0.0f)
            {
                advancedStart = 0.0f;
            }

            D3DXVECTOR3 laserCenter {};
            D3DXVECTOR3 laserSize {};
            D3DXVECTOR3 rotation {};
            BuildLaserProbeShape(*laser, ctx.effectiveFramerateMultiplier, frame, laserCenter, laserSize, rotation);

            const bool activeLaser = IsLaserActiveAt(*laser, frame);
            const i32 probeResult =
                activeLaser ? probe.ProbeLaserHitbox(&laserCenter, &laserSize, &rotation, laser->angle, 1) : PROBE_COLLISION_NONE;
            if (probeResult == PROBE_COLLISION_HIT)
            {
                return std::numeric_limits<float>::infinity();
            }
            BotVec2 a {};
            BotVec2 b {};
            float halfWidth = 1.0f;
            GetLaserSegment(*laser, ctx.effectiveFramerateMultiplier, frame, a, b, halfWidth);
            const float laserDistSq = DistanceSqPointToSegment(x, y, a.x, a.y, b.x, b.y);
            const float hitRadius = halfWidth + std::max(probe.hitboxSize.x, probe.hitboxSize.y);
            const float effectiveSq = std::max(laserDistSq - hitRadius * hitRadius, 9.0f);
            const float leadFactor = activeLaser ? 1.0f : 0.7f;
            const float laserFactor = (1.9f + std::min(halfWidth * 0.05f, 1.8f)) * leadFactor;
            risk += (kTrajectoryNearMissScale * laserFactor) / effectiveSq;
            if (probeResult == PROBE_COLLISION_GRAZE_OR_BOMB)
            {
                risk += kTrajectoryGrazeScale * 1.35f;
            }
        }
        risk += GenerateDangerAt(ctx, walls, wallCount, x, y) * 0.08f;
    }
    return risk;
}

void SimulateActionPosition(const BotContext &ctx, Action action, bool focus, int frames, float &x, float &y)
{
    x = ctx.player->positionCenter.x;
    y = ctx.player->positionCenter.y;
    for (int frame = 0; frame < frames; ++frame)
    {
        float dx = 0.0f;
        float dy = 0.0f;
        ApplyActionVelocity(*ctx.player, action, focus, ctx.effectiveFramerateMultiplier, dx, dy);
        x = std::clamp(x + dx, ctx.minX, ctx.maxX);
        y = std::clamp(y + dy, ctx.minY, ctx.maxY);
    }
}

float EvaluateSafeActionScore(const BotContext &ctx, const ThreatList &list, const VirtualWall *walls, int wallCount,
                              const DensityField &densityField, Action action, bool focus)
{
    if (ctx.player == nullptr || ActionPressesOutwardBoundary(ctx, action))
    {
        return std::numeric_limits<float>::infinity();
    }

    const float trajectoryRisk = EvaluateTrajectoryRisk(ctx, list, walls, wallCount, densityField, action, focus);
    if (!std::isfinite(trajectoryRisk))
    {
        return std::numeric_limits<float>::infinity();
    }

    float x = ctx.player->positionCenter.x;
    float y = ctx.player->positionCenter.y;
    SimulateActionPosition(ctx, action, focus, kSurvivalCheckFrames, x, y);

    const float localDanger = GenerateDangerAt(ctx, walls, wallCount, x, y);
    const bool squeezed = IsSqueezedAt(ctx, densityField, ctx.player->positionCenter.x, ctx.player->positionCenter.y,
                                       GenerateDangerAt(ctx, walls, wallCount, ctx.player->positionCenter.x, ctx.player->positionCenter.y));
    float score = trajectoryRisk;
    score += localDanger * 0.35f;
    score += ComputeDensityPenaltyAt(densityField, x, y);
    score += ComputeFarSectorPenaltyAt(densityField, x, y);
    score += ComputeEscapeLanePenaltyAt(densityField, x, y);
    score += ComputeStrategicLanePenaltyAt(ctx, densityField, x);
    score += ComputeTopCurtainPenaltyAt(ctx, densityField, x);
    score += ComputeLaserLanePenaltyAt(ctx, densityField, x);
    score += ComputeCornerTrapPenaltyAt(ctx, densityField, x, y);
    score += ComputeVerticalSafeZoneBias(ctx, y);
    score += std::fabs((ctx.minX + ctx.maxX) * 0.5f - x) * 0.04f;
    score -= ComputeObjectiveRewardAt(ctx, x, y, localDanger);
    if (squeezed && ActionPushesTowardNearestBoundary(ctx, ctx.player->positionCenter.x, ctx.player->positionCenter.y, action))
    {
        score += kSqueezeBoundaryPenaltyScale;
    }
    const bool calmWindow = localDanger < kIdleCalmDangerThreshold && trajectoryRisk < kIdleCalmTrajectoryThreshold;
    if (action != g_RuntimeState.lastAction)
    {
        score += kActionSwitchPenalty;
    }
    else
    {
        score -= kSameActionReward;
    }
    if (calmWindow)
    {
        if (action == Action::Idle)
        {
            score -= kIdleReward;
        }
        else if (g_RuntimeState.lastAction == Action::Idle && action != Action::Idle)
        {
            score += kIdleReward * 0.75f;
        }
    }
    if (focus)
    {
        score += 6.0f;
    }
    return score;
}

MovementProfile AnalyzeMovementProfile(const ThreatList &list, float currentDanger)
{
    MovementProfile profile {};
    int closeThreats = 0;
    int chaseThreats = 0;

    for (int idx = 0; idx < list.bulletCount; ++idx)
    {
        const float distSq = list.bulletDistSq[idx];
        if (distSq <= 58.0f * 58.0f)
        {
            ++closeThreats;
        }
        if (distSq <= 118.0f * 118.0f)
        {
            ++chaseThreats;
        }
    }
    for (int idx = 0; idx < list.laserCount; ++idx)
    {
        const float distSq = list.laserDistSq[idx];
        if (distSq <= 92.0f * 92.0f)
        {
            closeThreats += 2;
        }
        if (distSq <= 156.0f * 156.0f)
        {
            chaseThreats += 2;
        }
    }

    profile.closeThreats = closeThreats;
    profile.chaseThreats = chaseThreats;

    if (currentDanger >= kThreadDangerThreshold || closeThreats >= 5)
    {
        profile.mode = ModeHint::Thread;
    }
    else if (currentDanger >= kEscapeDangerThreshold || chaseThreats >= 6)
    {
        profile.mode = ModeHint::Escape;
    }
    else
    {
        profile.mode = ModeHint::Force;
    }

    return profile;
}

bool ChooseBestFocusForAction(const BotContext &ctx, const ThreatList &list, const VirtualWall *walls, int wallCount,
                              const DensityField &densityField, Action action, const MovementProfile &profile, bool &outFocus,
                              float &outScore)
{
    float fastScore = EvaluateSafeActionScore(ctx, list, walls, wallCount, densityField, action, false);
    float slowScore = EvaluateSafeActionScore(ctx, list, walls, wallCount, densityField, action, true);

    if (!std::isfinite(fastScore) && !std::isfinite(slowScore))
    {
        return false;
    }

    switch (profile.mode)
    {
    case ModeHint::Thread:
        if (std::isfinite(fastScore))
        {
            fastScore += kThreadFastPenalty;
        }
        if (std::isfinite(slowScore))
        {
            slowScore -= kThreadFocusBonus;
        }
        break;
    case ModeHint::Escape:
        if (std::isfinite(fastScore))
        {
            fastScore -= kEscapeFastBonus;
        }
        if (std::isfinite(slowScore))
        {
            slowScore += kEscapeSlowPenalty;
        }
        break;
    case ModeHint::Force:
    case ModeHint::Dest:
    case ModeHint::MoveCheck:
    case ModeHint::None:
    default:
        break;
    }

    if (!std::isfinite(slowScore))
    {
        outFocus = false;
        outScore = fastScore;
        return true;
    }
    if (!std::isfinite(fastScore))
    {
        outFocus = true;
        outScore = slowScore;
        return true;
    }

    if (fastScore <= slowScore + kFastModeTieSlack)
    {
        outFocus = false;
        outScore = fastScore;
        return true;
    }

    outFocus = true;
    outScore = slowScore;
    return true;
}

void EnumerateCorrectionCandidates(Action preferredAction, Action *outActions, int &outCount)
{
    outCount = 0;
    auto push = [&](Action action) {
        for (int idx = 0; idx < outCount; ++idx)
        {
            if (outActions[idx] == action)
            {
                return;
            }
        }
        outActions[outCount++] = action;
    };

    push(preferredAction);
    switch (preferredAction)
    {
    case Action::Left: push(Action::UpLeft); push(Action::DownLeft); push(Action::Idle); break;
    case Action::Right: push(Action::UpRight); push(Action::DownRight); push(Action::Idle); break;
    case Action::Up: push(Action::UpLeft); push(Action::UpRight); push(Action::Idle); break;
    case Action::Down: push(Action::DownLeft); push(Action::DownRight); push(Action::Idle); break;
    case Action::UpLeft: push(Action::Up); push(Action::Left); push(Action::Idle); break;
    case Action::UpRight: push(Action::Up); push(Action::Right); push(Action::Idle); break;
    case Action::DownLeft: push(Action::Left); push(Action::Down); push(Action::Idle); break;
    case Action::DownRight: push(Action::Right); push(Action::Down); push(Action::Idle); break;
    case Action::Idle: push(Action::Left); push(Action::Right); push(Action::Up); break;
    }

    for (Action action : kCandidateActions)
    {
        if (outCount >= ARRAY_SIZE_SIGNED(kCandidateActions))
        {
            break;
        }
        push(action);
    }
}

bool FindSafeAction(const BotContext &ctx, const ThreatList &list, const VirtualWall *walls, int wallCount,
                    const DensityField &densityField, Action preferredAction, bool preferredFocus, const MovementProfile &profile,
                    Action &outAction, bool &outFocus)
{
    Action bestAction = preferredAction;
    bool bestFocus = preferredFocus;
    float bestScore = std::numeric_limits<float>::infinity();
    const float currentX = ctx.player != nullptr ? ctx.player->positionCenter.x : 0.0f;
    const float currentY = ctx.player != nullptr ? ctx.player->positionCenter.y : 0.0f;
    const float currentDanger = ctx.player != nullptr ? GenerateDangerAt(ctx, walls, wallCount, currentX, currentY) : 0.0f;

    Action candidates[ARRAY_SIZE_SIGNED(kCandidateActions)] {};
    int candidateCount = 0;
    EnumerateCorrectionCandidates(preferredAction, candidates, candidateCount);

    auto consider = [&](Action candidate, bool preferred) {
        if (ShouldHardRejectAction(ctx, densityField, currentX, currentY, currentDanger, candidate))
        {
            return;
        }
        bool candidateFocus = preferredFocus;
        float candidateScore = std::numeric_limits<float>::infinity();
        if (!ChooseBestFocusForAction(ctx, list, walls, wallCount, densityField, candidate, profile, candidateFocus, candidateScore))
        {
            return;
        }
        if (preferred)
        {
            candidateScore -= 8.0f;
        }
        if (candidateScore < bestScore)
        {
            bestScore = candidateScore;
            bestAction = candidate;
            bestFocus = candidateFocus;
        }
    };

    for (int idx = 0; idx < candidateCount; ++idx)
    {
        consider(candidates[idx], idx == 0);
    }

    if (!std::isfinite(bestScore))
    {
        return false;
    }
    outAction = bestAction;
    outFocus = bestFocus;
    return true;
}

void ChangeMove(int mode, Action &action, bool &focus)
{
    static constexpr struct
    {
        Action action;
        bool focus;
    } kModes[] = {
        {Action::Left, true},      {Action::Right, true},   {Action::Up, true},       {Action::Down, true},
        {Action::UpLeft, true},    {Action::UpRight, true}, {Action::DownLeft, true}, {Action::DownRight, true},
        {Action::Idle, true},
    };
    const int idx = std::clamp(mode - 8, 0, ARRAY_SIZE_SIGNED(kModes) - 1);
    action = kModes[idx].action;
    focus = kModes[idx].focus;
}

Action ActionFromVector(const BotVec2 &move)
{
    const bool left = move.x < -kDirectionThreshold;
    const bool right = move.x > kDirectionThreshold;
    const bool up = move.y < -kDirectionThreshold;
    const bool down = move.y > kDirectionThreshold;

    if (up && left) return Action::UpLeft;
    if (up && right) return Action::UpRight;
    if (down && left) return Action::DownLeft;
    if (down && right) return Action::DownRight;
    if (up) return Action::Up;
    if (down) return Action::Down;
    if (left) return Action::Left;
    if (right) return Action::Right;
    return Action::Idle;
}

Action SanitizeActionForBounds(const BotContext &ctx, Action action)
{
    if (!ActionPressesOutwardBoundary(ctx, action))
    {
        return action;
    }

    switch (action)
    {
    case Action::Down:
        return Action::Idle;
    case Action::DownLeft:
        return (ctx.player != nullptr && ctx.player->positionCenter.x <= ctx.minX + kEdgeOutwardGuard) ? Action::UpRight : Action::Left;
    case Action::DownRight:
        return (ctx.player != nullptr && ctx.player->positionCenter.x >= ctx.maxX - kEdgeOutwardGuard) ? Action::UpLeft : Action::Right;
    case Action::Left:
    case Action::Right:
    case Action::Up:
        return Action::Idle;
    case Action::UpLeft:
        return Action::Up;
    case Action::UpRight:
        return Action::Up;
    case Action::Idle:
    default:
        return Action::Idle;
    }
}

u16 GetTargetMask(const InputOwnership &ownership)
{
    if (ownership.emitCanonicalLocalInput)
    {
        return TH_BUTTON_SHOOT | TH_BUTTON_BOMB | TH_BUTTON_FOCUS | TH_BUTTON_DIRECTION;
    }

    switch (ownership.controlledTarget)
    {
    case Target::P1: return TH_BUTTON_SHOOT | TH_BUTTON_BOMB | TH_BUTTON_FOCUS | TH_BUTTON_DIRECTION;
    case Target::P2: return TH_BUTTON_SHOOT2 | TH_BUTTON_BOMB2 | TH_BUTTON_FOCUS2 | TH_BUTTON_DIRECTION2;
    case Target::Off:
    default: return 0;
    }
}

u16 BuildInputBits(const InputOwnership &ownership, Action action, bool focus, bool shoot, bool bomb)
{
    u16 bits = 0;
    if (ownership.emitCanonicalLocalInput || ownership.controlledTarget == Target::P1)
    {
        switch (action)
        {
        case Action::Up: bits |= TH_BUTTON_UP; break;
        case Action::Down: bits |= TH_BUTTON_DOWN; break;
        case Action::Left: bits |= TH_BUTTON_LEFT; break;
        case Action::Right: bits |= TH_BUTTON_RIGHT; break;
        case Action::UpLeft: bits |= TH_BUTTON_UP_LEFT; break;
        case Action::UpRight: bits |= TH_BUTTON_UP_RIGHT; break;
        case Action::DownLeft: bits |= TH_BUTTON_DOWN_LEFT; break;
        case Action::DownRight: bits |= TH_BUTTON_DOWN_RIGHT; break;
        case Action::Idle:
        default: break;
        }
        if (focus) bits |= TH_BUTTON_FOCUS;
        if (shoot) bits |= TH_BUTTON_SHOOT;
        if (bomb) bits |= TH_BUTTON_BOMB;
    }
    else if (ownership.controlledTarget == Target::P2)
    {
        switch (action)
        {
        case Action::Up: bits |= TH_BUTTON_UP2; break;
        case Action::Down: bits |= TH_BUTTON_DOWN2; break;
        case Action::Left: bits |= TH_BUTTON_LEFT2; break;
        case Action::Right: bits |= TH_BUTTON_RIGHT2; break;
        case Action::UpLeft: bits |= TH_BUTTON_UP_LEFT2; break;
        case Action::UpRight: bits |= TH_BUTTON_UP_RIGHT2; break;
        case Action::DownLeft: bits |= TH_BUTTON_DOWN_LEFT2; break;
        case Action::DownRight: bits |= TH_BUTTON_DOWN_RIGHT2; break;
        case Action::Idle:
        default: break;
        }
        if (focus) bits |= TH_BUTTON_FOCUS2;
        if (shoot) bits |= TH_BUTTON_SHOOT2;
        if (bomb) bits |= TH_BUTTON_BOMB2;
    }
    return bits;
}

void ResetStatus(Target target)
{
    if (g_RuntimeState.bombCooldown > 0)
    {
        --g_RuntimeState.bombCooldown;
    }
    g_RuntimeState.status.active = false;
    g_RuntimeState.status.eligible = false;
    g_RuntimeState.status.remoteSession = Session::IsRemoteNetplaySession();
    g_RuntimeState.status.target = target;
    g_RuntimeState.status.action = Action::Idle;
    g_RuntimeState.status.focus = false;
    g_RuntimeState.status.bombCoolingDown = g_RuntimeState.bombCooldown > 0;
    g_RuntimeState.status.willBomb = false;
    g_RuntimeState.status.danger = 0.0f;
    g_RuntimeState.status.forecastThreatCount = 0;
    g_RuntimeState.status.bypassReason = BypassReason::None;
    g_RuntimeState.status.modeHint = ModeHint::None;
    g_RuntimeState.status.botInputBits = 0;
    g_RuntimeState.status.finalInputBits = 0;
}

bool ShouldBombNow(const BotContext &ctx, const ThreatList &threats, const VirtualWall *walls, int wallCount,
                   const DensityField &densityField, float currentDanger, Action action, bool focus)
{
    if (!ctx.autoBomb || ctx.player == nullptr || ctx.bombsRemaining <= 0 || g_RuntimeState.bombCooldown > 0 ||
        ctx.player->bombInfo.calc == nullptr || ctx.player->bombInfo.isInUse != 0)
    {
        return false;
    }
    if (IsDeathbombWindow(*ctx.player))
    {
        return true;
    }
    if (std::isfinite(EvaluateTrajectoryRisk(ctx, threats, walls, wallCount, densityField, action, focus)))
    {
        return false;
    }

    Action safeAction = action;
    bool safeFocus = focus;
    const MovementProfile profile = AnalyzeMovementProfile(threats, currentDanger);
    if (FindSafeAction(ctx, threats, walls, wallCount, densityField, action, focus, profile, safeAction, safeFocus))
    {
        return false;
    }

    if (currentDanger >= kFocusDangerThreshold)
    {
        return true;
    }
    return currentDanger >= kDangerBombThreshold ||
           GenerateDangerAt(ctx, walls, wallCount, ctx.player->positionCenter.x, ctx.player->positionCenter.y) >=
               kEmergencyBombDanger;
}
} // namespace

u16 ProcessGameplayInput(u16 nextInput, bool forRemoteNetplay)
{
    const u16 rawInput = nextInput;
    const Target requestedTarget = static_cast<Target>(THPrac::TH06::THPracGetAstroBotTarget());
    const InputOwnership ownership = ResolveInputOwnership(requestedTarget);
    ResetStatus(ownership.controlledTarget);

    BypassReason bypassReason = BypassReason::None;
    if (!IsEligible(requestedTarget, ownership, bypassReason))
    {
        g_RuntimeState.status.bypassReason = bypassReason;
        if (forRemoteNetplay && THPrac::TH06::THPracIsDebugLogEnabled())
        {
            Netplay::TraceDiagnostic("astrobot-netplay-bypass", "frame=%d ownership=%s reason=%d rawInput=%s", Netplay::g_State.currentNetFrame,
                                     OwnershipTag(ownership), (int)bypassReason, Netplay::FormatInputBits(rawInput).c_str());
        }
        return nextInput;
    }

    const BotContext ctx = CaptureContext(ownership);
    if (ctx.player == nullptr)
    {
        g_RuntimeState.status.bypassReason = BypassReason::NoPlayer;
        if (forRemoteNetplay && THPrac::TH06::THPracIsDebugLogEnabled())
        {
            Netplay::TraceDiagnostic("astrobot-netplay-bypass", "frame=%d ownership=%s reason=%d rawInput=%s", Netplay::g_State.currentNetFrame,
                                     OwnershipTag(ownership), (int)BypassReason::NoPlayer, Netplay::FormatInputBits(rawInput).c_str());
        }
        return nextInput;
    }

    g_RuntimeState.status.eligible = true;
    g_RuntimeState.status.remoteSession = ownership.remoteSession;
    g_RuntimeState.status.target = ownership.controlledTarget;

    VirtualWall walls[kMaxVirtualWalls] {};
    const int wallCount = BuildVirtualWalls(ctx, walls);
    const DensityField densityField = BuildDensityField(ctx);
    const BotVec2 moveForce = GenerateMoveForce(ctx, walls, wallCount, densityField);
    Action action = SanitizeActionForBounds(ctx, ActionFromVector(moveForce));

    int switchValue = kSpeedSwitchValue;
    switchValue += g_RuntimeState.lastFocus ? 25 : -25;
    const float moveMagnitude = std::sqrt(moveForce.x * moveForce.x + moveForce.y * moveForce.y);
    bool focus = moveMagnitude < static_cast<float>(switchValue);

    const float currentDanger = GenerateDangerAt(ctx, walls, wallCount, ctx.player->positionCenter.x, ctx.player->positionCenter.y);
    const ThreatList threats = GenerateNextNearestThreatList(ctx);
    const MovementProfile profile = AnalyzeMovementProfile(threats, currentDanger);
    if (profile.mode == ModeHint::Thread || currentDanger >= kFocusDangerThreshold)
    {
        focus = true;
    }
    else if (profile.mode == ModeHint::Escape)
    {
        focus = false;
    }

    ModeHint modeHint = profile.mode;
    const BotVec2 dest = GenerateDestPoint(ctx, walls, wallCount, densityField, kDestGridStep, kDestGridCount);
    if (modeHint == ModeHint::Force && DistanceSq2D(dest.x, dest.y, ctx.player->positionCenter.x, ctx.player->positionCenter.y) > 25.0f)
    {
        modeHint = ModeHint::Dest;
    }

    if (!FindSafeAction(ctx, threats, walls, wallCount, densityField, action, focus, profile, action, focus))
    {
        const float currentDanger = GenerateDangerAt(ctx, walls, wallCount, ctx.player->positionCenter.x, ctx.player->positionCenter.y);
        action = SanitizeActionForBounds(ctx, g_RuntimeState.lastAction);
        if (ShouldHardRejectAction(ctx, densityField, ctx.player->positionCenter.x, ctx.player->positionCenter.y, currentDanger, action))
        {
            if (densityField.hasTopCurtainGap)
            {
                const float dx = densityField.topCurtainGapX - ctx.player->positionCenter.x;
                action = dx < -8.0f ? Action::Left : (dx > 8.0f ? Action::Right : Action::Idle);
            }
            else if (densityField.hasStrategicLane)
            {
                const float dx = densityField.strategicLaneX - ctx.player->positionCenter.x;
                action = dx < -8.0f ? Action::Left : (dx > 8.0f ? Action::Right : Action::Idle);
            }
            else
            {
                action = Action::Idle;
            }
        }
        focus = true;
        modeHint = ModeHint::MoveCheck;
    }
    else if (modeHint != ModeHint::Dest)
    {
        modeHint = ModeHint::MoveCheck;
    }

    if (currentDanger < kIdleCalmDangerThreshold)
    {
        bool idleFocus = focus;
        float idleScore = EvaluateSafeActionScore(ctx, threats, walls, wallCount, densityField, Action::Idle, focus);
        float idleSlowScore = EvaluateSafeActionScore(ctx, threats, walls, wallCount, densityField, Action::Idle, true);
        if (std::isfinite(idleSlowScore) && (!std::isfinite(idleScore) || idleSlowScore < idleScore))
        {
            idleScore = idleSlowScore;
            idleFocus = true;
        }

        float chosenScore = EvaluateSafeActionScore(ctx, threats, walls, wallCount, densityField, action, focus);
        if (std::isfinite(idleScore) && std::isfinite(chosenScore) && idleScore <= chosenScore + 10.0f)
        {
            action = Action::Idle;
            focus = idleFocus;
            modeHint = ModeHint::MoveCheck;
        }
    }

    const bool bombEdge = ShouldBombNow(ctx, threats, walls, wallCount, densityField, currentDanger, action, focus);
    if (bombEdge)
    {
        g_RuntimeState.bombCooldown = kBombCooldownFrames;
    }

    const bool shoot = THPrac::TH06::THPracIsAstroBotAutoShootEnabled();
    const u16 mask = GetTargetMask(ownership);
    const u16 botBits = BuildInputBits(ownership, action, focus, shoot, bombEdge);
    nextInput = static_cast<u16>((nextInput & ~mask) | botBits);

    g_RuntimeState.lastAction = action;
    g_RuntimeState.lastFocus = focus;
    g_RuntimeState.status.active = true;
    g_RuntimeState.status.action = action;
    g_RuntimeState.status.focus = focus;
    g_RuntimeState.status.bombCoolingDown = g_RuntimeState.bombCooldown > 0;
    g_RuntimeState.status.willBomb = bombEdge;
    g_RuntimeState.status.danger = currentDanger;
    g_RuntimeState.status.forecastThreatCount = ctx.forecast.count;
    g_RuntimeState.status.modeHint = modeHint;
    g_RuntimeState.status.botInputBits = botBits;
    g_RuntimeState.status.finalInputBits = nextInput;
    if (forRemoteNetplay && THPrac::TH06::THPracIsDebugLogEnabled())
    {
        Netplay::TraceDiagnostic("astrobot-netplay-ownership", "frame=%d ownership=%s requested=%d effective=%d", Netplay::g_State.currentNetFrame,
                                 OwnershipTag(ownership), (int)requestedTarget, (int)ownership.controlledTarget);
        Netplay::TraceDiagnostic("astrobot-netplay-local-input",
                                 "frame=%d ownership=%s rawInput=%s botInput=%s finalLocalInput=%s", Netplay::g_State.currentNetFrame,
                                 OwnershipTag(ownership), Netplay::FormatInputBits(rawInput).c_str(),
                                 Netplay::FormatInputBits(botBits).c_str(), Netplay::FormatInputBits(nextInput).c_str());
    }
    return nextInput;
}

u16 ProcessLocalGameplayInput(u16 nextInput)
{
    return ProcessGameplayInput(nextInput, false);
}

u16 ProcessNetplayLocalInput(u16 nextInput)
{
    return ProcessGameplayInput(nextInput, true);
}

StatusSnapshot GetStatusSnapshot()
{
    return g_RuntimeState.status;
}
} // namespace th06::AstroBot
