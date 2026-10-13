#include "LocalControlAgentOps.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace forkmesh::localcontrol::agentops {

namespace {

QString oneLine(const QString &text, int max)
{
    QString out = text;
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    out = out.trimmed();
    if (out.size() > max)
        out = out.left(max - 1) + QChar(0x2026);
    return out;
}

QString lifetimePhrase(const QString &lifetime)
{
    if (lifetime == QLatin1String("session"))
        return QStringLiteral("for the rest of this session");
    if (lifetime == QLatin1String("worktree"))
        return QStringLiteral("for everything in this worktree");
    if (lifetime == QLatin1String("persistent"))
        return QStringLiteral("from now on, on this desktop (saved until you "
                              "revoke it)");
    return QStringLiteral("this once");
}

QString shortResource(const QString &resource)
{
    static const QRegularExpression windowsPath(
        QStringLiteral("^[A-Za-z]:[\\\\/]"));
    if (resource.startsWith(QLatin1Char('/')) ||
        windowsPath.match(resource).hasMatch()) {
        QString trimmed = resource;
        while (trimmed.size() > 1 && (trimmed.endsWith(QLatin1Char('/')) ||
                                      trimmed.endsWith(QLatin1Char('\\'))))
            trimmed.chop(1);
        const int cut = qMax(trimmed.lastIndexOf(QLatin1Char('/')),
                             trimmed.lastIndexOf(QLatin1Char('\\')));
        const QString name = trimmed.mid(cut + 1);
        return name.isEmpty() ? QStringLiteral("(a folder)") : name.left(200);
    }
    return resource.left(200);
}

}

QString familyPrefix()
{
    return QStringLiteral("agentops.");
}

QStringList actions()
{
    return {QStringLiteral("approve"), QStringLiteral("deny"),
            QStringLiteral("merge"),   QStringLiteral("stop"),
            QStringLiteral("continue"), QStringLiteral("revoke"),
            QStringLiteral("delete"),  QStringLiteral("purge")};
}

bool isKnownAction(const QString &action)
{
    return actions().contains(action);
}

bool isDestructiveAction(const QString &action)
{
    return action == QLatin1String("merge") || action == QLatin1String("delete") ||
           action == QLatin1String("purge");
}

QString confirmationHolder(const QString &deviceKey, int connectionId)
{
    return deviceKey + QLatin1Char('#') + QString::number(connectionId);
}

QStringList lifetimes()
{
    return {QStringLiteral("once"), QStringLiteral("session"),
            QStringLiteral("worktree"), QStringLiteral("persistent")};
}

bool validLifetime(const QString &lifetime)
{
    return lifetimes().contains(lifetime);
}

QString answerText(bool allow, const QString &lifetime)
{
    if (!allow)
        return QStringLiteral("Deny");
    if (lifetime == QLatin1String("session"))
        return QStringLiteral("Allow for session");
    if (lifetime == QLatin1String("worktree"))
        return QStringLiteral("Allow for worktree");
    if (lifetime == QLatin1String("persistent"))
        return QStringLiteral("Always allow");
    return QStringLiteral("Allow once");
}

QString approvalSummary(bool allow, const QString &lifetime,
                        const QString &question, const QString &sessionTitle)
{
    const QString who = sessionTitle.trimmed().isEmpty()
                            ? QStringLiteral("The agent")
                            : QStringLiteral("\"%1\"").arg(oneLine(sessionTitle, 80));
    const QString what = oneLine(question, 300);
    if (!allow)
        return QStringLiteral("Deny %1's request: %2. The agent is told no and "
                              "carries on without it.")
            .arg(who, what);
    return QStringLiteral("Allow %1's request %2: %3")
        .arg(who, lifetimePhrase(lifetime), what);
}

QString mergeSummary(const QString &branch, const QString &base,
                     const QString &repo)
{
    return QStringLiteral("Merge %1 into %2 in %3, then delete its worktree, the "
                          "branch and the agent record. Uncommitted work in the "
                          "worktree stops the merge. This cannot be undone.")
        .arg(branch, base.isEmpty() ? QStringLiteral("the default branch") : base,
             repo);
}

