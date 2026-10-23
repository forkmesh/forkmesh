#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

struct ClaudeImportBatch {
    QList<QJsonObject> events;
    QString log;
    qint64 newOffset = 0;
    qint64 lastMs = 0;
};

namespace ClaudeSessionImport {

bool validSessionId(const QString &sessionId);

ClaudeImportBatch convertLines(const QList<QJsonObject> &lines, const QString &sessionId);

ClaudeImportBatch convert(const QString &path, qint64 fromOffset, const QString &sessionId);

QString stagedPath(const QString &configDir, const QString &workdir, const QString &sessionId);

bool stage(const QString &sourcePath, const QString &configDir, const QString &workdir,
           const QString &sessionId, QString *error);

} // namespace ClaudeSessionImport
