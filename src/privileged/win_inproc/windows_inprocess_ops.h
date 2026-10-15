// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "privileged/privileged_ops.h"

namespace sb {

// Windows: the app runs elevated (requireAdministrator, P12), so privileged
// ops are direct calls.
class WindowsInProcessOps : public PrivilegedOps
{
public:
    bool isAvailable() const override { return true; }
    std::optional<SmbiosTables> readSmbios(PrivError *error) override;
};

} // namespace sb