QList<MergeShortstat> parseMergeShortstatLog(const QString &log)
{
    static const QRegularExpression shortstat(
        QStringLiteral("(\\d+) files? changed"
                       "(?:, (\\d+) insertions?\\(\\+\\))?"
                       "(?:, (\\d+) deletions?\\(-\\))?"));
    QList<MergeShortstat> merges;
    for (const QString &record : log.split(QChar(0x1e), Qt::SkipEmptyParts)) {
        const QStringList lines = record.split(QLatin1Char('\n'));
        const QStringList head = lines.value(0).split(QChar(0x1f));
        MergeShortstat merge;
        merge.sha = head.value(0).trimmed();
        if (merge.sha.isEmpty())
            continue;
        merge.subject = head.mid(1).join(QChar(0x1f)).trimmed();
        for (int i = 1; i < lines.size(); ++i) {
            const QRegularExpressionMatch match = shortstat.match(lines.at(i));
            if (!match.hasMatch())
                continue;
            merge.files = match.captured(1).toInt();
            merge.insertions = match.captured(2).toInt();
            merge.deletions = match.captured(3).toInt();
            break;
        }
        merges.append(merge);
    }
    return merges;
}

QJsonObject mergeStatsJson(const MergeStats &stats)
{
    QJsonArray velocity;
    for (const MergeVelocityWindow &window : stats.velocity) {
        QJsonObject row{
            {QStringLiteral("id"), window.id},
            {QStringLiteral("label"), window.label},
            {QStringLiteral("count"), window.count},
            {QStringLiteral("top"), qMax(window.count, window.top)},
            {QStringLiteral("avg"), std::round(window.avg * 10.0) / 10.0},
        };
        if (!window.series.isEmpty()) {
            QJsonArray series;
            for (int bucket : window.series)
                series.append(bucket);
            row.insert(QStringLiteral("series"), series);
        }
        velocity.append(row);
    }
    QJsonObject out{
        {QStringLiteral("base"), stats.base},
        {QStringLiteral("files"), stats.files},
        {QStringLiteral("insertions"), stats.insertions},
        {QStringLiteral("deletions"), stats.deletions},
        {QStringLiteral("velocity"), velocity},
    };
    if (!stats.commit.isEmpty())
        out.insert(QStringLiteral("commit"), stats.commit);
    if (stats.commits >= 0)
        out.insert(QStringLiteral("commits"), stats.commits);
    return out;
}

QString deleteSummary(const QString &branch, const QString &repo, bool running)
{
    const QString stop = running ? QStringLiteral("Stop this agent and delete it")
                                 : QStringLiteral("Delete this agent");
    if (branch.isEmpty())
        return QStringLiteral("%1 from the desktop in %2. Its record moves "
                              "to the recycle bin.")
            .arg(stop, repo);
    return QStringLiteral("%1 in %2 together with its worktree and the "
                          "unmerged branch %3. Work that was not merged is lost.")
        .arg(stop, repo, branch);
}

QString outsideDeleteSummary(const QString &label, const QString &cwd)
{
    return QStringLiteral("End %1, which was started outside ForkMesh in %2, and remove "
                          "it from the list. Anything it has not saved is lost.")
        .arg(label.isEmpty() ? QStringLiteral("the agent") : label,
             cwd.isEmpty() ? QStringLiteral("a terminal") : cwd);
}

QList<DeletePart> deleteParts(const DeleteFacts &facts)
{
    const QString unsettled = facts.settled ? QStringLiteral("kept") : QStringLiteral("pending");
    QList<DeletePart> parts;
    parts.append({QStringLiteral("agent"), QString(),
                  facts.agentGone ? QStringLiteral("deleted") : unsettled, QString()});
    const auto onDisk = [&](const QString &kind, const QString &name, bool existed, bool left) {
        if (name.isEmpty())
            return;
        parts.append({kind, name,
                      !existed ? QStringLiteral("absent")
                               : (left ? unsettled : QStringLiteral("deleted")),
                      QString()});
    };
    onDisk(QStringLiteral("branch"), facts.branch, facts.branchExisted, facts.branchLeft);
    onDisk(QStringLiteral("work"), facts.worktree, facts.worktreeExisted, facts.worktreeLeft);
    if (facts.issue > 0) {
        const QString status =
            !facts.issueFound ? QStringLiteral("absent")
            : facts.issueStatus == QLatin1String("closed") ? QStringLiteral("closed")
                                                           : unsettled;
        parts.append({QStringLiteral("issue"), QString::number(facts.issue), status, QString()});
    }
    if (facts.pull > 0) {
        DeletePart pull{QStringLiteral("pull"), QString::number(facts.pull), unsettled, QString()};
        if (facts.pullMerged) {
            pull.status = QStringLiteral("kept");
            pull.reason = QStringLiteral("merged");
        } else if (!facts.pullReachable) {
            pull.status = QStringLiteral("kept");
            pull.reason = QStringLiteral("no_checkout");
        } else if (facts.agentGone) {
            pull.status = QStringLiteral("deleted");
        }
        parts.append(pull);
    }
    if (!facts.task.isEmpty()) {
        parts.append({QStringLiteral("task"), facts.task,
                      facts.taskStatus.isEmpty() ? unsettled : facts.taskStatus,
                      facts.taskReason});
    }
    return parts;
}

