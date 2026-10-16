#include "BackgroundMode.h"

#include "AppPaths.h"
#include "ProcessReaper.h"
#include "SingleInstance.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <atomic>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace forkmesh::background {

namespace {

constexpr char kLegacyLabelPrefix[] = "forkmesh-core-";
constexpr char kLegacyLingerMarker[] = "forkmesh-core.linger";
constexpr char kLegacyEngineName[] = "forkmesh-core";

std::atomic_bool g_stopSignal{false};

bool writeText(const QString &path, const QString &content, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("could not create %1").arg(QFileInfo(path).absolutePath());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content.toUtf8()) < 0 || !file.commit()) {
        if (error)
            *error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    return true;
}

bool removeFile(const QString &path, QString *error = nullptr)
{
    if (!QFileInfo::exists(path) || QFile::remove(path))
        return true;
    if (error && error->isEmpty())
        *error = QStringLiteral("could not remove %1").arg(path);
    return false;
}

QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString systemdQuote(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('%'), QStringLiteral("%%"));
    value.replace(QLatin1Char('$'), QStringLiteral("$$"));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

QString desktopQuote(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\\\\\"));
    for (const QChar special : {QLatin1Char('"'), QLatin1Char('`'), QLatin1Char('$')})
        value.replace(special, QStringLiteral("\\\\") + special);
    value.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

bool validBinary(const QString &binary)
{
    if (!QDir::isAbsolutePath(binary) || binary.contains(QLatin1Char('\n')) ||
        binary.contains(QLatin1Char('\r')) || binary.contains(QChar(0)))
        return false;
    const QFileInfo info(binary);
    return info.isFile() && info.isExecutable();
}

bool legacyEngineProcess(qint64 pid)
{
#if defined(Q_OS_LINUX)
    QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
    return pid > 1 && comm.open(QIODevice::ReadOnly) &&
           comm.readAll().trimmed() == QByteArray(kLegacyEngineName);
#elif defined(Q_OS_MACOS)
    if (pid <= 1)
        return false;
    QProcess ps;
    ps.start(QStringLiteral("ps"), {QStringLiteral("-p"), QString::number(pid), QStringLiteral("-o"),
                                    QStringLiteral("comm=")});
    return ps.waitForFinished(3000) &&
           QFileInfo(QString::fromUtf8(ps.readAllStandardOutput().trimmed())).fileName() ==
               QLatin1String(kLegacyEngineName);
#else
    Q_UNUSED(pid);
    return false;
#endif
}

QProcessEnvironment launchEnvironment()
{
    QProcessEnvironment environment = processreaper::releasedEnvironment();
    for (const QString &name : serviceVariables())
        environment.remove(name);
    return environment;
}

bool startDetached(const QString &program, const QStringList &arguments)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(QDir::homePath());
    process.setProcessEnvironment(launchEnvironment());
    return process.startDetached();
}

bool startInOwnScope(const QString &scope, const QString &program, const QStringList &arguments)
{
#if defined(Q_OS_LINUX)
    const bool underService = !qEnvironmentVariableIsEmpty("INVOCATION_ID");
    const QString systemdRun =
        underService ? QStandardPaths::findExecutable(QStringLiteral("systemd-run")) : QString();
    if (!systemdRun.isEmpty() &&
        startDetached(systemdRun, QStringList{QStringLiteral("--user"), QStringLiteral("--scope"),
                                              QStringLiteral("--quiet"), QStringLiteral("--collect"),
                                              QStringLiteral("--unit=") + scope,
                                              QStringLiteral("--"), program} +
                                      arguments))
        return true;
#else
    Q_UNUSED(scope);
#endif
    return startDetached(program, arguments);
}

bool displayPlatform()
{
    const QString platform = QGuiApplication::platformName();
    return !platform.isEmpty() && platform != QLatin1String("offscreen") &&
           platform != QLatin1String("minimal");
}

} // namespace

QString label(const QString &project)
{
    return QStringLiteral("forkmesh-headless-") + project;
}

QString unitName(const QString &project)
{
    return label(project) + QStringLiteral(".service");
}

QString trayLabel(const QString &project)
{
    return QStringLiteral("forkmesh-tray-") + project;
}

QStringList headlessArguments(const QString &project)
{
    return {QStringLiteral("--headless"), QStringLiteral("--project"), project,
            QStringLiteral("--wait-for-lock")};
}

QStringList trayArguments(const QString &project)
{
    return {QStringLiteral("--tray"), QStringLiteral("--project"), project};
}

