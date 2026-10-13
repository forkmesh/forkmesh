#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentIntake.h"
#include "ReleaseDraft.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace forkmesh::ui;

namespace intake = forkmesh::agentintake;
namespace rd = forkmesh::releasedraft;

namespace {

struct RemoteReleasePlan {
    QString error;
    QString errorCode;
    QString tag;
    QString target;
    QString title;
    QString notes;
    QString previousTag;
    bool notesGenerated = false;
};

struct RemoteReleaseListing {
    bool ok = false;
    QJsonArray releases;
    QString latestTag;
    QString latestTitle;
    QString defaultBranch;
    QString publishedTag;
    QString publishedChannel;
};

QString releaseErrorMessage(const QString &code)
{
    if (code == QLatin1String("bad_repository"))
        return QStringLiteral("repository must be owner/name.");
    if (code == QLatin1String("bad_tag"))
        return QStringLiteral("tag must be a release tag such as v1.2.3.");
    if (code == QLatin1String("bad_target"))
        return QStringLiteral("target is not a valid branch name.");
    if (code == QLatin1String("bad_title"))
        return QStringLiteral("title must be at most %1 characters.")
            .arg(intake::kMaxReleaseTitleChars);
    if (code == QLatin1String("bad_notes"))
        return QStringLiteral("notes must be at most %1 characters.")
            .arg(intake::kMaxReleaseNotesChars);
    if (code == QLatin1String("bad_count"))
        return QStringLiteral("count must be a whole number from 1 to %1.")
            .arg(intake::kMaxReleaseListCount);
    if (code == QLatin1String("unknown_method"))
        return QStringLiteral("Unknown method.");
    return QStringLiteral("The request is malformed.");
}

QString latestTagLine(const QString &dir)
{
    QByteArray out;
    if (!runGitCapture(dir,
                       {QStringLiteral("for-each-ref"), QStringLiteral("--sort=-creatordate"),
                        QStringLiteral("--count=1"),
                        QStringLiteral("--format=%(refname:short)%09%(contents:subject)"),
                        QStringLiteral("refs/tags")},
                       &out, nullptr))
        return QString();
    return QString::fromUtf8(out);
}

}

void MainWindow::handleAgentReleaseRequest(int sessionId, const intake::Request &request,
                                           std::function<void(const QJsonObject &)> reply)
{
    intake::ReleaseRequest release;
    QString code;
    if (!intake::parseReleaseRequest(request.method, request.params, &release, &code)) {
        reply(intake::errorReply(code, releaseErrorMessage(code)));
        return;
    }
    if (release.method == intake::ReleaseListMethod) {
        listReleasesRemotely(release, std::move(reply));
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_agentReleaseLimiter.allow(now)) {
        reply(intake::errorReply(
            QStringLiteral("rate_limited"),
            QStringLiteral("Agents may create or publish at most %1 releases every %2 "
                           "minutes on this desktop. Try again later.")
                .arg(intake::kReleaseRateLimitCount)
                .arg(intake::kRateWindowMs / 60000)));
        return;
    }
    const QString requester = QStringLiteral("Agent #%1").arg(sessionId);
    logSystem(QStringLiteral("%1 asked to %2 %3 in %4/%5 through the ForkMesh connector.")
                  .arg(requester,
                       release.method == intake::ReleaseCreateMethod
                           ? QStringLiteral("create release")
                           : QStringLiteral("publish release"),
                       release.tag.isEmpty() ? QStringLiteral("(next tag)") : release.tag,
                       release.owner, release.name));
    if (release.method == intake::ReleaseCreateMethod)
        createReleaseRemotely(release, std::move(reply));
    else
        publishReleaseRemotely(release, std::move(reply));
}

