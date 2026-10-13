#include "LocalControlRepos.h"

#include <QRegularExpression>

#include <algorithm>

namespace forkmesh::localcontrol::repos {

namespace {

QString flatText(const QString &text, int max)
{
    QString out;
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

const QString kRunWorkflow = QStringLiteral("workflow.run");
const QString kApprove = QStringLiteral("run.approve");
const QString kReject = QStringLiteral("run.reject");
const QString kRerun = QStringLiteral("run.rerun");
const QString kStop = QStringLiteral("run.stop");
const QString kSkip = QStringLiteral("run.skip");
const QString kEnableWorkflow = QStringLiteral("workflow.enable");
const QString kDisableWorkflow = QStringLiteral("workflow.disable");

}

QString familyPrefix()
{
    return QStringLiteral("repos.");
}

int clampLimit(const QJsonValue &value, int fallback, int max)
{
    const int wanted = value.isDouble() ? int(value.toDouble()) : fallback;
    return std::clamp(wanted, 1, max);
}

bool parseRepoSlug(const QString &slug, QString *owner, QString *name)
{
    static const QRegularExpression pattern(
        QStringLiteral("^([A-Za-z0-9_.-]{1,100})/([A-Za-z0-9_.-]{1,100})$"));
    const QRegularExpressionMatch match = pattern.match(slug.trimmed());
    if (!match.hasMatch())
        return false;
    const QString o = match.captured(1);
    const QString n = match.captured(2);
    if (o == QLatin1String(".") || o == QLatin1String("..") || n == QLatin1String(".") ||
        n == QLatin1String(".."))
        return false;
    if (owner)
        *owner = o;
    if (name)
        *name = n;
    return true;
}

bool validWorkflowPath(const QString &path)
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\.forkmesh/[A-Za-z0-9_.-]{1,120}\\.ya?ml$"));
    return pattern.match(path).hasMatch() && !path.contains(QLatin1String(".."));
}

bool validBranchName(const QString &branch)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_./-]{1,200}$"));
    return pattern.match(branch).hasMatch() && !branch.startsWith(QLatin1Char('-')) &&
           !branch.startsWith(QLatin1Char('/')) && !branch.endsWith(QLatin1Char('/')) &&
           !branch.contains(QLatin1String("..")) && !branch.contains(QLatin1String("//")) &&
           !branch.endsWith(QLatin1String(".lock"));
}

QString branchFromRef(const QString &ref)
{
    if (ref.startsWith(QLatin1String("refs/heads/")))
        return ref.mid(11);
    if (ref.startsWith(QLatin1String("refs/tags/")))
        return ref.mid(10);
    return ref;
}

QString commitLogFormat()
{
    return QStringLiteral("--format=%H%x1f%an%x1f%at%x1f%s%x1e");
}

QJsonArray parseCommits(const QByteArray &output, int max)
{
    QJsonArray commits;
    const QList<QByteArray> records = output.split('\x1e');
    for (const QByteArray &raw : records) {
        if (commits.size() >= max)
            break;
        const QString record = QString::fromUtf8(raw).trimmed();
        if (record.isEmpty())
            continue;
        const QStringList fields = record.split(QChar(0x1f));
        if (fields.size() < 4 || fields.at(0).size() < 7)
            continue;
        commits.append(QJsonObject{
            {QStringLiteral("sha"), fields.at(0)},
            {QStringLiteral("author"), flatText(fields.at(1), 120)},
            {QStringLiteral("atMs"), double(fields.at(2).toLongLong() * 1000)},
            {QStringLiteral("subject"), flatText(fields.mid(3).join(QChar(0x1f)), kMaxSubjectChars)},
        });
    }
    return commits;
}

QString branchRefFormat()
{
    return QStringLiteral("--format=%(HEAD)%1f%(refname:short)%1f%(objectname)%1f"
                          "%(committerdate:unix)%1f%(upstream:short)%1f%(upstream:track)%1f"
                          "%(contents:subject)");
}

void parseTrack(const QString &track, int *ahead, int *behind, bool *gone)
{
    static const QRegularExpression aheadPattern(QStringLiteral("ahead (\\d+)"));
    static const QRegularExpression behindPattern(QStringLiteral("behind (\\d+)"));
    const QRegularExpressionMatch a = aheadPattern.match(track);
    const QRegularExpressionMatch b = behindPattern.match(track);
    if (ahead)
        *ahead = a.hasMatch() ? a.captured(1).toInt() : 0;
    if (behind)
        *behind = b.hasMatch() ? b.captured(1).toInt() : 0;
    if (gone)
        *gone = track.contains(QLatin1String("gone"));
}

