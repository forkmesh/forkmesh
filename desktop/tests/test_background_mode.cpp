#include "../src/BackgroundMode.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

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

    bg::CommandResult run(const QString &program, const QStringList &arguments)
    {
        const QString call = program + QLatin1Char(' ') + arguments.join(QLatin1Char(' '));
        calls << call;
        bg::CommandResult result;
        result.ok = true;
        if (call == QLatin1String("systemctl --user show-environment"))
            result.ok = systemd;
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
    m.uid = 501;
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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testTexts();
    testSystemd();
    testUserLinger();
    testXdg();
#if defined(Q_OS_LINUX)
    testLegacyEngine();
#endif
    if (g_failures == 0)
        std::printf("background mode tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
