
#include "GitRefusal.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowReposInternal.h"
#include "NodeEventSocket.h"
#include "PrivateMirrorRuntime.h"
#include "PublicMirrorRuntime.h"
#include "SettingsWrite.h"
#include "SystemOpenFolder.h"

#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

using namespace forkmesh::ui;
using namespace mainwindowrepos;

namespace mainwindowrepos {

bool promptAndSetGitIdentity(QWidget *parent, const QString &dir,
                             const QString &suggestedName)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Git author identity"));
    auto *name = new QLineEdit(&dialog);
    name->setText(suggestedName);
    name->setPlaceholderText(QStringLiteral("Your Git author name"));
    auto *email = new QLineEdit(&dialog);
    email->setPlaceholderText(QStringLiteral("you@example.com"));
    auto *note = new QLabel(
        QStringLiteral("ForkMesh will store these values only in this "
                       "repository's local Git config. Review them before "
                       "continuing; your profile name is never applied silently."),
        &dialog);
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("statusLine"));
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Git name"), name);
    form->addRow(QStringLiteral("Git email"), email);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *randomButton = buttons->addButton(
        QStringLiteral("Generate random identity"), QDialogButtonBox::ActionRole);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Use identity"));
    QObject::connect(randomButton, &QPushButton::clicked, &dialog, [name, email] {
        const QString token = QUuid::createUuid()
                                  .toString(QUuid::WithoutBraces)
                                  .left(8)
                                  .toLower();
        name->setText(QStringLiteral("ForkMesh User %1").arg(token));
        email->setText(QStringLiteral("%1@users.forkmesh.local").arg(token));
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(note);
    layout->addLayout(form);
    layout->addWidget(buttons);
    while (dialog.exec() == QDialog::Accepted) {
        const QString gitName = name->text().trimmed();
        const QString gitEmail = email->text().trimmed();
        if (gitName.isEmpty() || gitEmail.isEmpty() ||
            !gitEmail.contains(QLatin1Char('@'))) {
            QMessageBox::warning(&dialog, QStringLiteral("Git author identity"),
                                 QStringLiteral("Enter a Git name and a valid email."));
            continue;
        }
        QString error;
        if (runGitCapture(dir,
                          {QStringLiteral("config"), QStringLiteral("--local"),
                           QStringLiteral("user.name"), gitName},
                          nullptr, &error) &&
            runGitCapture(dir,
                          {QStringLiteral("config"), QStringLiteral("--local"),
                           QStringLiteral("user.email"), gitEmail},
                          nullptr, &error))
            return true;
        QMessageBox::warning(&dialog, QStringLiteral("Git author identity"),
                             error.trimmed().right(240));
    }
    return false;
}

QString privateReplicaRoot()
{
    return forkmesh::paths::dataRoot() +
           QStringLiteral("/private-replicas");
}

QString privateIdentityVaultPath()
{
    return forkmesh::paths::dataRoot() +
           QStringLiteral("/identity/private-mirror-vault.json");
}

QString publicArchiveRoot()
{
    return forkmesh::paths::dataRoot() +
           QStringLiteral("/public-mirror-archives");
}

QByteArray privateIdentityVaultSecret(const ForkMeshIdentity &identity)
{
    if (!identity.isValid())
        return {};
    const QByteArray canonical =
        QByteArrayLiteral("forkmesh-private-mirror-vault-unlock-v1\n") +
        identity.publicKey().toUtf8();
    return QByteArray::fromBase64(
        identity.signData(canonical).toLatin1(),
        QByteArray::Base64UrlEncoding);
}

} // namespace mainwindowrepos

namespace {

struct MirrorSourceIdentity {
    QString owner;
    QString name;
    bool remote = false;

    QString key() const
    {
        return owner.isEmpty() ? name : owner + QLatin1Char('/') + name;
    }
};

bool isWindowsDrivePath(const QString &source)
{
    static const QRegularExpression drive(
        QStringLiteral("^[A-Za-z]:([\\\\/]|$)"));
    return drive.match(source.trimmed()).hasMatch();
}

MirrorSourceIdentity mirrorSourceIdentity(QString source)
{
    MirrorSourceIdentity result;
    source = source.trimmed();
    if (source.isEmpty())
        return result;

    QString path = source;
    const QRegularExpression scpStyle(
        QStringLiteral("^[^/\\s@:]+@[^/\\s:]+:(.+)$"));
    const QRegularExpressionMatch scpMatch = scpStyle.match(source);
    if (scpMatch.hasMatch()) {
        result.remote = true;
        path = scpMatch.captured(1);
    } else if (!isWindowsDrivePath(source)) {
        const QUrl url(source);
        const QString scheme = url.scheme().toLower();
        if (url.isValid() && !scheme.isEmpty()) {
            result.remote = scheme != QLatin1String("file");
            path = result.remote ? url.path() : url.toLocalFile();
        }
    }

    path = QUrl::fromPercentEncoding(path.toUtf8());
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.isEmpty())
        return result;
    QString rawName = segments.takeLast();
    if (rawName.endsWith(QLatin1String(".git"), Qt::CaseInsensitive))
        rawName.chop(4);
    result.name = repoSegment(rawName, QString());
    if (result.remote && !segments.isEmpty())
        result.owner = repoSegment(segments.constLast(), QString());
    return result;
}

} // namespace

namespace mainwindowrepos {

QString repositoryIdentityKey(const RepositoryRecord &repo)
{
    return repoSegment(repo.owner, QStringLiteral("owner")) +
           QLatin1Char('/') +
           repoSegment(repo.name, QStringLiteral("repository"));
}

} // namespace mainwindowrepos

namespace {

QString gitRepositoryIdentity(const QString &gitDir)
{
    if (resolveGitDirPath(gitDir).isEmpty())
        return {};
    QByteArray output;
    if (!cachedGitConfigLocal(gitDir,
                              QStringLiteral("forkmesh.repositoryIdentity"),
                              &output)) {
        return {};
    }
    return QString::fromUtf8(output).trimmed().toLower();
}

} // namespace

namespace mainwindowrepos {

void stampGitRepositoryIdentity(const QString &gitDir, const QString &identity)
{
    if (gitDir.trimmed().isEmpty() || identity.trimmed().isEmpty())
        return;
    if (resolveGitDirPath(gitDir).isEmpty())
        return;
    // The mirror passes rewrite this marker on every rebuild, and the write is
    // a full `git config` spawn that took the GUI down for 25 s on a loaded box
    // (stalls.log 2026-09-09). The stamp is idempotent, so re-writing the value
    // it already holds buys nothing.
    if (gitRepositoryIdentity(gitDir) == identity.trimmed().toLower())
        return;
    runGitCapture(gitDir,
                  {QStringLiteral("config"), QStringLiteral("--local"),
                   QStringLiteral("forkmesh.repositoryIdentity"), identity},
                  nullptr, nullptr);
}

} // namespace mainwindowrepos

namespace {

QString gitOriginUrl(const QString &gitDir)
{
    QByteArray output;
    if (!runGitCapture(gitDir,
                       {QStringLiteral("remote"), QStringLiteral("get-url"),
                        QStringLiteral("origin")},
                       &output, nullptr)) {
        return {};
    }
    return QString::fromUtf8(output).trimmed();
}

QString mirrorSourceDisplay(const QString &source)
{
    QUrl url(source.trimmed());
    if (!isWindowsDrivePath(source) && url.isValid() &&
        !url.scheme().isEmpty()) {
        url.setUserInfo(QString());
        return url.toString(QUrl::RemoveUserInfo);
    }
    return QDir::toNativeSeparators(source.trimmed());
}

bool sameLocalSource(const QString &left, const QString &right)
{
    const QFileInfo a(left);
    const QFileInfo b(right);
    const QString aPath = a.exists() ? a.canonicalFilePath() : a.absoluteFilePath();
    const QString bPath = b.exists() ? b.canonicalFilePath() : b.absoluteFilePath();
    return !aPath.isEmpty() && aPath == bPath;
}

QString validateGitSourceIdentity(const RepositoryRecord &repo,
                                  const QString &source)
{
    const QString expected = repositoryIdentityKey(repo);
    const MirrorSourceIdentity parsed = mirrorSourceIdentity(source);
    if (parsed.remote) {
        if (parsed.owner.isEmpty() || parsed.name.isEmpty() ||
            parsed.key().compare(expected, Qt::CaseInsensitive) != 0) {
            return QStringLiteral("source %1 identifies %2, not %3")
                .arg(mirrorSourceDisplay(source),
                     parsed.key().isEmpty() ? QStringLiteral("no repository")
                                            : parsed.key(),
                     expected);
        }
        return {};
    }

    if (!QFileInfo(source).isDir()) {
        return QStringLiteral("local source %1 is missing for %2")
            .arg(mirrorSourceDisplay(source), expected);
    }
    // Only a "is this a Git repository" probe. resolveGitDirPath() answers it
    // from the filesystem for a repository root — which every configured mirror
    // source is — and falls through to git only for the cases it cannot see
    // (notably a path inside a work tree, where rev-parse walks up).
    if (resolveGitDirPath(source).isEmpty() &&
        !runGitCapture(source,
                       {QStringLiteral("rev-parse"),
                        QStringLiteral("--absolute-git-dir")},
                       nullptr, nullptr)) {
        return QStringLiteral("local source %1 is not a Git repository for %2")
            .arg(mirrorSourceDisplay(source), expected);
    }

    const QString marker = gitRepositoryIdentity(source);
    if (!marker.isEmpty()) {
        if (marker.compare(expected, Qt::CaseInsensitive) != 0) {
            return QStringLiteral("local source %1 is marked as %2, not %3")
                .arg(mirrorSourceDisplay(source), marker, expected);
        }
        return {};
    }

    // Legacy records predate the durable marker.  Repair only when either the
    // checkout directory or its origin independently confirms the repository
    // name. External imports intentionally retain their upstream owner, so the
    // owner portion of that origin is not evidence against the local ForkMesh
    // owner.
    const QString wantedName = repoSegment(repo.name, QStringLiteral("repository"));
    const MirrorSourceIdentity origin = mirrorSourceIdentity(gitOriginUrl(source));
    if (parsed.name.compare(wantedName, Qt::CaseInsensitive) == 0 ||
        origin.name.compare(wantedName, Qt::CaseInsensitive) == 0) {
        stampGitRepositoryIdentity(source, expected);
        return {};
    }
    const QString observed = !origin.key().isEmpty() ? origin.key() : parsed.name;
    return QStringLiteral("local source %1 identifies %2, not %3")
        .arg(mirrorSourceDisplay(source),
             observed.isEmpty() ? QStringLiteral("no repository") : observed,
             expected);
}

QString validateExistingMirrorIdentity(const RepositoryRecord &repo,
                                       const QString &source)
{
    const QString mirror = repo.mirrorPath.trimmed();
    if (mirror.isEmpty() || !QDir(mirror).exists())
        return {};
    const QString expected = repositoryIdentityKey(repo);
    const QString marker = gitRepositoryIdentity(mirror);
    if (!marker.isEmpty()) {
        if (marker.compare(expected, Qt::CaseInsensitive) != 0) {
            return QStringLiteral("local mirror %1 is marked as %2, not %3")
                .arg(mirrorSourceDisplay(mirror), marker, expected);
        }
        return {};
    }

    const QString originUrl = gitOriginUrl(mirror);
    if (originUrl.isEmpty()) {
        // An empty directory can be the residue of an interrupted first clone;
        // it has no repository contents to mis-associate and the normal clone
        // path will establish origin and the marker. An initialized bare repo,
        // however, has no independent identity without either its origin or
        // marker, so do not bless it merely because this record named it.
        if (!QFileInfo(QDir(mirror).filePath(QStringLiteral("HEAD"))).isFile())
            return {};
        return QStringLiteral("local mirror %1 has no origin or repository "
                              "identity marker for %2")
            .arg(mirrorSourceDisplay(mirror), expected);
    }
    const MirrorSourceIdentity origin = mirrorSourceIdentity(originUrl);
    const MirrorSourceIdentity selected = mirrorSourceIdentity(source);
    const bool agrees =
        (!origin.remote && !selected.remote && sameLocalSource(originUrl, source)) ||
        (origin.remote && selected.remote &&
         origin.key().compare(expected, Qt::CaseInsensitive) == 0 &&
         selected.key().compare(expected, Qt::CaseInsensitive) == 0);
    if (!agrees) {
        return QStringLiteral("local mirror %1 still points to source %2 (%3), "
                              "not %4")
            .arg(mirrorSourceDisplay(mirror), mirrorSourceDisplay(originUrl),
                 origin.key().isEmpty() ? QStringLiteral("unknown repository")
                                        : origin.key(),
                 expected);
    }
    stampGitRepositoryIdentity(mirror, expected);
    return {};
}

bool mirrorHasServedCommit(const QString &mirrorPath)
{
    if (mirrorPath.trimmed().isEmpty() || !QDir(mirrorPath).exists())
        return false;
    const QString branch = mirrorHeadBranch(mirrorPath);
    return !mirrorBranchCommit(mirrorPath, branch).isEmpty();
}

} // namespace

