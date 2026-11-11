// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/storage/ids.h"

namespace sb::storage {

namespace {

QString patternIdString(Pattern p)
{
    switch (p) {
    case Pattern::Seq1mQ8T1:
        return QStringLiteral("seq1m_q8t1");
    case Pattern::Seq1mQ1T1:
        return QStringLiteral("seq1m_q1t1");
    case Pattern::Rnd4kQ32T1:
        return QStringLiteral("rnd4k_q32t1");
    case Pattern::Rnd4kQ1T1:
        return QStringLiteral("rnd4k_q1t1");
    case Pattern::Latency:
        return QStringLiteral("latency");
    case Pattern::Mixed:
        return QStringLiteral("mixed");
    }
    return {};
}

QString patternLabelString(Pattern p)
{
    switch (p) {
    case Pattern::Seq1mQ8T1:
        return QStringLiteral("Seq 1M Q8T1");
    case Pattern::Seq1mQ1T1:
        return QStringLiteral("Seq 1M Q1T1");
    case Pattern::Rnd4kQ32T1:
        return QStringLiteral("Random 4K Q32T1");
    case Pattern::Rnd4kQ1T1:
        return QStringLiteral("Random 4K Q1T1");
    case Pattern::Latency:
        return QStringLiteral("Latency (4K Q1T1)");
    case Pattern::Mixed:
        return QStringLiteral("Mixed 70/30 random");
    }
    return {};
}

QString fieldSuffix(Field f)
{
    switch (f) {
    case Field::Read:
        return QStringLiteral("read");
    case Field::Write:
        return QStringLiteral("write");
    case Field::ReadIops:
        return QStringLiteral("read_iops");
    case Field::WriteIops:
        return QStringLiteral("write_iops");
    case Field::Throughput:
        return QStringLiteral("throughput");
    case Field::Iops:
        return QStringLiteral("iops");
    case Field::P50:
        return QStringLiteral("p50");
    case Field::P99:
        return QStringLiteral("p99");
    case Field::P999:
        return QStringLiteral("p999");
    }
    return {};
}

QString fieldLabel(Field f)
{
    switch (f) {
    case Field::Read:
        return QStringLiteral("read");
    case Field::Write:
        return QStringLiteral("write");
    case Field::ReadIops:
        return QStringLiteral("read IOPS");
    case Field::WriteIops:
        return QStringLiteral("write IOPS");
    case Field::Throughput:
        return QStringLiteral("throughput");
    case Field::Iops:
        return QStringLiteral("IOPS");
    case Field::P50:
        return QStringLiteral("p50 latency");
    case Field::P99:
        return QStringLiteral("p99 latency");
    case Field::P999:
        return QStringLiteral("p99.9 latency");
    }
    return {};
}

} // namespace

QString patternId(Pattern p)
{
    return patternIdString(p);
}

QString patternLabel(Pattern p)
{
    return patternLabelString(p);
}

QString key(Pattern p, Field f)
{
    return patternIdString(p) + QLatin1Char('.') + fieldSuffix(f);
}

QString label(Pattern p, Field f)
{
    // §10's stored labels: the latency percentiles are "p50 latency" ("p99.9
    // latency"), and the mixed pair is "Mixed throughput" / "Mixed IOPS" —
    // neither repeats the pattern's own name.
    if (p == Pattern::Latency)
        return fieldLabel(f);
    if (p == Pattern::Mixed)
        return QStringLiteral("Mixed ") + fieldLabel(f);
    return patternLabelString(p) + QLatin1Char(' ') + fieldLabel(f);
}

QString unit(Pattern p, Field f)
{
    if (fieldIsIops(f))
        return QStringLiteral("IOPS");
    if (p == Pattern::Latency)
        return QStringLiteral("µs");
    return QStringLiteral("MB/s");
}

bool fieldIsIops(Field f)
{
    return f == Field::ReadIops || f == Field::WriteIops || f == Field::Iops;
}

bool lowerIsBetter(Pattern p, Field f)
{
    return p == Pattern::Latency && (f == Field::P50 || f == Field::P99 || f == Field::P999);
}

std::optional<Pattern> patternFromId(QStringView id)
{
    for (Pattern p : kPatterns)
        if (id == patternIdString(p))
            return p;
    return std::nullopt;
}

std::optional<Field> fieldFromKey(QStringView suffix)
{
    for (Field f : {Field::Read, Field::Write, Field::ReadIops, Field::WriteIops, Field::Throughput, Field::Iops,
                    Field::P50, Field::P99, Field::P999})
        if (suffix == fieldSuffix(f))
            return f;
    return std::nullopt;
}

} // namespace sb::storage