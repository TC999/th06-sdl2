// SPDX-License-Identifier: MIT
// Phase 3 — VMA single-header bridge. Includers get vmaCreate*/vmaDestroy*/etc. The single
// implementation TU is VmaUsage.cpp (defines VMA_IMPLEMENTATION). This header MUST be
// included AFTER volk.h — it depends on Vulkan types but does not pull <vulkan/vulkan.h>
// itself (we configure VMA to use volk's dispatch tables via dynamic function loading).
#pragma once

// VMA assumes vulkan.h is already visible. With volk, types are declared by <volk.h>,
// which transitively includes <vulkan/vulkan.h> in types-only mode (no prototypes).
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>