namespace mainwindowrepos {

QList<RepoRemoteRow> readRepoRemotes(const QString &gitDir)
{
    QList<RepoRemoteRow> remotes;
    if (gitDir.isEmpty())
        return remotes;
    QByteArray out;
    if (!cachedRepoRemotesRaw(gitDir, &out))
        return remotes;
    QMap<QString, RepoRemoteRow> byName;
    const QStringList lines =
        QString::fromUtf8(out).split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const int tab = line.indexOf('\t');
        if (tab <= 0)
            continue;
        const QString name = line.left(tab).trimmed();
        QString rest = line.mid(tab + 1).trimmed();
        const bool isFetch = rest.endsWith(QStringLiteral("(fetch)"));
        const bool isPush = rest.endsWith(QStringLiteral("(push)"));
        rest.remove(QStringLiteral("(fetch)"));
        rest.remove(QStringLiteral("(push)"));
        rest = rest.trimmed();
        RepoRemoteRow row = byName.value(name);
        row.name = name;
        if (isFetch)
            row.fetchUrl = rest;
        else if (isPush)
            row.pushUrl = rest;
        byName.insert(name, row);
    }
    for (auto it = byName.constBegin(); it != byName.constEnd(); ++it)
        remotes.append(it.value());
    std::sort(remotes.begin(), remotes.end(),
              [](const RepoRemoteRow &a, const RepoRemoteRow &b) {
                  return a.name < b.name;
              });
    return remotes;
}

} // namespace mainwindowrepos

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testMirrorSourceIdentity(const QString &source)
{
    const MirrorSourceIdentity identity = mirrorSourceIdentity(source);
    return (identity.remote ? QStringLiteral("remote:")
                            : QStringLiteral("local:")) +
           identity.key();
}
#endif

QString MainWindow::repositoryMirrorRoot() const
{
    const QString configured =
        QSettings().value(kMirrorRootSetting).toString().trimmed();
    if (!configured.isEmpty())
        return configured;
    return forkmesh::paths::dataRoot() +
           "/mirrors";
}

QString MainWindow::repositoryPreviewRoot() const
{
    const QString configured =
        QSettings().value(kPreviewCacheRootSetting).toString().trimmed();
    if (!configured.isEmpty())
        return configured;
    return forkmesh::paths::dataRoot() +
           "/repo-preview-cache";
}

QString MainWindow::repositoryPreviewPath(const QString &owner,
                                          const QString &name) const
{
    return repositoryPreviewRoot() + "/" +
           repoSegment(owner, QStringLiteral("owner")) + "-" +
           repoSegment(name, QStringLiteral("repository")) + ".git";
}

QString MainWindow::repositoryNetworkCloneUrl(const QString &owner,
                                              const QString &name) const
{
    QUrl url = catalogApiUrl();
    url.setPath("/" + repoSegment(owner, QStringLiteral("owner")) + "/" +
                repoSegment(name, QStringLiteral("repository")));
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString();
}

void MainWindow::changeMirrorLocation()
{
    const QString chosen = QFileDialog::getExistingDirectory(
        this, "Choose where to store mirrored repositories",
        repositoryMirrorRoot());
    if (chosen.isEmpty())
        return;
    QSettings().setValue(kMirrorRootSetting, chosen);
    if (m_mirrorRootEdit)
        m_mirrorRootEdit->setText(chosen);
    logSystem("Mirror storage folder set to " + chosen +
              " (applies to newly added repositories).");
    QMessageBox::information(
        this, "Mirror storage",
        "New mirrors will be stored in:\n" + chosen +
            "\n\nExisting mirrors stay where they are. A local fork will push "
            "into its repository's mirror here.");
}

void MainWindow::changePreviewCacheLocation()
{
    const QString chosen = QFileDialog::getExistingDirectory(
        this, "Choose where to cache repository previews",
        repositoryPreviewRoot());
    if (chosen.isEmpty())
        return;
    QSettings().setValue(kPreviewCacheRootSetting, chosen);
    if (m_previewCacheRootEdit)
        m_previewCacheRootEdit->setText(chosen);
    logSystem("Preview cache folder set to " + chosen + ".");
    QMessageBox::information(
        this, "Preview cache",
        "Temporary repository previews will be stored in:\n" + chosen +
            "\n\nExisting preview caches stay where they are.");
}

QString MainWindow::repositoryChannel(const RepositoryRecord &repo) const
{
    Q_UNUSED(repo);
    return QStringLiteral("#general");
}

QString MainWindow::repositorySource(const RepositoryRecord &repo) const
{
    return repo.localPath.trimmed().isEmpty() ? repo.cloneUrl.trimmed()
                                             : repo.localPath.trimmed();
}

void MainWindow::stampRepositorySyncIdentity(const RepositoryRecord &repo)
{
    const QString local = repo.localPath.trimmed();
    if (!local.isEmpty())
        queueGitRepositoryIdentityStamp(local, repositoryIdentityKey(repo));
}

void MainWindow::queueGitRepositoryIdentityStamp(const QString &gitDir,
                                                 const QString &identity)
{
    const QString dir = gitDir.trimmed();
    if (dir.isEmpty() || identity.trimmed().isEmpty())
        return;
    m_pendingIdentityStamps.insert(dir, identity);
    runOffThread<bool>(
        [dir, identity] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("mirrors"),
                QStringLiteral("stamp repository identity on %1").arg(dir),
                forkmesh::ActionTelemetry::Execution::Worker);
            stampGitRepositoryIdentity(dir, identity);
            return true;
        },
        [this, dir, identity](bool) {
            if (m_pendingIdentityStamps.value(dir) == identity)
                m_pendingIdentityStamps.remove(dir);
        });
}

QString MainWindow::mirrorSyncAssociationRequest(const RepositoryRecord &repo,
                                                 const QString &source)
{
    return repositoryIdentityKey(repo) + QLatin1Char('\n') + source +
           QLatin1Char('\n') + repo.mirrorPath.trimmed();
}

bool MainWindow::validateMirrorSyncAssociation(int index, bool quiet)
{
    if (index < 0 || index >= m_repositories.size())
        return false;
    const RepositoryRecord repo = m_repositories.at(index);
    const QString source = repositorySource(repo);
    if (source.isEmpty()) {
        return applyMirrorSyncAssociation(
            index, QStringLiteral("record %1 has no source")
                       .arg(repositoryIdentityKey(repo)));
    }
    const QString request = mirrorSyncAssociationRequest(repo, source);
    if (m_mirrorSyncAssociationPassed.remove(request))
        return true;
    const auto running = m_mirrorSyncAssociationChecks.find(request);
    if (running != m_mirrorSyncAssociationChecks.end()) {
        running.value() = running.value() && quiet;
        return false;
    }
    m_mirrorSyncAssociationChecks.insert(request, quiet);
    QHash<QString, QString> pendingStamps;
    for (const QString &dir : {source, repo.mirrorPath.trimmed()}) {
        const auto pending = m_pendingIdentityStamps.constFind(dir);
        if (pending != m_pendingIdentityStamps.constEnd())
            pendingStamps.insert(dir, pending.value());
    }
    runOffThread<QString>(
        [repo, source, pendingStamps] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("mirrors"),
                QStringLiteral("check mirror source identity for %1")
                    .arg(repositoryIdentityKey(repo)),
                forkmesh::ActionTelemetry::Execution::Worker);
            for (auto it = pendingStamps.cbegin(); it != pendingStamps.cend();
                 ++it)
                stampGitRepositoryIdentity(it.key(), it.value());
            QString reason = validateGitSourceIdentity(repo, source);
            if (reason.isEmpty())
                reason = validateExistingMirrorIdentity(repo, source);
            return reason;
        },
        [this, index, request, owner = repo.owner, name = repo.name](
            QString reason) {
            const bool quietSync =
                m_mirrorSyncAssociationChecks.take(request);
            const auto matches = [this, &request](int at) {
                if (at < 0 || at >= m_repositories.size())
                    return false;
                const RepositoryRecord &current = m_repositories.at(at);
                return mirrorSyncAssociationRequest(
                           current, repositorySource(current)) == request;
            };
            int at = matches(index) ? index : -1;
            for (int i = 0; at < 0 && i < m_repositories.size(); ++i)
                if (matches(i))
                    at = i;
            if (at < 0) {
                const int moved = repoIndexFor(owner, name);
                if (moved >= 0) {
                    syncRepository(moved, quietSync);
                } else if (!quietSync) {
                    flashMessage(
                        QStringLiteral("Mirror sync for %1/%2 stopped: the "
                                       "repository was removed while its "
                                       "source was being checked.")
                            .arg(owner, name),
                        /*error=*/true);
                }
                return;
            }
            if (!applyMirrorSyncAssociation(at, reason))
                return;
            m_mirrorSyncAssociationPassed.insert(request);
            syncRepository(at, quietSync);
            m_mirrorSyncAssociationPassed.remove(request);
        });
    return false;
}

bool MainWindow::applyMirrorSyncAssociation(int index, const QString &reason)
{
    if (index < 0 || index >= m_repositories.size())
        return false;
    RepositoryRecord &repo = m_repositories[index];
    if (!reason.isEmpty()) {
        const bool newlyQuarantined =
            !repo.mirrorSyncQuarantined ||
            repo.mirrorSyncQuarantineReason != reason;
        repo.mirrorSyncQuarantined = true;
        repo.mirrorSyncQuarantineReason = reason;
        if (!repo.previewOnly)
            saveRepositories();
        refreshRepositoryList();
        if (newlyQuarantined) {
            flashMessage(
                QStringLiteral("Mirror sync quarantined for record %1: %2. No "
                               "fetch was attempted. Reattach the correct "
                               "source in Repository settings, then sync again.")
                    .arg(repositoryIdentityKey(repo), reason),
                /*error=*/true, QString(),
                QStringLiteral("mirror_integrity"));
        }
        return false;
    }

    if (repo.mirrorSyncQuarantined) {
        const QString key = repositoryIdentityKey(repo);
        repo.mirrorSyncQuarantined = false;
        repo.mirrorSyncQuarantineReason.clear();
        if (!repo.previewOnly)
            saveRepositories();
        logSystem(QStringLiteral("Mirror: repaired and released the quarantined "
                                 "source association for %1.")
                      .arg(key));
    }
    return true;
}

QStringList MainWindow::viewAuthGitArgs(const RepositoryRecord &repo,
                                        const QString &source) const
{
    if (!repo.isPrivate)
        return {};
    const QUrl src(source);
    if (!src.isValid() || src.host().isEmpty() ||
        src.host().compare(catalogApiUrl().host(), Qt::CaseInsensitive) != 0)
        return {};
    const QStringList segs =
        src.path().split('/', Qt::SkipEmptyParts);
    if (segs.size() < 2)
        return {};
    const QString owner = segs.at(segs.size() - 2);
    const QString name = segs.at(segs.size() - 1);
    if (!m_profileIdentity.isValid())
        return {};
    const QString viewer = accountOwner();
    if (viewer.isEmpty() || !hasOwnerSigningCapability(viewer))
        return {};
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const bool asOwner = viewer.isEmpty() || viewer == owner;
    const QString user = asOwner ? owner : viewer;
    const QByteArray canonical =
        asOwner
            ? ("forkmesh-view-v1\n" + owner + "\n" + name + "\n" + ts).toUtf8()
            : ("forkmesh-share-view-v1\n" + viewer + "\n" + owner + "\n" + name +
               "\n" + ts).toUtf8();
    const QString password = ts + "." + m_profileIdentity.signData(canonical);
    const QByteArray basic =
        (user + ":" + password).toUtf8().toBase64();
    return {QStringLiteral("-c"),
            QStringLiteral("http.extraHeader=Authorization: Basic ") +
                QString::fromLatin1(basic)};
}

QByteArray MainWindow::privateReplicaAuthorization(
    const RepositoryRecord &repo) const
{
    if (!repo.isPrivate || !m_profileIdentity.isValid())
        return {};
    const QString owner =
        repoSegment(repo.owner, QStringLiteral("owner"));
    const QString name =
        repoSegment(repo.name, QStringLiteral("repository"));
    const QString viewer = accountOwner().trimmed().toLower();
    if (owner.isEmpty() || name.isEmpty() || viewer.isEmpty() ||
        !hasOwnerSigningCapability(viewer)) {
        return {};
    }
    const QString ts =
        QString::number(QDateTime::currentMSecsSinceEpoch());
    const bool asOwner = viewer == owner;
    const QByteArray canonical =
        asOwner
            ? ("forkmesh-view-v1\n" + owner + "\n" + name + "\n" +
               ts)
                  .toUtf8()
            : ("forkmesh-share-view-v1\n" + viewer + "\n" + owner +
               "\n" + name + "\n" + ts)
                  .toUtf8();
    const QString signature =
        m_profileIdentity.signData(canonical);
    if (signature.isEmpty())
        return {};
    const QByteArray credentials =
        (asOwner ? owner : viewer).toUtf8() + ':' + ts.toUtf8() + '.' +
        signature.toUtf8();
    return QByteArrayLiteral("Basic ") + credentials.toBase64();
}

