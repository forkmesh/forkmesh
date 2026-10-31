#include "AgentCloudSessions.h"
#include "LocalControlHandoff.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace forkmesh::agents {

namespace {

qint64 jsonMs(const QJsonObject &object, const QString &key)
{
    const double value = object.value(key).toDouble();
    return value > 0 ? static_cast<qint64>(value) : 0;
}

int jsonCount(const QJsonObject &object, const QString &key)
{
    const double value = object.value(key).toDouble();
    return value > 0 ? static_cast<int>(value) : 0;
}

QString cloudImageExtension(const QString &mediaType)
{
    static const QHash<QString, QString> extensions{
        {QStringLiteral("image/png"), QStringLiteral("png")},
        {QStringLiteral("image/jpeg"), QStringLiteral("jpg")},
        {QStringLiteral("image/gif"), QStringLiteral("gif")},
        {QStringLiteral("image/webp"), QStringLiteral("webp")},
    };
    return extensions.value(mediaType);
}

QList<CloudAgentImage> parseCloudImages(const QJsonArray &raw)
{
    static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{32}$"));
    constexpr qint64 kMaxImageBytes = 5 << 20;
    QList<CloudAgentImage> images;
    for (const QJsonValue &value : raw) {
        const QJsonObject object = value.toObject();
        CloudAgentImage image;
        image.id = object.value(QStringLiteral("id")).toString();
        image.mediaType = object.value(QStringLiteral("mediaType")).toString();
        image.size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
        if (idPattern.match(image.id).hasMatch() &&
            !cloudImageExtension(image.mediaType).isEmpty() && image.size > 0 &&
            image.size <= kMaxImageBytes)
            images.append(image);
    }
    return images;
}

QString cloudToolInputKey(const QString &name)
{
    static const QHash<QString, QString> keys{
        {QStringLiteral("Bash"), QStringLiteral("command")},
        {QStringLiteral("Read"), QStringLiteral("file_path")},
        {QStringLiteral("Edit"), QStringLiteral("file_path")},
        {QStringLiteral("MultiEdit"), QStringLiteral("file_path")},
        {QStringLiteral("Write"), QStringLiteral("file_path")},
        {QStringLiteral("NotebookEdit"), QStringLiteral("file_path")},
        {QStringLiteral("Grep"), QStringLiteral("pattern")},
        {QStringLiteral("Glob"), QStringLiteral("pattern")},
        {QStringLiteral("WebSearch"), QStringLiteral("query")},
        {QStringLiteral("ToolSearch"), QStringLiteral("query")},
        {QStringLiteral("WebFetch"), QStringLiteral("url")},
        {QStringLiteral("LS"), QStringLiteral("path")},
    };
    return keys.value(name, QStringLiteral("description"));
}

QList<QJsonArray> cloudAssistantSegments(const QString &text,
                                         const QString &idPrefix)
{
    static const QRegularExpression toolLine(
        QStringLiteral("^\\x{25b8} ([^\\s:]+)(?:: (.*))?$"));
    QList<QJsonArray> segments;
    QStringList prose;
    QJsonArray tools;
    int toolCount = 0;
    const auto flushProse = [&] {
        const QString joined = prose.join(QStringLiteral("\n\n")).trimmed();
        prose.clear();
        if (!joined.isEmpty())
            segments.append(QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), joined}}});
    };
    const auto flushTools = [&] {
        if (!tools.isEmpty())
            segments.append(tools);
        tools = QJsonArray();
    };
    for (const QString &chunk : text.split(QStringLiteral("\n\n"))) {
        const QString line = chunk.trimmed();
        const QRegularExpressionMatch match =
            line.contains(QLatin1Char('\n')) ? QRegularExpressionMatch()
                                               : toolLine.match(line);
        if (!match.hasMatch()) {
            flushTools();
            prose.append(chunk);
            continue;
        }
        flushProse();
        const QString name = match.captured(1);
        QJsonObject input{{QStringLiteral("_summaryOnly"), true}};
        const QString summary = match.captured(2).trimmed();
        if (!summary.isEmpty())
            input.insert(cloudToolInputKey(name), summary);
        tools.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("tool_use")},
            {QStringLiteral("id"),
             idPrefix + QString::number(++toolCount)},
            {QStringLiteral("name"), name},
            {QStringLiteral("input"), input}});
    }
    flushTools();
    flushProse();
    if (!segments.isEmpty() &&
        segments.last().first().toObject().value(QStringLiteral("type")).toString() ==
            QLatin1String("tool_use"))
        segments.append(QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), QString()}}});
    return segments;
}

} // namespace

QString cloudAgentTaskId(const QString &taskKey, const QString &rawTitle)
{
    static const QRegularExpression keyed(
        QStringLiteral("^task:([0-9a-f]{32})$"));
    static const QRegularExpression tagged(
        QStringLiteral("^\\s*\\[task:([0-9a-fA-F]{32})\\]"));
    QRegularExpressionMatch match = keyed.match(taskKey.trimmed());
    if (match.hasMatch())
        return match.captured(1);
    match = tagged.match(rawTitle);
    return match.hasMatch() ? match.captured(1).toLower() : QString();
}

