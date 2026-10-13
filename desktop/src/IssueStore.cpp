#include "IssueStore.h"
#include "SafeDelete.h"

#include "EventSigning.h"
#include "ForkMeshIdentity.h"
#include "StrictGitReader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QPair>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kGitTimeoutMs = 10000;
constexpr int kGitRewriteTimeoutMs = 120000;

QString issuesRootRel() { return QStringLiteral(".forkmesh/issues"); }

QString openDirName() { return QStringLiteral("open"); }
QString closedDirName() { return QStringLiteral("closed"); }

QString statusDirNameFor(const QString &status)
{
    return status == QLatin1String("closed") ? closedDirName() : openDirName();
}

QString issueJsonFileName(int number)
{
    return QStringLiteral("issue-%1.json").arg(number);
}

QString issueMediaDirName(int number)
{
    return QString::number(number);
}

QStringList issueDirRelCandidates(int number)
{
    const QString name = issueMediaDirName(number);
    return {issuesRootRel() + QLatin1Char('/') + openDirName() +
                QLatin1Char('/') + name,
            issuesRootRel() + QLatin1Char('/') + closedDirName() +
                QLatin1Char('/') + name,
            issuesRootRel() + QLatin1Char('/') + name};
}

QList<int> numericDirEntries(const QString &dir)
{
    QList<int> numbers;
    const QStringList entries =
        QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool numeric = false;
        const int n = entry.toInt(&numeric);
        if (numeric)
            numbers.append(n);
    }
    return numbers;
}

bool runGit(const QString &dir, const QStringList &args, QByteArray *output = nullptr,
            QString *errText = nullptr, int timeoutMs = kGitTimeoutMs)
{
    QProcess process;
    if (args.contains(QStringLiteral("filter-branch"))) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("FILTER_BRANCH_SQUELCH_WARNING"), QStringLiteral("1"));
        process.setProcessEnvironment(env);
    }
    process.start("git", QStringList{"-C", dir} + args);
    if (!process.waitForFinished(timeoutMs)) {
        if (errText)
            *errText = QStringLiteral("git timed out");
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errText)
            *errText =
                QString::fromUtf8(process.readAllStandardError()).trimmed().left(200);
        return false;
    }
    return true;
}

// Like runGit, but feeds `input` to the process's stdin. Used for batched reads
// (`git cat-file --batch`) so a whole tree of blobs is fetched in one process
// instead of one `git show` per file. waitForFinished services both channels, so
// large input/output won't deadlock the pipes.
bool runGitInput(const QString &dir, const QStringList &args, const QByteArray &input,
                 QByteArray *output, QString *errText = nullptr,
                 int timeoutMs = kGitTimeoutMs)
{
    QProcess process;
    process.start("git", QStringList{"-C", dir} + args);
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        if (errText)
            *errText = QStringLiteral("git timed out");
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errText)
            *errText =
                QString::fromUtf8(process.readAllStandardError()).trimmed().left(200);
        return false;
    }
    return true;
}

QString stripEdgeNewlines(QString text)
{
    while (text.startsWith('\n') || text.startsWith('\r'))
        text.remove(0, 1);
    while (text.endsWith('\n') || text.endsWith('\r'))
        text.chop(1);
    return text;
}

struct IssueSubtaskLine {
    int line = -1;
    int indent = 0;
    QString beforeState;
    QString afterState;
    QString label;
    QString title;
    QString carriageReturn;
    bool checked = false;
    int subIssueNumber = 0;
};

int issueTaskIndent(const QString &whitespace)
{
    int indent = 0;
    for (const QChar ch : whitespace)
        indent += ch == QLatin1Char('\t') ? 2 : 1;
    return indent;
}

