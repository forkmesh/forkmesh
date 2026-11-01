#include "AppArmorSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>

namespace forkmesh::apparmor {
namespace {

QString readText(const QString &path, qint64 limit = 4 * 1024 * 1024)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.read(limit));
}

QString joined(const QString &root, const QString &path)
{
    if (root.isEmpty() || root == QStringLiteral("/"))
        return path;
    return QDir::cleanPath(root + QLatin1Char('/') + path);
}

QString scriptWrap(const QString &body)
{
    return QStringLiteral("set -u\n"
                          "PATH=/usr/sbin:/usr/bin:/sbin:/bin\n"
                          "parser=$(command -v apparmor_parser || echo /sbin/apparmor_parser)\n"
                          "rc=0\n")
        + body
        + QStringLiteral("echo @@profiles\n"
                         "cat /sys/kernel/security/apparmor/profiles 2>/dev/null\n"
                         "exit $rc\n");
}

QString findParser(const QString &root)
{
    if (root.isEmpty() || root == QStringLiteral("/")) {
        const QString found = QStandardPaths::findExecutable(QStringLiteral("apparmor_parser"),
                                                             {QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"),
                                                              QStringLiteral("/usr/bin")});
        if (!found.isEmpty())
            return found;
    }
    for (const QString &path : {QStringLiteral("/usr/sbin/apparmor_parser"), QStringLiteral("/sbin/apparmor_parser")}) {
        if (QFileInfo::exists(joined(root, path)))
            return path;
    }
    return {};
}

}

QList<ProfileFile> readPolicyFiles(const QString &dir, const QString &source, bool editable)
{
    QList<ProfileFile> files;
    const QDir policy(dir);
    if (!policy.exists())
        return files;
    const QFileInfoList entries = policy.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (!isPolicyFileName(entry.fileName()))
            continue;
        ProfileFile file;
        file.fileName = entry.fileName();
        file.path = entry.absoluteFilePath();
        file.source = source;
        file.editable = editable;
        file.text = readText(file.path, 1024 * 1024);
        file.profiles = parseProfileDeclarations(file.text);
        if (file.profiles.isEmpty())
            continue;
        file.disabled = editable && QFileInfo::exists(policy.filePath(QStringLiteral("disable/") + file.fileName));
        file.forceComplain = editable && QFileInfo::exists(policy.filePath(QStringLiteral("force-complain/") + file.fileName));
        files << file;
    }
    return files;
}

QList<ProcessInfo> readProcesses(const QString &procDir)
{
    QList<ProcessInfo> processes;
    const QDir proc(procDir);
    const QStringList entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool numeric = false;
        const qint64 pid = entry.toLongLong(&numeric);
        if (!numeric)
            continue;
        const QString base = proc.filePath(entry);
        const QByteArray cmdline = [&] {
            QFile file(base + QStringLiteral("/cmdline"));
            return file.open(QIODevice::ReadOnly) ? file.read(4096) : QByteArray();
        }();
        if (cmdline.isEmpty())
            continue;
        ProcessInfo process;
        process.pid = pid;
        process.command = QString::fromUtf8(cmdline).replace(QChar(0), QLatin1Char(' ')).trimmed();
        process.executable = QFileInfo(base + QStringLiteral("/exe")).symLinkTarget();
        const QString first = QString::fromUtf8(cmdline.left(cmdline.indexOf('\0')));
        if (process.executable.isEmpty() && first.startsWith(QLatin1Char('/')))
            process.executable = first;
        process.name = readText(base + QStringLiteral("/comm"), 256).trimmed();
        if (!process.executable.isEmpty())
            process.name = QFileInfo(process.executable).fileName();
        else if (process.name.isEmpty())
            process.name = first.section(QLatin1Char(' '), 0, 0);
        QString current = readText(base + QStringLiteral("/attr/apparmor/current"), 4096);
        if (current.isEmpty())
            current = readText(base + QStringLiteral("/attr/current"), 4096);
        process.label = parseLabel(current);
        processes << process;
    }
    std::sort(processes.begin(), processes.end(), [](const ProcessInfo &left, const ProcessInfo &right) { return left.pid < right.pid; });
    return processes;
}

QString readUserEvents(QString *source)
{
    QString text;
    QStringList sources;
    QFile audit(QStringLiteral("/var/log/audit/audit.log"));
    if (audit.open(QIODevice::ReadOnly)) {
        if (audit.size() > 32 * 1024 * 1024)
            audit.seek(audit.size() - 32 * 1024 * 1024);
        for (const QByteArray &line : audit.readAll().split('\n')) {
            if (line.contains("apparmor="))
                text += QString::fromUtf8(line) + QLatin1Char('\n');
        }
        sources << QStringLiteral("audit log");
    }
    const QString journalctl = QStandardPaths::findExecutable(QStringLiteral("journalctl"));
    if (!journalctl.isEmpty()) {
        QProcess process;
        process.start(journalctl, {QStringLiteral("-k"), QStringLiteral("-o"), QStringLiteral("short-iso"), QStringLiteral("--no-pager"),
                                   QStringLiteral("-q"), QStringLiteral("--since"), QStringLiteral("7 days ago")});
        if (process.waitForFinished(15000) && process.exitCode() == 0) {
            bool any = false;
            for (const QByteArray &line : process.readAllStandardOutput().split('\n')) {
                if (line.contains("apparmor=")) {
                    text += QString::fromUtf8(line) + QLatin1Char('\n');
                    any = true;
                }
            }
            if (any)
                sources << QStringLiteral("kernel journal");
        } else {
            process.kill();
            process.waitForFinished(1000);
        }
    }
    if (source)
        *source = sources.join(QStringLiteral(" and "));
    return text;
}

