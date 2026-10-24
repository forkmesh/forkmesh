#include "AppPaths.h"
#include "BackgroundFileWriter.h"
#include "DesktopPingForward.h"
#include "LocalControl.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "PingFixes.h"

#include <QAction>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>

namespace pingfixes = forkmesh::pingfixes;
using namespace forkmesh::ui;

namespace {

struct PendingRunFix {
    pingfixes::Context context;
    QString fixId;
};

class PingFixState : public QObject
{
public:
    explicit PingFixState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshPingFixState"));
        QFile file(path());
        if (file.open(QIODevice::ReadOnly))
            ledger.load(QJsonDocument::fromJson(file.readAll()).object());
    }

    static QString path()
    {
        return forkmesh::paths::dataRoot() + QLatin1Char('/') + pingfixes::ledgerFileName();
    }

    void save() const
    {
        forkmesh::diskwrite::writeLater(
            path(), QJsonDocument(ledger.toJson()).toJson(QJsonDocument::Compact));
    }

    pingfixes::Ledger ledger;
    QHash<int, PendingRunFix> runs;
};

PingFixState *pingFixState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(QStringLiteral("forkmeshPingFixState"),
                                                  Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<PingFixState *>(found))
        return state;
    return new PingFixState(window);
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

bool parseHeading(const QString &body, QString *workflow, QString *repo)
{
    const QString first = body.section(QLatin1Char('\n'), 0, 0);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const int at = first.lastIndexOf(dot);
    if (at <= 0)
        return false;
    *workflow = first.left(at).trimmed();
    *repo = first.mid(at + dot.size()).trimmed();
    return repo->count(QLatin1Char('/')) == 1;
}

QString causeLineOf(const QString &body)
{
    for (const QString &line : body.split(QLatin1Char('\n'))) {
        if (line.trimmed().startsWith(QLatin1String("Cause:")))
            return line.trimmed();
    }
    return {};
}

bool sameText(const QString &sent, const QString &ours, int max, bool singleLine)
{
    return forkmesh::pingforward::fitText(ours, max, singleLine).trimmed() == sent.trimmed();
}

}

pingfixes::Context MainWindow::pingFixContext(const AppNotification &item)
{
    pingfixes::Context context;
    context.kind = item.kind;
    context.title = item.title;
    context.body = item.body;
    context.warning = item.warning;
    context.runId = item.runId;
    if (item.link.kind == QLatin1String("agent") && item.link.number > 0)
        context.agentSession = item.link.number;
    QString owner;
    QString name;
    if (const ActionRun *run = item.runId > 0 ? findRun(item.runId) : nullptr) {
        owner = run->owner;
        name = run->name;
        context.workflowPath = run->workflowPath;
        context.workflowName = run->workflowName;
        context.runStatus = run->status;
    } else if (item.kind == QLatin1String("action")) {
        QString workflow;
        QString repo;
        if (parseHeading(item.body, &workflow, &repo)) {
            owner = repo.section(QLatin1Char('/'), 0, 0);
            name = repo.section(QLatin1Char('/'), 1, 1);
            context.workflowName = workflow;
            for (const ActionRun &run : std::as_const(m_actionRuns)) {
                if (run.owner == owner && run.name == name && run.workflowName == workflow) {
                    context.workflowPath = run.workflowPath;
                    break;
                }
            }
        }
        context.runId = -1;
    }
    if (!owner.isEmpty() && !name.isEmpty()) {
        context.repo = owner + QLatin1Char('/') + name;
        const int index = repoIndexFor(owner, name);
        if (index >= 0 && !context.workflowPath.isEmpty())
            context.workflowDisabled =
                m_repositories.at(index).disabledWorkflows.contains(context.workflowPath);
        else if (index < 0)
            context.workflowPath.clear();
    }
    return context;
}

