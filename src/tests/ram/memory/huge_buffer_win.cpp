// SPDX-License-Identifier: GPL-3.0-or-later
// HugeBuffer on Windows x64 / ARM64 (Phase 3 design §6.2): large pages only
// when the token already holds SeLockMemoryPrivilege (D10), never granted.
#include "tests/ram/memory/huge_buffer.h"

#include <algorithm>
#include <mutex>
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

// The privilege is the process token's, shared by every thread: the
// workers allocating their slices at once (§4.2) must not see each other's
// restore. One enable for all concurrent users, counted; the last one out
// restores the old state (D10).
std::mutex g_privilegeMutex;
int g_privilegeUsers = 0;
bool g_privilegeWasEnabled = false;
LUID g_privilegeLuid{};

// Takes a use of SeLockMemoryPrivilege, enabled. False (nothing taken) if
// the token doesn't hold it.
bool acquireLockMemoryPrivilege()
{
    std::lock_guard lock(g_privilegeMutex);
    if (g_privilegeUsers > 0) {
        ++g_privilegeUsers;
        return true;
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token))
        return false;
    const LockMemoryPrivilege priv = lockMemoryPrivilege(token);
    const bool ok = priv.present && (priv.enabled || setPrivilege(token, priv.luid, true));
    CloseHandle(token);
    if (!ok)
        return false;
    g_privilegeWasEnabled = priv.enabled;
    g_privilegeLuid = priv.luid;
    g_privilegeUsers = 1;
    return true;
}

void releaseLockMemoryPrivilege()
{
    std::lock_guard lock(g_privilegeMutex);
    if (--g_privilegeUsers > 0 || g_privilegeWasEnabled)
        return;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
        setPrivilege(token, g_privilegeLuid, false);  // back to its old state
        CloseHandle(token);
    }
}

// Large pages for `bytes` (rounded up to `largePage`), with the privilege
// enabled only while allocations run. Null, with `reason`, when they can't
// be had.
void *allocateLarge(std::size_t bytes, std::size_t largePage, std::string &reason)
{
    if (!acquireLockMemoryPrivilege()) {
        reason = "no SeLockMemoryPrivilege";
        return nullptr;
    }
    void *p = VirtualAlloc(nullptr, roundUp(bytes, largePage), MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                           PAGE_READWRITE);
    if (!p)
        reason = GetLastError() == ERROR_NO_SYSTEM_RESOURCES ? "large pages failed: memory fragmented"
                                                             : "large pages failed";
    releaseLockMemoryPrivilege();
    return p;
}

} // namespace

PageSupport pageSupport()
{
    PageSupport s;
    HANDLE token = nullptr;
    bool present = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        present = lockMemoryPrivilege(token).present;
        CloseHandle(token);
    }
    const SIZE_T large = GetLargePageMinimum();
    if (present && large > 0) {
        s.huge = true;
        s.text = formatPageSize(large) + " large pages available";
    } else {
        s.text = "Large pages not available (SeLockMemoryPrivilege not held)";
    }
    return s;
}

HugeBuffer HugeBuffer::allocate(std::size_t bytes)
{
    HugeBuffer b;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    b.m_basePageSize = std::max<std::size_t>(4096, info.dwPageSize);
    b.m_hugePageSize = GetLargePageMinimum();  // 0: no large-page support
    const std::size_t want = std::max<std::size_t>(bytes, 1);
    void *p = nullptr;
    if (b.m_hugePageSize) {
        p = allocateLarge(want, b.m_hugePageSize, b.m_smallReason);
        b.m_largePages = p != nullptr;
    } else {
        b.m_smallReason = "no large-page support";
    }
    if (!p) {
        p = VirtualAlloc(nullptr, want, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            b.m_error = "could not reserve " + formatPageSize(roundUp(want, b.m_basePageSize)) + " of memory";
            b.m_basePageSize = 0;
            return b;
        }
    }
    b.m_data = static_cast<std::byte *>(p);
    b.m_size = bytes;
    b.m_length = b.m_largePages ? roundUp(want, b.m_hugePageSize) : roundUp(want, b.m_basePageSize);
    return b;
}

void HugeBuffer::release()
{
    if (m_data)
        VirtualFree(m_data, 0, MEM_RELEASE);
    m_data = nullptr;
    m_size = 0;
    m_length = 0;
    m_basePageSize = 0;
}

bool HugeBuffer::firstTouch(const std::function<bool()> &cancelled)
{
    if (!m_data)
        return false;
    for (std::size_t chunk = 0; chunk < m_length; chunk += kChunk) {
        if (cancelled && cancelled())
            return false;
        const std::size_t end = std::min(m_length, chunk + kChunk);
        for (std::size_t page = chunk; page < end; page += m_basePageSize)
            *reinterpret_cast<volatile std::byte *>(m_data + page) = std::byte{0};
    }
    return true;
}

PageReport HugeBuffer::settle(const std::function<bool()> &)
{
    PageReport r;
    if (!m_data) {
        r.text = "no buffer";
        return r;
    }
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
