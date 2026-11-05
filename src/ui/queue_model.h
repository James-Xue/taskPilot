#pragma once
// QueueModel.hpp — the ranked queue as a Qt list model
//
// The daemon is the only ranker: rows are stored and exposed in the exact order
// the `get_queue` reply arrived in. There is deliberately no sort API and no
// comparator here, and the four `score_parts` are exposed for explanation only;
// `score` is the delivered total and is never recomputed.

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace taskpilot::ui
{

struct QueueRow
{
    qint64 id{ 0 };
    QString uid;
    QString title;
    QString notes;
    QString status;
    int importance{ 3 };
    int blocks{ 0 };
    QStringList tags;
    QString dueText;
    bool hasDue{ false };
    double score{ 0.0 };
    double scoreImportance{ 0.0 };
    double scoreUrgency{ 0.0 };
    double scoreAge{ 0.0 };
    double scoreBlocks{ 0.0 };
    double urgencyFactor{ 0.0 };
    double ageDays{ 0.0 };
};

/// Parse the reply of `get_queue` (`{now, weights, queue:[...]}`).
///
/// Free function so the mapping can be tested without a daemon. Returns false
/// and fills `error` when the reply is not shaped like a queue.
[[nodiscard]] bool parseQueueReply(const QByteArray &payload, QVector<QueueRow> *rows,
                                  QString *error);

class QueueModel : public QAbstractListModel
{
    Q_OBJECT

  public:
    enum Role
    {
        TaskIdRole = Qt::UserRole + 1,
        UidRole,
        TitleRole,
        NotesRole,
        StatusRole,
        ImportanceRole,
        BlocksRole,
        TagsRole,
        DueTextRole,
        HasDueRole,
        ScoreRole,
        ScoreImportanceRole,
        ScoreUrgencyRole,
        ScoreAgeRole,
        ScoreBlocksRole,
        UrgencyFactorRole,
        AgeDaysRole,
    };

    explicit QueueModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Detail-pane view of one row. Keys are the role names plus `id`.
    [[nodiscard]] Q_INVOKABLE QVariantMap get(int row) const;

    /// Replace the rows with a `get_queue` reply. On failure the previous rows
    /// are kept and `*error` is filled.
    bool applyQueueReply(const QByteArray &payload, QString *error);

    [[nodiscard]] const QVector<QueueRow> &rows() const { return m_rows; }

  private:
    QVector<QueueRow> m_rows;
};

} // namespace taskpilot::ui
