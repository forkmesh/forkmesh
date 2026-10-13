#include "SystemdTimers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace forkmesh::systemdtimers {

namespace {

constexpr int kCommandTimeoutMs = 15000;

const QStringList &managedPrefixes()
{
    static const QStringList prefixes = {QStringLiteral("forkmesh-"),
                                         QStringLiteral("fmnext-")};
    return prefixes;
}

qint64 usecValue(const QJsonValue &value)
{
    if (value.isDouble())
        return static_cast<qint64>(value.toDouble());
    if (value.isString())
        return value.toString().toLongLong();
    return 0;
}

QString unitFileDescription(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    const QString text = QString::fromUtf8(file.read(64 * 1024));
    static const QRegularExpression description(QStringLiteral("^Description=(.*)$"),
                                                QRegularExpression::MultilineOption);
    return description.match(text).captured(1).trimmed();
}

QStringList splitPaths(const QString &value)
{
    QStringList names;
    for (const QString &path : value.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        names.append(QFileInfo(path).fileName());
    return names;
}

} // namespace

Runner systemctlRunner()
{
    return [](const QStringList &arguments) {
        CommandResult result;
        const QString program = QStandardPaths::findExecutable(QStringLiteral("systemctl"));
        if (program.isEmpty()) {
            result.error = QStringLiteral("systemctl was not found");
            return result;
        }
        QProcess process;
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(program, QStringList{QStringLiteral("--user")} + arguments);
        // code-health: allow(blocking-wait) runs only inside the QtConcurrent worker the timers panel starts, capped at 15 s
        if (!process.waitForStarted(kCommandTimeoutMs)) {
            result.error = process.errorString();
            return result;
        }
        result.started = true;
        if (!process.waitForFinished(kCommandTimeoutMs)) {
            result.timedOut = true;
            process.kill();
            process.waitForFinished(1000);
            result.error = QStringLiteral("systemctl %1 took longer than 15 s")
                               .arg(arguments.value(0));
            return result;
        }
        result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
        result.output = process.readAllStandardOutput();
        if (result.exitCode != 0)
            result.error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return result;
    };
}

bool systemctlAvailable()
{
#ifdef Q_OS_LINUX
    return !QStandardPaths::findExecutable(QStringLiteral("systemctl")).isEmpty();
#else
    return false;
#endif
}

bool Timer::enabled() const
{
    return fileState.startsWith(QLatin1String("enabled"))
           || activeState == QLatin1String("active");
}

bool isManagedUnit(const QString &unit)
{
    if (!unit.endsWith(QLatin1String(".timer")))
        return false;
    return std::any_of(managedPrefixes().cbegin(), managedPrefixes().cend(),
                       [&unit](const QString &prefix) { return unit.startsWith(prefix); });
}

QString serviceFor(const QString &timerUnit)
{
    QString service = timerUnit;
    service.chop(int(sizeof(".timer")) - 1);
    return service + QStringLiteral(".service");
}

QList<ListedTimer> parseListTimers(const QByteArray &json)
{
    QList<ListedTimer> timers;
    const QJsonArray rows = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &row : rows) {
        const QJsonObject object = row.toObject();
        ListedTimer timer;
        timer.unit = object.value(QStringLiteral("unit")).toString();
        if (timer.unit.isEmpty())
            continue;
        timer.activates = object.value(QStringLiteral("activates")).toString();
        timer.nextUsec = usecValue(object.value(QStringLiteral("next")));
        timer.lastUsec = usecValue(object.value(QStringLiteral("last")));
        timers.append(timer);
    }
    return timers;
}

QHash<QString, QString> parseUnitFiles(const QByteArray &json)
{
    QHash<QString, QString> states;
    const QJsonArray rows = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &row : rows) {
        const QJsonObject object = row.toObject();
        const QString unit = object.value(QStringLiteral("unit_file")).toString();
        if (!unit.isEmpty())
            states.insert(unit, object.value(QStringLiteral("state")).toString());
    }
    return states;
}

QList<QHash<QString, QString>> parseShow(const QByteArray &text)
{
    QList<QHash<QString, QString>> blocks;
    QHash<QString, QString> current;
    const QList<QByteArray> lines = text.split('\n');
    for (const QByteArray &raw : lines) {
        const QString line = QString::fromUtf8(raw);
        if (line.trimmed().isEmpty()) {
            if (!current.isEmpty())
                blocks.append(current);
            current.clear();
            continue;
        }
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = line.left(equals);
        const QString value = line.mid(equals + 1);
        if (current.contains(key))
            current[key] += QStringLiteral("\n") + value;
        else
            current.insert(key, value);
    }
    if (!current.isEmpty())
        blocks.append(current);
    return blocks;
}

QString commandLine(const QString &execStart)
{
    static const QRegularExpression argv(QStringLiteral("argv\\[\\]=(.*?) ; "));
    QStringList commands;
    auto matches = argv.globalMatch(execStart);
    while (matches.hasNext())
        commands.append(matches.next().captured(1).trimmed());
    return commands.join(QStringLiteral("; "));
}