QString cloudAgentDisplayTitle(const QString &raw)
{
    static const QRegularExpression taskTag(
        QStringLiteral("^\\s*\\[task:[0-9a-fA-F]{8,64}\\]\\s*"));
    QString title = raw;
    title.remove(taskTag);
    return title.simplified();
}

QString cloudAgentShortTitle(const QString &title, int maxChars)
{
    const QString clean = title.simplified();
    if (clean.size() <= maxChars)
        return clean;
    QString cut = clean.left(maxChars);
    const int space = cut.lastIndexOf(QLatin1Char(' '));
    if (space > maxChars / 2)
        cut.truncate(space);
    return cut.trimmed() + QString::fromUtf8("\xE2\x80\xA6");
}

QString CloudAgentSession::key() const
{
    return owner.toLower() + QLatin1Char('/') + name.toLower() +
           QLatin1Char('/') + sessionId;
}

QString CloudAgentSession::relayRepoOwner() const
{
    return relayOwner.isEmpty() ? owner : relayOwner;
}

void fileCloudAgentSessionsUnderLocalRepos(
    QList<CloudAgentSession> &sessions,
    const QList<QPair<QString, QString>> &localRepos)
{
    for (CloudAgentSession &cloud : sessions) {
        if (cloud.relayOwner.isEmpty())
            cloud.relayOwner = cloud.owner;
        bool exact = false;
        QString onlyOwner;
        int sameName = 0;
        for (const auto &repo : localRepos) {
            if (repo.second.compare(cloud.name, Qt::CaseInsensitive) != 0)
                continue;
            if (repo.first.compare(cloud.relayOwner, Qt::CaseInsensitive) == 0) {
                exact = true;
                break;
            }
            if (onlyOwner.compare(repo.first, Qt::CaseInsensitive) != 0) {
                onlyOwner = repo.first;
                ++sameName;
            }
        }
        cloud.owner = (!exact && sameName == 1) ? onlyOwner : cloud.relayOwner;
    }
}

qint64 cloudAgentAccountFinishedBefore(const QJsonObject &payload)
{
    return jsonMs(payload, QStringLiteral("finishedBefore"));
}

bool cloudAgentBeyondListedWindow(const CloudAgentSession &cloud,
                                  qint64 finishedBeforeMs)
{
    if (finishedBeforeMs <= 0 || cloud.updatedAtMs <= 0 ||
        cloud.updatedAtMs > finishedBeforeMs)
        return false;
    return cloud.status != QLatin1String("queued") &&
           cloud.status != QLatin1String("running") &&
           cloud.status != QLatin1String("security_pending");
}

bool CloudAgentSession::promptable() const
{
    if (localAgentId <= 0)
        return false;
    return status == QLatin1String("running") ||
           status == QLatin1String("queued") ||
           status == QLatin1String("completed") ||
           status == QLatin1String("failed");
}

bool CloudAgentSession::claimable() const
{
    if (localAgentId > 0)
        return false; // a node already started it; its result is coming
    return status == QLatin1String("queued") ||
           status == QLatin1String("security_pending");
}

QString cloudAgentLocalStatus(const QString &status)
{
    if (status == QLatin1String("running"))
        return AgentStatus::Running;
    if (status == QLatin1String("completed"))
        return AgentStatus::Success;
    if (status == QLatin1String("failed") || status == QLatin1String("rejected"))
        return AgentStatus::Failed;
    // queued, security_pending, and anything a newer relay invents: not started.
    return AgentStatus::Queued;
}

