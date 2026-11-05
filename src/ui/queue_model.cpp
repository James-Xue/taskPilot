// QueueModel.cpp — parsing and role mapping for the ranked queue

#include "ui/queue_model.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <utility>

namespace taskpilot::ui
{
namespace
{

[[nodiscard]] QString shapeError(const QString &detail)
{
    return QStringLiteral("unexpected get_queue reply: ") + detail;
}

[[nodiscard]] double numberOr(const QJsonObject &object, const QString &key, double fallback)
{
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toDouble() : fallback;
}

} // namespace

bool parseQueueReply(const QByteArray &payload, QVector<QueueRow> *rows, QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (document.isNull())
    {
        *error = shapeError(QStringLiteral("not valid JSON (%1)").arg(parseError.errorString()));
        return false;
    }
    if (!document.isObject())
    {
        *error = shapeError(QStringLiteral("not a JSON object"));
        return false;
    }

    const QJsonValue queueValue = document.object().value(QStringLiteral("queue"));
    if (!queueValue.isArray())
    {
        *error = shapeError(QStringLiteral("`queue` is not an array"));
        return false;
    }

    const QJsonArray queue = queueValue.toArray();
    QVector<QueueRow> parsed;
    parsed.reserve(queue.size());
    for (const QJsonValue &value : queue)
    {
        if (!value.isObject())
        {
            *error = shapeError(QStringLiteral("a queue entry is not an object"));
            return false;
        }
        const QJsonObject object = value.toObject();

        QueueRow row;
        row.id = static_cast<qint64>(object.value(QStringLiteral("id")).toDouble());
        row.uid = object.value(QStringLiteral("uid")).toString();
        row.title = object.value(QStringLiteral("title")).toString();
        row.notes = object.value(QStringLiteral("notes")).toString();
        row.status = object.value(QStringLiteral("status")).toString();
        row.importance = object.value(QStringLiteral("importance")).toInt(3);
        row.blocks = object.value(QStringLiteral("blocks")).toInt(0);

        const QJsonArray tags = object.value(QStringLiteral("tags")).toArray();
        for (const QJsonValue &tag : tags)
        {
            row.tags.append(tag.toString());
        }

        const QJsonValue due = object.value(QStringLiteral("due_at"));
        if (due.isDouble())
        {
            const qint64 seconds = static_cast<qint64>(due.toDouble());
            row.hasDue = true;
            row.dueText = QDateTime::fromSecsSinceEpoch(seconds).date().toString(Qt::ISODate);
        }

        row.score = numberOr(object, QStringLiteral("score"), 0.0);
        const QJsonObject parts = object.value(QStringLiteral("score_parts")).toObject();
        row.scoreImportance = numberOr(parts, QStringLiteral("importance"), 0.0);
        row.scoreUrgency = numberOr(parts, QStringLiteral("urgency"), 0.0);
        row.scoreAge = numberOr(parts, QStringLiteral("age"), 0.0);
        row.scoreBlocks = numberOr(parts, QStringLiteral("blocks"), 0.0);
        row.urgencyFactor = numberOr(object, QStringLiteral("urgency_factor"), 0.0);
        row.ageDays = numberOr(object, QStringLiteral("age_days"), 0.0);

        parsed.push_back(std::move(row));
    }

    *rows = std::move(parsed);
    return true;
}

QueueModel::QueueModel(QObject *parent) : QAbstractListModel(parent)
{
}

int QueueModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant QueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
    {
        return {};
    }

    const QueueRow &row = m_rows.at(index.row());
    switch (role)
    {
    case TaskIdRole:
        return row.id;
    case UidRole:
        return row.uid;
    case TitleRole:
        return row.title;
    case NotesRole:
        return row.notes;
    case StatusRole:
        return row.status;
    case ImportanceRole:
        return row.importance;
    case BlocksRole:
        return row.blocks;
    case TagsRole:
        return row.tags;
    case DueTextRole:
        return row.dueText;
    case HasDueRole:
        return row.hasDue;
    case ScoreRole:
        return row.score;
    case ScoreImportanceRole:
        return row.scoreImportance;
    case ScoreUrgencyRole:
        return row.scoreUrgency;
    case ScoreAgeRole:
        return row.scoreAge;
    case ScoreBlocksRole:
        return row.scoreBlocks;
    case UrgencyFactorRole:
        return row.urgencyFactor;
    case AgeDaysRole:
        return row.ageDays;
    default:
        return {};
    }
}

QHash<int, QByteArray> QueueModel::roleNames() const
{
    return {
        { TaskIdRole, QByteArrayLiteral("taskId") },
        { UidRole, QByteArrayLiteral("uid") },
        { TitleRole, QByteArrayLiteral("title") },
        { NotesRole, QByteArrayLiteral("notes") },
        { StatusRole, QByteArrayLiteral("status") },
        { ImportanceRole, QByteArrayLiteral("importance") },
        { BlocksRole, QByteArrayLiteral("blocks") },
        { TagsRole, QByteArrayLiteral("tags") },
        { DueTextRole, QByteArrayLiteral("dueText") },
        { HasDueRole, QByteArrayLiteral("hasDue") },
        { ScoreRole, QByteArrayLiteral("score") },
        { ScoreImportanceRole, QByteArrayLiteral("scoreImportance") },
        { ScoreUrgencyRole, QByteArrayLiteral("scoreUrgency") },
        { ScoreAgeRole, QByteArrayLiteral("scoreAge") },
        { ScoreBlocksRole, QByteArrayLiteral("scoreBlocks") },
        { UrgencyFactorRole, QByteArrayLiteral("urgencyFactor") },
        { AgeDaysRole, QByteArrayLiteral("ageDays") },
    };
}

QVariantMap QueueModel::get(int row) const
{
    if (row < 0 || row >= m_rows.size())
    {
        return {};
    }

    const QueueRow &value = m_rows.at(row);
    return {
        { QStringLiteral("id"), value.id },
        { QStringLiteral("taskId"), value.id },
        { QStringLiteral("uid"), value.uid },
        { QStringLiteral("title"), value.title },
        { QStringLiteral("notes"), value.notes },
        { QStringLiteral("status"), value.status },
        { QStringLiteral("importance"), value.importance },
        { QStringLiteral("blocks"), value.blocks },
        { QStringLiteral("tags"), value.tags },
        { QStringLiteral("dueText"), value.dueText },
        { QStringLiteral("hasDue"), value.hasDue },
        { QStringLiteral("score"), value.score },
        { QStringLiteral("scoreImportance"), value.scoreImportance },
        { QStringLiteral("scoreUrgency"), value.scoreUrgency },
        { QStringLiteral("scoreAge"), value.scoreAge },
        { QStringLiteral("scoreBlocks"), value.scoreBlocks },
        { QStringLiteral("urgencyFactor"), value.urgencyFactor },
        { QStringLiteral("ageDays"), value.ageDays },
    };
}

bool QueueModel::applyQueueReply(const QByteArray &payload, QString *error)
{
    QVector<QueueRow> parsed;
    if (!parseQueueReply(payload, &parsed, error))
    {
        return false;
    }

    beginResetModel();
    m_rows = std::move(parsed);
    endResetModel();
    return true;
}

} // namespace taskpilot::ui
