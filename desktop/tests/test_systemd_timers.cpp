#include "SystemdTimers.h"
#include "SystemdTimersPanel.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <functional>
#include <memory>

using namespace forkmesh::systemdtimers;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

constexpr qint64 kNow = 1790983000000000;

const QByteArray kUnitFiles = R"([
 {"unit_file":"fmnext-backup.timer","state":"enabled","preset":"enabled"},
 {"unit_file":"forkmesh-release-host.timer","state":"enabled","preset":"enabled"},
 {"unit_file":"systemd-tmpfiles-clean.timer","state":"disabled","preset":"enabled"}
])";

const QByteArray kListTimers = R"([
 {"next":1790983300000000,"left":1,"last":1790982880000000,"passed":1,
  "unit":"forkmesh-release-host.timer","activates":"forkmesh-release-host.service"},
 {"next":1790990000000000,"left":1,"last":null,"passed":null,
  "unit":"fmnext-backup.timer","activates":"fmnext-backup.service"},
 {"next":1,"left":1,"last":1,"passed":1,
  "unit":"systemd-tmpfiles-clean.timer","activates":"systemd-tmpfiles-clean.service"}
])";

QByteArray showOutput(bool releaseHostOn)
{
    QByteArray text;
    text += "Id=fmnext-backup.timer\nDescription=Back up the fleet\nLoadState=loaded\n"
            "UnitFileState=enabled\nActiveState=active\nResult=success\nExecStart=\n"
            "DropInPaths=\n\n";
    text += "Id=fmnext-backup.service\nDescription=Fleet backup\nLoadState=loaded\n"
            "UnitFileState=static\nActiveState=inactive\nResult=exit-code\n"
            "ExecStart={ path=/usr/bin/python3 ; argv[]=/usr/bin/python3 backup.py ; "
            "ignore_errors=no ; start_time=[n/a] ; stop_time=[n/a] ; pid=0 ; "
            "code=(null) ; status=0/0 }\nDropInPaths=\n\n";
    text += "Id=forkmesh-release-android.timer\nDescription=forkmesh-release-android.timer\n"
            "LoadState=not-found\nUnitFileState=\nActiveState=inactive\nResult=success\n"
            "ExecStart=\nDropInPaths=\n\n";
    text += "Id=forkmesh-release-android.service\nDescription=Android release\n"
            "LoadState=loaded\nUnitFileState=linked\nActiveState=inactive\nResult=success\n"
            "ExecStart={ path=/usr/bin/python3 ; argv[]=/usr/bin/python3 "
            "/src/tools/forkmesh-release-android.py --auto --skip-ios ; ignore_errors=no ; "
            "start_time=[n/a] ; stop_time=[n/a] ; pid=0 ; code=(null) ; status=0/0 }\n"
            "DropInPaths=/home/u/.config/systemd/user/forkmesh-release-android.service.d/"
            "skip-ios.conf\n\n";
    text += "Id=forkmesh-release-host.timer\nDescription=Complete held-back releases\n"
            "LoadState=loaded\nUnitFileState=" + QByteArray(releaseHostOn ? "enabled" : "disabled")
            + "\nActiveState=" + QByteArray(releaseHostOn ? "active" : "inactive")
            + "\nResult=success\nExecStart=\nDropInPaths=\n\n";
    text += "Id=forkmesh-release-host.service\nDescription=Release host\nLoadState=loaded\n"
            "UnitFileState=linked\nActiveState=inactive\nResult=success\n"
            "ExecStart={ path=/usr/bin/python3 ; argv[]=/usr/bin/python3 "
            "/src/tools/forkmesh-release-host.py --auto --skip-ios ; ignore_errors=no ; "
            "start_time=[n/a] ; stop_time=[n/a] ; pid=0 ; code=(null) ; status=0/0 }\n"
            "DropInPaths=/a/skip-ios.conf /a/nice.conf\n";
    return text;
}

struct FakeSystemd {
    QMutex mutex;
    QList<QStringList> calls;
    bool releaseHostOn = true;
    bool failDisable = false;

