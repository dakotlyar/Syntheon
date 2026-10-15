// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/options.h"

#include <QStringList>

namespace sb {

const OptionItem *OptionDef::item(const QString &value) const
{
    for (const OptionItem &i : items) {
        if (i.value == value)
            return &i;
    }
    return nullptr;
}

const OptionDef *OptionSchema::find(const QString &key) const
{
    for (const OptionDef &o : options) {
        if (o.key == key)
            return &o;
    }
    return nullptr;
}

OptionValues OptionSchema::defaults() const
{
    OptionValues v;
    for (const OptionDef &o : options) {
        if (o.kind != OptionKind::Info && o.defaultValue.isValid())
            v.insert(o.key, o.defaultValue);
    }
    return v;
}

OptionValues OptionSchema::withDefaults(const OptionValues &values) const
{
    OptionValues v = defaults();
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        v.insert(it.key(), it.value());
    return v;
}

namespace {

// Values may arrive as QStringList (from C++) or as a QVariantList of strings
// (after a JSON round trip).
bool toStringList(const QVariant &v, QStringList *out)
{
    if (v.typeId() == QMetaType::QStringList) {
        *out = v.toStringList();
        return true;
    }
    if (v.typeId() != QMetaType::QVariantList)
        return false;
    out->clear();
    for (const QVariant &e : v.toList()) {
        if (e.typeId() != QMetaType::QString)
            return false;
        out->append(e.toString());
    }
    return true;
}

bool isNumber(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::Float:
        return true;
    default:
        return false;
    }
}

void checkItem(const OptionDef &def, const QString &value, Validation &out)
{
    const OptionItem *item = def.item(value);
    if (!item) {
        out.add(def.key, QStringLiteral("%1: \"%2\" is not a valid choice").arg(def.label, value));
        return;
    }
    if (!item->enabled) {
        out.add(def.key, item->disabledReason.isEmpty()
                             ? QStringLiteral("%1: %2 is unavailable").arg(def.label, item->label)
                             : QStringLiteral("%1: %2 is unavailable (%3)")
                                   .arg(def.label, item->label, item->disabledReason));
    }
}

} // namespace

Validation validateOptions(const OptionSchema &schema, const OptionValues &values)
{
    Validation out;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const OptionDef *def = schema.find(it.key());
        if (!def) {
            out.add(it.key(), QStringLiteral("Unknown option \"%1\"").arg(it.key()));
            continue;
        }
        const QVariant &v = it.value();
        switch (def->kind) {
        case OptionKind::Bool:
            if (v.typeId() != QMetaType::Bool)
                out.add(def->key, QStringLiteral("%1 must be on or off").arg(def->label));
            break;
        case OptionKind::MultiSelect: {
            QStringList list;
            if (!toStringList(v, &list)) {
                out.add(def->key, QStringLiteral("%1 must be a list of choices").arg(def->label));
                break;
            }
            for (const QString &value : list)
                checkItem(*def, value, out);
            if (list.size() < def->minSelected)
                out.add(def->key, QStringLiteral("%1: select at least %2")
                                      .arg(def->label)
                                      .arg(def->minSelected));
            break;
        }
        case OptionKind::SingleSelect:
        case OptionKind::DynamicSelect:
            if (v.typeId() != QMetaType::QString)
                out.add(def->key, QStringLiteral("%1 must be one choice").arg(def->label));
            else
                checkItem(*def, v.toString(), out);
            break;
        case OptionKind::Number: {
            if (!isNumber(v)) {
                out.add(def->key, QStringLiteral("%1 must be a number").arg(def->label));
                break;
            }
            const double d = v.toDouble();
            if (d < def->minimum || d > def->maximum)
                out.add(def->key, QStringLiteral("%1 must be between %2 and %3")
                                      .arg(def->label)
                                      .arg(def->minimum)
                                      .arg(def->maximum));
            break;
        }
        case OptionKind::Path:
            if (v.typeId() != QMetaType::QString)
                out.add(def->key, QStringLiteral("%1 must be a path").arg(def->label));
            break;
        case OptionKind::Info:
            out.add(def->key, QStringLiteral("%1 is read-only").arg(def->label));
            break;
        }
    }
    return out;
}

} // namespace sb