QJsonArray parseBranches(const QByteArray &output, int max)
{
    QJsonArray branches;
    const QStringList lines = QString::fromUtf8(output).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (branches.size() >= max)
            break;
        const QStringList fields = line.split(QChar(0x1f));
        if (fields.size() < 7 || fields.at(1).trimmed().isEmpty())
            continue;
        int ahead = 0;
        int behind = 0;
        bool gone = false;
        parseTrack(fields.at(5), &ahead, &behind, &gone);
        branches.append(QJsonObject{
            {QStringLiteral("name"), fields.at(1).trimmed()},
            {QStringLiteral("current"), fields.at(0).trimmed() == QLatin1String("*")},
            {QStringLiteral("sha"), fields.at(2).trimmed()},
            {QStringLiteral("atMs"), double(fields.at(3).trimmed().toLongLong() * 1000)},
            {QStringLiteral("upstream"), fields.at(4).trimmed()},
            {QStringLiteral("ahead"), ahead},
            {QStringLiteral("behind"), behind},
            {QStringLiteral("upstreamGone"), gone},
            {QStringLiteral("subject"),
             flatText(fields.mid(6).join(QChar(0x1f)), kMaxSubjectChars)},
        });
    }
    return branches;
}

QJsonObject runRow(const RunFacts &run)
{
    QJsonArray steps;
    for (int i = 0; i < run.stepNames.size(); ++i)
        steps.append(QJsonObject{
            {QStringLiteral("name"), flatText(run.stepNames.at(i), 160)},
            {QStringLiteral("status"),
             i < run.stepStatuses.size() ? run.stepStatuses.at(i) : QStringLiteral("pending")},
        });
    QJsonObject row{
        {QStringLiteral("id"), run.id},
        {QStringLiteral("workflow"), flatText(run.workflowName, 160)},
        {QStringLiteral("path"), run.workflowPath},
        {QStringLiteral("status"), run.status},
        {QStringLiteral("commit"), run.commit},
        {QStringLiteral("branch"), branchFromRef(run.ref)},
        {QStringLiteral("createdAtMs"), double(run.createdAtMs)},
        {QStringLiteral("startedAtMs"), double(run.startedAtMs)},
        {QStringLiteral("finishedAtMs"), double(run.finishedAtMs)},
        {QStringLiteral("currentStep"), run.currentStepIndex},
        {QStringLiteral("steps"), steps},
    };
    if (run.pullNumber > 0) {
        row.insert(QStringLiteral("pull"), run.pullNumber);
        row.insert(QStringLiteral("pullAuthor"), flatText(run.pullAuthor, 120));
    }
    return row;
}

QJsonObject workflowRow(const WorkflowFacts &workflow)
{
    QJsonObject row{
        {QStringLiteral("path"), workflow.path},
        {QStringLiteral("name"), flatText(workflow.name.isEmpty() ? workflow.path : workflow.name, 160)},
        {QStringLiteral("valid"), workflow.valid},
        {QStringLiteral("triggers"), QJsonArray::fromStringList(workflow.triggers)},
        {QStringLiteral("manual"), workflow.manual},
        {QStringLiteral("disabled"), workflow.disabled},
        {QStringLiteral("here"), workflow.here},
    };
    if (!workflow.error.isEmpty())
        row.insert(QStringLiteral("error"), flatText(workflow.error, 300));
    if (!workflow.runsOn.isEmpty())
        row.insert(QStringLiteral("runsOn"), workflow.runsOn);
    return row;
}

QJsonObject actionsSummary(const QList<RunFacts> &runs)
{
    int running = 0;
    int queued = 0;
    int awaiting = 0;
    const RunFacts *latest = nullptr;
    for (const RunFacts &run : runs) {
        if (run.status == QLatin1String("running"))
            ++running;
        else if (run.status == QLatin1String("queued"))
            ++queued;
        else if (run.status == QLatin1String("awaiting-approval"))
            ++awaiting;
        if (!latest || run.createdAtMs > latest->createdAtMs)
            latest = &run;
    }
    QJsonObject summary{
        {QStringLiteral("running"), running},
        {QStringLiteral("queued"), queued},
        {QStringLiteral("awaiting"), awaiting},
    };
    if (latest) {
        summary.insert(QStringLiteral("latestStatus"), latest->status);
        summary.insert(QStringLiteral("latestWorkflow"), flatText(latest->workflowName, 160));
        summary.insert(QStringLiteral("latestAtMs"), double(latest->createdAtMs));
    }
    return summary;
}

LogSlice logSlice(const QString &log, qint64 since, qint64 maxChars)
{
    LogSlice slice;
    const qint64 total = log.size();
    if (since >= 0 && since <= total) {
        slice.from = since;
        if (total - slice.from > maxChars)
            slice.from = total - maxChars;
    } else {
        slice.reset = since >= 0;
        slice.from = total > maxChars ? total - maxChars : 0;
    }
    if (slice.from > 0 && slice.from < total && log.at(int(slice.from)).isLowSurrogate())
        ++slice.from;
    slice.text = log.mid(int(slice.from));
    slice.next = total;
    return slice;
}

QStringList actions()
{
    return {kRunWorkflow, kApprove, kReject, kRerun, kStop, kSkip, kEnableWorkflow, kDisableWorkflow};
}

bool actionTargetsRun(const QString &action)
{
    return action != kRunWorkflow && !actionSwitchesWorkflow(action) && actions().contains(action);
}

bool actionSwitchesWorkflow(const QString &action)
{
    return action == kEnableWorkflow || action == kDisableWorkflow;
}