bool parseIssueChecklistLine(const QString &source, int line,
                             IssueSubtaskLine *task)
{
    static const QRegularExpression taskPattern(
        QStringLiteral("^(\\s*)((?:[-*+]\\s+)?\\[)([ xX])(\\]\\s+)(.*?)(\\r?)$"));
    static const QRegularExpression linkedPattern(
        QStringLiteral("\\s+\\(sub-issue\\s+#([1-9]\\d*)\\)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch match = taskPattern.match(source);
    if (!match.hasMatch())
        return false;
    QString label = match.captured(5).trimmed();
    if (label.isEmpty())
        return false;
    const QRegularExpressionMatch linked = linkedPattern.match(label);
    const int subIssueNumber = linked.hasMatch() ? linked.captured(1).toInt() : 0;
    if (linked.hasMatch())
        label = label.left(linked.capturedStart()).trimmed();
    if (label.isEmpty())
        return false;

    if (task) {
        task->line = line;
        task->indent = issueTaskIndent(match.captured(1));
        task->beforeState = match.captured(1) + match.captured(2);
        task->afterState = match.captured(4);
        task->label = label;
        task->title = label.left(240).trimmed();
        task->carriageReturn = match.captured(6);
        task->checked = match.captured(3).compare(
                            QStringLiteral("x"), Qt::CaseInsensitive) == 0;
        task->subIssueNumber = subIssueNumber;
    }
    return true;
}

QList<IssueSubtaskLine> issueChecklistLines(const QString &body,
                                            bool candidatesOnly = false,
                                            int maximum = 0)
{
    QList<IssueSubtaskLine> tasks;
    const QStringList lines = body.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QString fenceMarker;
    for (int line = 0; line < lines.size(); ++line) {
        const QString trimmed = lines.at(line).trimmed();
        const bool fenceLine = trimmed.startsWith(QStringLiteral("```")) ||
                               trimmed.startsWith(QStringLiteral("~~~"));
        if (fenceLine) {
            const QString marker = trimmed.left(3);
            if (fenceMarker.isEmpty())
                fenceMarker = marker;
            else if (fenceMarker == marker)
                fenceMarker.clear();
            continue;
        }
        if (!fenceMarker.isEmpty())
            continue;
        IssueSubtaskLine task;
        if (!parseIssueChecklistLine(lines.at(line), line, &task) ||
            (candidatesOnly &&
             (task.checked || task.subIssueNumber > 0))) {
            continue;
        }
        tasks.append(task);
        if (maximum > 0 && tasks.size() >= maximum)
            break;
    }
    return tasks;
}

bool issueChecklistLineAt(const QString &body, int line,
                          IssueSubtaskLine *task)
{
    if (line < 0)
        return false;
    for (const IssueSubtaskLine &candidate : issueChecklistLines(body)) {
        if (candidate.line == line) {
            if (task)
                *task = candidate;
            return true;
        }
        if (candidate.line > line)
            break;
    }
    return false;
}

QList<IssueSubtaskLine> issueSubtaskLines(const QString &body)
{
    // Accept both GFM's "- [ ] task" spelling and the compact "[ ] task"
    // spelling people commonly type into the issue editor. A stable suffix is
    // deliberately visible markdown: every client can understand the relation
    // without a schema migration, and a second run can recognize it exactly.
    constexpr int kMaxSubIssuesPerBatch = 50;
    return issueChecklistLines(body, true, kMaxSubIssuesPerBatch);
}

int issueTaskBlockEnd(const QStringList &lines, int start)
{
    IssueSubtaskLine first;
    if (start < 0 || start >= lines.size() ||
        !parseIssueChecklistLine(lines.at(start), start, &first)) {
        return start + 1;
    }
    int end = start + 1;
    static const QRegularExpression listLine(
        QStringLiteral("^\\s*(?:[-*+]|\\d+[.)])\\s+"));
    static const QRegularExpression blockBoundary(
        QStringLiteral("^\\s*(?:#{1,6}\\s|```|~~~|>|(?:-{3,}|\\*{3,}|_{3,})\\s*$|<)"));
    static const QRegularExpression leadingWhitespace(
        QStringLiteral("^(\\s*)"));
    while (end < lines.size()) {
        const QString &line = lines.at(end);
        if (line.trimmed().isEmpty()) {
            int next = end + 1;
            while (next < lines.size() && lines.at(next).trimmed().isEmpty())
                ++next;
            if (next >= lines.size())
                break;
            const QRegularExpressionMatch leading =
                leadingWhitespace.match(lines.at(next));
            if (issueTaskIndent(leading.captured(1)) > first.indent) {
                end = next;
                continue;
            }
            break;
        }
        const QRegularExpressionMatch leading =
            leadingWhitespace.match(line);
        const int indent = issueTaskIndent(leading.captured(1));
        if (listLine.match(line).hasMatch() && indent <= first.indent)
            break;
        if (indent <= first.indent && blockBoundary.match(line).hasMatch())
            break;
        ++end;
    }
    return end;
}

IssueEvent effectiveOpenEvent(const Issue &issue)
{
    IssueEvent open;
    for (const IssueEvent &event : issue.events) {
        if (event.type == QLatin1String("open")) {
            open = event;
            break;
        }
    }
    if (open.id.isEmpty())
        return open;
    for (const IssueEvent &event : issue.events) {
        if (event.type == QLatin1String("edit") && event.target == open.id) {
            open.body = event.body;
            open.attachments = event.attachments;
        }
    }
    return open;
}

QStringList toStringList(const QJsonArray &array)
{
    QStringList out;
    for (const QJsonValue &value : array)
        out << value.toString();
    return out;
}

QJsonArray fromStringList(const QStringList &list)
{
    QJsonArray array;
    for (const QString &item : list)
        array.append(item);
    return array;
}

// Fold one signed "claim" into an assignee list. A claim is a delta, not a
// replacement: it adds or removes exactly the account it names and leaves
// everybody else alone, so two contributors claiming the same issue from
// different nodes converge instead of overwriting one another. Names are
// matched case-insensitively because the relay lower-cases the account session
// it checks the claim against, while the list keeps the spelling that was
// signed.
void applyIssueClaim(QStringList &assignees, const IssueEvent &ev)
{
    const QString name = ev.assignee.trimmed();
    if (name.isEmpty())
        return;
    const auto matches = [&name](const QString &existing) {
        return existing.trimmed().compare(name, Qt::CaseInsensitive) == 0;
    };
    const bool present = std::any_of(assignees.cbegin(), assignees.cend(), matches);
    if (ev.claimState == QLatin1String("released"))
        assignees.removeIf(matches);
    else if (ev.claimState == QLatin1String("assigned") && !present)
        assignees << name;
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool writeTextFile(const QString &path, const QString &text, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    file.write(text.toUtf8());
    return true;
}

bool removeOriginalRefs(const QString &workTree, QString *error)
{
    QByteArray refs;
    QString err;
    if (!runGit(workTree, {"for-each-ref", "--format=%(refname)", "refs/original"},
                &refs, &err, kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git for-each-ref failed: ") + err;
        return false;
    }
    // One `update-ref --stdin` transaction deletes every backup ref, instead of
    // a git process per branch and tag the rewrite touched.
    QByteArray deletions;
    const QList<QByteArray> lines = refs.split('\n');
    for (const QByteArray &line : lines) {
        const QByteArray ref = line.trimmed();
        if (!ref.isEmpty())
            deletions += "delete " + ref + '\n';
    }
    if (deletions.isEmpty())
        return true;
    if (!runGitInput(workTree, {"update-ref", "--stdin"}, deletions, nullptr, &err,
                     kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git update-ref failed: ") + err;
        return false;
    }
    return true;
}

bool isAllowedTrackedPath(const QString &path, const QStringList &allowedRelPaths)
{
    for (const QString &relPath : allowedRelPaths) {
        const QString prefix = relPath + QLatin1Char('/');
        if (path == relPath || path.startsWith(prefix))
            return true;
    }
    return false;
}

bool hasUnrelatedTrackedChanges(const QString &workTree,
                                const QStringList &allowedRelPaths,
                                QString *error)
{
    QByteArray status;
    QString err;
    if (!runGit(workTree, {"status", "--porcelain", "--untracked-files=no"}, &status,
                &err)) {
        if (error)
            *error = QStringLiteral("git status failed: ") + err;
        return true;
    }
    const QList<QByteArray> lines = status.split('\n');
    for (const QByteArray &line : lines) {
        if (line.size() < 4)
            continue;
        const QString path = QString::fromUtf8(line.mid(3)).trimmed();
        if (!isAllowedTrackedPath(path, allowedRelPaths)) {
            if (error) {
                *error = QStringLiteral(
                    "Commit or stash unrelated tracked changes before deleting an issue.");
            }
            return true;
        }
    }
    return false;
}

QString pendingAttachmentPlaceholder(int index)
{
    return QStringLiteral("forkmesh-pending-image:%1").arg(index);
}

QStringList effectiveAttachmentPlaceholders(const QStringList &srcPaths,
                                            const QStringList &placeholders)
{
    if (!placeholders.isEmpty())
        return placeholders;
    QStringList inferred;
    for (int i = 0; i < srcPaths.size(); ++i)
        inferred << pendingAttachmentPlaceholder(i);
    return inferred;
}

QString replaceAttachmentPlaceholders(QString body, const QStringList &srcPaths,
                                      const QStringList &placeholders,
                                      const QStringList &copiedAttachments)
{
    const QStringList effective =
        effectiveAttachmentPlaceholders(srcPaths, placeholders);
    const int count = std::min(effective.size(), copiedAttachments.size());
    QList<int> order;
    order.reserve(count);
    for (int i = 0; i < count; ++i)
        order.append(i);
    std::sort(order.begin(), order.end(), [&effective](int a, int b) {
        return effective.at(a).size() > effective.at(b).size();
    });
    for (int i : order) {
        if (!effective.at(i).isEmpty())
            body.replace(effective.at(i), copiedAttachments.at(i));
    }
    return body;
}

bool copiedEveryAttachment(const QStringList &srcPaths,
                           const QStringList &copiedAttachments,
                           QString *error)
{
    if (copiedAttachments.size() == srcPaths.size())
        return true;
    if (error) {
        *error = QStringLiteral("Could not copy %1 of %2 attachment(s).")
                     .arg(srcPaths.size() - copiedAttachments.size())
                     .arg(srcPaths.size());
    }
    return false;
}

// Content-addressed filename shared by copyAttachments() (which writes the
// file under this name) and readAttachmentsForRemoteSubmit() (which has no
// working tree to write into, but must derive the identical name so the
// signed IssueEvent::attachments list matches what the owner materializes).
QString attachmentNameFor(const QByteArray &data, const QString &srcPath)
{
    const QString sha8 = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(8));
    QString ext = QFileInfo(srcPath).suffix().toLower();
    if (ext.isEmpty())
        ext = "bin";
    return sha8 + "." + ext;
}

constexpr double kMaxJsonSafeInteger = 9007199254740991.0;

bool strictOptionalJsonSafeInteger(const QJsonObject &object,
                                   const QString &key)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined())
        return true;
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= 0 &&
           std::floor(number) == number && number <= kMaxJsonSafeInteger;
}

bool strictIssueObjectValid(const QJsonObject &object, int expectedNumber)
{
    const QJsonValue schema = object.value(QStringLiteral("schema"));
    if (!schema.isString() ||
        schema.toString() != QLatin1String("forkmesh-issue-v1")) {
        return false;
    }

    const QJsonValue number = object.value(QStringLiteral("number"));
    if (!number.isDouble() || number.toDouble() != double(expectedNumber))
        return false;

    if (!strictOptionalJsonSafeInteger(object, QStringLiteral("createdAt")) ||
        !strictOptionalJsonSafeInteger(object, QStringLiteral("startDate")) ||
        !strictOptionalJsonSafeInteger(object, QStringLiteral("endDate"))) {
        return false;
    }

    const QJsonValue eventsValue = object.value(QStringLiteral("events"));
    if (!eventsValue.isArray())
        return false;
    for (const QJsonValue &value : eventsValue.toArray()) {
        if (!value.isObject())
            return false;
        const QJsonObject event = value.toObject();
        const QJsonValue type = event.value(QStringLiteral("type"));
        const QJsonValue author = event.value(QStringLiteral("author"));
        const QJsonValue timestamp = event.value(QStringLiteral("ts"));
        const QJsonValue signature = event.value(QStringLiteral("sig"));
        if (!type.isString() || type.toString().isEmpty() ||
            !author.isString() || author.toString().isEmpty() ||
            !timestamp.isDouble() || !signature.isString() ||
            signature.toString().isEmpty()) {
            return false;
        }
        const double timestampValue = timestamp.toDouble();
        if (!std::isfinite(timestampValue) || timestampValue < 0 ||
            std::floor(timestampValue) != timestampValue ||
            timestampValue > kMaxJsonSafeInteger) {
            return false;
        }
        const QJsonValue attachments =
            event.value(QStringLiteral("attachments"));
        if (!attachments.isUndefined()) {
            if (!attachments.isArray())
                return false;
            for (const QJsonValue &attachment : attachments.toArray()) {
                if (!attachment.isString())
                    return false;
            }
        }
        if (type.toString() == QLatin1String("open") &&
            (!event.value(QStringLiteral("title")).isString() ||
             !event.value(QStringLiteral("body")).isString() ||
             !event.value(QStringLiteral("attachments")).isArray())) {
            return false;
        }
    }
    return true;
}

} // namespace

// ---- IssueEvent (JSON is the wire format for the relay inbox) ---------------

QJsonObject IssueEvent::toJson() const
{
    QJsonObject obj{{"type", type}, {"id", id}, {"author", author},
                    {"authorName", authorName},
                    {"ts", double(ts)}};
    if (type == "open" || type == "title")
        obj.insert("title", title);
    if (type == "open" || type == "comment" || type == "edit") {
        obj.insert("body", body);
        obj.insert("attachments", fromStringList(attachments));
    }
    if (type == "edit" || type == "delete" || type == "reaction")
        obj.insert("target", target);
    if (type == "reaction")
        obj.insert("reaction", reaction);
    if (type == "status")
        obj.insert("status", status);
    if (type == "labels")
        obj.insert("labels", fromStringList(labels));
    if (type == "milestone")
        obj.insert("milestone", milestone);
    if (type == "dates") {
        obj.insert("startDate", double(startDate));
        obj.insert("endDate", double(endDate));
    }
    if (type == "priority")
        obj.insert("priority", priority);
    if (type == "progress")
        obj.insert("progress", progress);
    if (type == "bounty") {
        obj.insert("bountyUsd", bountyUsd);
        obj.insert("bountyAddress", bountyAddress);
        obj.insert("bountyStatus", bountyStatus);
    }
    if (type == "assignees")
        obj.insert("assignees", fromStringList(assignees));
    if (type == "claim") {
        obj.insert("assignee", assignee);
        obj.insert("claimState", claimState);
    }
    if (type == "reporter")
        obj.insert("reporter", reporter);
    if (type == "agent") {
        obj.insert("agentProvider", agentProvider);
        obj.insert("agentSessionId", agentSessionId);
        obj.insert("agentStatus", agentStatus);
        obj.insert("agentCreatePr", agentCreatePr);
    }
    obj.insert("sig", sig);
    return obj;
}

IssueEvent IssueEvent::fromJson(const QJsonObject &obj)
{
    IssueEvent ev;
    ev.type = obj.value("type").toString();
    ev.id = obj.value("id").toString();
    ev.author = obj.value("author").toString();
    ev.authorName = obj.value("authorName").toString();
    ev.ts = qint64(obj.value("ts").toDouble());
    ev.title = obj.value("title").toString();
    ev.body = obj.value("body").toString();
    ev.attachments = toStringList(obj.value("attachments").toArray());
    ev.target = obj.value("target").toString();
    ev.reaction = obj.value("reaction").toString();
    ev.status = obj.value("status").toString();
    ev.labels = toStringList(obj.value("labels").toArray());
    ev.milestone = obj.value("milestone").toString();
    ev.startDate = qint64(obj.value("startDate").toDouble());
    ev.endDate = qint64(obj.value("endDate").toDouble());
    ev.priority = obj.value("priority").toInt();
    ev.progress = obj.value("progress").toInt();
    ev.bountyUsd = obj.value("bountyUsd").toDouble();
    ev.bountyAddress = obj.value("bountyAddress").toString();
    ev.bountyStatus = obj.value("bountyStatus").toString();
    ev.assignees = toStringList(obj.value("assignees").toArray());
    ev.assignee = obj.value("assignee").toString();
    ev.claimState = obj.value("claimState").toString();
    ev.reporter = obj.value("reporter").toString();
    ev.agentProvider = obj.value("agentProvider").toString();
    ev.agentSessionId = obj.value("agentSessionId").toInt();
    ev.agentStatus = obj.value("agentStatus").toString();
    ev.agentCreatePr = obj.value("agentCreatePr").toBool();
    ev.sig = obj.value("sig").toString();
    return ev;
}


QJsonObject Issue::toJson() const
{
    QJsonArray eventsArray;
    qint64 updatedAt = createdAt;
    int voteCount = 0;
    for (const IssueEvent &ev : events) {
        eventsArray.append(ev.toJson());
        if (ev.ts > updatedAt)
            updatedAt = ev.ts;
        if (ev.type == QLatin1String("vote"))
            ++voteCount;
    }
    return {{"schema", "forkmesh-issue-v1"}, {"number", number}, {"title", title},
            {"status", status}, {"labels", fromStringList(labels)},
            {"milestone", milestone},
            {"startDate", double(startDate)}, {"endDate", double(endDate)},
            {"priority", priority}, {"progress", progress},
            {"assignees", fromStringList(assignees)},
            {"createdAt", double(createdAt)}, {"updatedAt", double(updatedAt)},
            {"author", author},
            {"authorName", authorName}, {"bountyUsd", bountyUsd},
            {"bountyAddress", bountyAddress}, {"bountyStatus", bountyStatus},
            {"rewardEstimateLamports", double(rewardEstimateLamports)},
            {"rewardRecipient", rewardRecipient}, {"rewardId", rewardId},
            {"rewardCondition", rewardCondition},
            {"rewardStatus", rewardStatus},
            {"rewardTxSignature", rewardTxSignature},
            {"votes", voteCount},
            {"events", eventsArray}};
}

Issue Issue::fromJson(const QJsonObject &obj)
{
    Issue issue;
    issue.number = obj.value("number").toInt();
    issue.title = obj.value("title").toString();
    issue.status = obj.value("status").toString("open");
    issue.labels = toStringList(obj.value("labels").toArray());
    issue.milestone = obj.value("milestone").toString();
    issue.startDate = qint64(obj.value("startDate").toDouble());
    issue.endDate = qint64(obj.value("endDate").toDouble());
    issue.priority = obj.value("priority").toInt();
    issue.progress = obj.value("progress").toInt();
    issue.assignees = toStringList(obj.value("assignees").toArray());
    issue.createdAt = qint64(obj.value("createdAt").toDouble());
    issue.author = obj.value("author").toString();
    issue.authorName = obj.value("authorName").toString();
    issue.bountyUsd = obj.value("bountyUsd").toDouble();
    issue.bountyAddress = obj.value("bountyAddress").toString();
    issue.bountyStatus = obj.value("bountyStatus").toString();
    issue.rewardEstimateLamports =
        qint64(obj.value("rewardEstimateLamports").toDouble());
    issue.rewardRecipient = obj.value("rewardRecipient").toString();
    issue.rewardId = obj.value("rewardId").toString();
    issue.rewardCondition = obj.value("rewardCondition").toString();
    issue.rewardStatus = obj.value("rewardStatus").toString();
    issue.rewardTxSignature = obj.value("rewardTxSignature").toString();
    for (const QJsonValue &value : obj.value("events").toArray())
        issue.events.append(IssueEvent::fromJson(value.toObject()));
    return issue;
}

bool Issue::isDeleted() const
{
    // Authoritative deletion: the issue's own creator (the open event's author)
    // signed the delete/self event. A tombstone from anyone else is not honoured
    // here (see hasUnauthorizedDeleteAttempt) — owners remove others' issues with
    // the hard "Delete with history", which erases the folder outright.
    QString creator;
    for (const IssueEvent &ev : events)
        if (ev.type == "open") {
            creator = ev.author;
            break;
        }
    for (const IssueEvent &ev : events)
        if (ev.type == "delete" && ev.target == "self" && !ev.author.isEmpty() &&
            ev.author == creator)
            return true;
    return false;
}

bool Issue::hasUnauthorizedDeleteAttempt() const
{
    // A delete/self event signed by someone other than the creator: someone
    // tried to delete an issue they didn't open. The issue stays open and
    // counted; the UI flags the attempt instead of hiding the issue.
    QString creator;
    for (const IssueEvent &ev : events)
        if (ev.type == "open") {
            creator = ev.author;
            break;
        }
    for (const IssueEvent &ev : events)
        if (ev.type == "delete" && ev.target == "self" && ev.author != creator)
            return true;
    return false;
}


IssueStore::IssueStore(QString workTreePath, QString mirrorPath,
                       const ForkMeshIdentity *identity, QString authorName)
    : m_workTree(std::move(workTreePath)), m_mirror(std::move(mirrorPath)),
      m_identity(identity), m_authorName(std::move(authorName))
{
}

bool IssueStore::canWrite() const
{
    if (m_workTree.isEmpty() || !m_identity || !m_identity->isValid())
        return false;
    return QFileInfo::exists(m_workTree + "/.git");
}

QString IssueStore::issuesDir() const
{
    return QDir(m_workTree).filePath(issuesRootRel());
}

QString IssueStore::issueDirPath(const QString &workTree, int number)
{
    const QStringList candidates = issueDirRelCandidates(number);
    for (const QString &rel : candidates) {
        const QString abs = QDir(workTree).filePath(rel);
        if (QDir(abs).exists())
            return abs;
    }
    return QDir(workTree).filePath(candidates.first());
}

QString IssueStore::issueDir(int number) const
{
    return issueDirPath(m_workTree, number);
}

QString IssueStore::issueFilePath(int number) const
{
    return QDir(issueDir(number)).filePath(issueJsonFileName(number));
}

// ---- Signing ---------------------------------------------------------------

QString IssueStore::contentForSigning(const IssueEvent &ev)
{
    const QChar nul(QChar::Null);
    if (ev.type == "open")
        return ev.title + nul + ev.body + nul + ev.attachments.join(",");
    if (ev.type == "comment" || ev.type == "edit")
        return ev.body + nul + ev.attachments.join(",");
    if (ev.type == "title")
        return ev.title;
    if (ev.type == "status")
        return ev.status;
    if (ev.type == "labels")
        return ev.labels.join(",");
    if (ev.type == "milestone")
        return ev.milestone;
    if (ev.type == "dates")
        return QString::number(ev.startDate) + nul + QString::number(ev.endDate);
    if (ev.type == "priority")
        return QString::number(ev.priority);
    if (ev.type == "progress")
        return QString::number(ev.progress);
    if (ev.type == "bounty")
        return QString::number(ev.bountyUsd, 'f', 2) + nul + ev.bountyAddress + nul +
               ev.bountyStatus;
    if (ev.type == "assignees")
        return ev.assignees.join(",");
    if (ev.type == "claim")
        return ev.assignee + nul + ev.claimState;
    if (ev.type == "reporter")
        return ev.reporter;
    if (ev.type == "agent")
        return ev.agentProvider + nul + QString::number(ev.agentSessionId) + nul +
               ev.agentStatus + nul + (ev.agentCreatePr ? "pr" : "no-pr");
    if (ev.type == "reaction")
        return ev.target + nul + ev.reaction;
    if (ev.type == "delete")
        return ev.target;
    return QString();
}

void IssueStore::setSigningRepository(const QString &owner, const QString &name)
{
    m_signingRepoKey = forkmesh::eventsigning::repoKey(owner, name);
}

QByteArray IssueStore::canonicalString(int number, const IssueEvent &ev,
                                       const QString &repoKey)
{
    const QByteArray contentHash =
        QCryptographicHash::hash(contentForSigning(ev).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex();
    QByteArray canonical = forkmesh::eventsigning::prefix("issue-event", repoKey);
    canonical += ev.type.toUtf8() + "\n";
    canonical += QByteArray::number(number) + "\n";
    canonical += ev.author.toUtf8() + "\n";
    canonical += QByteArray::number(ev.ts) + "\n";
    canonical += contentHash;
    return canonical;
}

IssueEvent IssueStore::makeSignedEvent(int number, IssueEvent ev) const
{
    if (ev.id.isEmpty())
        ev.id = newId();
    ev.author = m_identity ? m_identity->publicKey() : QString();
    if (ev.authorName.isEmpty())
        ev.authorName = m_authorName;
    if (ev.ts == 0)
        ev.ts = QDateTime::currentMSecsSinceEpoch();
    ev.sig = m_identity ? m_identity->signData(canonicalString(number, ev, m_signingRepoKey))
                        : QString();
    return ev;
}

QList<RemoteAttachment> IssueStore::readAttachmentsForRemoteSubmit(
    const QStringList &srcPaths)
{
    QList<RemoteAttachment> out;
    for (const QString &src : srcPaths) {
        QFile in(src);
        if (!in.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = in.readAll();
        out.append(RemoteAttachment{attachmentNameFor(data, src), data});
    }
    return out;
}

QString IssueStore::substituteAttachmentPlaceholders(
    QString body, const QStringList &srcPaths, const QStringList &placeholders,
    const QStringList &attachmentNames)
{
    return replaceAttachmentPlaceholders(body, srcPaths, placeholders, attachmentNames);
}


QList<Issue> IssueStore::loadAll(QString *error, const std::function<void()> &tick) const
{
    if (!canWrite())
        return loadFromMirror(error);

    migrateLegacyLayout();

    QList<Issue> issues;
    QList<int> numbers;
    QSet<int> seen;
    QHash<int, QString> folderStatus; // number -> "open"/"closed"; empty = legacy
    const QList<QPair<QString, QString>> roots{
        {issuesDir() + QLatin1Char('/') + openDirName(), QStringLiteral("open")},
        {issuesDir() + QLatin1Char('/') + closedDirName(), QStringLiteral("closed")},
        {issuesDir(), QString()}};
    for (const QPair<QString, QString> &root : roots)
        for (int number : numericDirEntries(root.first))
            if (!seen.contains(number)) {
                seen.insert(number);
                numbers.append(number);
                if (!root.second.isEmpty())
                    folderStatus.insert(number, root.second);
            }
    for (int number : std::as_const(numbers)) {
        Issue issue;
        if (readIssueFile(number, issue)) {
            const QString folder = folderStatus.value(number);
            if (!folder.isEmpty())
                issue.status = folder;
            issues.append(issue);
        } else {
            Issue placeholder;
            placeholder.number = number;
            placeholder.status = QStringLiteral("open");
            placeholder.title =
                QStringLiteral("issue #%1 (couldn't load from mirror)").arg(number);
            issues.append(placeholder);
        }
        if (tick)
            tick();
    }

    std::sort(issues.begin(), issues.end(),
              [](const Issue &a, const Issue &b) { return a.number < b.number; });
    return issues;
}

QList<Issue> IssueStore::loadAllStrict(QString *error) const
{
    if (error)
        error->clear();
    if (canWrite()) {
        const IssueStore mirrorView(QString(), m_workTree, nullptr);
        return mirrorView.loadFromMirror(error, true);
    }
    return loadFromMirror(error, true);
}

QList<Issue> IssueStore::loadAllStrictAtRef(const QString &ref,
                                            QString *error) const
{
    if (error)
        error->clear();
    if (canWrite()) {
        const IssueStore mirrorView(QString(), m_workTree, nullptr);
        return mirrorView.loadFromMirror(error, true, ref);
    }
    return loadFromMirror(error, true, ref);
}

QString IssueStore::contentSignature() const
{
    const QString dir = canWrite() ? m_workTree : m_mirror;
    if (dir.isEmpty())
        return QString();
    const QString ref = canWrite() ? QStringLiteral("HEAD") : mirrorRef();
    if (ref.isEmpty())
        return QString();
    QStringList parts;
    QByteArray oid;
    runGit(dir,
           {"rev-parse", "--verify", "-q",
            ref + QLatin1Char(':') + issuesRootRel()},
           &oid);
    const QString trimmed = QString::fromUtf8(oid).trimmed();
    if (!trimmed.isEmpty())
        parts << issuesRootRel() + QLatin1Char('=') + trimmed;
    return dir + QLatin1Char('\n') + parts.join(QLatin1Char('\n'));
}

bool IssueStore::readIssueFile(int number, Issue &out) const
{
    QFile jsonFile(issueFilePath(number));
    if (!jsonFile.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    out = Issue::fromJson(doc.object());
    if (out.number <= 0)
        out.number = number;
    recomputeMetadata(out);
    return true;
}


QString IssueStore::mirrorRef() const
{
    // The mirror doesn't change while a single store reads it, but mirrorRef() is
    // hit once per blob fetched from the mirror. Resolving it via git every time
    // spawns hundreds of subprocesses on the GUI thread; memoize for our lifetime.
    if (m_mirrorRefResolved)
        return m_cachedMirrorRef;
    m_mirrorRefResolved = true;

    QByteArray output;
    if (runGit(m_mirror, {"rev-parse", "--verify", "-q", "HEAD"}, &output) &&
        !output.trimmed().isEmpty()) {
        m_cachedMirrorRef = QStringLiteral("HEAD");
    } else if (runGit(m_mirror,
                      {"for-each-ref", "--format=%(refname)", "--count=1",
                       "refs/heads/"},
                      &output)) {
        m_cachedMirrorRef = QString::fromUtf8(output).trimmed();
    }
    return m_cachedMirrorRef;
}

QByteArray IssueStore::showFromMirror(const QString &repoRelPath, bool *ok,
                                      const QString &refOverride) const
{
    const QString ref = refOverride.isEmpty() ? mirrorRef() : refOverride;
    QByteArray output;
    const bool good = !ref.isEmpty() &&
                      runGit(m_mirror, {"show", ref + ":" + repoRelPath}, &output);
    if (ok)
        *ok = good;
    return good ? output : QByteArray();
}

QList<Issue> IssueStore::loadFromMirror(QString *error, bool strict,
                                        const QString &refOverride) const
{
    QList<Issue> issues;
    StrictGitReadInternal::Reader strictReader;
    auto readGit = [&](const QStringList &args, QByteArray *output = nullptr) {
        return strict ? strictReader.run(m_mirror, args, output)
                      : runGit(m_mirror, args, output);
    };
    auto readGitInput = [&](const QStringList &args, const QByteArray &input,
                            QByteArray *output) {
        return strict ? strictReader.runInput(m_mirror, args, input, output)
                      : runGitInput(m_mirror, args, input, output);
    };
    auto recordStrictError = [&](const QString &message) {
        if (strict && error && error->isEmpty())
            *error = message;
    };
    if (m_mirror.isEmpty()) {
        if (error)
            *error = QStringLiteral("No local mirror to read issues from.");
        return issues;
    }
    QString ref = refOverride;
    if (ref.isEmpty() && strict) {
        QByteArray output;
        if (readGit({"rev-parse", "--verify", "-q", "HEAD"}, &output) &&
            !output.trimmed().isEmpty()) {
            ref = QStringLiteral("HEAD");
        } else if (readGit({"for-each-ref", "--format=%(refname)",
                            "--count=1", "refs/heads/"},
                           &output)) {
            ref = QString::fromUtf8(output).trimmed();
        }
    } else if (ref.isEmpty()) {
        ref = mirrorRef();
    }
    if (ref.isEmpty()) {
        recordStrictError(QStringLiteral("Could not resolve issue metadata HEAD."));
        return issues;
    }

    auto fetchOids = [&](const QSet<QString> &wantedOids) {
        QHash<QString, QByteArray> contentByOid;
        if (wantedOids.isEmpty())
            return contentByOid;

        QByteArray batchInput;
        for (const QString &oid : wantedOids)
            batchInput += oid.toUtf8() + '\n';
        QByteArray batch;
        if (!readGitInput({"cat-file", "--batch"}, batchInput, &batch)) {
            recordStrictError(
                QStringLiteral("Could not read issue metadata blobs."));
            return contentByOid;
        }

        contentByOid.reserve(wantedOids.size());
        for (int pos = 0; pos < batch.size();) {
            const int nl = batch.indexOf('\n', pos);
            if (nl < 0) {
                recordStrictError(
                    QStringLiteral("Issue metadata blob framing is invalid."));
                break;
            }
            const QList<QByteArray> header = batch.mid(pos, nl - pos).split(' ');
            pos = nl + 1;
            if (header.size() < 3) { // "<oid> missing" or malformed, no body follows
                recordStrictError(
                    QStringLiteral("An issue metadata blob is missing."));
                continue;
            }
            bool sizeOk = false;
            const int size = header.at(2).toInt(&sizeOk);
            if (!sizeOk || size < 0 || size > batch.size() - pos) {
                recordStrictError(
                    QStringLiteral("Issue metadata blob framing is invalid."));
                break;
            }
            contentByOid.insert(QString::fromUtf8(header.at(0)),
                                batch.mid(pos, size));
            pos += size + 1; // skip body and its trailing newline
        }
        if (contentByOid.size() != wantedOids.size()) {
            recordStrictError(
                QStringLiteral("Not all issue metadata blobs could be read."));
        }
        return contentByOid;
    };

    QByteArray listing;
    QHash<int, QString> folderStatus;
    if (readGit({"ls-tree", "-r", ref, issuesRootRel() + "/"}, &listing)) {
        QMap<int, QString> issueJsonOids;
        QSet<QString> wantedOids;
        const QString prefix = issuesRootRel() + QLatin1Char('/');
        for (const QString &line :
             QString::fromUtf8(listing).split('\n', Qt::SkipEmptyParts)) {
            const int tab = line.indexOf('\t');
            if (tab < 0)
                continue;
            const QStringList meta = line.left(tab).split(' ', Qt::SkipEmptyParts);
            if (meta.size() < 3 || meta.at(1) != QStringLiteral("blob"))
                continue;
            const QString path = line.mid(tab + 1);
            if (!path.startsWith(prefix))
                continue;
            QString rel = path.mid(prefix.size());
            QString relFolderStatus;
            if (rel.startsWith(openDirName() + QLatin1Char('/'))) {
                rel = rel.mid(openDirName().size() + 1);
                relFolderStatus = QStringLiteral("open");
            } else if (rel.startsWith(closedDirName() + QLatin1Char('/'))) {
                rel = rel.mid(closedDirName().size() + 1);
                relFolderStatus = QStringLiteral("closed");
            }
            const int slash = rel.indexOf('/');
            if (slash < 0)
                continue;
            bool numeric = false;
            const int number = rel.left(slash).toInt(&numeric);
            if (!numeric)
                continue;
            const QString fileName = rel.mid(slash + 1);
            if (fileName == issueJsonFileName(number)) {
                const QString oid = meta.at(2);
                issueJsonOids.insert(number, oid);
                wantedOids.insert(oid);
                if (!relFolderStatus.isEmpty())
                    folderStatus.insert(number, relFolderStatus);
            }
        }

        QHash<QString, QByteArray> contentByOid = fetchOids(wantedOids);
        if (!strict) {
            QSet<QString> missingOids;
            for (auto it = issueJsonOids.constBegin();
                 it != issueJsonOids.constEnd(); ++it)
                if (!contentByOid.contains(it.value()))
                    missingOids.insert(it.value());
            if (!missingOids.isEmpty()) {
                const QHash<QString, QByteArray> retry = fetchOids(missingOids);
                for (auto it = retry.constBegin(); it != retry.constEnd(); ++it)
                    contentByOid.insert(it.key(), it.value());
            }
        }
        for (auto it = issueJsonOids.constBegin(); it != issueJsonOids.constEnd(); ++it) {
            auto keepPlaceholder = [&](const QString &reason) {
                recordStrictError(reason);
                if (strict)
                    return; // strict verification drops and reports instead
                Issue placeholder;
                placeholder.number = it.key();
                placeholder.status = QStringLiteral("open");
                placeholder.title =
                    QStringLiteral("issue #%1 (couldn't load from mirror)")
                        .arg(it.key());
                issues.append(placeholder);
            };
            if (!contentByOid.contains(it.value())) {
                keepPlaceholder(
                    QStringLiteral("An issue metadata blob could not be read."));
                continue;
            }
            QJsonParseError parseError;
            const QJsonDocument doc =
                QJsonDocument::fromJson(contentByOid.value(it.value()), &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                keepPlaceholder(
                    QStringLiteral("An issue metadata file is invalid."));
                continue;
            }
            if (strict && !strictIssueObjectValid(doc.object(), it.key())) {
                recordStrictError(
                    QStringLiteral("An issue metadata file is invalid."));
                continue;
            }
            Issue issue = Issue::fromJson(doc.object());
            if (issue.number <= 0)
                issue.number = it.key();
            recomputeMetadata(issue);
            // Folder wins over a stale record status for the open/closed tally.
            // Strict verification compares records byte-for-byte for signed
            // snapshots, so it keeps the record's own status untouched.
            if (!strict) {
                const QString folder = folderStatus.value(it.key());
                if (!folder.isEmpty())
                    issue.status = folder;
            }
            issues.append(issue);
        }
    } else {
        recordStrictError(
            QStringLiteral("Could not enumerate issue metadata."));
    }

    std::sort(issues.begin(), issues.end(),
              [](const Issue &a, const Issue &b) { return a.number < b.number; });
    return issues;
}

QList<IssueLabel> IssueStore::loadLabels() const
{
    QByteArray bytes;
    if (canWrite()) {
        QFile file(issuesDir() + "/labels.json");
        if (file.open(QIODevice::ReadOnly))
            bytes = file.readAll();
    } else {
        bool ok = false;
        bytes = showFromMirror(issuesRootRel() + "/labels.json", &ok);
    }
    QList<IssueLabel> labels;
    for (const QJsonValue &value : QJsonDocument::fromJson(bytes).array()) {
        const QJsonObject obj = value.toObject();
        labels.append({obj.value("name").toString(), obj.value("color").toString()});
    }
    return labels;
}

QList<IssueMilestone> IssueStore::loadMilestones() const
{
    QByteArray bytes;
    if (canWrite()) {
        QFile file(issuesDir() + "/milestones.json");
        if (file.open(QIODevice::ReadOnly))
            bytes = file.readAll();
    } else {
        bool ok = false;
        bytes = showFromMirror(issuesRootRel() + "/milestones.json", &ok);
    }
    QList<IssueMilestone> milestones;
    for (const QJsonValue &value : QJsonDocument::fromJson(bytes).array()) {
        const QJsonObject obj = value.toObject();
        milestones.append({obj.value("title").toString(),
                           qint64(obj.value("due").toDouble()),
                           obj.value("status").toString("open"),
                           obj.value("description").toString()});
    }
    return milestones;
}


bool IssueStore::writeIssueFile(const Issue &issue, QString *error) const
{
    if (issue.events.isEmpty()) {
        if (error)
            *error = QStringLiteral("Issue #%1 has no events.").arg(issue.number);
        return false;
    }
    Issue stored = issue;
    recomputeMetadata(stored);
    const QString desired =
        QDir(issuesDir())
            .filePath(statusDirNameFor(stored.status) + QLatin1Char('/') +
                      issueMediaDirName(stored.number));
    const QString current = issueDir(stored.number);
    if (QDir(current).exists() && current != desired) {
        QDir().mkpath(QFileInfo(desired).absolutePath());
        if (!QDir().rename(current, desired)) {
            if (error)
                *error = QStringLiteral("Could not move issue #%1 to %2.")
                             .arg(stored.number)
                             .arg(statusDirNameFor(stored.status));
            return false;
        }
    }
    QDir().mkpath(desired);
    const QByteArray bytes = QJsonDocument(stored.toJson()).toJson(QJsonDocument::Indented);
    if (!writeTextFile(QDir(desired).filePath(issueJsonFileName(stored.number)),
                       QString::fromUtf8(bytes), error))
        return false;
    return true;
}

void IssueStore::recomputeMetadata(Issue &issue) const
{
    int voteCount = 0;
    for (const IssueEvent &ev : issue.events) {
        if (ev.type == "open")
            issue.title = ev.title;
        else if (ev.type == "title" && !ev.title.isEmpty())
            issue.title = ev.title;  // later title events rename the issue
        else if (ev.type == "status")
            issue.status = ev.status;
        else if (ev.type == "labels")
            issue.labels = ev.labels;
        else if (ev.type == "milestone")
            issue.milestone = ev.milestone;
        else if (ev.type == "dates") {
            issue.startDate = ev.startDate;
            issue.endDate = ev.endDate;
        } else if (ev.type == "priority")
            issue.priority = ev.priority;
        else if (ev.type == "progress")
            issue.progress = ev.progress;
        else if (ev.type == "bounty") {
            issue.bountyUsd = ev.bountyUsd;
            issue.bountyAddress = ev.bountyAddress;
            issue.bountyStatus = ev.bountyStatus;
        } else if (ev.type == "assignees")
            issue.assignees = ev.assignees;
        else if (ev.type == "claim")
            applyIssueClaim(issue.assignees, ev);
        else if (ev.type == "reporter" && !ev.reporter.isEmpty())
            // Only a signed reporter event moves the credit: an issue filed on
            // someone's behalf already stores a top-level authorName that
            // disagrees with the open event's, and re-folding "open" here would
            // hand every one of those reports back to the node that typed it.
            issue.authorName = ev.reporter;
        else if (ev.type == "vote" && !ev.author.isEmpty())
            // Voters may now spend multiple credits on the same issue, so every
            // signed vote event counts (no longer one-per-author).
            ++voteCount;
    }
    issue.votes = voteCount;
}

QStringList IssueStore::copyAttachments(int number, const QStringList &srcPaths) const
{
    QStringList rel;
    QDir().mkpath(issueDir(number));
    for (const QString &src : srcPaths) {
        QFile in(src);
        if (!in.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = in.readAll();
        const QString name = attachmentNameFor(data, src);
        QFile out(issueDir(number) + "/" + name);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(data);
            rel << name;
        }
    }
    return rel;
}

void IssueStore::writeRemoteAttachmentFiles(int number, const QStringList &names,
                                            const QList<RemoteAttachment> &attachments) const
{
    if (names.isEmpty() || attachments.isEmpty())
        return;
    QDir().mkpath(issueDir(number));
    for (const QString &name : names) {
        for (const RemoteAttachment &att : attachments) {
            if (att.name != name)
                continue;
            QFile out(issueDir(number) + "/" + name);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                out.write(att.data);
            break;
        }
    }
}

bool IssueStore::commit(const QString &message, QString *error) const
{
    QString err;
    QStringList paths{issuesRootRel()};
    if (!runGit(m_workTree, QStringList{"add", "-A", "--"} + paths, nullptr, &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    QByteArray out;
    if (!runGit(m_workTree, QStringList{"commit", "-m", message, "--"} + paths, &out,
                &err)) {
        if (err.contains("nothing to commit") || err.isEmpty())
            return true;
        if (error)
            *error = "git commit failed: " + err;
        return false;
    }
    return true;
}

int IssueStore::nextNumber() const
{
    int max = 0;
    const QStringList roots{issuesDir() + QLatin1Char('/') + openDirName(),
                            issuesDir() + QLatin1Char('/') + closedDirName(),
                            issuesDir()};
    for (const QString &root : roots)
        for (int n : numericDirEntries(root))
            max = std::max(max, n);
    return max + 1;
}

void IssueStore::migrateLegacyLayout() const
{
    if (!canWrite())
        return;
    const QList<int> legacy = numericDirEntries(issuesDir());
    if (legacy.isEmpty())
        return;
    bool moved = false;
    for (int number : legacy) {
        const QString from =
            QDir(issuesDir()).filePath(issueMediaDirName(number));
        QString status = QStringLiteral("open");
        QFile jsonFile(QDir(from).filePath(issueJsonFileName(number)));
        if (jsonFile.open(QIODevice::ReadOnly)) {
            Issue issue = Issue::fromJson(
                QJsonDocument::fromJson(jsonFile.readAll()).object());
            recomputeMetadata(issue);
            status = issue.status;
        }
        const QString to =
            QDir(issuesDir())
                .filePath(statusDirNameFor(status) + QLatin1Char('/') +
                          issueMediaDirName(number));
        if (QDir(to).exists())
            continue; // both layouts hold this number; readers prefer the split copy
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (QDir().rename(from, to))
            moved = true;
    }
    if (moved)
        commit(QStringLiteral("issues: split into open/ and closed/ folders"),
               nullptr);
}


int IssueStore::createIssue(const QString &title, const QString &body,
                            const QStringList &labels, const QString &milestone,
                            int priority,
                            const QStringList &assignees,
                            const QStringList &attachmentSrcPaths, QString *error,
                            Issue *createdOut)
{
    return createIssue(title, body, labels, milestone, priority, assignees,
                       attachmentSrcPaths, {}, error, createdOut);
}

int IssueStore::createIssue(const QString &title, const QString &body,
                            const QStringList &labels, const QString &milestone,
                            int priority,
                            const QStringList &assignees,
                            const QStringList &attachmentSrcPaths,
                            const QStringList &attachmentPlaceholders, QString *error,
                            Issue *createdOut)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return -1;
    }
    const int number = nextNumber();
    QDir().mkpath(issueDir(number));

    IssueEvent ev;
    ev.type = "open";
    ev.id = QStringLiteral("open-%1").arg(number);
    ev.title = title;
    ev.attachments = copyAttachments(number, attachmentSrcPaths);
    if (!copiedEveryAttachment(attachmentSrcPaths, ev.attachments, error))
        return -1;
    ev.body = stripEdgeNewlines(replaceAttachmentPlaceholders(
        body, attachmentSrcPaths, attachmentPlaceholders, ev.attachments));
    ev = makeSignedEvent(number, ev);

    Issue issue;
    issue.number = number;
    issue.title = title;
    issue.status = "open";
    issue.labels = labels;
    issue.milestone = milestone;
    issue.priority = qBound(0, priority, 8999);
    issue.assignees = assignees;
    issue.createdAt = ev.ts;
    issue.author = ev.author;
    issue.authorName = ev.authorName;
    issue.events.append(ev);

    if (!writeIssueFile(issue, error))
        return -1;
    if (!commit(QStringLiteral("issue #%1: %2").arg(number).arg(title), error))
        return -1;
    if (createdOut)
        *createdOut = issue;
    return number;
}

QStringList IssueStore::subIssueTitles(const QString &body)
{
    QStringList titles;
    for (const IssueSubtaskLine &task : issueSubtaskLines(body))
        titles.append(task.title);
    return titles;
}

namespace {
// Sub-issues keep their relationship in the signed opening Markdown so every
// client and mirror can read it. A hand-authored issue may list several
// parents, hence the plural spelling and all references on the relationship
// line are accepted here too. Planning templates write the line as a list item
// ("- Depends on: #481, #484") or in bold, so a bullet and ** are allowed.
const QRegularExpression &issueDependencyLinePattern()
{
    static const QRegularExpression relation(
        QStringLiteral(R"(^\s*(?:[-*+]\s+)?(?:\*\*)?(?:parent\s+issues?|depends?\s+on)(?:\*\*)?\s*:(?:\*\*)?\s*(.+)$)"),
        QRegularExpression::CaseInsensitiveOption);
    return relation;
}

const QRegularExpression &issueDependencyReferencePattern()
{
    static const QRegularExpression reference(QStringLiteral(R"(#([1-9]\d*)\b)"));
    return reference;
}
} // namespace

QList<int> IssueStore::dependencyNumbers(const QString &body)
{
    QList<int> numbers;
    QSet<int> seen;
    for (const QString &line : body.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch relation =
            issueDependencyLinePattern().match(line);
        if (!relation.hasMatch())
            continue;
        QRegularExpressionMatchIterator references =
            issueDependencyReferencePattern().globalMatch(relation.captured(1));
        while (references.hasNext()) {
            const int number = references.next().captured(1).toInt();
            if (number <= 0 || seen.contains(number))
                continue;
            seen.insert(number);
            numbers.append(number);
            if (numbers.size() == 25)
                return numbers;
        }
    }
    return numbers;
}

QString IssueStore::bodyWithoutDependencyLines(const QString &body)
{
    QStringList lines = body.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines) {
        // "Depends on: None" names nothing, so it stays readable prose.
        const QRegularExpressionMatch relation =
            issueDependencyLinePattern().match(line);
        if (relation.hasMatch() &&
            issueDependencyReferencePattern().match(relation.captured(1)).hasMatch())
            line.clear();
    }
    return lines.join(QLatin1Char('\n'));
}

QString IssueStore::effectiveBody(const Issue &issue)
{
    return effectiveOpenEvent(issue).body;
}

QList<int> IssueStore::referencedIssueNumbers(const QString &body)
{
    QList<int> numbers;
    QSet<int> seen;
    // A reference is a #N that is not glued to a word, path, or HTML entity.
    static const QRegularExpression reference(
        QStringLiteral(R"((?<![\w&#/])#([1-9]\d{0,8})\b)"));
    static const QRegularExpression inlineCode(QStringLiteral("`+[^`]*`+"));
    bool inFence = false;
    QString fenceMarker;
    for (const QString &rawLine : body.split(QLatin1Char('\n'))) {
        const QString trimmed = rawLine.trimmed();
        const bool fenceLine = trimmed.startsWith(QStringLiteral("```")) ||
                               trimmed.startsWith(QStringLiteral("~~~"));
        if (inFence) {
            if (fenceLine && trimmed.startsWith(fenceMarker))
                inFence = false;
            continue;
        }
        if (fenceLine) {
            inFence = true;
            fenceMarker = trimmed.left(3);
            continue;
        }
        QString line = rawLine;
        line.replace(inlineCode, QStringLiteral(" "));
        QRegularExpressionMatchIterator it = reference.globalMatch(line);
        while (it.hasNext()) {
            const int number = it.next().captured(1).toInt();
            if (number > 0 && !seen.contains(number)) {
                seen.insert(number);
                numbers.append(number);
            }
        }
    }
    return numbers;
}

QList<int> IssueStore::subIssueNumbers(const QList<Issue> &issues,
                                       int parentNumber)
{
    QSet<int> children;
    if (parentNumber <= 0)
        return {};
    QSet<int> known;
    for (const Issue &issue : issues) {
        if (issue.isDeleted())
            continue;
        known.insert(issue.number);
        if (issue.number != parentNumber &&
            dependencyNumbers(effectiveBody(issue)).contains(parentNumber)) {
            children.insert(issue.number);
        }
    }
    // A checklist row converted on another client links its child before the
    // child's own record has necessarily reached this mirror; show it anyway.
    for (const Issue &issue : issues) {
        if (issue.number != parentNumber)
            continue;
        for (const IssueChecklistTask &task : checklistTasks(effectiveBody(issue))) {
            if (task.subIssueNumber > 0 && task.subIssueNumber != parentNumber)
                children.insert(task.subIssueNumber);
        }
        break;
    }
    QList<int> ordered(children.cbegin(), children.cend());
    std::sort(ordered.begin(), ordered.end());
    return ordered;
}

QList<IssueMention> IssueStore::mentionsOf(const QList<Issue> &issues,
                                           int number)
{
    QList<IssueMention> mentions;
    if (number <= 0)
        return mentions;
    for (const Issue &source : issues) {
        if (source.number == number || source.isDeleted())
            continue;
        QHash<QString, QString> editedBodies;
        QSet<QString> deleted;
        for (const IssueEvent &ev : source.events) {
            if (ev.type == QLatin1String("edit") && !ev.target.isEmpty())
                editedBodies.insert(ev.target, ev.body);
            else if (ev.type == QLatin1String("delete") && !ev.target.isEmpty())
                deleted.insert(ev.target);
        }
        QSet<QString> credited;
        for (const IssueEvent &ev : source.events) {
            const bool opening = ev.type == QLatin1String("open");
            if (!opening && ev.type != QLatin1String("comment"))
                continue;
            if (deleted.contains(ev.id))
                continue;
            const QString body = editedBodies.value(ev.id, ev.body);
            if (!referencedIssueNumbers(body).contains(number))
                continue;
            // A signed "reporter" event re-credits the description.
            const QString name = opening && !source.authorName.trimmed().isEmpty()
                ? source.authorName
                : ev.authorName;
            const QString who = name.trimmed().isEmpty() ? ev.author : name.trimmed();
            if (credited.contains(who))
                continue;
            credited.insert(who);
            IssueMention mention;
            mention.sourceNumber = source.number;
            mention.sourceTitle = source.title;
            mention.sourceStatus = source.status;
            mention.author = ev.author;
            mention.authorName = name;
            mention.ts = ev.ts;
            mentions.append(mention);
        }
    }
    std::stable_sort(mentions.begin(), mentions.end(),
                     [](const IssueMention &a, const IssueMention &b) {
                         return a.ts < b.ts;
                     });
    return mentions;
}

QList<IssueChecklistTask> IssueStore::checklistTasks(const QString &body)
{
    QList<IssueChecklistTask> tasks;
    for (const IssueSubtaskLine &task : issueChecklistLines(body)) {
        tasks.append({task.line, task.indent, task.checked, task.label,
                      task.subIssueNumber});
    }
    return tasks;
}

QString IssueStore::setChecklistTaskChecked(const QString &body, int line,
                                            bool checked, bool *changed)
{
    if (changed)
        *changed = false;
    QStringList lines = body.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (line < 0 || line >= lines.size())
        return body;
    IssueSubtaskLine task;
    if (!issueChecklistLineAt(body, line, &task) ||
        task.checked == checked) {
        return body;
    }
    // The state character immediately follows `beforeState`; replacing that
    // one code unit preserves bullets, spacing, suffixes, and CRLF exactly.
    lines[line][task.beforeState.size()] = checked ? QLatin1Char('x')
                                                   : QLatin1Char(' ');
    if (changed)
        *changed = true;
    return lines.join(QLatin1Char('\n'));
}

QString IssueStore::reorderChecklistTask(const QString &body, int fromLine,
                                         int toLine, bool placeAfter,
                                         bool *changed)
{
    if (changed)
        *changed = false;
    QStringList lines = body.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (fromLine < 0 || toLine < 0 || fromLine >= lines.size() ||
        toLine >= lines.size() || fromLine == toLine) {
        return body;
    }
    IssueSubtaskLine source;
    IssueSubtaskLine target;
    if (!issueChecklistLineAt(body, fromLine, &source) ||
        !issueChecklistLineAt(body, toLine, &target) ||
        source.indent != target.indent) {
        return body;
    }

    const int sourceEnd = issueTaskBlockEnd(lines, fromLine);
    const int targetEnd = issueTaskBlockEnd(lines, toLine);
    QStringList moved;
    for (int line = fromLine; line < sourceEnd; ++line)
        moved.append(lines.at(line));
    for (int count = moved.size(); count > 0; --count)
        lines.removeAt(fromLine);

    const int removedBeforeTarget = fromLine < toLine ? moved.size() : 0;
    const int adjustedTargetStart = toLine - removedBeforeTarget;
    const int adjustedTargetEnd = targetEnd - removedBeforeTarget;
    const int insertAt = placeAfter ? adjustedTargetEnd : adjustedTargetStart;
    for (int i = 0; i < moved.size(); ++i)
        lines.insert(insertAt + i, moved.at(i));

    const QString reordered = lines.join(QLatin1Char('\n'));
    if (changed)
        *changed = reordered != body;
    return reordered;
}

// code-health: allow(dir-walk-in-loop) removeRecursively only rolls back the sub-issue folders just written, on a write failure that returns
bool IssueStore::createSubIssues(int parentNumber, QList<int> *createdNumbers,
                                 QString *error)
{
    if (createdNumbers)
        createdNumbers->clear();
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }

    Issue parent;
    if (!readIssueFile(parentNumber, parent)) {
        if (error)
            *error = QStringLiteral("Issue #%1 not found.").arg(parentNumber);
        return false;
    }
    const IssueEvent effectiveOpen = effectiveOpenEvent(parent);
    if (effectiveOpen.id.isEmpty()) {
        if (error)
            *error = QStringLiteral("Issue #%1 has no opening event.").arg(parentNumber);
        return false;
    }

    const QList<IssueSubtaskLine> tasks = issueSubtaskLines(effectiveOpen.body);
    if (tasks.isEmpty())
        return true;

    QStringList linkedLines = effectiveOpen.body.split(QLatin1Char('\n'),
                                                       Qt::KeepEmptyParts);
    QList<Issue> children;
    QList<int> numbers;
    int number = nextNumber();
    children.reserve(tasks.size());
    numbers.reserve(tasks.size());
    for (const IssueSubtaskLine &task : tasks) {
        IssueEvent opened;
        opened.type = QStringLiteral("open");
        opened.id = QStringLiteral("open-%1").arg(number);
        opened.title = task.title;
        opened.body = QStringLiteral("Parent issue: #%1").arg(parentNumber);
        opened = makeSignedEvent(number, opened);

        Issue child;
        child.number = number;
        child.title = task.title;
        child.status = QStringLiteral("open");
        child.createdAt = opened.ts;
        child.author = opened.author;
        child.authorName = opened.authorName;
        child.events.append(opened);
        children.append(child);
        numbers.append(number);

        linkedLines[task.line] =
            task.beforeState + QLatin1Char(' ') + task.afterState + task.label +
            QStringLiteral(" (sub-issue #%1)").arg(number) +
            task.carriageReturn;
        ++number;
    }

    IssueEvent parentEdit;
    parentEdit.type = QStringLiteral("edit");
    parentEdit.target = effectiveOpen.id;
    parentEdit.body = stripEdgeNewlines(linkedLines.join(QLatin1Char('\n')));
    parentEdit.attachments = effectiveOpen.attachments;
    parentEdit = makeSignedEvent(parentNumber, parentEdit);
    parent.events.append(parentEdit);

    QList<QString> writtenChildDirs;
    for (const Issue &child : std::as_const(children)) {
        if (!writeIssueFile(child, error)) {
            for (const QString &path : std::as_const(writtenChildDirs))
                forkmesh::safedelete::removeTree(path, issuesDir());
            return false;
        }
        writtenChildDirs.append(issueDir(child.number));
    }
    if (!writeIssueFile(parent, error)) {
        for (const QString &path : std::as_const(writtenChildDirs))
            forkmesh::safedelete::removeTree(path, issuesDir());
        return false;
    }
    if (!commit(QStringLiteral("issue #%1: create %2 sub-issue%3")
                    .arg(parentNumber)
                    .arg(numbers.size())
                    .arg(numbers.size() == 1 ? QString() : QStringLiteral("s")),
                error)) {
        return false;
    }
    if (createdNumbers)
        *createdNumbers = numbers;
    return true;
}

bool IssueStore::createSubIssue(int parentNumber, int taskLine,
                                int *createdNumber, QString *error)
{
    if (createdNumber)
        *createdNumber = 0;
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }

    Issue parent;
    if (!readIssueFile(parentNumber, parent)) {
        if (error)
            *error = QStringLiteral("Issue #%1 not found.").arg(parentNumber);
        return false;
    }
    const IssueEvent effectiveOpen = effectiveOpenEvent(parent);
    if (effectiveOpen.id.isEmpty()) {
        if (error)
            *error = QStringLiteral("Issue #%1 has no opening event.").arg(parentNumber);
        return false;
    }

    QStringList linkedLines = effectiveOpen.body.split(QLatin1Char('\n'),
                                                        Qt::KeepEmptyParts);
    IssueSubtaskLine task;
    if (taskLine < 0 || taskLine >= linkedLines.size() ||
        !issueChecklistLineAt(effectiveOpen.body, taskLine, &task)) {
        if (error)
            *error = QStringLiteral("That checklist row no longer exists.");
        return false;
    }
    if (task.subIssueNumber > 0) {
        if (error)
            *error = QStringLiteral("That checklist row is already attached to sub-issue #%1.")
                         .arg(task.subIssueNumber);
        return false;
    }

    const int number = nextNumber();
    IssueEvent opened;
    opened.type = QStringLiteral("open");
    opened.id = QStringLiteral("open-%1").arg(number);
    opened.title = task.title;
    opened.body = QStringLiteral("Parent issue: #%1").arg(parentNumber);
    opened = makeSignedEvent(number, opened);

    Issue child;
    child.number = number;
    child.title = task.title;
    child.status = QStringLiteral("open");
    child.createdAt = opened.ts;
    child.author = opened.author;
    child.authorName = opened.authorName;
    child.events.append(opened);

    QString linkedLine = linkedLines.at(taskLine);
    const bool hadCarriageReturn = linkedLine.endsWith(QLatin1Char('\r'));
    if (hadCarriageReturn)
        linkedLine.chop(1);
    linkedLine += QStringLiteral(" (sub-issue #%1)").arg(number);
    if (hadCarriageReturn)
        linkedLine += QLatin1Char('\r');
    linkedLines[taskLine] = linkedLine;

    IssueEvent parentEdit;
    parentEdit.type = QStringLiteral("edit");
    parentEdit.target = effectiveOpen.id;
    parentEdit.body = stripEdgeNewlines(linkedLines.join(QLatin1Char('\n')));
    parentEdit.attachments = effectiveOpen.attachments;
    parentEdit = makeSignedEvent(parentNumber, parentEdit);
    parent.events.append(parentEdit);

    if (!writeIssueFile(child, error))
        return false;
    if (!writeIssueFile(parent, error)) {
        forkmesh::safedelete::removeTree(issueDir(child.number), issuesDir());
        return false;
    }
    if (!commit(QStringLiteral("issue #%1: create sub-issue #%2")
                    .arg(parentNumber)
                    .arg(number),
                error)) {
        return false;
    }
    if (createdNumber)
        *createdNumber = number;
    return true;
}

bool IssueStore::addComment(int number, const QString &body,
                            const QStringList &attachmentSrcPaths, QString *error)
{
    return addComment(number, body, attachmentSrcPaths, {}, error);
}

bool IssueStore::addComment(int number, const QString &body,
                            const QStringList &attachmentSrcPaths,
                            const QStringList &attachmentPlaceholders, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    Issue issue;
    if (!readIssueFile(number, issue)) {
        if (error)
            *error = QStringLiteral("Issue #%1 not found.").arg(number);
        return false;
    }
    IssueEvent ev;
    ev.type = "comment";
    ev.attachments = copyAttachments(number, attachmentSrcPaths);
    if (!copiedEveryAttachment(attachmentSrcPaths, ev.attachments, error))
        return false;
    ev.body = stripEdgeNewlines(replaceAttachmentPlaceholders(
        body, attachmentSrcPaths, attachmentPlaceholders, ev.attachments));
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: comment").arg(number), error);
}

bool IssueStore::addVote(int number, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    Issue issue;
    if (!readIssueFile(number, issue)) {
        if (error)
            *error = QStringLiteral("Issue #%1 not found.").arg(number);
        return false;
    }
    IssueEvent ev;
    ev.type = "vote";
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: vote").arg(number), error);
}

QString IssueStore::authorKey() const
{
    return m_identity ? m_identity->publicKey() : QString();
}

namespace {

// Reaction events that have been taken back. Un-reacting appends a delete
// event naming the reaction, so the same slug can go on and off a comment any
// number of times and only the live ones count.
QSet<QString> retractedEventIds(const Issue &issue)
{
    QSet<QString> out;
    for (const IssueEvent &ev : issue.events)
        if (ev.type == QLatin1String("delete") && !ev.target.isEmpty() &&
            ev.target != QLatin1String("self"))
            out.insert(ev.target);
    return out;
}

} // namespace

QList<IssueReaction> IssueStore::reactionsForEvent(const Issue &issue,
                                                   const QString &targetEventId,
                                                   const QString &author,
                                                   const QString &authorName)
{
    QList<IssueReaction> out;
    if (targetEventId.isEmpty())
        return out;
    const QSet<QString> retracted = retractedEventIds(issue);
    QSet<QString> counted; // one live reaction per slug + signer + display name
    for (const IssueEvent &ev : issue.events) {
        if (ev.type != QLatin1String("reaction") ||
            ev.target != targetEventId || ev.reaction.isEmpty() ||
            retracted.contains(ev.id))
            continue;
        const QChar nul(QChar::Null);
        const QString key = ev.reaction + nul + ev.author + nul + ev.authorName;
        if (counted.contains(key))
            continue;
        counted.insert(key);
        int slot = -1;
        for (int i = 0; i < out.size(); ++i)
            if (out.at(i).value == ev.reaction)
                slot = i;
        if (slot < 0) {
            out.append(IssueReaction{ev.reaction, {}, false});
            slot = out.size() - 1;
        }
        out[slot].names.append(ev.authorName.trimmed().isEmpty()
                                   ? ev.author.left(10)
                                   : ev.authorName.trimmed());
        if (!author.isEmpty() && ev.author == author &&
            ev.authorName == authorName)
            out[slot].mine = true;
    }
    return out;
}

bool IssueStore::setReaction(int number, const QString &targetEventId,
                             const QString &reaction, bool on, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    if (targetEventId.isEmpty() || reaction.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("There is nothing to react to.");
        return false;
    }
    Issue issue;
    if (!readIssueFile(number, issue)) {
        if (error)
            *error = QStringLiteral("Issue #%1 not found.").arg(number);
        return false;
    }
    const QSet<QString> retracted = retractedEventIds(issue);
    const QString me = authorKey();
    QString existing;
    for (const IssueEvent &ev : issue.events) {
        if (ev.type != QLatin1String("reaction") ||
            ev.target != targetEventId || ev.reaction != reaction ||
            retracted.contains(ev.id))
            continue;
        if (ev.author != me || ev.authorName != m_authorName)
            continue;
        existing = ev.id;
    }
    if (on == !existing.isEmpty())
        return true; // already where the caller wants it; don't churn a commit
    IssueEvent ev;
    if (on) {
        ev.type = "reaction";
        ev.target = targetEventId;
        ev.reaction = reaction;
    } else {
        ev.type = "delete";
        ev.target = existing;
    }
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: %2 %3")
                      .arg(number)
                      .arg(on ? QStringLiteral("react") : QStringLiteral("unreact"),
                           reaction),
                  error);
}

