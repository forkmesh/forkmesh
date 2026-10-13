#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace forkmesh::releasemanifest {

inline QString landedTag(const QByteArray &manifestBytes)
{
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes);
    if (!document.isObject())
        return QString();
    return document.object().value(QStringLiteral("tag")).toString().trimmed();
}

inline bool stillNamesAnotherRelease(const QByteArray &manifestBytes,
                                     const QString &wantedTag)
{
    const QString tag = landedTag(manifestBytes);
    return !tag.isEmpty() && !wantedTag.trimmed().isEmpty()
           && tag != wantedTag.trimmed();
}

}