const MainWindow::AppNotification *MainWindow::findPingForFix(const QJsonObject &params) const
{
    const qint64 localId = qint64(params.value(QStringLiteral("localId")).toDouble());
    if (localId > 0) {
        for (const AppNotification &item : m_notifications) {
            if (item.id == localId)
                return &item;
        }
    }
    const QString pingId = params.value(QStringLiteral("pingId")).toString();
    const int dash = pingId.indexOf(QLatin1Char('-'));
    if (dash > 0) {
        bool okTs = false;
        bool okId = false;
        const qint64 ts = pingId.left(dash).toLongLong(&okTs);
        const qint64 id = pingId.mid(dash + 1).toLongLong(&okId);
        const AppNotification *sameTime = nullptr;
        for (const AppNotification &item : m_notifications) {
            if (!okTs || item.timestampMs != ts)
                continue;
            if (okId && item.id == id)
                return &item;
            if (!sameTime)
                sameTime = &item;
        }
        if (sameTime)
            return sameTime;
    }
    const QString title = params.value(QStringLiteral("title")).toString();
    const QString body = params.value(QStringLiteral("body")).toString();
    if (title.trimmed().isEmpty() && body.trimmed().isEmpty())
        return nullptr;
    const qint64 ts = qint64(params.value(QStringLiteral("ts")).toDouble());
    constexpr qint64 kWindowMs = 15 * 60 * 1000;
    const AppNotification *best = nullptr;
    qint64 bestGap = 0;
    for (const AppNotification &item : m_notifications) {
        namespace pf = forkmesh::pingforward;
        const bool titleMatches = sameText(title, item.title, pf::kTitleMax, true) ||
                                  (title.isEmpty() && sameText(body, item.title, pf::kTitleMax, true));
        const bool bodyMatches = body.isEmpty() || sameText(body, item.body, pf::kBodyMax, false);
        if (!titleMatches || !bodyMatches)
            continue;
        const qint64 gap = ts > 0 ? qAbs(item.timestampMs - ts) : 0;
        if (ts > 0 && gap > kWindowMs)
            continue;
        if (!best || gap < bestGap) {
            best = &item;
            bestGap = gap;
        }
    }
    return best;
}

QJsonObject MainWindow::pingFixesReply(const AppNotification &item)
{
    const pingfixes::Context context = pingFixContext(item);
    const QList<pingfixes::Fix> fixes = pingFixState(this)->ledger.ranked(context, nowMs());
    QJsonArray rows;
    for (int i = 0; i < fixes.size(); ++i)
        rows.append(pingfixes::fixJson(fixes.at(i), i == 0));
    return QJsonObject{
        {QStringLiteral("found"), true},
        {QStringLiteral("localId"), double(item.id)},
        {QStringLiteral("event"), pingfixes::eventToken(pingfixes::eventOf(context))},
        {QStringLiteral("cause"), pingfixes::causeToken(pingfixes::causeOf(context.body))},
        {QStringLiteral("signature"), pingfixes::signature(context)},
        {QStringLiteral("fixes"), rows},
    };
}

