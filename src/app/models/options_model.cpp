// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/options_model.h"

#include <QVariantList>
#include <QVariantMap>

namespace sb {

OptionsModel::OptionsModel(OptionSchema schema, OptionValues *values, QObject *parent)
    : QAbstractListModel(parent), m_schema(std::move(schema)), m_values(values)
{
}

QString OptionsModel::kindName(OptionKind k)
{
    switch (k) {
    case OptionKind::Bool: return QStringLiteral("bool");
    case OptionKind::MultiSelect: return QStringLiteral("multi");
    case OptionKind::SingleSelect: return QStringLiteral("single");
    case OptionKind::DynamicSelect: return QStringLiteral("dynamic");
    case OptionKind::Number: return QStringLiteral("number");
    case OptionKind::Path: return QStringLiteral("path");
    case OptionKind::Info: return QStringLiteral("info");
    }
    return {};
}

int OptionsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_schema.options.size());
}

QVariant OptionsModel::displayValue(const OptionDef &def) const
{
    const QVariant v = m_values->value(def.key, def.defaultValue);
    if (def.kind == OptionKind::MultiSelect) {
        // Always a QStringList in QML, whatever shape a loaded value had.
        QStringList list;
        if (v.typeId() == QMetaType::QStringList) {
            list = v.toStringList();
        } else {
            for (const QVariant &e : v.toList())
                list.append(e.toString());
        }
        return list;
    }
    if (def.kind == OptionKind::Bool)
        return v.toBool();
    if (def.kind == OptionKind::Number)
        return v.toDouble();
    return v.isValid() ? v : QVariant(QString());
}

QVariant OptionsModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const OptionDef &d = m_schema.options[index.row()];
    switch (role) {
    case KeyRole: return d.key;
    case LabelRole: return d.label;
    case KindRole: return kindName(d.kind);
    case HelpRole: return d.help;
    case ValueRole: return displayValue(d);
    case ItemsRole: {
        QVariantList items;
        for (const OptionItem &i : d.items) {
            items.append(QVariantMap{{QStringLiteral("value"), i.value},
                                     {QStringLiteral("label"), i.label},
                                     {QStringLiteral("enabled"), i.enabled},
                                     {QStringLiteral("disabledReason"), i.disabledReason},
                                     {QStringLiteral("detail"), i.detail}});
        }
        return items;
    }
    case SegmentedRole: return d.presentation == SelectPresentation::Segmented;
    case MinimumRole: return d.minimum;
    case MaximumRole: return d.maximum;
    case StepRole: return d.step;
    case UnitRole: return d.unit;
    case InfoValueRole: {
        if (!d.infoFollows.isEmpty()) {
            const auto it = d.infoByValue.constFind(m_values->value(d.infoFollows).toString());
            if (it != d.infoByValue.cend())
                return *it;
        }
        return d.infoValue;
    }
    case InfoToneRole: return toKey(d.infoTone);
    default: return {};
    }
}

QHash<int, QByteArray> OptionsModel::roleNames() const
{
    return {
        {KeyRole, "key"},         {LabelRole, "label"},       {KindRole, "kind"},
        {HelpRole, "help"},       {ValueRole, "value"},       {ItemsRole, "items"},
        {SegmentedRole, "segmented"}, {MinimumRole, "minimum"}, {MaximumRole, "maximum"},
        {StepRole, "step"},       {UnitRole, "unit"},         {InfoValueRole, "infoValue"},
        {InfoToneRole, "infoTone"},
    };
}

void OptionsModel::setSchema(OptionSchema schema)
{
    beginResetModel();
    m_schema = std::move(schema);
    // Keep the user's values that the new schema still accepts.
    for (auto it = m_values->begin(); it != m_values->end();) {
        const OptionDef *def = m_schema.find(it.key());
        if (!def)
            it = m_values->erase(it);
        else
            ++it;
    }
    for (const OptionDef &d : std::as_const(m_schema.options)) {
        if (d.kind == OptionKind::Info)
            continue;
        if (!m_values->contains(d.key) && d.defaultValue.isValid())
            m_values->insert(d.key, d.defaultValue);
        else if (d.kind == OptionKind::SingleSelect || d.kind == OptionKind::DynamicSelect) {
            const OptionItem *item = d.item(m_values->value(d.key).toString());
            if (!item || !item->enabled)
                m_values->insert(d.key, d.defaultValue);  // e.g. a drive that went away
        } else if (d.kind == OptionKind::MultiSelect) {
            // Likewise a ticked entry that has gone away or is now disabled
            // (a runtime that was uninstalled): drop it from the value.
            const QVariant v = m_values->value(d.key);
            QStringList chosen = v.typeId() == QMetaType::QStringList ? v.toStringList() : QStringList();
            if (v.typeId() == QMetaType::QVariantList) {
                for (const QVariant &e : v.toList())
                    chosen.append(e.toString());
            }
            QStringList kept;
            for (const QString &c : std::as_const(chosen)) {
                const OptionItem *item = d.item(c);
                if (item && item->enabled)
                    kept.append(c);
            }
            if (kept != chosen)
                m_values->insert(d.key, kept);
        }
    }
    endResetModel();
}

void OptionsModel::setValue(const QString &key, const QVariant &value)
{
    const OptionDef *def = m_schema.find(key);
    if (!def || def->kind == OptionKind::Info)
        return;
    QVariant v = value;
    if (def->kind == OptionKind::MultiSelect) {
        // QML hands over a QVariantList; keep declaration order of the items.
        QStringList chosen;
        for (const QVariant &e : value.toList())
            chosen.append(e.toString());
        if (value.typeId() == QMetaType::QStringList)
            chosen = value.toStringList();
        QStringList ordered;
        for (const OptionItem &i : def->items) {
            if (chosen.contains(i.value))
                ordered.append(i.value);
        }
        v = ordered;
    } else if (def->kind == OptionKind::Number) {
        v = qBound(def->minimum, value.toDouble(), def->maximum);
    } else if (def->kind == OptionKind::Bool) {
        v = value.toBool();
    } else {
        v = value.toString();
    }
    if (m_values->value(key) == v)
        return;
    m_values->insert(key, v);
    for (int i = 0; i < m_schema.options.size(); ++i) {
        if (m_schema.options[i].key == key) {
            emit dataChanged(index(i), index(i), {ValueRole});
            break;
        }
    }
    // The Info rows that follow this option show the new selection's text.
    for (int i = 0; i < m_schema.options.size(); ++i) {
        if (m_schema.options[i].infoFollows == key)
            emit dataChanged(index(i), index(i), {InfoValueRole});
    }
    emit valueEdited(key);
}

QVariant OptionsModel::value(const QString &key) const
{
    const OptionDef *def = m_schema.find(key);
    return def ? displayValue(*def) : QVariant();
}

void OptionsModel::valuesChanged()
{
    if (!m_schema.options.isEmpty())
        emit dataChanged(index(0), index(int(m_schema.options.size()) - 1), {ValueRole, InfoValueRole});
}

} // namespace sb
