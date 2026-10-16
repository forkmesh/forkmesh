#include "TrayMode.h"

#include "AppPaths.h"
#include "BackgroundMode.h"
#include "DesktopDisplayReadiness.h"
#include "SingleInstance.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QIcon>
#include <QLockFile>
#include <QMenu>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSystemTrayIcon>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif

namespace forkmesh::tray {

#if defined(Q_OS_LINUX)

namespace {

constexpr int kPollMs = 2000;
constexpr qint64 kStartupGraceMs = 90000;
constexpr qint64 kGoneGraceMs = 30000;
constexpr qint64 kTrayWaitMs = 120000;
constexpr int kDisplayWaitMs = 30000;

bool pidAlive(qint64 pid)
{
    return pid > 1 && (::kill(pid_t(pid), 0) == 0 || errno == EPERM);
}

qint64 lockHolder()
{
    qint64 pid = 0;
    QString host;
    QString application;
    if (!QLockFile(singleInstanceLockPath()).getLockInfo(&pid, &host, &application))
        return 0;
    return pidAlive(pid) ? pid : 0;
}

QString toolTip(const QString &project)
{
    QString text = QStringLiteral("ForkMesh is running without a window");
    if (project != QLatin1String(paths::kHomeProjectSlug))
        text += QStringLiteral(" (%1)").arg(project);
    return text;
}

void openDesktop(const QString &project)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &name : background::serviceVariables())
        environment.remove(name);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(QDir::homePath());
    process.setProgram(background::desktopBinaryPath());
    process.setArguments({QStringLiteral("--project"), project});
    process.startDetached();
}

} // namespace

int run(int argc, char *argv[])
{
    if (waitForDesktopDisplay(QProcessEnvironment::systemEnvironment(), kDisplayWaitMs, 500).state !=
        DesktopDisplayState::Ready)
        return 0;
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ForkMesh"));
    app.setOrganizationName(QStringLiteral("ForkMesh"));
    app.setQuitOnLastWindowClosed(false);
    paths::activate(app.arguments());
    const QIcon appIcon(QStringLiteral(":/app/forkmesh.png"));
    app.setWindowIcon(appIcon);
    QGuiApplication::setDesktopFileName(QStringLiteral("forkmesh"));

    const QString project = paths::projectSlug();
    QDir().mkpath(paths::stateRoot());
    QLockFile lock(paths::stateRoot() + QStringLiteral("/tray.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return 0;

    QSystemTrayIcon icon(appIcon);
    icon.setToolTip(toolTip(project));
    QMenu menu;
    QAction *openAction = menu.addAction(QStringLiteral("Open ForkMesh"));
    QAction *stopAction = menu.addAction(QStringLiteral("Stop ForkMesh running without a window"));
    menu.addSeparator();
    QAction *hideAction = menu.addAction(QStringLiteral("Hide icon"));
    icon.setContextMenu(&menu);

    QObject::connect(openAction, &QAction::triggered, &app, [project] { openDesktop(project); });
    QObject::connect(stopAction, &QAction::triggered, &app, [&app] {
        askBackgroundInstanceToQuit();
        app.quit();
    });
    QObject::connect(hideAction, &QAction::triggered, &app, &QCoreApplication::quit);
    QObject::connect(&icon, &QSystemTrayIcon::activated, &app,
                     [project](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger)
                             openDesktop(project);
                     });

    QElapsedTimer started;
    started.start();
    QElapsedTimer lastSeen;
    bool trayFound = false;
    const auto poll = [&] {
        trayFound = trayFound || QSystemTrayIcon::isSystemTrayAvailable();
        if (!trayFound && started.elapsed() > kTrayWaitMs) {
            app.quit();
            return;
        }
        const qint64 holder = lockHolder();
        const qint64 background = backgroundInstancePid();
        if (holder > 0 && holder != background && lastSeen.isValid()) {
            app.quit();
            return;
        }
        if (background > 0 && holder == background) {
            lastSeen.start();
            if (trayFound && !icon.isVisible())
                icon.show();
            return;
        }
        if (icon.isVisible())
            icon.hide();
        const bool seen = lastSeen.isValid();
        if ((seen ? lastSeen.elapsed() : started.elapsed()) > (seen ? kGoneGraceMs : kStartupGraceMs))
            app.quit();
    };
    QTimer timer;
    timer.setInterval(kPollMs);
    QObject::connect(&timer, &QTimer::timeout, &app, poll);
    timer.start();
    QTimer::singleShot(0, &app, poll);
    return app.exec();
}

#else

int run(int, char *[])
{
    return 0;
}

#endif

} // namespace forkmesh::tray