QJsonArray deletePartsJson(const QList<DeletePart> &parts)
{
    QJsonArray out;
    for (const DeletePart &part : parts) {
        QJsonObject row{{QStringLiteral("kind"), part.kind},
                        {QStringLiteral("status"), part.status}};
        if (!part.name.isEmpty())
            row.insert(QStringLiteral("name"), part.name);
        if (!part.reason.isEmpty())
            row.insert(QStringLiteral("reason"), part.reason);
        out.append(row);
    }
    return out;
}

void DeleteTrace::finish(bool ok)
{
    if (!done)
        return;
    const auto callback = std::move(done);
    done = nullptr;
    callback(ok);
}

QJsonArray numstatRows(const QString &numstat, int max)
{
    QJsonArray rows;
    const QStringList lines = numstat.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (rows.size() >= max)
            break;
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 3)
            continue;
        const bool binary = parts.at(0) == QLatin1String("-");
        rows.append(QJsonObject{
            {QStringLiteral("path"), parts.mid(2).join(QLatin1Char('\t')).left(500)},
            {QStringLiteral("added"), binary ? 0 : parts.at(0).toInt()},
            {QStringLiteral("removed"), binary ? 0 : parts.at(1).toInt()},
            {QStringLiteral("binary"), binary},
        });
    }
    return rows;
}

QString capDiff(const QString &text, int budget, bool *truncated)
{
    if (budget <= 0) {
        if (truncated && !text.isEmpty())
            *truncated = true;
        return {};
    }
    if (text.size() <= budget)
        return text;
    if (truncated)
        *truncated = true;
    const int cut = text.lastIndexOf(QLatin1Char('\n'), budget - 1);
    return text.left(cut > 0 ? cut + 1 : budget);
}

QStringList untrackedPaths(const QString &porcelain, int max)
{
    QStringList out;
    const QStringList lines = porcelain.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (out.size() >= max)
            break;
        if (line.startsWith(QLatin1String("?? ")))
            out.append(line.mid(3).left(500));
    }
    return out;
}

QString diffImageMime(const QString &path)
{
    static const QList<QPair<QString, QString>> kinds{
        {QStringLiteral("png"), QStringLiteral("image/png")},
        {QStringLiteral("jpg"), QStringLiteral("image/jpeg")},
        {QStringLiteral("jpeg"), QStringLiteral("image/jpeg")},
        {QStringLiteral("gif"), QStringLiteral("image/gif")},
        {QStringLiteral("webp"), QStringLiteral("image/webp")},
        {QStringLiteral("bmp"), QStringLiteral("image/bmp")},
        {QStringLiteral("ico"), QStringLiteral("image/x-icon")},
        {QStringLiteral("heic"), QStringLiteral("image/heic")},
        {QStringLiteral("tif"), QStringLiteral("image/tiff")},
        {QStringLiteral("tiff"), QStringLiteral("image/tiff")},
        {QStringLiteral("svg"), QStringLiteral("image/svg+xml")},
    };
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot < 0 || dot < path.lastIndexOf(QLatin1Char('/')))
        return {};
    const QString ext = path.mid(dot + 1).toLower();
    for (const auto &kind : kinds) {
        if (kind.first == ext)
            return kind.second;
    }
    return {};
}

bool validDiffImagePath(const QString &path)
{
    if (path.isEmpty() || path.size() > 1000 || path.startsWith(QLatin1Char('/')) ||
        path.contains(QLatin1Char('\\')) || path.contains(QChar(0)))
        return false;
    const QStringList parts = path.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
    }
    return !diffImageMime(path).isEmpty();
}

QString bulkSummary(const QString &action, int count)
{
    const QString n = count == 1 ? QStringLiteral("1 agent")
                                 : QStringLiteral("%1 agents").arg(count);
    if (action == QLatin1String("stop"))
        return QStringLiteral("Stop %1 on the desktop. Their work so far stays in "
                              "their worktrees.")
            .arg(n);
    if (action == QLatin1String("continue"))
        return QStringLiteral("Continue %1 on the desktop where they left off.")
            .arg(n);
    return {};
}

