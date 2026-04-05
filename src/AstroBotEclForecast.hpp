#pragma once

#include "Enemy.hpp"
#include "Player.hpp"

namespace th06::AstroBotEclForecast
{
enum class ForecastThreatType : u8
{
    BulletCone = 0,
    BulletRing = 1,
    LaserBeam = 2,
};

struct ForecastThreat
{
    bool valid = false;
    ForecastThreatType type = ForecastThreatType::BulletCone;
    bool isLaser = false;
    int spawnFrameOffset = 0;
    D3DXVECTOR3 origin {};
    float angleCenter = 0.0f;
    float angleHalfRange = 0.0f;
    float rangeMin = 0.0f;
    float rangeMax = 0.0f;
    float width = 0.0f;
    float weight = 0.0f;
};

struct ForecastSnapshot
{
    int count = 0;
    ForecastThreat threats[64] {};
};

ForecastSnapshot BuildForecastSnapshot(const Player &player);
} // namespace th06::AstroBotEclForecast
