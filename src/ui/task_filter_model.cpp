// TaskFilterModel.cpp — OR-tag and text filtering without reordering

#include "ui/task_filter_model.h"

#include "ui/queue_model.h"

#include <QVariantMap>

namespace taskpilot::ui
{

TaskFilterModel::TaskFilterModel(QObject *parent) : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
}

void TaskFilterModel::setSearchText(const QString &text)
{
    if (m_searchText == text)
    {
        return;
    }
    beginFilterChange();
    m_searchText = text;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    emit searchTextChanged();
}

void TaskFilterModel::setTagFilter(const QStringList &tags)
{
    if (m_tagFilter == tags)
    {
        return;
    }
    beginFilterChange();
    m_tagFilter = tags;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    emit tagFilterChanged();
}

QVariantMap TaskFilterModel::get(int row) const
{
    const QModelIndex proxyIndex = index(row, 0);
    if (!proxyIndex.isValid())
    {
        return {};
    }
    const QModelIndex sourceIndex = mapToSource(proxyIndex);
    const auto *model = qobject_cast<const QueueModel *>(sourceModel());
    if (model == nullptr)
    {
        return {};
    }
    return model->get(sourceIndex.row());
}

bool TaskFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QAbstractItemModel *model = sourceModel();
    if (model == nullptr)
    {
        return false;
    }

    const QModelIndex index = model->index(sourceRow, 0, sourceParent);
    if (!index.isValid())
    {
        return false;
    }

    if (!m_searchText.trimmed().isEmpty())
    {
        const QString needle = m_searchText.trimmed();
        const QString title = index.data(QueueModel::TitleRole).toString();
        const QString notes = index.data(QueueModel::NotesRole).toString();
        if (!title.contains(needle, Qt::CaseInsensitive) &&
            !notes.contains(needle, Qt::CaseInsensitive))
        {
            return false;
        }
    }

    if (!m_tagFilter.isEmpty())
    {
        const QStringList tags = index.data(QueueModel::TagsRole).toStringList();
        bool matched = false;
        for (const QString &wanted : m_tagFilter)
        {
            if (tags.contains(wanted))
            {
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            return false;
        }
    }

    return true;
}

} // namespace taskpilot::ui
