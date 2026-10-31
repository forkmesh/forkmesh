#include "MainWindow.h"
#include "MainWindowInternal.h"

#include "AppPaths.h"
#include "PullReviewAll.h"
#include "PullReviewModel.h"
#include "PullStore.h"

#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QUuid>

using namespace forkmesh::ui;
namespace pullmergeall = forkmesh::pullmergeall;
namespace pullreviewall = forkmesh::pullreviewall;

namespace {

const QString kPullReviewAllRunsSetting = QStringLiteral("pulls/reviewAllRuns");
constexpr qint64 kMaxReportBytes = 4 * 1024 * 1024;

struct PullReviewAllPosted
{
    QList<int> approved;
    QList<int> changesRequested;
    QStringList failed;
    QStringList notOpen;
    int threads = 0;
};

QString numberList(const QList<int> &numbers)
{
    QStringList labels;
    for (int number : numbers)
        labels << QStringLiteral("#%1").arg(number);
    return labels.join(QStringLiteral(", "));
}

}

void MainWindow::loadPullReviewAllRuns()
{
    if (m_pullReviewAllRunsLoaded)
        return;
    m_pullReviewAllRunsLoaded = true;
    m_pullReviewAllRuns = pullreviewall::decodeRuns(
        QSettings().value(kPullReviewAllRunsSetting).toByteArray());
}

void MainWindow::savePullReviewAllRuns()
{
    QSettings settings;
    if (m_pullReviewAllRuns.isEmpty())
        settings.remove(kPullReviewAllRunsSetting);
    else
        settings.setValue(kPullReviewAllRunsSetting,
                          pullreviewall::encodeRuns(m_pullReviewAllRuns));
}

int MainWindow::activePullReviewAllSession(const QString &owner, const QString &name)
{
    loadPullReviewAllRuns();
    QList<int> gone;
    int active = 0;
    for (auto it = m_pullReviewAllRuns.cbegin(); it != m_pullReviewAllRuns.cend(); ++it) {
        const AgentSession *session = findAgentSession(it.key());
        if (!session) {
            gone << it.key();
            continue;
        }
        if (it->owner != owner || it->name != name)
            continue;
        if (session->status == AgentStatus::Running ||
            session->status == AgentStatus::Queued ||
            m_pullReviewAllPosting.contains(it.key()))
            active = it.key();
    }
    if (!gone.isEmpty() && m_agentStore) {
        for (int id : std::as_const(gone))
            m_pullReviewAllRuns.remove(id);
        savePullReviewAllRuns();
    }
    return active;
}

QString MainWindow::pullReviewAllBlocker(int repoIndex)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return QStringLiteral("unknown_repo");
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    if (writableRecordFor(repo).localPath.isEmpty())
        return QStringLiteral("read_only");
    if (m_pullMergeAllPreparing || activePullReviewAllSession(repo.owner, repo.name) > 0)
        return QStringLiteral("busy");
    return QString();
}

