// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The D3D12 backend's device, queue and fence (Phase 4a design §4.2, §5.4,
// §5.6, task 18). Windows only (P-WIN).

#include "tests/gpu/worker/dxgi/dxgi_swapchain.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>

#include <atomic>
#include <functional>
#include <string>

namespace sb::gpu::d3d12 {

using Microsoft::WRL::ComPtr;

class Context { public:
    std::string create(const BackendInit &, bool debugLayer, BackendInfo *);   // FL 12_0, SM 6.0 check (§4.2 reasons)
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12Fence> fence; UINT64 fenceValue = 0; HANDLE fenceEvent{};
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap, srvHeap /*shader-visible*/, samplerHeap /*shader-visible*/; UINT64 timestampFrequency = 0;

    // Beyond the declaration in task 18.
    ~Context();
    void destroy();
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12InfoQueue> infoQueue;    // the debug layer's messages (--validation), else null
    std::atomic<int> validationErrors{0};
    std::string firstValidationError;
    UINT rtvSize = 0, dsvSize = 0, srvSize = 0;
    static constexpr UINT kRtvCount = 64, kDsvCount = 16, kSrvCount = 4096;
    ComPtr<ID3D12CommandAllocator> uploadAllocator;  // one-time submits (uploads, readbacks)
    ComPtr<ID3D12GraphicsCommandList> uploadList;

    // "validation: <the first error>" once the debug layer reported an error or
    // corruption message (polled from the info queue), else "".
    std::string validationProblem();
    // Signals the fence and returns its new value.
    std::string signal(UINT64 *value);
    // Waits until the fence reaches value: 1 s steps (P-STOP), at most 10 s,
    // then "LOST: the GPU stopped responding".
    std::string wait(UINT64 value);
    // Records with record(), executes, waits.
    std::string oneTimeSubmit(const std::function<void(ID3D12GraphicsCommandList *)> &record);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(UINT index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu(UINT index) const;
    // A removed device's reason, or "" (checked after a failed call).
    std::string removedReason();
};

} // namespace sb::gpu::d3d12