    Runner runner()
    {
        return [this](const QStringList &arguments) {
            QMutexLocker lock(&mutex);
            calls.append(arguments);
            CommandResult result;
            result.started = true;
            result.exitCode = 0;
            const QString verb = arguments.value(0);
            if (verb == QLatin1String("list-unit-files")) {
                QByteArray files = kUnitFiles;
                if (!releaseHostOn)
                    files.replace("\"forkmesh-release-host.timer\",\"state\":\"enabled\"",
                                  "\"forkmesh-release-host.timer\",\"state\":\"disabled\"");
                result.output = files;
            } else if (verb == QLatin1String("list-timers")) {
                result.output = kListTimers;
            } else if (verb == QLatin1String("show")) {
                result.output = showOutput(releaseHostOn);
            } else if (verb == QLatin1String("disable")) {
                if (failDisable) {
                    result.exitCode = 1;
                    result.error = QStringLiteral("Access denied");
                } else {
                    releaseHostOn = false;
                }
            } else if (verb == QLatin1String("enable")) {
                releaseHostOn = true;
            }
            return result;
        };
    }

    bool called(const QStringList &arguments)
    {
        QMutexLocker lock(&mutex);
        return calls.contains(arguments);
    }
};

bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!elapsed.hasExpired(5000)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition())
            return true;
        QThread::msleep(5);
    }
    return condition();
}

QString writeSources(const QTemporaryDir &directory)
{
    const QString path = directory.filePath(QStringLiteral("systemd"));
    QDir().mkpath(path);
    QFile android(path + QStringLiteral("/forkmesh-release-android.timer"));
    android.open(QIODevice::WriteOnly);
    android.write("[Unit]\nDescription=Complete held-back releases every 5 minutes\n\n"
                  "[Timer]\nOnUnitActiveSec=5min\n");
    android.close();
    QFile other(path + QStringLiteral("/someone-else.timer"));
    other.open(QIODevice::WriteOnly);
    other.write("[Unit]\nDescription=Not ours\n");
    return path;
}

const Timer *find(const QList<Timer> &timers, const QString &unit)
{
    for (const Timer &timer : timers) {
        if (timer.unit == unit)
            return &timer;
    }
    return nullptr;
}

void testParsers()
{
    const QList<ListedTimer> listed = parseListTimers(kListTimers);
    check(listed.size() == 3, "list-timers rows");
    check(listed.at(0).nextUsec == 1790983300000000, "next usec");
    check(listed.at(1).lastUsec == 0, "null last is zero");
    check(listed.at(0).activates == QLatin1String("forkmesh-release-host.service"),
          "activates");

    const QHash<QString, QString> files = parseUnitFiles(kUnitFiles);
    check(files.value(QStringLiteral("systemd-tmpfiles-clean.timer")) == QLatin1String("disabled"),
          "unit file state");

    const auto blocks = parseShow(showOutput(true));
    check(blocks.size() == 6, "show blocks");
    check(blocks.at(3).value(QStringLiteral("UnitFileState")) == QLatin1String("linked"),
          "show values");

    check(commandLine(blocks.at(5).value(QStringLiteral("ExecStart")))
              == QLatin1String("/usr/bin/python3 /src/tools/forkmesh-release-host.py --auto "
                               "--skip-ios"),
          "command line from ExecStart");
    check(commandLine(QString()).isEmpty(), "empty ExecStart");

    check(isManagedUnit(QStringLiteral("forkmesh-x.timer")), "forkmesh prefix");
    check(isManagedUnit(QStringLiteral("fmnext-x.timer")), "fmnext prefix");
    check(!isManagedUnit(QStringLiteral("forkmesh-x.service")), "services are not timers");
    check(!isManagedUnit(QStringLiteral("systemd-tmpfiles-clean.timer")), "foreign timers");
    check(serviceFor(QStringLiteral("forkmesh-a.timer")) == QLatin1String("forkmesh-a.service"),
          "service name");

    check(relativeTime(kNow + 300000000, kNow) == QLatin1String("in 5 min"), "in 5 min");
    check(relativeTime(kNow - 30000000, kNow) == QLatin1String("30 s ago"), "30 s ago");
    check(relativeTime(kNow + 7500000000, kNow) == QLatin1String("in 2 h 5 min"), "hours");
    check(relativeTime(kNow - 2LL * 86400 * 1000000, kNow) == QLatin1String("2 d ago"), "days");
    check(relativeTime(0, kNow).isEmpty(), "never");
}

