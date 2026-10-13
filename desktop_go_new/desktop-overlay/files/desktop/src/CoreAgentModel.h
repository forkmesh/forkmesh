#pragma once

#include "LocalControl.h"

#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

namespace forkmesh::coreagentmodel {

inline QMutex &secretMutex()
{
    static QMutex mutex;
    return mutex;
}

inline QString &secretValue()
{
    static QString secret;
    return secret;
}

inline void setConfirmSecret(const QString &secret)
{
    const QMutexLocker lock(&secretMutex());
    secretValue() = secret;
}

inline QString confirmSecret()
{
    const QMutexLocker lock(&secretMutex());
    return secretValue();
}

inline bool confirmedByCore(int connectionId, const QJsonObject &params)
{
    if (connectionId < LocalControlServer::kCoreConnectionBase)
        return false;
    const QString secret = confirmSecret();
    return secret.size() >= 16 &&
           params.value(QStringLiteral("_coreConfirmed")).toString() == secret;
}

} // namespace forkmesh::coreagentmodel
