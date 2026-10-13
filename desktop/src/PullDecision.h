#pragma once

#include <QLatin1String>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace forkmesh::pulldecision {

inline QString decidedPullMessage(const QString &status)
{
    return QStringLiteral("This pull request is already %1.").arg(status);
}

inline bool decidedPullError(const QString &error)
{
    static const QRegularExpression pattern(
        QStringLiteral("^This pull request is already (\\S+)\\.$"));
    const QRegularExpressionMatch match = pattern.match(error.trimmed());
    return match.hasMatch() && match.captured(1) != QLatin1String("open");
}

inline QString localChangesBlockMessage(int number, const QString &checkout,
                                        const QString &gitError)
{
    QStringList files;
    for (const QString &line : gitError.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1Char('\t')) && !line.trimmed().isEmpty())
            files.append(line.trimmed());
    }
    QString message =
        QStringLiteral("Pull request #%1 changes files that have uncommitted "
                       "changes in %2. Commit or stash them, then merge again.")
            .arg(number)
            .arg(checkout);
    if (!files.isEmpty())
        message += QStringLiteral(" (%1)").arg(files.mid(0, 5).join(
            QStringLiteral(", ")));
    return message;
}

inline bool localChangesBlockError(const QString &error)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^Pull request #\\d+ changes files that have uncommitted changes in "));
    return pattern.match(error.trimmed()).hasMatch();
}

}