void testCollect()
{
    QTemporaryDir directory;
    const QString sources = writeSources(directory);
    FakeSystemd systemd;
    const Snapshot snapshot = collect(systemd.runner(), sources);
    check(snapshot.error.isEmpty(), "collect error");
    check(snapshot.timers.size() == 3, "managed timers only");
    check(!find(snapshot.timers, QStringLiteral("systemd-tmpfiles-clean.timer")),
          "foreign timer hidden");
    check(!find(snapshot.timers, QStringLiteral("someone-else.timer")), "foreign source hidden");

    const Timer *android = find(snapshot.timers, QStringLiteral("forkmesh-release-android.timer"));
    check(android, "uninstalled source timer listed");
    if (android) {
        check(!android->installed(), "uninstalled");
        check(!android->enabled(), "uninstalled is off");
        check(android->description
                  == QLatin1String("Complete held-back releases every 5 minutes"),
              "description read from the unit file");
        check(android->command.endsWith(QLatin1String("--auto --skip-ios")), "service command");
        check(android->dropIns == QStringList{QStringLiteral("skip-ios.conf")}, "drop-ins");
        const auto enable = enableCommands(*android);
        check(enable.size() == 1 && enable.at(0).value(2) == android->sourcePath,
              "enabling an uninstalled timer links its file");
    }

    const Timer *host = find(snapshot.timers, QStringLiteral("forkmesh-release-host.timer"));
    check(host && host->enabled(), "installed timer on");
    if (host) {
        check(host->nextUsec == 1790983300000000, "next run carried");
        check(host->dropIns == (QStringList{QStringLiteral("skip-ios.conf"),
                                            QStringLiteral("nice.conf")}),
              "several drop-ins");
        check(enableCommands(*host).at(0).value(2) == host->unit, "installed enable by name");
        const auto disable = disableCommands(*host);
        check(disable.at(0) == (QStringList{QStringLiteral("disable"), QStringLiteral("--now"),
                                           host->unit}),
              "disable stops and disables");
        check(runNowArguments(*host)
                  == (QStringList{QStringLiteral("start"), QStringLiteral("--no-block"),
                                  QStringLiteral("forkmesh-release-host.service")}),
              "run now starts the service without waiting");
    }
    const Timer *backup = find(snapshot.timers, QStringLiteral("fmnext-backup.timer"));
    check(backup && backup->serviceResult == QLatin1String("exit-code"), "last result");

    const Snapshot failed = collect(
        [](const QStringList &) {
            CommandResult result;
            result.error = QStringLiteral("Failed to connect to bus");
            return result;
        },
        sources);
    check(failed.error == QLatin1String("Failed to connect to bus"), "collect reports bus error");
}

QPushButton *rowButton(QWidget *panel, const QString &name, const QString &unit)
{
    for (QPushButton *button : panel->findChildren<QPushButton *>(name)) {
        if (button->property("unit").toString() == unit)
            return button;
    }
    return nullptr;
}

QString rowState(QWidget *panel, const QString &unit)
{
    for (QWidget *row : panel->findChildren<QWidget *>(QStringLiteral("timerRow"))) {
        if (row->property("unit").toString() == unit) {
            if (auto *label = row->findChild<QLabel *>(QStringLiteral("timerState")))
                return label->text();
        }
    }
    return QString();
}