QString revokeSummary(const QString &capability, const QString &resource)
{
    return QStringLiteral("Revoke the saved permission for %1 on %2. Agents are "
                          "asked again next time.")
        .arg(capability, shortResource(resource));
}

QString answerTarget(const QString &approvalId, bool allow,
                     const QString &lifetime)
{
    return approvalId + QLatin1Char(':') +
           (allow ? lifetime : QStringLiteral("deny"));
}

QString approvalId(int sessionId, const QString &requestId,
                   const QByteArray &salt)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    hash.addData(QByteArray(1, '\0'));
    hash.addData(QByteArray::number(sessionId));
    hash.addData(QByteArray(1, '\0'));
    hash.addData(requestId.toUtf8());
    return QString::fromLatin1(hash.result().toHex().left(32));
}

bool validApprovalId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(id).hasMatch();
}

QList<PendingQuestion> questionsInEvents(const QList<QJsonObject> &events,
                                         int maxScanned)
{
    QList<PendingQuestion> out;
    QSet<QString> seen;
    QSet<QString> answered;
    const int stop = qMax(0, events.size() - qMax(0, maxScanned));
    for (int i = events.size() - 1; i >= stop; --i) {
        const QJsonObject &ev = events.at(i);
        const QString type = ev.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("_local_ask_answer")) {
            answered.insert(ev.value(QStringLiteral("tool_use_id")).toString());
            continue;
        }
        if (type != QLatin1String("assistant"))
            continue;
        const QJsonArray content = ev.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        for (const QJsonValue &value : content) {
            const QJsonObject block = value.toObject();
            if (block.value(QStringLiteral("type")).toString() !=
                    QLatin1String("tool_use") ||
                block.value(QStringLiteral("name")).toString() !=
                    QLatin1String("AskUserQuestion"))
                continue;
            const QString id = block.value(QStringLiteral("id")).toString();
            if (id.isEmpty() || id.size() > 256 || seen.contains(id) ||
                answered.contains(id))
                continue;
            seen.insert(id);
            const QJsonObject input = block.value(QStringLiteral("input")).toObject();
            const QJsonArray questions = input.value(QStringLiteral("questions")).toArray();
            QString question = questions.isEmpty()
                                   ? QString()
                                   : questions.first()
                                         .toObject()
                                         .value(QStringLiteral("question"))
                                         .toString();
            out.append(PendingQuestion{
                id, question.left(kMaxQuestionChars),
                input,
                qint64(ev.value(QStringLiteral("_ts")).toDouble())});
        }
    }
    return out;
}

QString approvalKind(const QString &capability)
{
    if (capability == QLatin1String("process.execute"))
        return QStringLiteral("command");
    if (capability == QLatin1String("network.connect"))
        return QStringLiteral("network");
    if (capability == QLatin1String("file.read") ||
        capability == QLatin1String("file.write"))
        return QStringLiteral("file");
    if (capability == QLatin1String("secret.read"))
        return QStringLiteral("secret");
    if (capability == QLatin1String("tool.invoke") ||
        capability == QLatin1String("resource.read") ||
        capability == QLatin1String("device.control"))
        return QStringLiteral("tool");
    return QStringLiteral("other");
}

QString commandFromQuestion(const QString &question)
{
    static const QString prefix = QStringLiteral("Run command:\n");
    if (!question.startsWith(prefix))
        return {};
    QString command = question.mid(prefix.size());
    const int reason = command.indexOf(QStringLiteral("\n\nReason: "));
    if (reason >= 0)
        command = command.left(reason);
    return command.left(kMaxCommandChars);
}

QString capabilityFromQuestion(const QString &question)
{
    if (!commandFromQuestion(question).isEmpty())
        return QStringLiteral("process.execute");
    static const QRegularExpression generic(
        QStringLiteral("^Allow ([a-z]+\\.[a-z]+) for "));
    const QRegularExpressionMatch match = generic.match(question);
    return match.hasMatch() ? match.captured(1) : QString();
}

