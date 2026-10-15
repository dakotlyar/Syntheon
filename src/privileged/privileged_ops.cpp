// SPDX-License-Identifier: GPL-3.0-or-later
#include "privileged/privileged_ops.h"

namespace sb {

bool UnavailablePrivilegedOps::isAvailable() const
{
    return false;
}

std::optional<SmbiosTables> UnavailablePrivilegedOps::readSmbios(PrivError *error)
{
    if (error)
        *error = {PrivError::Code::Unavailable, QStringLiteral("Privileged operations aren't available")};
    return std::nullopt;
}

} // namespace sb