void testPanel()
{
    QTemporaryDir directory;
    const QString sources = writeSources(directory);
    FakeSystemd systemd;
    PanelOptions options;
    options.runner = systemd.runner();
    options.unitSourceDirectory = [sources] { return sources; };
    options.nowUsec = [] { return kNow; };
    std::unique_ptr<QWidget> panel(buildSettingsPanel(options));
    auto *status = panel->findChild<QLabel *>(QStringLiteral("timersStatus"));
    check(status && status->text().isEmpty(), "nothing runs before the panel is shown");
    panel->show();
    check(waitFor([&] { return status->text() == QLatin1String("3 timers, 2 on."); }),
          "first show lists the timers");
    check(rowState(panel.get(), QStringLiteral("forkmesh-release-host.timer"))
              == QStringLiteral("On · next run in 5 min · last run 2 min ago"),
          "row state line");
    check(rowState(panel.get(), QStringLiteral("forkmesh-release-android.timer"))
              == QLatin1String("Off"),
          "uninstalled row");
    check(rowState(panel.get(), QStringLiteral("fmnext-backup.timer"))
              .endsWith(QLatin1String("last result: exit-code")),
          "failed result shown");

    QPushButton *toggle =
        rowButton(panel.get(), QStringLiteral("timerToggle"),
                  QStringLiteral("forkmesh-release-host.timer"));
    check(toggle && toggle->text() == QLatin1String("Turn off"), "toggle reads Turn off");
    if (toggle)
        toggle->click();
    check(waitFor([&] {
              return status->text() == QLatin1String("forkmesh-release-host.timer is off.");
          }),
          "turning off reports");
    check(systemd.called({QStringLiteral("disable"), QStringLiteral("--now"),
                          QStringLiteral("forkmesh-release-host.timer")}),
          "disable ran");
    check(waitFor([&] {
              QPushButton *again = rowButton(panel.get(), QStringLiteral("timerToggle"),
                                             QStringLiteral("forkmesh-release-host.timer"));
              return again && again->text() == QLatin1String("Turn on") && again->isEnabled();
          }),
          "row re-rendered as off");
    check(status->text() == QLatin1String("forkmesh-release-host.timer is off."),
          "result stays after the refresh");

    QPushButton *runNow = rowButton(panel.get(), QStringLiteral("timerRunNow"),
                                    QStringLiteral("forkmesh-release-host.timer"));
    if (runNow)
        runNow->click();
    check(waitFor([&] {
              return systemd.called({QStringLiteral("start"), QStringLiteral("--no-block"),
                                     QStringLiteral("forkmesh-release-host.service")});
          }),
          "run now starts the service");
    check(waitFor([&] {
              return status->text() == QLatin1String("forkmesh-release-host.service started.");
          }),
          "run now reports");

    QPushButton *android = nullptr;
    check(waitFor([&] {
              android = rowButton(panel.get(), QStringLiteral("timerToggle"),
                                  QStringLiteral("forkmesh-release-android.timer"));
              return android && android->isEnabled();
          }),
          "idle after run now");
    check(android && android->text() == QLatin1String("Turn on"), "uninstalled can be turned on");
    if (android)
        android->click();
    check(waitFor([&] {
              return systemd.called({QStringLiteral("enable"), QStringLiteral("--now"),
                                     sources + QStringLiteral("/forkmesh-release-android.timer")});
          }),
          "enable links the source file");

    QPushButton *backupRun = rowButton(panel.get(), QStringLiteral("timerRunNow"),
                                       QStringLiteral("fmnext-backup.timer"));
    check(waitFor([&] { return backupRun && backupRun->isEnabled(); }), "idle after enable");

    systemd.releaseHostOn = true;
    systemd.failDisable = true;
    auto *refresh = panel->findChild<QPushButton *>(QStringLiteral("timersRefresh"));
    refresh->click();
    check(waitFor([&] {
              QPushButton *again = rowButton(panel.get(), QStringLiteral("timerToggle"),
                                             QStringLiteral("forkmesh-release-host.timer"));
              return again && again->text() == QLatin1String("Turn off") && again->isEnabled();
          }),
          "refresh picks up the change");
    rowButton(panel.get(), QStringLiteral("timerToggle"),
              QStringLiteral("forkmesh-release-host.timer"))
        ->click();
    check(waitFor([&] {
              return status->text().contains(QLatin1String("Access denied"))
                     && refresh->isEnabled();
          }),
          "failure is reported");
    check(status->styleSheet().contains(QLatin1String("#f85149")), "failure in red");
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    testParsers();
    testCollect();
    testPanel();
    if (failures == 0)
        std::printf("systemd timers tests passed\n");
    return failures == 0 ? 0 : 1;
}
