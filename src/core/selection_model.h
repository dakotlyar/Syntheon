#pragma once

#include "core/test_info.h"

#include <QAbstractListModel>
#include <QStringList>
#include <QVector>

namespace sb {

class Registry;

// The test picker's state. The exclusivity rule (§2.6) lives here, not in QML,
// so it's unit-testable:
//  - ticking a RunsAlone test clears all others, and ticking any other test
//    clears it;
//  - Select all skips RunsAlone and Unavailable tests;
//  - an Unavailable test can't be ticked (and is unticked if it becomes
//    unavailable).
// The orchestrator re-checks the rule on the plan as defence in depth.
class SelectionModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(bool runsAloneSelected READ runsAloneSelected NOTIFY selectionChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        SummaryRole,
        ColorTokenRole,
        SelectedRole,
        AvailableRole,  // false = Unavailable (N/A)
        UnavailableReasonRole,
        RunsAloneRole,
    };
    Q_ENUM(Role)

    explicit SelectionModel(const Registry &registry, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Returns false if the change was refused (unknown or unavailable test).
    Q_INVOKABLE bool setSelected(const QString &id, bool selected);
    Q_INVOKABLE bool toggle(const QString &id);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clear();
    Q_INVOKABLE bool isSelected(const QString &id) const;

    void setAvailability(const QString &id, const Availability &availability);

    // Selected test ids in canonical order (P19), whatever order they were ticked.
    QStringList selectedIds() const;
    int selectedCount() const;
    bool runsAloneSelected() const;

signals:
    void selectionChanged();

private:
    struct Row {
        TestInfo info;
        Availability availability;
        bool selected = false;
    };

    int rowOf(const QString &id) const;
    bool runsAlone(const Row &r) const { return r.info.exclusivity == Exclusivity::RunsAlone; }
    bool selectable(const Row &r) const { return r.availability.state != Availability::State::Unavailable; }
    // Applies `wanted` (one bool per row); emits change signals only if something changed.
    void apply(const QVector<bool> &wanted);

    QVector<Row> m_rows;
};

} // namespace sb
