#pragma once
// AppController.hpp — the QML-facing state machine
//
// Owns the worker thread, the queue model and the filter model. Every QML
// action lands here; the controller turns it into exactly one RPC (or one
// refresh) and never touches the daemon from the GUI thread.

#include <QAbstractItemModel>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>

namespace taskpilot::ui
{

class ControlWorker;
class QueueModel;
class TaskFilterModel;

class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool offline READ offline NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString sourceLabel READ sourceLabel NOTIFY stateChanged)
    Q_PROPERTY(QString statusSummary READ statusSummary NOTIFY summaryChanged)
    Q_PROPERTY(QString weightsSummary READ weightsSummary NOTIFY summaryChanged)
    Q_PROPERTY(QVariantList tagCounts READ tagCounts NOTIFY tagCountsChanged)
    Q_PROPERTY(QString searchText READ searchText NOTIFY searchTextChanged)
    Q_PROPERTY(QStringList activeTags READ activeTags NOTIFY activeTagsChanged)
    Q_PROPERTY(QAbstractItemModel *queueModel READ queueModel CONSTANT)

  public:
    AppController(QString host, quint16 port, QObject *parent = nullptr);
    ~AppController() override;

    [[nodiscard]] bool busy() const { return m_busy; }
    [[nodiscard]] bool offline() const { return m_offline; }
    [[nodiscard]] QString errorText() const { return m_errorText; }
    [[nodiscard]] QString sourceLabel() const;
    [[nodiscard]] QString statusSummary() const { return m_statusSummary; }
    [[nodiscard]] QString weightsSummary() const { return m_weightsSummary; }
    [[nodiscard]] QVariantList tagCounts() const { return m_tagCounts; }
    [[nodiscard]] QString searchText() const;
    [[nodiscard]] QStringList activeTags() const;
    [[nodiscard]] QAbstractItemModel *queueModel() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool addTask(const QString &title, int importance, const QString &dueText,
                             int blocks, const QString &tagsText, const QString &notes);
    Q_INVOKABLE void completeTask(qint64 id);
    Q_INVOKABLE void reopenTask(qint64 id);
    Q_INVOKABLE void deleteTask(qint64 id);
    Q_INVOKABLE void setSearchText(const QString &text);
    Q_INVOKABLE void toggleTag(const QString &tag, bool enabled);
    Q_INVOKABLE void clearFilters();

  signals:
    void refreshRequested();
    void mutationRequested(const QString &method, const QByteArray &params);
    void busyChanged();
    void stateChanged();
    void summaryChanged();
    void tagCountsChanged();
    void searchTextChanged();
    void activeTagsChanged();

  private slots:
    void onRefreshed(const QByteArray &queuePayload, const QByteArray &statsPayload);
    void onMutationSucceeded(const QString &method);
    void onFailed(const QString &message, bool offline);

  private:
    void runMutation(const QString &method, const QByteArray &params);
    void setBusy(bool busy);
    void setError(const QString &message, bool offline);
    void setLocalError(const QString &message);
    void rebuildTagCounts();
    void applyStats(const QByteArray &payload);
    void applyWeights(const QByteArray &payload);
    void finishRequest();

    QString m_host;
    quint16 m_port;
    QThread m_thread;
    ControlWorker *m_worker{ nullptr };
    QueueModel *m_queueModel{ nullptr };
    TaskFilterModel *m_filterModel{ nullptr };

    bool m_busy{ false };
    bool m_offline{ false };
    bool m_connected{ false };
    bool m_refreshAgain{ false };
    QString m_errorText;
    QString m_statusSummary;
    QString m_weightsSummary;
    QVariantList m_tagCounts;
};

} // namespace taskpilot::ui