void MainWindow::listReleasesRemotely(const intake::ReleaseRequest &request,
                                      std::function<void(const QJsonObject &)> reply)
{
    int index = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &r = m_repositories.at(i);
        if (r.owner.compare(request.owner, Qt::CaseInsensitive) == 0 &&
            r.name.compare(request.name, Qt::CaseInsensitive) == 0 &&
            repoIndexFor(r.owner, r.name) == i) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        reply(intake::errorReply(
            QStringLiteral("no_checkout"),
            QStringLiteral("%1/%2 is not a repository this desktop has.")
                .arg(request.owner, request.name)));
        return;
    }
    const RepositoryRecord repo = m_repositories.at(index);
    QString dir = repo.localPath.trimmed();
    const bool writable = !dir.isEmpty() && QDir(dir).exists(QStringLiteral(".git"));
    if (!writable)
        dir = QDir(repo.mirrorPath).exists() ? repo.mirrorPath : QString();
    if (dir.isEmpty()) {
        reply(intake::errorReply(
            QStringLiteral("no_checkout"),
            QStringLiteral("%1/%2 has no local copy on this desktop.")
                .arg(repo.owner, repo.name)));
        return;
    }
    const int count = request.count;
    runOffThread<RemoteReleaseListing>(
        [dir, count] {
            RemoteReleaseListing listing;
            QByteArray out;
            if (!runGitCapture(
                    dir,
                    {QStringLiteral("for-each-ref"), QStringLiteral("--sort=-creatordate"),
                     QStringLiteral("--count=%1").arg(count),
                     QStringLiteral("--format=%(refname:short)%x1f%(creatordate:iso-strict)"
                                    "%x1f%(*objectname)%x1f%(objectname)%x1f%(contents:subject)"),
                     QStringLiteral("refs/tags")},
                    &out, nullptr))
                return listing;
            listing.ok = true;
            for (const QByteArray &line : out.split('\n')) {
                const QStringList f = QString::fromUtf8(line).split(QLatin1Char('\x1f'));
                if (f.size() < 5 || f.at(0).trimmed().isEmpty())
                    continue;
                const QString commit =
                    f.at(2).trimmed().isEmpty() ? f.at(3).trimmed() : f.at(2).trimmed();
                listing.releases.append(QJsonObject{
                    {QStringLiteral("tag"), f.at(0).trimmed()},
                    {QStringLiteral("created"), f.at(1).trimmed()},
                    {QStringLiteral("commit"), commit},
                    {QStringLiteral("title"), f.at(4).trimmed()},
                });
            }
            const rd::Latest latest = rd::parseLatest(latestTagLine(dir).toUtf8());
            listing.latestTag = latest.tag;
            listing.latestTitle = latest.title;
            listing.defaultBranch = chooseDefaultBranch(listRepoBranches(dir), QString(), dir,
                                                        QString(), true);
            QByteArray manifest;
            if (!listing.defaultBranch.isEmpty() &&
                runGitCapture(dir,
                              {QStringLiteral("show"),
                               listing.defaultBranch +
                                   QStringLiteral(":.forkmesh/releases/latest/release.json")},
                              &manifest, nullptr)) {
                const QJsonObject json = QJsonDocument::fromJson(manifest).object();
                listing.publishedTag = json.value(QStringLiteral("tag")).toString();
                listing.publishedChannel = json.value(QStringLiteral("channel")).toString();
            }
            return listing;
        },
        [this, reply, owner = repo.owner, name = repo.name, writable,
         canRollOut = releaseRolloutAllowed(repo)](RemoteReleaseListing listing) {
            if (!listing.ok) {
                reply(intake::errorReply(QStringLiteral("git_failed"),
                                         QStringLiteral("Could not read the release tags.")));
                return;
            }
            QJsonArray releases;
            for (const QJsonValue &value : std::as_const(listing.releases)) {
                QJsonObject row = value.toObject();
                const QString tag = row.value(QStringLiteral("tag")).toString();
                row.insert(QStringLiteral("rolloutPending"),
                           releaseRolloutPending(owner, name, tag));
                row.insert(QStringLiteral("publicationPending"),
                           releasePublicationPending(owner, name, tag));
                row.insert(QStringLiteral("artifactPrunePending"),
                           releaseArtifactPrunePending(owner, name, tag));
                releases.append(row);
            }
            const QString next = rd::nextTag(listing.latestTag);
            reply(QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("repository"), owner + QLatin1Char('/') + name},
                {QStringLiteral("canCreate"), writable},
                {QStringLiteral("canRollOut"), canRollOut},
                {QStringLiteral("defaultBranch"), listing.defaultBranch},
                {QStringLiteral("latestTag"), listing.latestTag},
                {QStringLiteral("nextTag"), next},
                {QStringLiteral("nextTitle"),
                 rd::nextTitle(listing.latestTag, listing.latestTitle, next)},
                {QStringLiteral("publishedLatestTag"), listing.publishedTag},
                {QStringLiteral("publishedLatestChannel"), listing.publishedChannel},
                {QStringLiteral("releases"), releases},
            });
        });
}

