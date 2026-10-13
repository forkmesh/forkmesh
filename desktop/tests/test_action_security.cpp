#include "ActionFile.h"
#include "ActionRunner.h"
#include "ActionStore.h"
#include "CrashHandler.h"
#include "SystemStats.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <csignal>
#include <cstdio>

#ifndef Q_OS_WIN
#include <sys/resource.h>
#endif

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("  ok  %s\n", message);
    } else {
        std::fprintf(stderr, "  FAIL  %s\n", message);
        ++failures;
    }
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

QByteArray git(const QString &repository, const QStringList &arguments,
               bool *ok = nullptr)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), repository} + arguments);
    const bool success =
        process.waitForStarted(3000) && process.waitForFinished(30000) &&
        process.exitStatus() == QProcess::NormalExit &&
        process.exitCode() == 0;
    if (ok)
        *ok = success;
    return process.readAllStandardOutput();
}

QString commitAll(const QString &repository, const QString &message)
{
    bool ok = false;
    git(repository,
        {QStringLiteral("add"), QStringLiteral("--all")}, &ok);
    if (!ok)
        return {};
    git(repository,
        {QStringLiteral("-c"),
         QStringLiteral("user.name=Action Security Test"),
         QStringLiteral("-c"),
         QStringLiteral("user.email=actions-test@forkmesh.local"),
         QStringLiteral("commit"), QStringLiteral("-m"), message},
        &ok);
    if (!ok)
        return {};
    return QString::fromLatin1(
               git(repository,
                   {QStringLiteral("rev-parse"), QStringLiteral("HEAD")},
                   &ok))
        .trimmed();
}

// A top-level package that only the node's per-user site directory provides —
// `python3 -s` (user site disabled) must not find it. That makes it a witness
// for the runner's site-packages mount: a step can only import it if the
// sandbox re-exposed the host interpreter's own packages.
QString userSiteOnlyPackage()
{
    const QString lib = QDir::homePath() + QStringLiteral("/.local/lib");
    const QStringList pythons =
        QDir(lib).entryList({QStringLiteral("python*")},
                            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &python : pythons) {
        const QString site =
            lib + QLatin1Char('/') + python + QStringLiteral("/site-packages");
        const QStringList packages =
            QDir(site).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &package : packages) {
            if (package.startsWith(QLatin1Char('_')) ||
                package.contains(QLatin1Char('.')) ||
                package.contains(QLatin1Char('-')))
                continue;
            if (!QFileInfo::exists(site + QLatin1Char('/') + package +
                                   QStringLiteral("/__init__.py")))
                continue;
            QProcess probe;
            probe.start(QStringLiteral("python3"),
                        {QStringLiteral("-s"), QStringLiteral("-c"),
                         QStringLiteral("import %1").arg(package)});
            if (!probe.waitForFinished(10000))
                continue;
            if (probe.exitCode() != 0)
                return package;
        }
    }
    return {};
}

struct RunResult {
    bool signalOk = false;
    bool finished = false;
    bool terminalRefreshCompleted = false;
    ActionRun persisted;
    QString log;
};

RunResult runWorkflow(const QString &repository, const QString &commit,
                      const QString &content, ActionStore *store,
                      int runId, const ActionSandboxLimits &limits,
                      bool reenterOnTerminalStatus = false,
                      const QMap<QString, QString> &variables = {},
                      const QString &repositoryName =
                          QStringLiteral("sandbox"))
{
    ActionRun run;
    run.id = runId;
    run.owner = QStringLiteral("security");
    run.name = repositoryName;
    run.workflowPath = QStringLiteral(".forkmesh/security.yml");
    run.workflowName = QStringLiteral("Security test");
    run.workflowContent = content;
    run.commit = commit;
    run.ref = QStringLiteral("refs/heads/main");
    QString digestError;
    check(ActionStore::repositoryStateDigest(
              repository, commit, &run.repositoryTree,
              &run.executionDigest, &digestError),
          "test workflow repository snapshot can be hashed");

    const ActionWorkflow workflow =
        ActionFile::parse(run.workflowPath, content);
    check(workflow.valid, "test workflow parses");

    ActionRunner runner(store);
    runner.setSandboxLimitsForTesting(limits);
    QEventLoop loop;
    RunResult result;
    QObject::connect(
        &runner, &ActionRunner::statusChanged, &loop,
        [&](int id, const QString &status) {
            if (!reenterOnTerminalStatus || id != runId ||
                status == ActionStatus::Running)
                return;
            // Pull-check refreshes use a nested event loop for their Git probe.
            // Exercise that same re-entrancy while the finished QProcess is
            // pending deleteLater(); its destructor may dispatch final socket
            // notifications before the outer event loop resumes.
            QEventLoop refresh;
            QTimer::singleShot(0, &refresh, &QEventLoop::quit);
            refresh.exec();
            result.terminalRefreshCompleted = true;
        });
    QObject::connect(
        &runner, &ActionRunner::finished, &loop,
        [&](int id, bool ok) {
            if (id == runId) {
                result.finished = true;
                result.signalOk = ok;
                loop.quit();
            }
        });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        runner.stop();
        loop.quit();
    });
    guard.start(60000);
    runner.start(run, workflow, repository, QString(), variables);
    if (!result.finished)
        loop.exec();
    guard.stop();
    const QList<ActionRun> runs = store->loadAllRuns();
    for (const ActionRun &saved : runs)
        if (saved.id == runId)
            result.persisted = saved;
    result.log = store->readLog(run);
    return result;
}

