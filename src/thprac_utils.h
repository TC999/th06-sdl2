#pragma once
// thprac_utils.h - Adapter for th06 source build
// Provides SINGLETON macro and GetMemContent/GetMemAddr helpers

#include <cstdint>
#include <cstddef>

namespace THPrac {

// InitHook stub (not needed in source build)
inline void InitHook(int /*ver*/, void* /*refreshRateAddr1*/ = nullptr, void* /*refreshRateAddr2*/ = nullptr) {}

#define SINGLETON_DEPENDENCY(...)
#define SINGLETON(className)                        \
public:                                             \
    className(const className&) = delete;           \
    className& operator=(className&) = delete;      \
    className(className&&) = delete;                \
    className& operator=(className&&) = delete;     \
    static auto& singleton()                        \
    {                                               \
        static className* s_singleton = nullptr;    \
        if (!s_singleton)                           \
            s_singleton = new className();          \
        return *s_singleton;                        \
    }                                               \
                                                    \
private:

// Memory access helpers (for compatibility with address-based code)
template <typename R = size_t>
inline R GetMemContent(uintptr_t addr)
{
    return *(R*)addr;
}
template <typename R = size_t, typename... OffsetArgs>
inline R GetMemContent(uintptr_t addr, size_t offset, OffsetArgs... remaining_offsets)
{
    return GetMemContent<R>(((uintptr_t) * (R*)addr) + offset, remaining_offsets...);
}

template <typename R = uintptr_t>
inline R GetMemAddr(uintptr_t addr)
{
    return (R)addr;
}
template <typename R = uintptr_t, typename... OffsetArgs>
inline R GetMemAddr(uintptr_t addr, size_t offset, OffsetArgs... remaining_offsets)
{
    return GetMemAddr<R>(((uintptr_t) * (R*)addr) + offset, remaining_offsets...);
}

}
