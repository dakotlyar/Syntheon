// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// DXGI for the Direct3D backends and Direct2D (Phase 4a design §4.1, §5.4,
// §5.5, task 18): the adapter from the GPU id and the flip-model swapchain of
// the test window, shared by DX12, DX11 and D2D. Windows only (P-WIN).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include "tests/gpu/worker/backend.h"

#include <cstdint>
#include <string>

namespace sb::gpu::dxgi {

// "<what> failed (0x887A0005)"; a removed or reset device is "LOST: the GPU
// driver reset (DXGI_ERROR_DEVICE_REMOVED)" (backend.h's reason prefixes).
std::string failure(const char *what, HRESULT hr);
// UTF-16 to UTF-8 (adapter names).
std::string utf8(const wchar_t *text);

// Flip-discard, 3 buffers, BGRA8_UNORM, tearing when supported (§5.5); the device is an ID3D12CommandQueue* (DX12) or an ID3D11Device* (DX11/D2D).
struct Swapchain {
    std::string create(IUnknown *deviceOrQueue, HWND hwnd, std::uint32_t width, std::uint32_t height);
    std::string present();                 // Present(0, tearing ? ALLOW_TEARING : 0); OCCLUDED -> "the test window was hidden ..."
    void destroy();
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain; bool tearing = false; HANDLE waitable = nullptr;
    std::string modeName() const;          // "flip, tearing" / "flip, no tearing"

    // Beyond the declaration: waits on the frame-latency waitable object (1 s
    // steps, at most 10 s; P-STOP), so at most 2 frames are queued (§5.4).
    std::string waitForFrame();
    std::uint32_t width = 0, height = 0;
    static constexpr UINT kBuffers = 3;
};

// By LUID, else id "#n" order: the n-th adapter with the id's vendor:device in
// IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE) order (EnumAdapters1
// without IDXGIFactory6), software adapters included only with allowSoftware;
// WARP (1414:008c) is also found through EnumWarpAdapter. Task 23's probe must
// number adapters the same way.
std::string findAdapter(const BackendInit &init, Microsoft::WRL::ComPtr<IDXGIAdapter1> *out, bool allowSoftware);

} // namespace sb::gpu::dxgi