void MainWindow::ensurePrivateMirrorRecipientIdentityRegistered(
    std::function<void(bool, QString)> onDone)
{
    auto finish =
        [onDone = std::move(onDone)](
            bool ok, const QString &error = QString()) mutable {
            if (onDone)
                onDone(ok, error);
        };
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/security/owner-keys"));
    url.setQuery(QString());
    url.setFragment(QString());
    const QString sessionToken = accountSessionTokenForUrl(url);
    if (!m_networkAccess || sessionToken.isEmpty() ||
        !m_profileIdentity.isValid() ||
        !hasOwnerSigningCapability(accountOwner())) {
        finish(false,
               QStringLiteral(
                   "an authenticated desktop-capable account is required"));
        return;
    }
    MirrorCrypto::Identity mirrorIdentity;
    QString localError;
    if (!loadOwnerEncryptionIdentity(&mirrorIdentity, &localError)) {
        finish(false,
               QStringLiteral(
                   "the local encryption identity vault is unavailable"));
        return;
    }
    const QString keyId = mirrorIdentity.keyId();
    const QJsonObject publicBundle = mirrorIdentity.publicBundle();
    mirrorIdentity.x25519Priv.fill('\0');
    mirrorIdentity.mlkemPriv.fill('\0');
    mirrorIdentity = {};

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader(
        "Authorization",
        QByteArrayLiteral("Bearer ") +
            sessionToken.toUtf8());
    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(QJsonObject{
                          {QStringLiteral("publicBundle"), publicBundle},
                      })
            .toJson(QJsonDocument::Compact));
    connect(
        reply, &QNetworkReply::finished, this,
        [reply, keyId, finish = std::move(finish)]() mutable {
            const QByteArray body = reply->readAll();
            const int status =
                reply->attribute(
                         QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            const QJsonObject object =
                QJsonDocument::fromJson(body).object();
            const bool ok =
                reply->error() == QNetworkReply::NoError &&
                status >= 200 && status < 300 &&
                object.value(QStringLiteral("ok")).toBool() &&
                object.value(QStringLiteral("keyId")).toString() ==
                    keyId &&
                !object.value(QStringLiteral("privateKeysStored"))
                     .toBool(true);
            reply->deleteLater();
            finish(ok,
                   ok ? QString()
                      : QStringLiteral(
                            "the relay did not accept the public-only "
                            "encryption key"));
        });
}

bool MainWindow::loadOwnerEncryptionIdentity(
    MirrorCrypto::Identity *identity, QString *error) const
{
    if (!identity) {
        if (error)
            *error = QStringLiteral("No encryption identity destination.");
        return false;
    }
    *identity = {};
    QByteArray vaultSecret =
        privateIdentityVaultSecret(m_profileIdentity);
    if (vaultSecret.size() < 32) {
        vaultSecret.fill('\0');
        vaultSecret.clear();
        if (error)
            *error = QStringLiteral(
                "The local device identity cannot unlock the owner vault.");
        return false;
    }
    const bool ok = PrivateMirrorRuntime::loadOrCreateIdentity(
        privateIdentityVaultPath(), vaultSecret, identity, error);
    vaultSecret.fill('\0');
    vaultSecret.clear();
    if (!ok)
        *identity = {};
    return ok;
}

void MainWindow::loadRepositories()
{
    m_repositories.clear();
    QSettings settings;
    const int count = settings.beginReadArray(kRepositoriesArray);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        RepositoryRecord repo;
        repo.owner = settings.value("owner").toString();
        repo.name = settings.value("name").toString();
        repo.description = settings.value("description").toString();
        repo.cloneUrl = settings.value("cloneUrl").toString();
        repo.relayUrl = settings.value("relayUrl").toString().trimmed();
        repo.localPath = settings.value("localPath").toString();
        repo.solanaAddress = settings.value("solanaAddress").toString();
        repo.mirrorPath = settings.value("mirrorPath").toString();
        repo.forkedFrom =
            settings.value("forkedFrom").toString().trimmed().toLower();
        repo.privateReplicaId =
            settings.value("privateReplicaId").toString().trimmed();
        if (!PrivateMirrorStore::isOpaqueId(repo.privateReplicaId))
            repo.privateReplicaId.clear();
        repo.publicArchiveId =
            settings.value("publicArchiveId").toString().trimmed();
        if (!PublicMirrorRuntime::isArchiveId(repo.publicArchiveId))
            repo.publicArchiveId.clear();
        repo.publishToNetwork = settings.value("publishToNetwork").toBool();
        repo.isPrivate = settings.value("isPrivate").toBool();
        if (!repo.isPrivate && repo.mirrorPath.trimmed().isEmpty() &&
            !repo.owner.trimmed().isEmpty() && !repo.name.trimmed().isEmpty()) {
            repo.mirrorPath = repositoryMirrorRoot() + "/" +
                              repoSegment(repo.owner,
                                          QStringLiteral("owner")) +
                              "-" +
                              repoSegment(repo.name,
                                          QStringLiteral("repository")) +
                              ".git";
        }
        repo.actionsEnabled =
            settings.value("actionsEnabled", repo.owner == accountOwner())
                .toBool();
        repo.actionsAutoApprove =
            settings.value("actionsAutoApprove", true).toBool();
        repo.agentGuardrailsEnabled =
            settings.value("agentGuardrailsEnabled", false).toBool();
        repo.agentAllowedDirectories =
            forkmesh::agentguard::normalizeDirectories(
                settings.value("agentAllowedDirectories").toStringList());
        repo.requirePeerApproval =
            settings.value("requirePeerApproval", true).toBool();
        repo.mergeQueueEnabled =
            settings.value("mergeQueueEnabled", false).toBool();
        repo.mergeQueuePaused = settings.value("mergeQueuePaused", false).toBool();
        repo.mergeQueue = settings.value("mergeQueue").toStringList();
        repo.externallyManagedActions =
            settings.value("externallyManagedActions", false).toBool();
        repo.externalActionsSource =
            settings.value("externalActionsSource").toString().trimmed();
        repo.externalActionsRef =
            settings.value("externalActionsRef").toString().trimmed();
        repo.secretScanningEnabled =
            settings.value("secretScanningEnabled", true).toBool();
        repo.disabledWorkflows = settings.value("disabledWorkflows").toStringList();
        repo.workflowNodes = settings.value("workflowNodes").toStringList();
        repo.hostedSinceMs = settings.value("hostedSinceMs").toLongLong();
        repo.lastSyncMs = settings.value("lastSyncMs").toLongLong();
        repo.privateSyncRefsHash =
            settings.value("privateSyncRefsHash").toString().trimmed();
        repo.mirrorSyncQuarantined =
            settings.value("mirrorSyncQuarantined", false).toBool();
        repo.mirrorSyncQuarantineReason =
            settings.value("mirrorSyncQuarantineReason").toString().trimmed();
        if (!repo.mirrorSyncQuarantined)
            repo.mirrorSyncQuarantineReason.clear();
        if (!repo.name.isEmpty() &&
            (!repositorySource(repo).isEmpty() ||
             PublicMirrorRuntime::isArchiveId(repo.publicArchiveId) ||
             PrivateMirrorStore::isOpaqueId(repo.privateReplicaId))) {
            m_repositories.append(repo);
        }
    }
    settings.endArray();
    bool migrated = false;
    for (RepositoryRecord &repo : m_repositories)
        migrated = reconcileMirrorPath(repo) || migrated;
    if (migrated)
        saveRepositories();
}

bool MainWindow::reconcileMirrorPath(RepositoryRecord &repo)
{
    if (repo.previewOnly || repo.mirrorPath.trimmed().isEmpty() ||
        QDir(repo.mirrorPath).exists())
        return false;
    const QString localPath = repo.localPath.trimmed();
    if (localPath.isEmpty() || !QDir(localPath).exists(QStringLiteral(".git")))
        return false;
    QByteArray out;
    if (!runGitCapture(localPath,
                       {QStringLiteral("remote"), QStringLiteral("get-url"),
                        QStringLiteral("--push"), QStringLiteral("origin")},
                       &out, nullptr))
        return false;
    const QString oldMirror = QString::fromUtf8(out).trimmed();
    if (oldMirror.isEmpty() || oldMirror.contains(QLatin1String("://")) ||
        QDir::cleanPath(oldMirror) == QDir::cleanPath(repo.mirrorPath) ||
        !QFileInfo(oldMirror).isDir() ||
        !QFileInfo(QDir(oldMirror).filePath(QStringLiteral("HEAD"))).isFile())
        return false;
    QDir().mkpath(QFileInfo(repo.mirrorPath).absolutePath());
    if (QDir().rename(oldMirror, repo.mirrorPath)) {
        logSystem(QStringLiteral(
                      "Mirror: moved %1/%2's served mirror from %3 to %4 "
                      "(record and on-disk mirror had diverged).")
                      .arg(repo.owner, repo.name, oldMirror, repo.mirrorPath));
        ensurePushHook(repo);
        return false; // record unchanged; only the directory moved
    }
    // Could not move (permissions, cross-device): serve the mirror where it
    // actually is instead of attesting a path that doesn't exist.
    repo.mirrorPath = oldMirror;
    logSystem(QStringLiteral(
                  "Mirror: %1/%2's recorded mirror path was missing; using the "
                  "existing mirror at %3.")
                  .arg(repo.owner, repo.name, oldMirror));
    return true;
}

void MainWindow::adoptMaterializedMirror(RepositoryRecord &repo,
                                         const QString &repositoryPath)
{
    if (repositoryPath.trimmed().isEmpty() ||
        QDir::cleanPath(repo.mirrorPath) == QDir::cleanPath(repositoryPath)) {
        repo.mirrorPath = repositoryPath;
        if (!repositoryPath.trimmed().isEmpty())
            queueGitRepositoryIdentityStamp(
                repositoryPath, repositoryIdentityKey(repo).toLower());
        return;
    }
    const int carried = carryMirrorReleaseCas(repo.mirrorPath, repositoryPath);
    if (carried > 0)
        logSystem(QStringLiteral(
                      "Mirror: carried %1 release artifact blob(s) for %2/%3 "
                      "into the freshly sealed mirror.")
                      .arg(carried)
                      .arg(repo.owner, repo.name));
    repo.mirrorPath = repositoryPath;
    // A verified private replica is materialized by cloning its authenticated
    // bundle, so Git records that temporary bundle as origin. Stamp the
    // repository identity at the trust boundary where the authenticated
    // materialization is adopted; otherwise the normal sync guard mistakes
    // that implementation-detail origin for a different repository and
    // quarantines every later refresh.
    queueGitRepositoryIdentityStamp(
        repositoryPath, repositoryIdentityKey(repo).toLower());
}

namespace {

constexpr int kRepositorySyncTimesDelayMs = 30000;

bool sameRepositoryValue(const QVariant &stored, const QVariant &value)
{
    if (forkmesh::settings::sameStoredValue(stored, value))
        return true;
    if (value.typeId() != QMetaType::QStringList)
        return false;
    if (stored.isValid() && stored.typeId() != QMetaType::QString &&
        stored.typeId() != QMetaType::QStringList)
        return false;
    return stored.toStringList() == value.toStringList();
}

bool storeRepositoryRecords(const QList<RepositoryRecord> &repositories,
                            bool includeSyncTimes)
{
    QSettings settings;
    settings.beginGroup(kRepositoriesArray);
    QList<std::pair<QString, QVariant>> changes;
    bool syncTimesOnly = true;
    auto stage = [&](const QString &key, const QVariant &value) {
        const bool stored = settings.contains(key);
        if (stored && sameRepositoryValue(settings.value(key), value))
            return;
        syncTimesOnly = syncTimesOnly && stored &&
                        key.endsWith(QLatin1String("/lastSyncMs")) &&
                        settings.value(key).toLongLong() > 0;
        changes.append({key, value});
    };
    int saved = 0;
    for (const RepositoryRecord &repo : repositories) {
        if (repo.previewOnly)
            continue;
        const QString prefix = QString::number(++saved) + QLatin1Char('/');
        auto field = [&](const char *name, const QVariant &value) {
            stage(prefix + QLatin1String(name), value);
        };
        field("owner", repo.owner);
        field("name", repo.name);
        field("description", repo.description);
        field("cloneUrl", repo.cloneUrl);
        field("relayUrl", repo.relayUrl);
        field("localPath", repo.localPath);
        field("solanaAddress", repo.solanaAddress);
        // A private mirror path is an owner-only runtime materialization. Once
        // an opaque encrypted replica exists, never persist that temporary
        // plaintext path. Public repositories intentionally persist their
        // protected bare path; publicArchiveId is only a migration marker from
        // releases that encrypted public data unnecessarily.
        field("mirrorPath",
              !repo.privateReplicaId.isEmpty() ? QString() : repo.mirrorPath);
        field("forkedFrom", repo.forkedFrom);
        field("privateReplicaId", repo.privateReplicaId);
        field("publicArchiveId", repo.publicArchiveId);
        field("publishToNetwork", repo.publishToNetwork);
        field("isPrivate", repo.isPrivate);
        field("actionsEnabled", repo.actionsEnabled);
        field("actionsAutoApprove", repo.actionsAutoApprove);
        field("agentGuardrailsEnabled", repo.agentGuardrailsEnabled);
        field("agentAllowedDirectories", repo.agentAllowedDirectories);
        field("requirePeerApproval", repo.requirePeerApproval);
        field("mergeQueueEnabled", repo.mergeQueueEnabled);
        field("mergeQueuePaused", repo.mergeQueuePaused);
        field("mergeQueue", repo.mergeQueue);
        field("externallyManagedActions", repo.externallyManagedActions);
        field("externalActionsSource", repo.externalActionsSource);
        field("externalActionsRef", repo.externalActionsRef);
        field("secretScanningEnabled", repo.secretScanningEnabled);
        field("disabledWorkflows", repo.disabledWorkflows);
        field("workflowNodes", repo.workflowNodes);
        field("hostedSinceMs", repo.hostedSinceMs);
        field("lastSyncMs", repo.lastSyncMs);
        field("privateSyncRefsHash", repo.privateSyncRefsHash);
        field("mirrorSyncQuarantined", repo.mirrorSyncQuarantined);
        field("mirrorSyncQuarantineReason", repo.mirrorSyncQuarantineReason);
    }
    stage(QStringLiteral("size"), saved);
    if (changes.isEmpty())
        return false;
    if (syncTimesOnly && !includeSyncTimes)
        return true;
    for (const auto &[key, value] : std::as_const(changes))
        settings.setValue(key, value);
    settings.endGroup();
    return false;
}

} // namespace