QJsonObject approvalRow(const ApprovalFacts &facts)
{
    const QString question = facts.question.left(kMaxQuestionChars);
    return QJsonObject{
        {QStringLiteral("id"), facts.id},
        {QStringLiteral("sessionId"), facts.sessionId},
        {QStringLiteral("title"), oneLine(facts.title, 200)},
        {QStringLiteral("repo"), facts.repo},
        {QStringLiteral("provider"), facts.provider},
        {QStringLiteral("capability"), facts.capability},
        {QStringLiteral("kind"), approvalKind(facts.capability)},
        {QStringLiteral("question"), question},
        {QStringLiteral("command"), commandFromQuestion(question)},
        {QStringLiteral("askedAtMs"), double(facts.askedAtMs)},
        {QStringLiteral("lifetimes"), QJsonArray::fromStringList(lifetimes())},
    };
}

QString approvalsSignature(QStringList ids)
{
    std::sort(ids.begin(), ids.end());
    return QString::fromLatin1(
        QCryptographicHash::hash(ids.join(QLatin1Char(',')).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex()
            .left(16));
}

bool canonicalIds(const QJsonArray &ids, QList<int> *out, int max)
{
    if (!out)
        return false;
    out->clear();
    if (ids.isEmpty() || ids.size() > max)
        return false;
    QSet<int> seen;
    for (const QJsonValue &value : ids) {
        if (!value.isDouble())
            return false;
        const double raw = value.toDouble();
        if (raw != std::floor(raw) || raw <= 0 || raw > 2147483647.0)
            return false;
        const int id = int(raw);
        if (!seen.contains(id)) {
            seen.insert(id);
            out->append(id);
        }
    }
    std::sort(out->begin(), out->end());
    return true;
}

QString bulkTarget(const QString &action, const QList<int> &ids)
{
    QStringList parts;
    for (const int id : ids)
        parts << QString::number(id);
    return action + QLatin1Char(':') + parts.join(QLatin1Char(','));
}

QString binTitle(int issueNumber, const QString &issueTitle, const QString &prompt)
{
    QString title = issueNumber > 0 && !issueTitle.isEmpty() ? issueTitle : prompt;
    title = title.section(QLatin1Char('\n'), 0, 0).simplified();
    if (title.size() > 90)
        title = title.left(89) + QChar(0x2026);
    return title;
}

QJsonObject binRow(const BinEntry &entry, qint64 retentionMs, qint64 nowMs)
{
    qint64 removeInMs = -1;
    if (entry.deletedAtMs > 0)
        removeInMs = std::max<qint64>(0, retentionMs - (nowMs - entry.deletedAtMs));
    return {{QStringLiteral("id"), entry.id},
            {QStringLiteral("title"), entry.title},
            {QStringLiteral("owner"), entry.owner},
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("branch"), entry.branch},
            {QStringLiteral("provider"), entry.provider},
            {QStringLiteral("deletedAtMs"), double(entry.deletedAtMs)},
            {QStringLiteral("removeInMs"), double(removeInMs)}};
}

QJsonObject binPayload(const QList<BinEntry> &entries, int retentionHours,
                       int minHours, int maxHours, qint64 nowMs)
{
    const qint64 retentionMs = qint64(retentionHours) * 60 * 60 * 1000;
    QJsonArray rows;
    for (const BinEntry &entry : entries) {
        if (rows.size() >= kMaxBinAgents)
            break;
        rows.append(binRow(entry, retentionMs, nowMs));
    }
    return {{QStringLiteral("agents"), rows},
            {QStringLiteral("count"), int(entries.size())},
            {QStringLiteral("truncated"), entries.size() > rows.size()},
            {QStringLiteral("retentionHours"), retentionHours},
            {QStringLiteral("minHours"), minHours},
            {QStringLiteral("maxHours"), maxHours}};
}

QString purgeSummary(int count)
{
    const QString n = count == 1 ? QStringLiteral("1 deleted agent")
                                 : QStringLiteral("%1 deleted agents").arg(count);
    return QStringLiteral("Permanently delete %1 from the desktop's recycle bin, with "
                          "their transcripts. This cannot be undone.")
        .arg(n);
}