namespace {

// One payload's rows. `owner`/`name` address a repository list; the
// account-wide list passes them empty and every row names its own.
QList<CloudAgentSession> parseCloudAgentRows(const QJsonObject &payload,
                                             const QString &owner,
                                             const QString &name,
                                             const QString &thisNode)
{
    QList<CloudAgentSession> sessions;
    const QString self = thisNode.trimmed().toLower();
    const QJsonArray rows = payload.value(QStringLiteral("sessions")).toArray();
    for (const QJsonValue &rowValue : rows) {
        const QJsonObject row = rowValue.toObject();
        CloudAgentSession cloud;
        cloud.sessionId = row.value(QStringLiteral("id")).toString().trimmed();
        if (cloud.sessionId.isEmpty())
            continue;
        cloud.targetNode =
            row.value(QStringLiteral("targetNode")).toString().trimmed();
        cloud.targetsThisNode =
            !self.isEmpty() && cloud.targetNode.toLower() == self;
        cloud.onRunner = row.value(QStringLiteral("onRunner")).toBool();
        cloud.owner = owner.isEmpty()
                          ? row.value(QStringLiteral("org")).toString().trimmed()
                          : owner;
        cloud.name = name.isEmpty()
                         ? row.value(QStringLiteral("repo")).toString().trimmed()
                         : name;
        // A row the relay could not address belongs to no repository in the
        // list, and every action on it (branch, pull request, follow-up) is
        // built from that pair.
        if (cloud.owner.isEmpty() || cloud.name.isEmpty())
            continue;
        cloud.relayOwner = cloud.owner;
        cloud.provider =
            row.value(QStringLiteral("provider")).toString().trimmed().toLower();
        cloud.status =
            row.value(QStringLiteral("status")).toString().trimmed().toLower();
        cloud.taskId = cloudAgentTaskId(
            row.value(QStringLiteral("taskKey")).toString(),
            row.value(QStringLiteral("title")).toString());
        cloud.title = cloudAgentDisplayTitle(
            row.value(QStringLiteral("title")).toString());
        cloud.createdBy =
            row.value(QStringLiteral("createdBy")).toString().trimmed();
        cloud.localAgentId = jsonCount(row, QStringLiteral("localAgentId"));
        cloud.issueNumber = jsonCount(row, QStringLiteral("issueNumber"));
        cloud.createPr = row.value(QStringLiteral("createPr")).toBool();
        cloud.createdAtMs = jsonMs(row, QStringLiteral("createdAt"));
        cloud.updatedAtMs = jsonMs(row, QStringLiteral("updatedAt"));
        cloud.completedAtMs = jsonMs(row, QStringLiteral("completedAt"));
        cloud.model =
            row.value(QStringLiteral("requestedModel")).toString().trimmed();

        const QJsonObject diagnostic =
            row.value(QStringLiteral("diagnostic")).toObject();
        cloud.diagnostic =
            diagnostic.value(QStringLiteral("message")).toString().trimmed();
        cloud.needsAttention =
            diagnostic.value(QStringLiteral("level")).toString() ==
            QLatin1String("attention");
        cloud.diagnosticCode =
            diagnostic.value(QStringLiteral("code")).toString().trimmed();

        // What the claiming node reported about its own run: the branch it
        // cut, the pull request it opened, what the run cost.
        const QJsonObject info = row.value(QStringLiteral("agentInfo")).toObject();
        cloud.branchName =
            info.value(QStringLiteral("branchName")).toString().trimmed();
        cloud.baseBranch =
            info.value(QStringLiteral("baseBranch")).toString().trimmed();
        cloud.baseRef = info.value(QStringLiteral("baseRef")).toString().trimmed();
        cloud.prNumber = jsonCount(info, QStringLiteral("prNumber"));
        cloud.merged = info.value(QStringLiteral("merged")).toBool();
        if (cloud.issueNumber <= 0)
            cloud.issueNumber = jsonCount(info, QStringLiteral("issueNumber"));
        const QString reportedModel =
            info.value(QStringLiteral("model")).toString().trimmed();
        if (!reportedModel.isEmpty())
            cloud.model = reportedModel;
        cloud.promptTokens = jsonCount(info, QStringLiteral("promptTokens"));
        cloud.completionTokens =
            jsonCount(info, QStringLiteral("completionTokens"));
        cloud.totalTokens = jsonCount(info, QStringLiteral("totalTokens"));
        cloud.costUsd = qMax(0.0, info.value(QStringLiteral("costUsd")).toDouble());
        cloud.startedAtMs = jsonMs(info, QStringLiteral("startedAt"));
        cloud.finishedAtMs = jsonMs(info, QStringLiteral("finishedAt"));
        cloud.mergedAtMs = jsonMs(info, QStringLiteral("mergedAt"));
        cloud.durationMs = jsonMs(info, QStringLiteral("durationMs"));

        if (cloud.branchName.isEmpty())
            cloud.branchName =
                row.value(QStringLiteral("branch")).toString().trimmed();
        if (cloud.baseBranch.isEmpty())
            cloud.baseBranch =
                row.value(QStringLiteral("baseBranch")).toString().trimmed();
        cloud.runner = row.value(QStringLiteral("runner")).toString().trimmed();
        const QJsonObject merge = row.value(QStringLiteral("merge")).toObject();
        cloud.mergeState =
            merge.value(QStringLiteral("state")).toString().trimmed().toLower();
        if (cloud.mergeState == QLatin1String("merged"))
            cloud.merged = true;
        cloud.canMerge = row.value(QStringLiteral("canMerge")).toBool();
        cloud.canOpenPull = row.value(QStringLiteral("canOpenPull")).toBool();
        cloud.branchDeleted =
            merge.value(QStringLiteral("branchDeleted")).toBool();
        cloud.canCleanup =
            cloud.mergeState == QLatin1String("merged") &&
            (!cloud.branchDeleted ||
             !merge.value(QStringLiteral("cleanupPending")).toArray().isEmpty());
        const int linkedPull = jsonCount(row, QStringLiteral("prNumber"));
        if (linkedPull > 0)
            cloud.prNumber = linkedPull;
        if (cloud.prNumber > 0)
            cloud.canOpenPull = false;
        const QJsonObject stats =
            row.value(QStringLiteral("diffStats")).toObject();
        cloud.diffKnown = stats.value(QStringLiteral("known")).toBool();
        cloud.diffFiles = jsonCount(stats, QStringLiteral("files"));
        cloud.diffAdded = jsonCount(stats, QStringLiteral("added"));
        cloud.diffRemoved = jsonCount(stats, QStringLiteral("removed"));
        cloud.diffAhead = jsonCount(stats, QStringLiteral("ahead"));

        cloud.historyTruncated =
            row.value(QStringLiteral("historyTruncated")).toBool();
        cloud.organization = row.value(QStringLiteral("scope")).toString() ==
                             QLatin1String("organization");
        const QJsonArray history = row.value(QStringLiteral("history")).toArray();
        for (const QJsonValue &turnValue : history) {
            const QJsonObject turnObject = turnValue.toObject();
            CloudAgentTurn turn;
            turn.role = turnObject.value(QStringLiteral("role")).toString();
            turn.author = turnObject.value(QStringLiteral("author")).toString();
            turn.text = turnObject.value(QStringLiteral("text")).toString();
            turn.atMs = jsonMs(turnObject, QStringLiteral("at"));
            if (turn.role == QLatin1String("user"))
                turn.images = parseCloudImages(
                    turnObject.value(QStringLiteral("attachments")).toArray());
            if (!turn.text.trimmed().isEmpty() || !turn.images.isEmpty())
                cloud.history.append(turn);
        }
        // Mirror nodes before 2026-09-24 appended the run's error to the
        // transcript as well as reporting it as lastError, which the relay
        // also serves as the diagnostic (the detail's Attention row). Drop
        // that trailer so the error shows once.
        const QString lastError =
            info.value(QStringLiteral("lastError")).toString().trimmed();
        cloud.lastError = lastError;
        if (!lastError.isEmpty() && !cloud.history.isEmpty() &&
            cloud.history.last().role != QLatin1String("user")) {
            QString &text = cloud.history.last().text;
            const QString trailer = QStringLiteral("\n\n") + lastError;
            QString trimmed = text;
            while (!trimmed.isEmpty() && trimmed.back().isSpace())
                trimmed.chop(1);
            if (trimmed.endsWith(trailer) && trimmed.size() > trailer.size())
                text = trimmed.left(trimmed.size() - trailer.size());
        }
        sessions.append(cloud);
    }
    return sessions;
}

} // namespace