void MainWindow::saveRepositories() const
{
    if (!storeRepositoryRecords(m_repositories, false) ||
        m_repositorySyncTimesQueued)
        return;
    m_repositorySyncTimesQueued = true;
    QTimer::singleShot(kRepositorySyncTimesDelayMs, this,
                       [this] { flushRepositorySyncTimes(); });
}

void MainWindow::flushRepositorySyncTimes() const
{
    if (!m_repositorySyncTimesQueued)
        return;
    m_repositorySyncTimesQueued = false;
    storeRepositoryRecords(m_repositories, true);
}

QStringList MainWindow::mentionCandidateNames() const
{
    QSet<QString> seen;
    QStringList names;
    auto add = [&seen, &names](const QString &raw) {
        const QString n = raw.trimmed();
        if (n.isEmpty() || seen.contains(n.toLower()))
            return;
        // "Opus 4.8 [bot]" is a display name, not a handle: it has a space in
        // it, nobody can be invited as one, and the bots are offered under
        // their aliases (issueMentionCandidateNames) instead.
        if (forkmesh::issuemention::isBotAuthorName(n))
            return;
        seen.insert(n.toLower());
        names.append(n);
    };

    for (const MemberInfo &m : m_homeRoster)
        add(m.name);

    for (const Issue &iss : m_currentIssues) {
        add(iss.authorName);
        for (const IssueEvent &ev : iss.events)
            add(ev.authorName);
    }
    for (const PullRequest &pr : m_currentPulls) {
        add(pr.authorName);
        for (const PullEvent &ev : pr.events)
            add(ev.authorName);
    }

    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.localeAwareCompare(b) < 0;
    });
    return names;
}

// The same roster with the model bots on the front. Only the issue composers
// use it: an @alias in an issue is answered by the host (answerIssueModelMentions),
// while the same word in a pull, a discussion or a Cove invite addresses nobody,
// so offering it there would be a promise the app does not keep.
QStringList MainWindow::issueMentionCandidateNames() const
{
    return forkmesh::issuemention::aliases() + mentionCandidateNames();
}

void MainWindow::refreshRepositoryList()
{
    // Re-entrancy guard: the periodic m_homeStatsTimer fires this once
    // a minute, which can land inside another heavy refresh's GitKeepAlive pump.
    // Running the per-repo git reads (mirror head/commit/size) plus
    // refreshRepoSyncIndicators nested in that pump stacks synchronous git work and
    // stalls the GUI. Coalesce + defer to a fresh event-loop turn instead; the
    // deferred call re-checks the guard and re-arms if the pump is still active.
    if (m_heavyRefreshInFlight) {
        if (!m_repoListRefreshQueued) {
            m_repoListRefreshQueued = true;
            QTimer::singleShot(250, this, [this] {
                m_repoListRefreshQueued = false;
                refreshRepositoryList();
            });
        }
        return;
    }
    const ScopedFlag refreshGuard(m_heavyRefreshInFlight);

    m_repoMenuEntries.clear();
    m_nodeMenuEntries.clear();
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size() &&
        !repositoryBelongsToActiveRelay(m_repositories.at(m_repoDetailIndex)))
        clearRepoDetail();

    const bool selfIsUserAccount =
        m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty();
    const QString localUser = selfIsUserAccount
                                  ? accountOwner().trimmed().toLower()
                                  : QString();
    auto isDirectoryUser = [this, localUser](const QString &name) {
        const QString key = name.trimmed().toLower();
        return m_chatDirectoryUsers.contains(key) ||
               (!localUser.isEmpty() && key == localUser);
    };

    QHash<QString, QList<int>> reposByNode;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        if (repositoryBelongsToActiveRelay(repo))
            reposByNode[repo.owner].append(i);
    }

    struct NodeInfo {
        bool inRoster = false;
        bool online = false;
        bool self = false;
        QString solana;
        QString balance;
        QString platform;
        QStringList mirrors;
    };
    QHash<QString, NodeInfo> nodes;
    QStringList nodeOrder;
    QList<MemberInfo> ranked = m_homeRoster;
    std::sort(ranked.begin(), ranked.end(),
              [](const MemberInfo &a, const MemberInfo &b) {
                  if (a.self != b.self)
                      return a.self;
                  if (a.online != b.online)
                      return a.online;
                  return a.name.localeAwareCompare(b.name) < 0;
              });
    for (const MemberInfo &m : std::as_const(ranked)) {
        if (m.accountKind.compare(QLatin1String("user"),
                                  Qt::CaseInsensitive) == 0)
            continue;
        if (isTemporaryChatGuest(m))
            continue;
        const QString nodeKey = nodeListIdentityKey(m);
        if (nodeKey.isEmpty() || isDirectoryUser(nodeKey))
            continue;
        if (!nodes.contains(nodeKey)) {
            NodeInfo ni;
            ni.inRoster = true;
            ni.online = m.online;
            ni.self = m.self;
            ni.solana = m.solanaAddress.trimmed();
            if (ni.self && ni.solana.isEmpty())
                ni.solana = savedSolanaAddress();
            ni.balance = m.solanaBalance.trimmed();
            ni.platform = m.platform;
            ni.mirrors = m.mirrors;
            nodes.insert(nodeKey, ni);
            nodeOrder.append(nodeKey);
        } else {
            NodeInfo &ni = nodes[nodeKey];
            if (m.online)
                ni.online = true;
            if (ni.platform.isEmpty())
                ni.platform = m.platform;
            if (!m.mirrors.isEmpty())
                ni.mirrors = m.mirrors;
        }
    }
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        if (!repositoryBelongsToActiveRelay(repo))
            continue;
        const QString owner = repo.owner;
        if (isDirectoryUser(owner))
            continue;
        if (!nodes.contains(owner)) {
            if (repo.previewOnly)
                continue;
            nodes.insert(owner, NodeInfo());
            nodeOrder.append(owner);
        }
    }

    for (const MemberInfo &user : std::as_const(m_chatDirectoryUsers)) {
        for (const QString &node :
             user.nodeName.split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
            const QString nodeName = node.trimmed();
            if (nodeName.isEmpty() || isDirectoryUser(nodeName) ||
                nodes.contains(nodeName))
                continue;
            nodes.insert(nodeName, NodeInfo());
            nodeOrder.append(nodeName);
        }
    }

    QSet<QString> shownLocalRepoKeys;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (!repositoryBelongsToActiveRelay(repo))
            continue;
        const QString key = repo.owner + "/" + repo.name;
        shownLocalRepoKeys.insert(key);
    }
    QSet<QString> shownAdvertised; // dedupe a repo advertised by several nodes

    bool selectedStillExists = false;
    for (const QString &node : std::as_const(nodeOrder)) {
        const NodeInfo info = nodes.value(node);
        NodeMenuEntry entry;
        entry.name = node;
        entry.platform = info.platform;
        entry.online = info.inRoster && info.online;
        entry.self = info.self;
        entry.repoCount = reposByNode.value(node).size();
        m_nodeMenuEntries.append(entry);
        if (node == m_selectedNode)
            selectedStillExists = true;
    }

    if (!selectedStillExists)
        m_selectedNode = nodeOrder.isEmpty() ? QString() : nodeOrder.first();
    updateNodeSwitcher();

    const NodeInfo selInfo = nodes.value(m_selectedNode);
    for (int i : reposByNode.value(m_selectedNode)) {
        const RepositoryRecord &repo = m_repositories.at(i);
        QString label = repo.name;
        if (repo.previewOnly)
            label += "  \xC2\xB7 preview";
        else if (!repo.publishToNetwork)
            label += "  \xC2\xB7 local only";
        else if (repo.isPrivate && !accountOwner().isEmpty() &&
                 repo.owner != accountOwner())
            label += "  \xC2\xB7 shared";
        else
            label += repo.isPrivate ? "  \xC2\xB7 private"
                                    : "  \xC2\xB7 public";
        if (m_syncingRepos.contains(i))
            label += repo.previewOnly ? "  \xC2\xB7 caching" : "  \xC2\xB7 syncing";
        QString detail;
        if (repo.previewOnly) {
            detail = QString::fromUtf8("Cached preview \xE2\x80\x94 browsed from the "
                                    "network, not added to this device");
            if (!repo.cloneUrl.isEmpty())
                detail += QStringLiteral("\nSource: %1").arg(repo.cloneUrl);
        } else {
            detail = QStringLiteral("Local workspace on this device");
            if (!repo.localPath.isEmpty())
                detail += QStringLiteral("\nLocal path: %1").arg(repo.localPath);
            if (!repo.cloneUrl.isEmpty())
                detail += QStringLiteral("\nImported from: %1").arg(repo.cloneUrl);
        }
        RepoMenuEntry entry;
        entry.label = label;
        entry.index = i;
        entry.detail = detail;
        entry.icon = themedOcticon(
            repo.previewOnly ? QStringLiteral("cloud") : QStringLiteral("repo"),
            repo.previewOnly ? Theme::nextOr(Theme::Role::FgMuted, QColor("#58a6ff"))
                             : (repo.publishToNetwork ? Theme::nextOr(Theme::Role::AccentFg, QColor("#58a6ff"))
                                                      : Theme::remapColor(QColor("#6e7681"))),
            14);
        m_repoMenuEntries.append(entry);
    }
    for (const QString &ownerName : selInfo.mirrors) {
        if (shownLocalRepoKeys.contains(ownerName) ||
            shownAdvertised.contains(ownerName))
            continue;
        shownAdvertised.insert(ownerName);
        RepoMenuEntry entry;
        entry.label = QString::fromUtf8("\xE2\x86\x93 ") + ownerName +
                      QString::fromUtf8("   \xC2\xB7 browse");
        entry.index = -2; // advertised mirror marker
        entry.advertised = ownerName;
        entry.icon = themedOcticon("cloud", Theme::nextOr(Theme::Role::FgMuted, QColor("#58a6ff")), 14);
        m_repoMenuEntries.append(entry);
    }
    updateRepoSwitcher();
    refreshRepoSyncIndicators();
    updateRepoDetailStatus();
    updateRepoActionMenus();
    updateHomeStats();

    int firstRepo = -1;
    for (const RepoMenuEntry &entry : std::as_const(m_repoMenuEntries))
        if (entry.index >= 0) {
            firstRepo = entry.index;
            break;
        }
    if (m_deferredStartupRun && m_pendingRestoreRepoIndex < 0 &&
        !m_nodeSwitching && !m_repoDetailLoading) {
        if (m_repoDetailIndex < 0 && firstRepo >= 0)
            openRepoDetailDeferred(firstRepo);
        else if (firstRepo < 0 && m_repoDetailIndex >= 0)
            clearRepoDetail();
    }

    refreshMirrorAdverts();
}

