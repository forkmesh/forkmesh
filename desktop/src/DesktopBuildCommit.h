#pragma once

#include <QRegularExpression>
#include <QString>

namespace forkmesh {

QString desktopBuildCommit();

inline QString shortBuildCommit(const QString &commit)
{
    static const QRegularExpression exact(
        QStringLiteral("^[0-9a-f]{40}([0-9a-f]{24})?$"));
    if (exact.match(commit).hasMatch())
        return commit.left(10);
    if (commit == QLatin1String("dirty"))
        return commit;
    return QString();
}

}
