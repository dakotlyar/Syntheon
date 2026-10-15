#include "core/selection_model.h"

#include "core/registry.h"

namespace sb {

SelectionModel::SelectionModel(const Registry &registry, QObject *parent) : QAbstractListModel(parent)
{
    for (const auto &t : registry.tests())
        m_rows.append({t->info(), Availability::available(), false});
}

int SelectionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant SelectionModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const Row &r = m_rows[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return r.info.name;
    case IdRole:
        return r.info.id;
    case SummaryRole:
        return r.info.summary;
    case ColorTokenRole:
        return r.info.colorToken;
    case SelectedRole:
        return r.selected;
    case AvailableRole:
        return selectable(r);
    case UnavailableReasonRole:
        return selectable(r) ? QString() : r.availability.reason;
    case RunsAloneRole:
        return runsAlone(r);
    default:
        return {};
    }
}

QHash<int, QByteArray> SelectionModel::roleNames() const
{
    return {
        {IdRole, "testId"},
        {NameRole, "name"},
        {SummaryRole, "summary"},
        {ColorTokenRole, "colorToken"},
        {SelectedRole, "selected"},
        {AvailableRole, "available"},
        {UnavailableReasonRole, "unavailableReason"},
        {RunsAloneRole, "runsAlone"},
    };
}

int SelectionModel::rowOf(const QString &id) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].info.id == id)
            return i;
    }
    return -1;
}

void SelectionModel::apply(const QVector<bool> &wanted)
{
    bool changed = false;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].selected == wanted[i])
            continue;
        m_rows[i].selected = wanted[i];
        const QModelIndex idx = index(i);
        emit dataChanged(idx, idx, {SelectedRole});
        changed = true;
    }
    if (changed)
        emit selectionChanged();
}

bool SelectionModel::setSelected(const QString &id, bool selected)
{
    const int row = rowOf(id);
    if (row < 0)
        return false;

    QVector<bool> wanted;
    for (const Row &r : std::as_const(m_rows))
        wanted.append(r.selected);

    if (!selected) {
        wanted[row] = false;
        apply(wanted);
        return true;
    }

    const Row &target = m_rows[row];
    if (!selectable(target))
        return false;
    for (int i = 0; i < m_rows.size(); ++i) {
        // A RunsAlone test clears everything else; any other test clears the
        // RunsAlone ones.
        if (i != row && (runsAlone(target) || runsAlone(m_rows[i])))
            wanted[i] = false;
    }
    wanted[row] = true;
    apply(wanted);
    return true;
}

bool SelectionModel::toggle(const QString &id)
{
    return setSelected(id, !isSelected(id));
}

void SelectionModel::selectAll()
{
    QVector<bool> wanted;
    for (const Row &r : std::as_const(m_rows))
        wanted.append(selectable(r) && !runsAlone(r));
    apply(wanted);
}

void SelectionModel::clear()
{
    apply(QVector<bool>(m_rows.size(), false));
}

bool SelectionModel::isSelected(const QString &id) const
{
    const int row = rowOf(id);
    return row >= 0 && m_rows[row].selected;
}

void SelectionModel::setAvailability(const QString &id, const Availability &availability)
{
    const int row = rowOf(id);
    if (row < 0)
        return;
    m_rows[row].availability = availability;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {AvailableRole, UnavailableReasonRole});
    if (!selectable(m_rows[row]) && m_rows[row].selected)
        setSelected(id, false);
}

QStringList SelectionModel::selectedIds() const
{
    QStringList out;
    for (const Row &r : m_rows) {
        if (r.selected)
            out.append(r.info.id);
    }
    return out;
}

int SelectionModel::selectedCount() const
{
    return static_cast<int>(selectedIds().size());
}

bool SelectionModel::runsAloneSelected() const
{
    for (const Row &r : m_rows) {
        if (r.selected && runsAlone(r))
            return true;
    }
    return false;
}

} // namespace sb