void MainWindow::refreshMirrorAdverts()
{
    if (!m_backend)
        return;
    if (m_mirrorAdvertRefreshInFlight)
        return;

    struct AdvertInput {
        RepositoryRecord repo;
        QString catalogOwner;
    };
    QList<AdvertInput> repositories;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly || !repositoryBelongsToActiveRelay(repo))
            continue;
        repositories.append({repo, catalogOwner(repo)});
    }

    // The skip signature is intentionally filesystem-only. The old "skip"
    // check launched several synchronous Git subprocesses just to decide
    // whether the expensive snapshot could be skipped, defeating its own
    // purpose. It is read on a worker too: the recursive refs walk it needs
    // is thousands of stats for a checkout with hundreds of agent branches,
    // and on 2026-09-12 that walk sat on the GUI thread inside a 95 s freeze
    // while the box was paging (stalls.log 0020, sampled in getdents64).
    // Overlap is collapsed through m_mirrorAdvertRefreshInFlight for both
    // phases; the pass's own completion asks again once it lands.
    m_mirrorAdvertRefreshInFlight = true;
    runOffThread<QString>(
        [repositories] {
            QString inputSignature;
            for (const AdvertInput &input : repositories) {
                const RepositoryRecord &repo = input.repo;
                const QString gitDir = resolveGitDirPath(repo.localPath);
                const QFileInfo workHead(gitDir + QStringLiteral("/HEAD"));
                const QFileInfo worktrees(gitDir + QStringLiteral("/worktrees"));
                // A recursive refs stamp for the mirror (a nested agent/x
                // branch never touched the flat refs/ mtime), the checkout's
                // HEAD and worktrees, and no lastSyncMs: autoSyncMirrors()
                // rewrote it every minute, which defeated the 60 s gate below
                // on every round and re-ran the fifteen-command pass per
                // repository for nothing — the only reader of these adverts
                // (the Repos page's local table) does not show the sync time.
                inputSignature +=
                    repo.owner + QLatin1Char('|') + repo.name +
                    QLatin1Char('|') + repo.cloneUrl + QLatin1Char('|') +
                    QString::number(repo.publishToNetwork) + QLatin1Char('|') +
                    repo.mirrorPath + QLatin1Char('|') + repo.localPath +
                    QLatin1Char('|') + gitRefsDirSignature(repo.mirrorPath) +
                    QLatin1Char('|') +
                    QString::number(
                        workHead.lastModified().toMSecsSinceEpoch()) +
                    QLatin1Char('|') +
                    QString::number(
                        worktrees.lastModified().toMSecsSinceEpoch()) +
                    QLatin1Char('\n');
            }
            return inputSignature;
        },
        [this, repositories](QString inputSignature) {
            m_mirrorAdvertRefreshInFlight = false;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            constexpr qint64 kAdvertRefreshMs = 60 * 1000;
            if (!m_mirrorAdvertSig.isEmpty() &&
                inputSignature == m_mirrorAdvertInputSig &&
                m_mirrorAdvertCompletedAtMs > 0 &&
                now - m_mirrorAdvertCompletedAtMs < kAdvertRefreshMs) {
                return;
            }
            // The pass exists to fill the Repos page's local-repository
            // table. While that page is not on screen, remember that the
            // figures fell behind and let refreshNetworkReposPage() run the
            // pass when the page comes up.
            if (!m_localReposTable || !m_localReposTable->isVisible()) {
                m_mirrorAdvertStale = true;
                return;
            }
            m_mirrorAdvertStale = false;

            m_mirrorAdvertRefreshInFlight = true;
            m_mirrorAdvertInputSig = inputSignature;
            auto adverts = std::make_shared<QList<MirrorAdvert>>();
            QThread *worker = QThread::create([repositories, adverts] {
                const forkmesh::BackgroundScope activity(
                    QStringLiteral("mirrors"),
                    QStringLiteral("refresh %1 repository advert(s)")
                        .arg(repositories.size()),
                    forkmesh::ActionTelemetry::Execution::Worker);
                for (const AdvertInput &input : repositories) {
                    const RepositoryRecord &repo = input.repo;
                    MirrorAdvert advert;
                    advert.ownerName =
                        input.catalogOwner + "/" +
                        repoSegment(repo.name, QStringLiteral("repository"));
                    advert.source =
                        repoSegment(repo.owner, QStringLiteral("owner")) + "/" +
                        repoSegment(repo.name, QStringLiteral("repository"));
                    const MirrorBranchTip primaryTip =
                        mirrorPrimaryBranchTip(repo.mirrorPath, repo.localPath);
                    advert.branch = primaryTip.branch;
                    advert.commit = primaryTip.commit;
                    if (advert.commit.isEmpty())
                        continue;
                    advert.refsFingerprint =
                        MainWindow::mirrorStateHash(repo.mirrorPath);
                    advert.commitIdentity = mirrorCommitIdentity(
                        repo.mirrorPath, repo.localPath, advert.commit);
                    advert.updatedMs = repo.lastSyncMs;
                    advert.sizeBytes = mirrorRepoSizeBytes(repo.mirrorPath);
                    advert.issueCount =
                        mirrorIssueCount(repo.mirrorPath, advert.branch);
                    advert.commitCount =
                        mirrorCommitCount(repo.mirrorPath, advert.branch);
                    advert.mergeCount =
                        mirrorMergeCommitCount(repo.mirrorPath, advert.branch);
                    advert.activityWeeks =
                        mirrorCommitActivityWeeks(repo.mirrorPath, advert.branch);
                    advert.mergeActivityWeeks =
                        mirrorMergeActivityWeeks(repo.mirrorPath, advert.branch);
                    advert.branchCount = mirrorBranchCount(repo.mirrorPath);
                    advert.pullCount =
                        mirrorPullCount(repo.mirrorPath, advert.branch);
                    advert.discussionCount =
                        mirrorDiscussionCount(repo.mirrorPath, advert.branch);
                    advert.worktreeCount = mirrorWorktreeCount(repo.localPath);
                    advert.artifactCount = mirrorArtifactCount(repo.mirrorPath);
                    adverts->append(advert);
                }
            });
            connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            connect(worker, &QThread::finished, this,
                    [this, adverts, repositories, inputSignature] {
                        m_mirrorAdvertRefreshInFlight = false;
                        m_mirrorAdvertCompletedAtMs =
                            QDateTime::currentMSecsSinceEpoch();
                        m_mirrorAdvertSig = inputSignature;
                        // The Repos page's private/local table reads what this
                        // pass measured rather than running the same git work
                        // again on the GUI thread. Adverts are keyed by the
                        // sanitized source, so map them back onto the record
                        // each one came from.
                        m_localRepoAdverts.clear();
                        for (const AdvertInput &input : repositories) {
                            const QString source =
                                repoSegment(input.repo.owner,
                                            QStringLiteral("owner")) +
                                "/" +
                                repoSegment(input.repo.name,
                                            QStringLiteral("repository"));
                            for (const MirrorAdvert &advert : *adverts) {
                                if (advert.source != source)
                                    continue;
                                m_localRepoAdverts.insert(
                                    input.repo.owner + QLatin1Char('/') +
                                        input.repo.name,
                                    advert);
                                break;
                            }
                        }
                        // Only repaint a table the Repos page has already
                        // filled once: before its first load nothing above
                        // stands for anything yet, so every local repository
                        // would look relay-less.
                        if (m_networkRepoRowCount >= 0)
                            renderLocalOnlyRepos();
                        refreshMirrorAdverts();
                    });
            worker->start();
        });
}

void MainWindow::mirrorAdvertisedRepo(const QString &ownerName)
{
    const int slash = ownerName.indexOf('/');
    if (slash <= 0)
        return;
    const QString owner = ownerName.left(slash);
    const QString name = ownerName.mid(slash + 1);
    int previewIndex = -1;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &r = m_repositories.at(i);
        if (!repositoryBelongsToActiveRelay(r) || r.owner != owner ||
            r.name != name)
            continue;
        if (r.previewOnly) {
            previewIndex = i;
            continue;
        }
        QMessageBox::information(this, "Mirror",
                                 "You already mirror this repository.");
        return;
    }
    if (previewIndex >= 0) {
        mirrorPreviewRepository(previewIndex);
        return;
    }
    const QString cloneUrl = repositoryNetworkCloneUrl(owner, name);
    if (cloneUrl.isEmpty())
        return;

    if (QMessageBox::question(
            this, "Mirror it too",
            QStringLiteral("Mirror %1 into your local mirrors?\n\nIt will be cloned "
                           "from %2.")
                .arg(ownerName, cloneUrl)) != QMessageBox::Yes)
        return;

    RepositoryRecord repo;
    repo.owner = owner;
    repo.name = name;
    repo.cloneUrl = cloneUrl;
    repo.publishToNetwork = true;
    repo.actionsEnabled = false;
    repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
    repo.mirrorPath = repositoryMirrorRoot() + "/" +
                      repoSegment(owner, QStringLiteral("owner")) + "-" +
                      repoSegment(name, QStringLiteral("repository")) + ".git";
    stampRepositoryRelay(repo);
    m_repositories.append(repo);
    saveRepositories();
    if (m_backend)
        m_backend->addChannel(repositoryChannel(repo));
    refreshRepositoryList();
    logSystem("Mirroring " + ownerName + " from " + cloneUrl);
    syncRepository(m_repositories.size() - 1); // clone from the network mirror
}

void MainWindow::previewAdvertisedRepo(const QString &ownerName)
{
    const int slash = ownerName.indexOf('/');
    if (slash <= 0)
        return;
    const QString owner = ownerName.left(slash);
    const QString name = ownerName.mid(slash + 1);

    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        if (repositoryBelongsToActiveRelay(repo) && repo.owner == owner &&
            repo.name == name) {
            openRepoDetail(i);
            if (repo.previewOnly && !m_syncingRepos.contains(i) &&
                !QDir(repo.mirrorPath).exists())
                syncRepository(i);
            return;
        }
    }

    RepositoryRecord repo;
    repo.owner = owner;
    repo.name = name;
    repo.cloneUrl = repositoryNetworkCloneUrl(owner, name);
    repo.previewOnly = true;
    repo.actionsEnabled = false;
    repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
    repo.mirrorPath = repositoryPreviewPath(owner, name);
    stampRepositoryRelay(repo);
    m_repositories.append(repo);
    const int index = m_repositories.size() - 1;
    refreshRepositoryList();
    logSystem("Preview: caching " + ownerName + " from " + repo.cloneUrl);
    openRepoDetail(index);
    syncRepository(index);
}

void MainWindow::mirrorPreviewRepository(int index)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord preview = m_repositories.at(index);
    if (!preview.previewOnly)
        return;

    for (int i = 0; i < m_repositories.size(); ++i) {
        if (i == index)
            continue;
        const RepositoryRecord &repo = m_repositories.at(i);
        if (!repo.previewOnly && repo.owner == preview.owner &&
            repo.name == preview.name) {
            QMessageBox::information(
                this, "Mirror repository",
                QStringLiteral("You already mirror %1/%2.")
                    .arg(preview.owner, preview.name));
            return;
        }
    }

    const QString permanentPath = repositoryMirrorRoot() + "/" +
                                  repoSegment(preview.owner, QStringLiteral("owner")) +
                                  "-" +
                                  repoSegment(preview.name,
                                              QStringLiteral("repository")) +
                                  ".git";
    const QString source =
        (!preview.mirrorPath.isEmpty() && QDir(preview.mirrorPath).exists())
            ? preview.mirrorPath
            : preview.cloneUrl;
    if (source.isEmpty()) {
        setRepoDetailNotice("Preview is not cached yet; try again after it loads.",
                            true);
        return;
    }

    if (!QDir().mkpath(QFileInfo(permanentPath).absolutePath())) {
        QMessageBox::warning(this, "Mirror repository",
                             "Could not create " +
                                 QFileInfo(permanentPath).absolutePath());
        return;
    }

    if (QDir(permanentPath).exists()) {
        RepositoryRecord &repo = m_repositories[index];
        repo.previewOnly = false;
        repo.publishToNetwork = true;
        repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
        repo.mirrorPath = permanentPath;
        if (repo.lastSyncMs <= 0)
            repo.lastSyncMs = QDateTime::currentMSecsSinceEpoch();
        saveRepositories();
        ensurePushHook(repo);
        if (m_backend)
            m_backend->addChannel(repositoryChannel(repo));
        refreshRepositoryMirror(index);
        startNodeEventSocket();
        refreshRepositoryList();
        openRepoDetail(index);
        setRepoDetailNotice("Mirroring " + repo.owner + "/" + repo.name + ".");
        return;
    }

    m_syncingRepos.insert(index, false);
    refreshRepositoryList();
    setRepoDetailNotice("Creating permanent mirror...");
    logSystem(QStringLiteral("Mirror: promoting preview %1/%2 from %3 to %4.")
                  .arg(preview.owner, preview.name, source, permanentPath));

    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this,
            [this, process, index, permanentPath](int exitCode,
                                                  QProcess::ExitStatus status) {
                const QString errors =
                    QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                m_syncingRepos.remove(index);
                if (index < 0 || index >= m_repositories.size()) {
                    refreshRepositoryList();
                    return;
                }
                RepositoryRecord &repo = m_repositories[index];
                if (status == QProcess::NormalExit && exitCode == 0) {
                    repo.previewOnly = false;
                    repo.publishToNetwork = true;
                    repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
                    repo.lastSyncMs = QDateTime::currentMSecsSinceEpoch();
                    repo.mirrorPath = permanentPath;
                    saveRepositories();
                    ensurePushHook(repo);
                    if (m_backend)
                        m_backend->addChannel(repositoryChannel(repo));
                    refreshRepositoryMirror(index);
                    startNodeEventSocket();
                    refreshRepositoryList();
                    openRepoDetail(index);
                    logSystem("Mirror: added " + repo.owner + "/" + repo.name +
                              " from preview cache.");
                    setRepoDetailNotice("Mirroring " + repo.owner + "/" +
                                        repo.name + ".");
                } else {
                    refreshRepositoryList();
                    logSystem("Mirror: could not promote preview: " +
                              errors.right(300));
                    setRepoDetailNotice(
                        "Could not create mirror" +
                            (errors.isEmpty() ? QString() :
                                                ": " + errors.right(160)),
                        true);
                }
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, index](QProcess::ProcessError) {
                process->deleteLater();
                m_syncingRepos.remove(index);
                refreshRepositoryList();
                setRepoDetailNotice("Could not run git. Install Git and try again.",
                                    true);
            });
    trackProcessActivity(process, QStringLiteral("fork"),
                         QStringLiteral("Mirroring %1/%2")
                             .arg(preview.owner, preview.name));
    process->start(QStringLiteral("git"),
                   {QStringLiteral("clone"), QStringLiteral("--mirror"), source,
                    permanentPath});
}

