#include "PingFixes.h"

#include <QRegularExpression>

#include <algorithm>

namespace forkmesh::pingfixes {

namespace {

struct Pattern {
    Cause cause;
    const char *expression;
};

const QList<QPair<Cause, QRegularExpression>> &causePatterns()
{
    static const QList<QPair<Cause, QRegularExpression>> patterns = [] {
        const Pattern table[] = {
            {Cause::Network,
             "network is unreachable|could not resolve|temporary failure in name resolution|"
             "name or service not known|connection (refused|reset)|tls handshake|ssl_error|"
             "failed to connect|econnrefused|econnreset|enotfound"},
            {Cause::HostOffline,
             "not on the (local )?network|is it on\\?|host is down|no route to host|"
             "(host|machine|mac|runner|server) (is )?(unreachable|not reachable|offline|asleep)|"
             "is (offline|asleep|powered off)|did not answer|not responding"},
            {Cause::Disk,
             "no space left on device|disk quota|disk full|not enough free disk space"},
            {Cause::Timeout, "timed? ?out|deadline exceeded|took too long"},
            {Cause::Auth,
             "permission denied|authentication (failed|required)|unauthori[sz]ed|forbidden|"
             "\\b40[13]\\b|invalid credentials|bad credentials|access denied|token (expired|invalid)"},
            {Cause::MissingTool,
             "command not found|executable file not found|is not installed|no such command|"
             "not found in path|unknown command"},
            {Cause::Test,
             "tests? failed|failing tests?|assertion|assert(ion)?error|\\bfailed \\d+|"
             "\\d+ failed|expected .* (but|got)"},
            {Cause::Build,
             "error:|compil(e|ation) failed|build failed|undefined reference|cannot find symbol|"
             "syntaxerror|typeerror|nameerror|importerror|modulenotfounderror|exception"},
        };
        QList<QPair<Cause, QRegularExpression>> out;
        for (const Pattern &row : table)
            out.append({row.cause,
                        QRegularExpression(QString::fromLatin1(row.expression),
                                           QRegularExpression::CaseInsensitiveOption)});
        return out;
    }();
    return patterns;
}

QString causeLine(const QString &body)
{
    for (const QString &line : body.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1String("Cause:"), Qt::CaseInsensitive))
            return trimmed.mid(6).trimmed();
    }
    return {};
}

QString keyPart(const QString &text)
{
    QString out;
    for (const QChar c : text.trimmed().toLower()) {
        if (out.size() >= 120)
            break;
        if (c.isLetterOrNumber() || c == QLatin1Char('/') || c == QLatin1Char('.') ||
            c == QLatin1Char('-') || c == QLatin1Char('_'))
            out.append(c);
        else if (!out.endsWith(QLatin1Char('-')))
            out.append(QLatin1Char('-'));
    }
    return out;
}

double priorFor(Event event, Cause cause, const QString &id)
{
    if (event == Event::ActionFailed) {
        if (id == QLatin1String(fix::kDisableWorkflow)) {
            switch (cause) {
            case Cause::HostOffline: return 0.85;
            case Cause::MissingTool: return 0.55;
            case Cause::Auth: return 0.45;
            case Cause::Disk: return 0.4;
            default: return 0.3;
            }
        }
        if (id == QLatin1String(fix::kRerun)) {
            switch (cause) {
            case Cause::Network: return 0.75;
            case Cause::Timeout: return 0.7;
            case Cause::Unknown:
            case Cause::None: return 0.45;
            case Cause::HostOffline: return 0.35;
            case Cause::Build: return 0.2;
            default: return 0.3;
            }
        }
        if (id == QLatin1String(fix::kAgentFix)) {
            switch (cause) {
            case Cause::Build:
            case Cause::Test: return 0.8;
            case Cause::Unknown:
            case Cause::None: return 0.5;
            case Cause::MissingTool: return 0.45;
            case Cause::Auth: return 0.3;
            case Cause::HostOffline: return 0.15;
            default: return 0.25;
            }
        }
        if (id == QLatin1String(fix::kEnableWorkflow))
            return 0.3;
    }
    if (event == Event::ActionStopped) {
        if (id == QLatin1String(fix::kRerun))
            return 0.6;
        if (id == QLatin1String(fix::kDisableWorkflow))
            return 0.3;
        if (id == QLatin1String(fix::kEnableWorkflow))
            return 0.3;
    }
    if (event == Event::ActionApproval) {
        if (id == QLatin1String(fix::kApprove))
            return 0.6;
        if (id == QLatin1String(fix::kReject))
            return 0.4;
    }
    if (event == Event::WorkflowAutoDisabled && id == QLatin1String(fix::kEnableWorkflow))
        return 0.6;
    if (event == Event::AgentFailed && id == QLatin1String(fix::kAgentRetry))
        return 0.7;
    return 0.2;
}

