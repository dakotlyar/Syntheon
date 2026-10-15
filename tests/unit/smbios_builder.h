// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QtEndian>

// Builds synthetic SMBIOS tables for tests (real ones are root-only and full
// of serial numbers).
class SmbiosBuilder
{
public:
    // A structure: formatted area of `length` bytes (header included), with
    // fields set via the returned ref, plus its strings.
    struct Structure {
        QByteArray formatted;
        QList<QByteArray> strings;
        Structure &byte(int off, int v) { formatted[off] = char(v); return *this; }
        Structure &word(int off, int v) { qToLittleEndian<quint16>(quint16(v), formatted.data() + off); return *this; }
        Structure &dword(int off, quint32 v) { qToLittleEndian<quint32>(v, formatted.data() + off); return *this; }
        // Adds a string and points field `off` at it.
        Structure &str(int off, const QByteArray &s) { strings.append(s); return byte(off, int(strings.size())); }
    };

    Structure &add(int type, int length)
    {
        Structure s;
        s.formatted = QByteArray(length, '\0');
        s.formatted[0] = char(type);
        s.formatted[1] = char(length);
        qToLittleEndian<quint16>(quint16(m_structs.size()), s.formatted.data() + 2);
        m_structs.append(s);
        return m_structs.last();
    }

    QByteArray table() const
    {
        QByteArray out;
        for (const Structure &s : m_structs) {
            out += s.formatted;
            if (s.strings.isEmpty()) {
                out += QByteArray(2, '\0');
            } else {
                for (const QByteArray &str : s.strings)
                    out += str + '\0';
                out += '\0';
            }
        }
        return out;
    }

    // A full table with BIOS, system and two memory devices plus an empty slot.
    static QByteArray sample()
    {
        SmbiosBuilder b;
        b.add(0, 0x18).str(0x04, "American Megatrends International, LLC.").str(0x05, "FA401EA.304").str(0x08, "01/28/2026");
        b.add(1, 0x1B)
            .str(0x04, "ASUSTeK COMPUTER INC.")
            .str(0x05, "TUF Gaming A14 FA401EA")
            .str(0x06, "1.0")
            .str(0x07, "SERIAL-SHOULD-NOT-APPEAR")
            .str(0x1A, "TUF Gaming A14");
        for (int i = 0; i < 2; ++i) {
            b.add(17, 0x5C)
                .word(0x0C, 0x7FFF)
                .dword(0x1C, 32768)  // 32 GiB via extended size
                .str(0x10, i == 0 ? "DIMM 0" : "DIMM 1")
                .str(0x11, i == 0 ? "P0 CHANNEL A" : "P0 CHANNEL B")
                .byte(0x12, 0x23)    // LPDDR5
                .word(0x15, 8000)
                .str(0x17, "Samsung")
                .str(0x18, "SERIAL-SHOULD-NOT-APPEAR")
                .str(0x1A, "K3KL3L30EM-BGCV ")
                .word(0x20, 8000);
        }
        b.add(17, 0x28).word(0x0C, 0);  // empty slot
        b.add(127, 4);
        return b.table();
    }

private:
    QList<Structure> m_structs;
};