bool MainWindow::applyPingFix(const AppNotification &item, const QString &fixId, bool automatic,
                              QString *message, QString *error)
{
    const pingfixes::Context context = pingFixContext(item);
    bool offered = false;
    for (const pingfixes::Fix &fix : pingfixes::candidates(context))
        offered = offered || fix.id == fixId;
    if (!offered) {
        *error = QStringLiteral("fix_unavailable");
        return false;
    }
    const QString workflow =
        context.workflowName.isEmpty() ? context.workflowPath : context.workflowName;
    if (fixId == QLatin1String(pingfixes::fix::kDisableWorkflow) ||
        fixId == QLatin1String(pingfixes::fix::kEnableWorkflow)) {
        const int index = repoIndexFor(context.repo.section(QLatin1Char('/'), 0, 0),
                                       context.repo.section(QLatin1Char('/'), 1, 1));
        if (index < 0) {
            *error = QStringLiteral("unknown_repo");
            return false;
        }
        const bool disable = fixId == QLatin1String(pingfixes::fix::kDisableWorkflow);
        setRepoWorkflowDisabled(index, context.workflowPath, disable);
        if (index == m_repoDetailIndex)
            refreshWorkflowProgress();
        *message = disable ? QStringLiteral("Turned off “%1” on %2.").arg(workflow, context.repo)
                           : QStringLiteral("Turned “%1” back on for %2.").arg(workflow, context.repo);
    } else if (fixId == QLatin1String(pingfixes::fix::kRerun)) {
        const int created = rerunActionRun(context.runId);
        if (created <= 0) {
            *error = QStringLiteral("not_started");
            return false;
        }
        pingFixState(this)->runs.insert(created, {context, fixId});
        *message = QStringLiteral("Started run #%1 of “%2”.").arg(created).arg(workflow);
    } else if (fixId == QLatin1String(pingfixes::fix::kAgentFix)) {
        if (!findRun(context.runId)) {
            *error = QStringLiteral("unknown_run");
            return false;
        }
        sendActionRunToAgent(context.runId, defaultAgentProvider(), QString());
        *message = QStringLiteral("Started an agent on “%1”'s failure.").arg(workflow);
    } else if (fixId == QLatin1String(pingfixes::fix::kApprove)) {
        QString why;
        if (!approveActionRun(context.runId, &why)) {
            *error = why.isEmpty() ? QStringLiteral("not_started") : why;
            return false;
        }
        m_actionRuns = m_actionStore->loadAllRuns();
        refreshActionsTable();
        updateNotificationButton();
        refreshOpenPullChecks();
        processActionQueue();
        *message = QStringLiteral("Approved run #%1.").arg(context.runId);
    } else if (fixId == QLatin1String(pingfixes::fix::kReject)) {
        if (!rejectActionRun(context.runId)) {
            *error = QStringLiteral("wrong_state");
            return false;
        }
        *message = QStringLiteral("Rejected run #%1.").arg(context.runId);
    } else if (fixId == QLatin1String(pingfixes::fix::kAgentRetry)) {
        if (!findAgentSession(context.agentSession)) {
            *error = QStringLiteral("unknown_agent");
            return false;
        }
        const QString detail = item.body.trimmed().isEmpty() ? item.title : item.body;
        sendPromptToAgentSession(
            context.agentSession,
            QStringLiteral("Your last turn ended with this failure:\n\n%1\n\nWork out what went "
                           "wrong and carry on until the task is done.")
                .arg(detail));
        *message = QStringLiteral("Asked agent #%1 to try again.").arg(context.agentSession);
    } else {
        *error = QStringLiteral("fix_unavailable");
        return false;
    }
    PingFixState *state = pingFixState(this);
    if (automatic)
        state->ledger.autoApplied(context, fixId, nowMs());
    else
        state->ledger.chosen(context, fixId, nowMs());
    state->save();
    logSystem(QStringLiteral("Pings: %1 quick fix %2: %3")
                  .arg(automatic ? QStringLiteral("applied") : QStringLiteral("chose"), fixId,
                       *message));
    return true;
}

void MainWindow::notePingForFixes(const AppNotification &item)
{
    if (item.kind != QLatin1String("action") && item.link.kind != QLatin1String("agent"))
        return;
    const pingfixes::Context context = pingFixContext(item);
    if (pingfixes::candidates(context).isEmpty())
        return;
    PingFixState *state = pingFixState(this);
    const qint64 now = nowMs();
    if (pingfixes::eventOf(context) == pingfixes::Event::ActionFailed)
        state->ledger.recurred(context, now);
    state->ledger.offered(context, now);
    pingfixes::Fix automatic;
    const bool trusted = state->ledger.autoFix(context, now, &automatic);
    state->save();
    if (!trusted)
        return;
    QString message;
    QString error;
    if (!applyPingFix(item, automatic.id, true, &message, &error)) {
        logSystem(QStringLiteral("Pings: the trusted quick fix %1 did not apply: %2.")
                      .arg(automatic.id, error));
        return;
    }
    const int streak =
        state->ledger.stats(pingfixes::signature(context), automatic.id).streak;
    AppNotification done;
    done.title = QStringLiteral("Action turned off automatically");
    QStringList lines{
        QString::fromUtf8("%1 \xC2\xB7 %2").arg(context.workflowName, context.repo),
        QStringLiteral("It failed this way before and you turned it off the last %1 times, so "
                       "ForkMesh did it for you. Undo turns it back on.")
            .arg(streak)};
    const QString cause = causeLineOf(item.body);
    if (!cause.isEmpty())
        lines << cause;
    done.body = lines.join(QLatin1Char('\n'));
    done.runId = item.runId;
    done.kind = QStringLiteral("action");
    done.repo = context.repo;
    recordNotification(done);
}