Fix makeFix(const Context &context, Event event, Cause cause, const QString &id)
{
    Fix out;
    out.id = id;
    out.prior = priorFor(event, cause, id);
    out.score = out.prior;
    const QString workflow =
        context.workflowName.isEmpty() ? context.workflowPath : context.workflowName;
    const QString where = context.repo.isEmpty() ? QStringLiteral("this repository") : context.repo;
    if (id == QLatin1String(fix::kDisableWorkflow)) {
        out.label = QStringLiteral("Turn off this action");
        out.detail = QStringLiteral("Stops “%1” from running on %2 until you turn it back on.")
                         .arg(workflow, where);
        out.icon = QStringLiteral("pause");
        out.autoSafe = true;
    } else if (id == QLatin1String(fix::kEnableWorkflow)) {
        out.label = event == Event::WorkflowAutoDisabled ? QStringLiteral("Undo: turn it back on")
                                                         : QStringLiteral("Turn this action back on");
        out.detail = QStringLiteral("Lets “%1” run on %2 again.").arg(workflow, where);
        out.icon = QStringLiteral("play");
    } else if (id == QLatin1String(fix::kRerun)) {
        out.label = QStringLiteral("Run it again");
        out.detail = QStringLiteral("Runs “%1” again on the same commit.").arg(workflow);
        out.icon = QStringLiteral("rerun");
    } else if (id == QLatin1String(fix::kAgentFix)) {
        out.label = QStringLiteral("Ask an agent to fix it");
        out.detail = QStringLiteral("Starts an agent with the run's log and the ask to make it pass.");
        out.icon = QStringLiteral("agent");
    } else if (id == QLatin1String(fix::kApprove)) {
        out.label = QStringLiteral("Approve and run");
        out.detail = QStringLiteral("Lets this run of “%1” start.").arg(workflow);
        out.icon = QStringLiteral("approve");
    } else if (id == QLatin1String(fix::kReject)) {
        out.label = QStringLiteral("Reject this run");
        out.detail = QStringLiteral("This run of “%1” will not start.").arg(workflow);
        out.icon = QStringLiteral("reject");
    } else if (id == QLatin1String(fix::kAgentRetry)) {
        out.label = QStringLiteral("Tell the agent to try again");
        out.detail = QStringLiteral("Sends agent #%1 the failure and asks it to carry on.")
                         .arg(context.agentSession);
        out.icon = QStringLiteral("agent");
    }
    return out;
}

Context failedTwin(const Context &context)
{
    Context twin = context;
    twin.title = QStringLiteral("Action failed");
    return twin;
}

bool hasEvidence(const Stats &stats)
{
    return stats.offered > 0 || stats.chosen > 0 || stats.autoApplied > 0 || stats.ok > 0 ||
           stats.failed > 0 || stats.undone > 0;
}

void settleInPlace(Stats &stats, qint64 nowMs)
{
    if (stats.pendingSinceMs > 0 && nowMs - stats.pendingSinceMs >= kSettleMs) {
        ++stats.ok;
        ++stats.streak;
        stats.pendingSinceMs = 0;
    }
}

}

QString familyPrefix()
{
    return QStringLiteral("pings.");
}

QString ledgerFileName()
{
    return QStringLiteral("ping-fixes.json");
}

