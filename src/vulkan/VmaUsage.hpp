// SPDX-License-Identifier: MIT
// Phase 1 skeleton — 仅声明，不调用 VMA API。Phase 3 才会启用真正的 vmaCreate*。
// 该 header 集中处理 VMA 单 header 的实现宏，仅 .cpp 中包含。
#pragma once

// VMA 推荐：使用动态加载，避免直接依赖 vulkan.h 的内联函数解析。
// Phase 1 暂不引入实现宏，等 VkContext.cpp 第一次需要 vmaCreate* 时再 #define VMA_IMPLEMENTATION
// 一次性。该 header 的目的是为 includer 屏蔽路径细节并固化 VMA 配置。
#ifndef TH06_VULKAN_SKIP_VMA
#  include <vma/vk_mem_alloc.h>
#endif