void MainWindow::createReleaseRemotely(const intake::ReleaseRequest &request,
                                       std::function<void(const QJsonObject &)> reply)
{
    int index = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &r = m_repositories.at(i);
        if (r.owner.compare(request.owner, Qt::CaseInsensitive) == 0 &&
            r.name.compare(request.name, Qt::CaseInsensitive) == 0 &&
            repoIndexFor(r.owner, r.name) == i && !r.localPath.trimmed().isEmpty() &&
            QDir(r.localPath).exists(QStringLiteral(".git"))) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        reply(intake::errorReply(
            QStringLiteral("no_checkout"),
            QStringLiteral("%1/%2 has no working copy on this desktop, so releases "
                           "can't be created here.")
                .arg(request.owner, request.name)));
        return;
    }
    if (m_newReleaseRunning) {
        reply(intake::errorReply(
            QStringLiteral("busy"),
            QStringLiteral("Another release is being created on this desktop. Try again "
                           "in a minute.")));
        return;
    }
    const RepositoryRecord repo = m_repositories.at(index);
    const bool canRollOut = releaseRolloutAllowed(repo);
    if (request.hasRollout && request.rollout && !canRollOut) {
        reply(intake::errorReply(
            QStringLiteral("rollout_unavailable"),
            QStringLiteral("A mirror-node rollout needs the canonical forkmesh/forkmesh "
                           "source working copy with Actions enabled.")));
        return;
    }
    m_newReleaseRunning = true;
    const QString dir = repo.localPath;
    runOffThread<RemoteReleasePlan>(
        [dir, request] {
            RemoteReleasePlan plan;
            const rd::Latest latest = rd::parseLatest(latestTagLine(dir).toUtf8());
            plan.previousTag = latest.tag;
            plan.tag = request.tag.isEmpty() ? rd::nextTag(latest.tag) : request.tag;
            if (plan.tag.isEmpty() || !rd::tagNameValid(plan.tag)) {
                plan.errorCode = QStringLiteral("bad_tag");
                plan.error = QStringLiteral("The repository has no numbered release to "
                                            "count from; pass a tag such as v1.0.0.");
                return plan;
            }
            if (runGitCapture(dir, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                    QStringLiteral("--quiet"),
                                    QStringLiteral("refs/tags/") + plan.tag},
                              nullptr, nullptr)) {
                plan.errorCode = QStringLiteral("tag_exists");
                plan.error = QStringLiteral("Release %1 already exists.").arg(plan.tag);
                return plan;
            }
            plan.target = request.target.isEmpty()
                              ? chooseDefaultBranch(listRepoBranches(dir), QString(), dir,
                                                    QString(), true)
                              : request.target;
            if (plan.target.isEmpty()) {
                plan.errorCode = QStringLiteral("bad_target");
                plan.error = QStringLiteral("The repository has no branch to release from.");
                return plan;
            }
            plan.title = request.title.isEmpty()
                             ? rd::nextTitle(latest.tag, latest.title, plan.tag)
                             : request.title;
            plan.notes = request.notes;
            if (plan.notes.isEmpty()) {
                QByteArray log;
                runGitCapture(dir, rd::logArgs(latest.tag, plan.target), &log, nullptr);
                plan.notes = rd::notesFromLog(log, latest.tag, plan.tag, plan.target);
                plan.notesGenerated = true;
            }
            return plan;
        },
        [this, reply, owner = repo.owner, name = repo.name, dir, canRollOut,
         request](RemoteReleasePlan plan) {
            if (!plan.error.isEmpty()) {
                m_newReleaseRunning = false;
                reply(intake::errorReply(plan.errorCode, plan.error));
                return;
            }
            if (repoIndexFor(owner, name) < 0) {
                m_newReleaseRunning = false;
                reply(intake::errorReply(
                    QStringLiteral("no_checkout"),
                    QStringLiteral("%1/%2 is no longer open on this desktop.")
                        .arg(owner, name)));
                return;
            }
            rd::Cut cut;
            cut.owner = owner;
            cut.name = name;
            cut.dir = dir;
            cut.tag = plan.tag;
            cut.targetRef = plan.target;
            cut.title = plan.title;
            cut.notes = plan.notes;
            cut.canRollOut = canRollOut;
            cut.rollout = request.hasRollout ? request.rollout
                                             : canRollOut && rd::deployableTag(plan.tag);
            const bool rollout = cut.rollout;
            cutRelease(cut, [this, reply, owner, name, plan, rollout](bool created,
                                                                     const QString &detail) {
                m_newReleaseRunning = false;
                if (!created) {
                    reply(intake::errorReply(QStringLiteral("not_created"), detail));
                    return;
                }
                const int current = repoIndexFor(owner, name);
                if (current >= 0 && current == m_repoDetailIndex)
                    loadReleasesPanel();
                reply(QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("repository"), owner + QLatin1Char('/') + name},
                    {QStringLiteral("tag"), plan.tag},
                    {QStringLiteral("previousTag"), plan.previousTag},
                    {QStringLiteral("target"), plan.target},
                    {QStringLiteral("title"), plan.title},
                    {QStringLiteral("notesGenerated"), plan.notesGenerated},
                    {QStringLiteral("rollout"), rollout},
                    {QStringLiteral("detail"), detail},
                });
            });
        });
}

