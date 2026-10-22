// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/vulkan/vk_context.h"

#include "tests/gpu/worker/vulkan/layer_manifests.h"
#include "tests/gpu/worker/vulkan/vk_loader.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace sb::gpu::vk {

namespace {

constexpr const char *kValidationLayer = "VK_LAYER_KHRONOS_validation";

std::string versionText(std::uint32_t v)
{
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "."
           + std::to_string(VK_API_VERSION_PATCH(v));
}

bool hasExtension(const std::vector<VkExtensionProperties> &list, const char *name)
{
    for (const VkExtensionProperties &e : list) {
        if (std::strcmp(e.extensionName, name) == 0)
            return true;
    }
    return false;
}

std::vector<VkExtensionProperties> instanceExtensions(const char *layer)
{
    std::uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &n, nullptr) != VK_SUCCESS)
        return {};
    std::vector<VkExtensionProperties> out(n);
    if (vkEnumerateInstanceExtensionProperties(layer, &n, out.data()) < VK_SUCCESS)
        return {};
    out.resize(n);
    return out;
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice pd)
{
    std::uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr) != VK_SUCCESS)
        return {};
    std::vector<VkExtensionProperties> out(n);
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, out.data()) < VK_SUCCESS)
        return {};
    out.resize(n);
    return out;
}

// The layer's specVersion, 0 when it isn't installed.
std::uint32_t layerSpecVersion(const char *name)
{
    std::uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS)
        return 0;
    std::vector<VkLayerProperties> layers(n);
    if (vkEnumerateInstanceLayerProperties(&n, layers.data()) < VK_SUCCESS)
        return 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (std::strcmp(layers[i].layerName, name) == 0)
            return layers[i].specVersion;
    }
    return 0;
}

bool hasLayer(const char *name)
{
    std::uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkLayerProperties> layers(n);
    if (vkEnumerateInstanceLayerProperties(&n, layers.data()) < VK_SUCCESS)
        return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (std::strcmp(layers[i].layerName, name) == 0)
            return true;
    }
    return false;
}

// "#n" at the end of a GPU id (P11: a device without a PCI slot, or Windows):
// the n-th device with that vendor:device, 0 when absent.
std::uint32_t occurrence(const std::string &gpuId)
{
    const std::size_t hash = gpuId.rfind('#');
    if (hash == std::string::npos)
        return 0;
    return std::uint32_t(std::strtoul(gpuId.c_str() + hash + 1, nullptr, 10));
}

std::string pciSlotOf(VkPhysicalDevice pd)
{
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pci};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    char slot[32];
    std::snprintf(slot, sizeof slot, "%04x:%02x:%02x.%x", pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
    return slot;
}

// P17 (Antilles' rule): layers actually loaded into this process, never the installed ones
// (layer_manifests.h). Linux: the implicit-layer manifests in the loader's folders against
// /proc/self/maps (the loader maps a layer only when it uses it; implicit layers are normally
// disabled, VK_LOADER_LAYERS_DISABLE=~implicit~, which loaders before 1.3.234 ignore).
// Windows: the manifests named by the HKLM and HKCU SOFTWARE\Khronos\Vulkan\ImplicitLayers
// values against the loaded modules, by full path.
std::string readTextFile(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<std::string> activeLayerNames()
{
    std::vector<LayerEntry> entries;
    std::vector<std::string> loaded;
#if defined(__linux__)
    {
        std::ifstream f("/proc/self/maps");
        std::string line;
        while (std::getline(f, line)) {
            const std::size_t slash = line.find('/');
            if (slash != std::string::npos)
                loaded.push_back(line.substr(slash));
        }
    }
    // The loader's search order (its documentation): $XDG_CONFIG_HOME or ~/.config, $XDG_CONFIG_DIRS
    // or /etc/xdg, /etc, $XDG_DATA_HOME or ~/.local/share, $XDG_DATA_DIRS or /usr/local/share:/usr/share.
    const auto env = [](const char *name) { const char *v = std::getenv(name); return std::string(v ? v : ""); };
    const auto split = [](const std::string &list, std::vector<std::string> *out) {
        std::size_t from = 0;
        while (from <= list.size()) {
            const std::size_t colon = list.find(':', from);
            const std::string item = list.substr(from, colon == std::string::npos ? std::string::npos : colon - from);
            if (!item.empty())
                out->push_back(item);
            if (colon == std::string::npos)
                break;
            from = colon + 1;
        }
    };
    const std::string home = env("HOME");
    std::vector<std::string> bases;
    bases.push_back(env("XDG_CONFIG_HOME").empty() ? (home.empty() ? "" : home + "/.config") : env("XDG_CONFIG_HOME"));
    split(env("XDG_CONFIG_DIRS").empty() ? "/etc/xdg" : env("XDG_CONFIG_DIRS"), &bases);
    bases.push_back("/etc");
    bases.push_back(env("XDG_DATA_HOME").empty() ? (home.empty() ? "" : home + "/.local/share") : env("XDG_DATA_HOME"));
    split(env("XDG_DATA_DIRS").empty() ? "/usr/local/share:/usr/share" : env("XDG_DATA_DIRS"), &bases);
    std::vector<std::filesystem::path> dirs;
    for (const std::string &base : bases) {
        if (!base.empty())
            dirs.push_back(std::filesystem::path(base) / "vulkan/implicit_layer.d");
    }
    for (const std::filesystem::path &dir : dirs) {
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".json")
                continue;
            const std::vector<LayerEntry> found = parseLayerManifest(readTextFile(entry.path()), entry.path().string());
            entries.insert(entries.end(), found.begin(), found.end());
        }
    }
