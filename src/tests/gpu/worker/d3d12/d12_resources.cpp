// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/gpu/worker/d3d12/d12_resources.h"

#include "tests/gpu/worker/scene/textures.h"

#include <cstring>

namespace sb::gpu::d3d12 {

namespace {

D3D12_HEAP_PROPERTIES heapOf(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

} // namespace

std::string makeImage(Context &ctx, std::uint32_t w, std::uint32_t h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, std::uint32_t mips,
                      D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE *clear, Image *out)
{
    Image img;
    img.format = format;
    img.width = w;
    img.height = h;
    img.mips = mips;
    img.state = initial;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = UINT16(mips);
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = flags;
    const D3D12_HEAP_PROPERTIES heap = heapOf(D3D12_HEAP_TYPE_DEFAULT);
    if (const HRESULT hr = ctx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, initial, clear, IID_PPV_ARGS(&img.resource));
        FAILED(hr))
        return dxgi::failure("CreateCommittedResource (texture)", hr);
    *out = img;
    return {};
}

std::string makeBuffer(Context &ctx, UINT64 bytes, D3D12_HEAP_TYPE type, Buffer *out)
{
    Buffer b;
    b.size = bytes;
    b.state = type == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
              : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                 : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES heap = heapOf(type);
    if (const HRESULT hr = ctx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, b.state, nullptr, IID_PPV_ARGS(&b.resource));
        FAILED(hr))
        return dxgi::failure("CreateCommittedResource (buffer)", hr);
    if (type != D3D12_HEAP_TYPE_DEFAULT) {
        const D3D12_RANGE none{0, 0};  // the CPU reads a READBACK buffer only through readback paths
        if (const HRESULT hr = b.resource->Map(0, type == D3D12_HEAP_TYPE_UPLOAD ? &none : nullptr, &b.mapped); FAILED(hr))
            return dxgi::failure("ID3D12Resource::Map", hr);
    }
    *out = b;
    return {};
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

void transition(ID3D12GraphicsCommandList *list, Image &img, D3D12_RESOURCE_STATES to)
{
    transition(list, img.resource.Get(), img.state, to);
    img.state = to;
}

std::string uploadBuffer(Context &ctx, Buffer &dst, const void *data, std::size_t bytes)
{
    if (bytes > dst.size)
        return "an upload larger than its buffer";
    if (dst.mapped) {
        std::memcpy(dst.mapped, data, bytes);
        return {};
    }
    Buffer staging;
    if (std::string e = makeBuffer(ctx, bytes, D3D12_HEAP_TYPE_UPLOAD, &staging); !e.empty())
        return e;
    std::memcpy(staging.mapped, data, bytes);
    return ctx.oneTimeSubmit([&](ID3D12GraphicsCommandList *list) {
        transition(list, dst.resource.Get(), dst.state, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(dst.resource.Get(), 0, staging.resource.Get(), 0, bytes);
        // Vertex, index and constant buffers are all GENERIC_READ-compatible reads afterwards.
        transition(list, dst.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
        dst.state = D3D12_RESOURCE_STATE_GENERIC_READ;
    });
}

std::string uploadTexture(Context &ctx, const Texture &level0, Image *out)
{
    const std::vector<Texture> chain = level0.mips ? mipChain(level0) : std::vector<Texture>{level0};
    Image img;
    if (std::string e = makeImage(ctx, level0.width, level0.height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                  std::uint32_t(chain.size()), D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &img);
        !e.empty())
        return e;
    const D3D12_RESOURCE_DESC rd = img.resource->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(chain.size());
    std::vector<UINT> rows(chain.size());
    std::vector<UINT64> rowBytes(chain.size());
    UINT64 total = 0;
    ctx.device->GetCopyableFootprints(&rd, 0, UINT(chain.size()), 0, footprints.data(), rows.data(), rowBytes.data(), &total);
    Buffer staging;
    if (std::string e = makeBuffer(ctx, total, D3D12_HEAP_TYPE_UPLOAD, &staging); !e.empty())
        return e;
    for (std::size_t level = 0; level < chain.size(); ++level) {
        auto *dst = static_cast<std::uint8_t *>(staging.mapped) + footprints[level].Offset;
        const std::size_t srcPitch = std::size_t(chain[level].width) * 4;
        for (UINT y = 0; y < rows[level]; ++y)
            std::memcpy(dst + std::size_t(y) * footprints[level].Footprint.RowPitch, chain[level].rgba8.data() + y * srcPitch, srcPitch);
    }
    std::string e = ctx.oneTimeSubmit([&](ID3D12GraphicsCommandList *list) {
        for (UINT level = 0; level < chain.size(); ++level) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = img.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = level;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = staging.resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[level];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        transition(list, img, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    });
    if (!e.empty())
        return e;
    *out = img;
    return {};
}

void writeSrv(Context &ctx, UINT index, const Image *img)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = img ? img->format : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (sd.Format == DXGI_FORMAT_R32_TYPELESS)
        sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = img ? img->mips : 1;
    ctx.device->CreateShaderResourceView(img ? img->resource.Get() : nullptr, &sd, ctx.srvCpu(index));
}

UINT64 readbackBytes(Context &ctx, const Image &img, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprint)
{
    const D3D12_RESOURCE_DESC rd = img.resource->GetDesc();
    UINT64 total = 0;
    ctx.device->GetCopyableFootprints(&rd, 0, 1, 0, footprint, nullptr, nullptr, &total);
    return total;
}

void recordReadback(ID3D12GraphicsCommandList *list, Image &img, Buffer &readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint)
{
    transition(list, img, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = img.resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

void unpack(const Buffer &readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, std::uint32_t w, std::uint32_t h,
            std::vector<std::uint8_t> *rgba8)
{
    rgba8->resize(std::size_t(w) * h * 4);
    const auto *src = static_cast<const std::uint8_t *>(readback.mapped) + footprint.Offset;
    for (std::uint32_t y = 0; y < h; ++y)
        std::memcpy(rgba8->data() + std::size_t(y) * w * 4, src + std::size_t(y) * footprint.Footprint.RowPitch, std::size_t(w) * 4);
}

} // namespace sb::gpu::d3d12