bool IssueStore::toggleReaction(int number, const QString &targetEventId,
                                const QString &reaction, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    Issue issue;
    if (!readIssueFile(number, issue)) {
        if (error)
            *error = QStringLiteral("Issue #%1 not found.").arg(number);
        return false;
    }
    bool mine = false;
    for (const IssueReaction &entry :
         reactionsForEvent(issue, targetEventId, authorKey(), m_authorName))
        if (entry.value == reaction)
            mine = entry.mine;
    return setReaction(number, targetEventId, reaction, !mine, error);
}

bool IssueStore::editEvent(int number, const QString &eventId, const QString &newBody,
                           const QStringList &keepAttachments,
                           const QStringList &newAttachmentSrcPaths, QString *error)
{
    return editEvent(number, eventId, newBody, keepAttachments,
                     newAttachmentSrcPaths, {}, error);
}

bool IssueStore::editEvent(int number, const QString &eventId, const QString &newBody,
                           const QStringList &keepAttachments,
                           const QStringList &newAttachmentSrcPaths,
                           const QStringList &newAttachmentPlaceholders, QString *error)
{
    if (!canWrite())
        return false;
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "edit";
    ev.target = eventId;
    ev.attachments = keepAttachments;
    const QStringList copiedAttachments = copyAttachments(number, newAttachmentSrcPaths);
    if (!copiedEveryAttachment(newAttachmentSrcPaths, copiedAttachments, error))
        return false;
    ev.attachments += copiedAttachments;
    ev.body = stripEdgeNewlines(replaceAttachmentPlaceholders(
        newBody, newAttachmentSrcPaths, newAttachmentPlaceholders, copiedAttachments));
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: edit").arg(number), error);
}