void MainWindow::notePingFixRunFinished(const ActionRun &run)
{
    PingFixState *state = pingFixState(this);
    const auto it = state->runs.constFind(run.id);
    if (it == state->runs.constEnd())
        return;
    const PendingRunFix pending = it.value();
    state->runs.remove(run.id);
    if (run.status == ActionStatus::Cancelled)
        return;
    state->ledger.outcome(pending.context, pending.fixId, run.status == ActionStatus::Success,
                          nowMs());
    state->save();
}

void MainWindow::addPingFixMenuActions(QMenu *menu, qint64 localId)
{
    if (!menu || localId <= 0)
        return;
    QJsonObject params{{QStringLiteral("localId"), double(localId)}};
    const AppNotification *item = findPingForFix(params);
    if (!item)
        return;
    const QJsonArray fixes = pingFixesReply(*item).value(QStringLiteral("fixes")).toArray();
    for (const QJsonValue &value : fixes) {
        const QJsonObject fix = value.toObject();
        const QString id = fix.value(QStringLiteral("id")).toString();
        QAction *action = menu->addAction(
            QStringLiteral("Quick fix: %1").arg(fix.value(QStringLiteral("label")).toString()));
        action->setToolTip(fix.value(QStringLiteral("detail")).toString());
        if (fix.value(QStringLiteral("primary")).toBool()) {
            QFont font = action->font();
            font.setBold(true);
            action->setFont(font);
        }
        connect(action, &QAction::triggered, this, [this, localId, id] {
            const AppNotification *target =
                findPingForFix(QJsonObject{{QStringLiteral("localId"), double(localId)}});
            if (!target)
                return;
            const AppNotification copy = *target;
            QString message;
            QString error;
            if (applyPingFix(copy, id, false, &message, &error))
                flashMessage(message);
            else
                flashMessage(QStringLiteral("That fix could not run: %1.")
                                 .arg(error.replace(QLatin1Char('_'), QLatin1Char(' '))),
                             true);
        });
    }
    if (!fixes.isEmpty())
        menu->addSeparator();
}

void MainWindow::handleLocalControlPingsRequest(int connectionId, int requestId,
                                                const QString &method, const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const AppNotification *found = findPingForFix(params);
    if (method == QLatin1String("pings.fixes")) {
        if (!found) {
            server->respond(connectionId, requestId,
                            {{QStringLiteral("found"), false}, {QStringLiteral("fixes"), QJsonArray()}});
            return;
        }
        server->respond(connectionId, requestId, pingFixesReply(*found));
        return;
    }
    if (method == QLatin1String("pings.fix")) {
        if (!found) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_ping"));
            return;
        }
        const AppNotification item = *found;
        QString message;
        QString error;
        if (!applyPingFix(item, params.value(QStringLiteral("fix")).toString(), false, &message,
                          &error)) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        logSystem(QStringLiteral("Pings: quick fix requested by %1.")
                      .arg(localControlDeviceLabel(connectionId)));
        QJsonObject reply = pingFixesReply(item);
        reply.insert(QStringLiteral("ok"), true);
        reply.insert(QStringLiteral("message"), message);
        server->respond(connectionId, requestId, reply);
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}
