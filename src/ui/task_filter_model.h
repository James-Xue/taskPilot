#pragma once
// TaskFilterModel.hpp — order-preserving filter over the queue
//
// Filtering is client-side because `get_queue` has no tag or text parameter.
// The proxy never sorts: the daemon's order is the only order the product has.
// Counts shown in the tag pane come from the unfiltered source model.

#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>

namespace taskpilot::ui
{

class TaskFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QStringList tagFilter READ tagFilter WRITE setTagFilter NOTIFY tagFilterChanged)

  public:
    explicit TaskFilterModel(QObject *parent = nullptr);

    [[nodiscard]] QString searchText() const { return m_searchText; }
    void setSearchText(const QString &text);

    [[nodiscard]] QStringList tagFilter() const { return m_tagFilter; }
    void setTagFilter(const QStringList &tags);

    [[nodiscard]] Q_INVOKABLE QVariantMap get(int row) const;

  signals:
    void searchTextChanged();
    void tagFilterChanged();

  protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                        const QModelIndex &sourceParent) const override;

  private:
    QString m_searchText;
    QStringList m_tagFilter;
};

} // namespace taskpilot::ui