Snapshot readSnapshot(const QString &root, bool readUserLogs)
{
    Snapshot snapshot;
    snapshot.readAt = QDateTime::currentDateTime();
#if defined(Q_OS_LINUX)
    snapshot.supported = true;
#else
    snapshot.supported = !root.isEmpty() && root != QStringLiteral("/");
#endif
    if (!snapshot.supported)
        return snapshot;
    const QString enabled = readText(joined(root, QStringLiteral("/sys/module/apparmor/parameters/enabled")), 64).trimmed();
    snapshot.moduleLoaded = QFileInfo::exists(joined(root, QStringLiteral("/sys/module/apparmor")));
    snapshot.enabled = enabled.startsWith(QLatin1Char('Y'), Qt::CaseInsensitive);
    snapshot.lsm = readText(joined(root, QStringLiteral("/sys/kernel/security/lsm")), 4096).trimmed();
    snapshot.inLsmList = snapshot.lsm.split(QLatin1Char(',')).contains(QStringLiteral("apparmor"));
    snapshot.parserPath = findParser(root);
    snapshot.policyDir = QStringLiteral("/etc/apparmor.d");
    const QString policy = joined(root, snapshot.policyDir);
    snapshot.abiVersions = QDir(policy + QStringLiteral("/abi")).entryList(QDir::Files, QDir::Name);
    snapshot.files = readPolicyFiles(policy, QStringLiteral("system"), true);
    snapshot.files += readPolicyFiles(joined(root, QStringLiteral("/var/lib/snapd/apparmor/profiles")), QStringLiteral("snap"), false);
    snapshot.processes = readProcesses(joined(root, QStringLiteral("/proc")));
    if (readUserLogs) {
        QString source;
        snapshot.events = parseEvents(readUserEvents(&source));
        snapshot.eventSource = source;
    }
    return snapshot;
}

QString backupDir()
{
    return QStringLiteral("/var/backups/forkmesh-apparmor");
}

QString adminLiveScript()
{
    return scriptWrap(QStringLiteral("echo @@events\n"
                                     "if [ -r /var/log/audit/audit.log ]; then tail -n 200000 /var/log/audit/audit.log | grep -F 'apparmor='; fi\n"
                                     "journalctl -k -o short-iso --no-pager -q --since '7 days ago' 2>/dev/null | grep -F 'apparmor=' | tail -n 50000\n"));
}

QString adminInstallScript()
{
    return scriptWrap(QStringLiteral("src=$1; dest=$2; backups=$3; load=$4\n"
                                     "stamp=$(date +%Y%m%d-%H%M%S)\n"
                                     "label=$(printf '%s' \"$dest\" | tr / _)\n"
                                     "backup=\n"
                                     "mkdir -p \"$backups\" || rc=1\n"
                                     "if [ $rc = 0 ] && [ -e \"$dest\" ]; then backup=\"$backups/$label.$stamp\"; cp -p \"$dest\" \"$backup\" || rc=1; fi\n"
                                     "if [ $rc = 0 ]; then install -D -m 0644 \"$src\" \"$dest\" || rc=1; fi\n"
                                     "if [ $rc = 0 ] && [ -n \"$load\" ]; then\n"
                                     "  if ! \"$parser\" -r -W \"$load\"; then\n"
                                     "    rc=3\n"
                                     "    if [ -n \"$backup\" ]; then cp -p \"$backup\" \"$dest\"; else mv \"$dest\" \"$backups/$label.rejected.$stamp\"; fi\n"
                                     "  fi\n"
                                     "fi\n"));
}

QString adminDisableScript()
{
    return scriptWrap(QStringLiteral("dir=$1; file=$2\n"
                                     "mkdir -p \"$dir/disable\" && ln -sf \"$dir/$file\" \"$dir/disable/$file\" || rc=1\n"
                                     "if [ $rc = 0 ]; then \"$parser\" -R \"$dir/$file\" 2>/dev/null || true; fi\n"));
}

QString adminEnableScript()
{
    return scriptWrap(QStringLiteral("dir=$1; file=$2; backups=$3\n"
                                     "if [ -L \"$dir/disable/$file\" ] || [ -e \"$dir/disable/$file\" ]; then\n"
                                     "  mkdir -p \"$backups\" && mv \"$dir/disable/$file\" \"$backups/disable.$file.$(date +%Y%m%d-%H%M%S)\" || rc=1\n"
                                     "fi\n"
                                     "if [ $rc = 0 ]; then \"$parser\" -r -W \"$dir/$file\" || rc=3; fi\n"));
}

QString adminReloadScript()
{
    return scriptWrap(QStringLiteral("\"$parser\" -r -W \"$1\" || rc=3\n"));
}

QString adminUnloadScript()
{
    return scriptWrap(QStringLiteral("\"$parser\" -R \"$1\" || rc=3\n"));
}

QString adminReloadAllScript()
{
    return scriptWrap(QStringLiteral("systemctl reload apparmor || rc=3\n"));
}

}