QList<WorktreeEntry> parseWorktreePorcelain(const QString &text, int max)
{
    QList<WorktreeEntry> out;
    WorktreeEntry current;
    bool open = false;
    const auto flush = [&] {
        if (open && !current.path.isEmpty() && out.size() < max)
            out.append(current);
        current = WorktreeEntry();
        open = false;
    };
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            flush();
            continue;
        }
        if (line.startsWith(QLatin1String("worktree "))) {
            flush();
            current.path = line.mid(9);
            open = true;
        } else if (line.startsWith(QLatin1String("HEAD "))) {
            current.head = line.mid(5);
        } else if (line.startsWith(QLatin1String("branch "))) {
            QString branch = line.mid(7);
            if (branch.startsWith(QLatin1String("refs/heads/")))
                branch = branch.mid(11);
            current.branch = branch;
        } else if (line == QLatin1String("bare")) {
            current.bare = true;
        } else if (line == QLatin1String("detached")) {
            current.detached = true;
        } else if (line == QLatin1String("locked") ||
                   line.startsWith(QLatin1String("locked "))) {
            current.locked = true;
        } else if (line == QLatin1String("prunable") ||
                   line.startsWith(QLatin1String("prunable "))) {
            current.prunable = true;
        }
    }
    flush();
    return out;
}

QJsonObject worktreeRow(const WorktreeEntry &entry, bool isMain, int ahead,
                        int behind, int sessionId)
{
    QString name = QFileInfo(entry.path).fileName();
    if (name.isEmpty())
        name = entry.branch;
    QJsonObject row{
        {QStringLiteral("name"), name},
        {QStringLiteral("branch"), entry.branch},
        {QStringLiteral("head"), entry.head.left(12)},
        {QStringLiteral("isMain"), isMain},
        {QStringLiteral("detached"), entry.detached},
        {QStringLiteral("locked"), entry.locked},
        {QStringLiteral("prunable"), entry.prunable},
        {QStringLiteral("ahead"), ahead},
        {QStringLiteral("behind"), behind},
    };
    if (sessionId > 0)
        row.insert(QStringLiteral("sessionId"), sessionId);
    return row;
}

bool parseLeftRight(const QString &text, int *behind, int *ahead)
{
    const QStringList parts =
        text.trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                             Qt::SkipEmptyParts);
    if (parts.size() != 2)
        return false;
    bool ok1 = false;
    bool ok2 = false;
    const int left = parts.at(0).toInt(&ok1);
    const int right = parts.at(1).toInt(&ok2);
    if (!ok1 || !ok2 || left < 0 || right < 0)
        return false;
    if (behind)
        *behind = left;
    if (ahead)
        *ahead = right;
    return true;
}

QList<UsageWindow> codexWindows(const QJsonArray &rows)
{
    QList<UsageWindow> out;
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        const QString label = row.value(QStringLiteral("label")).toString().left(40);
        if (label.isEmpty())
            continue;
        UsageWindow window;
        window.id = label.toLower();
        window.label = label;
        window.usedPercent =
            row.contains(QStringLiteral("usedPercent"))
                ? qBound(0, row.value(QStringLiteral("usedPercent")).toInt(), 100)
                : -1;
        window.resetAtMs = qint64(row.value(QStringLiteral("resetAt")).toDouble());
        window.exhausted = window.usedPercent >= 99;
        out.append(window);
        if (out.size() >= 8)
            break;
    }
    return out;
}

QJsonObject usageSummary(const QList<UsageProvider> &providers,
                         double claudeSpendUsd, double openAiSpendUsd,
                         qint64 nowMs)
{
    const auto windowRows = [nowMs](const QList<UsageWindow> &source) {
        QJsonArray windows;
        for (const UsageWindow &window : source) {
            const bool elapsed = window.resetAtMs > 0 && window.resetAtMs <= nowMs;
            const int used = elapsed ? 0 : window.usedPercent;
            QString state = QStringLiteral("unknown");
            if (used >= 99 || (!elapsed && window.exhausted))
                state = QStringLiteral("exhausted");
            else if (used >= 80)
                state = QStringLiteral("high");
            else if (used >= 0)
                state = QStringLiteral("ok");
            QJsonObject row{{QStringLiteral("id"), window.id},
                            {QStringLiteral("label"), window.label},
                            {QStringLiteral("usedPercent"), used},
                            {QStringLiteral("state"), state}};
            if (!elapsed && window.resetAtMs > 0)
                row.insert(QStringLiteral("resetAtMs"), double(window.resetAtMs));
            windows.append(row);
        }
        return windows;
    };
    QJsonArray list;
    for (const UsageProvider &provider : providers) {
        const QJsonArray windows = windowRows(provider.windows);
        QJsonArray accounts;
        for (const UsageAccount &account : provider.accounts)
            accounts.append(QJsonObject{{QStringLiteral("id"), account.id.left(80)},
                                        {QStringLiteral("label"),
                                         account.label.left(80)},
                                        {QStringLiteral("active"), account.active},
                                        {QStringLiteral("signedIn"), account.signedIn},
                                        {QStringLiteral("identity"),
                                         account.identity.left(120)},
                                        {QStringLiteral("windows"),
                                         windowRows(account.windows)}});
        QJsonObject row{{QStringLiteral("id"), provider.id},
                        {QStringLiteral("label"), provider.label},
                        {QStringLiteral("windows"), windows},
                        {QStringLiteral("accounts"), accounts}};
        if (provider.creditsEnabled) {
            QJsonObject credits{{QStringLiteral("currency"),
                                 provider.creditsCurrency.left(8)}};
            if (provider.creditsUsed >= 0)
                credits.insert(QStringLiteral("used"), provider.creditsUsed);
            if (provider.creditsLimit >= 0)
                credits.insert(QStringLiteral("limit"), provider.creditsLimit);
            row.insert(QStringLiteral("credits"), credits);
        }
        list.append(row);
    }
    QJsonObject spend;
    if (!std::isnan(claudeSpendUsd) && claudeSpendUsd >= 0)
        spend.insert(QStringLiteral("claudeUsd"), claudeSpendUsd);
    if (!std::isnan(openAiSpendUsd) && openAiSpendUsd >= 0)
        spend.insert(QStringLiteral("openAiUsd"), openAiSpendUsd);
    return QJsonObject{{QStringLiteral("providers"), list},
                       {QStringLiteral("spend"), spend},
                       {QStringLiteral("atMs"), double(nowMs)}};
}

