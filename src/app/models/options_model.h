// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/options.h"

#include <QAbstractListModel>

namespace sb {

// The rows of one test's option tab, built from its OptionSchema (P7): one
// generic renderer in QML draws every kind. Values live in an OptionValues
// map owned by the RunController; this model reads and writes it.
class OptionsModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        LabelRole,
        KindRole,        // "bool" | "multi" | "single" | "dynamic" | "number" | "path" | "info"
        HelpRole,
        ValueRole,       // bool | QStringList | QString | double | QString
        ItemsRole,       // [{value, label, enabled, disabledReason, detail}]
        SegmentedRole,   // SingleSelect presentation
        MinimumRole,
        MaximumRole,
        StepRole,
        UnitRole,
        InfoValueRole,
        InfoToneRole,
    };

    OptionsModel(OptionSchema schema, OptionValues *values, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    const OptionSchema &schema() const { return m_schema; }
    // Replaces the schema (a refreshed drive list, a new availability) and
    // keeps the user's values that are still valid.
    void setSchema(OptionSchema schema);

    Q_INVOKABLE void setValue(const QString &key, const QVariant &value);
    Q_INVOKABLE QVariant value(const QString &key) const;
    // Re-emits every value, after the controller changed the map directly.
    void valuesChanged();

signals:
    void valueEdited(const QString &key);

private:
    static QString kindName(OptionKind k);
    QVariant displayValue(const OptionDef &def) const;

    OptionSchema m_schema;
    OptionValues *m_values;
};

} // namespace sb
