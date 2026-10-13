#include "LocalControlMergeQueue.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace forkmesh::localcontrol::mergequeue {

QString familyPrefix()
{
    return QStringLiteral("mergequeue.");
}

QStringList actions()
{
    return {QStringLiteral("enqueue"), QStringLiteral("remove"),
            QStringLiteral("move"),    QStringLiteral("pause"),
            QStringLiteral("resume"),  QStringLiteral("enable"),
            QStringLiteral("disable"), QStringLiteral("clear"),
            QStringLiteral("process")};
}

bool isKnownAction(const QString &action)
{
    return actions().contains(action);
}

bool needsNumber(const QString &action)
{
    return action == QLatin1String("enqueue") || action == QLatin1String("remove") ||
           action == QLatin1String("move");
}

bool validRepoSegment(const QString &segment)
{
    static const QRegularExpression allowed(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$"));
    return allowed.match(segment).hasMatch() && !segment.contains(QLatin1String(".."));
}

bool validRequest(const QString &owner, const QString &name, const QString &action,
                  int number, int delta)
{
    if (!validRepoSegment(owner) || !validRepoSegment(name) || !isKnownAction(action))
        return false;
    if (needsNumber(action) && (number <= 0 || number > 10000000))
        return false;
    if (action == QLatin1String("move") && (delta == 0 || delta < -kMaxEntries ||
                                            delta > kMaxEntries))
        return false;
    return true;
}

QString confirmationTarget(const QString &owner, const QString &name,
                           const QString &action, int number, int delta)
{
    QString target = owner.toLower() + QLatin1Char('/') + name.toLower();
    if (needsNumber(action))
        target += QStringLiteral("#%1").arg(number);
    if (action == QLatin1String("move"))
        target += QStringLiteral(":%1").arg(delta);
    return target;
}

QString actionSummary(const QString &owner, const QString &name,
                      const QString &action, int number, int delta)
{
    const QString repo = owner + QLatin1Char('/') + name;
    if (action == QLatin1String("enqueue"))
        return QStringLiteral("Add pull request #%1 to the merge queue of %2. The "
                              "desktop merges it when its turn comes and the "
                              "review and checks allow it.")
            .arg(number)
            .arg(repo);
    if (action == QLatin1String("remove"))
        return QStringLiteral("Take pull request #%1 out of the merge queue of %2. "
                              "It stays open.")
            .arg(number)
            .arg(repo);
    if (action == QLatin1String("move"))
        return QStringLiteral("Move pull request #%1 %2 %3 place%4 in the merge "
                              "queue of %5.")
            .arg(number)
            .arg(delta < 0 ? QStringLiteral("up") : QStringLiteral("down"))
            .arg(qAbs(delta))
            .arg(qAbs(delta) == 1 ? QString() : QStringLiteral("s"))
            .arg(repo);
    if (action == QLatin1String("pause"))
        return QStringLiteral("Pause the merge queue of %1. Nothing merges until "
                              "it is resumed.")
            .arg(repo);
    if (action == QLatin1String("resume"))
        return QStringLiteral("Resume the merge queue of %1.").arg(repo);
    if (action == QLatin1String("enable"))
        return QStringLiteral("Turn on the merge queue for %1.").arg(repo);
    if (action == QLatin1String("disable"))
        return QStringLiteral("Turn off the merge queue for %1. Queued pull "
                              "requests stay listed but nothing merges.")
            .arg(repo);
    if (action == QLatin1String("clear"))
        return QStringLiteral("Remove every pull request from the merge queue of "
                              "%1. They all stay open.")
            .arg(repo);
    if (action == QLatin1String("process"))
        return QStringLiteral("Merge the next ready pull request in the queue of "
                              "%1 now, on this desktop.")
            .arg(repo);
    return {};
}

QString precheck(const RepoState &repo, const QString &action, int number,
                 const QString &pullStatus)
{
    if (!repo.writable)
        return QStringLiteral("read_only");
    if (action == QLatin1String("enable") || action == QLatin1String("disable"))
        return {};
    if (!repo.enabled)
        return QStringLiteral("queue_off");
    if (action == QLatin1String("enqueue")) {
        if (!pullStatus.isEmpty() && pullStatus != QLatin1String("open"))
            return QStringLiteral("not_open_pull");
        if (repo.queue.contains(number))
            return QStringLiteral("already_queued");
        if (repo.queue.size() >= kMaxEntries)
            return QStringLiteral("queue_full");
    }
    if ((action == QLatin1String("remove") || action == QLatin1String("move")) &&
        !repo.queue.contains(number))
        return QStringLiteral("not_queued");
    if (action == QLatin1String("move")) {
        const QString state = repo.queue.at(number).state;
        if (state == MergeQueueState::Updating || state == MergeQueueState::Merging)
            return QStringLiteral("busy");
    }
    if (action == QLatin1String("process")) {
        if (repo.paused)
            return QStringLiteral("paused");
        if (!repo.open)
            return QStringLiteral("repo_not_open");
    }
    return {};
}

QString applyToQueue(MergeQueue &queue, const QString &action, int number,
                     int delta, qint64 nowMs)
{
    if (action == QLatin1String("enqueue"))
        return queue.enqueue(number, nowMs) ? QString() : QStringLiteral("already_queued");
    if (action == QLatin1String("remove"))
        return queue.remove(number) ? QString() : QStringLiteral("not_queued");
    if (action == QLatin1String("move")) {
        if (!queue.contains(number))
            return QStringLiteral("not_queued");
        queue.move(number, delta);
        return {};
    }
    if (action == QLatin1String("clear")) {
        queue.clear();
        return {};
    }
    return QStringLiteral("unsupported_action");
}

QString resultMessage(const QString &action, int number, const MergeQueue &queue)
{
    const int place = queue.indexOf(number) + 1;
    if (action == QLatin1String("enqueue"))
        return QStringLiteral("Pull request #%1 joined the merge queue at place %2.")
            .arg(number)
            .arg(place);
    if (action == QLatin1String("remove"))
        return QStringLiteral("Pull request #%1 left the merge queue.").arg(number);
    if (action == QLatin1String("move"))
        return QStringLiteral("Pull request #%1 is now at place %2.").arg(number).arg(place);
    if (action == QLatin1String("pause"))
        return QStringLiteral("Merge queue paused.");
    if (action == QLatin1String("resume"))
        return QStringLiteral("Merge queue resumed.");
    if (action == QLatin1String("enable"))
        return QStringLiteral("Merge queue turned on.");
    if (action == QLatin1String("disable"))
        return QStringLiteral("Merge queue turned off.");
    if (action == QLatin1String("clear"))
        return QStringLiteral("Merge queue cleared.");
    if (action == QLatin1String("process"))
        return QStringLiteral("The desktop is working through the queue.");
    return {};
}

QJsonObject repoRow(const RepoState &repo, const QHash<int, QString> &titles)
{
    QJsonArray entries;
    for (const MergeQueueEntry &entry : repo.queue.entries()) {
        if (entries.size() >= kMaxEntries)
            break;
        entries.append(QJsonObject{
            {QStringLiteral("number"), entry.number},
            {QStringLiteral("title"), titles.value(entry.number).left(kMaxTitleChars)},
            {QStringLiteral("state"), entry.state},
            {QStringLiteral("detail"), MergeQueue::sanitizeDetail(entry.detail)},
            {QStringLiteral("queuedAtMs"), double(entry.queuedAtMs)},
            {QStringLiteral("stateAtMs"), double(entry.stateAtMs)},
        });
    }
    return QJsonObject{
        {QStringLiteral("owner"), repo.owner},
        {QStringLiteral("name"), repo.name},
        {QStringLiteral("enabled"), repo.enabled},
        {QStringLiteral("paused"), repo.paused},
        {QStringLiteral("open"), repo.open},
        {QStringLiteral("writable"), repo.writable},
        {QStringLiteral("summary"), repo.enabled ? repo.queue.summary() : QStringLiteral("Off")},
        {QStringLiteral("entries"), entries},
    };
}

} // namespace forkmesh::localcontrol::mergequeue
