// SPDX-License-Identifier: GPL-3.0-or-later
#include "privileged/win_inproc/windows_inprocess_ops.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QtEndian>

namespace sb {

std::optional<SmbiosTables> WindowsInProcessOps::readSmbios(PrivError *error)
{
    const DWORD rsmb = (DWORD('R') << 24) | (DWORD('S') << 16) | (DWORD('M') << 8) | DWORD('B');
    const UINT size = GetSystemFirmwareTable(rsmb, 0, nullptr, 0);
    QByteArray raw(int(size), Qt::Uninitialized);
    if (size < 8 || GetSystemFirmwareTable(rsmb, 0, raw.data(), size) != size) {
        if (error)
            *error = {PrivError::Code::IoError,
                      QStringLiteral("GetSystemFirmwareTable failed (error %1)").arg(qulonglong(GetLastError()))};
        return std::nullopt;
    }
    // RawSMBIOSData: u8 method, u8 major, u8 minor, u8 dmi revision, u32 length, table.
    const auto *p = reinterpret_cast<const uchar *>(raw.constData());
    const quint32 length = qFromLittleEndian<quint32>(p + 4);
    if (length > quint32(raw.size() - 8)) {
        if (error)
            *error = {PrivError::Code::IoError, QStringLiteral("Firmware returned a truncated SMBIOS table")};
        return std::nullopt;
    }
    SmbiosTables t;
    t.major = p[1];
    t.minor = p[2];
    t.table = raw.mid(8, int(length));
    return t;
}

} // namespace sb
