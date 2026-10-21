#include "../src/AppPaths.h"
#include "../src/BackgroundMode.h"
#include "../src/SingleInstance.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <atomic>
#include <cstdio>
#include <thread>

namespace bg = forkmesh::background;

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

struct Recorder {
    QStringList calls;
    bool systemd = true;
    bool linger = false;
    QByteArray systemState = "running\n";
    QByteArray userState = "running\n";
    bool preparingForShutdown = false;
    QByteArray sleepStates = "static\nstatic\nstatic\nstatic\nstatic\n";
    QByteArray unitShow = "UnitFileState=enabled\nActiveState=inactive\nSubState=dead\n"
                          "Result=success\nMainPID=0\nNRestarts=0\nExecMainStatus=0\n";
    QByteArray journal = "2026-10-20T08:00:00+0200 box forkmesh[42]: started\n";

    bg::CommandResult run(const QString &program, const QStringList &arguments)
    {
        const QString call = program + QLatin1Char(' ') + arguments.join(QLatin1Char(' '));
        calls << call;
        bg::CommandResult result;
        result.ok = true;
        if (call == QLatin1String("systemctl --user show-environment"))
            result.ok = systemd;
        if (call == QLatin1String("systemctl is-system-running"))
            result.output = systemState;
        if (call == QLatin1String("systemctl --user is-system-running"))
            result.output = userState;
        if (arguments.value(0) == QLatin1String("is-enabled")) {
            result.output = sleepStates;
            result.ok = !sleepStates.startsWith("masked");
        }
        if (arguments.value(1) == QLatin1String("show"))
            result.output = unitShow;
        if (program == QLatin1String("journalctl"))
            result.output = journal;
        if (program == QLatin1String("busctl"))
            result.output = preparingForShutdown ? "b true\n" : "b false\n";
        if (program == QLatin1String("loginctl")) {
            if (arguments.value(0) == QLatin1String("show-user"))
                result.output = linger ? "yes\n" : "no\n";
            else if (call.contains(QLatin1String("enable-linger")))
                linger = true;
            else if (call.contains(QLatin1String("disable-linger")))
                linger = false;
        }
        return result;
    }

    bool called(const QString &call) const { return calls.contains(call); }
};

bg::StartManager manager(const QString &home, Recorder *recorder)
{
    bg::StartManager m;
    m.home = home;
    m.configHome = home + QStringLiteral("/.config");
    m.polkitRulesHome = home + QStringLiteral("/polkit rules");
    m.uid = 501;
    m.account = QStringLiteral("forkmesh-test");
    m.run = [recorder](const QString &program, const QStringList &arguments) {
        return recorder->run(program, arguments);
    };
    return m;
}

QString fakeBinary(const QString &home)
{
    const QString path = home + QStringLiteral("/bin dir/for%k \"mesh\" $x`y\\z");
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write("#!/bin/sh\n");
    file.close();
    file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
    return path;
}

QString read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

