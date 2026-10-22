// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// §4.3's GL_VENDOR / GL_RENDERER rules, in one place for the model (QString wrappers in
// gpu_identity) and the Qt-free GL backend (checkpoint 3, L6). Header-only, std only.
// Matching is case-insensitive.

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace sb::gpu {

inline bool glContainsNoCase(std::string_view text, std::string_view needle)
{
    if (needle.empty() || text.size() < needle.size())
        return needle.empty();
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && std::tolower(static_cast<unsigned char>(text[i + j])) == std::tolower(static_cast<unsigned char>(needle[j])))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

// GL_VENDOR -> PCI vendor: NVIDIA 0x10de; AMD, ATI Technologies, Advanced Micro Devices 0x1002; Intel 0x8086; Qualcomm 0x5143; 0 = unknown.
inline std::uint32_t glVendorPci(std::string_view vendor)
{
    if (glContainsNoCase(vendor, "NVIDIA"))
        return 0x10de;
    if (glContainsNoCase(vendor, "AMD") || glContainsNoCase(vendor, "ATI Technologies") || glContainsNoCase(vendor, "Advanced Micro Devices"))
        return 0x1002;
    if (glContainsNoCase(vendor, "Intel"))
        return 0x8086;
    if (glContainsNoCase(vendor, "Qualcomm"))
        return 0x5143;
    return 0;
}

// llvmpipe | softpipe | GDI Generic | Microsoft Basic Render
inline bool glSoftwareRenderer(std::string_view renderer)
{
    for (const char *needle : {"llvmpipe", "softpipe", "GDI Generic", "Microsoft Basic Render"}) {
        if (glContainsNoCase(renderer, needle))
            return true;
    }
    return false;
}

} // namespace sb::gpu
