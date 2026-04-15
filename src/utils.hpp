#pragma once
#include "GameErrorContext.hpp"
#include "ZunMath.hpp"
#include "ZunResult.hpp"
#include "inttypes.hpp"
#include <cstring>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define ARRAY_SIZE_SIGNED(x) ((i32)sizeof(x) / (i32)sizeof(x[0]))

#define ZUN_BIT(a) (1 << (a))
#define ZUN_MASK(a) (ZUN_BIT(a) - 1)
#define ZUN_RANGE(a, count) (ZUN_MASK((a) + (count)) & ~ZUN_MASK(a))
#define ZUN_CLEAR_BITS(a, keep_mask) (a & ~keep_mask)

#define IS_PRESSED(key) (g_CurFrameInput & (key))
#define WAS_PRESSED(key) (((g_CurFrameInput & (key)) != 0) && (g_CurFrameInput & (key)) != (g_LastFrameInput & (key)))
#define WAS_PRESSED_WEIRD(key)                                                                                         \
    (WAS_PRESSED(key) || (((g_CurFrameInput & (key)) != 0) && (g_IsEigthFrameOfHeldInput != 0)))

namespace th06
{
namespace utils
{
inline size_t BoundedStringLength(const char *src, size_t maxLen)
{
    size_t len;

    if (src == NULL)
    {
        return 0;
    }

    for (len = 0; len < maxLen && src[len] != '\0'; len++)
    {
    }

    return len;
}

inline void CopyStringToFixedField(char *dst, size_t dstSize, const char *src)
{
    size_t len;

    if (dst == NULL || dstSize == 0)
    {
        return;
    }

    memset(dst, ' ', dstSize);
    len = BoundedStringLength(src, dstSize);
    if (len != 0)
    {
        memcpy(dst, src, len);
    }
}

inline void CopyStringToSizedBuffer(char *dst, size_t dstSize, const char *src)
{
    size_t len;

    if (dst == NULL || dstSize == 0)
    {
        return;
    }

    memset(dst, 0, dstSize);
    if (dstSize == 1)
    {
        return;
    }

    memset(dst, ' ', dstSize - 1);
    len = BoundedStringLength(src, dstSize - 1);
    if (len != 0)
    {
        memcpy(dst, src, len);
    }
    dst[dstSize - 1] = '\0';
}

inline void CopyFixedFieldToSizedBuffer(char *dst, size_t dstSize, const char *src, size_t srcSize)
{
    size_t len;

    if (dst == NULL || dstSize == 0)
    {
        return;
    }

    memset(dst, 0, dstSize);
    if (dstSize == 1)
    {
        return;
    }

    len = BoundedStringLength(src, srcSize);
    if (len > dstSize - 1)
    {
        len = dstSize - 1;
    }
    if (len != 0)
    {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

template <typename T> inline T ReadUnaligned(const void *src)
{
    T value;
    memcpy(&value, src, sizeof(value));
    return value;
}

template <typename T> inline void WriteUnaligned(void *dst, T value)
{
    memcpy(dst, &value, sizeof(value));
}

ZunResult CheckForRunningGameInstance(void);
void DebugPrint(const char *fmt, ...);
void DebugPrint2(const char *fmt, ...);

f32 AddNormalizeAngle(f32 a, f32 b);
void Rotate(D3DXVECTOR3 *outVector, D3DXVECTOR3 *point, f32 angle);
}; // namespace utils
}; // namespace th06
