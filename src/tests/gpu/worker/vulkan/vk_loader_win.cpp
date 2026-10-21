// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "tests/gpu/worker/vulkan/vk_loader.h"

namespace sb::gpu::vk {

PFN_vkGetInstanceProcAddr systemVulkanLoader()
{
    static const HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader)
        return nullptr;
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void *>(GetProcAddress(loader, "vkGetInstanceProcAddr")));
}

} // namespace sb::gpu::vk
