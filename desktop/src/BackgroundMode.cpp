#include "BackgroundMode.h"

#include "AppPaths.h"
#include "ProcessReaper.h"
#include "SingleInstance.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <atomic>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <pwd.h>
#include <unistd.h>
#endif

namespace forkmesh::background {

namespace {

constexpr char kLegacyLabelPrefix[] = "forkmesh-core-";
constexpr char kLegacyLingerMarker[] = "forkmesh-core.linger";
constexpr char kLegacyEngineName[] = "forkmesh-core";

std::atomic_bool g_stopSignal{false};
std::atomic_bool g_sessionEnding{false};
std::atomic_bool g_handOverRequested{false};

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

QString scriptString(const QString &value)
{
    const QByteArray quoted = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(quoted.mid(1, quoted.size() - 2));
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
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const bool userManager =
        !qEnvironmentVariableIsEmpty("INVOCATION_ID") ||
        (QDir::isAbsolutePath(runtime) &&
         QFileInfo::exists(runtime + QStringLiteral("/systemd/private")));
    const QString systemdRun =
        userManager ? QStandardPaths::findExecutable(QStringLiteral("systemd-run")) : QString();
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

QString logoutUnitName(const QString &project)
{
    return QStringLiteral("forkmesh-logout-") + project + QStringLiteral(".service");
}

QStringList logoutArguments(const QString &project)
{
    return {QStringLiteral("--hand-over-at-logout"), QStringLiteral("--project"), project};
}

QStringList serviceVariables()
{
    return {QStringLiteral("INVOCATION_ID"),    QStringLiteral("JOURNAL_STREAM"),
            QStringLiteral("SYSTEMD_EXEC_PID"), QStringLiteral("MANAGERPID"),
            QStringLiteral("NOTIFY_SOCKET"),    QStringLiteral("LISTEN_PID"),
            QStringLiteral("LISTEN_FDS"),       QStringLiteral("LISTEN_FDNAMES"),
            QStringLiteral("WATCHDOG_PID"),     QStringLiteral("WATCHDOG_USEC")};
}

QStringList sleepTargets()
{
    return {QStringLiteral("sleep.target"), QStringLiteral("suspend.target"),
            QStringLiteral("hibernate.target"), QStringLiteral("hybrid-sleep.target"),
            QStringLiteral("suspend-then-hibernate.target")};
}

QStringList systemSleepArguments(bool disable)
{
    return QStringList{QStringLiteral("systemctl"),
                       disable ? QStringLiteral("mask") : QStringLiteral("unmask")} +
           sleepTargets();
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
               QStringLiteral("StartLimitIntervalSec=0"),
               QString(),
               QStringLiteral("[Service]"),
               QStringLiteral("Type=simple"),
               QStringLiteral("ExecStart=") + command.join(QLatin1Char(' ')),
               QStringLiteral("Environment=QT_QPA_PLATFORM=offscreen FORKMESH_NO_SPLASH=1"),
               QStringLiteral("UnsetEnvironment=FORKMESH_HOME FORKMESH_PROJECTS_DIR "
                              "FORKMESH_PROJECT FORKMESH_PROJECT_DIR"),
               QStringLiteral("Restart=on-failure"),
               QStringLiteral("RestartSec=10"),
               QStringLiteral("RestartSteps=5"),
               QStringLiteral("RestartMaxDelaySec=300"),
               QStringLiteral("KillMode=mixed"),
               QStringLiteral("TimeoutStopSec=30"),
               QString(),
               QStringLiteral("[Install]"),
               QStringLiteral("WantedBy=default.target"),
               QString(),
           }
        .join(QLatin1Char('\n'));
}

QString logoutUnit(const QString &binary, const QString &project)
{
    QStringList command{systemdQuote(binary)};
    for (const QString &argument : logoutArguments(project))
        command << systemdQuote(argument);
    return QStringList{
               QStringLiteral("[Unit]"),
               QStringLiteral("Description=Keep ForkMesh running after log-out (%1)").arg(project),
               QStringLiteral("PartOf=graphical-session.target"),
               QStringLiteral("After=graphical-session.target"),
               QString(),
               QStringLiteral("[Service]"),
               QStringLiteral("Type=oneshot"),
               QStringLiteral("RemainAfterExit=yes"),
               QStringLiteral("ExecStop=") + command.join(QLatin1Char(' ')),
               QStringLiteral("Environment=QT_QPA_PLATFORM=offscreen"),
               QStringLiteral("UnsetEnvironment=FORKMESH_HOME FORKMESH_PROJECTS_DIR "
                              "FORKMESH_PROJECT FORKMESH_PROJECT_DIR"),
               QStringLiteral("TimeoutStopSec=%1").arg(kLogoutWaitMs / 1000 + 15),
               QString(),
               QStringLiteral("[Install]"),
               QStringLiteral("WantedBy=graphical-session.target"),
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

namespace {

QString execBinary(const QString &unit, QLatin1String key)
{
    for (const QString &line : unit.split(QLatin1Char('\n'))) {
        if (!line.startsWith(key))
            continue;
        const QString rest = line.mid(key.size()).trimmed();
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

} // namespace

QString execStartBinary(const QString &unit)
{
    return execBinary(unit, QLatin1String("ExecStart="));
}

QString execStopBinary(const QString &unit)
{
    return execBinary(unit, QLatin1String("ExecStop="));
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
    QByteArray accountBuffer(16384, Qt::Uninitialized);
    struct passwd entry;
    struct passwd *found = nullptr;
    if (::getpwuid_r(::getuid(), &entry, accountBuffer.data(), size_t(accountBuffer.size()), &found) == 0 && found)
        manager.account = QString::fromLocal8Bit(entry.pw_name);
#endif
    manager.run = [](const QString &program, const QStringList &arguments) {
        CommandResult result;
        if (QStandardPaths::findExecutable(program).isEmpty())
            return result;
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(program, arguments);
        if (!process.waitForFinished(program == QLatin1String("pkexec") ? 120000 : 15000)) {
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

QString StartManager::logoutUnitPath(const QString &project) const
{
    return configHome + QStringLiteral("/systemd/user/") + logoutUnitName(project);
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

QString StartManager::bootSleepPolicyPath() const
{
    return polkitRulesHome + QStringLiteral("/49-forkmesh-sleep-%1.rules").arg(uid);
}

QString StartManager::bootSleepPolicy() const
{
    if (account.isEmpty())
        return {};
    return QStringLiteral(
        "polkit.addRule(function(action, subject) {\n"
        "    if (action.id === \"org.freedesktop.login1.inhibit-block-sleep\" &&\n"
        "        subject.user === %1)\n"
        "        return polkit.Result.YES;\n"
        "});\n").arg(scriptString(account));
}

bool StartManager::bootSleepProtected() const
{
    return !account.isEmpty() && readText(bootSleepPolicyPath()) == bootSleepPolicy();
}

bool StartManager::ensureBootSleepPermission(QString *error) const
{
    if (bootSleepProtected() || sleepDisabled())
        return true;
    if (account.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not determine the account for the boot sleep permission.");
        return false;
    }
    const CommandResult result =
        installPolicy(QStringLiteral("forkmesh-boot-sleep"), bootSleepPolicyPath(), bootSleepPolicy());
    if (!result.ok && error)
        *error = QStringLiteral("Start at boot is registered, but keeping the computer awake "
                                "before login needs administrator authentication. %1")
                     .arg(QString::fromUtf8(result.output).trimmed());
    return result.ok;
}

CommandResult StartManager::installPolicy(const QString &tag, const QString &path,
                                          const QString &text) const
{
    return call(QStringLiteral("pkexec"), {
        QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("umask 022; mkdir -p -- \"$1\" && "
                       "printf '%s' \"$3\" > \"$2\" && chmod 0644 -- \"$2\""),
        tag, polkitRulesHome, path, text});
}

QString StartManager::restartPolicyPath() const
{
    return polkitRulesHome + QStringLiteral("/49-forkmesh-restart-%1.rules").arg(uid);
}

QString StartManager::restartPolicy() const
{
    if (account.isEmpty())
        return {};
    return QStringLiteral(
        "polkit.addRule(function(action, subject) {\n"
        "    if ((action.id === \"org.freedesktop.login1.reboot\" ||\n"
        "         action.id === \"org.freedesktop.login1.reboot-multiple-sessions\") &&\n"
        "        subject.user === %1)\n"
        "        return polkit.Result.YES;\n"
        "});\n").arg(scriptString(account));
}

bool StartManager::restartAllowed() const
{
    return !account.isEmpty() && readText(restartPolicyPath()) == restartPolicy();
}

bool StartManager::allowRestart(bool on, QString *error) const
{
    if (on ? restartAllowed() : !QFileInfo::exists(restartPolicyPath()))
        return true;
    if (on && account.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not determine the account for the restart permission.");
        return false;
    }
    const CommandResult result =
        on ? installPolicy(QStringLiteral("forkmesh-restart"), restartPolicyPath(), restartPolicy())
           : call(QStringLiteral("pkexec"),
                  {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                   QStringLiteral("rm -f -- \"$1\""), QStringLiteral("forkmesh-restart"),
                   restartPolicyPath()});
    if (!result.ok && error) {
        const QString output = QString::fromUtf8(result.output).trimmed();
        *error = QStringLiteral("Changing who may restart this computer needs administrator "
                                "authentication. %1")
                     .arg(output.isEmpty() ? QStringLiteral("Not approved.") : output);
    }
    return result.ok;
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
                                  .entryList({QStringLiteral("forkmesh-headless-*.service"),
                                              QStringLiteral("forkmesh-logout-*.service")},
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

bool StartManager::refreshUnit(const QString &binary, const QString &project) const
{
    const QString path = systemdUnitPath(project);
    if (!paths::isValidSlug(project) || !QFileInfo::exists(path))
        return false;
    const QString current = readText(path);
    const QString installed = execStartBinary(current);
    const QString chosen = validBinary(installed) ? installed : binary;
    if (!validBinary(chosen))
        return false;
    const QString wanted = systemdUnit(chosen, project);
    if (current == wanted || !writeText(path, wanted, nullptr))
        return false;
    call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")});
    return true;
}

bool StartManager::sleepDisabled() const
{
    const QStringList states =
        QString::fromUtf8(
            call(QStringLiteral("systemctl"), QStringList{QStringLiteral("is-enabled")} + sleepTargets())
                .output)
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (states.size() != sleepTargets().size())
        return false;
    for (const QString &state : states) {
        if (!state.trimmed().startsWith(QLatin1String("masked")))
            return false;
    }
    return true;
}

bool StartManager::keepAcrossLogout(const QString &binary, const QString &project,
                                    QString *error) const
{
    if (!paths::isValidSlug(project)) {
        if (error)
            *error = QStringLiteral("invalid project name %1").arg(project);
        return false;
    }
    if (!hasSystemd()) {
        if (error)
            *error = QStringLiteral("there is no user systemd to keep it running after log-out");
        return false;
    }
    const QString path = logoutUnitPath(project);
    const QString current = readText(path);
    const QString installed = execStopBinary(current);
    const QString chosen = validBinary(installed) ? installed : binary;
    if (!validBinary(chosen)) {
        if (error)
            *error = QStringLiteral("%1 is not an executable file").arg(chosen);
        return false;
    }
    const QString wanted = logoutUnit(chosen, project);
    if (current != wanted) {
        if (!writeText(path, wanted, error))
            return false;
        call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")});
    }
    const CommandResult enabled =
        call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("enable"),
                                           QStringLiteral("--now"), logoutUnitName(project)});
    if (!enabled.ok) {
        if (error)
            *error = QStringLiteral("systemctl --user enable --now %1 failed: %2")
                         .arg(logoutUnitName(project), QString::fromUtf8(enabled.output).trimmed());
        return false;
    }
    enableLinger();
    return true;
}

void StartManager::dropLogoutHandOver(const QString &project) const
{
    if (!paths::isValidSlug(project) || !QFileInfo::exists(logoutUnitPath(project)))
        return;
    call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("disable"),
                                       QStringLiteral("--now"), logoutUnitName(project)});
    removeFile(logoutUnitPath(project));
    call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")});
    releaseLinger();
}

bool StartManager::syncLogoutHandOver(bool keep, const QString &binary, const QString &project,
                                      QString *error) const
{
    if (keep)
        return keepAcrossLogout(binary, project, error);
    dropLogoutHandOver(project);
    return true;
}

bool StartManager::shuttingDown() const
{
    for (const QStringList &arguments :
         {QStringList{QStringLiteral("is-system-running")},
          QStringList{QStringLiteral("--user"), QStringLiteral("is-system-running")}}) {
        if (call(QStringLiteral("systemctl"), arguments).output.trimmed() == "stopping")
            return true;
    }
    const CommandResult preparing = call(
        QStringLiteral("busctl"),
        {QStringLiteral("get-property"), QStringLiteral("org.freedesktop.login1"),
         QStringLiteral("/org/freedesktop/login1"), QStringLiteral("org.freedesktop.login1.Manager"),
         QStringLiteral("PreparingForShutdown")});
    return preparing.ok && preparing.output.trimmed() == "b true";
}

StartStatus StartManager::status(const QString &project) const
{
    StartStatus status;
    status.sleepDisabled = sleepDisabled();
    status.bootSleepProtected = bootSleepProtected();
    status.restartAllowed = restartAllowed();
    status.afterLogout = QFileInfo::exists(logoutUnitPath(project)) && lingering();
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
        if (!isBackgroundInstance())
            ensureBootSleepPermission(error);
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

RunnerDiagnostics StartManager::diagnose(const QString &project, qint64 instancePid,
                                         bool withJournal) const
{
    RunnerDiagnostics diagnostics;
    diagnostics.start = status(project);
    diagnostics.unit = unitName(project);
    diagnostics.instancePid = instancePid;
    diagnostics.instanceRunning = headlessProcessRunning(instancePid);
    diagnostics.binaryExecutable = validBinary(diagnostics.start.binary);
    if (!diagnostics.start.enabled ||
        !diagnostics.start.mechanism.startsWith(QLatin1String("systemd")))
        return diagnostics;
    const CommandResult shown = call(
        QStringLiteral("systemctl"),
        {QStringLiteral("--user"), QStringLiteral("show"), diagnostics.unit,
         QStringLiteral("--property=UnitFileState,ActiveState,SubState,Result,MainPID,"
                        "NRestarts,ExecMainStatus,ActiveEnterTimestamp,InactiveEnterTimestamp")});
    diagnostics.systemdReachable = shown.ok;
    const QStringList lines = QString::fromUtf8(shown.output).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = line.left(equals);
        const QString value = line.mid(equals + 1).trimmed();
        if (key == QLatin1String("UnitFileState"))
            diagnostics.unitFileState = value;
        else if (key == QLatin1String("ActiveState"))
            diagnostics.activeState = value;
        else if (key == QLatin1String("SubState"))
            diagnostics.subState = value;
        else if (key == QLatin1String("Result"))
            diagnostics.result = value;
        else if (key == QLatin1String("MainPID"))
            diagnostics.mainPid = value.toLongLong();
        else if (key == QLatin1String("NRestarts"))
            diagnostics.restarts = value.toInt();
        else if (key == QLatin1String("ExecMainStatus"))
            diagnostics.lastExitStatus = value.toInt();
        else if (key == QLatin1String("ActiveEnterTimestamp"))
            diagnostics.activeSince = value;
        else if (key == QLatin1String("InactiveEnterTimestamp"))
            diagnostics.inactiveSince = value;
    }
    if (!withJournal)
        return diagnostics;
    diagnostics.journal =
        QString::fromUtf8(call(QStringLiteral("journalctl"),
                               {QStringLiteral("--user-unit=") + diagnostics.unit,
                                QStringLiteral("-n"), QStringLiteral("20"),
                                QStringLiteral("--no-pager"), QStringLiteral("-q"),
                                QStringLiteral("-o"),
                                QStringLiteral("short-iso")})
                              .output)
            .trimmed();
    return diagnostics;
}

bool StartManager::turnOff(const QString &project, QString *error) const
{
    if (paths::isValidSlug(project) && QFileInfo::exists(systemdUnitPath(project)) &&
        !isBackgroundInstance()) {
        call(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("stop"),
                                           QStringLiteral("--no-block"), unitName(project)});
        call(QStringLiteral("systemctl"),
             {QStringLiteral("--user"), QStringLiteral("reset-failed"), unitName(project)});
    }
    return disable(project, error);
}

bool headlessProcessRunning(qint64 pid)
{
#if defined(Q_OS_LINUX)
    if (pid <= 1)
        return false;
    QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!cmdline.open(QIODevice::ReadOnly))
        return false;
    return cmdline.readAll().split('\0').contains("--headless");
#else
    Q_UNUSED(pid);
    return false;
#endif
}

namespace {

struct RunnerState {
    QString name;
    QString severity;
    QString detail;
};

RunnerState runnerState(const RunnerDiagnostics &d)
{
    const bool systemd = d.start.mechanism.startsWith(QLatin1String("systemd"));
    if (d.instanceRunning || d.activeState == QLatin1String("active")) {
        const qint64 pid = d.instanceRunning ? d.instancePid : d.mainPid;
        QString detail = QStringLiteral("ForkMesh is running without a window (pid %1).").arg(pid);
        if (d.restarts > 0)
            detail += QStringLiteral(" systemd restarted it %1 times since it was enabled.")
                          .arg(d.restarts);
        return {QStringLiteral("Running"), QStringLiteral("ok"), detail};
    }
    if (!d.binaryExecutable)
        return {QStringLiteral("Broken"), QStringLiteral("error"),
                QStringLiteral("It starts %1, which is not an executable file any more. Turn "
                               "start at boot off and on in Settings to use this copy.")
                    .arg(d.start.binary.isEmpty() ? QStringLiteral("no program") : d.start.binary)};
    if (d.activeState == QLatin1String("failed"))
        return {QStringLiteral("Failed"), QStringLiteral("error"),
                QStringLiteral("Its last run failed (%1, exit status %2). It starts again at "
                               "the next boot.")
                    .arg(d.result, QString::number(d.lastExitStatus))};
    if (d.activeState == QLatin1String("activating"))
        return {QStringLiteral("Restarting"), QStringLiteral("warn"),
                QStringLiteral("It stopped with an error and systemd is starting it again "
                               "(%1 restarts so far).")
                    .arg(d.restarts)};
    if (systemd && !d.systemdReachable)
        return {QStringLiteral("Unknown"), QStringLiteral("warn"),
                QStringLiteral("systemctl --user did not answer, so its state is unknown.")};
    if (systemd && d.unitFileState != QLatin1String("enabled"))
        return {QStringLiteral("Not enabled"), QStringLiteral("error"),
                QStringLiteral("The unit file is there, but systemd reports it as %1, so it "
                               "does not start.")
                    .arg(d.unitFileState.isEmpty() ? QStringLiteral("unknown") : d.unitFileState)};
    const QString closing = d.keepRunning
                                ? QStringLiteral(" and when you close this window")
                                : QString();
    if (systemd && !d.start.atBoot)
        return {QStringLiteral("At log-in"), QStringLiteral("warn"),
                QStringLiteral("Lingering is off, so it starts when you log in%1, not when the "
                               "computer starts. To start it before anyone logs in, run: "
                               "loginctl enable-linger")
                    .arg(closing)};
    return {QStringLiteral("Ready"), QStringLiteral("ok"),
            QStringLiteral("Not running while this window is open. It starts %1%2.")
                .arg(systemd ? QStringLiteral("when the computer starts")
                             : QStringLiteral("when you log in"),
                     closing)};
}

QString yesNo(bool on)
{
    return on ? QStringLiteral("yes") : QStringLiteral("no");
}

} // namespace

RunnerBadge runnerBadge(const RunnerDiagnostics &diagnostics)
{
    RunnerBadge badge;
    if (!diagnostics.start.enabled)
        return badge;
    const RunnerState state = runnerState(diagnostics);
    badge.visible = true;
    badge.severity = state.severity;
    badge.text = QStringLiteral("Background: %1").arg(state.name);
    badge.toolTip = QStringLiteral("ForkMesh without a window: %1\n%2\n\nClick for diagnostics "
                                   "or to turn it off.")
                        .arg(state.name, state.detail);
    return badge;
}

bool canCloseToHeadless(const RunnerDiagnostics &diagnostics)
{
    return diagnostics.start.enabled && !diagnostics.instanceRunning &&
           diagnostics.activeState != QLatin1String("active");
}

QString runnerReport(const RunnerDiagnostics &d)
{
    if (!d.start.enabled)
        return QStringLiteral("ForkMesh is not set to start without a window.\nIt would be added "
                              "as a %1 at %2.")
            .arg(d.start.mechanism, d.start.location);
    const RunnerState state = runnerState(d);
    const bool systemd = d.start.mechanism.startsWith(QLatin1String("systemd"));
    QStringList lines;
    lines << QStringLiteral("State: %1").arg(state.name) << state.detail << QString();
    lines << QStringLiteral("Registered as: %1").arg(d.start.mechanism);
    lines << QStringLiteral("File: %1").arg(d.start.location);
    lines << QStringLiteral("Program: %1 (%2)")
                 .arg(d.start.binary.isEmpty() ? QStringLiteral("none") : d.start.binary,
                      d.binaryExecutable ? QStringLiteral("executable")
                                         : QStringLiteral("missing or not executable"));
    if (systemd) {
        lines << QStringLiteral("Unit: %1").arg(d.unit);
        if (d.systemdReachable) {
            lines << QStringLiteral("systemd: %1, %2 (%3), result %4")
                         .arg(d.unitFileState, d.activeState, d.subState, d.result);
            if (d.mainPid > 0)
                lines << QStringLiteral("Main pid: %1").arg(d.mainPid);
            lines << QStringLiteral("Restarts: %1").arg(d.restarts);
            lines << QStringLiteral("Last exit status: %1").arg(d.lastExitStatus);
            if (!d.activeSince.isEmpty())
                lines << QStringLiteral("Last started: %1").arg(d.activeSince);
            if (!d.inactiveSince.isEmpty())
                lines << QStringLiteral("Last stopped: %1").arg(d.inactiveSince);
        } else {
            lines << QStringLiteral("systemd: systemctl --user did not answer");
        }
        lines << QStringLiteral("Starts before log-in (lingering): %1").arg(yesNo(d.start.atBoot));
    }
    lines << QStringLiteral("Windowless instance: %1")
                 .arg(d.instanceRunning ? QStringLiteral("running, pid %1").arg(d.instancePid)
                                        : QStringLiteral("not running"));
    lines << QStringLiteral("Keep running after the window closes: %1").arg(yesNo(d.keepRunning));
    lines << QStringLiteral("Keeps running after log-out: %1").arg(yesNo(d.start.afterLogout));
    lines << QStringLiteral("Sleep: %1")
                 .arg(d.start.sleepDisabled
                          ? QStringLiteral("this computer never goes to sleep")
                          : d.start.bootSleepProtected
                                ? QStringLiteral("ForkMesh may keep it awake before log-in")
                                : QStringLiteral("the login screen can put it to sleep"));
    if (systemd) {
        lines << QString() << QStringLiteral("Recent log (journalctl --user-unit=%1 -n 20):")
                                  .arg(d.unit);
        lines << (d.journal.isEmpty() ? QStringLiteral("(nothing)") : d.journal);
    }
    return lines.join(QLatin1Char('\n'));
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
    QSettings settings;
    settings.setValue(QLatin1String(kKeepRunningSetting), on);
    settings.sync();
}

void noteStopSignal()
{
    g_stopSignal = true;
}

void noteSessionEnding()
{
    g_sessionEnding = true;
}

bool sessionEnding()
{
    return g_sessionEnding;
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

void requestHandOverOnClose()
{
    g_handOverRequested = true;
}

bool handOverAfterWindowClose(bool restarting)
{
    if (restarting || g_stopSignal || !supported() || !(keepRunning() || g_handOverRequested) ||
        isBackgroundInstance() || !singleInstanceHeld() || !displayPlatform())
        return false;
    if (!startHeadless()) {
        qWarning().noquote() << "ForkMesh closed, but could not start it without a window.";
        return false;
    }
    qInfo().noquote() << "ForkMesh keeps running without a window for phones and agents.";
    if (!g_sessionEnding)
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

void syncLogoutHandOver()
{
    if (!supported() || isBackgroundInstance())
        return;
    QString error;
    if (!StartManager::system().syncLogoutHandOver(keepRunning(), desktopBinaryPath(),
                                                   paths::projectSlug(), &error))
        qWarning().noquote() << "ForkMesh may stop when you log out:" << error;
}

void refreshStartUnit()
{
    if (!supported())
        return;
    const StartManager manager = StartManager::system();
    if (manager.refreshUnit(desktopBinaryPath(), paths::projectSlug()))
        qInfo().noquote() << "Updated the unit that starts ForkMesh without a window.";
    if (!isBackgroundInstance() && manager.unitInstalled(paths::projectSlug())) {
        QString error;
        if (!manager.ensureBootSleepPermission(&error))
            qWarning().noquote() << error;
    }
}

QString handOverAtLogout()
{
    if (!supported() || !keepRunning())
        return QStringLiteral("ForkMesh is not set to keep running without a window.");
    if (StartManager::system().shuttingDown())
        return QStringLiteral("The computer is shutting down; ForkMesh stops with it.");
    const qint64 window = singleInstanceHolderPid();
    if (!askWindowToHandOver())
        return QStringLiteral("No ForkMesh window is open.");
    QElapsedTimer waited;
    waited.start();
#ifdef Q_OS_UNIX
    while (window > 1 && waited.elapsed() < kLogoutWaitMs &&
           (::kill(pid_t(window), 0) == 0 || errno != ESRCH))
        QThread::msleep(200);
#endif
    return QStringLiteral("ForkMesh closed its window after %1 ms and keeps running without one.")
        .arg(waited.elapsed());
}

} // namespace forkmesh::background