void MainWindow::promptAddRepository()
{
    const QString selectedPath = QFileDialog::getExistingDirectory(
        this, "Choose a Git repository or a folder containing repositories");
    if (selectedPath.isEmpty())
        return;
    importRepositoriesFrom(selectedPath);
}

void MainWindow::openFolderFromSystem(const QString &folder)
{
    QStringList localPaths;
    localPaths.reserve(m_repositories.size());
    for (const RepositoryRecord &repo : std::as_const(m_repositories))
        localPaths.append(repo.localPath);
    const int known =
        forkmesh::systemopen::repositoryContaining(folder, localPaths);
    if (known >= 0) {
        logSystem("Opened " + folder + " from the file manager.");
        openRepoDetail(known);
        return;
    }
    QString target = folder;
    if (forkmesh::systemopen::gitCheckoutsAt(folder).isEmpty()) {
        const QString enclosing =
            forkmesh::systemopen::enclosingGitCheckout(folder);
        if (!enclosing.isEmpty())
            target = enclosing;
    }
    importRepositoriesFrom(target, true);
}

void MainWindow::importRepositoriesFrom(const QString &selectedPath,
                                        bool openSingleImport)
{
    const QStringList paths =
        forkmesh::systemopen::gitCheckoutsAt(selectedPath);
    if (paths.isEmpty()) {
        QMessageBox::warning(
            this, "Import repositories",
            "No Git repositories were found. Choose a Git checkout or a parent "
            "folder whose immediate subfolders are Git repositories.");
        return;
    }

    QStringList importPaths;
    QStringList skippedNames;
    QSet<QString> batchNames;
    for (const QString &path : std::as_const(paths)) {
        const QString name = repoNameFromUrl(path);
        const QString key = name.toLower();
        if (name.isEmpty() || batchNames.contains(key) ||
            repoIndexFor(accountOwner(), name) >= 0) {
            skippedNames.append(name.isEmpty() ? path : name);
            continue;
        }
        batchNames.insert(key);
        importPaths.append(path);
    }
    if (importPaths.isEmpty()) {
        QMessageBox::information(this, "Import repositories",
                                 "All discovered repositories are already added.");
        return;
    }

    QMessageBox visibility(this);
    visibility.setWindowTitle(QStringLiteral("Repository visibility"));
    visibility.setIcon(QMessageBox::Question);
    visibility.setText(
        importPaths.size() == 1
            ? QStringLiteral("Import %1?").arg(repoNameFromUrl(importPaths.first()))
            : QStringLiteral("Import %1 repositories?").arg(importPaths.size()));
    QString details = QStringLiteral(
        "Public repositories appear in the catalog. Private repositories are "
        "hidden from public browse and clone routes unless shared. Local-only "
        "repositories stay on this computer and appear under Private & local.\n\n"
        "Discovered:\n%1")
                          .arg(importPaths.mid(0, 12).replaceInStrings(
                                   QRegularExpression(QStringLiteral("^")),
                                   QStringLiteral("• "))
                                   .join(QLatin1Char('\n')));
    if (importPaths.size() > 12)
        details += QStringLiteral("\n…and %1 more").arg(importPaths.size() - 12);
    if (!skippedNames.isEmpty())
        details += QStringLiteral("\n\nSkipped %1 duplicate(s).")
                       .arg(skippedNames.size());
    visibility.setInformativeText(details);
    QPushButton *publicButton =
        visibility.addButton(QStringLiteral("Public"), QMessageBox::AcceptRole);
    QPushButton *privateButton =
        visibility.addButton(QStringLiteral("Private"), QMessageBox::AcceptRole);
    QPushButton *localOnlyButton = visibility.addButton(
        QStringLiteral("Local only"), QMessageBox::AcceptRole);
    visibility.addButton(QMessageBox::Cancel);
    visibility.setDefaultButton(publicButton);
    visibility.exec();
    if (visibility.clickedButton() != publicButton &&
        visibility.clickedButton() != privateButton &&
        visibility.clickedButton() != localOnlyButton)
        return;
    const bool isPrivate = visibility.clickedButton() == privateButton;
    const bool localOnly = visibility.clickedButton() == localOnlyButton;

    runOffThread<QStringList>(
        [importPaths] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("repositories"),
                QStringLiteral("read origins of %1 imported repositories")
                    .arg(importPaths.size()),
                forkmesh::ActionTelemetry::Execution::Worker);
            QStringList origins;
            for (const QString &path : importPaths) {
                QByteArray originOut;
                runGitCapture(path,
                              {QStringLiteral("remote"), QStringLiteral("get-url"),
                               QStringLiteral("origin")},
                              &originOut, nullptr);
                origins.append(QString::fromUtf8(originOut).trimmed());
            }
            return origins;
        },
        [this, importPaths, isPrivate, localOnly,
         openSingleImport](QStringList origins) {
            QList<int> addedIndexes;
            for (int i = 0; i < importPaths.size(); ++i) {
                const QString &path = importPaths.at(i);
                RepositoryRecord repo;
                repo.localPath = path;
                repo.name = repoNameFromUrl(path);
                repo.owner = accountOwner();
                repo.solanaAddress = savedSolanaAddress();
                repo.publishToNetwork = !localOnly;
                repo.isPrivate = isPrivate || localOnly;
                if (repoIndexFor(repo.owner, repo.name) >= 0)
                    continue;
                const QString originUrl = origins.value(i);
                const QUrl parsedOrigin(originUrl);
                const QStringList originParts = parsedOrigin.path().split(
                    QLatin1Char('/'), Qt::SkipEmptyParts);
                QString originName = originParts.isEmpty() ? QString() : originParts.last();
                if (originName.endsWith(QLatin1String(".git"), Qt::CaseInsensitive))
                    originName.chop(4);
                const QString originOwner =
                    originParts.size() >= 2 ? originParts.at(originParts.size() - 2)
                                            : QString();
                const QString originHost = parsedOrigin.host().toLower();
                const bool forkMeshOrigin = originHost == QLatin1String("forkmesh.com") ||
                                            originHost.endsWith(QLatin1String(".forkmesh.com"));
                if (forkMeshOrigin && !originOwner.isEmpty() && !originName.isEmpty() &&
                    originOwner.compare(repo.owner, Qt::CaseInsensitive) != 0) {
                    repo.cloneUrl = originUrl;
                    repo.forkedFrom =
                        (originOwner + QLatin1Char('/') + originName).toLower();
                }
                repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
                repo.mirrorPath = repositoryMirrorRoot() + "/" +
                                  repoSegment(repo.owner, QStringLiteral("owner")) + "-" +
                                  repoSegment(repo.name, QStringLiteral("repository")) + ".git";
                stampRepositoryRelay(repo);
                stampRepositorySyncIdentity(repo);
                m_repositories.append(repo);
                addedIndexes.append(m_repositories.size() - 1);
                if (m_backend)
                    m_backend->addChannel(repositoryChannel(repo));

                const QJsonObject metadata{
                    {"owner", repo.owner}, {"name", repo.name},
                    {"channel", repositoryChannel(repo)}, {"mirrorPath", repo.mirrorPath},
                    {"hostedSince", QString::number(repo.hostedSinceMs)},
                    {"visibility", localOnly ? QStringLiteral("local-only")
                                              : (repo.isPrivate ? QStringLiteral("private")
                                                                : QStringLiteral("public"))},
                    {"maintainer", m_profileIdentity.publicKey()}};
                logSystem("Repository: signed mirror metadata for " + repo.owner + "/" +
                          repo.name + " with signature " +
                          m_profileIdentity.signJson(metadata).left(16) + "...");
            }
            saveRepositories();
            refreshRepositoryList();
            for (const int index : std::as_const(addedIndexes))
                refreshRepositoryMirror(index);
            if (openSingleImport && addedIndexes.size() == 1)
                openRepoDetail(addedIndexes.first());
        });
}

void MainWindow::createNewRepository()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("newRepositoryDialog"));
    dialog.setWindowTitle(QStringLiteral("New repository"));

    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText(QStringLiteral("my-project"));

    auto *descriptionEdit = new QPlainTextEdit(&dialog);
    descriptionEdit->setPlaceholderText(
        QStringLiteral("What is this repository about?"));
    descriptionEdit->setMaximumHeight(72);

    auto *organizationCombo = new QComboBox(&dialog);
    organizationCombo->setObjectName(
        QStringLiteral("newRepositoryOrganization"));
    auto *organizationHint = new QLabel(&dialog);
    organizationHint->setObjectName(QStringLiteral("statusLine"));
    organizationHint->setAccessibleName(
        QStringLiteral("New repository organization status"));
    organizationHint->setWordWrap(true);
    auto *organizationLayout = new QVBoxLayout;
    organizationLayout->setContentsMargins(0, 0, 0, 0);
    organizationLayout->setSpacing(2);
    organizationLayout->addWidget(organizationCombo);
    organizationLayout->addWidget(organizationHint);
    auto *organizationWidget = new QWidget(&dialog);
    organizationWidget->setLayout(organizationLayout);
    populateNewRepositoryOrganizationChoices(organizationCombo,
                                             organizationHint);

    auto *promptEdit = new QPlainTextEdit(&dialog);
    promptEdit->setPlaceholderText(QStringLiteral(
        "Optional: the first thing you want done here. Filed as issue #1 so an "
        "agent can pick it up."));
    promptEdit->setMaximumHeight(96);

    auto *publicRadio = new QRadioButton(QStringLiteral("Public"), &dialog);
    auto *privateRadio = new QRadioButton(QStringLiteral("Private"), &dialog);
    publicRadio->setCursor(Qt::PointingHandCursor);
    privateRadio->setCursor(Qt::PointingHandCursor);
    publicRadio->setChecked(true);
    publicRadio->setToolTip(
        QStringLiteral("Listed in the catalog; anyone can browse and clone it."));
    privateRadio->setToolTip(
        QStringLiteral("Hidden from the public catalog and clone routes. Only "
                       "you and people you share it with can read it."));
    auto *visibilityHint = new QLabel(
        QStringLiteral("Public repos appear in the catalog. Private repos stay "
                       "hidden until you share them."),
        &dialog);
    visibilityHint->setObjectName(QStringLiteral("statusLine"));
    visibilityHint->setWordWrap(true);
    auto *visibilityRow = new QHBoxLayout;
    visibilityRow->setContentsMargins(0, 0, 0, 0);
    visibilityRow->addWidget(publicRadio);
    visibilityRow->addWidget(privateRadio);
    visibilityRow->addStretch(1);
    auto *visibilityBox = new QVBoxLayout;
    visibilityBox->setContentsMargins(0, 0, 0, 0);
    visibilityBox->setSpacing(2);
    visibilityBox->addLayout(visibilityRow);
    visibilityBox->addWidget(visibilityHint);
    auto *visibilityWidget = new QWidget(&dialog);
    visibilityWidget->setLayout(visibilityBox);

    auto *readmeBox =
        new QCheckBox(QStringLiteral("Add a README on the main branch"), &dialog);
    readmeBox->setChecked(true);

    auto *localOnlyBox = new QCheckBox(
        QString::fromUtf8("Local only \xE2\x80\x94 don't publish it to the network"),
        &dialog);
    localOnlyBox->setObjectName(QStringLiteral("newRepositoryLocalOnly"));
    localOnlyBox->setToolTip(
        QStringLiteral("Create the repository on this machine only. Nothing is "
                       "pushed to ForkMesh until you turn its \"Serve\" toggle "
                       "on from the Control Node page."));
    auto *localOnlyHint = new QLabel(
        QStringLiteral("Stays on this machine: no mirror is served and it never "
                       "appears in the catalog. Publish it later with the "
                       "\"Serve\" toggle on the Control Node page; the "
                       "visibility above applies from then on."),
        &dialog);
    localOnlyHint->setObjectName(QStringLiteral("statusLine"));
    localOnlyHint->setWordWrap(true);
    localOnlyHint->setVisible(false);
    connect(localOnlyBox, &QCheckBox::toggled, &dialog,
            [localOnlyHint, visibilityHint, organizationCombo,
             organizationHint](bool on) {
                localOnlyHint->setVisible(on);
                visibilityHint->setEnabled(!on);
                organizationCombo->setEnabled(!on);
                if (on) {
                    organizationCombo->setCurrentIndex(0);
                    organizationHint->setText(QStringLiteral(
                        "Organization URLs require a network-published "
                        "repository. Turn off Local only to add it now."));
                } else {
                    organizationHint->setText(
                        organizationCombo
                            ->property("forkmeshOrganizationHint")
                            .toString());
                }
            });

    auto *locationEdit = new QLineEdit(QDir::homePath(), &dialog);
    auto *browseButton = new QPushButton(QString::fromUtf8("Browse\xE2\x80\xA6"), &dialog);
    browseButton->setCursor(Qt::PointingHandCursor);
    connect(browseButton, &QPushButton::clicked, &dialog, [&dialog, locationEdit] {
        const QString picked = QFileDialog::getExistingDirectory(
            &dialog, QStringLiteral("Choose where to create the repository"),
            locationEdit->text().isEmpty() ? QDir::homePath()
                                           : locationEdit->text());
        if (!picked.isEmpty())
            locationEdit->setText(picked);
    });
    auto *locationRow = new QHBoxLayout;
    locationRow->setContentsMargins(0, 0, 0, 0);
    locationRow->addWidget(locationEdit, 1);
    locationRow->addWidget(browseButton);
    auto *locationWidget = new QWidget(&dialog);
    locationWidget->setLayout(locationRow);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Name"), nameEdit);
    form->addRow(QStringLiteral("Description"), descriptionEdit);
    form->addRow(QStringLiteral("Organization"), organizationWidget);
    form->addRow(QStringLiteral("First prompt"), promptEdit);
    form->addRow(QStringLiteral("Visibility"), visibilityWidget);
    form->addRow(QString(), readmeBox);
    form->addRow(QString(), localOnlyBox);
    form->addRow(QString(), localOnlyHint);
    form->addRow(QStringLiteral("Location"), locationWidget);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const QString rawName = nameEdit->text().trimmed();
        if (rawName.isEmpty()) {
            QMessageBox::warning(&dialog, "New repository",
                                 QStringLiteral("Enter a repository name."));
            return;
        }
        const QString name = repoSegment(rawName, QStringLiteral("repository"));
        if (repoIndexFor(accountOwner(), name) >= 0) {
            QMessageBox::information(&dialog, "New repository",
                                 QStringLiteral(
                                     "You already have a repository named \"%1\".")
                                     .arg(name));
            return;
        }
        const QString parent = locationEdit->text().trimmed();
        if (parent.isEmpty()) {
            QMessageBox::warning(&dialog, "New repository",
                                 QStringLiteral("Choose where to create it."));
            return;
        }
        const QString dest = QDir(parent).filePath(name);
        if (QDir(dest).exists() && !QDir(dest).isEmpty()) {
            QMessageBox::warning(
                &dialog, "New repository",
                QStringLiteral("%1 already exists and is not empty. Choose "
                               "another name or location.")
                    .arg(dest));
            return;
        }
        QString error;
        const QString organization =
            organizationCombo->currentData().toString().trimmed().toLower();
        const int index = provisionNewRepository(
            dest, name, descriptionEdit->toPlainText(), promptEdit->toPlainText(),
            readmeBox->isChecked(), privateRadio->isChecked(),
            localOnlyBox->isChecked(), &error);
        if (index < 0) {
            QMessageBox::warning(&dialog, "New repository",
                                 error.isEmpty()
                                     ? QStringLiteral("Could not create the "
                                                      "repository.")
                                     : error);
            return;
        }
        if (!organization.isEmpty()) {
            linkNewRepositoryToOrganization(
                organization, name, accountOwner().trimmed().toLower());
        }
        dialog.accept();
    });

    nameEdit->setFocus();
    dialog.exec();
}

