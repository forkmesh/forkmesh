#pragma once

#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QString>

namespace forkmesh::localcontrol {

inline bool handoffRefNameValid(const QString &name)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$"));
    if (!pattern.match(name).hasMatch())
        return false;
    if (name.contains(QLatin1String("..")) || name.contains(QLatin1String("//")) ||
        name.contains(QLatin1String("/.")) || name.endsWith(QLatin1Char('/')) ||
        name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1String(".lock")) ||
        name.contains(QLatin1String(".lock/")))
        return false;
    return true;
}

inline bool handoffBranchValid(const QString &branch)
{
    return branch.startsWith(QLatin1String("agent/")) && branch.size() > 6 &&
           handoffRefNameValid(branch);
}

inline bool takeoverSessionValid(const QString &sessionId)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$"));
    return pattern.match(sessionId).hasMatch();
}

inline int takeoverRepoIndex(const QList<QPair<QString, QString>> &checkouts,
                             const QString &owner, const QString &name)
{
    if (name.trimmed().isEmpty())
        return -1;
    int first = -1;
    QSet<QString> owners;
    for (int i = 0; i < checkouts.size(); ++i) {
        const auto &repo = checkouts.at(i);
        if (repo.second.isEmpty() ||
            repo.second.compare(name, Qt::CaseInsensitive) != 0)
            continue;
        if (!owner.isEmpty() && repo.first.compare(owner, Qt::CaseInsensitive) == 0)
            return i;
        if (first < 0)
            first = i;
        owners.insert(repo.first.toLower());
    }
    return owners.size() == 1 ? first : -1;
}

}