void MainWindow::publishReleaseRemotely(const intake::ReleaseRequest &request,
                                        std::function<void(const QJsonObject &)> reply)
{
    int index = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &r = m_repositories.at(i);
        if (r.owner.compare(request.owner, Qt::CaseInsensitive) == 0 &&
            r.name.compare(request.name, Qt::CaseInsensitive) == 0 &&
            repoIndexFor(r.owner, r.name) == i && !r.localPath.trimmed().isEmpty()) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        reply(intake::errorReply(
            QStringLiteral("no_checkout"),
            QStringLiteral("Only the repository's source working copy can push "
                           "releases, and %1/%2 has none on this desktop.")
                .arg(request.owner, request.name)));
        return;
    }
    const RepositoryRecord repo = m_repositories.at(index);
    if (repo.isPrivate) {
        reply(intake::errorReply(
            QStringLiteral("private_repository"),
            QStringLiteral("Private repositories use sealed replica synchronization, "
                           "not mirror pushes.")));
        return;
    }
    const QString dir = repo.localPath;
    const QString tag = request.tag;
    runOffThread<bool>(
        [dir, tag] {
            return runGitCapture(dir, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                       QStringLiteral("--quiet"),
                                       QStringLiteral("refs/tags/") + tag +
                                           QStringLiteral("^{commit}")},
                                 nullptr, nullptr);
        },
        [this, reply, owner = repo.owner, name = repo.name, tag](bool exists) {
            const int current = repoIndexFor(owner, name);
            if (!exists || current < 0) {
                reply(intake::errorReply(
                    QStringLiteral("no_such_release"),
                    QStringLiteral("%1/%2 has no release tag %3.").arg(owner, name, tag)));
                return;
            }
            pushReleaseToMirrors(current, tag);
            reply(QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("repository"), owner + QLatin1Char('/') + name},
                {QStringLiteral("tag"), tag},
                {QStringLiteral("detail"),
                 QStringLiteral("Release push to the mirrors started; the desktop log "
                                "reports each host.")},
            });
        });
}
