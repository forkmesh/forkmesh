#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace forkmesh::claudedesktop {

QString defaultClaudeHome();

bool isSessionId(const QString &id);

QUrl resumeUrl(const QString &sessionId);

QString findTranscript(const QString &projectsRoot, const QString &sessionId);

QString linkProjectDir(const QString &accountProjectDir, const QString &accountConfigDir,
                       const QString &claudeHome);

struct Exposure {
    bool ok = false;
    bool linked = false;
    QString transcript;
    QString error;
};

Exposure exposeSession(const QString &sessionId, const QStringList &accountConfigDirs,
                       const QString &claudeHome);

int exposeAccount(const QString &accountConfigDir, const QString &claudeHome);

QString latestSessionId(const QList<QJsonObject> &events);

} // namespace forkmesh::claudedesktop