// code-health: allow(blocking-wait) New repository dialog's Create click; git init/add/commit on a just-created directory holding at most README.md and info.json
int MainWindow::provisionNewRepository(const QString &dest, const QString &name,
                                       const QString &description,
                                       const QString &firstPrompt, bool addReadme,
                                       bool isPrivate, bool localOnly,
                                       QString *error)
{
    const auto fail = [&](const QString &message) -> int {
        if (error)
            *error = message;
        return -1;
    };

    if (!QDir(dest).exists() && !QDir().mkpath(dest))
        return fail(QStringLiteral("Could not create %1.").arg(dest));

    const auto runGit = [&dest](const QStringList &args, QString *errOut) -> bool {
        QProcess git;
        git.setWorkingDirectory(dest);
        git.start(QStringLiteral("git"), args);
        git.waitForFinished(30000);
        if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0) {
            if (errOut)
                *errOut =
                    QString::fromUtf8(git.readAllStandardError()).trimmed().right(200);
            return false;
        }
        return true;
    };

    QString gitErr;
    if (!runGit({QStringLiteral("init"), QStringLiteral("-b"),
                 QStringLiteral("main")},
                &gitErr)) {
        logSystem("New repository: git init failed in " + dest + ": " + gitErr);
        return fail(QStringLiteral("git init failed: %1").arg(gitErr));
    }

    const QString about = description.trimmed();
    const QDir repoDir(dest);
    bool seeded = false;

    if (addReadme) {
        QString body = QStringLiteral("# %1\n").arg(name);
        if (!about.isEmpty())
            body += QStringLiteral("\n%1\n").arg(about);
        QSaveFile readme(repoDir.filePath(QStringLiteral("README.md")));
        if (readme.open(QIODevice::WriteOnly)) {
            const QByteArray data = body.toUtf8();
            if (readme.write(data) == data.size() && readme.commit())
                seeded = true;
        }
        if (!seeded)
            logSystem("New repository: could not write README.md in " + dest);
    }

    if (!about.isEmpty() && repoDir.mkpath(QStringLiteral(".forkmesh"))) {
        QJsonObject info;
        info.insert(QStringLiteral("about"), about);
        const QByteArray data =
            QJsonDocument(info).toJson(QJsonDocument::Indented);
        QSaveFile file(repoDir.filePath(kRepoInfoPath));
        if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size() &&
            file.commit())
            seeded = true;
        else
            logSystem("New repository: could not write " + QString(kRepoInfoPath) +
                      " in " + dest);
    }

    if (seeded) {
        if (!runGit({QStringLiteral("add"), QStringLiteral("-A")}, &gitErr))
            logSystem("New repository: git add failed in " + dest + ": " + gitErr);
        const QStringList commitArgs{QStringLiteral("commit"), QStringLiteral("-m"),
                                     QStringLiteral("Initial commit")};
        if (!runGit(commitArgs, &gitErr)) {
            if (!promptAndSetGitIdentity(this, dest, accountOwner()) ||
                !runGit(commitArgs, &gitErr))
                logSystem("New repository: initial commit failed in " + dest +
                          ": " + gitErr);
        }
    }

    RepositoryRecord repo;
    repo.localPath = dest;
    repo.name = name;
    repo.owner = accountOwner();
    repo.description = about;
    repo.solanaAddress = savedSolanaAddress();
    repo.publishToNetwork = !localOnly;
    repo.isPrivate = isPrivate;
    repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
    repo.mirrorPath = repositoryMirrorRoot() + "/" +
                      repoSegment(repo.owner, QStringLiteral("owner")) + "-" +
                      repoSegment(repo.name, QStringLiteral("repository")) + ".git";

    stampRepositoryRelay(repo);
    stampRepositorySyncIdentity(repo);
    m_repositories.append(repo);
    const int index = m_repositories.size() - 1;
    saveRepositories();
    refreshRepositoryList();
    // The catalog cannot include this repository until mirror publication
    // succeeds. If the Repos page has already loaded, surface the durable local
    // record there immediately; otherwise a failed provisioning attempt leaves
    // a successfully created checkout looking as though it disappeared.
    if (m_networkRepoRowCount >= 0)
        renderLocalOnlyRepos();
    if (m_backend && !localOnly)
        m_backend->addChannel(repositoryChannel(repo));

    const QString prompt = firstPrompt.trimmed();
    if (!prompt.isEmpty()) {
        const QString title = prompt.section('\n', 0, 0).trimmed().left(120);
        const QString body = prompt == title ? QString() : prompt;
        QString issueErr;
        IssueStore store(repo.localPath, repo.mirrorPath, &m_profileIdentity,
                         chatDisplayName());
        if (store.createIssue(title, body, {}, QString(), 0, {}, {}, &issueErr) < 0)
            logSystem("New repository: could not file first-prompt issue for " +
                      repo.owner + "/" + repo.name + ": " + issueErr);
    }

    if (!localOnly)
        refreshRepositoryMirror(index);
    logSystem("New repository: created " +
              QString(localOnly ? "local-only "
                                : (repo.isPrivate ? "private " : "public ")) +
              repo.owner + "/" + repo.name + " in " + dest + ".");
    flashMessage(localOnly ? QStringLiteral("Created %1/%2 locally.")
                                 .arg(repo.owner, repo.name)
                           : QStringLiteral("Created %1/%2.")
                                 .arg(repo.owner, repo.name));
    return index;
}

QStringList MainWindow::importAuthGitArgs(const QString &url) const
{
    const QString host = QUrl(url).host().toLower();
    QString token, user;
    if (host.contains(QLatin1String("github"))) {
        token = QSettings().value(kGithubTokenSetting).toString().trimmed();
        user = QStringLiteral("x-access-token"); // GitHub PATs auth as this user
    } else if (host.contains(QLatin1String("gitlab"))) {
        token = QSettings().value(kGitlabTokenSetting).toString().trimmed();
        user = QStringLiteral("oauth2"); // GitLab PATs auth as oauth2:<token>
    }
    if (token.isEmpty())
        return {};
    // Carry the token in a one-shot Authorization header rather than baking it
    // into the cloned repo's origin URL, so the secret is never persisted on disk.
    const QByteArray basic = (user + ":" + token).toUtf8().toBase64();
    return {QStringLiteral("-c"),
            QStringLiteral("http.extraHeader=Authorization: Basic ") +
                QString::fromLatin1(basic)};
}

void MainWindow::setImportStatus(const QString &text, bool error)
{
    if (!m_importStatus)
        return;
    m_importStatus->setText(text);
    m_importStatus->setStyleSheet(Theme::remapSheet(error ? QStringLiteral("color:#f85149;")
                                        : QString()));
    m_importStatus->setVisible(!text.isEmpty());
}

void MainWindow::setImportControlsEnabled(bool enabled)
{
    if (m_importButton)
        m_importButton->setEnabled(enabled);
    if (m_importUrlEdit)
        m_importUrlEdit->setEnabled(enabled);
}

void MainWindow::importRemoteRepository()
{
    if (!m_importUrlEdit || !m_importButton)
        return;
    const QString url = m_importUrlEdit->text().trimmed();

    const QUrl parsed(url);
    if (url.isEmpty() || !parsed.isValid() ||
        (parsed.scheme() != QLatin1String("https") &&
         parsed.scheme() != QLatin1String("http"))) {
        setImportStatus(
            "Enter an https URL to a GitHub or GitLab repository, or a GitLab "
            "group to import the whole organization.", true);
        return;
    }

    const QString groupPath = gitlabGroupPathFor(parsed);
    if (!groupPath.isEmpty()) {
        probeGitlabGroup(url, groupPath);
        return;
    }
    importSingleRemoteRepository(url);
}

void MainWindow::importSingleRemoteRepository(const QString &url)
{
    if (!m_importUrlEdit || !m_importButton)
        return;
    auto setStatus = [this](const QString &text, bool error) {
        setImportStatus(text, error);
    };

    const QString name = repoNameFromUrl(url);
    if (repoIndexFor(accountOwner(), name) >= 0) {
        setStatus(QStringLiteral("You already have a repository named \"%1\".")
                      .arg(name),
                  true);
        return;
    }

    const QString parent = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose where to clone \"%1\"").arg(name),
        QDir::homePath());
    if (parent.isEmpty())
        return; // cancelled
    const QString dest = QDir(parent).filePath(name);
    if (QDir(dest).exists() && !QDir(dest).isEmpty()) {
        setStatus(QStringLiteral("%1 already exists and is not empty. Choose "
                                 "another location.")
                      .arg(dest),
                  true);
        return;
    }

    m_importButton->setEnabled(false);
    m_importUrlEdit->setEnabled(false);
    setStatus(QStringLiteral("Cloning %1 into %2…").arg(url, dest), false);
    logSystem("Import: cloning " + url + " into " + dest + ".");

    const QStringList args =
        importAuthGitArgs(url) + QStringList{"clone", url, dest};
    auto *process = new QProcess(this);
    process->setProgram(QStringLiteral("git"));
    process->setArguments(args);
    connect(process, &QProcess::finished, this,
            [this, process, dest, name, url, setStatus](int exitCode,
                                                        QProcess::ExitStatus) {
                const QString errors =
                    QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                if (m_importButton)
                    m_importButton->setEnabled(true);
                if (m_importUrlEdit)
                    m_importUrlEdit->setEnabled(true);

                if (exitCode != 0) {
                    const QString explained =
                        forkmesh::gitrefusal::explainFailure(errors);
                    setStatus(QStringLiteral("Clone failed: %1")
                                  .arg(explained.isEmpty() ? errors.right(200)
                                                           : explained),
                              true);
                    logSystem("Import: clone failed for " + url + ": " +
                              errors.right(300));
                    return;
                }

                RepositoryRecord repo;
                repo.localPath = dest;
                repo.name = name;
                repo.owner = accountOwner();
                repo.solanaAddress = savedSolanaAddress();
                repo.publishToNetwork = true;
                repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
                repo.mirrorPath =
                    repositoryMirrorRoot() + "/" +
                    repoSegment(repo.owner, QStringLiteral("owner")) + "-" +
                    repoSegment(repo.name, QStringLiteral("repository")) + ".git";

                stampRepositoryRelay(repo);
                stampRepositorySyncIdentity(repo);
                m_repositories.append(repo);
                saveRepositories();
                refreshRepositoryList();
                if (m_backend)
                    m_backend->addChannel(repositoryChannel(repo));
                const int index = m_repositories.size() - 1;
                refreshRepositoryMirror(index);

                setStatus(QStringLiteral("Imported %1/%2 — mirroring and "
                                         "publishing now.")
                              .arg(repo.owner, repo.name),
                          false);
                logSystem("Import: cloned " + url + " as " + repo.owner + "/" +
                          repo.name + ".");
                if (m_importUrlEdit)
                    m_importUrlEdit->clear();
            });
    trackProcessActivity(process, QStringLiteral("clone"),
                         QStringLiteral("Cloning %1").arg(url));
    process->start();
}