void testTexts()
{
    const QString binary = QStringLiteral("/opt/For Mesh/%u \"x\" $HOME `id` \\n/forkmesh");
    const QString unit = bg::systemdUnit(binary, QStringLiteral("forkmesh"));
    check(unit.contains(QLatin1String("\"--headless\" \"--project\" \"forkmesh\" \"--wait-for-lock\"")),
          "the unit runs the desktop without a window");
    check(unit.contains(QLatin1String("Environment=QT_QPA_PLATFORM=offscreen")),
          "the unit uses the offscreen platform");
    check(unit.contains(QLatin1String("Restart=on-failure")), "the unit restarts after a crash");
    check(unit.contains(QLatin1String("StartLimitIntervalSec=0")) &&
              !unit.contains(QLatin1String("StartLimitBurst")),
          "the unit never gives up restarting");
    check(unit.contains(QLatin1String("RestartSteps=5")) &&
              unit.contains(QLatin1String("RestartMaxDelaySec=300")),
          "restarts back off to at most five minutes apart");
    check(bg::sleepTargets().size() == 5 && bg::sleepTargets().contains(QLatin1String("suspend.target")),
          "every systemd sleep target is listed");
    check(bg::systemSleepArguments(true) ==
              QStringList{QStringLiteral("systemctl"), QStringLiteral("mask")} + bg::sleepTargets(),
          "turning sleep off masks the sleep targets");
    check(bg::systemSleepArguments(false).value(1) == QLatin1String("unmask"),
          "allowing sleep unmasks them");
    check(unit.contains(QLatin1String("WantedBy=default.target")), "the unit is wanted at start");
    check(bg::execStartBinary(unit) == binary, "the unit binary round-trips");
    const QString entry = bg::headlessDesktopEntry(binary, QStringLiteral("work"));
    check(bg::desktopExecBinary(entry) == binary, "the autostart binary round-trips");
    check(entry.contains(QLatin1String(" --headless --project work --wait-for-lock")),
          "the autostart entry runs the desktop without a window");
    const QString tray = bg::trayDesktopEntry(binary, QStringLiteral("work"));
    check(bg::desktopExecBinary(tray) == binary, "the tray binary round-trips");
    check(tray.contains(QLatin1String(" --tray --project work")), "the tray entry starts the tray");
    check(bg::unitName(QStringLiteral("source")) == QLatin1String("forkmesh-headless-source.service"),
          "a project named source cannot collide with forkmesh-source.service");
    const QString logout = bg::logoutUnit(binary, QStringLiteral("work"));
    check(logout.contains(QLatin1String("PartOf=graphical-session.target")) &&
              logout.contains(QLatin1String("After=graphical-session.target")),
          "the log-out unit stops with the graphical session, before its display goes");
    check(logout.contains(QLatin1String("WantedBy=graphical-session.target")),
          "the log-out unit starts with every graphical session");
    check(logout.contains(QLatin1String("Type=oneshot")) &&
              logout.contains(QLatin1String("RemainAfterExit=yes")),
          "the log-out unit stays active until the session ends");
    check(logout.contains(QLatin1String("\"--hand-over-at-logout\" \"--project\" \"work\"")),
          "the log-out unit hands the window over");
    check(bg::execStopBinary(logout) == binary, "the log-out binary round-trips");
    check(bg::execStartBinary(logout).isEmpty(), "the log-out unit starts nothing");
    check(bg::logoutUnitName(QStringLiteral("work")) == QLatin1String("forkmesh-logout-work.service"),
          "the log-out unit is named per project");
}

void testLogout()
{
    QTemporaryDir home;
    Recorder recorder;
    const bg::StartManager m = manager(home.path(), &recorder);
    const QString binary = fakeBinary(home.path());
    const QString project = QStringLiteral("forkmesh");
    check(!m.status(project).afterLogout, "not kept across log-out before it is set up");
    QString error;
    check(m.keepAcrossLogout(binary, project, &error), "keeping it across log-out succeeds");
    check(bg::execStopBinary(read(m.logoutUnitPath(project))) == binary, "the log-out unit is written");
    check(recorder.called(QStringLiteral("systemctl --user enable --now forkmesh-logout-forkmesh.service")),
          "the log-out unit is enabled and active in this session");
    check(recorder.linger && QFileInfo::exists(m.lingerMarkerPath()),
          "lingering is turned on so the user manager outlives the log-out");
    check(m.status(project).afterLogout, "the status says it keeps running after log-out");

    recorder.calls.clear();
    const QString other = home.path() + QStringLiteral("/other/forkmesh");
    QDir().mkpath(QFileInfo(other).absolutePath());
    QFile::copy(binary, other);
    check(m.keepAcrossLogout(other, project, &error), "a second call succeeds");
    check(bg::execStopBinary(read(m.logoutUnitPath(project))) == binary,
          "a working installed binary is kept, so a test build does not take it over");
    check(!recorder.called(QStringLiteral("systemctl --user daemon-reload")),
          "an unchanged unit is not reloaded");

    QFile::remove(binary);
    check(m.keepAcrossLogout(other, project, &error) &&
              bg::execStopBinary(read(m.logoutUnitPath(project))) == other,
          "a missing installed binary is replaced");

    check(m.enable(other, project, &error), "start at boot is enabled too");
    check(m.disable(project, &error), "start at boot is disabled");
    check(recorder.linger, "lingering stays while the log-out unit remains");
    recorder.calls.clear();
    check(m.syncLogoutHandOver(false, other, project, &error), "turning keep running off succeeds");
    check(recorder.called(QStringLiteral("systemctl --user disable --now forkmesh-logout-forkmesh.service")),
          "the log-out unit is disabled");
    check(!QFileInfo::exists(m.logoutUnitPath(project)), "the log-out unit is removed");
    check(!recorder.linger && !QFileInfo::exists(m.lingerMarkerPath()),
          "lingering we turned on is turned off with the last unit");
    recorder.calls.clear();
    m.dropLogoutHandOver(project);
    check(recorder.calls.isEmpty(), "nothing runs when there is nothing to remove");

    Recorder noSystemd;
    noSystemd.systemd = false;
    const bg::StartManager plain = manager(home.path(), &noSystemd);
    check(!plain.keepAcrossLogout(other, project, &error) && !error.isEmpty(),
          "without a user systemd it says it cannot keep running after log-out");
}