#elif defined(_WIN32)
    HMODULE modules[1024];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameExW(GetCurrentProcess(), modules[i], path, MAX_PATH)) {
                std::string utf8(std::size_t(WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr)), '\0');
                WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), int(utf8.size()), nullptr, nullptr);
                utf8.resize(std::strlen(utf8.c_str()));
                loaded.push_back(utf8);
            }
        }
    }
    // HKLM and HKCU: value names are manifest paths; a data of 0 means enabled.
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
            continue;
        for (DWORD index = 0;; ++index) {
            wchar_t name[2048];
            DWORD nameLen = DWORD(sizeof name / sizeof name[0]), type = 0, data = 1, dataLen = sizeof data;
            const LONG r = RegEnumValueW(key, index, name, &nameLen, nullptr, &type, reinterpret_cast<LPBYTE>(&data), &dataLen);
            if (r == ERROR_NO_MORE_ITEMS)
                break;
            if (r != ERROR_SUCCESS || type != REG_DWORD || data != 0)
                continue;
            std::string utf8(std::size_t(WideCharToMultiByte(CP_UTF8, 0, name, int(nameLen), nullptr, 0, nullptr, nullptr)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, name, int(nameLen), utf8.data(), int(utf8.size()), nullptr, nullptr);
            const std::vector<LayerEntry> found = parseLayerManifest(readTextFile(std::filesystem::path(name, name + nameLen)), utf8);
            entries.insert(entries.end(), found.begin(), found.end());
        }
        RegCloseKey(key);
    }
#endif
    return activeLayers(entries, loaded);
}

} // namespace

std::string resultName(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VkResult " + std::to_string(int(r));
    }
}

std::string failure(const char *what, VkResult r)
{
    if (r == VK_ERROR_DEVICE_LOST)
        return "LOST: the GPU driver reset (VK_ERROR_DEVICE_LOST)";
    return std::string(what) + " failed (" + resultName(r) + ")";
}

std::string loadVulkanLoader()
{
#if defined(_WIN32)
    const PFN_vkGetInstanceProcAddr getInstanceProcAddr = systemVulkanLoader();
    if (!getInstanceProcAddr)
        return "N/A: No Vulkan driver is installed";
    volkInitializeCustom(getInstanceProcAddr);
#else
    if (volkInitialize() != VK_SUCCESS)
        return "N/A: No Vulkan driver is installed";
#endif
    return {};
}

VKAPI_ATTR VkBool32 VKAPI_CALL Context::onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT types,
                                                  const VkDebugUtilsMessengerCallbackDataEXT *data, void *self)
{
    if (!(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))
        return VK_FALSE;
    const char *text = data && data->pMessage ? data->pMessage : "(no message)";
    std::fprintf(stderr, "synbench-gpu: vulkan: %s\n", text);
    // Only the validation layer's own errors fail a run (a loader complaint
    // about some other layer is printed, not counted).
    if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
        auto *ctx = static_cast<Context *>(self);
        std::lock_guard<std::mutex> lock(ctx->validationMutex);
        if (ctx->validationErrors.fetch_add(1) == 0)
            ctx->firstValidationError = text;
    }
    return VK_FALSE;
}

std::string Context::validationProblem()
{
    if (validationErrors.load() == 0)
        return {};
    std::lock_guard<std::mutex> lock(validationMutex);
    return "validation: " + firstValidationError;
}