bool IssueStore::setTitle(int number, const QString &newTitle, QString *error)
{
    if (!canWrite())
        return false;
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "title";
    ev.title = newTitle.trimmed();
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: retitle").arg(number), error);
}

bool IssueStore::setStatus(int number, const QString &status, QString *error)
{
    if (!canWrite())
        return false;
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "status";
    ev.status = status;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: %2").arg(number).arg(status), error);
}

bool IssueStore::setLabels(int number, const QStringList &labels, QString *error)
{
    if (!canWrite())
        return false;
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "labels";
    ev.labels = labels;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: labels").arg(number), error);
}

bool IssueStore::setMilestone(int number, const QString &milestone, QString *error)
{
    if (!canWrite())
        return false;
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "milestone";
    ev.milestone = milestone;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: milestone").arg(number), error);
}

bool IssueStore::setDates(int number, qint64 startDate, qint64 endDate,
                          QString *error)
{
    if (!canWrite())
        return false;
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "dates";
    ev.startDate = startDate;
    ev.endDate = endDate;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: dates").arg(number), error);
}

bool IssueStore::setPriority(int number, int priority, QString *error)
{
    if (!canWrite())
        return false;
    if (priority < 0 || priority > 8999) {
        if (error)
            *error = QStringLiteral(
                "Priority must be between 1 and 8999, or 0 to clear it.");
        return false;
    }
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "priority";
    ev.priority = priority;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: priority %2")
                      .arg(number)
                      .arg(priority == 0 ? QStringLiteral("cleared")
                                         : QString::number(priority)),
                  error);
}