void testSleepDisabled()
{
    QTemporaryDir home;
    Recorder recorder;
    const bg::StartManager m = manager(home.path(), &recorder);
    check(!m.sleepDisabled() && !m.status(QStringLiteral("forkmesh")).sleepDisabled,
          "sleep is allowed while the targets are not masked");
    recorder.sleepStates = "masked\nmasked\nmasked\nmasked-runtime\nmasked\n";
    check(m.sleepDisabled() && m.status(QStringLiteral("forkmesh")).sleepDisabled,
          "masked sleep targets mean the computer never sleeps");
    recorder.sleepStates = "masked\nstatic\nmasked\nmasked\nmasked\n";
    check(!m.sleepDisabled(), "one unmasked target still lets the computer sleep");
    recorder.sleepStates = "masked\n";
    check(!m.sleepDisabled(), "a short answer is not trusted");
}

void testBootSleepPermission()
{
    QTemporaryDir home;
    Recorder recorder;
    bg::StartManager m = manager(home.path(), &recorder);
    int prompts = 0;
    bool approve = false;
    m.run = [&](const QString &program, const QStringList &arguments) {
        if (program != QLatin1String("pkexec"))
            return recorder.run(program, arguments);
        ++prompts;
        if (!approve)
            return bg::CommandResult{false, "authentication dismissed"};
        check(arguments.value(4).startsWith(home.path()), "the policy directory is inside the test home");
        check(arguments.value(5).startsWith(home.path()), "the policy file is inside the test home");
        if (!arguments.value(4).startsWith(home.path()) || !arguments.value(5).startsWith(home.path()))
            return bg::CommandResult{};
        QProcess process;
        process.start(arguments.first(), arguments.mid(1));
        const bool finished = process.waitForFinished(5000);
        return bg::CommandResult{finished && process.exitCode() == 0, process.readAllStandardError()};
    };
    check(!m.bootSleepProtected(), "an absent permission does not claim boot sleep protection");
    QString error;
    check(!m.ensureBootSleepPermission(&error) && error.contains(QLatin1String("authentication")),
          "dismissed authentication reports that pre-login sleep remains possible");
    check(!m.bootSleepProtected(), "dismissed authentication installs no policy");
    approve = true;
    check(m.ensureBootSleepPermission(), "approved authentication installs the boot permission");
    check(m.bootSleepProtected() && m.status(QStringLiteral("forkmesh")).bootSleepProtected,
          "the installed policy is detected across manager instances");
    const bg::StartManager restarted = manager(home.path(), &recorder);
    check(restarted.bootSleepProtected(), "boot protection survives a restart");
    check(m.ensureBootSleepPermission() && prompts == 2, "an installed policy never prompts again");
    check(read(m.bootSleepPolicyPath()) == m.bootSleepPolicy(), "the policy text is installed unchanged");
    check(m.bootSleepPolicy().contains(QLatin1String("subject.user === \"forkmesh-test\"")) &&
              m.bootSleepPolicy().count(QLatin1String("action.id")) == 1 &&
              m.bootSleepPolicy().contains(QLatin1String("org.freedesktop.login1.inhibit-block-sleep")),
          "only the configured account may acquire the sleep block permission");
    m.uid = 502;
    check(!m.bootSleepProtected(), "the permission does not apply to another account");
    recorder.sleepStates = "masked\nmasked\nmasked\nmasked\nmasked\n";
    check(m.ensureBootSleepPermission() && prompts == 2,
          "an already sleep-disabled computer needs no extra permission");
    recorder.sleepStates = "static\nstatic\nstatic\nstatic\nstatic\n";
    m.account.clear();
    check(!m.bootSleepProtected() && !m.ensureBootSleepPermission() && prompts == 2,
          "an unresolved account cannot install a permission");
}