QString causeToken(Cause cause)
{
    switch (cause) {
    case Cause::None: return QStringLiteral("none");
    case Cause::HostOffline: return QStringLiteral("host-offline");
    case Cause::Network: return QStringLiteral("network");
    case Cause::Timeout: return QStringLiteral("timeout");
    case Cause::MissingTool: return QStringLiteral("missing-tool");
    case Cause::Auth: return QStringLiteral("auth");
    case Cause::Disk: return QStringLiteral("disk");
    case Cause::Test: return QStringLiteral("test");
    case Cause::Build: return QStringLiteral("build");
    case Cause::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

Cause causeOf(const QString &text)
{
    const QString cause = causeLine(text);
    const QString subject = cause.isEmpty() ? text : cause;
    if (subject.trimmed().isEmpty())
        return Cause::None;
    for (const auto &pattern : causePatterns()) {
        if (pattern.second.match(subject).hasMatch())
            return pattern.first;
    }
    return Cause::Unknown;
}

QString eventToken(Event event)
{
    switch (event) {
    case Event::Other: return QStringLiteral("other");
    case Event::ActionFailed: return QStringLiteral("action-failed");
    case Event::ActionStopped: return QStringLiteral("action-stopped");
    case Event::ActionApproval: return QStringLiteral("action-approval");
    case Event::WorkflowAutoDisabled: return QStringLiteral("action-auto-disabled");
    case Event::AgentFailed: return QStringLiteral("agent-failed");
    case Event::Warning: return QStringLiteral("warning");
    }
    return QStringLiteral("other");
}

Event eventOf(const Context &context)
{
    const QString title = context.title.trimmed().toLower();
    if (context.kind == QLatin1String("action") || context.runId > 0) {
        if (title.startsWith(QLatin1String("action turned off")))
            return Event::WorkflowAutoDisabled;
        if (context.runStatus == QLatin1String("awaiting-approval") ||
            title.contains(QLatin1String("approval")))
            return Event::ActionApproval;
        if (title == QLatin1String("action failed") || context.runStatus == QLatin1String("failed"))
            return Event::ActionFailed;
        if (title == QLatin1String("action stopped") ||
            context.runStatus == QLatin1String("cancelled"))
            return Event::ActionStopped;
    }
    if (context.agentSession > 0 && context.warning)
        return Event::AgentFailed;
    return context.warning ? Event::Warning : Event::Other;
}

QString signature(const Context &context)
{
    const QString subject = context.workflowPath.isEmpty() ? keyPart(context.workflowName)
                                                           : keyPart(context.workflowPath);
    return QStringList{eventToken(eventOf(context)),
                       causeToken(causeOf(context.body)),
                       keyPart(context.repo),
                       subject}
        .join(QLatin1Char('|'));
}

QString familySignature(const Context &context)
{
    return eventToken(eventOf(context)) + QLatin1Char('|') + causeToken(causeOf(context.body));
}

QList<Fix> candidates(const Context &context)
{
    const Event event = eventOf(context);
    const Cause cause = causeOf(context.body);
    const bool hasWorkflow = !context.repo.isEmpty() && !context.workflowPath.isEmpty();
    QStringList ids;
    switch (event) {
    case Event::ActionFailed:
    case Event::ActionStopped:
        if (hasWorkflow)
            ids << QString::fromLatin1(context.workflowDisabled ? fix::kEnableWorkflow
                                                                : fix::kDisableWorkflow);
        if (context.runId > 0)
            ids << QString::fromLatin1(fix::kRerun);
        if (event == Event::ActionFailed && context.runId > 0)
            ids << QString::fromLatin1(fix::kAgentFix);
        break;
    case Event::ActionApproval:
        if (context.runId > 0 && context.runStatus == QLatin1String("awaiting-approval"))
            ids << QString::fromLatin1(fix::kApprove) << QString::fromLatin1(fix::kReject);
        break;
    case Event::WorkflowAutoDisabled:
        if (hasWorkflow && context.workflowDisabled)
            ids << QString::fromLatin1(fix::kEnableWorkflow);
        break;
    case Event::AgentFailed:
        ids << QString::fromLatin1(fix::kAgentRetry);
        break;
    case Event::Warning:
    case Event::Other:
        break;
    }
    QList<Fix> out;
    for (const QString &id : std::as_const(ids))
        out.append(makeFix(context, event, cause, id));
    std::stable_sort(out.begin(), out.end(),
                     [](const Fix &a, const Fix &b) { return a.score > b.score; });
    return out;
}

bool deferredOutcome(const QString &fixId)
{
    return fixId == QLatin1String(fix::kRerun) || fixId == QLatin1String(fix::kAgentFix) ||
           fixId == QLatin1String(fix::kAgentRetry);
}

QString undoFor(const QString &fixId)
{
    if (fixId == QLatin1String(fix::kEnableWorkflow))
        return QString::fromLatin1(fix::kDisableWorkflow);
    return {};
}

QJsonObject fixJson(const Fix &fix, bool primary)
{
    return QJsonObject{
        {QStringLiteral("id"), fix.id},
        {QStringLiteral("label"), fix.label},
        {QStringLiteral("detail"), fix.detail},
        {QStringLiteral("icon"), fix.icon},
        {QStringLiteral("score"), qRound(fix.score * 1000) / 1000.0},
        {QStringLiteral("trusted"), fix.trusted},
        {QStringLiteral("autoSafe"), fix.autoSafe},
        {QStringLiteral("primary"), primary},
    };
}

double score(const Stats &stats, double prior)
{
    const double outcomes = stats.ok + stats.failed + 2.0 * stats.undone;
    const double success = (stats.ok + prior * kPriorWeight) / (outcomes + kPriorWeight);
    const double picks = stats.chosen + stats.autoApplied;
    const double preference =
        (picks + prior * kPriorWeight) / (std::max<double>(stats.offered, picks) + kPriorWeight);
    return 0.6 * success + 0.4 * preference;
}

void Ledger::load(const QJsonObject &json)
{
    m_stats.clear();
    const QJsonObject signatures = json.value(QStringLiteral("signatures")).toObject();
    for (auto it = signatures.begin(); it != signatures.end(); ++it) {
        const QJsonObject fixes = it.value().toObject();
        QHash<QString, Stats> &row = m_stats[it.key()];
        for (auto f = fixes.begin(); f != fixes.end(); ++f) {
            const QJsonObject o = f.value().toObject();
            Stats s;
            s.offered = o.value(QStringLiteral("offered")).toInt();
            s.chosen = o.value(QStringLiteral("chosen")).toInt();
            s.ok = o.value(QStringLiteral("ok")).toInt();
            s.failed = o.value(QStringLiteral("failed")).toInt();
            s.undone = o.value(QStringLiteral("undone")).toInt();
            s.streak = o.value(QStringLiteral("streak")).toInt();
            s.autoApplied = o.value(QStringLiteral("auto")).toInt();
            s.pendingSinceMs = qint64(o.value(QStringLiteral("pending")).toDouble());
            s.lastMs = qint64(o.value(QStringLiteral("last")).toDouble());
            row.insert(f.key(), s);
        }
    }
}

QJsonObject Ledger::toJson() const
{
    QJsonObject signatures;
    for (auto it = m_stats.begin(); it != m_stats.end(); ++it) {
        QJsonObject fixes;
        for (auto f = it.value().begin(); f != it.value().end(); ++f) {
            const Stats &s = f.value();
            fixes.insert(f.key(), QJsonObject{
                                      {QStringLiteral("offered"), s.offered},
                                      {QStringLiteral("chosen"), s.chosen},
                                      {QStringLiteral("ok"), s.ok},
                                      {QStringLiteral("failed"), s.failed},
                                      {QStringLiteral("undone"), s.undone},
                                      {QStringLiteral("streak"), s.streak},
                                      {QStringLiteral("auto"), s.autoApplied},
                                      {QStringLiteral("pending"), double(s.pendingSinceMs)},
                                      {QStringLiteral("last"), double(s.lastMs)},
                                  });
        }
        signatures.insert(it.key(), fixes);
    }
    return QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("signatures"), signatures}};
}