QStringList serviceVariables()
{
    return {QStringLiteral("INVOCATION_ID"),    QStringLiteral("JOURNAL_STREAM"),
            QStringLiteral("SYSTEMD_EXEC_PID"), QStringLiteral("MANAGERPID"),
            QStringLiteral("NOTIFY_SOCKET"),    QStringLiteral("LISTEN_PID"),
            QStringLiteral("LISTEN_FDS"),       QStringLiteral("LISTEN_FDNAMES"),
            QStringLiteral("WATCHDOG_PID"),     QStringLiteral("WATCHDOG_USEC")};
}

QString desktopBinaryPath()
{
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty() && QFileInfo(appImage).isFile())
        return appImage;
    return QCoreApplication::applicationFilePath();
}

QString systemdUnit(const QString &binary, const QString &project)
{
    QStringList command{systemdQuote(binary)};
    for (const QString &argument : headlessArguments(project))
        command << systemdQuote(argument);
    return QStringList{
               QStringLiteral("[Unit]"),
               QStringLiteral("Description=ForkMesh without a window (%1)").arg(project),
               QStringLiteral("After=network-online.target"),
               QStringLiteral("Wants=network-online.target"),
               QStringLiteral("StartLimitIntervalSec=600"),
               QStringLiteral("StartLimitBurst=5"),
               QString(),
               QStringLiteral("[Service]"),
               QStringLiteral("Type=simple"),
               QStringLiteral("ExecStart=") + command.join(QLatin1Char(' ')),
               QStringLiteral("Environment=QT_QPA_PLATFORM=offscreen FORKMESH_NO_SPLASH=1"),
               QStringLiteral("UnsetEnvironment=FORKMESH_HOME FORKMESH_PROJECTS_DIR "
                              "FORKMESH_PROJECT FORKMESH_PROJECT_DIR"),
               QStringLiteral("Restart=on-failure"),
               QStringLiteral("RestartSec=10"),
               QStringLiteral("KillMode=mixed"),
               QStringLiteral("TimeoutStopSec=30"),
               QString(),
               QStringLiteral("[Install]"),
               QStringLiteral("WantedBy=default.target"),
               QString(),
           }
        .join(QLatin1Char('\n'));
}

QString headlessDesktopEntry(const QString &binary, const QString &project)
{
    return QStringList{
               QStringLiteral("[Desktop Entry]"),
               QStringLiteral("Type=Application"),
               QStringLiteral("Name=ForkMesh without a window (%1)").arg(project),
               QStringLiteral("Exec=") + desktopQuote(binary) + QLatin1Char(' ') +
                   headlessArguments(project).join(QLatin1Char(' ')),
               QStringLiteral("NoDisplay=true"),
               QStringLiteral("Terminal=false"),
               QStringLiteral("X-GNOME-Autostart-enabled=true"),
               QStringLiteral("X-GNOME-Autostart-Delay=5"),
               QString(),
           }
        .join(QLatin1Char('\n'));
}

QString trayDesktopEntry(const QString &binary, const QString &project)
{
    return QStringList{
               QStringLiteral("[Desktop Entry]"),
               QStringLiteral("Type=Application"),
               QStringLiteral("Name=ForkMesh tray (%1)").arg(project),
               QStringLiteral("Exec=") + desktopQuote(binary) + QLatin1Char(' ') +
                   trayArguments(project).join(QLatin1Char(' ')),
               QStringLiteral("Icon=forkmesh"),
               QStringLiteral("NoDisplay=true"),
               QStringLiteral("Terminal=false"),
               QStringLiteral("X-GNOME-Autostart-enabled=true"),
               QStringLiteral("X-GNOME-Autostart-Delay=10"),
               QString(),
           }
        .join(QLatin1Char('\n'));
}

QString execStartBinary(const QString &unit)
{
    for (const QString &line : unit.split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1String("ExecStart=")))
            continue;
        const QString rest = line.mid(10).trimmed();
        if (!rest.startsWith(QLatin1Char('"')))
            return rest.section(QLatin1Char(' '), 0, 0);
        QString out;
        for (int i = 1; i < rest.size(); ++i) {
            const QChar c = rest.at(i);
            const QChar next = i + 1 < rest.size() ? rest.at(i + 1) : QChar();
            if (c == QLatin1Char('\\') && !next.isNull()) {
                out += next;
                ++i;
            } else if ((c == QLatin1Char('%') || c == QLatin1Char('$')) && next == c) {
                out += c;
                ++i;
            } else if (c == QLatin1Char('"')) {
                return out;
            } else {
                out += c;
            }
        }
        return out;
    }
    return {};
}