void testRefreshUnit()
{
    QTemporaryDir home;
    Recorder recorder;
    const bg::StartManager m = manager(home.path(), &recorder);
    const QString binary = fakeBinary(home.path());
    const QString project = QStringLiteral("forkmesh");
    check(!m.refreshUnit(binary, project), "nothing is written while start at boot is off");
    check(!QFileInfo::exists(m.systemdUnitPath(project)), "refreshing does not install the unit");
    QString error;
    check(m.enable(binary, project, &error), "enable succeeds");
    QFile old(m.systemdUnitPath(project));
    old.open(QIODevice::WriteOnly);
    old.write(bg::systemdUnit(binary, project)
                  .replace(QLatin1String("StartLimitIntervalSec=0"),
                           QLatin1String("StartLimitIntervalSec=600\nStartLimitBurst=5"))
                  .toUtf8());
    old.close();
    const QString other = home.path() + QStringLiteral("/other/forkmesh");
    QDir().mkpath(QFileInfo(other).absolutePath());
    QFile::copy(binary, other);
    recorder.calls.clear();
    check(m.refreshUnit(other, project), "an old unit is rewritten");
    check(read(m.systemdUnitPath(project)) == bg::systemdUnit(binary, project),
          "the rewritten unit keeps its working binary");
    check(recorder.called(QStringLiteral("systemctl --user daemon-reload")),
          "systemd reloads the rewritten unit");
    recorder.calls.clear();
    check(!m.refreshUnit(other, project) && recorder.calls.isEmpty(),
          "a current unit is left alone");
}

void testShuttingDown()
{
    QTemporaryDir home;
    Recorder recorder;
    const bg::StartManager m = manager(home.path(), &recorder);
    check(!m.shuttingDown(), "a log-out is not a shutdown");
    recorder.systemState = "stopping\n";
    check(m.shuttingDown(), "a stopping system is a shutdown");
    recorder.systemState = "running\n";
    recorder.userState = "stopping\n";
    check(m.shuttingDown(), "a stopping user manager is a shutdown");
    recorder.userState = "running\n";
    recorder.preparingForShutdown = true;
    check(m.shuttingDown(), "logind preparing a shutdown is a shutdown");
}

