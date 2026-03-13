#pragma once
#include "diffbuild.hpp"
#include "inttypes.hpp"
#include "sdl2_compat.hpp"
#include <cmath>

struct ZunVec2
{
    f32 x;
    f32 y;

    f32 VectorLength()
    {
        return sqrtf(this->x * this->x + this->y * this->y);
    }

    f64 VectorLengthF64()
    {
        return (f64)this->VectorLength();
    }

    D3DXVECTOR2 *AsD3dXVec()
    {
        return (D3DXVECTOR2 *)this;
    }
};
ZUN_ASSERT_SIZE(ZunVec2, 0x8);

struct ZunVec3
{
    f32 x;
    f32 y;
    f32 z;

    D3DXVECTOR3 *AsD3dXVec()
    {
        return (D3DXVECTOR3 *)this;
    }

    static void SetVecCorners(ZunVec3 *topLeftCorner, ZunVec3 *bottomRightCorner, const D3DXVECTOR3 *centerPosition,
                              const D3DXVECTOR3 *size)
    {
        topLeftCorner->x = centerPosition->x - size->x / 2.0f;
        topLeftCorner->y = centerPosition->y - size->y / 2.0f;
        bottomRightCorner->x = size->x / 2.0f + centerPosition->x;
        bottomRightCorner->y = size->y / 2.0f + centerPosition->y;
    }
};
ZUN_ASSERT_SIZE(ZunVec3, 0xC);

#define ZUN_MIN(x, y) ((x) > (y) ? (y) : (x))
#define ZUN_PI ((f32)(3.14159265358979323846))
#define ZUN_2PI ((f32)(ZUN_PI * 2.0f))

#define RADIANS(degrees) ((degrees * ZUN_PI / 180.0f))

#define sincos(in, out_sine, out_cosine)                                                                               \
    {                                                                                                                  \
        (out_cosine) = cosf(in);                                                                                       \
        (out_sine) = sinf(in);                                                                                         \
    }

inline void fsincos_wrapper(f32 *out_sine, f32 *out_cosine, f32 angle)
{
    *out_cosine = cosf(angle);
    *out_sine = sinf(angle);
}

inline void sincosmul(D3DXVECTOR3 *out_vel, f32 input, f32 multiplier)
{
    out_vel->x = cosf(input) * multiplier;
    out_vel->y = sinf(input) * multiplier;
}

inline f32 invertf(f32 x)
{
    return 1.f / x;
}

inline f32 rintf(f32 float_in)
{
    return roundf(float_in);
}
