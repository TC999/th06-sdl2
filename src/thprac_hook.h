#pragma once
// thprac_hook.h - Adapter for th06 source build
// In the source build, we don't use DLL injection hooks.
// Instead, EHOOK/PATCH macros are stubbed out, and we call
// game functions directly.

#include <stdint.h>
#include <cstddef>

namespace THPrac {

// Stub for ingame_image_base (not needed in source build)
inline uintptr_t ingame_image_base = 0;
#define RVA(a) ((uintptr_t)(a))

// Forward declarations
struct HookCtx;

// In source build, we don't have PCONTEXT. Define a minimal stub.
struct THPRAC_CONTEXT {
    uint32_t Eax, Ebx, Ecx, Edx, Esi, Edi, Ebp, Esp, Eip;
};

typedef void Callback(THPRAC_CONTEXT* pCtx, HookCtx* self);

struct PatchBufferImpl {
    void* ptr = nullptr;
    size_t size = 0;
    constexpr PatchBufferImpl() = default;
};

struct PatchHookImpl {
    void* codecave = nullptr;
    uint8_t orig_byte = 0;
    uint8_t instr_len = 0;
    constexpr PatchHookImpl() = default;
    constexpr PatchHookImpl(uint8_t) : codecave(nullptr), orig_byte(0), instr_len(0) {}
};

union PatchData {
    PatchBufferImpl buffer;
    PatchHookImpl hook;
    constexpr PatchData() : buffer() {}
    constexpr PatchData(const PatchBufferImpl& b) : buffer(b) {}
    constexpr PatchData(const PatchHookImpl& h) : hook(h) {}
};

struct HookCtx {
    uintptr_t addr = 0;
    const char* name = nullptr;
    bool setup = false;
    bool enabled = false;
    Callback* callback = nullptr;
    PatchData data;

    constexpr HookCtx() = default;
    constexpr HookCtx(uintptr_t a, const char* n) : addr(a), name(n) {}

    void Enable() { enabled = true; }
    void Disable() { enabled = false; }
    void Setup() { setup = true; }
    void Toggle(bool status) { enabled = status; }
};

struct HookSlice {
    HookCtx* ptr = nullptr;
    size_t len = 0;
};

// make_hook_array not needed in source build (requires C++20 non-type template params)

// In source build, all hook/patch macros produce no-op stubs.
// The actual game modifications happen through direct source code changes.

#define HOOKSET_DEFINE(name) static HookCtx name[] = {
#define EHOOK_DY(name_, addr_, instr_size_, ...) HookCtx((uintptr_t)addr_, #name_),
#define PATCH_DY(name_, addr_, ...) HookCtx((uintptr_t)addr_, #name_),
#define HOOKSET_ENDDEF() };

#define EHOOK_ST(name_, addr_, instr_size_, ...) inline HookCtx name_((uintptr_t)addr_, #name_)
#define PATCH_ST(name_, addr_, ...) inline HookCtx name_((uintptr_t)addr_, #name_)

#ifndef elementsof
#define elementsof(a) (sizeof(a) / sizeof(a[0]))
#endif

inline void EnableAllHooksImpl(HookCtx* hooks, size_t num) {
    for (size_t i = 0; i < num; i++) hooks[i].Enable();
}
inline void DisableAllHooksImpl(HookCtx* hooks, size_t num) {
    for (size_t i = 0; i < num; i++) hooks[i].Disable();
}
#define EnableAllHooks(hooks) EnableAllHooksImpl(hooks, elementsof(hooks))
#define DisableAllHooks(hooks) DisableAllHooksImpl(hooks, elementsof(hooks))

} // namespace THPrac
