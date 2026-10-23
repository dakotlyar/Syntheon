// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/dxgi/dxgi_probe.h"

#include <d3d11_1.h>
#include <d3d12.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace sb::gpu::dxgi {

namespace {

constexpr UINT kWarpVendor = 0x1414, kWarpDevice = 0x008c;  // Microsoft Basic Render Driver

const char *level12(D3D_FEATURE_LEVEL l)
{
    switch (l) {
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    default: return "12_0";
    }
}

ApiProbe probeDx12(IDXGIAdapter1 *adapter, bool *umaKnown, bool *uma)
{
    ApiProbe p;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        p.reason = "Needs DirectX 12 feature level 12_0";
        return p;
    }
    // L5: unified memory (integrated GPUs) from the device itself.
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch))) {
        *umaKnown = true;
        *uma = arch.UMA == TRUE;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_0};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
        p.reason = "Needs shader model 6.0";
        return p;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
    fl.NumFeatureLevels = UINT(sizeof levels / sizeof levels[0]);
    fl.pFeatureLevelsRequested = levels;
    fl.MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;
    device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
    p.ok = true;
    p.version = std::string("DirectX 12 (feature level ") + level12(fl.MaxSupportedFeatureLevel) + ")";
    return p;
}

// A D3D11 device on the adapter; `flags` D3D11_CREATE_DEVICE_BGRA_SUPPORT for Direct2D.
HRESULT createDevice11(IDXGIAdapter1 *adapter, UINT flags, ComPtr<ID3D11Device> *device, D3D_FEATURE_LEVEL *level)
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    return D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION, device->GetAddressOf(), level, nullptr);
}

// M5: one D3D11 device with BGRA support answers both DirectX 11 (feature level, constant-buffer offsets:
// the backend's own requirement, task 20) and Direct2D (BGRA support).
void probeDx11AndD2d(IDXGIAdapter1 *adapter, ApiProbe *dx11, ApiProbe *d2d)
{
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (SUCCEEDED(createDevice11(adapter, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &device, &level))) {
        d2d->ok = true;
        d2d->version = "Direct2D";
    } else {
        d2d->reason = "Needs a Direct3D 11 device with BGRA support";
        // No device with BGRA support: the plain one still decides DirectX 11.
        if (FAILED(createDevice11(adapter, 0, &device, &level))) {
            dx11->reason = "Needs DirectX 11 feature level 11_0";
            return;
        }
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    ComPtr<ID3D11Device1> device1;
    if (FAILED(device.As(&device1)) || FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof options))
        || !options.ConstantBufferOffsetting) {
        dx11->reason = "Needs DirectX 11.1 constant-buffer offsets";
        return;
    }
    dx11->ok = true;
    dx11->version = std::string("DirectX 11 (feature level ") + (level == D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0") + ")";
}

AdapterProbe describe(IDXGIAdapter1 *adapter, const DXGI_ADAPTER_DESC1 &desc)
{
    AdapterProbe a;
    a.name = utf8(desc.Description);
    a.vendorId = desc.VendorId;
    a.deviceId = desc.DeviceId;
    a.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    a.dedicatedBytes = std::int64_t(desc.DedicatedVideoMemory);
    const DWORD low = desc.AdapterLuid.LowPart;
    const LONG high = desc.AdapterLuid.HighPart;
    for (int i = 0; i < 4; ++i) {
        a.luid[std::size_t(i)] = std::uint8_t(low >> (8 * i));  // little-endian, as VkPhysicalDeviceIDProperties::deviceLUID
        a.luid[std::size_t(4 + i)] = std::uint8_t(DWORD(high) >> (8 * i));
    }
    a.sharedBytes = std::int64_t(desc.SharedSystemMemory);
    {
        ComPtr<IDXGIOutput> output;
        a.drivesOutput = SUCCEEDED(adapter->EnumOutputs(0, &output));  // DXGI_ERROR_NOT_FOUND: none
    }
    a.dx12 = probeDx12(adapter, &a.umaKnown, &a.uma);
    probeDx11AndD2d(adapter, &a.dx11, &a.d2d);
    return a;
}

} // namespace

std::vector<AdapterProbe> probeAdapters(bool allowSoftware, std::string *error)
{
    std::vector<AdapterProbe> out;
    ComPtr<IDXGIFactory1> factory;
    if (const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(hr)) {
        *error = failure("CreateDXGIFactory1", hr);
        return out;
    }
    ComPtr<IDXGIFactory6> factory6;
    factory.As(&factory6);
    bool warpListed = false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory6 ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
                                    : factory->EnumAdapters1(i, &adapter);
        if (FAILED(hr))
            break;  // DXGI_ERROR_NOT_FOUND: the end of the list
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        warpListed = warpListed || (desc.VendorId == kWarpVendor && desc.DeviceId == kWarpDevice);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !allowSoftware)
            continue;
        out.push_back(describe(adapter.Get(), desc));
    }
    // WARP isn't always in the list; dxgi::findAdapter finds it through EnumWarpAdapter under --allow-software.
    if (allowSoftware && !warpListed) {
        ComPtr<IDXGIFactory4> factory4;
        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(factory.As(&factory4)) && SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
            DXGI_ADAPTER_DESC1 desc{};
            warp->GetDesc1(&desc);
            out.push_back(describe(warp.Get(), desc));
        }
    }
    return out;
}

} // namespace sb::gpu::dxgi
