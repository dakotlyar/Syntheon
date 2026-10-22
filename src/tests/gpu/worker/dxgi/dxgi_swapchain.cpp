// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/dxgi/dxgi_swapchain.h"

#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace sb::gpu::dxgi {

namespace {

constexpr UINT kWarpVendor = 0x1414, kWarpDevice = 0x008c;  // Microsoft Basic Render Driver

std::uint32_t occurrence(const std::string &gpuId)
{
    const std::size_t hash = gpuId.rfind('#');
    return hash == std::string::npos ? 0 : std::uint32_t(std::strtoul(gpuId.c_str() + hash + 1, nullptr, 10));
}

} // namespace

std::string failure(const char *what, HRESULT hr)
{
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        return "LOST: the GPU driver reset (DXGI_ERROR_DEVICE_REMOVED)";
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(what) + " failed (" + code + ")";
}

std::string utf8(const wchar_t *text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string out(std::size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string findAdapter(const BackendInit &init, ComPtr<IDXGIAdapter1> *out, bool allowSoftware)
{
    ComPtr<IDXGIFactory1> factory;
    if (const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(hr))
        return failure("CreateDXGIFactory1", hr);
    ComPtr<IDXGIFactory6> factory6;
    factory.As(&factory6);
    const std::uint32_t wanted = occurrence(init.gpuId);
    std::uint32_t seen = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory6 ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
                                    : factory->EnumAdapters1(i, &adapter);
        if (FAILED(hr))
            break;  // DXGI_ERROR_NOT_FOUND: the end of the list
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !allowSoftware)
            continue;
        bool match = false;
        if (init.hasLuid) {
            match = desc.AdapterLuid.LowPart == (DWORD(init.luid[0]) | DWORD(init.luid[1]) << 8 | DWORD(init.luid[2]) << 16 | DWORD(init.luid[3]) << 24)
                    && desc.AdapterLuid.HighPart
                           == LONG(DWORD(init.luid[4]) | DWORD(init.luid[5]) << 8 | DWORD(init.luid[6]) << 16 | DWORD(init.luid[7]) << 24);
        } else if (desc.VendorId == init.vendorId && desc.DeviceId == init.deviceId) {
            match = seen++ == wanted;
        }
        if (match) {
            *out = adapter;
            return {};
        }
    }
    // WARP isn't always in the list (it is as the Basic Render Driver on a machine without a GPU).
    if (allowSoftware && init.vendorId == kWarpVendor && init.deviceId == kWarpDevice) {  // also with a LUID: the probe lists WARP from EnumWarpAdapter
        ComPtr<IDXGIFactory4> factory4;
        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(factory.As(&factory4)) && SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
            *out = warp;
            return {};
        }
    }
    return "N/A: this GPU isn't in DirectX's adapter list";
}

std::string adapterDescription(const BackendInit &init, bool allowSoftware)
{
    ComPtr<IDXGIAdapter1> adapter;
    if (!findAdapter(init, &adapter, allowSoftware).empty())
        return {};
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    return utf8(desc.Description);
}

int countHardwareAdapters(std::uint32_t vendorId)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return 0;
    int count = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(i, &adapter)))
            break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId == vendorId)
            ++count;
    }
    return count;
}

std::string Swapchain::create(IUnknown *deviceOrQueue, HWND hwnd, std::uint32_t w, std::uint32_t h)
{
    destroy();
    ComPtr<IDXGIFactory2> factory;
    if (const HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)); FAILED(hr))
        return failure("CreateDXGIFactory2", hr);
    // §5.5: tearing (vsync off in a window) where the system allows it.
    ComPtr<IDXGIFactory5> factory5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5))
        && FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof allowTearing)))
        allowTearing = FALSE;
    tearing = allowTearing == TRUE;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBuffers;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ComPtr<IDXGISwapChain1> sc1;
    if (const HRESULT hr = factory->CreateSwapChainForHwnd(deviceOrQueue, hwnd, &desc, nullptr, nullptr, &sc1); FAILED(hr)) {
        // A system that can't present this GPU into the window at all (backend.h: N/A; the
        // backend turns it into an Error after the first resume).
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            return failure("CreateSwapChainForHwnd", hr);
        return "N/A: this GPU can't present to the test window on this display system (" + failure("CreateSwapChainForHwnd", hr) + ")";
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (const HRESULT hr = sc1.As(&swapchain); FAILED(hr)) {
        destroy();
        return failure("IDXGISwapChain3", hr);
    }
    swapchain->SetMaximumFrameLatency(2);
    waitable = swapchain->GetFrameLatencyWaitableObject();
    DXGI_SWAP_CHAIN_DESC1 got{};
    swapchain->GetDesc1(&got);
    width = got.Width;
    height = got.Height;
    return {};
}

std::string Swapchain::waitForFrame()
{
    if (!waitable)
        return {};
    for (int s = 0; s < 10; ++s) {
        const DWORD r = WaitForSingleObjectEx(waitable, 1000, TRUE);
        if (r == WAIT_OBJECT_0)
            return {};
        if (r != WAIT_TIMEOUT && r != WAIT_IO_COMPLETION)
            return "waiting for the swapchain failed";
    }
    return "LOST: the GPU stopped responding";
}

std::string Swapchain::present()
{
    const HRESULT hr = swapchain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (hr == DXGI_STATUS_OCCLUDED)
        return "the test window was hidden or minimised during the measurement";
    if (FAILED(hr))
        return failure("Present", hr);
    return {};
}

void Swapchain::destroy()
{
    if (waitable)
        CloseHandle(waitable);
    waitable = nullptr;
    swapchain.Reset();
    width = height = 0;
}

std::string Swapchain::modeName() const
{
    return tearing ? "flip, tearing" : "flip, no tearing";
}

} // namespace sb::gpu::dxgi