QString desktopExecBinary(const QString &entry)
{
    for (const QString &line : entry.split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1String("Exec=")))
            continue;
        const QString rest = line.mid(5);
        if (!rest.startsWith(QLatin1Char('"')))
            return rest.section(QLatin1Char(' '), 0, 0);
        QString out;
        for (int i = 1; i < rest.size(); ++i) {
            const QStringView tail = QStringView(rest).mid(i);
            if (tail.startsWith(QLatin1String("\\\\\\\\"))) {
                out += QLatin1Char('\\');
                i += 3;
            } else if (tail.startsWith(QLatin1String("\\\\")) && i + 2 < rest.size()) {
                out += rest.at(i + 2);
                i += 2;
            } else if (tail.startsWith(QLatin1String("%%"))) {
                out += QLatin1Char('%');
                ++i;
            } else if (rest.at(i) == QLatin1Char('"')) {
                return out;
            } else {
                out += rest.at(i);
            }
        }
        return out;
    }
    return {};
}

StartManager StartManager::system()
{
    StartManager manager;
    manager.home = QDir::homePath();
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    manager.configHome =
        QDir::isAbsolutePath(xdg) ? xdg : manager.home + QStringLiteral("/.config");
#ifdef Q_OS_UNIX
    manager.uid = qint64(::getuid());
#endif
    manager.run = [](const QString &program, const QStringList &arguments) {
        CommandResult result;
        if (QStandardPaths::findExecutable(program).isEmpty())
            return result;
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(program, arguments);
        if (!process.waitForFinished(15000)) {
            process.kill();
            process.waitForFinished(1000);
            return result;
        }
        result.output = process.readAll();
        result.ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        return result;
    };
    return manager;
}

CommandResult StartManager::call(const QString &program, const QStringList &arguments) const
{
    return run ? run(program, arguments) : CommandResult{};
}

QString StartManager::systemdUnitPath(const QString &project) const
{
    return configHome + QStringLiteral("/systemd/user/") + unitName(project);
}

QString StartManager::headlessEntryPath(const QString &project) const
{
    return configHome + QStringLiteral("/autostart/") + label(project) + QStringLiteral(".desktop");
}

QString StartManager::trayEntryPath(const QString &project) const
{
    return configHome + QStringLiteral("/autostart/") + trayLabel(project) +
           QStringLiteral(".desktop");
}

QString StartManager::lingerMarkerPath() const
{
    return configHome + QStringLiteral("/systemd/user/forkmesh-headless.linger");
}

bool StartManager::hasSystemd() const
{
    return call(QStringLiteral("systemctl"), {QStringLiteral("--user"),
                                              QStringLiteral("show-environment")})
        .ok;
}

bool StartManager::lingering() const
{
    const CommandResult result =
        call(QStringLiteral("loginctl"), {QStringLiteral("show-user"), QString::number(uid),
                                          QStringLiteral("--property=Linger"),
                                          QStringLiteral("--value")});
    return result.ok && result.output.trimmed() == "yes";
}

void StartManager::enableLinger() const
{
    if (lingering())
        return;
    if (!call(QStringLiteral("loginctl"), {QStringLiteral("--no-ask-password"),
                                           QStringLiteral("enable-linger"), QString::number(uid)})
             .ok)
        return;
    writeText(lingerMarkerPath(), QString(), nullptr);
}

void StartManager::releaseLinger() const
{
    if (!QFileInfo::exists(lingerMarkerPath()))
        return;
    const QStringList units = QDir(QFileInfo(lingerMarkerPath()).absolutePath())
                                  .entryList({QStringLiteral("forkmesh-headless-*.service")},
                                             QDir::Files);
    if (!units.isEmpty())
        return;
    if (!call(QStringLiteral("loginctl"), {QStringLiteral("--no-ask-password"),
                                           QStringLiteral("disable-linger"), QString::number(uid)})
             .ok)
        return;
    removeFile(lingerMarkerPath());
}

bool StartManager::unitInstalled(const QString &project) const
{
    return QFileInfo::exists(systemdUnitPath(project));
}

