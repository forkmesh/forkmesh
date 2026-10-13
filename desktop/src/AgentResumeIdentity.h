#pragma once


#include <QJsonObject>
#include <QList>
#include <QString>

namespace forkmesh::agents {

inline QString resumeAccountKey()
{
    return QStringLiteral("_forkmesh_account");
}

inline bool transcriptEventIsCodex(const QJsonObject &ev)
{
    return !ev.value(QStringLiteral("thread_id")).toString().isEmpty() ||
           ev.value(QStringLiteral("provider")).toString() ==
               QLatin1String("codex");
}

inline bool transcriptEventMatchesAccount(const QJsonObject &ev,
                                          const QString &accountId)
{
    if (accountId.isEmpty())
        return true;
    const QString stamped = ev.value(resumeAccountKey()).toString();
    return stamped.isEmpty() || stamped == accountId;
}

inline QString codexResumeThreadId(const QList<QJsonObject> &events,
                                   const QString &accountId = QString())
{
    for (auto it = events.crbegin(); it != events.crend(); ++it) {
        if (!transcriptEventIsCodex(*it) ||
            !transcriptEventMatchesAccount(*it, accountId))
            continue;
        QString id = it->value(QStringLiteral("thread_id")).toString();
        if (id.isEmpty())
            id = it->value(QStringLiteral("session_id")).toString();
        if (!id.isEmpty())
            return id;
    }
    return QString();
}

inline QString claudeResumeSessionId(const QList<QJsonObject> &events,
                                     const QString &accountId = QString())
{
    for (auto it = events.crbegin(); it != events.crend(); ++it) {
        if (transcriptEventIsCodex(*it) ||
            !transcriptEventMatchesAccount(*it, accountId))
            continue;
        const QString id = it->value(QStringLiteral("session_id")).toString();
        if (!id.isEmpty())
            return id;
    }
    return QString();
}

inline QString resumeConversationAccountId(const QList<QJsonObject> &events,
                                           bool codexTransport)
{
    for (auto it = events.crbegin(); it != events.crend(); ++it) {
        if (transcriptEventIsCodex(*it) != codexTransport)
            continue;
        const bool hasId =
            !it->value(QStringLiteral("session_id")).toString().isEmpty() ||
            !it->value(QStringLiteral("thread_id")).toString().isEmpty();
        if (hasId)
            return it->value(resumeAccountKey()).toString();
    }
    return QString();
}

} // namespace forkmesh::agents
