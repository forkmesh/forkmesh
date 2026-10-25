#include "ClaudeDesktopSessions.h"

#include "AgentResumeIdentity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrlQuery>

namespace forkmesh::claudedesktop {

namespace {

QString projectsRootOf(const QString &configDir)
{
    return QDir(configDir).filePath(QStringLiteral("projects"));
}

QString canonical(const QString &path)
{
    const QString resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? QDir::cleanPath(path) : resolved;
}

bool samePlace(const QString &a, const QString &b)
{
    return canonical(a) == canonical(b);
}

QString linkOrReuse(const QString &target, const QString &source)
{
    const QFileInfo info(target);
    if (info.isSymLink()) {
        if (samePlace(info.symLinkTarget(), source))
            return target;
        return QString();
    }
    if (info.exists())
        return samePlace(target, source) ? target : QString();
    if (!QFile::link(source, target))
        return QString();
    return target;
}

} // namespace

QString defaultClaudeHome()
{
    return QDir::home().filePath(QStringLiteral(".claude"));
}

bool isSessionId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return pattern.match(id).hasMatch();
}

QUrl resumeUrl(const QString &sessionId)
{
    if (!isSessionId(sessionId))
        return QUrl();
    QUrl url;
    url.setScheme(QStringLiteral("claude"));
    url.setHost(QStringLiteral("resume"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("session"), sessionId.toLower());
    url.setQuery(query);
    return url;
}

QString findTranscript(const QString &projectsRoot, const QString &sessionId)
{
    if (!isSessionId(sessionId))
        return QString();
    const QString fileName = sessionId.toLower() + QStringLiteral(".jsonl");
    const QDir root(projectsRoot);
    if (!root.exists())
        return QString();
    const QStringList dirs =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QString &dir : dirs) {
        const QFileInfo file(root.filePath(dir + QLatin1Char('/') + fileName));
        if (file.exists() && file.isFile() && !file.isSymLink())
            return file.filePath();
    }
    return QString();
}

QString linkProjectDir(const QString &accountProjectDir, const QString &accountConfigDir,
                       const QString &claudeHome)
{
    const QFileInfo source(accountProjectDir);
    if (!source.isDir() || source.isSymLink())
        return QString();
    const QString root = projectsRootOf(claudeHome);
    if (samePlace(source.absolutePath(), root))
        return source.absoluteFilePath();
    if (!QDir().mkpath(root))
        return QString();
    const QString name = source.fileName();
    const QString primary = linkOrReuse(QDir(root).filePath(name), source.absoluteFilePath());
    if (!primary.isEmpty())
        return primary;
    const QString account = QFileInfo(QDir::cleanPath(accountConfigDir)).fileName().left(8);
    if (account.isEmpty())
        return QString();
    return linkOrReuse(QDir(root).filePath(name + QStringLiteral("--forkmesh-") + account),
                       source.absoluteFilePath());
}

Exposure exposeSession(const QString &sessionId, const QStringList &accountConfigDirs,
                       const QString &claudeHome)
{
    Exposure out;
    if (!isSessionId(sessionId)) {
        out.error = QStringLiteral("not a Claude Code session id");
        return out;
    }
    const QString home = findTranscript(projectsRootOf(claudeHome), sessionId);
    if (!home.isEmpty()) {
        out.ok = true;
        out.transcript = home;
        return out;
    }
    for (const QString &configDir : accountConfigDirs) {
        if (configDir.trimmed().isEmpty() || samePlace(configDir, claudeHome))
            continue;
        const QString transcript = findTranscript(projectsRootOf(configDir), sessionId);
        if (transcript.isEmpty())
            continue;
        const QString linked =
            linkProjectDir(QFileInfo(transcript).absolutePath(), configDir, claudeHome);
        if (linked.isEmpty()) {
            out.error = QStringLiteral("could not link %1 into %2")
                            .arg(QFileInfo(transcript).absolutePath(),
                                 projectsRootOf(claudeHome));
            return out;
        }
        out.ok = true;
        out.linked = true;
        out.transcript = QDir(linked).filePath(QFileInfo(transcript).fileName());
        return out;
    }
    out.error = QStringLiteral("no transcript for %1").arg(sessionId);
    return out;
}

int exposeAccount(const QString &accountConfigDir, const QString &claudeHome)
{
    if (accountConfigDir.trimmed().isEmpty() || samePlace(accountConfigDir, claudeHome))
        return 0;
    const QDir root(projectsRootOf(accountConfigDir));
    if (!root.exists())
        return 0;
    int linked = 0;
    const QStringList dirs =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QString &dir : dirs) {
        if (!linkProjectDir(root.filePath(dir), accountConfigDir, claudeHome).isEmpty())
            ++linked;
    }
    return linked;
}

QString latestSessionId(const QList<QJsonObject> &events)
{
    const QString id = forkmesh::agents::claudeResumeSessionId(events);
    return isSessionId(id) ? id.toLower() : QString();
}

} // namespace forkmesh::claudedesktop