std::uint32_t Context::memoryType(std::uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

std::string Context::waitFence(VkFence fence)
{
    for (int s = 0; s < kWaitLimitS; ++s) {
        const VkResult r = vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000ull);
        if (r == VK_SUCCESS)
            return {};
        if (r != VK_TIMEOUT)
            return failure("vkWaitForFences", r);
    }
    return "LOST: the GPU stopped responding";
}

std::string Context::oneTimeSubmit(const std::function<void(VkCommandBuffer)> &record)
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = uploadPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (const VkResult r = vkAllocateCommandBuffers(device, &ai, &cmd); r != VK_SUCCESS)
        return failure("vkAllocateCommandBuffers", r);
    VkFence fence = VK_NULL_HANDLE;
    const auto done = [&](std::string e) {
        if (fence)
            vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, uploadPool, 1, &cmd);
        return e;
    };
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    record(cmd);
    if (const VkResult r = vkEndCommandBuffer(cmd); r != VK_SUCCESS)
        return done(failure("vkEndCommandBuffer", r));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (const VkResult r = vkCreateFence(device, &fi, nullptr, &fence); r != VK_SUCCESS)
        return done(failure("vkCreateFence", r));
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    if (const VkResult r = vkQueueSubmit2(queue, 1, &si, fence); r != VK_SUCCESS)
        return done(failure("vkQueueSubmit2", r));
    return done(waitFence(fence));
}

