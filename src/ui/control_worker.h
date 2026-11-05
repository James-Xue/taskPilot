#pragma once
// ControlWorker.hpp — the blocking control-socket client on its own thread
//
// ControlClient::call is synchronous (one fresh connection, 5 s per step) and
// must never run on the GUI thread. This object lives on a QThread and is
// driven by queued signals from AppController; it converts replies to compact
// UTF-8 QByteArray before emitting, so no nlohmann object crosses threads.

#include <QByteArray>
#include <QObject>
#include <QString>

namespace taskpilot::ui
{

class ControlWorker : public QObject
{
    Q_OBJECT

  public:
    ControlWorker(QString host, quint16 port, QObject *parent = nullptr);

  public slots:
    /// Fetch `get_queue` and `get_stats`, then emit `refreshed`.
    void refresh();

    /// One mutation call; `paramsJson` is a JSON object encoded as UTF-8.
    void mutate(const QString &method, const QByteArray &paramsJson);

  signals:
    void refreshed(const QByteArray &queuePayload, const QByteArray &statsPayload);
    void mutationSucceeded(const QString &method);
    void failed(const QString &message, bool offline);

  private:
    void reportFailure(const QString &message, bool offline);

    QString m_host;
    quint16 m_port;
};

} // namespace taskpilot::ui