bool IssueStore::setProgress(int number, int progress, QString *error)
{
    if (!canWrite())
        return false;
    if (progress < 0 || progress > 100) {
        if (error)
            *error = QStringLiteral("Progress must be between 0 and 100.");
        return false;
    }
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "progress";
    ev.progress = progress;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: progress %2%").arg(number).arg(progress),
                  error);
}

bool IssueStore::setBounty(int number, double amountUsd, const QString &address,
                           const QString &status, QString *error)
{
    if (!canWrite())
        return false;
    if (amountUsd < 0) {
        if (error)
            *error = QStringLiteral("Bounty amount must not be negative.");
        return false;
    }
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "bounty";
    ev.bountyUsd = amountUsd;
    ev.bountyAddress = address;
    ev.bountyStatus = status.isEmpty() ? QStringLiteral("open") : status;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: bounty $%2 (%3)")
                      .arg(number)
                      .arg(QString::number(amountUsd, 'f', 2), ev.bountyStatus),
                  error);
}

bool IssueStore::recordRewardPayment(int number, const QString &rewardId,
                                     const QString &chainSignature,
                                     QString *error,
                                     const RemoteIssueMeta *estimate)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral(
                "Reward receipts can only be recorded from a writable local checkout.");
        return false;
    }
    const QString reward = rewardId.trimmed();
    const QString signature = chainSignature.trimmed();
    Issue issue;
    if (!readIssueFile(number, issue)) {
        if (error)
            *error = QStringLiteral("Could not read issue #%1.").arg(number);
        return false;
    }
    // An issue with no reward whatsoever lost it in intake rather than
    // belonging to someone else's; take the relay's estimate when it credits
    // this issue's reporter, which a mis-numbered subject would not.
    const QString estimateRecipient =
        estimate ? estimate->rewardRecipient.trimmed() : QString();
    if (!reward.isEmpty() && issue.rewardId.trimmed().isEmpty() &&
        issue.rewardStatus.trimmed().isEmpty() &&
        issue.rewardTxSignature.trimmed().isEmpty() &&
        !estimateRecipient.isEmpty() && estimate->rewardEstimateLamports > 0 &&
        estimateRecipient.compare(issue.authorName.trimmed(),
                                  Qt::CaseInsensitive) == 0) {
        issue.rewardId = reward;
        issue.rewardRecipient = estimateRecipient.toLower();
        issue.rewardEstimateLamports = estimate->rewardEstimateLamports;
        issue.rewardCondition = estimate->rewardCondition.trimmed();
    }
    if (reward.isEmpty() || signature.isEmpty() ||
        issue.rewardId.trimmed() != reward) {
        if (error)
            *error = QStringLiteral("Issue #%1 does not carry reward %2.")
                         .arg(number)
                         .arg(rewardId);
        return false;
    }
    const QString recorded = issue.rewardTxSignature.trimmed();
    if (recorded == signature &&
        issue.rewardStatus.trimmed() == QLatin1String("paid"))
        return true; // already reconciled; nothing to write
    // A different signature already on the record means two transfers are
    // claiming the same estimate. Leave the first one standing rather than
    // silently rewriting which transfer the reader is told to verify.
    if (!recorded.isEmpty() && recorded != signature) {
        if (error)
            *error = QStringLiteral(
                         "Issue #%1 already records a different reward transaction.")
                         .arg(number);
        return false;
    }
    if (issue.rewardStatus.trimmed() == QLatin1String("cancelled")) {
        if (error)
            *error = QStringLiteral("Issue #%1's reward was cancelled.").arg(number);
        return false;
    }
    issue.rewardTxSignature = signature;
    issue.rewardStatus = QStringLiteral("paid");
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: reward paid").arg(number), error);
}

