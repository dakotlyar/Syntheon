// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which memory channel an SMBIOS type 17 device sits on, from its bank
// locator and locator, shared by System info's channel estimate and the RAM
// test's bus width (Phase 3, AP-5 L4), so the two always agree. Pure.
//
//   "P0 CHANNEL A"                  -> "P0 A"   (P1 CHANNEL A is another one)
//   "Controller0-ChannelA-DIMM0"    -> "CONTROLLER0 A"
//   "ChannelB-DIMM0", "Channel-B"   -> "B"
//   "DIMM_A1", "DIMMB2"             -> "A", "B"  (the letter of a DIMMxN slot)
//   "CPU1_DIMM_A1", "P2-DIMMA1"     -> "CPU1 A", "P2 A"
//   "DIMM_P1_A0" (EPYC)             -> "P1 A"
//   "J1_DIMMA1"                     -> "A" (a prefix is kept only when it
//                                      names a socket or a controller)
// The bank locator is read first, then the locator. Empty when neither says.

#include <QList>
#include <QPair>
#include <QString>

namespace sb {

QString memoryChannelKey(const QString &bankLocator, const QString &locator);

struct MemoryChannelCount {
    int channels = 0;
    // Every device named its channel; false: the count is a guess (the
    // device count, at most 2).
    bool fromLocators = false;
};
// The channels the devices (bank locator, locator) sit on: the distinct
// keys when every device has one, else min(devices, 2). 0 for none.
MemoryChannelCount countMemoryChannels(const QList<QPair<QString, QString>> &devices);

} // namespace sb
