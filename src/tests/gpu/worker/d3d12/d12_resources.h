// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Textures, buffers, uploads and state tracking for the D3D12 backend (Phase
// 4a task 18; DX-1: every resource's current state is tracked, and a barrier
// goes before every use in another state). Windows only (P-WIN).

#include "tests/gpu/worker/d3d12/d12_context.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sb::gpu::d3d12 {

// A texture and its tracked state (§5.3's targets, the atlas, the textures).
struct Image {
    ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t width = 0, height = 0, mips = 1;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
// A buffer: UPLOAD (mapped, GENERIC_READ), READBACK (mapped, COPY_DEST) or DEFAULT.
struct Buffer {
    ComPtr<ID3D12Resource> resource;
    void *mapped = nullptr;
    UINT64 size = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

// clear: the optimised clear value of a render target or depth buffer, or null.
std::string makeImage(Context &, std::uint32_t w, std::uint32_t h, DXGI_FORMAT, D3D12_RESOURCE_FLAGS, std::uint32_t mips,
                      D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE *clear, Image *out);
std::string makeBuffer(Context &, UINT64 bytes, D3D12_HEAP_TYPE, Buffer *out);
std::string uploadBuffer(Context &, Buffer &dst, const void *data, std::size_t bytes);  // DEFAULT: staging + one-time submit + wait
std::string uploadTexture(Context &, const Texture &level0, Image *out);               // mipChain(), PIXEL_SHADER_RESOURCE at the end
// DX-1: a barrier from the tracked state to `to` (none when they are the same).
void transition(ID3D12GraphicsCommandList *, Image &, D3D12_RESOURCE_STATES to);
void transition(ID3D12GraphicsCommandList *, ID3D12Resource *, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
// An SRV of the whole texture at heap index `index`; a null SRV (a Texture2D) for img == null.
void writeSrv(Context &, UINT index, const Image *img);
// Copies mip 0 of img (in COPY_SOURCE) into `readback` (a READBACK buffer of
// readbackBytes()), recorded into list; then unpack() gives W x H x 4, top row first.
UINT64 readbackBytes(Context &, const Image &img, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprint);
void recordReadback(ID3D12GraphicsCommandList *, Image &img, Buffer &readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint);
void unpack(const Buffer &readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, std::uint32_t w, std::uint32_t h,
            std::vector<std::uint8_t> *rgba8);

} // namespace sb::gpu::d3d12
