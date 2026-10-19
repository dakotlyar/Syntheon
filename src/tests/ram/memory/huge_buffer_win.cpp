// SPDX-License-Identifier: GPL-3.0-or-later
// HugeBuffer on Windows x64 / ARM64 (Phase 3 design §6.2): large pages only
// when the token already holds SeLockMemoryPrivilege (D10), never granted.
#include "tests/ram/memory/huge_buffer.h"

#include <algorithm>
#include <vector>

#define NOMINMAX
#include <windows.h>

namespace sb::ram {

namespace {

constexpr std::size_t kChunk = std::size_t(64) << 20;

std::size_t roundUp(std::size_t n, std::size_t to)
{
    return (n + to - 1) / to * to;
}

// SeLockMemoryPrivilege in the process token: whether it is there, and
// whether it is enabled.
struct LockMemoryPrivilege {
    bool present = false;
    bool enabled = false;
    LUID luid{};
};

LockMemoryPrivilege lockMemoryPrivilege(HANDLE token)
{
    LockMemoryPrivilege p;
    if (!LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &p.luid))
        return p;
    DWORD length = 0;
    GetTokenInformation(token, TokenPrivileges, nullptr, 0, &length);
    if (length == 0)
        return p;
    std::vector<unsigned char> buffer(length);
    if (!GetTokenInformation(token, TokenPrivileges, buffer.data(), length, &length))
        return p;
    const auto *privileges = reinterpret_cast<const TOKEN_PRIVILEGES *>(buffer.data());
    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
        const LUID_AND_ATTRIBUTES &a = privileges->Privileges[i];
        if (a.Luid.LowPart == p.luid.LowPart && a.Luid.HighPart == p.luid.HighPart) {
            p.present = true;
            p.enabled = (a.Attributes & SE_PRIVILEGE_ENABLED) != 0;
        }
    }
    return p;
}

bool setPrivilege(HANDLE token, const LUID &luid, bool enable)
{
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
    // AdjustTokenPrivileges "succeeds" without assigning when the token
    // lacks the privilege: ERROR_NOT_ALL_ASSIGNED.
    return AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
}

// Large pages for `bytes` (rounded up to `largePage`), with the privilege
// enabled only for the call and restored afterwards. Null, with `reason`,
// when they can't be had.
void *allocateLarge(std::size_t bytes, std::size_t largePage, std::string &reason)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token)) {
        reason = "no SeLockMemoryPrivilege";
        return nullptr;
    }
    void *p = nullptr;
    const LockMemoryPrivilege priv = lockMemoryPrivilege(token);
    if (!priv.present) {
        reason = "no SeLockMemoryPrivilege";
    } else if (!priv.enabled && !setPrivilege(token, priv.luid, true)) {
        reason = "no SeLockMemoryPrivilege";
    } else {
        p = VirtualAlloc(nullptr, roundUp(bytes, largePage), MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                         PAGE_READWRITE);
        if (!p)
            reason = GetLastError() == ERROR_NO_SYSTEM_RESOURCES ? "large pages failed: memory fragmented"
                                                                 : "large pages failed";
        if (!priv.enabled)
            setPrivilege(token, priv.luid, false);  // back to its old state
    }
    CloseHandle(token);
    return p;
}

} // namespace

HugeBuffer HugeBuffer::allocate(std::size_t bytes)
{
    HugeBuffer b;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    b.m_basePageSize = std::max<std::size_t>(4096, info.dwPageSize);
    b.m_hugePageSize = GetLargePageMinimum();  // 0: no large-page support
    bytes = std::max<std::size_t>(bytes, 1);
    void *p = nullptr;
    if (b.m_hugePageSize) {
        p = allocateLarge(bytes, b.m_hugePageSize, b.m_smallReason);
        b.m_largePages = p != nullptr;
    } else {
        b.m_smallReason = "no large-page support";
    }
    if (!p) {
        p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            b.m_error = "could not reserve " + formatPageSize(roundUp(bytes, b.m_basePageSize)) + " of memory";
            return b;
        }
    }
    b.m_data = static_cast<std::byte *>(p);
    b.m_size = bytes;
    b.m_mapLength = b.m_largePages ? roundUp(bytes, b.m_hugePageSize) : roundUp(bytes, b.m_basePageSize);
    return b;
}

void HugeBuffer::release()
{
    if (m_data)
        VirtualFree(m_data, 0, MEM_RELEASE);
    m_data = nullptr;
    m_size = 0;
    m_mapLength = 0;
}

bool HugeBuffer::firstTouch(const std::function<bool()> &cancelled)
{
    for (std::size_t chunk = 0; chunk < m_mapLength; chunk += kChunk) {
        if (cancelled && cancelled())
            return false;
        const std::size_t end = std::min(m_mapLength, chunk + kChunk);
        for (std::size_t page = chunk; page < end; page += m_basePageSize)
            *reinterpret_cast<volatile std::byte *>(m_data + page) = std::byte{0};
    }
    return true;
}

PageReport HugeBuffer::settle(const std::function<bool()> &)
{
    PageReport r;
    if (m_largePages) {
        r.mode = PageReport::Mode::Huge;
        r.hugePercent = 100;
        r.text = formatPageSize(m_hugePageSize) + " large pages";
    } else {
        r.mode = PageReport::Mode::Small;
        r.hugePercent = 0;
        r.text = formatPageSize(m_basePageSize) + " pages (" + m_smallReason + ")";
    }
    r.notes = m_notes;
    return r;
}

} // namespace sb::ram