QList<CloudAgentSession> parseCloudAgentSessions(const QJsonObject &payload,
                                                 const QString &owner,
                                                 const QString &name,
                                                 const QString &thisNode)
{
    return parseCloudAgentRows(payload, owner, name, thisNode);
}

QList<CloudAgentSession> parseCloudAgentAccountSessions(
    const QJsonObject &payload, const QString &thisNode)
{
    return parseCloudAgentRows(payload, QString(), QString(), thisNode);
}

AgentSession cloudAgentListEntry(const CloudAgentSession &cloud, int id)
{
    AgentSession session;
    session.id = id;
    session.cloud = true;
    session.cloudSessionId = cloud.sessionId;
    session.cloudNode = cloud.targetNode;
    session.orgTaskId = cloud.taskId;
    session.cloudHere = cloud.targetsThisNode;
    session.owner = cloud.owner;
    session.name = cloud.name;
    session.provider = cloud.provider;
    session.model = cloud.model;
    session.status = cloudAgentLocalStatus(cloud.status);
    session.issueNumber = cloud.issueNumber;
    session.issueTitle = cloud.title;
    for (const CloudAgentTurn &turn : cloud.history) {
        if (turn.role == QLatin1String("user")) {
            session.prompt = turn.text;
            break;
        }
    }
    if (session.issueTitle.isEmpty())
        session.issueTitle =
            session.prompt.section(QLatin1Char('\n'), 0, 0).simplified().left(160);
    if (session.issueTitle.isEmpty())
        session.issueTitle = QStringLiteral("Cloud agent");
    session.createPr = cloud.createPr;
    session.prNumber = cloud.prNumber;
    session.branchName = cloud.branchName;
    session.baseBranch = cloud.baseBranch;
    session.baseRef = cloud.baseRef;
    session.merged = cloud.merged;
    session.mergedAtMs = cloud.mergedAtMs;
    session.createdAtMs = cloud.createdAtMs;
    session.startedAtMs = cloud.startedAtMs;
    // The relay's last word on the row stands in for an end time the node has
    // not reported, so "updated" in the list is when the cloud last moved.
    session.finishedAtMs = qMax(cloud.finishedAtMs,
                                qMax(cloud.completedAtMs, cloud.updatedAtMs));
    session.durationMs = cloud.durationMs;
    session.promptTokens = cloud.promptTokens;
    session.completionTokens = cloud.completionTokens;
    session.totalTokens = cloud.totalTokens;
    session.costUsd = cloud.costUsd;
    return session;
}

QString cloudAgentImageFileName(const CloudAgentSession &cloud,
                                const CloudAgentImage &image)
{
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_-]"));
    const QString extension = cloudImageExtension(image.mediaType);
    QString session = cloud.sessionId;
    session.replace(unsafe, QString());
    if (session.isEmpty() || extension.isEmpty() || image.id.isEmpty())
        return QString();
    return QStringLiteral("cloud-%1-%2.%3").arg(session.left(64), image.id, extension);
}