void Ledger::touch(const QString &sig, const QString &fixId,
                   const std::function<void(Stats &)> &change, qint64 nowMs)
{
    Stats &stats = m_stats[sig][fixId];
    settleInPlace(stats, nowMs);
    change(stats);
    stats.lastMs = nowMs;
}

void Ledger::offered(const Context &context, qint64 nowMs)
{
    const QList<Fix> fixes = candidates(context);
    for (const Fix &fix : fixes) {
        for (const QString &sig : {signature(context), familySignature(context)})
            touch(sig, fix.id, [](Stats &s) { ++s.offered; }, nowMs);
    }
    trim();
}

void Ledger::chosen(const Context &context, const QString &fixId, qint64 nowMs)
{
    const bool deferred = deferredOutcome(fixId);
    for (const QString &sig : {signature(context), familySignature(context)}) {
        touch(sig, fixId,
              [deferred, nowMs](Stats &s) {
                  ++s.chosen;
                  if (deferred) {
                      s.pendingSinceMs = nowMs;
                  } else {
                      ++s.ok;
                      ++s.streak;
                  }
              },
              nowMs);
    }
    const QString undoes = undoFor(fixId);
    if (!undoes.isEmpty())
        undone(eventOf(context) == Event::ActionStopped ? context : failedTwin(context), undoes,
               nowMs);
    trim();
}

