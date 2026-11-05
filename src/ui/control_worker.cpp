// ControlWorker.cpp — the only place that calls ControlClient

#include "ui/control_worker.h"

#include "control/ControlClient.hpp"

#include <nlohmann/json.hpp>

#include <exception>

namespace taskpilot::ui
{
namespace
{

// get_queue returns the whole ranked queue, so its reply grows with the
// backlog and the client's 1 MiB default guard would eventually make every
// refresh fail. The cap is still a runaway guard, not a size policy.
constexpr std::size_t kQueueMaxReplyBytes{ 64U * 1024U * 1024U };

} // namespace

ControlWorker::ControlWorker(QString host, quint16 port, QObject *parent)
    : QObject(parent), m_host(std::move(host)), m_port(port)
{
}

void ControlWorker::refresh()
{
    ControlClient client(m_host.toStdString(), m_port, kDefaultTimeoutMs, kQueueMaxReplyBytes);

    const RpcResult queue = client.call("get_queue", nlohmann::json::object());
    if (!queue.ok())
    {
        reportFailure(QString::fromStdString(queue.error().message),
                      queue.error().code == ErrorCode::kStorageFailure && !client.ping());
        return;
    }

    const RpcResult stats = client.call("get_stats", nlohmann::json::object());
    if (!stats.ok())
    {
        reportFailure(QString::fromStdString(stats.error().message),
                      stats.error().code == ErrorCode::kStorageFailure && !client.ping());
        return;
    }

    emit refreshed(QByteArray::fromStdString(queue.value().dump()),
                   QByteArray::fromStdString(stats.value().dump()));
}

void ControlWorker::mutate(const QString &method, const QByteArray &paramsJson)
{
    nlohmann::json params;
    try
    {
        params = nlohmann::json::parse(paramsJson.toStdString());
    }
    catch (const std::exception &)
    {
        emit failed(QStringLiteral("The request could not be encoded."), false);
        return;
    }

    ControlClient client(m_host.toStdString(), m_port);
    const RpcResult result = client.call(method.toStdString(), params);
    if (!result.ok())
    {
        reportFailure(QString::fromStdString(result.error().message),
                      result.error().code == ErrorCode::kStorageFailure && !client.ping());
        return;
    }

    emit mutationSucceeded(method);
}

void ControlWorker::reportFailure(const QString &message, bool offline)
{
    emit failed(message, offline);
}

} // namespace taskpilot::ui
