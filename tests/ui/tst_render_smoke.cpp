// tst_render_smoke.cpp — offscreen render smoke for the whole desktop shell

#include "ui/queue_model.h"

#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QtTest>

using taskpilot::ui::QueueModel;

namespace
{

class StubController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(bool offline READ offline CONSTANT)
    Q_PROPERTY(QString errorText READ errorText CONSTANT)
    Q_PROPERTY(QString sourceLabel READ sourceLabel CONSTANT)
    Q_PROPERTY(QString statusSummary READ statusSummary CONSTANT)
    Q_PROPERTY(QString weightsSummary READ weightsSummary CONSTANT)
    Q_PROPERTY(QVariantList tagCounts READ tagCounts CONSTANT)
    Q_PROPERTY(QString searchText READ searchText NOTIFY searchTextChanged)
    Q_PROPERTY(QStringList activeTags READ activeTags NOTIFY activeTagsChanged)

  public:
    [[nodiscard]] bool busy() const { return false; }
    [[nodiscard]] bool offline() const { return false; }
    [[nodiscard]] QString errorText() const { return {}; }
    [[nodiscard]] QString sourceLabel() const { return QStringLiteral("Connected · 127.0.0.1:8091"); }
    [[nodiscard]] QString statusSummary() const
    {
        return QStringLiteral("open 2 · in progress 0 · overdue 0 · updated 12:00:00");
    }
    [[nodiscard]] QString weightsSummary() const
    {
        return QStringLiteral("importance 5 · urgency 30 · age 2 · blocks 8 · horizon 7d");
    }
    [[nodiscard]] QVariantList tagCounts() const
    {
        QVariantMap work;
        work.insert(QStringLiteral("tag"), QStringLiteral("work"));
        work.insert(QStringLiteral("count"), 2);
        return { work };
    }
    [[nodiscard]] QString searchText() const { return {}; }
    [[nodiscard]] QStringList activeTags() const { return {}; }

    Q_INVOKABLE void refresh() {}
    Q_INVOKABLE bool addTask(const QString &, int, const QString &, int, const QString &,
                             const QString &)
    {
        return true;
    }
    Q_INVOKABLE void completeTask(qint64) {}
    Q_INVOKABLE void reopenTask(qint64) {}
    Q_INVOKABLE void deleteTask(qint64) {}
    Q_INVOKABLE void setSearchText(const QString &) { emit searchTextChanged(); }
    Q_INVOKABLE void toggleTag(const QString &, bool) { emit activeTagsChanged(); }
    Q_INVOKABLE void clearFilters() { emit activeTagsChanged(); }

  signals:
    void searchTextChanged();
    void activeTagsChanged();
};

} // namespace

class RenderSmokeTest : public QObject
{
    Q_OBJECT

  private slots:
    void rendersTheDesktopShell();
};

void RenderSmokeTest::rendersTheDesktopShell()
{
    QueueModel model;
    QString error;
    const QByteArray payload = R"({"now":1,"weights":
        {"importance":5,"urgency":30,"age":2,"blocks":8,"urgency_horizon_days":7},
        "queue":[
        {"id":1,"uid":"a","title":"Alpha","notes":"first","status":"open",
         "importance":3,"due_at":null,"blocks":0,"tags":["work"],
         "score":30,"score_parts":{"importance":15,"urgency":10,"age":5,"blocks":0},
         "urgency_factor":0.3,"age_days":2},
        {"id":2,"uid":"b","title":"Beta","notes":"second","status":"open",
         "importance":4,"due_at":null,"blocks":1,"tags":["home"],
         "score":20,"score_parts":{"importance":20,"urgency":0,"age":0,"blocks":0},
         "urgency_factor":0,"age_days":0}
    ]})";
    QVERIFY2(model.applyQueueReply(payload, &error), qPrintable(error));

    StubController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("taskQueueModel"), &model);

    const QString qmlDirectory = QStringLiteral(TASKPILOT_UI_QML_DIR);
    engine.load(QUrl::fromLocalFile(qmlDirectory + QStringLiteral("/Main.qml")));
    QVERIFY(!engine.rootObjects().isEmpty());

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    QVERIFY(window != nullptr);
    window->show();
    QTest::qWait(400);

    const QImage image = window->grabWindow();
    QVERIFY(!image.isNull());
    QVERIFY(image.width() >= 900);
    QVERIFY(image.height() >= 600);

    QSet<QRgb> colours;
    for (int y = 0; y < image.height(); y += 40)
    {
        for (int x = 0; x < image.width(); x += 40)
        {
            colours.insert(image.pixel(x, y));
        }
    }
    QVERIFY2(colours.size() > 1, "the frame is a single colour; the shell did not render");

    window->close();
}

QTEST_MAIN(RenderSmokeTest)

#include "tst_render_smoke.moc"
