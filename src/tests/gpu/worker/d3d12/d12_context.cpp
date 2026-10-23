// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/d3d12/d12_context.h"

#include <cstdio>
#include <vector>

namespace sb::gpu::d3d12 {

namespace {

const char *featureLevelName(D3D_FEATURE_LEVEL l)
{
    switch (l) {
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    default: return "11_x";
    }
}

// The UMD version DXGI reports (CheckInterfaceSupport), as "31.0.15.5222".
std::string driverVersion(IDXGIAdapter1 *adapter)
{
    LARGE_INTEGER umd{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        return {};
    char text[64];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                  unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
    return text;
}

} // namespace

Context::~Context()
{
    destroy();
}

void Context::destroy()
{
    if (device && fence && queue) {
        UINT64 v = 0;
        if (signal(&v).empty())
            wait(v);
    }
    uploadList.Reset();
    uploadAllocator.Reset();
    infoQueue.Reset();
    rtvHeap.Reset();
    dsvHeap.Reset();
    srvHeap.Reset();
    samplerHeap.Reset();
    fence.Reset();
    queue.Reset();
    device.Reset();
    adapter.Reset();
    if (fenceEvent)
        CloseHandle(fenceEvent);
    fenceEvent = nullptr;
}

std::string Context::create(const BackendInit &init, bool debugLayer, BackendInfo *info)
{
    // --validation: the debug layer when it is installed (Windows' "Graphics Tools").
    if (debugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
        else
            std::fprintf(stderr, "synbench-gpu: d3d12: the debug layer isn't installed; running without validation\n");
    }
    if (std::string e = dxgi::findAdapter(init, &adapter, init.allowSoftware); !e.empty())
        return e;
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (const HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)); FAILED(hr))
        return "N/A: Needs DirectX 12 feature level 12_0";
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_0};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_0)
        return "N/A: Needs shader model 6.0";
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
    fl.NumFeatureLevels = UINT(sizeof levels / sizeof levels[0]);
    fl.pFeatureLevelsRequested = levels;
    fl.MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;
    device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
    if (debugLayer && SUCCEEDED(device.As(&infoQueue))) {
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (const HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)); FAILED(hr))
        return dxgi::failure("CreateCommandQueue", hr);
    if (const HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)); FAILED(hr))
        return dxgi::failure("CreateFence", hr);
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent)
        return "CreateEvent failed";

    const auto heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool shaderVisible, ComPtr<ID3D12DescriptorHeap> *out) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = type;
        hd.NumDescriptors = count;
        hd.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        return device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(out->ReleaseAndGetAddressOf()));
    };
    if (FAILED(heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount, false, &rtvHeap)) || FAILED(heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, &dsvHeap))
        || FAILED(heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, &srvHeap)))
        return "CreateDescriptorHeap failed";
    // samplerHeap stays empty: s0-s3 are the root signature's static samplers (§13.1).
    rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (const HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAllocator)); FAILED(hr))
        return dxgi::failure("CreateCommandAllocator", hr);
    if (const HRESULT hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAllocator.Get(), nullptr, IID_PPV_ARGS(&uploadList));
        FAILED(hr))
        return dxgi::failure("CreateCommandList", hr);
    uploadList->Close();

    // §5.6: timestamps on the direct queue; none -> GPU time "-".
    UINT64 frequency = 0;
    if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)))
        timestampFrequency = frequency;

    if (info) {
        info->deviceName = dxgi::utf8(desc.Description);
        info->apiVersion = std::string("DirectX 12 (feature level ") + featureLevelName(fl.MaxSupportedFeatureLevel) + ")";
        info->driver = driverVersion(adapter.Get());
        info->vendorId = desc.VendorId;
        info->deviceId = desc.DeviceId;
        info->gpuTimestamps = timestampFrequency > 0;
        info->layers.clear();
        info->presentMode.clear();  // known after resume() (the result's "present")
    }
    return validationProblem();
}

std::string Context::validationProblem()
{
    if (infoQueue) {
        const UINT64 n = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(infoQueue->GetMessage(i, nullptr, &bytes)) || bytes == 0)
                continue;
            std::vector<unsigned char> buffer(bytes);
            auto *m = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
            if (FAILED(infoQueue->GetMessage(i, m, &bytes)))
                continue;
            if (m->Severity != D3D12_MESSAGE_SEVERITY_ERROR && m->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION)
                continue;
            const std::string text(m->pDescription ? m->pDescription : "(no message)");
            std::fprintf(stderr, "synbench-gpu: d3d12: %s\n", text.c_str());
            if (validationErrors.fetch_add(1) == 0)
                firstValidationError = text;
        }
        infoQueue->ClearStoredMessages();
    }
    if (validationErrors.load() == 0)
        return {};
    return "validation: " + firstValidationError;
}

std::string Context::removedReason()
{
    if (device && FAILED(device->GetDeviceRemovedReason()))
        return "LOST: the GPU driver reset (DXGI_ERROR_DEVICE_REMOVED)";
    return {};
}

std::string Context::signal(UINT64 *value)
{
    const UINT64 v = ++fenceValue;
    if (const HRESULT hr = queue->Signal(fence.Get(), v); FAILED(hr))
        return dxgi::failure("Signal", hr);
    *value = v;
    return {};
}

std::string Context::wait(UINT64 value)
{
    if (fence->GetCompletedValue() >= value)
        return {};
    if (const HRESULT hr = fence->SetEventOnCompletion(value, fenceEvent); FAILED(hr))
        return dxgi::failure("SetEventOnCompletion", hr);
    for (int s = 0; s < 10; ++s) {
        if (WaitForSingleObject(fenceEvent, 1000) == WAIT_OBJECT_0 || fence->GetCompletedValue() >= value)
            return {};
        if (std::string lost = removedReason(); !lost.empty())
            return lost;
    }
    return "LOST: the GPU stopped responding";
}

std::string Context::oneTimeSubmit(const std::function<void(ID3D12GraphicsCommandList *)> &record)
{
    if (const HRESULT hr = uploadAllocator->Reset(); FAILED(hr))
        return dxgi::failure("ID3D12CommandAllocator::Reset", hr);
    if (const HRESULT hr = uploadList->Reset(uploadAllocator.Get(), nullptr); FAILED(hr))
        return dxgi::failure("ID3D12GraphicsCommandList::Reset", hr);
    record(uploadList.Get());
    if (const HRESULT hr = uploadList->Close(); FAILED(hr))
        return dxgi::failure("ID3D12GraphicsCommandList::Close", hr);
    ID3D12CommandList *lists[] = {uploadList.Get()};
    queue->ExecuteCommandLists(1, lists);
    UINT64 v = 0;
    if (std::string e = signal(&v); !e.empty())
        return e;
    return wait(v);
}

D3D12_CPU_DESCRIPTOR_HANDLE Context::rtv(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * rtvSize;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Context::dsv(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * dsvSize;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Context::srvCpu(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * srvSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Context::srvGpu(UINT index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * srvSize;
    return h;
}

} // namespace sb::gpu::d3d12