StartStatus StartManager::status(const QString &project) const
{
    StartStatus status;
    const QString unit = systemdUnitPath(project);
    if (QFileInfo::exists(unit)) {
        status.enabled = true;
        status.mechanism = QStringLiteral("systemd user unit");
        status.location = unit;
        status.binary = execStartBinary(readText(unit));
        status.atBoot = lingering();
        return status;
    }
    const QString entry = headlessEntryPath(project);
    if (QFileInfo::exists(entry)) {
        status.enabled = true;
        status.mechanism = QStringLiteral("XDG autostart entry");
        status.location = entry;
        status.binary = desktopExecBinary(readText(entry));
        return status;
    }
    if (hasSystemd()) {
        status.mechanism = QStringLiteral("systemd user unit");
        status.location = unit;
    } else {
        status.mechanism = QStringLiteral("XDG autostart entry");
        status.location = entry;
    }
    return status;
}

bool StartManager::enable(const QString &binary, const QString &project, QString *error) const
{
    if (!paths::isValidSlug(project)) {
        if (error)
            *error = QStringLiteral("invalid project name %1").arg(project);
        return false;
    }
    if (!validBinary(binary)) {
        if (error)
            *error = QStringLiteral("%1 is not an executable file").arg(binary);
        return false;
    }
    if (hasSystemd()) {
        if (!writeText(systemdUnitPath(project), systemdUnit(binary, project), error))
            return false;
        removeFile(headlessEntryPath(project));
        if (!call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")}).ok) {
            if (error)
                *error = QStringLiteral("systemctl --user daemon-reload failed");
            return false;
        }
        const CommandResult enabled = call(
            QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("enable"), unitName(project)});
        if (!enabled.ok) {
            if (error)
                *error = QStringLiteral("systemctl --user enable %1 failed: %2")
                             .arg(unitName(project), QString::fromUtf8(enabled.output).trimmed());
            return false;
        }
        enableLinger();
    } else if (!writeText(headlessEntryPath(project), headlessDesktopEntry(binary, project), error)) {
        return false;
    }
    return writeText(trayEntryPath(project), trayDesktopEntry(binary, project), error);
}

bool StartManager::disable(const QString &project, QString *error) const
{
    if (!paths::isValidSlug(project)) {
        if (error)
            *error = QStringLiteral("invalid project name %1").arg(project);
        return false;
    }
    bool ok = true;
    const QString unit = systemdUnitPath(project);
    if (QFileInfo::exists(unit)) {
        call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("disable"), unitName(project)});
        ok = removeFile(unit, error) && ok;
        call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")});
    }
    releaseLinger();
    ok = removeFile(headlessEntryPath(project), error) && ok;
    ok = removeFile(trayEntryPath(project), error) && ok;
    return ok;
}

LegacyEngine StartManager::removeLegacyEngine(const QString &project) const
{
    LegacyEngine legacy;
    if (!paths::isValidSlug(project))
        return legacy;
    const QString legacyLabel = QLatin1String(kLegacyLabelPrefix) + project;
#if defined(Q_OS_MACOS)
    const QString plist = home + QStringLiteral("/Library/LaunchAgents/com.forkmesh.core.") + project +
                          QStringLiteral(".plist");
    if (QFileInfo::exists(plist)) {
        legacy.found = legacy.startedAtLogin = true;
        call(QStringLiteral("launchctl"),
             {QStringLiteral("bootout"),
              QStringLiteral("gui/%1/com.forkmesh.core.%2").arg(uid).arg(project)});
        removeFile(plist);
        legacy.steps << QStringLiteral("removed the login item %1").arg(plist);
    }
#elif defined(Q_OS_WIN)
    const QString runKey = QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    if (call(QStringLiteral("reg"), {QStringLiteral("query"), runKey, QStringLiteral("/v"), legacyLabel}).ok) {
        legacy.found = legacy.startedAtLogin = true;
        call(QStringLiteral("reg"), {QStringLiteral("delete"), runKey, QStringLiteral("/v"), legacyLabel,
                                     QStringLiteral("/f")});
        legacy.steps << QStringLiteral("removed the Run entry %1").arg(legacyLabel);
    }
#else
    const QString unit = configHome + QStringLiteral("/systemd/user/") + legacyLabel +
                         QStringLiteral(".service");
    if (QFileInfo::exists(unit)) {
        legacy.found = legacy.startedAtLogin = true;
        call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("disable"),
                                           QStringLiteral("--now"), legacyLabel + QStringLiteral(".service")});
        removeFile(unit);
        call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")});
        legacy.steps << QStringLiteral("stopped and removed %1").arg(unit);
    }
    const QString entry = configHome + QStringLiteral("/autostart/") + legacyLabel +
                          QStringLiteral(".desktop");
    if (QFileInfo::exists(entry)) {
        legacy.found = legacy.startedAtLogin = true;
        removeFile(entry);
        legacy.steps << QStringLiteral("removed %1").arg(entry);
    }
    const QString legacyMarker =
        configHome + QStringLiteral("/systemd/user/") + QLatin1String(kLegacyLingerMarker);
    if (QFileInfo::exists(legacyMarker)) {
        if (!QFileInfo::exists(lingerMarkerPath()))
            QFile::rename(legacyMarker, lingerMarkerPath());
        else
            removeFile(legacyMarker);
    }