std::string Context::create(const BackendInit &init, const PresentTarget &target, bool validation, BackendInfo *info)
{
    if (std::string e = loadVulkanLoader(); !e.empty())
        return e;
    std::uint32_t instanceVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&instanceVersion);
    if (instanceVersion < VK_API_VERSION_1_3)
        return "N/A: Needs Vulkan 1.3 (the driver has " + versionText(instanceVersion) + ")";

    // The window system's extensions (none when headless), then the validation hook's.
    const std::vector<VkExtensionProperties> available = instanceExtensions(nullptr);
    std::vector<const char *> extensions;
    for (const std::string &e : target.vulkanInstanceExtensions) {
        if (!hasExtension(available, e.c_str()))
            return "N/A: the Vulkan driver can't draw into a window here (no " + e + ")";
        extensions.push_back(e.c_str());
    }
    std::vector<const char *> layers;
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    const VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger.pfnUserCallback = &Context::onMessage;
    messenger.pUserData = this;
    if (validation) {
        if (!hasLayer(kValidationLayer))
            return "validation: the Khronos validation layer isn't installed";
        const std::vector<VkExtensionProperties> layerExts = instanceExtensions(kValidationLayer);
        if (!hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) && !hasExtension(layerExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            return "validation: no VK_EXT_debug_utils";
        if (!hasExtension(layerExts, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
            return "validation: the validation layer has no VK_EXT_validation_features";
        layers.push_back(kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        // Synchronisation validation: any hazard is an error (task 12, R1), where the layer is new
        // enough to be right about it; older ones get core validation only.
        std::uint32_t spec = layerSpecVersion(kValidationLayer);
#if defined(SYNBENCH_DEV_HOOKS)
        // Dev hook "1.3.204": pretend the validation layer is that old (the test of the sync-validation gate).
        if (const char *fake = std::getenv("SYNBENCH_GPU_FAKE_LAYER_SPEC")) {
            unsigned major = 0, minor = 0, patch = 0;
            if (std::sscanf(fake, "%u.%u.%u", &major, &minor, &patch) == 3)
                spec = VK_MAKE_API_VERSION(0, major, minor, patch);
        }
#endif
        if (layerSupportsSyncValidation(spec)) {
            features.enabledValidationFeatureCount = 1;
            features.pEnabledValidationFeatures = enables;
        } else {
            std::fprintf(stderr, "synbench-gpu: sync validation skipped: validation layer %u.%u.%u too old\n", VK_API_VERSION_MAJOR(spec),
                         VK_API_VERSION_MINOR(spec), VK_API_VERSION_PATCH(spec));
        }
        features.pNext = &messenger;  // also covers vkCreateInstance / vkDestroyInstance
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SynBench GPU test";
    app.pEngineName = "SynBench";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = validation ? &features : nullptr;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = std::uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = std::uint32_t(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    if (const VkResult r = vkCreateInstance(&ici, nullptr, &instance); r != VK_SUCCESS) {
        instance = VK_NULL_HANDLE;
        if (r == VK_ERROR_INCOMPATIBLE_DRIVER)
            return "N/A: No Vulkan driver is installed";
        return failure("vkCreateInstance", r);
    }
    volkLoadInstance(instance);
    if (validation) {
        if (const VkResult r = vkCreateDebugUtilsMessengerEXT(instance, &messenger, nullptr, &m_messenger); r != VK_SUCCESS)
            return failure("vkCreateDebugUtilsMessengerEXT", r);
    }

    // The device by id (§4.1).
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (count)
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
    devices.resize(count);
    const std::uint32_t wantedOccurrence = occurrence(init.gpuId);
    std::uint32_t seen = 0;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.vendorID != init.vendorId || p.deviceID != init.deviceId)
            continue;
        const std::vector<VkExtensionProperties> exts = deviceExtensions(pd);
        bool match = false;
        if (init.hasLuid && p.apiVersion >= VK_API_VERSION_1_1) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
            vkGetPhysicalDeviceProperties2(pd, &p2);
            match = id.deviceLUIDValid && std::memcmp(id.deviceLUID, init.luid.data(), VK_LUID_SIZE) == 0;
        } else if (!init.pciSlot.empty()) {
            match = hasExtension(exts, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) && pciSlotOf(pd) == init.pciSlot;
        } else {
            match = seen++ == wantedOccurrence;
        }
        if (match) {
            physical = pd;
            properties = p;
            break;
        }
    }
    if (!physical)
        return "N/A: This GPU has no Vulkan driver";
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !init.allowSoftware)
        return std::string("N/A: ") + properties.deviceName + " is a software renderer";
    if (properties.apiVersion < VK_API_VERSION_1_3)
        return "N/A: Needs Vulkan 1.3 (the driver has " + versionText(properties.apiVersion) + ")";
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &have13};
    vkGetPhysicalDeviceFeatures2(physical, &have);
    if (!have13.dynamicRendering || !have13.synchronization2)
        return "N/A: Needs Vulkan 1.3 dynamic rendering and synchronization2";
    anisotropy = have.features.samplerAnisotropy == VK_TRUE;

    std::vector<const char *> deviceExts;
    if (!target.headless) {
        if (!hasExtension(deviceExtensions(physical), VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            return "N/A: the Vulkan driver can't present (no VK_KHR_swapchain)";
        deviceExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        swapchain = true;
    }

    // One graphics queue; whether it can present to the window is checked at resume() (task 9).
    std::uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> qf(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, qf.data());
    bool found = false;
    for (std::uint32_t i = 0; i < families && !found; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamily = i;
            timestampValidBits = qf[i].timestampValidBits;
            found = true;
        }
    }
    if (!found)
        return "N/A: This GPU has no Vulkan graphics queue";
    timestampPeriodNs = double(properties.limits.timestampPeriod);
    timestamps = timestampValidBits > 0 && timestampPeriodNs > 0;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features want13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    want13.dynamicRendering = VK_TRUE;
    want13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &want13};
    want.features.samplerAnisotropy = anisotropy ? VK_TRUE : VK_FALSE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &want;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = std::uint32_t(deviceExts.size());
    dci.ppEnabledExtensionNames = deviceExts.data();
    if (const VkResult r = vkCreateDevice(physical, &dci, nullptr, &device); r != VK_SUCCESS) {
        device = VK_NULL_HANDLE;
        return failure("vkCreateDevice", r);
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = queueFamily;
    if (const VkResult r = vkCreateCommandPool(device, &pci, nullptr, &uploadPool); r != VK_SUCCESS)
        return failure("vkCreateCommandPool", r);

    if (info) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        vkGetPhysicalDeviceProperties2(physical, &p2);
        info->deviceName = properties.deviceName;
        info->apiVersion = "Vulkan " + versionText(properties.apiVersion);
        info->driver = std::string(driver.driverName) + (driver.driverInfo[0] ? std::string(" ") + driver.driverInfo : std::string());
        info->vendorId = properties.vendorID;
        info->deviceId = properties.deviceID;
        info->gpuTimestamps = timestamps;
        info->layers = validation ? kValidationLayer : "";
        const std::vector<std::string> active = activeLayerNames();
        if (!active.empty()) {
            std::string list;
            for (const std::string &n : active)
                list += (list.empty() ? "" : ", ") + n;
            info->note = "Vulkan layers were active (" + list + "): overlays and recorders can lower the result";
        }
        info->presentMode.clear();  // known after resume() (the result's "present")
    }
    return validationProblem();
}

void Context::destroy()
{
    if (device) {
        vkDeviceWaitIdle(device);
        if (uploadPool)
            vkDestroyCommandPool(device, uploadPool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    uploadPool = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    physical = VK_NULL_HANDLE;
    surface = VK_NULL_HANDLE;  // the window's (qt_glue owns it)
    if (instance) {
        if (m_messenger)
            vkDestroyDebugUtilsMessengerEXT(instance, m_messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
    m_messenger = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

} // namespace sb::gpu::vk