void Ledger::outcome(const Context &context, const QString &fixId, bool ok, qint64 nowMs)
{
    for (const QString &sig : {signature(context), familySignature(context)}) {
        touch(sig, fixId,
              [ok](Stats &s) {
                  s.pendingSinceMs = 0;
                  if (ok) {
                      ++s.ok;
                      ++s.streak;
                  } else {
                      ++s.failed;
                      s.streak = 0;
                  }
              },
              nowMs);
    }
}

void Ledger::undone(const Context &context, const QString &fixId, qint64 nowMs)
{
    for (const QString &sig : {signature(context), familySignature(context)}) {
        auto row = m_stats.find(sig);
        if (row == m_stats.end() || !row->contains(fixId))
            continue;
        touch(sig, fixId,
              [](Stats &s) {
                  ++s.undone;
                  s.streak = 0;
                  s.pendingSinceMs = 0;
              },
              nowMs);
    }
}

void Ledger::autoApplied(const Context &context, const QString &fixId, qint64 nowMs)
{
    for (const QString &sig : {signature(context), familySignature(context)})
        touch(sig, fixId, [](Stats &s) { ++s.autoApplied; }, nowMs);
}

void Ledger::recurred(const Context &context, qint64 nowMs)
{
    for (const QString &sig : {signature(context), familySignature(context)}) {
        auto row = m_stats.find(sig);
        if (row == m_stats.end())
            continue;
        for (auto it = row->begin(); it != row->end(); ++it) {
            Stats &s = it.value();
            if (s.pendingSinceMs <= 0)
                continue;
            if (nowMs - s.pendingSinceMs < kSettleMs) {
                ++s.failed;
                s.streak = 0;
                s.pendingSinceMs = 0;
                s.lastMs = nowMs;
            } else {
                settleInPlace(s, nowMs);
            }
        }
    }
}

Stats Ledger::settled(const QString &sig, const QString &fixId, qint64 nowMs) const
{
    const auto row = m_stats.constFind(sig);
    if (row == m_stats.constEnd())
        return {};
    Stats stats = row->value(fixId);
    settleInPlace(stats, nowMs);
    return stats;
}

Stats Ledger::stats(const QString &sig, const QString &fixId) const
{
    return m_stats.value(sig).value(fixId);
}

QList<Fix> Ledger::ranked(const Context &context, qint64 nowMs) const
{
    QList<Fix> fixes = candidates(context);
    const QString sig = signature(context);
    const QString family = familySignature(context);
    for (Fix &fix : fixes) {
        const Stats own = settled(sig, fix.id, nowMs);
        const Stats shared = settled(family, fix.id, nowMs);
        double total = fix.prior * 0.5;
        double weight = 0.5;
        if (hasEvidence(own)) {
            total += score(own, fix.prior) * 2.0;
            weight += 2.0;
        }
        if (hasEvidence(shared)) {
            total += score(shared, fix.prior);
            weight += 1.0;
        }
        fix.score = total / weight;
        fix.trusted = fix.autoSafe && own.streak >= kTrustStreak;
        if (fix.trusted)
            fix.detail += QStringLiteral(" You picked this the last %1 times, so ForkMesh now does it "
                                         "on its own.")
                              .arg(own.streak);
    }
    std::stable_sort(fixes.begin(), fixes.end(), [](const Fix &a, const Fix &b) {
        if (a.trusted != b.trusted)
            return a.trusted;
        return a.score > b.score;
    });
    return fixes;
}

bool Ledger::autoFix(const Context &context, qint64 nowMs, Fix *out) const
{
    if (eventOf(context) != Event::ActionFailed)
        return false;
    const QList<Fix> fixes = ranked(context, nowMs);
    if (fixes.isEmpty() || !fixes.first().trusted)
        return false;
    if (out)
        *out = fixes.first();
    return true;
}

void Ledger::trim()
{
    if (m_stats.size() <= kMaxSignatures)
        return;
    QList<QPair<qint64, QString>> ages;
    for (auto it = m_stats.begin(); it != m_stats.end(); ++it) {
        qint64 last = 0;
        for (const Stats &s : it.value())
            last = std::max(last, s.lastMs);
        ages.append({last, it.key()});
    }
    std::sort(ages.begin(), ages.end());
    for (int i = 0; i < ages.size() && m_stats.size() > kMaxSignatures; ++i)
        m_stats.remove(ages.at(i).second);
}

}
