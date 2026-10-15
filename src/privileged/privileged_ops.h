// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace sb {

// The root-only operations SynBench may perform (§6). Every op is a fixed,
// reviewed function; a new one (Phase 3 READ_SPD, Phase 8 backlight) is a
// deliberate change here, in the protocol and in the helper.
enum class PrivOp {
    ReadSmbios,
};

struct PrivError {
    enum class Code {
        Unavailable,   // Linux: the helper isn't running (never a hang)
        NotPermitted,  // the OS refused (EACCES/EPERM)
        IoError,
        Protocol,      // the helper sent something unexpected
        Unsupported,   // not on this platform
    };
    Code code = Code::Unavailable;
    QString message;
};

struct SmbiosTables {
    QByteArray entryPoint;  // Linux: raw smbios_entry_point ("_SM3_"/"_SM_")
    int major = 0, minor = 0;  // Windows: from the RawSMBIOSData header
    QByteArray table;          // the structure table
};

// Linux: LinuxHelperClient talks to synbench-helper (root, via pkexec).
// Windows: WindowsInProcessOps calls the APIs directly (the app is elevated).
// Calls block and are safe from any thread. Results are raw bytes; parsing
// happens in the unprivileged code (sysinfo), never in root.
class PrivilegedOps
{
public:
    virtual ~PrivilegedOps() = default;
    virtual bool isAvailable() const = 0;
    virtual std::optional<SmbiosTables> readSmbios(PrivError *error) = 0;
};

// Used where no privileged backend exists (tests, headless tools): every call
// fails with Unavailable.
class UnavailablePrivilegedOps : public PrivilegedOps
{
public:
    bool isAvailable() const override;
    std::optional<SmbiosTables> readSmbios(PrivError *error) override;
};

} // namespace sb