bool IssueStore::setAssignees(int number, const QStringList &assignees, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral(
                "Assignees can only be changed from a writable local checkout.");
        return false;
    }
    Issue issue;
    if (!readIssueFile(number, issue)) {
        if (error)
            *error = QStringLiteral("Could not read issue #%1.").arg(number);
        return false;
    }
    IssueEvent ev;
    ev.type = "assignees";
    ev.assignees = assignees;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: assignees").arg(number), error);
}

bool IssueStore::setReporter(int number, const QString &reporter, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral(
                "The reporter can only be changed from a writable local checkout.");
        return false;
    }
    const QString name = reporter.trimmed();
    if (name.isEmpty()) {
        if (error)
            *error = QStringLiteral("Name the person to credit for issue #%1.")
                         .arg(number);
        return false;
    }
    Issue issue;
    if (!readIssueFile(number, issue)) {
        if (error)
            *error = QStringLiteral("Could not read issue #%1.").arg(number);
        return false;
    }
    IssueEvent ev;
    ev.type = "reporter";
    ev.reporter = name;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    recomputeMetadata(issue);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: reporter").arg(number), error);
}

bool IssueStore::assignAgent(int number, const QString &provider, int sessionId,
                             bool createPr, const QString &status, QString *error)
{
    if (!canWrite())
        return false;
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "agent";
    ev.agentProvider = provider;
    ev.agentSessionId = sessionId;
    ev.agentCreatePr = createPr;
    ev.agentStatus = status;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    if (!writeIssueFile(issue, error))
        return false;
    const QString action = (sessionId <= 0 || status == QLatin1String("cleared"))
                               ? QStringLiteral("clear agent")
                               : QStringLiteral("assign agent");
    return commit(QStringLiteral("issue #%1: %2").arg(number).arg(action), error);
}