QJsonArray grantRows(const QJsonArray &persistentGrants, int max)
{
    QJsonArray out;
    for (const QJsonValue &value : persistentGrants) {
        if (out.size() >= max)
            break;
        const QJsonObject grant = value.toObject();
        const QString id = grant.value(QStringLiteral("id")).toString();
        if (!validGrantId(id))
            continue;
        const QString capability =
            grant.value(QStringLiteral("capability")).toString();
        QJsonObject row{
            {QStringLiteral("id"), id},
            {QStringLiteral("capability"), capability},
            {QStringLiteral("kind"), approvalKind(capability)},
            {QStringLiteral("resource"),
             shortResource(grant.value(QStringLiteral("resource")).toString())},
            {QStringLiteral("decision"),
             grant.value(QStringLiteral("decision")).toString()},
            {QStringLiteral("lifetime"),
             grant.value(QStringLiteral("lifetime")).toString()},
        };
        const QString worktree =
            grant.value(QStringLiteral("worktreePath")).toString();
        if (!worktree.isEmpty())
            row.insert(QStringLiteral("worktree"), QFileInfo(worktree).fileName());
        out.append(row);
    }
    return out;
}

bool validGrantId(const QString &id)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                       "[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return pattern.match(id).hasMatch();
}

QByteArray approvalSaltFromStored(const QString &hex)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (!pattern.match(hex).hasMatch())
        return {};
    return QByteArray::fromHex(hex.toLatin1());
}

QByteArray newApprovalSalt()
{
    QByteArray salt(kApprovalSaltBytes, '\0');
    for (char &byte : salt)
        byte = char(QRandomGenerator::system()->bounded(256));
    return salt;
}

namespace {

QString flatText(const QString &text, int max)
{
    QString out;
    out.reserve(qMin(text.size(), max * 2));
    bool space = false;
    for (const QChar c : text) {
        if (c.isSpace() || c.category() == QChar::Other_Control ||
            c.category() == QChar::Other_Format) {
            space = !out.isEmpty();
            continue;
        }
        if (space)
            out.append(QLatin1Char(' '));
        space = false;
        out.append(c);
        if (out.size() >= max)
            break;
    }
    if (out.size() > max)
        out.truncate(max);
    if (!out.isEmpty() && out.back().isHighSurrogate())
        out.chop(1);
    return out.trimmed();
}

}

QString approvalPingProof()
{
    return QStringLiteral("forkmesh-account-agent-approval-ping-v1");
}

QString approvalPingPath()
{
    return QStringLiteral("/api/notifications/agent-approval");
}

bool validPingField(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.:-]{1,64}$"));
    return pattern.match(value).hasMatch();
}

QString approvalPingAgent(const QString &provider)
{
    const QString id = provider.trimmed().toLower();
    if (id.isEmpty() || id == QLatin1String("claude") || id == QLatin1String("claude-code"))
        return QStringLiteral("Claude Code");
    if (id == QLatin1String("codex") || id == QLatin1String("openai-codex"))
        return QStringLiteral("Codex");
    return flatText(provider, kMaxPingAgentChars);
}

