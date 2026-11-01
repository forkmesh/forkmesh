#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

struct ExternalClaudeSession {
    QString uuid;           // jsonl file stem == the CLI's session id
    QString path;           // absolute path to the .jsonl transcript
    QString cwd;            // working directory the session runs in
    QString gitBranch;      // git branch recorded in the transcript (may be empty)
    QString title;          // the session's AI-generated title, when present
    QString firstPrompt;
    QString model;
    QString entrypoint;
    qint64 createdMs = 0;
    qint64 lastActivityMs = 0; // transcript mtime — last time it produced output
    qint64 bytes = 0;
};

namespace ClaudeSessionScan {

QList<ExternalClaudeSession> scan(const QString &repoLocalPath,
                                  const QSet<QString> &excludeCwds,
                                  qint64 activeWindowMs,
                                  const QStringList &ownConfigDirs = {});

QList<ExternalClaudeSession> listAll(const QStringList &configDirs,
                                     const QString &repoLocalPath,
                                     bool anyFolder,
                                     const QStringList &skipRoots = {});

void readMeta(ExternalClaudeSession &s);

QString promptTitle(const QString &prompt);

QString desktopConfigRoot();

QHash<QString, QString> desktopTitles(const QString &desktopRoot);

QString projectKey(const QString &cwd);

int removeOrphanedTranscripts(const QStringList &configDirs, const QString &cwd,
                              QStringList *removed = nullptr);

QList<QJsonObject> readEvents(const QString &path, qint64 fromOffset,
                              qint64 *newOffset);

qint64 tailStartOffset(const QString &path, qint64 maxBytes);

QList<qint64> findSessionPids(const QString &uuid, const QString &cwd);
// The same match over `claudePids`, the processes already known to be named
// `claude` (from a walk of /proc the caller made anyway), without walking it
// again.
QList<qint64> findSessionPids(const QString &uuid, const QString &cwd,
                              const QList<qint64> &claudePids);

} // namespace ClaudeSessionScan