void testSystemd()
{
    QTemporaryDir home;
    Recorder recorder;
    const bg::StartManager m = manager(home.path(), &recorder);
    const QString binary = fakeBinary(home.path());
    check(!m.status(QStringLiteral("forkmesh")).enabled, "off before enabling");
    QString error;
    check(m.enable(binary, QStringLiteral("forkmesh"), &error), "enable succeeds");
    check(recorder.calls.filter(QStringLiteral("pkexec ")).size() == 1,
          "enabling boot startup also requests the missing sleep permission");
    const bg::StartStatus status = m.status(QStringLiteral("forkmesh"));
    check(status.enabled && status.binary == binary && status.atBoot, "enabled at boot with the binary");
    check(recorder.called(QStringLiteral("systemctl --user enable forkmesh-headless-forkmesh.service")),
          "the unit is enabled");
    check(recorder.called(QStringLiteral("loginctl --no-ask-password enable-linger 501")),
          "lingering is turned on for a start before log-in");
    check(QFileInfo::exists(m.lingerMarkerPath()), "the linger marker records who turned it on");
    check(QFileInfo::exists(m.trayEntryPath(QStringLiteral("forkmesh"))), "the tray starts at log-in");
    check(m.enable(binary, QStringLiteral("work"), &error), "a second project enables beside the first");
    check(m.disable(QStringLiteral("forkmesh"), &error), "disable succeeds");
    check(recorder.linger, "lingering stays while another project's unit remains");
    check(m.disable(QStringLiteral("work"), &error), "the second project disables");
    check(!recorder.linger && !QFileInfo::exists(m.lingerMarkerPath()),
          "lingering we turned on is turned off with the last unit");
    check(!QFileInfo::exists(m.systemdUnitPath(QStringLiteral("forkmesh"))) &&
              !QFileInfo::exists(m.trayEntryPath(QStringLiteral("forkmesh"))),
          "disable removes what enable wrote");
    check(!m.enable(QStringLiteral("relative/forkmesh"), QStringLiteral("forkmesh"), &error),
          "a relative binary is refused");
    check(!m.enable(binary, QStringLiteral("../x"), &error), "an invalid project is refused");
}

void testDiagnostics()
{
    QTemporaryDir home;
    Recorder recorder;
    const bg::StartManager m = manager(home.path(), &recorder);
    const QString project = QStringLiteral("forkmesh");
    check(!bg::runnerBadge(m.diagnose(project)).visible, "no badge while nothing is registered");
    check(!bg::canCloseToHeadless(m.diagnose(project)),
          "no close-to-background offer while nothing is registered");
    check(!bg::runnerReport(m.diagnose(project)).isEmpty(), "an unregistered report still explains");
    QString error;
    check(m.enable(fakeBinary(home.path()), project, &error), "enable succeeds");

    bg::RunnerDiagnostics ready = m.diagnose(project);
    check(ready.unit == QStringLiteral("forkmesh-headless-forkmesh.service") &&
              ready.systemdReachable && ready.unitFileState == QStringLiteral("enabled") &&
              ready.activeState == QStringLiteral("inactive") && ready.binaryExecutable,
          "diagnose reads the unit state");
    check(ready.journal.isEmpty() && !recorder.calls.filter(QStringLiteral("journalctl")).size(),
          "the journal is read only when asked");
    bg::RunnerBadge badge = bg::runnerBadge(ready);
    check(badge.visible && badge.text == QStringLiteral("Background: Ready") &&
              badge.severity == QStringLiteral("ok"),
          "an enabled unit standing by is ready");
    check(bg::canCloseToHeadless(ready), "a ready unit offers to close the window into it");

    recorder.unitShow = "UnitFileState=enabled\nActiveState=active\nSubState=running\n"
                        "MainPID=4242\nNRestarts=2\n";
    const bg::RunnerDiagnostics running = m.diagnose(project);
    badge = bg::runnerBadge(running);
    check(running.mainPid == 4242 && running.restarts == 2 &&
              badge.text == QStringLiteral("Background: Running") &&
              badge.toolTip.contains(QStringLiteral("4242")),
          "an active unit shows as running with its pid");
    check(!bg::canCloseToHeadless(running), "a running unit does not offer a second hand-over");

    recorder.unitShow = "UnitFileState=enabled\nActiveState=failed\nSubState=failed\n"
                        "Result=exit-code\nExecMainStatus=3\n";
    badge = bg::runnerBadge(m.diagnose(project));
    check(badge.text == QStringLiteral("Background: Failed") &&
              badge.severity == QStringLiteral("error") &&
              badge.toolTip.contains(QStringLiteral("exit-code")),
          "a failed unit is an error");

    recorder.unitShow = "UnitFileState=enabled\nActiveState=activating\nSubState=auto-restart\n";
    check(bg::runnerBadge(m.diagnose(project)).severity == QStringLiteral("warn"),
          "a restarting unit is a warning");

    recorder.unitShow = "UnitFileState=disabled\nActiveState=inactive\n";
    check(bg::runnerBadge(m.diagnose(project)).text == QStringLiteral("Background: Not enabled"),
          "a unit file systemd does not start is reported");

    recorder.unitShow = "UnitFileState=enabled\nActiveState=inactive\n";
    recorder.linger = false;
    check(bg::runnerBadge(m.diagnose(project)).text == QStringLiteral("Background: At log-in"),
          "without lingering it starts only at log-in");
    recorder.linger = true;

    bg::RunnerDiagnostics full = m.diagnose(project, 0, true);
    full.keepRunning = true;
    const QString report = bg::runnerReport(full);
    check(full.journal.contains(QStringLiteral("started")) &&
              report.contains(QStringLiteral("journalctl --user-unit=forkmesh-headless-forkmesh.service")) &&
              report.contains(QStringLiteral("started")) &&
              report.contains(QStringLiteral("Keep running after the window closes: yes")),
          "the report carries the state and the recent log");

    QFile::remove(ready.start.binary);
    check(bg::runnerBadge(m.diagnose(project)).text == QStringLiteral("Background: Broken"),
          "a unit whose program is gone is broken");

    recorder.calls.clear();
    check(m.turnOff(project, &error), "turn off succeeds");
    check(recorder.called(QStringLiteral("systemctl --user stop --no-block forkmesh-headless-forkmesh.service")) &&
              recorder.called(QStringLiteral("systemctl --user reset-failed forkmesh-headless-forkmesh.service")) &&
              recorder.called(QStringLiteral("systemctl --user disable forkmesh-headless-forkmesh.service")),
          "turning off stops, clears and disables the unit");
    check(!QFileInfo::exists(m.systemdUnitPath(project)) &&
              !bg::runnerBadge(m.diagnose(project)).visible,
          "the badge goes away once it is turned off");
    check(!bg::headlessProcessRunning(0) && !bg::headlessProcessRunning(QCoreApplication::applicationPid()),
          "only a --headless process counts as the windowless instance");
}

