// =============================================================================
// BackendAvailability.cpp - Phase 5b.3
// Probes which renderer backends actually work on this device, so the runtime
// selector (CLI, cfg.unk[0], in-game switcher) can clamp the request to an
// available backend instead of hard-crashing on init.
//
// Compiled into every build (desktop + Android). Vulkan probe is gated on
// TH06_USE_VULKAN; when not compiled in, IsBackendCompiledIn(Vulkan) == false
// and the probe trivially marks Vulkan unavailable.
// =============================================================================

#include "IRenderer.hpp"

#include <cstdio>
#include <cstring>

#ifdef TH06_USE_VULKAN
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>
#endif

namespace th06 {

namespace {
    bool s_probed       = false;
    bool s_avail_gl     = false;
    bool s_avail_gles   = false;
    bool s_avail_vulkan = false;

#ifdef TH06_USE_VULKAN
    // Lightweight Vulkan probe: load loader (volk) → create a minimal
    // VkInstance → enumerate physical devices. Anything <1 GPU or any error
    // marks Vulkan unavailable. The probe deliberately avoids creating a
    // SDL_Window / VkSurfaceKHR / VkDevice — those happen later inside
    // RendererVulkan::Init only after the runtime selector commits to Vulkan.
    bool ProbeVulkan()
    {
        VkResult vr = volkInitialize();
        if (vr != VK_SUCCESS) {
            std::fprintf(stderr, "[backend-probe] Vulkan: volkInitialize -> %d (loader missing?)\n", (int)vr);
            return false;
        }

        VkApplicationInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        ai.pApplicationName   = "th06-backend-probe";
        ai.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
        ai.pEngineName        = "th06";
        ai.engineVersion      = VK_MAKE_VERSION(0, 0, 1);
        ai.apiVersion         = VK_API_VERSION_1_0;

        VkInstanceCreateInfo ici{};
        ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &ai;

        VkInstance inst = VK_NULL_HANDLE;
        vr = vkCreateInstance(&ici, nullptr, &inst);
        if (vr != VK_SUCCESS || inst == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[backend-probe] Vulkan: vkCreateInstance -> %d\n", (int)vr);
            return false;
        }
        volkLoadInstance(inst);

        uint32_t devCount = 0;
        vr = vkEnumeratePhysicalDevices(inst, &devCount, nullptr);
        const bool ok = (vr == VK_SUCCESS && devCount >= 1);
        if (!ok) {
            std::fprintf(stderr, "[backend-probe] Vulkan: enumerate -> %d, devCount=%u\n",
                         (int)vr, devCount);
        }
        vkDestroyInstance(inst, nullptr);
        return ok;
    }
#endif
} // namespace

bool IsBackendCompiledIn(BackendKind kind)
{
    switch (kind) {
        case BackendKind::GL:
#ifdef __ANDROID__
            return false;
#else
            return true;
#endif
        case BackendKind::GLES:
            return true;
        case BackendKind::Vulkan:
#ifdef TH06_USE_VULKAN
            return true;
#else
            return false;
#endif
    }
    return false;
}

BackendKind PlatformDefaultBackend()
{
#ifdef __ANDROID__
    return BackendKind::GLES;
#else
    return BackendKind::GL;
#endif
}

void ProbeBackendAvailability()
{
    if (s_probed) return;
    s_probed = true;

    // GL / GLES: we don't actually create a context here (would race window
    // creation). Both are assumed available iff compiled in. SDL guarantees a
    // software fallback (Mesa swrast / ANGLE) so creation effectively never
    // fails on a sane system.
    s_avail_gl   = IsBackendCompiledIn(BackendKind::GL);
    s_avail_gles = IsBackendCompiledIn(BackendKind::GLES);

#ifdef TH06_USE_VULKAN
    s_avail_vulkan = ProbeVulkan();
#else
    s_avail_vulkan = false;
#endif

    std::fprintf(stderr, "[backend-probe] availability: GL=%d GLES=%d Vulkan=%d\n",
                 (int)s_avail_gl, (int)s_avail_gles, (int)s_avail_vulkan);
}

bool IsBackendAvailable(BackendKind kind)
{
    if (!s_probed) ProbeBackendAvailability();
    switch (kind) {
        case BackendKind::GL:     return s_avail_gl;
        case BackendKind::GLES:   return s_avail_gles;
        case BackendKind::Vulkan: return s_avail_vulkan;
    }
    return false;
}

BackendKind ResolveBackend(BackendKind requested)
{
    if (IsBackendAvailable(requested)) return requested;
    // Try platform default next.
    BackendKind def = PlatformDefaultBackend();
    if (IsBackendAvailable(def)) {
        std::fprintf(stderr, "[backend-probe] requested backend unavailable; "
                             "falling back to platform default.\n");
        return def;
    }
    // Last resort: GLES is always compiled in.
    std::fprintf(stderr, "[backend-probe] platform default also unavailable; "
                         "forcing GLES.\n");
    return BackendKind::GLES;
}

} // namespace th06