QString approvalPingSummary(const QString &capability, const QString &repo)
{
    const QString kind = approvalKind(capability);
    QString action;
    if (kind == QLatin1String("command"))
        action = QStringLiteral("Wants to run a command");
    else if (kind == QLatin1String("network"))
        action = QStringLiteral("Wants network access");
    else if (capability == QLatin1String("file.write"))
        action = QStringLiteral("Wants to write a file");
    else if (kind == QLatin1String("file"))
        action = QStringLiteral("Wants to read a file");
    else if (kind == QLatin1String("secret"))
        action = QStringLiteral("Wants to read a secret");
    else if (kind == QLatin1String("tool"))
        action = QStringLiteral("Wants to use a tool");
    else
        action = QStringLiteral("Wants your approval");
    static const QRegularExpression slug(
        QStringLiteral("^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$"));
    const QString where = repo.trimmed();
    if (slug.match(where).hasMatch())
        action += QStringLiteral(" in ") + where;
    return flatText(action, kMaxPingSummaryChars);
}

ApprovalPing approvalPingFor(const QJsonObject &approvalRow)
{
    ApprovalPing ping;
    ping.approvalId = approvalRow.value(QStringLiteral("id")).toString();
    const int session = approvalRow.value(QStringLiteral("sessionId")).toInt();
    if (session > 0)
        ping.sessionId = QString::number(session);
    ping.agent = approvalPingAgent(approvalRow.value(QStringLiteral("provider")).toString());
    ping.summary = approvalPingSummary(approvalRow.value(QStringLiteral("capability")).toString(),
                                       approvalRow.value(QStringLiteral("repo")).toString());
    return ping;
}

QString approvalPingResource(const ApprovalPing &ping)
{
    return ping.sessionId + QLatin1Char('\n') + ping.approvalId + QLatin1Char('\n') +
           ping.agent + QLatin1Char('\n') + ping.summary;
}

QString approvalPingCanonical(const QString &user, const ApprovalPing &ping,
                              const QString &ts)
{
    return approvalPingProof() + QLatin1Char('\n') + user + QLatin1Char('\n') +
           approvalPingResource(ping) + QLatin1Char('\n') + ts;
}

QJsonObject approvalPingBody(const ApprovalPing &ping)
{
    return QJsonObject{
        {QStringLiteral("sessionId"), ping.sessionId},
        {QStringLiteral("approvalId"), ping.approvalId},
        {QStringLiteral("agent"), ping.agent},
        {QStringLiteral("summary"), ping.summary},
    };
}

bool approvalPingRetryable(int httpStatus, bool networkError)
{
    if (httpStatus >= 200 && httpStatus < 300)
        return false;
    if (httpStatus == 408 || httpStatus == 429 || httpStatus >= 500)
        return true;
    return httpStatus <= 0 && networkError;
}

ApprovalPingLedger::ApprovalPingLedger(const QStringList &stored, int max)
    : m_max(qMax(1, max))
{
    for (const QString &id : stored) {
        if (validApprovalId(id) && !m_ids.contains(id))
            m_ids.append(id);
    }
    while (m_ids.size() > m_max)
        m_ids.removeFirst();
}

bool ApprovalPingLedger::contains(const QString &approvalId) const
{
    return m_ids.contains(approvalId);
}

bool ApprovalPingLedger::add(const QString &approvalId)
{
    if (!validApprovalId(approvalId) || m_ids.contains(approvalId))
        return false;
    m_ids.append(approvalId);
    while (m_ids.size() > m_max)
        m_ids.removeFirst();
    return true;
}

void ApprovalPingLedger::remove(const QString &approvalId)
{
    m_ids.removeAll(approvalId);
}

QList<ApprovalPing> ApprovalPingLedger::unsent(const QJsonArray &approvals) const
{
    QList<ApprovalPing> out;
    QSet<QString> seen;
    for (const QJsonValue &value : approvals) {
        const ApprovalPing ping = approvalPingFor(value.toObject());
        if (!validApprovalId(ping.approvalId) || !validPingField(ping.sessionId) ||
            contains(ping.approvalId) || seen.contains(ping.approvalId))
            continue;
        seen.insert(ping.approvalId);
        out.append(ping);
    }
    return out;
}

} // namespace forkmesh::localcontrol::agentops