QList<QJsonObject> cloudAgentTranscriptEvents(const CloudAgentSession &cloud,
                                              const QString &imageDirectory)
{
    QList<QJsonObject> events;
    events.reserve(cloud.history.size());
    for (int index = 0; index < cloud.history.size(); ++index) {
        const CloudAgentTurn &turn = cloud.history.at(index);
        const bool user = turn.role == QLatin1String("user");
        QList<QJsonValue> contents;
        if (user) {
            QString text = turn.text;
            for (const CloudAgentImage &image : turn.images) {
                const QString name = cloudAgentImageFileName(cloud, image);
                if (imageDirectory.isEmpty() || name.isEmpty())
                    continue;
                const QString path = QDir(imageDirectory).filePath(name);
                if (!QFileInfo::exists(path))
                    continue;
                if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
                    text += QLatin1Char('\n');
                text += QStringLiteral("Attached image: ") + path;
            }
            contents.append(text);
        } else {
            const QString idPrefix = QStringLiteral("cloud-%1-%2-")
                                         .arg(cloud.sessionId)
                                         .arg(index);
            for (const QJsonArray &segment :
                 cloudAssistantSegments(turn.text, idPrefix))
                contents.append(segment);
        }
        for (const QJsonValue &content : std::as_const(contents)) {
            QJsonObject event{
                {QStringLiteral("type"),
                 user ? QStringLiteral("user") : QStringLiteral("assistant")},
                {QStringLiteral("message"),
                 QJsonObject{{QStringLiteral("role"),
                              user ? QStringLiteral("user")
                                   : QStringLiteral("assistant")},
                             {QStringLiteral("content"), content}}}};
            if (turn.atMs > 0)
                event.insert(QStringLiteral("_ts"),
                             static_cast<double>(turn.atMs));
            events.append(event);
        }
    }
    return events;
}

bool cloudAgentFinished(const CloudAgentSession &cloud)
{
    static const QStringList finished = {
        QStringLiteral("failed"),    QStringLiteral("rejected"),
        QStringLiteral("completed"), QStringLiteral("cancelled"),
        QStringLiteral("canceled"),  QStringLiteral("stopped"),
        QStringLiteral("expired"),   QStringLiteral("timeout"),
    };
    return finished.contains(cloud.status);
}

bool cloudAgentNeedsRunnerSignIn(const CloudAgentSession &cloud)
{
    if (cloud.diagnosticCode == QLatin1String("provider_credential_missing"))
        return true;
    if (cloud.status != QLatin1String("failed") &&
        cloud.status != QLatin1String("rejected"))
        return false;
    static const QStringList signs = {
        QStringLiteral("oauth session expired"),
        QStringLiteral("oauth token has expired"),
        QStringLiteral("please run /login"),
        QStringLiteral("not logged in"),
        QStringLiteral("invalid api key"),
        QStringLiteral("authentication_error"),
    };
    const QString text =
        (cloud.diagnostic + QLatin1Char('\n') + cloud.lastError).toLower();
    for (const QString &sign : signs)
        if (text.contains(sign))
            return true;
    return false;
}

QList<CloudAgentAction> cloudAgentActions(const CloudAgentSession &cloud)
{
    QList<CloudAgentAction> actions;
    if (cloud.claimable()) {
        actions << CloudAgentAction::ClaimHere;
        return actions;
    }
    const bool finished = cloudAgentFinished(cloud);
    if (finished && cloudAgentNeedsRunnerSignIn(cloud))
        actions << CloudAgentAction::SignInRunner;
    if (finished)
        actions << CloudAgentAction::ContinueLocally;
    if (cloud.promptable())
        actions << CloudAgentAction::FollowUp;
    if (finished && !cloudAgentOriginalPrompt(cloud).isEmpty())
        actions << CloudAgentAction::RetryInCloud;
    return actions;
}

QString cloudAgentOriginalPrompt(const CloudAgentSession &cloud)
{
    if (!cloud.historyTruncated) {
        for (const CloudAgentTurn &turn : cloud.history)
            if (turn.role == QLatin1String("user") &&
                !turn.text.trimmed().isEmpty())
                return turn.text.trimmed();
    }
    return cloud.title.trimmed();
}

QString cloudAgentContinuationTitle(const CloudAgentSession &cloud)
{
    const QString title = cloud.title.trimmed().isEmpty()
                              ? cloudAgentOriginalPrompt(cloud)
                                    .section(QLatin1Char('\n'), 0, 0)
                                    .left(80)
                              : cloud.title.trimmed();
    return title.isEmpty() ? QStringLiteral("Continued cloud run")
                           : QStringLiteral("Continue: ") + title;
}