bool IssueStore::deleteEvent(int number, const QString &eventId, QString *error)
{
    if (!canWrite())
        return false;
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    IssueEvent ev;
    ev.type = "delete";
    ev.target = eventId;
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: delete comment").arg(number), error);
}

bool IssueStore::tombstoneIssue(int number, QString *error)
{
    if (!canWrite())
        return false;
    Issue issue;
    if (!readIssueFile(number, issue))
        return false;
    if (issue.isDeleted())
        return true; // already tombstoned; nothing to do
    IssueEvent ev;
    ev.type = "delete";
    ev.target = "self";
    ev = makeSignedEvent(number, ev);
    issue.events.append(ev);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(QStringLiteral("issue #%1: delete").arg(number), error);
}

// code-health: allow(dir-walk-in-loop) loops over the 3 candidate issue folders from issueDirRelCandidates()
bool IssueStore::removeIssue(int number, QString *error)
{
    if (!canWrite())
        return false;
    const QStringList relPaths = issueDirRelCandidates(number);
    if (hasUnrelatedTrackedChanges(m_workTree, relPaths, error))
        return false;

    QString err;
    if (!runGit(m_workTree,
                QStringList{"rm", "-r", "--ignore-unmatch", "--"} + relPaths,
                nullptr, &err)) {
        if (error)
            *error = QStringLiteral("Could not remove issue from git: ") + err;
        return false;
    }
    for (const QString &relPath : relPaths) {
        const QString path = QDir(m_workTree).filePath(relPath);
        if (QDir(path).exists() && !forkmesh::safedelete::removeTree(path, issuesDir())) {
            if (error)
                *error = QStringLiteral("Could not remove issue folder.");
            return false;
        }
    }
    if (!runGit(m_workTree,
                {"commit", "-m", QStringLiteral("issue #%1: delete").arg(number),
                 "--", issuesRootRel()},
                nullptr, &err)) {
        if (!err.contains(QStringLiteral("nothing to commit")) && !err.isEmpty()) {
            if (error)
                *error = QStringLiteral("git commit failed: ") + err;
            return false;
        }
    }
    return true;
}