Snapshot collect(const Runner &runner, const QString &unitSourceDirectory)
{
    Snapshot snapshot;
    const CommandResult files = runner({QStringLiteral("list-unit-files"),
                                        QStringLiteral("--type=timer"),
                                        QStringLiteral("--output=json"),
                                        QStringLiteral("--no-pager")});
    if (!files.ok()) {
        snapshot.error = files.error.isEmpty()
                             ? QStringLiteral("systemctl --user list-unit-files failed")
                             : files.error;
        return snapshot;
    }
    const QHash<QString, QString> fileStates = parseUnitFiles(files.output);

    QHash<QString, ListedTimer> listed;
    const CommandResult timers = runner({QStringLiteral("list-timers"), QStringLiteral("--all"),
                                         QStringLiteral("--output=json"),
                                         QStringLiteral("--no-pager")});
    if (timers.ok()) {
        for (const ListedTimer &timer : parseListTimers(timers.output))
            listed.insert(timer.unit, timer);
    }

    QHash<QString, QString> sources;
    if (!unitSourceDirectory.isEmpty()) {
        const QDir directory(unitSourceDirectory);
        const QStringList names =
            directory.entryList({QStringLiteral("*.timer")}, QDir::Files, QDir::Name);
        for (const QString &name : names) {
            if (isManagedUnit(name))
                sources.insert(name, directory.filePath(name));
        }
    }

    QSet<QString> units;
    for (auto it = fileStates.cbegin(); it != fileStates.cend(); ++it) {
        if (isManagedUnit(it.key()))
            units.insert(it.key());
    }
    for (auto it = listed.cbegin(); it != listed.cend(); ++it) {
        if (isManagedUnit(it.key()))
            units.insert(it.key());
    }
    for (auto it = sources.cbegin(); it != sources.cend(); ++it)
        units.insert(it.key());
    QStringList ordered(units.cbegin(), units.cend());
    std::sort(ordered.begin(), ordered.end());
    if (ordered.isEmpty())
        return snapshot;

    for (const QString &unit : ordered) {
        Timer timer;
        timer.unit = unit;
        timer.fileState = fileStates.value(unit);
        timer.sourcePath = sources.value(unit);
        const ListedTimer row = listed.value(unit);
        timer.nextUsec = row.nextUsec;
        timer.lastUsec = row.lastUsec;
        timer.service = row.activates.isEmpty() ? serviceFor(unit) : row.activates;
        snapshot.timers.append(timer);
    }

    QStringList showArguments = {QStringLiteral("show"), QStringLiteral("--no-pager"),
                                 QStringLiteral("-p"),
                                 QStringLiteral("Id,Description,LoadState,UnitFileState,"
                                                "ActiveState,Result,ExecStart,DropInPaths")};
    for (const Timer &timer : snapshot.timers)
        showArguments << timer.unit << timer.service;
    QHash<QString, QHash<QString, QString>> details;
    const CommandResult shown = runner(showArguments);
    if (shown.ok()) {
        for (const auto &block : parseShow(shown.output))
            details.insert(block.value(QStringLiteral("Id")), block);
    }

    for (Timer &timer : snapshot.timers) {
        const auto unit = details.value(timer.unit);
        const auto service = details.value(timer.service);
        const bool loaded = unit.value(QStringLiteral("LoadState")) == QLatin1String("loaded");
        if (loaded) {
            timer.description = unit.value(QStringLiteral("Description"));
            timer.activeState = unit.value(QStringLiteral("ActiveState"));
            if (timer.fileState.isEmpty())
                timer.fileState = unit.value(QStringLiteral("UnitFileState"));
        }
        if (timer.description.isEmpty() || timer.description == timer.unit) {
            const QString fromFile = unitFileDescription(timer.sourcePath);
            if (!fromFile.isEmpty())
                timer.description = fromFile;
        }
        timer.serviceActiveState = service.value(QStringLiteral("ActiveState"));
        timer.serviceResult = service.value(QStringLiteral("Result"));
        timer.command = commandLine(service.value(QStringLiteral("ExecStart")));
        timer.dropIns = splitPaths(service.value(QStringLiteral("DropInPaths")));
    }
    return snapshot;
}

QList<QStringList> enableCommands(const Timer &timer)
{
    const QString target = timer.installed() || timer.sourcePath.isEmpty() ? timer.unit
                                                                            : timer.sourcePath;
    return {{QStringLiteral("enable"), QStringLiteral("--now"), target}};
}

QList<QStringList> disableCommands(const Timer &timer)
{
    return {{QStringLiteral("disable"), QStringLiteral("--now"), timer.unit},
            {QStringLiteral("reset-failed"), timer.unit}};
}

QStringList runNowArguments(const Timer &timer)
{
    return {QStringLiteral("start"), QStringLiteral("--no-block"), timer.service};
}

QString relativeTime(qint64 usec, qint64 nowUsec)
{
    if (usec <= 0)
        return QString();
    const qint64 seconds = (usec - nowUsec) / 1000000;
    const qint64 span = seconds < 0 ? -seconds : seconds;
    QString amount;
    if (span < 60)
        amount = QStringLiteral("%1 s").arg(span);
    else if (span < 3600)
        amount = QStringLiteral("%1 min").arg(span / 60);
    else if (span < 86400)
        amount = (span % 3600) / 60 == 0
                     ? QStringLiteral("%1 h").arg(span / 3600)
                     : QStringLiteral("%1 h %2 min").arg(span / 3600).arg((span % 3600) / 60);
    else
        amount = QStringLiteral("%1 d").arg(span / 86400);
    return seconds < 0 ? QStringLiteral("%1 ago").arg(amount)
                       : QStringLiteral("in %1").arg(amount);
}

} // namespace forkmesh::systemdtimers