QString cloudAgentContinuationPrompt(const CloudAgentSession &cloud)
{
    constexpr int kMaxHistoryChars = 24 * 1024;
    const QString original = cloudAgentOriginalPrompt(cloud);
    QString where = cloud.targetNode.isEmpty()
                        ? QString()
                        : QStringLiteral(" on %1").arg(cloud.targetNode);
    QString prompt =
        QStringLiteral("This continues a ForkMesh cloud agent run (session "
                       "%1) that ended as \"%2\"%3 before it was finished.")
            .arg(cloud.sessionId, cloud.status, where);
    const QString reason =
        cloud.diagnostic.isEmpty() ? cloud.lastError : cloud.diagnostic;
    if (!reason.isEmpty())
        prompt += QStringLiteral(" It stopped with: ") + reason;
    prompt += QStringLiteral("\n\nThe original request:\n\n") +
              (original.isEmpty() ? QStringLiteral("(not recorded)")
                                  : original);
    QStringList turns;
    int used = 0;
    const int firstUser = [&cloud] {
        if (cloud.historyTruncated)
            return -1;
        for (int i = 0; i < cloud.history.size(); ++i)
            if (cloud.history.at(i).role == QLatin1String("user") &&
                !cloud.history.at(i).text.trimmed().isEmpty())
                return i;
        return -1;
    }();
    bool dropped = cloud.historyTruncated;
    for (int i = cloud.history.size() - 1; i >= 0; --i) {
        const CloudAgentTurn &turn = cloud.history.at(i);
        if (i == firstUser || turn.text.trimmed().isEmpty())
            continue;
        const QString who =
            turn.role == QLatin1String("user")
                ? QStringLiteral("User")
                : QStringLiteral("Assistant");
        const QString label = turn.author.trimmed().isEmpty()
                                  ? who
                                  : who + QStringLiteral(" (") +
                                        turn.author.trimmed() +
                                        QLatin1Char(')');
        const QString entry = label + QStringLiteral(":\n") + turn.text.trimmed();
        if (used + entry.size() > kMaxHistoryChars) {
            dropped = true;
            break;
        }
        used += entry.size();
        turns.prepend(entry);
    }
    if (!turns.isEmpty()) {
        prompt += QStringLiteral("\n\nThe conversation after it");
        if (dropped)
            prompt += QStringLiteral(" (older turns left out)");
        prompt += QStringLiteral(":\n\n") + turns.join(QStringLiteral("\n\n"));
    }
    prompt += QStringLiteral(
        "\n\nPick up where that run stopped: look at what is already in the "
        "working copy, then finish the original request.");
    return prompt;
}

QString cloudAgentQueueRuntime(const QString &provider)
{
    const QString clean = provider.trimmed().toLower();
    if (clean == QLatin1String("claude-code"))
        return clean;
    if (clean == QLatin1String("codex") || clean.startsWith(QStringLiteral("codex")))
        return QStringLiteral("codex");
    return {};
}

bool cloudAgentStoppable(const CloudAgentSession &cloud)
{
    if (cloud.localAgentId <= 0 || cloud.claimable())
        return false;
    return cloud.status == QLatin1String("queued") ||
           cloud.status == QLatin1String("running") ||
           cloud.status == QLatin1String("security_pending");
}

bool cloudAgentBranchMerged(const CloudAgentSession &cloud)
{
    return cloud.merged || cloud.mergeState == QLatin1String("merged");
}

QString cloudAgentContinueBranch(const CloudAgentSession &cloud)
{
    const QString branch = cloud.branchName.trimmed();
    if (cloudAgentBranchMerged(cloud) ||
        !forkmesh::localcontrol::handoffBranchValid(branch))
        return {};
    return branch;
}

QString cloudAgentRunnerName(const CloudAgentSession &cloud)
{
    if (!cloud.runner.trimmed().isEmpty())
        return cloud.runner.trimmed();
    if (!cloud.targetNode.trimmed().isEmpty())
        return cloud.targetNode.trimmed();
    return QStringLiteral("the cloud");
}

CloudAgentContinuePlan cloudAgentContinuePlan(const CloudAgentSession &cloud,
                                              bool branchFetched)
{
    CloudAgentContinuePlan plan;
    const QString base = cloud.baseBranch.trimmed();
    plan.landingBase =
        forkmesh::localcontrol::handoffRefNameValid(base) ? base : QString();
    plan.branch = cloud.branchName.trimmed();
    plan.startBranch = plan.landingBase;
    const QString from = plan.landingBase.isEmpty()
                             ? QStringLiteral("the default branch")
                             : plan.landingBase;
    if (cloudAgentBranchMerged(cloud) && !plan.branch.isEmpty()) {
        plan.start = CloudContinueStart::BranchMerged;
        plan.notice = QStringLiteral("Its branch %1 was already merged, so "
                                     "this run starts from %2.")
                          .arg(plan.branch, from);
        return plan;
    }
    const QString branch = cloudAgentContinueBranch(cloud);
    if (branch.isEmpty()) {
        plan.start = CloudContinueStart::NoBranch;
        plan.notice = plan.branch.isEmpty()
                          ? QStringLiteral("The cloud run left no branch, so "
                                           "this run starts from %1.")
                                .arg(from)
                          : QStringLiteral("Its branch %1 is not a cloud agent "
                                           "branch, so this run starts from "
                                           "%2.")
                                .arg(plan.branch, from);
        return plan;
    }
    if (!branchFetched) {
        plan.start = CloudContinueStart::BranchMissing;
        plan.notice = QStringLiteral("Its branch %1 is not on the mirror, so "
                                     "this run starts from %2.")
                          .arg(branch, from);
        return plan;
    }
    plan.start = CloudContinueStart::SessionBranch;
    plan.startBranch = branch;
    plan.notice = QStringLiteral("Continuing on top of %1.").arg(branch);
    return plan;
}

