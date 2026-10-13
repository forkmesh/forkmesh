// The "files.browse." local control family (WS16): read-only browsing of the desktop's repositories and worktrees.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.
// Gate 2 answers "unsupported" for every method; WS16 fills it in.

#include "LocalControl.h"
#include "LocalControlFiles.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSharedPointer>
#include <QTimer>

namespace filesctl = forkmesh::localcontrol::filesbrowse;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

constexpr int kMaxRoots = 200;
constexpr int kMaxWorktreesPerRepo = 40;
constexpr int kMaxEntries = 500;
constexpr int kMaxStatusFiles = 500;
constexpr int kStatusTimeoutMs = 15000;
constexpr int kGitTimeoutMs = 120000;
constexpr qint64 kMaxGitOutputBytes = 4 * 1024 * 1024;

QString headBranch(const QString &repoPath)
{
    QFile head(QDir(repoPath).filePath(QStringLiteral(".git/HEAD")));
    if (!head.open(QIODevice::ReadOnly))
        return {};
    QString text = QString::fromUtf8(head.read(512)).trimmed();
    if (text.startsWith(QLatin1String("ref: refs/heads/")))
        return text.mid(16);
    return text.left(7);
}

QProcess *startGit(QObject *parent, const QString &workingDirectory, const QStringList &args)
{
    auto *process = new QProcess(parent);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(workingDirectory);
    process->start(QStringLiteral("git"), args);
    return process;
}

}

QJsonArray MainWindow::localControlFileRoots()
{
    QJsonArray roots;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (roots.size() >= kMaxRoots)
            break;
        if (repo.owner.isEmpty() || repo.name.isEmpty() || repo.localPath.isEmpty() ||
            !QFileInfo(repo.localPath).isDir())
            continue;
        const QString key = repo.owner + QLatin1Char('/') + repo.name;
        if (!filesctl::validRootId(key))
            continue;
        filesctl::Root root;
        root.id = key;
        root.label = key;
        root.kind = QStringLiteral("repo");
        root.branch = headBranch(repo.localPath);
        root.repo = key;
        roots.append(filesctl::rootRow(root));
        for (const filesctl::Root &worktree :
             filesctl::discoverWorktrees(key, repo.localPath, kMaxWorktreesPerRepo)) {
            if (roots.size() >= kMaxRoots)
                break;
            roots.append(filesctl::rootRow(worktree));
        }
    }
    return roots;
}

QString MainWindow::localControlFileRootPath(const QString &rootId, QString *label)
{
    if (!filesctl::validRootId(rootId))
        return {};
    const QString repoKey = rootId.section(QLatin1Char('#'), 0, 0);
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.localPath.isEmpty() ||
            (repo.owner + QLatin1Char('/') + repo.name).compare(repoKey, Qt::CaseInsensitive) != 0)
            continue;
        if (!rootId.contains(QLatin1Char('#'))) {
            if (label)
                *label = repoKey;
            return repo.localPath;
        }
        for (const filesctl::Root &worktree : filesctl::discoverWorktrees(
                 repo.owner + QLatin1Char('/') + repo.name, repo.localPath, kMaxWorktreesPerRepo)) {
            if (worktree.id.compare(rootId, Qt::CaseInsensitive) == 0) {
                if (label)
                    *label = worktree.label;
                return worktree.path;
            }
        }
        return {};
    }
    return {};
}