void testUserLinger()
{
    QTemporaryDir home;
    Recorder recorder;
    recorder.linger = true;
    const bg::StartManager m = manager(home.path(), &recorder);
    QString error;
    check(m.enable(fakeBinary(home.path()), QStringLiteral("forkmesh"), &error), "enable succeeds");
    check(!QFileInfo::exists(m.lingerMarkerPath()), "lingering the user set is not claimed");
    check(m.disable(QStringLiteral("forkmesh"), &error), "disable succeeds");
    check(recorder.linger, "lingering the user set stays on");
}

void testXdg()
{
    QTemporaryDir home;
    Recorder recorder;
    recorder.systemd = false;
    const bg::StartManager m = manager(home.path(), &recorder);
    const QString binary = fakeBinary(home.path());
    QString error;
    check(m.enable(binary, QStringLiteral("forkmesh"), &error), "enable without systemd succeeds");
    const bg::StartStatus status = m.status(QStringLiteral("forkmesh"));
    check(status.enabled && !status.atBoot && status.binary == binary &&
              status.location == m.headlessEntryPath(QStringLiteral("forkmesh")),
          "without systemd it starts at log-in");
    check(m.disable(QStringLiteral("forkmesh"), &error) &&
              !QFileInfo::exists(m.headlessEntryPath(QStringLiteral("forkmesh"))),
          "disable removes the entry");
}

