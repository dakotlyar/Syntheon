// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/itest.h"

#include <QStringList>

namespace sb {

void SubTestHandle::begin()
{
    m_ctx.subTestBegin(m_id);
}

void SubTestHandle::end(Status status)
{
    m_ctx.subTestEnd(m_id, status);
}

OptionSchema effectiveOptionSchema(const ITest &test, const ProbeContext &ctx, const Availability &availability)
{
    OptionSchema schema = test.options(ctx, availability);

    OptionDef subTests;
    subTests.key = kSubTestsOptionKey;
    subTests.label = QStringLiteral("Sub-tests");
    subTests.kind = OptionKind::MultiSelect;
    subTests.minSelected = 1;
    QStringList defaults;
    for (const SubTestInfo &s : test.info().subTests) {
        if (!s.userSelectable)
            continue;
        OptionItem item{s.id, s.name};
        const auto reason = availability.subTestUnavailable.constFind(s.id);
        if (reason != availability.subTestUnavailable.cend()) {
            item.enabled = false;
            item.disabledReason = *reason;
        } else if (s.defaultOn) {
            defaults.append(s.id);
        }
        subTests.items.append(item);
    }
    if (subTests.items.isEmpty())
        return schema;

    subTests.defaultValue = defaults;
    schema.options.prepend(subTests);
    return schema;
}

} // namespace sb