QString cloudAgentBranchContinuationPrompt(const CloudAgentSession &cloud,
                                           const CloudAgentContinuePlan &plan)
{
    QString where;
    if (plan.start == CloudContinueStart::SessionBranch) {
        where = QStringLiteral("Its work so far was pushed to branch %1")
                    .arg(plan.branch);
        if (!plan.landingBase.isEmpty())
            where += QStringLiteral(" (started from %1)").arg(plan.landingBase);
        where += QLatin1Char('.');
        if (cloud.diffKnown && (cloud.diffFiles > 0 || cloud.diffAhead > 0))
            where += QStringLiteral(" It is %1 commit%2 ahead, with %3 file%4 "
                                    "changed (+%5/-%6).")
                         .arg(cloud.diffAhead)
                         .arg(cloud.diffAhead == 1 ? QString()
                                                   : QStringLiteral("s"))
                         .arg(cloud.diffFiles)
                         .arg(cloud.diffFiles == 1 ? QString()
                                                   : QStringLiteral("s"))
                         .arg(cloud.diffAdded)
                         .arg(cloud.diffRemoved);
        where += QStringLiteral(" This working copy is a new branch cut from "
                                "that branch's tip, so those commits are "
                                "already here: check what is done before "
                                "changing anything.");
    } else {
        where = plan.notice +
                QStringLiteral(" None of the cloud run's changes are in this "
                               "working copy.");
    }
    const QString continuation = cloudAgentContinuationPrompt(cloud);
    const QString marker = QStringLiteral("\n\nThe original request:");
    const int split = continuation.indexOf(marker);
    if (split < 0)
        return continuation + QStringLiteral("\n\n") + where;
    return continuation.left(split) + QStringLiteral("\n\n") + where +
           continuation.mid(split);
}

QString cloudAgentQueueText(const QString &provider, const QString &model,
                            bool createPr, const QString &fromDevice,
                            const QString &prompt)
{
    return QStringList{provider, model,
                       createPr ? QStringLiteral("1") : QStringLiteral("0"),
                       fromDevice, prompt}
        .join(QLatin1Char('\n'));
}

QByteArray cloudAgentProofCanonical(const QString &proof, const QString &user,
                                    const QString &path, const QString &prompt,
                                    const QString &timestamp)
{
    QString canonical = proof + QLatin1Char('\n') + user + QLatin1Char('\n') +
                        path + QLatin1Char('\n');
    if (proof == cloudAgentPromptProof() || proof == cloudAgentQueueProof() ||
        proof == cloudAgentBranchProof())
        canonical += QString::fromLatin1(
                         QCryptographicHash::hash(prompt.toUtf8(),
                                                  QCryptographicHash::Sha256)
                             .toHex()) +
                     QLatin1Char('\n');
    canonical += timestamp;
    return canonical.toUtf8();
}

QString cloudAgentBranchActionText(const QJsonObject &body)
{
    static const QStringList kKeys = {
        QStringLiteral("action"), QStringLiteral("cleanup"),
        QStringLiteral("discard"), QStringLiteral("requestId"),
        QStringLiteral("number")};
    for (const QString &key : body.keys()) {
        if (!kKeys.contains(key))
            return QString();
    }
    const QString action = body.value(QStringLiteral("action")).toString();
    if (action != QLatin1String("merge") && action != QLatin1String("cleanup") &&
        action != QLatin1String("link-pull") && action != QLatin1String("update"))
        return QString();
    QStringList parts{action};
    for (const QString &key :
         {QStringLiteral("cleanup"), QStringLiteral("discard")}) {
        if (!body.contains(key)) {
            parts << QString();
            continue;
        }
        if (!body.value(key).isBool())
            return QString();
        parts << (body.value(key).toBool() ? QStringLiteral("1")
                                           : QStringLiteral("0"));
    }
    if (body.contains(QStringLiteral("requestId")) &&
        !body.value(QStringLiteral("requestId")).isString())
        return QString();
    parts << body.value(QStringLiteral("requestId")).toString();
    if (body.contains(QStringLiteral("number"))) {
        const QJsonValue number = body.value(QStringLiteral("number"));
        if (!number.isDouble() ||
            number.toDouble() != double(qint64(number.toDouble())))
            return QString();
        parts << QString::number(qint64(number.toDouble()));
    } else {
        parts << QString();
    }
    return parts.join(QLatin1Char('\n'));
}

QJsonObject cloudAgentMergeBody(const QString &requestId)
{
    return QJsonObject{{QStringLiteral("action"), QStringLiteral("merge")},
                       {QStringLiteral("cleanup"), true},
                       {QStringLiteral("requestId"), requestId}};
}

QJsonObject cloudAgentCleanupBody(const QString &requestId)
{
    return QJsonObject{{QStringLiteral("action"), QStringLiteral("cleanup")},
                       {QStringLiteral("requestId"), requestId}};
}

QJsonObject cloudAgentLinkPullBody(int number)
{
    return QJsonObject{{QStringLiteral("action"), QStringLiteral("link-pull")},
                       {QStringLiteral("number"), number}};
}

