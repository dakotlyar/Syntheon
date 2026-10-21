// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Vulkan loader on Windows (vk_loader_win.cpp; P-WIN). C headers only, so
// A-W can check it without a Windows C++ library.

#include <volk.h>

namespace sb::gpu::vk {

// vkGetInstanceProcAddr of System32's vulkan-1.dll, opened by full system
// path (never the current folder, PATH or the app folder: SynBench runs
// elevated; the vulkan_probe.cpp rule); null when there is none.
PFN_vkGetInstanceProcAddr systemVulkanLoader();

} // namespace sb::gpu::vk