void MainWindow::reviewAllPullsWithAgent()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString blocker = pullReviewAllBlocker(m_repoDetailIndex);
    if (blocker == QLatin1String("busy")) {
        const int running = activePullReviewAllSession(repo.owner, repo.name);
        if (running > 0)
            switchToAgentsTab(running);
        flashMessage(QStringLiteral("An agent is already reviewing this repository's "
                                    "pull requests."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    if (!blocker.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Review all with AI"),
                             pullmergeall::errorMessage(blocker));
        return;
    }
    const QList<PullRequest> open = openPullsToMergeAll(m_currentPulls);
    if (open.isEmpty()) {
        setRepoDetailNotice(QStringLiteral("There are no open pull requests to review."));
        return;
    }
    const QString owner = repo.owner;
    const QString name = repo.name;
    preparePullMergeAll(
        owner, name, open,
        [this, owner, name](const QList<pullmergeall::Target> &targets,
                            const QStringList &skipped) {
            const int repoIndex = repoIndexFor(owner, name);
            if (repoIndex < 0)
                return;
            if (targets.isEmpty()) {
                if (repoIndex == m_repoDetailIndex)
                    setRepoDetailNotice(
                        QStringLiteral("No open pull request has its code on this node, "
                                       "so there is nothing to review. Left out: %1")
                            .arg(skipped.join(QStringLiteral(", "))));
                return;
            }
            draftPullReviewAllPrompt(repoIndex, targets, skipped);
        });
}

void MainWindow::draftPullReviewAllPrompt(int repoIndex,
                                          const QList<pullmergeall::Target> &targets,
                                          const QStringList &skipped)
{
    if (!m_issueQuickAdd || repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    const QString dir = forkmesh::paths::dataPath(QStringLiteral("pull-reviews"));
    if (forkmesh::paths::dataRoot().isEmpty() || !QDir().mkpath(dir)) {
        setRepoDetailNotice(QStringLiteral("Review all: could not create the folder for "
                                           "the review report."),
                            true);
        return;
    }
    const QString report = QDir(dir).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    const QString prompt = pullreviewall::agentPrompt(
        repo.owner, repo.name, writableRecordFor(repo).localPath, targets, report);
    m_pullReviewAllDraft = pullmergeall::Draft();
    m_pullReviewAllDraft.run.owner = repo.owner;
    m_pullReviewAllDraft.run.name = repo.name;
    m_pullReviewAllDraft.run.targets = targets;
    m_pullReviewAllDraft.prompt = prompt;
    m_pullReviewAllDraft.skipped = skipped;
    m_pullReviewAllDraftReport = report;
    fillComposerForPullDraft(repoIndex, prompt,
                             pullreviewall::draftMessage(targets.size(), skipped),
                             m_pullReviewAllAiButton);
}

void MainWindow::adoptPullReviewAllDraft(int sessionId, const RepositoryRecord &repo,
                                         const QString &prompt)
{
    if (!pullmergeall::draftMatches(m_pullReviewAllDraft, repo.owner, repo.name, prompt) ||
        !prompt.contains(m_pullReviewAllDraftReport))
        return;
    const pullmergeall::Draft draft = m_pullReviewAllDraft;
    const QString report = m_pullReviewAllDraftReport;
    m_pullReviewAllDraft = pullmergeall::Draft();
    m_pullReviewAllDraftReport.clear();
    if (activePullReviewAllSession(repo.owner, repo.name) > 0)
        return;
    pullreviewall::Run run;
    run.owner = draft.run.owner;
    run.name = draft.run.name;
    run.targets = draft.run.targets;
    run.reportPath = report;
    loadPullReviewAllRuns();
    m_pullReviewAllRuns.insert(sessionId, run);
    savePullReviewAllRuns();
    if (AgentSession *session = findAgentSession(sessionId); session && m_agentStore) {
        QList<int> numbers;
        for (const pullmergeall::Target &target : run.targets)
            numbers << target.number;
        m_agentStore->appendLog(*session,
                                QStringLiteral("==> Review all: reviewing %1. The report "
                                               "goes to %2.\n")
                                    .arg(numberList(numbers), report));
        if (!draft.skipped.isEmpty())
            m_agentStore->appendLog(
                *session, QStringLiteral("==> Review all: left out %1.\n")
                              .arg(draft.skipped.join(QStringLiteral(", "))));
    }
}

void MainWindow::finishPullReviewAllRun(int sessionId)
{
    loadPullReviewAllRuns();
    const auto found = m_pullReviewAllRuns.constFind(sessionId);
    if (found == m_pullReviewAllRuns.constEnd() || m_pullReviewAllPosting.contains(sessionId))
        return;
    const pullreviewall::Run run = found.value();
    const auto sessionLog = [this, sessionId](const QString &text) {
        if (AgentSession *session = findAgentSession(sessionId); session && m_agentStore)
            m_agentStore->appendLog(*session, text + QLatin1Char('\n'));
    };
    const auto giveUp = [this, sessionId, run, sessionLog](const QString &message) {
        m_pullReviewAllRuns.remove(sessionId);
        savePullReviewAllRuns();
        sessionLog(QStringLiteral("!! ") + message);
        logSystem(message);
        if (repoIndexFor(run.owner, run.name) == m_repoDetailIndex)
            setRepoDetailNotice(message, true, forkmesh::kLocalOperationNoticeKind);
    };
    const int repoIndex = repoIndexFor(run.owner, run.name);
    if (repoIndex < 0) {
        giveUp(QStringLiteral("Review all: %1/%2 is no longer open here, so agent #%3's "
                               "reviews were not posted.")
                    .arg(run.owner, run.name)
                    .arg(sessionId));
        return;
    }
    QFile file(run.reportPath);
    if (!file.exists() || file.size() > kMaxReportBytes ||
        !file.open(QIODevice::ReadOnly)) {
        giveUp(QStringLiteral("Review all: agent #%1 finished without a readable report "
                               "at %2, so nothing was posted.")
                    .arg(sessionId)
                    .arg(QDir::toNativeSeparators(run.reportPath)));
        return;
    }
    pullreviewall::Report report;
    QString parseError;
    const bool parsed = pullreviewall::parseReport(file.readAll(), &report, &parseError);
    file.close();
    if (!parsed) {
        giveUp(QStringLiteral("Review all: agent #%1's report could not be read (%2), so "
                               "nothing was posted.")
                    .arg(sessionId)
                    .arg(parseError));
        return;
    }
    PullStore store = pullStoreForRepoIndex(repoIndex);
    if (!store.canWrite()) {
        giveUp(QStringLiteral("Review all: %1/%2 is read-only on this node, so agent #%3's "
                               "reviews were not posted.")
                    .arg(run.owner, run.name)
                    .arg(sessionId));
        return;
    }

    QString reviewer;
    if (const AgentSession *session = findAgentSession(sessionId))
        reviewer = pullreviewall::reviewerLabel(agentProviderName(session->provider),
                                                agentModelLabel(session->model),
                                                report.model);
    else
        reviewer = pullreviewall::reviewerLabel(QString(), QString(), report.model);

    QList<pullreviewall::Review> reviews;
    QList<int> missing;
    for (const pullmergeall::Target &target : run.targets) {
        bool listed = false;
        for (const pullreviewall::Review &review : std::as_const(report.reviews))
            if (review.number == target.number) {
                reviews << review;
                listed = true;
            }
        if (!listed)
            missing << target.number;
    }

    m_pullReviewAllPosting.insert(sessionId);
    runOffThread<PullReviewAllPosted>(
        [store, reviews, reviewer]() mutable {
            PullReviewAllPosted out;
            QHash<int, PullRequest> pulls;
            for (const PullRequest &pr : store.loadAll())
                pulls.insert(pr.number, pr);
            for (const pullreviewall::Review &review : reviews) {
                const PullRequest pr = pulls.value(review.number);
                if (pr.number != review.number || pr.status != QLatin1String("open")) {
                    out.notOpen << QStringLiteral("#%1").arg(review.number);
                    continue;
                }
                QStringList threadIds;
                for (const AiReviewFinding &finding : review.findings) {
                    QString threadId;
                    QString error;
                    store.addThreadComment(
                        review.number, finding.path, QStringLiteral("new"),
                        finding.lineStart, finding.lineEnd,
                        pullreviewall::threadBody(finding), finding.suggestionPatch,
                        pullAnchorFor(pr, finding.path, QStringLiteral("new"),
                                      finding.lineStart, finding.lineEnd),
                        &error, &threadId);
                    if (!threadId.isEmpty())
                        ++out.threads;
                    threadIds << threadId;
                }
                const QString state = pullreviewall::reviewState(review);
                QString error;
                if (!store.addReview(review.number, state,
                                     pullreviewall::summaryBody(review, threadIds, reviewer),
                                     &error)) {
                    out.failed << QStringLiteral("#%1 (%2)").arg(review.number).arg(
                        error.isEmpty() ? QStringLiteral("unknown error") : error);
                    continue;
                }
                if (state == QLatin1String("approved"))
                    out.approved << review.number;
                else
                    out.changesRequested << review.number;
            }
            return out;
        },
        [this, sessionId, run, missing, reviewer, sessionLog](PullReviewAllPosted out) {
            m_pullReviewAllPosting.remove(sessionId);
            m_pullReviewAllRuns.remove(sessionId);
            savePullReviewAllRuns();
            QFile::remove(run.reportPath);
            QStringList parts;
            parts << QStringLiteral("approved %1").arg(
                out.approved.isEmpty() ? QStringLiteral("none") : numberList(out.approved));
            parts << QStringLiteral("requested changes on %1").arg(
                out.changesRequested.isEmpty() ? QStringLiteral("none")
                                               : numberList(out.changesRequested));
            QString message = QStringLiteral("Review all by %1: %2, with %3 review "
                                             "thread(s).")
                                  .arg(reviewer, parts.join(QStringLiteral("; ")))
                                  .arg(out.threads);
            if (!missing.isEmpty())
                message += QStringLiteral(" Not in the report: %1.").arg(numberList(missing));
            if (!out.notOpen.isEmpty())
                message += QStringLiteral(" No longer open: %1.")
                               .arg(out.notOpen.join(QStringLiteral(", ")));
            if (!out.failed.isEmpty())
                message += QStringLiteral(" Could not post: %1.")
                               .arg(out.failed.join(QStringLiteral(", ")));
            sessionLog(QStringLiteral("==> ") + message);
            logSystem(message);
            if (repoIndexFor(run.owner, run.name) == m_repoDetailIndex) {
                reloadPulls();
                setRepoDetailNotice(message, !out.failed.isEmpty(),
                                    forkmesh::kLocalOperationNoticeKind);
            }
        });
}