#endif
    return legacy;
}

bool supported()
{
#if defined(Q_OS_LINUX)
    return paths::isDefaultStateRoot() && paths::isValidSlug(paths::projectSlug());
#else
    return false;
#endif
}

bool keepRunning()
{
    return QSettings().value(QLatin1String(kKeepRunningSetting), false).toBool();
}

void setKeepRunning(bool on)
{
    QSettings().setValue(QLatin1String(kKeepRunningSetting), on);
}

void noteStopSignal()
{
    g_stopSignal = true;
}

bool startHeadless()
{
    const QString project = paths::projectSlug();
    const StartManager manager = StartManager::system();
    if (manager.unitInstalled(project) &&
        manager.run(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("start"),
                                                  QStringLiteral("--no-block"), unitName(project)})
            .ok)
        return true;
    return startInOwnScope(label(project), desktopBinaryPath(), headlessArguments(project));
}

bool launchTray()
{
    if (!supported() || !displayPlatform())
        return false;
    const QString project = paths::projectSlug();
    return startInOwnScope(trayLabel(project), desktopBinaryPath(), trayArguments(project));
}

bool handOverAfterWindowClose(bool restarting)
{
    if (restarting || g_stopSignal || !supported() || !keepRunning() || isBackgroundInstance() ||
        !singleInstanceHeld() || !displayPlatform())
        return false;
    if (!startHeadless()) {
        qWarning().noquote() << "ForkMesh closed, but could not start it without a window.";
        return false;
    }
    qInfo().noquote() << "ForkMesh keeps running without a window for phones and agents.";
    launchTray();
    return true;
}

void migrateLegacyEngine()
{
    const QString project = paths::projectSlug();
    if (isBackgroundInstance() || !paths::isDefaultStateRoot() || !paths::isValidSlug(project))
        return;
    const StartManager manager = StartManager::system();
    LegacyEngine legacy = manager.removeLegacyEngine(project);
    QFile endpoint(paths::stateRoot() + QStringLiteral("/core/endpoint.json"));
    if (endpoint.open(QIODevice::ReadOnly)) {
        const qint64 pid = QJsonDocument::fromJson(endpoint.readAll())
                               .object()
                               .value(QStringLiteral("pid"))
                               .toInteger();
        endpoint.close();
#ifdef Q_OS_UNIX
        if (legacyEngineProcess(pid) && ::kill(pid_t(pid), SIGTERM) == 0) {
            legacy.found = true;
            legacy.steps << QStringLiteral("stopped the running forkmesh-core (pid %1)").arg(pid);
        }
#else
        Q_UNUSED(pid);
#endif
    }
    QFile daemon(paths::configRoot() + QStringLiteral("/core/daemon.json"));
    const bool legacyBackground =
        daemon.open(QIODevice::ReadOnly) &&
        QJsonDocument::fromJson(daemon.readAll()).object().value(QStringLiteral("background")).toBool();
    QSettings settings;
    const QString migratedKey = QStringLiteral("background/legacyEngineMigrated");
    if (!legacy.found && (!legacyBackground || settings.value(migratedKey, false).toBool()))
        return;
    settings.setValue(migratedKey, true);
    if (!supported()) {
        for (const QString &step : std::as_const(legacy.steps))
            qInfo().noquote() << "Retired the Go background engine:" << step;
        return;
    }
    if (legacy.startedAtLogin || legacyBackground)
        setKeepRunning(true);
    if (legacy.startedAtLogin) {
        QString error;
        if (manager.enable(desktopBinaryPath(), project, &error))
            legacy.steps << QStringLiteral("ForkMesh now starts without a window at computer start");
        else
            legacy.steps << QStringLiteral("could not set up the start without a window: %1").arg(error);
    }
    for (const QString &step : std::as_const(legacy.steps))
        qInfo().noquote() << "Retired the Go background engine:" << step;
}

} // namespace forkmesh::background