bool runStatusAllows(const QString &action, const QString &status)
{
    if (action == kApprove || action == kReject)
        return status == QLatin1String("awaiting-approval");
    if (action == kStop)
        return status == QLatin1String("running") || status == QLatin1String("queued");
    if (action == kSkip)
        return status == QLatin1String("queued") || status == QLatin1String("awaiting-approval");
    if (action == kRerun)
        return status == QLatin1String("success") || status == QLatin1String("failed") ||
               status == QLatin1String("cancelled") || status == QLatin1String("rejected") ||
               status == QLatin1String("skipped");
    return false;
}

QString actionLabel(const QString &action)
{
    if (action == kRunWorkflow)
        return QStringLiteral("Run workflow");
    if (action == kApprove)
        return QStringLiteral("Approve run");
    if (action == kReject)
        return QStringLiteral("Reject run");
    if (action == kRerun)
        return QStringLiteral("Rerun");
    if (action == kStop)
        return QStringLiteral("Stop run");
    if (action == kSkip)
        return QStringLiteral("Skip run");
    if (action == kEnableWorkflow)
        return QStringLiteral("Turn on workflow");
    if (action == kDisableWorkflow)
        return QStringLiteral("Turn off workflow");
    return action;
}

QString intentSubject(const Intent &intent)
{
    if (intent.action == kRunWorkflow)
        return intent.repo + QLatin1Char(':') + intent.path + QLatin1Char('@') + intent.branch;
    if (actionSwitchesWorkflow(intent.action))
        return intent.repo + QLatin1Char(':') + intent.path;
    return intent.repo + QLatin1Char('#') + QString::number(intent.run);
}

QString actionSummary(const Intent &intent, const QString &workflowName)
{
    const QString name = flatText(workflowName, 160);
    const QString quoted = QStringLiteral("\"%1\"").arg(name.isEmpty() ? intent.path : name);
    if (intent.action == kRunWorkflow)
        return QStringLiteral("Run %1 on %2 of %3. It runs on this desktop in the Actions "
                              "sandbox; a workflow that is not approved yet waits for approval.")
            .arg(name.isEmpty() ? intent.path
                                : QStringLiteral("%1 (%2)").arg(quoted, intent.path),
                 intent.branch, intent.repo);
    if (intent.action == kEnableWorkflow)
        return QStringLiteral("Turn on %1 in %2. Its push, pull request and release triggers "
                              "start runs again.")
            .arg(quoted, intent.repo);
    if (intent.action == kDisableWorkflow)
        return QStringLiteral("Turn off %1 in %2. It does not run until it is turned on again.")
            .arg(quoted, intent.repo);
    const QString run = QStringLiteral("run #%1, %2 of %3").arg(intent.run).arg(quoted, intent.repo);
    if (intent.action == kApprove)
        return QStringLiteral("Approve %1. The desktop checks the repository has not changed, "
                              "then runs it.")
            .arg(run);
    if (intent.action == kReject)
        return QStringLiteral("Reject %1. It will not run.").arg(run);
    if (intent.action == kRerun)
        return QStringLiteral("Run %1 of %2 again on the same commit as run #%3.")
            .arg(quoted, intent.repo)
            .arg(intent.run);
    if (intent.action == kStop)
        return QStringLiteral("Stop %1.").arg(run);
    if (intent.action == kSkip)
        return QStringLiteral("Skip %1. It will not run.").arg(run);
    return {};
}

ActionConfirmations::Issued ActionConfirmations::issue(const QString &holder,
                                                       const Intent &intent, qint64 nowMs)
{
    for (auto it = m_intents.begin(); it != m_intents.end();) {
        if (it->expiresAtMs <= nowMs)
            it = m_intents.erase(it);
        else
            ++it;
    }
    while (m_intents.size() >= hosts::ConfirmationStore::kMaxOutstanding) {
        auto oldest = std::min_element(m_intents.begin(), m_intents.end(),
                                       [](const Entry &a, const Entry &b) {
                                           return a.expiresAtMs < b.expiresAtMs;
                                       });
        m_intents.erase(oldest);
    }
    Issued issued;
    issued.token = m_store.issue(holder, intent.action, intentSubject(intent), nowMs);
    issued.expiresAtMs = nowMs + hosts::ConfirmationStore::kLifetimeMs;
    m_intents.insert(issued.token, Entry{intent, issued.expiresAtMs});
    return issued;
}

hosts::ConfirmationStore::Result ActionConfirmations::consume(const QString &token,
                                                              const QString &holder,
                                                              qint64 nowMs, Intent *intent)
{
    const Entry entry = m_intents.take(token);
    if (token.isEmpty() || entry.intent.action.isEmpty()) {
        m_store.consume(token, holder, QString(), QString(), nowMs);
        return hosts::ConfirmationStore::Result::Missing;
    }
    const auto result =
        m_store.consume(token, holder, entry.intent.action, intentSubject(entry.intent), nowMs);
    if (result == hosts::ConfirmationStore::Result::Ok && intent)
        *intent = entry.intent;
    return result;
}

} // namespace forkmesh::localcontrol::repos
