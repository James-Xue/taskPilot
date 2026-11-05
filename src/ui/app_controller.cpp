// AppController.cpp — QML-facing state machine over the worker and the models

#include "ui/app_controller.h"

#include "ui/control_worker.h"
#include "ui/queue_model.h"
#include "ui/task_filter_model.h"

#include <QDate>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTime>
#include <QVariantMap>
#include <QtGlobal>

#include <nlohmann/json.hpp>

#include <utility>

namespace taskpilot::ui
{
namespace
{

[[nodiscard]] QByteArray encode(const nlohmann::json &value)
{
    return QByteArray::fromStdString(value.dump());
}

[[nodiscard]] QString formatWeight(double value)
{
    return QString::number(value, 'g', 8);
}

} // namespace

AppController::AppController(QString host, quint16 port, QObject *parent)
    : QObject(parent), m_host(std::move(host)), m_port(port)
{
    m_queueModel = new QueueModel(this);
    m_filterModel = new TaskFilterModel(this);
    m_filterModel->setSourceModel(m_queueModel);

    m_worker = new ControlWorker(m_host, m_port);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &AppController::refreshRequested, m_worker, &ControlWorker::refresh);
    connect(this, &AppController::mutationRequested, m_worker, &ControlWorker::mutate);
    connect(m_worker, &ControlWorker::refreshed, this, &AppController::onRefreshed);
    connect(m_worker, &ControlWorker::mutationSucceeded, this,
            &AppController::onMutationSucceeded);
    connect(m_worker, &ControlWorker::failed, this, &AppController::onFailed);
    connect(m_filterModel, &TaskFilterModel::searchTextChanged, this,
            &AppController::searchTextChanged);
    connect(m_filterModel, &TaskFilterModel::tagFilterChanged, this,
            &AppController::activeTagsChanged);

    m_thread.setObjectName(QStringLiteral("taskpilot-ui-client"));
    m_thread.start();
}

AppController::~AppController()
{
    m_thread.quit();
    // One refresh performs two calls, each bounded at three 5 s steps, and a
    // failed call may add a ping. Sixty seconds covers the worst case; a
    // shorter wait would let ~QThread abort the process on a stalled daemon.
    m_thread.wait(60000);
}

QString AppController::sourceLabel() const
{
    if (m_offline)
    {
        return QStringLiteral("Offline");
    }
    if (!m_connected)
    {
        return QStringLiteral("Connecting...");
    }
    return QStringLiteral("Connected · %1:%2").arg(m_host).arg(m_port);
}

QString AppController::searchText() const
{
    return m_filterModel->searchText();
}

QStringList AppController::activeTags() const
{
    return m_filterModel->tagFilter();
}

QAbstractItemModel *AppController::queueModel() const
{
    return m_filterModel;
}

void AppController::refresh()
{
    if (m_busy)
    {
        m_refreshAgain = true;
        return;
    }
    setBusy(true);
    emit refreshRequested();
}

bool AppController::addTask(const QString &title, int importance, const QString &dueText,
                            int blocks, const QString &tagsText, const QString &notes)
{
    if (m_busy)
    {
        setLocalError(QStringLiteral("A request is already in progress."));
        return false;
    }

    const QString trimmedTitle = title.trimmed();
    if (trimmedTitle.isEmpty())
    {
        setLocalError(QStringLiteral("A title is required."));
        return false;
    }

    nlohmann::json params;
    params["title"] = trimmedTitle.toStdString();
    params["importance"] = qBound(1, importance, 5);
    params["blocks"] = blocks < 0 ? 0 : blocks;

    if (!notes.trimmed().isEmpty())
    {
        params["notes"] = notes.trimmed().toStdString();
    }

    if (!dueText.trimmed().isEmpty())
    {
        const QDate date = QDate::fromString(dueText.trimmed(), QStringLiteral("yyyy-MM-dd"));
        if (!date.isValid())
        {
            setLocalError(QStringLiteral("The due date must be yyyy-MM-dd."));
            return false;
        }
        const QDateTime due(date, QTime(0, 0));
        params["due_at"] = due.toSecsSinceEpoch();
    }

    nlohmann::json tags = nlohmann::json::array();
    const QStringList parts = tagsText.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &part : parts)
    {
        const QString tag = part.trimmed();
        if (!tag.isEmpty())
        {
            tags.push_back(tag.toStdString());
        }
    }
    if (!tags.empty())
    {
        params["tags"] = std::move(tags);
    }

    runMutation(QStringLiteral("add_task"), encode(params));
    return true;
}

void AppController::completeTask(qint64 id)
{
    if (m_busy)
    {
        return;
    }
    nlohmann::json params;
    params["id"] = id;
    runMutation(QStringLiteral("complete_task"), encode(params));
}

void AppController::reopenTask(qint64 id)
{
    if (m_busy)
    {
        return;
    }
    nlohmann::json params;
    params["id"] = id;
    runMutation(QStringLiteral("reopen_task"), encode(params));
}

void AppController::deleteTask(qint64 id)
{
    if (m_busy)
    {
        return;
    }
    nlohmann::json params;
    params["id"] = id;
    runMutation(QStringLiteral("delete_task"), encode(params));
}

void AppController::setSearchText(const QString &text)
{
    m_filterModel->setSearchText(text);
}

