// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `synbench-gpu --probe` on Windows (Phase 4a design §4.1, §4.2, task 23): the DXGI adapters and, per
// adapter, whether Direct3D 12, Direct3D 11 and Direct2D can be used on it. Adapters are numbered
// exactly as dxgi::findAdapter does (high-performance order; software adapters only with
// allowSoftware; WARP, 1414:008c, added last when the enumeration doesn't list it), so the GUI's
// "<vendor>:<device>#n" id finds the same adapter at run time. Windows only (P-WIN).

#include "tests/gpu/worker/dxgi/dxgi_swapchain.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sb::gpu::dxgi {

struct ApiProbe {
    bool ok = false;
    std::string version, reason;  // version "DirectX 12 (feature level 12_1)"; reason the §4.2 string
};

struct AdapterProbe {
    std::string name;
    std::uint32_t vendorId = 0, deviceId = 0;
    bool software = false;
    std::int64_t dedicatedBytes = 0;
    std::array<std::uint8_t, 8> luid{};
    ApiProbe dx12, dx11, d2d;
};

// Empty on failure with *error ("CreateDXGIFactory1 failed (0x...)").
std::vector<AdapterProbe> probeAdapters(bool allowSoftware, std::string *error);

} // namespace sb::gpu::dxgi
