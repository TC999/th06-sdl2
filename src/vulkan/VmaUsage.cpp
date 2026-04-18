// SPDX-License-Identifier: MIT
// Phase 3 — single TU that emits VMA's implementation. Combined with volk, we use
// VMA_DYNAMIC_VULKAN_FUNCTIONS=1 + VMA_STATIC_VULKAN_FUNCTIONS=0 so VMA pulls function
// pointers via vkGetInstanceProcAddr / vkGetDeviceProcAddr (provided by volk). No other
// translation unit may #define VMA_IMPLEMENTATION.
//
// Phase 2 carve-out (3x bare vkAllocateMemory) is now retired — see ADR-002 Amendment 2026-04-19.

// Order matters: volk first so vulkan types are present before VMA's impl is included.
#include <volk.h>

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4100 4127 4189 4324 4505 4548 4701 4702 4458 4244)
#endif

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
// VMA's macros that decide whether to include <vulkan/vulkan.h>. We already pulled
// the types via volk, so don't include vulkan.h again (avoids duplicate-define noise).
#ifndef VMA_VULKAN_VERSION
#  define VMA_VULKAN_VERSION 1001000  // 1.1.0
#endif

#include "VmaUsage.hpp"

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
