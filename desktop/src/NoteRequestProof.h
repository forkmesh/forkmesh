#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

namespace forkmesh::notes {

inline const QString kNotesRequestProof =
    QStringLiteral("forkmesh-notes-request-v1");

inline QString noteRequestResource(const QByteArray &method,
                                   const QString &path,
                                   const QByteArray &body)
{
    QString resource = QString::fromLatin1(method) + QLatin1Char('\n') + path;
    if (method != QByteArrayLiteral("GET")) {
        resource += QLatin1Char('\n') + QString::fromLatin1(
            QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());
    }
    return resource;
}

}