bool IssueStore::deleteIssue(int number, QString *error)
{
    if (!canWrite())
        return false;
    const QStringList relPaths = issueDirRelCandidates(number);
    QString err;
    if (hasUnrelatedTrackedChanges(m_workTree, relPaths, error))
        return false;

    runGit(m_workTree,
           QStringList{"rm", "-r", "--ignore-unmatch", "--"} + relPaths, nullptr,
           nullptr);
    if (QDir(issueDir(number)).exists() &&
        !forkmesh::safedelete::removeTree(issueDir(number), issuesDir())) {
        if (error)
            *error = QStringLiteral("Could not remove issue folder.");
        return false;
    }
    if (!runGit(m_workTree,
                QStringList{"commit", "-m", QStringLiteral("delete issue"), "--",
                            issuesRootRel()},
                nullptr, &err)) {
        if (!err.contains(QStringLiteral("nothing to commit")) && !err.isEmpty()) {
            if (error)
                *error = QStringLiteral("git commit failed: ") + err;
            return false;
        }
    }

    QByteArray refs;
    if (!runGit(m_workTree, {"rev-list", "--all", "--max-count=1"}, &refs, &err,
                kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git rev-list failed: ") + err;
        return false;
    }
    if (refs.trimmed().isEmpty())
        return true;

    const QString indexFilter =
        QStringLiteral("git rm -r --cached --ignore-unmatch -- %1")
            .arg(relPaths.join(QLatin1Char(' ')));
    QByteArray out;
    if (!runGit(m_workTree,
                {"filter-branch", "--force", "--index-filter", indexFilter,
                 "--prune-empty", "--tag-name-filter", "cat", "--", "--all"},
                &out, &err, kGitRewriteTimeoutMs)) {
        const QString stdoutText = QString::fromUtf8(out);
        if (!err.contains(QStringLiteral("Not a valid object name HEAD")) &&
            !stdoutText.contains(QStringLiteral("was deleted"))) {
            if (error)
                *error = QStringLiteral("git history rewrite failed: ") + err;
            return false;
        }
    }

    if (!runGit(m_workTree, {"rev-parse", "--verify", "HEAD"}, nullptr, nullptr) &&
        !runGit(m_workTree, {"commit", "--allow-empty", "-m",
                             QStringLiteral("Initial commit")},
                nullptr, &err)) {
        if (error)
            *error = QStringLiteral("git commit failed: ") + err;
        return false;
    }
    if (!removeOriginalRefs(m_workTree, error))
        return false;
    if (!runGit(m_workTree, {"reflog", "expire", "--expire=now",
                             "--expire-unreachable=now", "--all"},
                nullptr, &err, kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git reflog expire failed: ") + err;
        return false;
    }
    if (!runGit(m_workTree, {"gc", "--prune=now"}, nullptr, &err,
                kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git gc failed: ") + err;
        return false;
    }
    return true;
}


bool IssueStore::saveLabels(const QList<IssueLabel> &labels, QString *error)
{
    if (!canWrite())
        return false;
    QJsonArray array;
    for (const IssueLabel &label : labels)
        array.append(QJsonObject{{"name", label.name}, {"color", label.color}});
    QDir().mkpath(issuesDir());
    if (!writeTextFile(issuesDir() + "/labels.json",
                       QString::fromUtf8(QJsonDocument(array).toJson()), error))
        return false;
    return commit(QStringLiteral("issues: update labels"), error);
}

bool IssueStore::saveMilestones(const QList<IssueMilestone> &milestones, QString *error)
{
    if (!canWrite())
        return false;
    QJsonArray array;
    for (const IssueMilestone &ms : milestones)
        array.append(QJsonObject{{"title", ms.title}, {"due", double(ms.due)},
                                 {"status", ms.status}, {"description", ms.description}});
    QDir().mkpath(issuesDir());
    if (!writeTextFile(issuesDir() + "/milestones.json",
                       QString::fromUtf8(QJsonDocument(array).toJson()), error))
        return false;
    return commit(QStringLiteral("issues: update milestones"), error);
}

// ---- Cross-user sync (merge an inbound signed event) -----------------------

static bool hasFabricatedOpening(const Issue &issue)
{
    if (issue.events.isEmpty())
        return false;
    const IssueEvent &opening = issue.events.first();
    if (opening.type != QLatin1String("open"))
        return false;
    if (opening.sig == QLatin1String("relay-placeholder"))
        return true;
    if (!opening.sig.isEmpty() || !opening.body.isEmpty() ||
        !opening.attachments.isEmpty() ||
        opening.id != QStringLiteral("open-%1").arg(issue.number))
        return false;
    return opening.title.isEmpty() ||
           (issue.events.size() > 1 && issue.events.at(1).ts == opening.ts);
}

bool IssueStore::applyRemoteEvent(int number, const IssueEvent &ev,
                                  const QString &, QString *error,
                                  const RemoteIssueMeta &meta,
                                  const QList<RemoteAttachment> &attachments)
{
    if (!canWrite())
        return false;

    // A brand-new issue ("open") authored elsewhere (e.g. filed from a mirror).
    // The proposed number is only a hint: the open event's id is number-derived
    // ("open-N"), so it isn't a stable identity. We dedup on the signature (which
    // binds content+number+author+ts) across ALL issues so a re-synced submission
    // never duplicates, and we always assign a free number so a collision can't
    // staple a second open event onto an existing issue.
    if (ev.type == "open") {
        const QList<Issue> all = loadAll();
        for (const Issue &i : all)
            for (const IssueEvent &e : i.events) {
                if (e.type != "open")
                    continue;
                const bool sameSig = !ev.sig.isEmpty() && e.sig == ev.sig;
                const bool sameAuthorTs = ev.sig.isEmpty() && e.author == ev.author &&
                                          e.ts == ev.ts && e.title == ev.title;
                if (sameSig || sameAuthorTs)
                    return true; // already merged this submission
            }

        Issue probe;
        const bool slotTaken = number > 0 && readIssueFile(number, probe);
        if (slotTaken && hasFabricatedOpening(probe)) {
            Issue issue = probe;
            issue.events.first() = ev;
            issue.title = ev.title;
            issue.createdAt = ev.ts;
            issue.author = ev.author;
            issue.authorName = meta.reporter.trimmed().isEmpty()
                                   ? ev.authorName
                                   : meta.reporter.trimmed();
            issue.labels = meta.labels;
            issue.milestone = meta.milestone;
            issue.priority = qBound(0, meta.priority, 8999);
            issue.assignees = meta.assignees;
            issue.rewardEstimateLamports = meta.rewardEstimateLamports;
            issue.rewardRecipient = meta.rewardRecipient;
            issue.rewardId = meta.rewardId;
            issue.rewardCondition = meta.rewardCondition;
            issue.rewardStatus = meta.rewardStatus;
            recomputeMetadata(issue);
            writeRemoteAttachmentFiles(number, ev.attachments, attachments);
            if (!writeIssueFile(issue, error))
                return false;
            return commit(QStringLiteral("issue #%1: opened (from %2)")
                              .arg(number)
                              .arg(ev.authorName.isEmpty() ? ev.author.left(8)
                                                           : ev.authorName),
                          error);
        }
        if (number <= 0 || slotTaken)
            number = nextNumber();

        Issue issue;
        issue.number = number;
        issue.title = ev.title;
        issue.status = "open";
        issue.createdAt = ev.ts;
        issue.author = ev.author;
        issue.authorName = meta.reporter.trimmed().isEmpty()
                               ? ev.authorName
                               : meta.reporter.trimmed();
        // Issue-level metadata isn't part of the open-event signature; apply it
        // as vouched data that rode along with the submission.
        issue.labels = meta.labels;
        issue.milestone = meta.milestone;
        issue.priority = qBound(0, meta.priority, 8999);
        issue.assignees = meta.assignees;
        issue.rewardEstimateLamports = meta.rewardEstimateLamports;
        issue.rewardRecipient = meta.rewardRecipient;
        issue.rewardId = meta.rewardId;
        issue.rewardCondition = meta.rewardCondition;
        issue.rewardStatus = meta.rewardStatus;
        issue.events.append(ev);
        recomputeMetadata(issue);
        writeRemoteAttachmentFiles(number, ev.attachments, attachments);
        if (!writeIssueFile(issue, error))
            return false;
        return commit(QStringLiteral("issue #%1: opened (from %2)")
                          .arg(number)
                          .arg(ev.authorName.isEmpty() ? ev.author.left(8)
                                                       : ev.authorName),
                      error);
    }

    // The web app's "Delete issue" (issue #582) arrives here as a signed
    // delete/self event. The relay only accepts one from a verified repository
    // manager, so this is the owner's authoritative removal rather than a
    // creator tombstone: take the issue out of the tree exactly as the
    // desktop's own Delete button does, so every mirror and reader converges on
    // its absence. Git history keeps the record.
    if (ev.type == "delete" && ev.target == "self" && number > 0) {
        Issue doomed;
        if (!readIssueFile(number, doomed))
            return true; // already removed; ack the row so it stops redelivering
        return removeIssue(number, error);
    }

    Issue issue;
    if (number <= 0 || !readIssueFile(number, issue)) {
        if (error)
            *error = QStringLiteral("issue #%1 has not been opened here yet; "
                                    "its %2 event waits for the opening event")
                         .arg(number)
                         .arg(ev.type);
        return false;
    }
    // Comment deletions (delete with a target other than "self") stay append-
    // only: the comment event is preserved and folded out at render time. Only
    // the whole-issue delete above removes anything from the tree.
    // Voters may cast several votes (one per credit), so we no longer collapse
    // to one-per-author. We still ignore an event we've already merged (same id)
    // so re-syncing the inbox doesn't double-count the same vote.
    if (!ev.id.isEmpty()) {
        for (const IssueEvent &existing : std::as_const(issue.events))
            if (existing.id == ev.id)
                return true;
    }
    issue.events.append(ev);
    recomputeMetadata(issue);
    writeRemoteAttachmentFiles(number, ev.attachments, attachments);
    if (!writeIssueFile(issue, error))
        return false;
    return commit(
        QStringLiteral("issue #%1: %2 (from %3)")
            .arg(number)
            .arg(ev.type, ev.authorName.isEmpty() ? ev.author.left(8) : ev.authorName),
        error);
}
