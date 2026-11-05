// main.cpp — the Qt Quick desktop client
//
// The daemon owns the data and the ranking. This process only renders the
// queue and forwards user actions through ControlClient; it never opens the
// database and never connects to a non-loopback address.

#include "ui/app_controller.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <cstdio>

namespace
{

[[nodiscard]] bool isLoopbackHost(const QString &host)
{
    const QString lowered = host.trimmed().toLower();
    return lowered == QStringLiteral("127.0.0.1") || lowered == QStringLiteral("localhost") ||
           lowered == QStringLiteral("::1");
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("taskPilot UI"));
    QCoreApplication::setOrganizationName(QStringLiteral("taskPilot"));
    // Basic gives the custom QML controls a neutral base; the Apple-style
    // look lives in the app's own components and Theme tokens.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("taskPilot desktop client (the daemon must be running)"));
    parser.addHelpOption();
    const QCommandLineOption hostOption(
        QStringLiteral("host"), QStringLiteral("Daemon host (loopback only)."),
        QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    const QCommandLineOption portOption(QStringLiteral("port"), QStringLiteral("Daemon port."),
                                        QStringLiteral("port"), QStringLiteral("8091"));
    const QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"), QStringLiteral("Save one frame as PNG and exit."),
        QStringLiteral("path"));
    const QCommandLineOption selectFirstOption(
        QStringLiteral("select-first"),
        QStringLiteral("Select the first queue row before capturing (screenshot checks)."));
    const QCommandLineOption openAddDialogOption(
        QStringLiteral("open-add-dialog"),
        QStringLiteral("Open the Add task dialog before capturing (screenshot checks)."));
    parser.addOption(hostOption);
    parser.addOption(portOption);
    parser.addOption(screenshotOption);
    parser.addOption(selectFirstOption);
    parser.addOption(openAddDialogOption);
    parser.process(app);

    // Precedence matches the daemon: built-in default < environment < flag.
    QString host = parser.value(hostOption).trimmed();
    if (!parser.isSet(hostOption))
    {
        const QByteArray environmentHost = qgetenv("TASKPILOT_BIND");
        if (!environmentHost.isEmpty())
        {
            host = QString::fromLocal8Bit(environmentHost).trimmed();
        }
    }

    if (!isLoopbackHost(host))
    {
        std::fprintf(stderr,
                     "taskpilot-ui: the control socket is loopback-only; refusing host '%s'\n",
                     qPrintable(host));
        return 2;
    }

    QString portText = parser.value(portOption);
    if (!parser.isSet(portOption))
    {
        const QByteArray environmentPort = qgetenv("TASKPILOT_PORT");
        if (!environmentPort.isEmpty())
        {
            portText = QString::fromLocal8Bit(environmentPort);
        }
    }

    bool portOk = false;
    const uint portValue = portText.toUInt(&portOk);
    if (!portOk || portValue == 0 || portValue > 65535)
    {
        std::fprintf(stderr, "taskpilot-ui: invalid port '%s'\n", qPrintable(portText));
        return 2;
    }

    taskpilot::ui::AppController controller(host, static_cast<quint16>(portValue));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("taskQueueModel"),
                                             controller.queueModel());
    engine.load(QUrl(QStringLiteral("qrc:/ui/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
    {
        std::fprintf(stderr, "taskpilot-ui: failed to load the QML interface\n");
        return 1;
    }

    const QString screenshotPath = parser.value(screenshotOption);
    if (!screenshotPath.isEmpty())
    {
        const bool selectFirst = parser.isSet(selectFirstOption);
        const bool openAddDialog = parser.isSet(openAddDialogOption);
        QTimer::singleShot(2500, &app,
                           [&engine, screenshotPath, selectFirst, openAddDialog, &app]
                           {
                               auto *window =
                                   qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
                               if (window == nullptr)
                               {
                                   std::fprintf(stderr, "taskpilot-ui: no window to capture\n");
                                   app.exit(1);
                                   return;
                               }
                               if (selectFirst)
                               {
                                   if (auto *queuePane = window->findChild<QQuickItem *>(
                                           QStringLiteral("queuePane")))
                                   {
                                       QMetaObject::invokeMethod(
                                           queuePane, "selectRow", Q_ARG(QVariant, 0));
                                       QCoreApplication::processEvents();
                                   }
                               }
                               auto capture = [&app, window, screenshotPath]
                               {
                                   const QImage image = window->grabWindow();
                                   if (image.isNull() || !image.save(screenshotPath, "PNG"))
                                   {
                                       std::fprintf(stderr,
                                                    "taskpilot-ui: failed to save screenshot to %s\n",
                                                    qPrintable(screenshotPath));
                                       app.exit(1);
                                       return;
                                   }
                                   std::fprintf(stdout, "taskpilot-ui: screenshot saved to %s\n",
                                                qPrintable(screenshotPath));
                                   app.exit(0);
                               };
                               if (openAddDialog)
                               {
                                   if (auto *shell = window->findChild<QQuickItem *>(
                                           QStringLiteral("desktopShell")))
                                   {
                                       QMetaObject::invokeMethod(shell, "openAddDialog");
                                   }
                                   QTimer::singleShot(500, &app, capture);
                                   return;
                               }
                               capture();
                           });
    }

    return app.exec();
}