void MainWindow::handleLocalControlFilesRequest(int connectionId, int requestId,
                                                const QString &method,
                                                const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const QString rootId = params.value(QStringLiteral("root")).toString().trimmed();
    const QString relative = params.value(QStringLiteral("path")).toString();

    if (method == QLatin1String("files.browse.roots")) {
        server->respond(connectionId, requestId, {{QStringLiteral("roots"), localControlFileRoots()}});
        return;
    }
    QString label;
    const QString rootPath = localControlFileRootPath(rootId, &label);
    if (rootPath.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_root"));
        return;
    }
    if (method == QLatin1String("files.browse.list")) {
        QString error;
        const QString dir = filesctl::resolveInside(rootPath, relative, &error);
        if (dir.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        if (!QFileInfo(dir).isDir()) {
            server->respondError(connectionId, requestId, QStringLiteral("not_a_directory"));
            return;
        }
        bool ok = false;
        const QString clean = filesctl::normalizeRelativePath(relative, &ok);
        const filesctl::Listing listing = filesctl::listDirectory(rootPath, dir, kMaxEntries);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("root"), rootId},
                         {QStringLiteral("path"), clean},
                         {QStringLiteral("entries"), listing.entries},
                         {QStringLiteral("total"), listing.total},
                         {QStringLiteral("truncated"), listing.truncated}});
        return;
    }
    if (method == QLatin1String("files.browse.read")) {
        QString error;
        const QString file = filesctl::resolveInside(rootPath, relative, &error);
        if (file.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        const QFileInfo info(file);
        if (!info.isFile()) {
            server->respondError(connectionId, requestId, QStringLiteral("not_a_file"));
            return;
        }
        if (filesctl::isSensitiveName(info.fileName())) {
            server->respondError(connectionId, requestId, QStringLiteral("sensitive_file"));
            return;
        }
        const qint64 offset = qint64(params.value(QStringLiteral("offset")).toDouble(0));
        const qint64 length = qint64(
            params.value(QStringLiteral("length")).toDouble(double(filesctl::kMaxReadBytes)));
        QJsonObject slice = filesctl::readFileSlice(file, offset, length);
        if (slice.contains(QStringLiteral("error"))) {
            server->respondError(connectionId, requestId,
                                 slice.value(QStringLiteral("error")).toString());
            return;
        }
        bool ok = false;
        slice.insert(QStringLiteral("root"), rootId);
        slice.insert(QStringLiteral("path"), filesctl::normalizeRelativePath(relative, &ok));
        slice.insert(QStringLiteral("modifiedMs"), double(info.lastModified().toMSecsSinceEpoch()));
        server->respond(connectionId, requestId, slice);
        return;
    }
    if (method == QLatin1String("files.browse.status")) {
        QPointer<LocalControlServer> guard(server);
        QProcess *process = startGit(this, rootPath,
                                     {QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                                      QStringLiteral("-b"), QStringLiteral("-z"),
                                      QStringLiteral("--untracked-files=normal")});
        auto answered = QSharedPointer<bool>::create(false);
        connect(process, &QProcess::finished, this,
                [guard, process, connectionId, requestId, rootId, label, answered](
                    int exitCode, QProcess::ExitStatus status) {
                    process->deleteLater();
                    if (*answered || !guard)
                        return;
                    *answered = true;
                    const QByteArray output = process->readAllStandardOutput().left(
                        kMaxGitOutputBytes);
                    if (status != QProcess::NormalExit || exitCode != 0) {
                        guard->respondError(connectionId, requestId,
                                            QStringLiteral("git_status_failed"));
                        return;
                    }
                    QJsonObject result = filesctl::parsePorcelainStatus(output, kMaxStatusFiles);
                    result.insert(QStringLiteral("root"), rootId);
                    result.insert(QStringLiteral("label"), label);
                    guard->respond(connectionId, requestId, result);
                });
        connect(process, &QProcess::errorOccurred, this,
                [guard, process, connectionId, requestId, answered](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart || *answered)
                        return;
                    *answered = true;
                    process->deleteLater();
                    if (guard)
                        guard->respondError(connectionId, requestId, QStringLiteral("git_unavailable"));
                });
        QTimer::singleShot(kStatusTimeoutMs, process, [guard, process, connectionId, requestId,
                                                       answered] {
            if (process->state() == QProcess::NotRunning || *answered)
                return;
            *answered = true;
            process->kill();
            if (guard)
                guard->respondError(connectionId, requestId, QStringLiteral("timeout"));
        });
        return;
    }
    if (method == QLatin1String("files.browse.prepare")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        if (!filesctl::gitActions().contains(action)) {
            server->respondError(connectionId, requestId, QStringLiteral("unsupported_action"));
            return;
        }
        const QString token = m_filesControlConfirmations.issue(
            device.key, action, rootId, QDateTime::currentMSecsSinceEpoch());
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), rootId},
                         {QStringLiteral("summary"), filesctl::gitActionSummary(action, label)},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }
    if (method == QLatin1String("files.browse.run")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        const hostctl::ConfirmationStore::Result verdict = m_filesControlConfirmations.consume(
            params.value(QStringLiteral("token")).toString(), device.key, action, rootId,
            QDateTime::currentMSecsSinceEpoch());
        if (verdict != hostctl::ConfirmationStore::Result::Ok) {
            logSystem(QStringLiteral("Refused a git %1 of %2 from %3: %4.")
                          .arg(action, rootId, localControlDeviceLabel(connectionId),
                               hostctl::confirmationError(verdict)));
            server->respondError(connectionId, requestId, hostctl::confirmationError(verdict));
            return;
        }
        runLocalControlFilesGit(connectionId, requestId, action, rootId);
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

void MainWindow::runLocalControlFilesGit(int connectionId, int requestId, const QString &action,
                                         const QString &rootId)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    QString label;
    const QString rootPath = localControlFileRootPath(rootId, &label);
    const QStringList args = filesctl::gitArguments(action);
    if (rootPath.isEmpty() || args.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_root"));
        return;
    }
    const QString key = action + QLatin1Char(':') + rootId.toLower();
    if (m_filesGitRunning.contains(key)) {
        server->respondError(connectionId, requestId, QStringLiteral("already_running"));
        return;
    }
    m_filesGitRunning.insert(key);
    const QString who = localControlDeviceLabel(connectionId);
    logSystem(QStringLiteral("git %1 of %2 requested by %3.").arg(action, label, who));
    QProcess *process = startGit(this, rootPath, args);
    process->setProcessChannelMode(QProcess::MergedChannels);
    const auto finish = [this, key, action, rootId, label](bool ok, const QString &output) {
        if (!m_filesGitRunning.remove(key))
            return;
        const QString message =
            output.trimmed().isEmpty()
                ? (ok ? QStringLiteral("Done.") : QStringLiteral("git %1 failed.").arg(action))
                : output.trimmed().right(1500);
        logSystem(QStringLiteral("git %1 of %2 from the phone %3.")
                      .arg(action, label, ok ? QStringLiteral("finished") : QStringLiteral("failed")));
        if (!m_localControl)
            return;
        m_localControl->broadcast(QStringLiteral("files.browse.action"),
                                  {{QStringLiteral("action"), action},
                                   {QStringLiteral("root"), rootId},
                                   {QStringLiteral("ok"), ok},
                                   {QStringLiteral("message"), message}});
        m_localControl->broadcast(QStringLiteral("files.browse.changed"),
                                  {{QStringLiteral("root"), rootId}});
    };
    connect(process, &QProcess::finished, this,
            [process, finish](int exitCode, QProcess::ExitStatus status) {
                process->deleteLater();
                finish(status == QProcess::NormalExit && exitCode == 0,
                       QString::fromUtf8(process->readAll().left(kMaxGitOutputBytes)));
            });
    connect(process, &QProcess::errorOccurred, this,
            [process, finish](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                finish(false, QStringLiteral("git is not available on the desktop."));
            });
    QTimer::singleShot(kGitTimeoutMs, process, [process, finish] {
        if (process->state() == QProcess::NotRunning)
            return;
        process->kill();
        finish(false, QStringLiteral("git timed out."));
    });
    server->respond(connectionId, requestId,
                    {{QStringLiteral("started"), true},
                     {QStringLiteral("action"), action},
                     {QStringLiteral("subject"), rootId},
                     {QStringLiteral("message"),
                      action == QLatin1String("pull") ? QStringLiteral("Pulling %1.").arg(label)
                                                      : QStringLiteral("Fetching %1.").arg(label)}});
}