bool destroyWorkflowDuringCheckout(const QString &repository,
                                   const QString &commit,
                                   const QString &content,
                                   ActionStore *store, int runId,
                                   const ActionSandboxLimits &limits)
{
    ActionRun run;
    run.id = runId;
    run.owner = QStringLiteral("security");
    run.name = QStringLiteral("destructor");
    run.workflowPath = QStringLiteral(".forkmesh/security.yml");
    run.workflowName = QStringLiteral("Destructor test");
    run.workflowContent = content;
    run.commit = commit;
    run.ref = QStringLiteral("refs/heads/main");
    QString digestError;
    if (!ActionStore::repositoryStateDigest(
            repository, commit, &run.repositoryTree,
            &run.executionDigest, &digestError))
        return false;
    const ActionWorkflow workflow =
        ActionFile::parse(run.workflowPath, content);
    if (!workflow.valid)
        return false;

    ActionRunner runner(store);
    runner.setSandboxLimitsForTesting(limits);
    runner.start(run, workflow, repository, QString(),
                 QMap<QString, QString>{});
    // Returning destroys the runner immediately while its asynchronous checkout
    // QProcess is still Starting/Running. The destructor must disconnect, stop,
    // reap, and delete that child without dispatching an output callback.
    return runner.busy();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("ActionSecurity-%1")
            .arg(QCoreApplication::applicationPid()));
    QSettings::setDefaultFormat(QSettings::IniFormat);

    QTemporaryDir temp;
    check(temp.isValid(), "temporary test directory is available");
    if (!temp.isValid())
        return 1;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       temp.filePath(QStringLiteral("settings")));
    QSettings().clear();

    check(ActionRunner::checkoutLostWorktreeRace(QStringLiteral(
              "Preparing worktree (detached HEAD 825125e4bf)\n"
              "fatal: failed to read worktrees/source/commondir: Success\n")),
          "checkout retries when a concurrent add leaves commondir half written");
    check(ActionRunner::checkoutLostWorktreeRace(QStringLiteral(
              "Preparing worktree (detached HEAD ab3f672f3)\n"
              "fatal: could not open 'worktrees/source1/locked' for writing: "
              "No such file or directory\n")),
          "checkout retries when a concurrent prune removes the new admin folder");
    check(!ActionRunner::checkoutLostWorktreeRace(QStringLiteral(
              "fatal: invalid reference: 0123456789abcdef\n")),
          "checkout does not retry a commit the mirror lacks");
    check(!ActionRunner::checkoutLostWorktreeRace(QStringLiteral(
              "fatal: '/sandbox/source' already exists\n")),
          "checkout does not retry an occupied destination");

    // Pre-v2 storage replaced every non [A-Za-z0-9.-] character with "_".
    // These two distinct repositories therefore shared both run and approval
    // storage. The v2 key must split their records without copying that
    // ambiguous trust decision to either repository.
    ActionRun collisionA;
    collisionA.id = 11;
    collisionA.owner = QStringLiteral("team/a");
    collisionA.name = QStringLiteral("repo");
    collisionA.workflowPath = QStringLiteral(".forkmesh/collision.yml");
    collisionA.workflowContent = QStringLiteral("name: collision");
    collisionA.repositoryTree = QStringLiteral("tree-a");
    collisionA.executionDigest = QStringLiteral("digest-a");
    ActionRun collisionB = collisionA;
    collisionB.id = 12;
    collisionB.owner = QStringLiteral("team_a");
    ActionRun legacySolo = collisionA;
    legacySolo.id = 13;
    legacySolo.owner = QStringLiteral("legacy/team");
    legacySolo.name = QStringLiteral("solo");
    legacySolo.workflowPath = QStringLiteral(".forkmesh/solo.yml");
    legacySolo.workflowContent = QStringLiteral("name: solo");
    legacySolo.repositoryTree = QStringLiteral("tree-solo");
    legacySolo.executionDigest = QStringLiteral("digest-solo");

    check(collisionA.legacyRepoKey() == collisionB.legacyRepoKey(),
          "legacy repository key fixture reproduces the lossy collision");
    check(collisionA.repoKey() != collisionB.repoKey() &&
              QRegularExpression(QStringLiteral("^v2-[a-f0-9]{64}$"))
                  .match(collisionA.repoKey())
                  .hasMatch(),
          "length-delimited SHA-256 repository keys are path safe and collision resistant");

    const QString actionsRoot = temp.filePath(QStringLiteral("actions"));
    auto writeLegacyRun = [&](const ActionRun &run, const QByteArray &log) {
        const QString directory =
            actionsRoot + QStringLiteral("/runs/") + run.legacyRepoKey() +
            QLatin1Char('/') + QString::number(run.id);
        return QDir().mkpath(directory) &&
               writeFile(directory + QStringLiteral("/meta.json"),
                         QJsonDocument(run.toJson())
                             .toJson(QJsonDocument::Compact)) &&
               writeFile(directory + QStringLiteral("/log.txt"), log);
    };
    check(writeLegacyRun(collisionA, QByteArray("collision A log\n")) &&
              writeLegacyRun(collisionB, QByteArray("collision B log\n")) &&
              writeLegacyRun(legacySolo, QByteArray("solo log\n")),
          "legacy run fixtures are persisted under lossy keys");
    const QString legacyArtifact =
        actionsRoot + QStringLiteral("/artifacts/") +
        legacySolo.legacyRepoKey() + QStringLiteral("/13");
    check(QDir().mkpath(legacyArtifact) &&
              writeFile(legacyArtifact + QStringLiteral("/result.txt"),
                        QByteArray("artifact\n")),
          "legacy artifact fixture is persisted");

    auto approvalDocument = [](const ActionRun &run) {
        QJsonObject approvals;
        approvals.insert(
            run.workflowPath,
            QJsonObject{
                {QStringLiteral("schema"), 2},
                {QStringLiteral("workflowContent"), run.workflowContent},
                {QStringLiteral("repositoryTree"), run.repositoryTree},
                {QStringLiteral("executionDigest"), run.executionDigest},
            });
        return QJsonDocument(approvals).toJson(QJsonDocument::Compact);
    };
    QSettings migrationSettings;
    migrationSettings.setValue(
        QStringLiteral("actions/approved/") + collisionA.legacyRepoKey(),
        approvalDocument(collisionA));
    migrationSettings.setValue(
        QStringLiteral("actions/approved/") + legacySolo.legacyRepoKey(),
        approvalDocument(legacySolo));
    migrationSettings.sync();

    ActionStore migrationStore(actionsRoot);
    // Legacy migration is deliberately lazy so a desktop start that never
    // opens Actions does not scan every historical run. The first history
    // read is the public trigger and must complete migration atomically before
    // exposing any record.
    migrationStore.loadAllRuns();
    check(QFileInfo::exists(
              actionsRoot + QStringLiteral("/runs/") +
              collisionA.repoKey() + QStringLiteral("/11/meta.json")) &&
              QFileInfo::exists(
                  actionsRoot + QStringLiteral("/runs/") +
                  collisionB.repoKey() + QStringLiteral("/12/meta.json")) &&
              migrationStore.readLog(collisionA) ==
                  QStringLiteral("collision A log\n") &&
              migrationStore.readLog(collisionB) ==
                  QStringLiteral("collision B log\n"),
          "legacy colliding runs migrate into separate canonical directories");
    check(QFileInfo::exists(
              actionsRoot + QStringLiteral("/artifacts/") +
              legacySolo.repoKey() + QStringLiteral("/13/result.txt")),
          "legacy run artifacts migrate with their exact repository and run");
    check(!ActionStore::isApproved(
              collisionA.repoKey(), collisionA.workflowPath,
              collisionA.workflowContent, collisionA.repositoryTree,
              collisionA.executionDigest) &&
              !ActionStore::isApproved(
                  collisionB.repoKey(), collisionB.workflowPath,
                  collisionB.workflowContent, collisionB.repositoryTree,
                  collisionB.executionDigest),
          "ambiguous legacy approval collision fails closed for both repositories");
    check(!ActionStore::isApproved(
              legacySolo.repoKey(), legacySolo.workflowPath,
              legacySolo.workflowContent, legacySolo.repositoryTree,
              legacySolo.executionDigest) &&
              ActionStore::lastApprovedContent(
                  legacySolo.repoKey(), legacySolo.workflowPath) ==
                  legacySolo.workflowContent &&
              !QSettings().contains(
                  QStringLiteral("actions/approved/") +
                  legacySolo.legacyRepoKey()),
          "unambiguous legacy approval migrates as review-only content and fails closed");

    // Status signals can arrive several times per process transition. They used
    // to make MainWindow call loadAllRuns(), which reparsed every meta.json each
    // time, while createRun() separately walked the same tree for nextId(). The
    // store now owns a write-through index, so both paths share one initial scan.
    {
        QTemporaryDir cacheTemp;
        check(cacheTemp.isValid(), "action run cache temp dir is valid");
        const QString cacheRoot =
            cacheTemp.filePath(QStringLiteral("actions"));
        const QString orphanRun =
            cacheRoot + QStringLiteral("/runs/orphan/97");
        check(QDir().mkpath(orphanRun) &&
                  writeFile(orphanRun + QStringLiteral("/meta.json"),
                            QByteArray("{not valid json")),
              "malformed high-id run fixture is staged");
        ActionStore cacheStore(cacheRoot);
        ActionRun first;
        first.owner = QStringLiteral("cache");
        first.name = QStringLiteral("one");
        first.workflowPath = QStringLiteral(".forkmesh/cache.yml");
        first = cacheStore.createRun(first);
        check(first.id == 98 &&
                  cacheStore.runDirectoryScanCountForTesting() == 1,
              "the first scan reserves malformed numeric run directories and "
              "initializes the disk index once");

        first.status = ActionStatus::Running;
        check(cacheStore.saveRun(first), "cached run status update persists");
        bool cachedStatusVisible = true;
        for (int i = 0; i < 20; ++i) {
            const QList<ActionRun> loaded = cacheStore.loadAllRuns();
            cachedStatusVisible =
                cachedStatusVisible && loaded.size() == 1 &&
                loaded.first().status == ActionStatus::Running;
        }
        check(cachedStatusVisible &&
                  cacheStore.runDirectoryScanCountForTesting() == 1,
              "repeated status refreshes use the write-through index without "
              "rescanning run directories");

        ActionRun second = first;
        second.name = QStringLiteral("two");
        second = cacheStore.createRun(second);
        check(second.id == first.id + 1 &&
                  cacheStore.runDirectoryScanCountForTesting() == 1,
              "nextId allocation reuses the initialized index");
        check(cacheStore.deleteRun(second), "cached run deletion succeeds");
        ActionRun third = first;
        third.name = QStringLiteral("three");
        third = cacheStore.createRun(third);
        check(third.id == second.id + 1,
              "deleting a run does not recycle its id within the process");
    }

    const QString repository = temp.filePath(QStringLiteral("repository"));
    QDir().mkpath(repository + QStringLiteral("/.forkmesh"));
    bool gitOk = false;
    git(repository, {QStringLiteral("init"), QStringLiteral("-q")}, &gitOk);
    check(gitOk, "test Git repository initializes");

    const QString workflowPath =
        repository + QStringLiteral("/.forkmesh/security.yml");
    const QString stableWorkflow =
        QStringLiteral("name: Bound approval\non: push\nsteps:\n"
                       "  - name: helper\n"
                       "    run: ./ci.sh\n");
    check(writeFile(workflowPath, stableWorkflow.toUtf8()),
          "workflow fixture is written");
    check(writeFile(repository + QStringLiteral("/ci.sh"),
                    QByteArray("#!/bin/sh\necho safe\n")),
          "helper fixture is written");
    QFile::setPermissions(repository + QStringLiteral("/ci.sh"),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner);
    const QString firstCommit =
        commitAll(repository, QStringLiteral("initial workflow"));
    check(!firstCommit.isEmpty(), "initial fixture commit is created");

    QString firstTree;
    QString firstDigest;
    QString digestError;
    check(ActionStore::repositoryStateDigest(
              repository, firstCommit, &firstTree, &firstDigest,
              &digestError),
          "initial repository execution state is hashed");
    const QString approvalScope = QStringLiteral("security-sandbox");
    ActionStore::approve(approvalScope, QStringLiteral(".forkmesh/security.yml"),
                         stableWorkflow, firstTree, firstDigest);
    check(ActionStore::isApproved(
              approvalScope, QStringLiteral(".forkmesh/security.yml"),
              stableWorkflow, firstTree, firstDigest),
          "exact workflow and repository state is approved");
    QThreadPool::globalInstance()->waitForDone();
    {
        const QJsonObject persisted =
            QJsonDocument::fromJson(
                QSettings()
                    .value(QStringLiteral("actions/approved/") + approvalScope)
                    .toByteArray())
                .object()
                .value(QStringLiteral(".forkmesh/security.yml"))
                .toObject();
        check(persisted.value(QStringLiteral("executionDigest")).toString() ==
                      firstDigest &&
                  ActionStore::isApproved(
                      approvalScope, QStringLiteral(".forkmesh/security.yml"),
                      stableWorkflow, firstTree, firstDigest),
              "an approval given on the GUI thread is saved by a worker and "
              "still reads back once saved");
    }

    check(writeFile(repository + QStringLiteral("/ci.sh"),
                    QByteArray("#!/bin/sh\necho changed\n")),
          "helper changes without changing workflow YAML");
    QFile::setPermissions(repository + QStringLiteral("/ci.sh"),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner);
    const QString secondCommit =
        commitAll(repository, QStringLiteral("change helper only"));
    QString secondTree;
    QString secondDigest;
    check(ActionStore::repositoryStateDigest(
              repository, secondCommit, &secondTree, &secondDigest,
              &digestError),
          "changed repository execution state is hashed");
    check(firstDigest != secondDigest && firstTree != secondTree,
          "changing only ci.sh changes the bound execution state");
    check(!ActionStore::isApproved(
              approvalScope, QStringLiteral(".forkmesh/security.yml"),
              stableWorkflow, secondTree, secondDigest),
          "changed ci.sh cannot reuse workflow approval or receive secrets");

    {
        ActionStore spoolStore(temp.filePath(QStringLiteral("spool-actions")));
        const QString spoolCommit =
            QStringLiteral("0123456789abcdef0123456789abcdef01234567");
        check(spoolStore.spoolPushEvent(QStringLiteral("forkmesh"),
                                        QStringLiteral("forkmesh"),
                                        spoolCommit.toUpper(),
                                        QStringLiteral("refs/heads/main")),
              "a push the node could not bind is saved to the spool");
        const QStringList spooled =
            QDir(spoolStore.spoolDir())
                .entryList({QStringLiteral("*.push")}, QDir::Files);
        QString spooledText;
        if (spooled.size() == 1) {
            QFile spooledFile(
                QDir(spoolStore.spoolDir()).filePath(spooled.first()));
            if (spooledFile.open(QIODevice::ReadOnly))
                spooledText = QString::fromUtf8(spooledFile.readAll());
        }
        check(spooledText ==
                  QStringLiteral("owner forkmesh\nname forkmesh\nref %1 %2 "
                                 "refs/heads/main\n")
                      .arg(QString(40, QLatin1Char('0')), spoolCommit),
              "the spooled push reads back in the post-receive hook's format");
        check(!spoolStore.spoolPushEvent(QStringLiteral("forkmesh"),
                                         QStringLiteral("forkmesh"),
                                         QStringLiteral("not-a-commit"),
                                         QStringLiteral("refs/heads/main")) &&
                  !spoolStore.spoolPushEvent(QStringLiteral("forkmesh"),
                                             QStringLiteral("forkmesh"),
                                             spoolCommit,
                                             QStringLiteral("refs/pr/1/head")) &&
                  !spoolStore.spoolPushEvent(QStringLiteral("forkmesh\nname x"),
                                             QStringLiteral("forkmesh"),
                                             spoolCommit,
                                             QStringLiteral("refs/heads/main")),
              "the spool refuses a malformed commit, a non-branch ref and a "
              "field that would inject a line");
    }

    {
        ActionStore rangeStore(
            temp.filePath(QStringLiteral("spool-range-actions")));
        const QString afterCommit =
            QStringLiteral("0123456789abcdef0123456789abcdef01234567");
        const QString beforeCommit =
            QStringLiteral("89abcdef0123456789abcdef0123456789abcdef");
        check(rangeStore.spoolPushEvent(QStringLiteral("forkmesh"),
                                        QStringLiteral("forkmesh"),
                                        afterCommit,
                                        QStringLiteral("refs/heads/main"),
                                        beforeCommit.toUpper()),
              "a push with a known old value is saved to the spool");
        const QStringList spooled =
            QDir(rangeStore.spoolDir())
                .entryList({QStringLiteral("*.push")}, QDir::Files);
        QString spooledText;
        if (spooled.size() == 1) {
            QFile spooledFile(
                QDir(rangeStore.spoolDir()).filePath(spooled.first()));
            if (spooledFile.open(QIODevice::ReadOnly))
                spooledText = QString::fromUtf8(spooledFile.readAll());
        }
        check(spooledText ==
                  QStringLiteral("owner forkmesh\nname forkmesh\nref %1 %2 "
                                 "refs/heads/main\n")
                      .arg(beforeCommit, afterCommit),
              "the spooled push keeps the old value so the replay diffs the "
              "whole pushed range");
        ActionStore junkStore(
            temp.filePath(QStringLiteral("spool-junk-before-actions")));
        check(junkStore.spoolPushEvent(QStringLiteral("forkmesh"),
                                       QStringLiteral("forkmesh"), afterCommit,
                                       QStringLiteral("refs/heads/main"),
                                       QStringLiteral("x y\nref")),
              "a malformed old value still spools the push");
        const QStringList junk =
            QDir(junkStore.spoolDir())
                .entryList({QStringLiteral("*.push")}, QDir::Files);
        QString junkText;
        if (junk.size() == 1) {
            QFile junkFile(QDir(junkStore.spoolDir()).filePath(junk.first()));
            if (junkFile.open(QIODevice::ReadOnly))
                junkText = QString::fromUtf8(junkFile.readAll());
        }
        check(junkText ==
                  QStringLiteral("owner forkmesh\nname forkmesh\nref %1 %2 "
                                 "refs/heads/main\n")
                      .arg(QString(40, QLatin1Char('0')), afterCommit),
              "a malformed old value is spooled as unknown, never as a line");
    }
    const QString retiredPath = QStringLiteral(".forkmesh/deploy.yml");
    check(ActionFile::defaultBranchRetiresPush(
              retiredPath,
              QStringLiteral("name: Deploy Cloudflare Worker\n"
                             "on: [workflow_dispatch]\nsteps:\n"
                             "  - name: refuse\n"
                             "    run: exit 1\n")),
          "a workflow the default branch made manual-only is retired for "
          "pushes to other branches");
    check(!ActionFile::defaultBranchRetiresPush(
              retiredPath,
              QStringLiteral("name: Deploy\non: [push, workflow_dispatch]\n"
                             "steps:\n  - run: echo ok\n")),
          "a workflow the default branch still runs on push is not retired");
    check(!ActionFile::defaultBranchRetiresPush(retiredPath, QString()),
          "a workflow the default branch does not have is not retired");
    check(!ActionFile::defaultBranchRetiresPush(
              retiredPath, QStringLiteral("not: [a, workflow")),
          "an unreadable default-branch copy does not retire the workflow");

    QString sandboxReason;
    if (!ActionRunner::sandboxAvailable(&sandboxReason)) {
        std::printf("  SKIP sandbox integration: %s\n",
                    sandboxReason.toUtf8().constData());
    } else {
        const QString hostSecret =
            temp.filePath(QStringLiteral("host-secret.txt"));
        check(writeFile(hostSecret, QByteArray("must-not-be-readable")),
              "host secret fixture is written");
        qputenv("FORKMESH_HOST_ONLY_SECRET", "must-not-be-inherited");

        const QString hostileWorkflow = QStringLiteral(
            "name: Hostile boundary checks\non: push\nsteps:\n"
            "  - name: escape egress secret and process limits\n"
            "    run: |\n"
            "      set -eu\n"
            "      [ \"$(id -u)\" = \"65534\" ]\n"
            "      [ ! -e '%1' ]\n"
            "      [ ! -e /root ]\n"
            "      [ -z \"${FORKMESH_HOST_ONLY_SECRET:-}\" ]\n"
            "      if touch /source/escape 2>/dev/null; then exit 41; fi\n"
            "      printf isolated > /workspace/sandbox-marker\n"
            "      python3 - <<'PY'\n"
            "      import os\n"
            "      import socket\n"
            "      import time\n"
            "      children = []\n"
            "      for _ in range(64):\n"
            "          try:\n"
            "              pid = os.fork()\n"
            "          except OSError:\n"
            "              break\n"
            "          if pid == 0:\n"
            "              time.sleep(0.2)\n"
            "              os._exit(0)\n"
            "          children.append(pid)\n"
            "      assert len(children) < 64\n"
            "      for pid in children:\n"
            "          os.waitpid(pid, 0)\n"
            "      s = socket.socket()\n"
            "      s.settimeout(0.3)\n"
            "      assert s.connect_ex(('1.1.1.1', 53)) != 0\n"
            "      PY\n")
                                            .arg(hostSecret);
        check(writeFile(workflowPath, hostileWorkflow.toUtf8()),
              "hostile workflow fixture is written");
        const QString hostileCommit =
            commitAll(repository, QStringLiteral("hostile boundary workflow"));
        ActionStore store(temp.filePath(QStringLiteral("actions")));
        ActionSandboxLimits limits;
        limits.maxMemoryBytes = 512LL * 1024 * 1024;
        limits.maxScopeMemoryBytes = 512LL * 1024 * 1024;
        limits.maxFileBytes = 8LL * 1024 * 1024;
        limits.maxWorkspaceBytes = 128LL * 1024 * 1024;
        limits.maxProcesses = 32;
        limits.maxOpenFiles = 64;
        limits.maxCpuSeconds = 30;
        limits.stepTimeoutMs = 15000;
        limits.jobTimeoutMs = 45000;
        const RunResult hostile =
            runWorkflow(repository, hostileCommit, hostileWorkflow,
                        &store, 101, limits);
        check(hostile.finished && hostile.signalOk &&
                  hostile.persisted.status == ActionStatus::Success,
              "hostile escape, egress, fork ceiling, and secret-read checks "
              "remain contained");
        check(!QFile::exists(repository + QStringLiteral("/escape")),
              "read-only source mount was not modified");
        // The sandbox hands git neither a system nor a global config, so
        // without an explicit pin every workspace clone is born on master and
        // prints the init.defaultBranch advice block into the run log.
        check(!hostile.log.contains(QStringLiteral("init.defaultBranch")),
              "the workspace clone defaults to main and prints no "
              "default-branch advice");

        // A queued PR run stays bound to its approved commit even if the PR
        // ref moves before its sandbox starts. The commit remains readable in
        // the mirror long enough for `git worktree add <commit>`, but an
        // ordinary clone of that mirror does not transfer unreachable objects.
        // The runner must clone the exact detached source worktree it just
        // created, whose HEAD advertises the approved commit.
        const QString detachedWorkflow = QStringLiteral(
            "name: Detached approval\non: push\nsteps:\n"
            "  - name: approved commit remains cloneable\n"
            "    run: echo \"detached commit ok\"\n");
        check(writeFile(workflowPath, detachedWorkflow.toUtf8()),
              "detached-approval workflow fixture is written");
        const QString detachedCommit =
            commitAll(repository, QStringLiteral("detached approval workflow"));
        git(repository,
            {QStringLiteral("reset"), QStringLiteral("--hard"), hostileCommit},
            &gitOk);
        check(gitOk && !detachedCommit.isEmpty(),
              "approved commit becomes unreferenced before its run starts");
        const RunResult detached =
            runWorkflow(repository, detachedCommit, detachedWorkflow,
                        &store, 113, limits);
        check(detached.finished && detached.signalOk &&
                  detached.persisted.status == ActionStatus::Success &&
                  detached.log.contains(QStringLiteral("detached commit ok")),
              "an approved detached commit is cloned from its stable source "
              "worktree after the mutable ref moves");

        const QString noCheckoutWorkflow = QStringLiteral(
            "name: No checkout\non: push\ncheckout: false\nsteps:\n"
            "  - name: nothing is checked out\n"
            "    run: |\n"
            "      test -z \"$(ls -A /workspace)\"\n"
            "      test -z \"$(ls -A /source)\"\n"
            "      echo \"commit=$FORKMESH_COMMIT ref=$FORKMESH_REF "
            "run=$FORKMESH_RUN_ID\"\n");
        check(writeFile(workflowPath, noCheckoutWorkflow.toUtf8()),
              "no-checkout workflow fixture is written");
        const QString noCheckoutCommit =
            commitAll(repository, QStringLiteral("no-checkout workflow"));
        const RunResult noCheckout =
            runWorkflow(repository, noCheckoutCommit, noCheckoutWorkflow,
                        &store, 119, limits);
        check(noCheckout.finished && noCheckout.signalOk &&
                  noCheckout.persisted.status == ActionStatus::Success &&
                  noCheckout.log.contains(
                      QStringLiteral("commit=%1 ref=refs/heads/main run=119")
                          .arg(noCheckoutCommit)) &&
                  !noCheckout.log.contains(
                      QStringLiteral("disposable writable clone")),
              "checkout: false runs the steps in an empty workspace with the "
              "commit in FORKMESH_COMMIT and the run in FORKMESH_RUN_ID");

        const QString timeoutWorkflow = QStringLiteral(
            "name: Timeout\non: push\nsteps:\n"
            "  - name: never finishes\n"
            "    run: sleep 30\n");
        check(writeFile(workflowPath, timeoutWorkflow.toUtf8()),
              "timeout workflow fixture is written");
        const QString timeoutCommit =
            commitAll(repository, QStringLiteral("timeout workflow"));
        ActionSandboxLimits timeoutLimits = limits;
        timeoutLimits.stepTimeoutMs = 350;
        const RunResult timed =
            runWorkflow(repository, timeoutCommit, timeoutWorkflow,
                        &store, 102, timeoutLimits);
        check(timed.finished && !timed.signalOk &&
                  timed.persisted.status == ActionStatus::Failed &&
                  timed.log.contains(QStringLiteral("time limit")),
              "stale workflow processes are killed at the configured deadline");

        const QString diskWorkflow = QStringLiteral(
            "name: Disk quota\non: push\nsteps:\n"
            "  - name: oversized file\n"
            "    run: dd if=/dev/zero of=too-large bs=1M count=4 status=none\n");
        check(writeFile(workflowPath, diskWorkflow.toUtf8()),
              "disk-limit workflow fixture is written");
        const QString diskCommit =
            commitAll(repository, QStringLiteral("disk workflow"));
        ActionSandboxLimits diskLimits = limits;
        diskLimits.maxFileBytes = 1024 * 1024;
        const RunResult disk =
            runWorkflow(repository, diskCommit, diskWorkflow,
                        &store, 103, diskLimits);
        check(disk.finished && !disk.signalOk &&
                  disk.persisted.status == ActionStatus::Failed,
              "oversized workflow files are stopped by RLIMIT_FSIZE");

#ifndef Q_OS_WIN
        // JVM build tools keep every classpath jar open. At the old 256 cap
        // the Gradle 9.3.1 daemon (~308 distribution jars) died with "Unable
        // to create daemon log file (Too many open files)", so every Android
        // build workflow failed before compiling anything (runs #8259, #8267).
        {
            struct rlimit hostFiles {};
            const rlim_t wanted = 4096;
            if (::getrlimit(RLIMIT_NOFILE, &hostFiles) != 0 ||
                (hostFiles.rlim_max != RLIM_INFINITY &&
                 hostFiles.rlim_max < wanted)) {
                std::printf("  SKIP open-file ceiling: node hard limit is "
                            "below %lu\n", static_cast<unsigned long>(wanted));
            } else {
                const QString fdWorkflow = QStringLiteral(
                    "name: Open files\non: push\nsteps:\n"
                    "  - name: hold more descriptors than a Gradle daemon\n"
                    "    run: |\n"
                    "      python3 - <<'PY'\n"
                    "      import os\n"
                    "      fds = [os.open('/dev/null', os.O_RDONLY) for _ in range(%1)]\n"
                    "      print('held', len(fds))\n"
                    "      PY\n")
                                                 .arg(int(wanted) - 64);
                check(writeFile(workflowPath, fdWorkflow.toUtf8()),
                      "open-file workflow fixture is written");
                const QString fdCommit =
                    commitAll(repository, QStringLiteral("open-file workflow"));
                ActionSandboxLimits fdLimits = limits;
                fdLimits.maxOpenFiles = ActionSandboxLimits().maxOpenFiles;
                const RunResult fdRun =
                    runWorkflow(repository, fdCommit, fdWorkflow,
                                &store, 118, fdLimits);
                check(fdRun.finished && fdRun.signalOk &&
                          fdRun.persisted.status == ActionStatus::Success,
                      "the default open-file ceiling fits a JVM build tool's "
                      "classpath");
            }
        }
#endif

        const QString outputWorkflow = QStringLiteral(
            "name: Output quota\non: push\nsteps:\n"
            "  - name: noisy\n"
            "    run: python3 -c \"import sys; sys.stdout.write('x' * 2097152)\"\n");
        check(writeFile(workflowPath, outputWorkflow.toUtf8()),
              "output-limit workflow fixture is written");
        const QString outputCommit =
            commitAll(repository, QStringLiteral("output workflow"));
        const RunResult noisy =
            runWorkflow(repository, outputCommit, outputWorkflow,
                        &store, 104, limits);
        check(noisy.finished && noisy.signalOk &&
                  noisy.persisted.status == ActionStatus::Success,
              "large output does not wedge the isolated runner");
        check(noisy.log.toUtf8().size() < 1400 * 1024 &&
                  noisy.log.contains(
                      QStringLiteral("Action output exceeded")),
              "workflow output is bounded and reports truncation");

        // Exposed variable values are masked out of the log by exact substring
        // replacement. A value too short to be a credential must not take part:
        // with SSH_GATEWAY_PORT=22 exposed, the deploy workflow's "no Node >= v22"
        // warning was logged (and reported in the crash diagnostic) as
        // "no Node >= v***", hiding the real problem behind a non-secret.
        const QString redactionWorkflow = QStringLiteral(
            "name: Secret redaction\non: push\nsteps:\n"
            "  - name: log a short and a long variable\n"
            "    run: |\n"
            "      set -eu\n"
            "      echo \"no Node >= v${{ vars.SHORT_PORT }} found\"\n"
            "      echo \"token=${{ vars.LONG_TOKEN }}\"\n");
        check(writeFile(workflowPath, redactionWorkflow.toUtf8()),
              "redaction workflow fixture is written");
        const QString redactionCommit =
            commitAll(repository, QStringLiteral("redaction workflow"));
        const RunResult redacted = runWorkflow(
            repository, redactionCommit, redactionWorkflow, &store, 111,
            limits, false,
            QMap<QString, QString>{
                {QStringLiteral("SHORT_PORT"), QStringLiteral("22")},
                {QStringLiteral("LONG_TOKEN"),
                 QStringLiteral("super-secret-token-value")}});
        check(redacted.finished && redacted.signalOk &&
                  redacted.persisted.status == ActionStatus::Success,
              "variable substitution workflow runs to completion");
        check(!redacted.log.contains(QStringLiteral("super-secret-token-value")) &&
                  redacted.log.contains(QStringLiteral("token=***")),
              "credential-length variable values are masked out of the log");
        check(redacted.log.contains(QStringLiteral("no Node >= v22 found")),
              "values too short to be credentials stay readable in the log");

        const QString failingSecretWorkflow = QStringLiteral(
            "name: Secret in a failing command\non: push\nsteps:\n"
            "  - name: fail after using a credential\n"
            "    run: |\n"
            "      token=\"${{ vars.LONG_TOKEN }}\"\n"
            "      exit 3\n");
        check(writeFile(workflowPath, failingSecretWorkflow.toUtf8()),
              "failing credential workflow fixture is written");
        const QString failingSecretCommit =
            commitAll(repository, QStringLiteral("failing credential workflow"));
        static QString capturedDiagnostics;
        capturedDiagnostics.clear();
        const QtMessageHandler previousHandler = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext &, const QString &message) {
                capturedDiagnostics += message;
            });
        const RunResult failedSecret = runWorkflow(
            repository, failingSecretCommit, failingSecretWorkflow, &store, 150,
            limits, false,
            QMap<QString, QString>{
                {QStringLiteral("LONG_TOKEN"),
                 QStringLiteral("super-secret-token-value")}});
        qInstallMessageHandler(previousHandler);
        check(failedSecret.finished && !failedSecret.signalOk &&
                  capturedDiagnostics.contains(
                      QStringLiteral("Action failed before UI refresh")) &&
                  capturedDiagnostics.contains(QStringLiteral("token=\"***\"")) &&
                  !capturedDiagnostics.contains(
                      QStringLiteral("super-secret-token-value")),
              "the failure diagnostic masks credentials in the step command");

        const QString tornRef =
            repository + QStringLiteral("/.git/refs/heads/torn-write");
        check(writeFile(workflowPath, stableWorkflow.toUtf8()) &&
                  writeFile(repository + QStringLiteral("/ci.sh"),
                            QByteArray("#!/bin/sh\necho torn-ref-safe\n")),
              "torn-ref workflow fixture is written");
        QFile::setPermissions(repository + QStringLiteral("/ci.sh"),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner);
        const QString tornRefCommit =
            commitAll(repository, QStringLiteral("torn ref workflow"));
        check(writeFile(tornRef, QByteArray()),
              "an empty loose ref stands in for a torn ref write");
        const RunResult tornRefRun = runWorkflow(
            repository, tornRefCommit, stableWorkflow, &store, 151, limits);
        QFile::remove(tornRef);
        check(tornRefRun.finished && tornRefRun.signalOk &&
                  tornRefRun.log.contains(QStringLiteral("torn-ref-safe")),
              "a broken ref elsewhere in the repository does not stop the "
              "workspace clone");

        // A step cannot read its own cgroup (no /sys in the namespace), so the
        // runner publishes the CPU and memory budget it enforces. Builds size
        // -j from it instead of from the host's core count (adhoc #329).
        const QString budgetWorkflow = QStringLiteral(
            "name: Build budget\non: push\nsteps:\n"
            "  - name: sandbox publishes its own cpu and memory budget\n"
            "    run: |\n"
            "      set -eu\n"
            "      [ \"${FORKMESH_ACTIONS_MEMORY_MB:-0}\" = \"512\" ]\n"
            "      [ \"${FORKMESH_ACTIONS_CPUS:-0}\" -ge 2 ]\n"
            "      echo \"budget ok\"\n");
        check(writeFile(workflowPath, budgetWorkflow.toUtf8()),
              "build-budget workflow fixture is written");
        const QString budgetCommit =
            commitAll(repository, QStringLiteral("budget workflow"));
        const RunResult budget =
            runWorkflow(repository, budgetCommit, budgetWorkflow,
                        &store, 105, limits);
        check(budget.finished && budget.signalOk &&
                  budget.persisted.status == ActionStatus::Success &&
                  budget.log.contains(QStringLiteral("budget ok")),
              "steps see the sandbox CPU and memory budget they must build "
              "within");

        // ...and those two budgets must stay consistent with each other. A
        // step sizes -j from FORKMESH_ACTIONS_CPUS, so a memory ceiling that
        // does not grow with the CPU quota turns extra cores into an OOM kill:
        // the flat 4 GiB default ran this repository's own Qt build at -j8 on a
        // 36-core node and systemd stopped the scope mid-compile (adhoc #1582).
        ActionSandboxLimits productionLimits;
        const qint64 derivedScope = actionScopeMemoryBytes(productionLimits);
        const qint64 derivedAddressSpace =
            actionAddressSpaceBytes(productionLimits);
        const qint64 hostBytes = SystemStats::totalMemoryBytes();
        const int grantedCpus = qMax(1, actionCpuQuotaPercent() / 100);
        const qint64 perCpuBytes = 3LL * 1024 * 1024 * 1024 / 2;
        check(derivedScope >= productionLimits.maxMemoryBytes,
              "the sandbox memory budget is never below its configured "
              "physical-memory floor");
        check(derivedAddressSpace >= 64LL * 1024 * 1024 * 1024 &&
                  derivedAddressSpace >= derivedScope,
              "the sparse virtual-address allowance covers Node WebAssembly "
              "without weakening the scope's physical-memory ceiling");
        check(derivedScope >= qMin(qint64(grantedCpus) * perCpuBytes,
                                   hostBytes > 0 ? hostBytes / 2
                                                 : qint64(grantedCpus) *
                                                       perCpuBytes),
              "the sandbox memory budget scales with the CPU quota it grants "
              "the same step");
        // On a host too small for even one process's cap the floor wins; that
        // is the only case allowed past half the host's RAM.
        check(hostBytes <= 0 ||
                  derivedScope <= qMax(hostBytes / 2,
                                       productionLimits.maxMemoryBytes),
              "the sandbox memory budget never claims more than half the "
              "host's RAM");
        ActionSandboxLimits pinnedLimits;
        pinnedLimits.maxMemoryBytes = 256LL * 1024 * 1024;
        pinnedLimits.maxScopeMemoryBytes = 768LL * 1024 * 1024;
        check(actionScopeMemoryBytes(pinnedLimits) == 768LL * 1024 * 1024,
              "an explicitly configured memory budget overrides the "
              "CPU-derived one");

        // The same argument applies to TasksMax, which counts threads: a step
        // runs one tool per granted CPU and each tool sizes its own thread pool
        // from the host's cores, because the quota is invisible inside the
        // sandbox. The flat 128 killed the Qt build at -j8 with
        // pthread_create's EAGAIN once make had six AUTOMOC drivers up at once
        // (adhoc #1586).
        const int derivedTasks = actionScopeTasksMax(productionLimits);
        const int hostThreads = qMax(1, QThread::idealThreadCount());
        check(derivedTasks >= grantedCpus * hostThreads,
              "the sandbox task ceiling covers one host-sized thread pool per "
              "job the CPU quota grants");
        check(derivedTasks <= 4096,
              "the sandbox task ceiling stays bounded against a runaway fork "
              "loop");
        ActionSandboxLimits pinnedTasks;
        pinnedTasks.maxProcesses = 32;
        check(actionScopeTasksMax(pinnedTasks) == 32,
              "an explicitly configured task ceiling overrides the CPU-derived "
              "one");

        // /usr/bin/awk, cc, c++ and friends are symlinks into
        // /etc/alternatives on Debian-family hosts. When that farm is outside
        // the mount namespace they dangle, and a step using one dies with exit
        // 127 — which is how the v0.7.10 release publish failed after a full
        // desktop build.
        if (QStandardPaths::findExecutable(QStringLiteral("awk")).isEmpty()) {
            std::printf("  SKIP alternatives resolution: no awk on this host\n");
        } else {
            const QString toolWorkflow = QStringLiteral(
                "name: Toolchain\non: push\nsteps:\n"
                "  - name: alternatives-managed commands resolve\n"
                "    run: |\n"
                "      set -eu\n"
                "      [ \"$(printf 'x y\\n' | awk '{print $2}')\" = \"y\" ]\n"
                "      echo \"alternatives ok\"\n");
            check(writeFile(workflowPath, toolWorkflow.toUtf8()),
                  "alternatives workflow fixture is written");
            const QString toolCommit =
                commitAll(repository, QStringLiteral("alternatives workflow"));
            const RunResult tools =
                runWorkflow(repository, toolCommit, toolWorkflow,
                            &store, 106, limits);
            check(tools.finished && tools.signalOk &&
                      tools.persisted.status == ActionStatus::Success &&
                      tools.log.contains(QStringLiteral("alternatives ok")),
                  "commands resolved through /etc/alternatives run inside the "
                  "sandbox");
        }

        // Release packagers are too large and fast-moving to bake into the
        // application. The node owner installs them into a dedicated Actions
        // directory, which must be visible in the disposable sandbox home but
        // remain read-only to the workflow that consumes them.
        const QString nodeToolchain = store.toolchainBinDir();
        bool nodeToolsWritten = true;
        for (const QString &tool : {QStringLiteral("linuxdeploy"),
                                    QStringLiteral("linuxdeploy-plugin-qt"),
                                    QStringLiteral("appimagetool")}) {
            const QString path = QDir(nodeToolchain).filePath(tool);
            nodeToolsWritten =
                writeFile(path,
                          QStringLiteral("#!/bin/sh\nprintf '%1 ready\\n'\n")
                              .arg(tool)
                              .toUtf8()) &&
                nodeToolsWritten;
            QFile::setPermissions(
                path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                          QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                          QFileDevice::ExeGroup | QFileDevice::ReadOther |
                          QFileDevice::ExeOther);
        }
        check(nodeToolsWritten,
              "node-owned Actions toolchain fixtures are installed");
        const QString nodeToolWorkflow = QStringLiteral(
            "name: Node toolchain\non: push\nsteps:\n"
            "  - name: offline node tools are read-only and on PATH\n"
            "    run: |\n"
            "      set -eu\n"
            "      for tool in linuxdeploy linuxdeploy-plugin-qt appimagetool; do\n"
            "        command -v \"$tool\" >/dev/null\n"
            "        \"$tool\" | grep -q \"$tool ready\"\n"
            "      done\n"
            "      if touch /home/forkmesh/.local/bin/linuxdeploy 2>/dev/null; then exit 42; fi\n"
            "      echo \"node toolchain ok\"\n");
        check(writeFile(workflowPath, nodeToolWorkflow.toUtf8()),
              "node-toolchain workflow fixture is written");
        const QString nodeToolCommit =
            commitAll(repository, QStringLiteral("node toolchain workflow"));
        const RunResult nodeTools =
            runWorkflow(repository, nodeToolCommit, nodeToolWorkflow,
                        &store, 112, limits);
        check(nodeTools.finished && nodeTools.signalOk &&
                  nodeTools.persisted.status == ActionStatus::Success &&
                  nodeTools.log.contains(QStringLiteral("node toolchain ok")),
              "offline steps can execute but cannot modify node-owned tools");

        // Egress is denied, so a step imports only what the runner mounts.
        // System packages arrive with /usr, but pip's per-user site directory
        // lives in the host home — without it an offline suite fails with
        // "Runner image is missing pytest" on a healthy tree.
        const QString userPackage = userSiteOnlyPackage();
        if (userPackage.isEmpty()) {
            std::printf("  SKIP node python packages: none are user-site only\n");
        } else {
            const QString pythonWorkflow =
                QStringLiteral(
                    "name: Node python\non: push\nsteps:\n"
                    "  - name: offline step imports the node's own packages\n"
                    "    run: |\n"
                    "      set -eu\n"
                    "      python3 -c 'import %1'\n"
                    "      [ ! -e /home/forkmesh/.local/share ]\n"
                    "      echo \"python ok\"\n")
                    .arg(userPackage);
            check(writeFile(workflowPath, pythonWorkflow.toUtf8()),
                  "node-python workflow fixture is written");
            const QString pythonCommit =
                commitAll(repository, QStringLiteral("node python workflow"));
            const RunResult python =
                runWorkflow(repository, pythonCommit, pythonWorkflow,
                            &store, 107, limits);
            check(python.finished && python.signalOk &&
                      python.persisted.status == ActionStatus::Success &&
                      python.log.contains(QStringLiteral("python ok")),
                  "offline steps import the node interpreter's packages while "
                  "the rest of the host home stays out of the namespace");
        }

        // Disposable trees are disk-backed beside the run's own records, not on
        // the host's shared tmpfs: a step that fills that tmpfs corrupts other
        // checkouts and can fail silently mid-build. An app-data directory does
        // not self-clean, so a dead run's leftovers are swept on the next start
        // while a live sibling run's tree is left alone.
        const QDir sandboxBase(store.sandboxDir());
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QString staleTree =
            sandboxBase.absoluteFilePath(QStringLiteral("forkmesh-run-901-%1")
                                             .arg(nowMs - 3 * 24 * 3600 * 1000LL));
        const QString liveTree =
            sandboxBase.absoluteFilePath(
                QStringLiteral("forkmesh-run-902-%1").arg(nowMs));
        // A tree only an hour old used to be kept for twice the job timeout —
        // eight hours by default — so a node that dropped a few of them filled
        // its disk and every later run died inside `git clone` with "No space
        // left on device". Age is not what makes one safe to delete: a live
        // run's claim is, and nothing claims this one.
        const QString abandonedTree =
            sandboxBase.absoluteFilePath(
                QStringLiteral("forkmesh-run-903-%1")
                    .arg(nowMs - 60 * 60 * 1000LL));
        check(QDir().mkpath(staleTree + QStringLiteral("/workspace")) &&
                  QDir().mkpath(abandonedTree + QStringLiteral("/workspace")) &&
                  QDir().mkpath(liveTree + QStringLiteral("/workspace")),
              "abandoned and in-flight sandbox trees are staged");

        const QString sweepWorkflow = QStringLiteral(
            "name: Sandbox home\non: push\nsteps:\n"
            "  - name: disposable tree location\n"
            "    run: echo \"sandbox ok\"\n");
        check(writeFile(workflowPath, sweepWorkflow.toUtf8()),
              "sandbox-location workflow fixture is written");
        const QString sweepCommit =
            commitAll(repository, QStringLiteral("sandbox location workflow"));
        const RunResult sweep =
            runWorkflow(repository, sweepCommit, sweepWorkflow,
                        &store, 108, limits);
        check(sweep.finished && sweep.signalOk &&
                  sweep.persisted.status == ActionStatus::Success,
              "a run whose sandbox lives under the Actions store succeeds");
        check(!QDir(staleTree).exists(),
              "the abandoned sandbox tree is swept before the next run");
        check(!QDir(abandonedTree).exists(),
              "a tree no run claims is reclaimed without waiting out a job "
              "timeout");
        check(QDir(liveTree).exists(),
              "a concurrent run's sandbox tree is left alone");
        check(sandboxBase.entryList({QStringLiteral("forkmesh-run-108-*")},
                                    QDir::Dirs | QDir::NoDotAndDotDot)
                  .isEmpty(),
              "a finished run removes its own disposable tree");

        // Every other directory in the sandbox is created fresh for the run,
        // which made every C++ build start with no objects and a cold ccache.
        // One cache mount per repository survives instead, and the step's own
        // environment points ccache into it — so the next run of the *same*
        // repository reuses those objects while another repository, and the
        // discarded sandbox itself, see nothing.
        const QString cacheWorkflow = QStringLiteral(
            "name: Kept cache\non: push\nsteps:\n"
            "  - name: reuse what the previous run compiled\n"
            "    run: |\n"
            "      set -eu\n"
            "      [ \"$FORKMESH_ACTIONS_CACHE\" = /cache ]\n"
            "      [ \"$CCACHE_DIR\" = /cache/ccache ]\n"
            "      [ -d \"$CCACHE_DIR\" ]\n"
            "      [ -n \"${CCACHE_MAXSIZE:-}\" ]\n"
            "      if [ -f /cache/objects/witness ]; then\n"
            "        printf 'kept cache %s\\n' \"$(cat /cache/objects/witness)\"\n"
            "      else\n"
            "        printf 'kept cache cold\\n'\n"
            "      fi\n"
            "      mkdir -p /cache/objects\n"
            "      printf warm > /cache/objects/witness\n");
        check(writeFile(workflowPath, cacheWorkflow.toUtf8()),
              "kept-cache workflow fixture is written");
        const QString cacheCommit =
            commitAll(repository, QStringLiteral("kept cache workflow"));
        const RunResult coldRun =
            runWorkflow(repository, cacheCommit, cacheWorkflow,
                        &store, 113, limits);
        check(coldRun.finished && coldRun.signalOk &&
                  coldRun.log.contains(QStringLiteral("kept cache cold")),
              "the first run of a repository gets an empty writable /cache "
              "with ccache already pointed at it");
        const RunResult warmRun =
            runWorkflow(repository, cacheCommit, cacheWorkflow,
                        &store, 114, limits);
        check(warmRun.finished && warmRun.signalOk &&
                  warmRun.log.contains(QStringLiteral("kept cache warm")),
              "what a run leaves in /cache is still there for the next run of "
              "the same repository");

        ActionRun cacheScope;
        cacheScope.owner = QStringLiteral("security");
        cacheScope.name = QStringLiteral("sandbox");
        const QString witness = store.buildCacheDir(cacheScope) +
                                QStringLiteral("/objects/witness");
        check(QFileInfo::exists(witness),
              "the kept cache lives outside the sandbox the run discards");

        const RunResult otherRepository =
            runWorkflow(repository, cacheCommit, cacheWorkflow, &store, 115,
                        limits, false, QMap<QString, QString>{},
                        QStringLiteral("other"));
        check(otherRepository.finished && otherRepository.signalOk &&
                  otherRepository.log.contains(
                      QStringLiteral("kept cache cold")),
              "one repository's cache is never mounted for another");

        const RunResult disabledCache = runWorkflow(
            repository, cacheCommit, cacheWorkflow, &store, 116, limits, false,
            QMap<QString, QString>{
                {QStringLiteral("FORKMESH_ACTIONS_DISABLE_CACHE"),
                 QStringLiteral("1")}});
        check(disabledCache.finished && !disabledCache.signalOk &&
                  disabledCache.log.contains(
                      QStringLiteral("Persistent build cache disabled")),
              "a node can turn the surviving mount off, and then no step has "
              "one");

        // Nothing else prunes this directory, so the node keeps a ceiling on
        // it: a repository whose cache outgrows the budget starts over rather
        // than filling the disk. The walk and the wipe run on the worker that
        // discards the sandbox, so wait for it.
        ActionSandboxLimits tinyCache = limits;
        tinyCache.maxCacheBytes = 1;
        const RunResult overBudget =
            runWorkflow(repository, cacheCommit, cacheWorkflow,
                        &store, 117, tinyCache);
        check(overBudget.finished && overBudget.signalOk,
              "a run whose cache is over budget still succeeds");
        bool cacheReclaimed = false;
        for (int attempt = 0; attempt < 200 && !cacheReclaimed; ++attempt) {
            cacheReclaimed = !QFileInfo::exists(witness);
            if (!cacheReclaimed)
                QThread::msleep(50);
        }
        check(cacheReclaimed,
              "a cache that outgrew its ceiling is reclaimed after the run");

        const QString reentrantWorkflow = QStringLiteral(
            "name: Reentrant completion\non: push\nsteps:\n"
            "  - name: failing merge check\n"
            "    run: printf 'final process output\\n'; exit 9\n");
        check(writeFile(workflowPath, reentrantWorkflow.toUtf8()),
              "reentrant completion workflow fixture is written");
        const QString reentrantCommit =
            commitAll(repository, QStringLiteral("reentrant completion workflow"));
        const RunResult reentrant =
            runWorkflow(repository, reentrantCommit, reentrantWorkflow,
                        &store, 109, limits, true);
        check(reentrant.finished && !reentrant.signalOk &&
                  reentrant.persisted.status == ActionStatus::Failed &&
                  reentrant.terminalRefreshCompleted &&
                  reentrant.log.contains(QStringLiteral("final process output")),
              "terminal pull-check refresh cannot outlive the finished process");
        check(destroyWorkflowDuringCheckout(
                  repository, reentrantCommit, reentrantWorkflow,
                  &store, 110, limits),
              "destroying a runner safely reaps an in-flight checkout process");
        // A node restarted or killed mid-run destroys its runners without ever
        // reaching cleanupWorktree(), so the tree survives the process. The
        // destructor drops the claim, which is what lets the next sweep take
        // the disk back instead of stranding a checkout, a clone and a build
        // tree — the leak that filled this node and broke every later run.
        check(!sandboxBase
                   .entryList({QStringLiteral("forkmesh-run-110-*")},
                              QDir::Dirs | QDir::NoDotAndDotDot)
                   .isEmpty(),
              "a destroyed runner leaves its tree behind for the sweeper");
        const ActionSandboxSweep reclaimed =
            ActionRunner::reclaimSandboxes(store.sandboxDir(), 0);
        check(reclaimed.removed >= 1 && reclaimed.freeBytes > 0 &&
                  sandboxBase
                      .entryList({QStringLiteral("forkmesh-run-110-*")},
                                 QDir::Dirs | QDir::NoDotAndDotDot)
                      .isEmpty(),
              "the tree a destroyed runner abandoned is reclaimed, and the "
              "sweep reports the room left on the node");
        QDir(liveTree).removeRecursively();

        const QString interruptWorkflow = QStringLiteral(
            "name: Interrupted by a stop signal\non: push\nsteps:\n"
            "  - name: interruptible step\n"
            "    run: sleep 31.4159\n");
        check(writeFile(workflowPath, interruptWorkflow.toUtf8()),
              "interruptible workflow fixture is written");
        const QString interruptCommit =
            commitAll(repository, QStringLiteral("interruptible workflow"));
        forkmesh::installCrashHandler(
            temp.filePath(QStringLiteral("crash.log")),
            temp.filePath(QStringLiteral("main.log")));
        ActionRun interruptRun;
        interruptRun.id = 120;
        interruptRun.owner = QStringLiteral("security");
        interruptRun.name = QStringLiteral("interrupt");
        interruptRun.workflowPath = QStringLiteral(".forkmesh/security.yml");
        interruptRun.workflowName = QStringLiteral("Interrupt test");
        interruptRun.workflowContent = interruptWorkflow;
        interruptRun.commit = interruptCommit;
        interruptRun.ref = QStringLiteral("refs/heads/main");
        check(ActionStore::repositoryStateDigest(
                  repository, interruptCommit, &interruptRun.repositoryTree,
                  &interruptRun.executionDigest, &digestError),
              "interruptible workflow snapshot can be hashed");
        ActionRunner interruptRunner(&store);
        interruptRunner.setSandboxLimitsForTesting(limits);
        QEventLoop interruptLoop;
        bool sawInterrupted = false;
        bool sawFinished = false;
        bool signalled = false;
        QObject::connect(&interruptRunner, &ActionRunner::interrupted,
                         &interruptLoop, [&](int id) {
                             sawInterrupted = id == interruptRun.id;
                             interruptLoop.quit();
                         });
        QObject::connect(&interruptRunner, &ActionRunner::finished,
                         &interruptLoop, [&](int, bool) {
                             sawFinished = true;
                             interruptLoop.quit();
                         });
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &interruptLoop, [&] {
            if (signalled)
                return;
            if (!store.readLog(interruptRun)
                     .contains(QStringLiteral("interruptible step")))
                return;
            QThread::msleep(300);
            signalled = true;
            std::raise(SIGTERM);
            QProcess::execute(QStringLiteral("pkill"),
                              {QStringLiteral("-TERM"), QStringLiteral("-f"),
                               QStringLiteral("sleep 31.4159")});
        });
        poll.start(100);
        QTimer interruptGuard;
        interruptGuard.setSingleShot(true);
        QObject::connect(&interruptGuard, &QTimer::timeout, &interruptLoop,
                         [&] { interruptLoop.quit(); });
        interruptGuard.start(60000);
        interruptRunner.start(interruptRun,
                              ActionFile::parse(interruptRun.workflowPath,
                                                interruptWorkflow),
                              repository, QString(), {});
        interruptLoop.exec();
        poll.stop();
        interruptGuard.stop();
        ActionRun interruptedRun;
        for (const ActionRun &saved : store.loadAllRuns())
            if (saved.id == interruptRun.id)
                interruptedRun = saved;
        const QString interruptLog = store.readLog(interruptRun);
        check(signalled && forkmesh::terminationSignalReceived(),
              "the stop signal reached the node while the step ran");
        check(sawInterrupted && !sawFinished &&
                  interruptedRun.status == ActionStatus::Running &&
                  interruptLog.contains(QStringLiteral("INTERRUPTED")) &&
                  !interruptLog.contains(QStringLiteral("FAILED")) &&
                  !interruptRunner.busy(),
              "a step killed while the node is stopping is an interruption, "
              "not a failed run");
        check(interruptedRun.requeueAfterDesktopInterruption() &&
                  interruptedRun.status == ActionStatus::Queued,
              "the interrupted run is re-queued from its approved snapshot");
    }

    {
        const QString steps = QStringLiteral(
            "on: [push]\njobs:\n  build:\n    steps:\n      - run: true\n");
        check(ActionFile::parse(QStringLiteral(".forkmesh/ci.yml"), steps).fetchDepth == 2,
              "a workflow clones two commits deep by default");
        check(ActionFile::parse(QStringLiteral(".forkmesh/release.yml"),
                                    QStringLiteral("fetch-depth: 0\n") + steps).fetchDepth == 0,
              "fetch-depth: 0 asks for the full history");
        check(ActionFile::parse(QStringLiteral(".forkmesh/x.yml"),
                                    QStringLiteral("fetch-depth: 50\n") + steps).fetchDepth == 50,
              "fetch-depth takes a positive depth");
        check(ActionFile::parse(QStringLiteral(".forkmesh/y.yml"),
                                    QStringLiteral("fetch-depth: -3\n") + steps).fetchDepth == 2,
              "a negative fetch-depth keeps the default");
    }

    if (failures == 0)
        std::printf("\nAll Action security tests passed.\n");
    else
        std::fprintf(stderr, "\n%d Action security test(s) failed.\n",
                     failures);
    return failures == 0 ? 0 : 1;
}
