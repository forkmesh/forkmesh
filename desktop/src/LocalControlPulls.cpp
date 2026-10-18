#include "LocalControlPulls.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace forkmesh::localcontrol::pulls {

QString familyPrefix()
{
    return QStringLiteral("pulls.");
}

QStringList actions()
{
    return {QStringLiteral("merge-all-ai")};
}

bool isKnownAction(const QString &action)
{
    return actions().contains(action);
}

bool validRepoSegment(const QString &segment)
{
    static const QRegularExpression allowed(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$"));
    return allowed.match(segment).hasMatch() && !segment.contains(QLatin1String(".."));
}

bool validRequest(const QString &owner, const QString &name, const QString &action)
{
    return validRepoSegment(owner) && validRepoSegment(name) && isKnownAction(action);
}

QString confirmationTarget(const QString &owner, const QString &name,
                           const QString &action)
{
    return action + QLatin1Char(':') + owner.toLower() + QLatin1Char('/') + name.toLower();
}

QString actionSummary(const QString &owner, const QString &name, const QString &action)
{
    if (action == QLatin1String("merge-all-ai"))
        return QStringLiteral(
                   "Start an AI agent on this desktop, in %1/%2's own checkout with no "
                   "branch and no permission prompts. It reviews every open pull "
                   "request for security issues, bugs and code that does not fit, "
                   "merges the safe ones into their base, and ForkMesh then deletes "
                   "the merged pull requests and their branches. Skipped pull "
                   "requests stay open, and the agent does not push.")
            .arg(owner, name);
    return QString();
}

QJsonObject launchReply(const forkmesh::pullmergeall::Launch &launch)
{
    QJsonArray skipped;
    for (const QString &entry : launch.skipped) {
        if (skipped.size() >= kMaxSkippedInReply)
            break;
        skipped.append(entry);
    }
    return QJsonObject{{QStringLiteral("message"),
                        forkmesh::pullmergeall::launchMessage(launch)},
                       {QStringLiteral("sessionId"), launch.sessionId},
                       {QStringLiteral("pulls"), launch.count},
                       {QStringLiteral("skipped"), skipped}};
}

}