namespace {
bool safeGitlabSegment(const QString &segment)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9_.-]{1,100}$"));
    return pattern.match(segment).hasMatch();
}

bool reservedGitlabRoot(const QString &segment)
{
    static const QStringList reserved{
        QStringLiteral("explore"),   QStringLiteral("help"),
        QStringLiteral("dashboard"), QStringLiteral("projects"),
        QStringLiteral("groups"),    QStringLiteral("users"),
        QStringLiteral("admin"),     QStringLiteral("search"),
        QStringLiteral("api"),       QStringLiteral("-"),
    };
    return reserved.contains(segment, Qt::CaseInsensitive);
}
} // namespace

QString MainWindow::gitlabGroupPathFor(const QUrl &url) const
{
    if (url.scheme() != QLatin1String("https"))
        return {};
    const QString host = url.host().toLower();
    if (host != QLatin1String("gitlab.com") &&
        host != QLatin1String("www.gitlab.com"))
        return {};
    if (url.port(443) != 443 || url.hasQuery() || url.hasFragment() ||
        !url.userInfo().isEmpty())
        return {};
    QString path = url.path();
    if (path.endsWith(QLatin1String(".git")))
        path.chop(4);
    const QStringList parts =
        path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.size() > 20 ||
        reservedGitlabRoot(parts.first()))
        return {};
    for (const QString &part : parts) {
        if (!safeGitlabSegment(part))
            return {};
    }
    return parts.join(QLatin1Char('/'));
}

void MainWindow::probeGitlabGroup(const QString &url, const QString &groupPath)
{
    if (!m_networkAccess) {
        importSingleRemoteRepository(url);
        return;
    }
    setImportControlsEnabled(false);
    setImportStatus(QStringLiteral("Checking whether %1 is a GitLab group…")
                        .arg(groupPath),
                    false);

    QUrl api(QStringLiteral("https://gitlab.com/api/v4/groups/") +
             QString::fromUtf8(QUrl::toPercentEncoding(groupPath)));
    QNetworkRequest request(api);
    request.setTransferTimeout(15000);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    const QString token =
        QSettings().value(kGitlabTokenSetting).toString().trimmed();
    if (!token.isEmpty())
        request.setRawHeader(QByteArrayLiteral("PRIVATE-TOKEN"),
                             token.toUtf8());
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, groupPath] {
                reply->deleteLater();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const bool isGroup =
                    reply->error() == QNetworkReply::NoError && status == 200;
                if (!isGroup) {
                    setImportControlsEnabled(true);
                    setImportStatus(QString(), false);
                    importSingleRemoteRepository(url);
                    return;
                }
                logSystem("Import: " + groupPath +
                          " is a GitLab group; listing its projects.");
                m_gitlabGroupImport = GitlabGroupImport{};
                m_gitlabGroupImport.group = groupPath;
                fetchGitlabGroupProjects(groupPath, 1);
            });
}

void MainWindow::fetchGitlabGroupProjects(const QString &groupPath, int page)
{
    if (!m_networkAccess) {
        finishGitlabGroupImport();
        return;
    }
    constexpr int kPerPage = 100;
    constexpr int kMaxPages = 10;
    setImportStatus(
        QStringLiteral("Listing %1 — %2 projects found so far…")
            .arg(groupPath)
            .arg(m_gitlabGroupImport.cloneUrls.size()),
        false);

    QUrl api(QStringLiteral("https://gitlab.com/api/v4/groups/") +
             QString::fromUtf8(QUrl::toPercentEncoding(groupPath)) +
             QStringLiteral("/projects"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("include_subgroups"),
                       QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("archived"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("order_by"), QStringLiteral("path"));
    query.addQueryItem(QStringLiteral("sort"), QStringLiteral("asc"));
    query.addQueryItem(QStringLiteral("per_page"), QString::number(kPerPage));
    query.addQueryItem(QStringLiteral("page"), QString::number(page));
    api.setQuery(query);

    QNetworkRequest request(api);
    request.setTransferTimeout(20000);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    const QString token =
        QSettings().value(kGitlabTokenSetting).toString().trimmed();
    if (!token.isEmpty())
        request.setRawHeader(QByteArrayLiteral("PRIVATE-TOKEN"),
                             token.toUtf8());
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, groupPath, page] {
                reply->deleteLater();
                const QByteArray raw = reply->readAll();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                if (reply->error() != QNetworkReply::NoError || status != 200) {
                    if (m_gitlabGroupImport.cloneUrls.isEmpty()) {
                        setImportControlsEnabled(true);
                        setImportStatus(
                            QStringLiteral("Could not list %1 (HTTP %2). A "
                                           "private group needs a GitLab token "
                                           "in Settings.")
                                .arg(groupPath)
                                .arg(status),
                            true);
                        logSystem("Import: listing " + groupPath +
                                  " failed with HTTP " +
                                  QString::number(status) + ".");
                        return;
                    }
                    logSystem("Import: listing " + groupPath +
                              " stopped early at page " +
                              QString::number(page) + ".");
                    cloneNextGitlabGroupProject();
                    return;
                }
                const QJsonArray rows =
                    QJsonDocument::fromJson(raw).array();
                for (const QJsonValue &value : rows) {
                    const QJsonObject project = value.toObject();
                    const QString cloneUrl =
                        project.value(QStringLiteral("http_url_to_repo"))
                            .toString()
                            .trimmed();
                    if (cloneUrl.isEmpty() ||
                        !cloneUrl.startsWith(QLatin1String("https://")))
                        continue;
                    const QString fullPath =
                        project.value(QStringLiteral("path_with_namespace"))
                            .toString();
                    QString name = repoNameFromUrl(cloneUrl);
                    if (name.isEmpty())
                        continue;
                    const bool taken =
                        repoIndexFor(accountOwner(), name) >= 0 ||
                        m_gitlabGroupImport.names.contains(name);
                    if (taken && !fullPath.isEmpty()) {
                        QString flattened = fullPath;
                        flattened.replace(QLatin1Char('/'), QLatin1Char('-'));
                        name = repoSegment(flattened,
                                           QStringLiteral("repository"));
                    }
                    if (repoIndexFor(accountOwner(), name) >= 0 ||
                        m_gitlabGroupImport.names.contains(name)) {
                        logSystem("Import: skipping " + cloneUrl +
                                  " — a repository named \"" + name +
                                  "\" already exists.");
                        m_gitlabGroupImport.failed += 1;
                        continue;
                    }
                    m_gitlabGroupImport.cloneUrls.append(cloneUrl);
                    m_gitlabGroupImport.names.append(name);
                }
                if (rows.size() == kPerPage && page < kMaxPages) {
                    fetchGitlabGroupProjects(groupPath, page + 1);
                    return;
                }
                if (rows.size() == kPerPage)
                    logSystem("Import: " + groupPath + " has more than " +
                              QString::number(kPerPage * kMaxPages) +
                              " projects; importing the first pages only.");
                if (m_gitlabGroupImport.cloneUrls.isEmpty()) {
                    setImportControlsEnabled(true);
                    setImportStatus(
                        QStringLiteral("%1 has no new projects to import.")
                            .arg(groupPath),
                        true);
                    return;
                }
                const QString parent = QFileDialog::getExistingDirectory(
                    this,
                    QStringLiteral("Choose where to clone the %1 group (%2 "
                                   "repositories)")
                        .arg(groupPath)
                        .arg(m_gitlabGroupImport.cloneUrls.size()),
                    QDir::homePath());
                if (parent.isEmpty()) {
                    m_gitlabGroupImport = GitlabGroupImport{};
                    setImportControlsEnabled(true);
                    setImportStatus(QString(), false);
                    return;
                }
                m_gitlabGroupImport.parentDir = parent;
                m_gitlabGroupImport.total =
                    m_gitlabGroupImport.cloneUrls.size();
                logSystem("Import: cloning " +
                          QString::number(m_gitlabGroupImport.total) +
                          " projects from the GitLab group " + groupPath +
                          " into " + parent + ".");
                cloneNextGitlabGroupProject();
            });
}

void MainWindow::cloneNextGitlabGroupProject()
{
    if (m_gitlabGroupImport.cloneUrls.isEmpty() ||
        m_gitlabGroupImport.parentDir.isEmpty()) {
        finishGitlabGroupImport();
        return;
    }
    const QString url = m_gitlabGroupImport.cloneUrls.takeFirst();
    const QString name = m_gitlabGroupImport.names.takeFirst();
    const QString dest = QDir(m_gitlabGroupImport.parentDir).filePath(name);
    const int position =
        m_gitlabGroupImport.total - m_gitlabGroupImport.cloneUrls.size();
    setImportStatus(QStringLiteral("Cloning %1 (%2 of %3)…")
                        .arg(name)
                        .arg(position)
                        .arg(m_gitlabGroupImport.total),
                    false);

    if (QDir(dest).exists() && !QDir(dest).isEmpty()) {
        logSystem("Import: skipping " + url + " — " + dest +
                  " already exists and is not empty.");
        m_gitlabGroupImport.failed += 1;
        QTimer::singleShot(0, this,
                           [this] { cloneNextGitlabGroupProject(); });
        return;
    }

    const QStringList args =
        importAuthGitArgs(url) + QStringList{"clone", url, dest};
    auto *process = new QProcess(this);
    process->setProgram(QStringLiteral("git"));
    process->setArguments(args);
    connect(process, &QProcess::finished, this,
            [this, process, dest, name, url](int exitCode, QProcess::ExitStatus) {
                const QString errors =
                    QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                if (exitCode != 0) {
                    m_gitlabGroupImport.failed += 1;
                    logSystem("Import: clone failed for " + url + ": " +
                              errors.right(300));
                    cloneNextGitlabGroupProject();
                    return;
                }

                RepositoryRecord repo;
                repo.localPath = dest;
                repo.name = name;
                repo.owner = accountOwner();
                repo.solanaAddress = savedSolanaAddress();
                repo.publishToNetwork = true;
                repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
                repo.mirrorPath =
                    repositoryMirrorRoot() + "/" +
                    repoSegment(repo.owner, QStringLiteral("owner")) + "-" +
                    repoSegment(repo.name, QStringLiteral("repository")) + ".git";

                stampRepositoryRelay(repo);
                stampRepositorySyncIdentity(repo);
                m_repositories.append(repo);
                saveRepositories();
                refreshRepositoryList();
                if (m_backend)
                    m_backend->addChannel(repositoryChannel(repo));
                refreshRepositoryMirror(m_repositories.size() - 1);
                m_gitlabGroupImport.imported += 1;
                logSystem("Import: cloned " + url + " as " + repo.owner + "/" +
                          repo.name + ".");
                cloneNextGitlabGroupProject();
            });
    trackProcessActivity(process, QStringLiteral("clone"),
                         QStringLiteral("Cloning %1").arg(url));
    process->start();
}

void MainWindow::finishGitlabGroupImport()
{
    const GitlabGroupImport summary = m_gitlabGroupImport;
    m_gitlabGroupImport = GitlabGroupImport{};
    setImportControlsEnabled(true);
    if (summary.group.isEmpty())
        return;
    if (!summary.imported) {
        setImportStatus(QStringLiteral("No repositories were imported from %1.")
                            .arg(summary.group),
                        true);
    } else {
        setImportStatus(
            QStringLiteral("Imported %1 of %2 repositories from %3%4 — "
                           "mirroring and publishing now.")
                .arg(summary.imported)
                .arg(summary.total ? summary.total : summary.imported)
                .arg(summary.group,
                     summary.failed
                         ? QStringLiteral(" (%1 skipped)").arg(summary.failed)
                         : QString()),
            false);
        if (m_importUrlEdit)
            m_importUrlEdit->clear();
    }
    logSystem(QStringLiteral(
                  "Import: GitLab group %1 finished — %2 imported, %3 skipped.")
                  .arg(summary.group)
                  .arg(summary.imported)
                  .arg(summary.failed));
    if (!summary.imported)
        return;
    const QString message = QStringLiteral("Imported %1 repositories from %2.")
                                .arg(summary.imported)
                                .arg(summary.group);
    if (m_importStatus && m_importStatus->isVisible())
        fileMessage(message);
    else
        flashMessage(message);
}