void AppController::toggleTag(const QString &tag, bool enabled)
{
    QStringList tags = m_filterModel->tagFilter();
    if (enabled)
    {
        if (!tags.contains(tag))
        {
            tags.append(tag);
        }
    }
    else
    {
        tags.removeAll(tag);
    }
    m_filterModel->setTagFilter(tags);
}

void AppController::clearFilters()
{
    m_filterModel->setSearchText(QString());
    m_filterModel->setTagFilter(QStringList());
}

void AppController::onRefreshed(const QByteArray &queuePayload, const QByteArray &statsPayload)
{
    QString parseError;
    if (!m_queueModel->applyQueueReply(queuePayload, &parseError))
    {
        setError(parseError, false);
        finishRequest();
        return;
    }

    applyStats(statsPayload);
    applyWeights(queuePayload);
    rebuildTagCounts();

    m_connected = true;
    m_offline = false;
    m_errorText.clear();
    emit stateChanged();
    finishRequest();
}

void AppController::onMutationSucceeded(const QString &method)
{
    Q_UNUSED(method);
    m_connected = true;
    m_offline = false;
    m_errorText.clear();
    emit stateChanged();
    // Consume a refresh requested while the mutation was in flight so it does
    // not run a second time through finishRequest().
    m_refreshAgain = false;
    setBusy(false);
    refresh();
}

void AppController::onFailed(const QString &message, bool offline)
{
    if (offline)
    {
        m_connected = false;
    }
    m_offline = offline;
    m_errorText = message;
    emit stateChanged();
    finishRequest();
}

void AppController::runMutation(const QString &method, const QByteArray &params)
{
    setBusy(true);
    emit mutationRequested(method, params);
}

void AppController::setBusy(bool busy)
{
    if (m_busy == busy)
    {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void AppController::setError(const QString &message, bool offline)
{
    m_errorText = message;
    m_offline = offline;
    if (offline)
    {
        m_connected = false;
    }
    emit stateChanged();
}

void AppController::setLocalError(const QString &message)
{
    // A validation failure says nothing about the connection; do not let it
    // turn an offline banner into "Connecting..." without a connection try.
    m_errorText = message;
    emit stateChanged();
}

void AppController::rebuildTagCounts()
{
    QMap<QString, int> counts;
    for (const QueueRow &row : m_queueModel->rows())
    {
        for (const QString &tag : row.tags)
        {
            counts[tag] += 1;
        }
    }

    QVariantList result;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
    {
        QVariantMap entry;
        entry.insert(QStringLiteral("tag"), it.key());
        entry.insert(QStringLiteral("count"), it.value());
        result.append(entry);
    }

    // An active tag that no longer exists would have no checkbox, leaving an
    // invisible filter behind; drop it when the queue no longer carries it.
    const QStringList active = m_filterModel->tagFilter();
    QStringList kept;
    for (const QString &tag : active)
    {
        if (counts.contains(tag))
        {
            kept.append(tag);
        }
    }
    if (kept != active)
    {
        m_filterModel->setTagFilter(kept);
    }

    m_tagCounts = result;
    emit tagCountsChanged();
}

void AppController::applyStats(const QByteArray &payload)
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject())
    {
        return;
    }

    const QJsonObject root = document.object();
    const qint64 now = static_cast<qint64>(root.value(QStringLiteral("now")).toDouble());
    const QString updated = QDateTime::fromSecsSinceEpoch(now).toString(QStringLiteral("HH:mm:ss"));

    QString summary = QStringLiteral("open %1 · in progress %2 · overdue %3 · updated %4")
                          .arg(root.value(QStringLiteral("open")).toInt())
                          .arg(root.value(QStringLiteral("in_progress")).toInt())
                          .arg(root.value(QStringLiteral("overdue")).toInt())
                          .arg(updated);

    const QString top = root.value(QStringLiteral("top_title")).toString();
    if (!top.isEmpty())
    {
        summary += QStringLiteral(" · top: %1").arg(top);
    }

    if (summary != m_statusSummary)
    {
        m_statusSummary = summary;
        emit summaryChanged();
    }
}

void AppController::applyWeights(const QByteArray &payload)
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject())
    {
        return;
    }

    const QJsonObject weights = document.object().value(QStringLiteral("weights")).toObject();
    if (weights.isEmpty())
    {
        return;
    }

    const QString summary =
        QStringLiteral("importance %1 · urgency %2 · age %3 · blocks %4 · horizon %5d")
            .arg(formatWeight(weights.value(QStringLiteral("importance")).toDouble()))
            .arg(formatWeight(weights.value(QStringLiteral("urgency")).toDouble()))
            .arg(formatWeight(weights.value(QStringLiteral("age")).toDouble()))
            .arg(formatWeight(weights.value(QStringLiteral("blocks")).toDouble()))
            .arg(formatWeight(weights.value(QStringLiteral("urgency_horizon_days")).toDouble()));

    if (summary != m_weightsSummary)
    {
        m_weightsSummary = summary;
        emit summaryChanged();
    }
}

void AppController::finishRequest()
{
    setBusy(false);
    if (m_refreshAgain)
    {
        m_refreshAgain = false;
        refresh();
    }
}

} // namespace taskpilot::ui