QJsonObject cloudAgentUpdateBody(const QString &requestId)
{
    return QJsonObject{{QStringLiteral("action"), QStringLiteral("update")},
                       {QStringLiteral("requestId"), requestId}};
}

QString cloudAgentMergedTaskNote(const CloudAgentSession &cloud,
                                 const QJsonObject &payload)
{
    const QString base = payload.value(QStringLiteral("base")).toString().isEmpty()
                             ? cloud.baseBranch
                             : payload.value(QStringLiteral("base")).toString();
    QString note = QStringLiteral("Merged %1 into %2").arg(
        cloud.branchName, base.isEmpty() ? QStringLiteral("its base") : base);
    const QString oid = payload.value(QStringLiteral("mergeOid")).toString();
    if (!oid.isEmpty())
        note += QStringLiteral(" (%1)").arg(oid.left(7));
    note += QLatin1Char('.');
    const QString message =
        cloudAgentMergeOutcome(payload, cloud.branchName, base).trimmed();
    if (!message.isEmpty())
        note += QLatin1Char(' ') + message;
    return note;
}

QString cloudAgentMergeOutcome(const QJsonObject &payload, const QString &branch,
                               const QString &base)
{
    const QString message =
        payload.value(QStringLiteral("message")).toString().trimmed();
    if (payload.value(QStringLiteral("status")).toString() !=
        QLatin1String("merged")) {
        QString text = message.isEmpty()
                           ? QStringLiteral("%1 was not merged.").arg(branch)
                           : message;
        const QJsonArray conflicts =
            payload.value(QStringLiteral("conflicts")).toArray();
        QStringList files;
        for (const QJsonValue &file : conflicts)
            files << file.toString();
        if (!files.isEmpty())
            text += QStringLiteral(" Conflicts: %1.")
                        .arg(files.join(QStringLiteral(", ")));
        return text;
    }
    QString text = message.isEmpty()
                       ? QStringLiteral("Merged %1 into %2.").arg(branch, base)
                       : message;
    const QJsonObject stats = payload.value(QStringLiteral("stats")).toObject();
    const int commits = stats.value(QStringLiteral("commits")).toInt();
    const int files = stats.value(QStringLiteral("files")).toInt();
    if (commits > 0 || files > 0)
        text += QStringLiteral(" %1 commit%2 · %3 file%4 +%5 \u2212%6.")
                    .arg(commits)
                    .arg(commits == 1 ? QString() : QStringLiteral("s"))
                    .arg(files)
                    .arg(files == 1 ? QString() : QStringLiteral("s"))
                    .arg(stats.value(QStringLiteral("insertions")).toInt())
                    .arg(stats.value(QStringLiteral("deletions")).toInt());
    return text;
}

bool cloudAgentDeleteFoundNothing(int httpStatus, const QJsonObject &payload)
{
    return httpStatus == 404 &&
           payload.value(QStringLiteral("error")).toString() ==
               QLatin1String("not_found");
}

QList<HandoffDesktop> handoffDesktops(const QJsonObject &payload)
{
    QList<HandoffDesktop> desktops;
    QSet<QString> keys;
    for (const QJsonValue &value :
         payload.value(QStringLiteral("desktops")).toArray()) {
        const QJsonObject item = value.toObject();
        HandoffDesktop desktop;
        desktop.key = item.value(QStringLiteral("key")).toString().trimmed();
        if (desktop.key.isEmpty() || keys.contains(desktop.key))
            continue;
        keys.insert(desktop.key);
        desktop.node = item.value(QStringLiteral("node")).toString().trimmed();
        if (desktop.node.isEmpty())
            desktop.node = QStringLiteral("Unnamed desktop");
        desktop.lastSeenMs =
            qint64(item.value(QStringLiteral("lastSeen")).toDouble());
        desktops.append(desktop);
    }
    std::stable_sort(desktops.begin(), desktops.end(),
                     [](const HandoffDesktop &a, const HandoffDesktop &b) {
                         return a.lastSeenMs > b.lastSeenMs;
                     });
    QHash<QString, int> names;
    for (const HandoffDesktop &desktop : std::as_const(desktops))
        ++names[desktop.node.toLower()];
    for (HandoffDesktop &desktop : desktops)
        desktop.label = names.value(desktop.node.toLower()) > 1
                            ? QStringLiteral("%1 (%2)").arg(desktop.node,
                                                            desktop.key.left(6))
                            : desktop.node;
    return desktops;
}

QString handoffDesktopSeenText(qint64 lastSeenMs, qint64 nowMs)
{
    if (lastSeenMs <= 0)
        return QStringLiteral("Not seen online yet");
    const qint64 minutes = qMax<qint64>(0, nowMs - lastSeenMs) / 60000;
    if (minutes < 2)
        return QStringLiteral("Seen just now");
    if (minutes < 120)
        return QStringLiteral("Seen %1 minutes ago").arg(minutes);
    const qint64 hours = minutes / 60;
    if (hours < 48)
        return QStringLiteral("Seen %1 hours ago").arg(hours);
    return QStringLiteral("Seen %1 days ago").arg(hours / 24);
}

} // namespace forkmesh::agents
