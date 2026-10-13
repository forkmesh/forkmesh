#include "AgentParity.h"
#include "AgentScratch.h"
#include "AgentWorktree.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

using namespace forkmesh::ui;

namespace parity = forkmesh::agentparity;

namespace {

class AgentParityState : public QObject
{
public:
    static AgentParityState *of(QObject *owner)
    {
        return static_cast<AgentParityState *>(owner->findChild<QObject *>(
            QStringLiteral("forkmeshAgentParityState"),
            Qt::FindDirectChildrenOnly));
    }

    explicit AgentParityState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshAgentParityState"));
    }

    parity::Watch watch;
    QTimer timer;
    bool busy = false;
};

struct RepoParityInput {
    QString repo;
    QString gitDir;
    QList<parity::Agent> agents;
};

parity::Agent parityAgent(const AgentSession &session, bool recycled)
{
    parity::Agent agent;
    agent.id = session.id;
    agent.title = session.issueTitle;
    agent.branch = session.branchName.trimmed();
    agent.folder = session.worktreeName.trimmed();
    if (agent.folder.isEmpty() && session.id > 0 && !agent.branch.isEmpty())
        agent.folder = forkmesh::agentwt::dirName(session.id, session.issueTitle);
    agent.recycled = recycled;
    const bool owned = !recycled && session.id > 0 && !session.cloud &&
                       !session.directCheckout && !session.associationOnly &&
                       !session.merged && !agent.branch.isEmpty() &&
                       session.status != AgentStatus::Queued &&
                       session.status != AgentStatus::Cleared;
    agent.needsBranch = owned;
    const bool cliRun = session.provider == QLatin1String("claude-code") ||
                        agentIsCodexProvider(session.provider);
    const bool live = session.status == AgentStatus::Running ||
                      session.status == AgentStatus::Waiting;
    const bool keepsWorktree = cliRun && !(session.yolo && !session.vibe);
    agent.needsWorktree = owned && (live || keepsWorktree);
    return agent;
}

QString canonicalWorktreePath(const QString &path)
{
    const QFileInfo info(QDir::cleanPath(path));
    const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    return parent.isEmpty() ? info.absoluteFilePath()
                            : QDir(parent).filePath(info.fileName());
}

QList<parity::Finding> readRepoParity(const RepoParityInput &input)
{
    QList<parity::Finding> findings;
    const QString root = QDir(input.gitDir).filePath(QStringLiteral(".worktrees"));
    const bool agentRoot = forkmesh::agentscratch::isAgentRoot(root);
    if (input.agents.isEmpty() && !agentRoot)
        return findings;
    QByteArray porcelain;
    QByteArray refs;
    if (!runGitCapture(input.gitDir,
                       {QStringLiteral("worktree"), QStringLiteral("list"),
                        QStringLiteral("--porcelain")},
                       &porcelain, nullptr) ||
        !runGitCapture(input.gitDir,
                       {QStringLiteral("for-each-ref"),
                        QStringLiteral("--format=%(refname:short)"),
                        QStringLiteral("refs/heads/")},
                       &refs, nullptr))
        return findings;
    QList<parity::Worktree> worktrees =
        parity::parseWorktrees(QString::fromUtf8(porcelain));
    for (parity::Worktree &worktree : worktrees) {
        if (!QFileInfo::exists(worktree.path))
            worktree.missing = true;
        worktree.path = canonicalWorktreePath(worktree.path);
    }
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QList<parity::Issue> issues = parity::check(
        input.agents, worktrees, parity::parseBranches(QString::fromUtf8(refs)),
        agentRoot ? canonicalRoot : QString());
    for (const parity::Issue &issue : issues)
        findings << parity::Finding{input.repo, issue};
    return findings;
}

}

void MainWindow::startAgentParityWatch()
{
    if (AgentParityState::of(this))
        return;
    auto *state = new AgentParityState(this);
    state->timer.setInterval(parity::kCheckIntervalMs);
    connect(&state->timer, &QTimer::timeout, state,
            [this] { checkAgentParity(); });
    state->timer.start();
    QTimer::singleShot(parity::kFirstCheckDelayMs, state,
                       [this] { checkAgentParity(); });
}

void MainWindow::checkAgentParity()
{
    auto *state = AgentParityState::of(this);
    if (!state || state->busy || !m_agentStore)
        return;
    QHash<QString, QString> checkoutOfRepo;
    QHash<QString, RepoParityInput> byCheckout;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        const QString gitDir = repoAgentGitDir(repo);
        if (gitDir.isEmpty() ||
            !QFileInfo::exists(gitDir + QStringLiteral("/.git")))
            continue;
        const QString checkout = QDir::cleanPath(QFileInfo(gitDir).absoluteFilePath());
        const QString key = repo.owner + QLatin1Char('/') + repo.name;
        checkoutOfRepo.insert(key, checkout);
        if (!byCheckout.contains(checkout))
            byCheckout.insert(checkout, RepoParityInput{key, gitDir, {}});
    }
    if (byCheckout.isEmpty())
        return;
    const auto add = [&](const AgentSession &session, bool recycled) {
        if (session.cloud)
            return;
        const QString checkout = checkoutOfRepo.value(
            session.owner + QLatin1Char('/') + session.name);
        auto it = byCheckout.find(checkout);
        if (!checkout.isEmpty() && it != byCheckout.end())
            it->agents << parityAgent(session, recycled);
    };
    for (const AgentSession &session : std::as_const(m_agentSessions))
        add(session, false);
    for (const AgentSession &session : std::as_const(m_agentRecycled))
        add(session, true);
    QList<RepoParityInput> inputs = byCheckout.values();
    state->busy = true;
    runOffThread<QList<parity::Finding>>(
        [inputs] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("agents"),
                QStringLiteral("check agent, branch and worktree parity"),
                forkmesh::ActionTelemetry::Execution::Worker);
            QList<parity::Finding> findings;
            for (const RepoParityInput &input : inputs)
                findings << readRepoParity(input);
            return findings;
        },
        [this](QList<parity::Finding> findings) {
            auto *state = AgentParityState::of(this);
            if (!state)
                return;
            state->busy = false;
            const parity::Alert alert = state->watch.observe(findings);
            if (!alert.isEmpty())
                addNotification(alert.title, alert.body, true,
                                NotificationLink(),
                                forkmesh::kLocalOperationNoticeKind, QString());
        });
}