void testLegacyEngine()
{
    QTemporaryDir home;
    Recorder recorder;
    const bg::StartManager m = manager(home.path(), &recorder);
    const QString units = home.path() + QStringLiteral("/.config/systemd/user");
    QDir().mkpath(units);
    for (const QString &name : {QStringLiteral("forkmesh-core-forkmesh.service"),
                                QStringLiteral("forkmesh-core.linger"),
                                QStringLiteral("forkmesh-source.service")}) {
        QFile file(units + QLatin1Char('/') + name);
        file.open(QIODevice::WriteOnly);
    }
    const bg::LegacyEngine legacy = m.removeLegacyEngine(QStringLiteral("forkmesh"));
    check(legacy.found && legacy.startedAtLogin, "the Go engine's unit is found");
    check(recorder.called(QStringLiteral(
              "systemctl --user disable --now forkmesh-core-forkmesh.service")),
          "the Go engine is stopped and disabled");
    check(!QFileInfo::exists(units + QStringLiteral("/forkmesh-core-forkmesh.service")),
          "the Go engine's unit is removed");
    check(QFileInfo::exists(m.lingerMarkerPath()), "the linger marker carries over");
    check(QFileInfo::exists(units + QStringLiteral("/forkmesh-source.service")),
          "other units are left alone");
    check(!m.removeLegacyEngine(QStringLiteral("forkmesh")).found, "a second pass finds nothing");
}

bool askWindowFromAnotherThread()
{
    std::atomic_bool done{false};
    bool answer = false;
    std::thread client([&] {
        answer = forkmesh::askWindowToHandOver();
        done = true;
    });
    QElapsedTimer waited;
    waited.start();
    while (!done && waited.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    client.join();
    QCoreApplication::processEvents();
    return answer;
}

void testHandOverMessage()
{
    QTemporaryDir root;
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (QDir::tempPath().size() > 60 && QDir::isAbsolutePath(runtime) && QFileInfo(runtime).isDir())
        qputenv("TMPDIR", QFile::encodeName(runtime));
    for (const char *name : {"FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT", "FORKMESH_PROJECT_DIR",
                             "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR"})
        qunsetenv(name);
    qputenv("HOME", QFile::encodeName(root.path()));
    qputenv("XDG_DATA_HOME", QFile::encodeName(root.path() + QStringLiteral("/xdg-data")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(root.path() + QStringLiteral("/xdg-config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(root.path() + QStringLiteral("/xdg-cache")));
    qputenv("FORKMESH_HOME", QFile::encodeName(root.path() + QStringLiteral("/forkmesh")));
    forkmesh::paths::activate({QStringLiteral("forkmesh-background-mode-tests"),
                               QStringLiteral("--project"), QStringLiteral("handover")},
                              false);
    if (!forkmesh::paths::stateRoot().startsWith(root.path())) {
        check(false, "the test profile is inside its temporary folder");
        return;
    }
    check(forkmesh::singleInstanceHolderPid() == 0, "nobody holds the lock yet");
    check(!forkmesh::askWindowToHandOver(), "with nothing running nobody answers");
    check(forkmesh::acquireSingleInstance(), "the window takes the lock");
    check(forkmesh::singleInstanceHolderPid() == QCoreApplication::applicationPid(),
          "the lock names the window's process");
    check(!askWindowFromAnotherThread(), "a window without a hand-over handler does not answer");
    bool handedOver = false;
    forkmesh::onSingleInstanceHandOver([&handedOver] { handedOver = true; });
    check(askWindowFromAnotherThread(), "a window answers hand-over with yield");
    check(handedOver, "the window's hand-over handler runs");
    handedOver = false;
    forkmesh::setBackgroundInstance();
    check(!askWindowFromAnotherThread() && !handedOver,
          "a windowless instance ignores hand-over");
    Recorder recorder;
    const bg::StartManager m = manager(root.path(), &recorder);
    check(m.enable(fakeBinary(root.path()), QStringLiteral("headless")) &&
              recorder.calls.filter(QStringLiteral("pkexec ")).isEmpty(),
          "a headless instance never opens administrator authentication");
    forkmesh::releaseSingleInstance();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testTexts();
    testSystemd();
    testDiagnostics();
    testUserLinger();
    testXdg();
    testLogout();
    testShuttingDown();
    testSleepDisabled();
    testBootSleepPermission();
    testRefreshUnit();
#if defined(Q_OS_LINUX)
    testLegacyEngine();
    testHandOverMessage();
#endif
    if (g_failures == 0)
        std::printf("background mode tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
