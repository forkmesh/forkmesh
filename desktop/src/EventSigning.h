#pragma once

#include <QByteArray>
#include <QString>

namespace forkmesh::eventsigning {

inline QString repoKey(const QString &owner, const QString &name)
{
    const QString o = owner.trimmed().toLower();
    const QString n = name.trimmed().toLower();
    if (o.isEmpty() || n.isEmpty())
        return QString();
    for (const QString &part : {o, n}) {
        if (part.contains(QLatin1Char('/')) || part.contains(QLatin1Char('\n')) ||
            part.contains(QChar(QChar::Null)))
            return QString();
    }
    return o + QLatin1Char('/') + n;
}

inline QByteArray prefix(const char *kind, const QString &repoKey)
{
    QByteArray out("forkmesh-");
    out += kind;
    if (repoKey.isEmpty())
        return out + "-v1\n";
    return out + "-v2\n" + repoKey.toUtf8() + "\n";
}

}
