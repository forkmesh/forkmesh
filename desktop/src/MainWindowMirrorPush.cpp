// Mirror push: the ssh:// mirror remotes and the managed-host ref and
// release-artifact fan-out. Split out of MainWindowRepos.cpp.

#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "ControlNode.h"
#include "MirrorGatewayHealth.h"
#include "MirrorPushOutcome.h"

#include <QRegularExpression>

using namespace forkmesh::ui;

// Push the served bare mirror's stable refs (heads + tags) to every ssh://
// push remote configured on the working copy — e.g. the ssh.<worker> Git
// gateway feeding the managed mirror fleet, whose post-receive hook refreshes
// each mirror and lets its daemon publish catalog state. The relay's
// "mirror-update" websocket frame
// only reaches desktop peers in the live room; without this push the SSH-fed
// mirrors sat frozen at whatever the owner last pushed by hand.
// Best-effort and fully async; runs immediately for source changes and after
// every successful mirror sync, so the safety pass also self-heals a push the
// gateway missed. A source-convergence pass prunes after it has first read every
// exact mirror route; ordinary event pushes do not delete mirror-only refs.
// This makes source branch deletion converge without racing work that still
// needs to travel in the other direction.
// The fleet is not fixed: nodes are retired while their remotes stay
// configured. A gateway that stops answering therefore rotates out of the
// automatic pass for a growing cooldown (MirrorGatewayHealth) rather than
// costing a TCP connect timeout and an identical red log line every pass. A
// user-initiated release fan-out still tries every configured gateway.
void MainWindow::pushToSshMirrorRemotes(int index)
{
    pushToSshMirrorRemotes(index, /*wakeManagedIfUnchanged=*/true);
}

void MainWindow::pushToSshMirrorRemotes(int index,
                                        bool wakeManagedIfUnchanged)
{
    pushToSshMirrorRemotes(index, wakeManagedIfUnchanged,
                           /*pruneRefs=*/false);
}

void MainWindow::pushToSshMirrorRemotes(int index,
                                        bool wakeManagedIfUnchanged,
                                        bool pruneRefs)
{
    (void)pushToSshMirrorRemotes(index, /*userInitiated=*/false, QString(),
                                 nullptr, pruneRefs);
    (void)pushRefsToManagedHosts(
        index, wakeManagedIfUnchanged, QString(), QStringList(), {},
        /*reconcileArtifacts=*/true, pruneRefs);
}

int MainWindow::pushReleaseToManagedHosts(
    int index, const QString &tag,
    const QStringList &artifactHashes,
    std::function<void(int succeeded, int failed)> onFinished)
{
    if (tag.trimmed().isEmpty() || artifactHashes.isEmpty()) {
        logSystem(QStringLiteral(
                      "Release: managed-host refs were held back because no "
                      "authenticated artifact set was supplied."));
        if (onFinished)
            QTimer::singleShot(0, this,
                               [onFinished] { onFinished(0, 1); });
        return 0;
    }
    return pushRefsToManagedHosts(
        index, /*wakeIfUnchanged=*/true, tag, artifactHashes,
        std::move(onFinished));
}

struct MainWindow::ManagedPushTarget {
    QString node;
    QString host;
    QString url;
    QProcessEnvironment environment;
    forkmesh::control::HostSshCommand wakeCommand;
    forkmesh::control::HostSshCommand artifactInventoryCommand;
    QList<forkmesh::control::HostSshCommand> artifactCommands;
};

int MainWindow::pushRefsToManagedHosts(
    int index, bool wakeIfUnchanged, const QString &releaseTag,
    const QStringList &releaseArtifactHashes,
    std::function<void(int succeeded, int failed)> onFinished,
    bool reconcileArtifacts, bool pruneRefs)
{
    const auto finishEmpty = [this, onFinished] {
        if (onFinished) {
            QTimer::singleShot(0, this,
                               [onFinished] { onFinished(0, 0); });
        }
        return 0;
    };
    if (index < 0 || index >= m_repositories.size())
        return finishEmpty();
    const RepositoryRecord repo = m_repositories.at(index);
    if (repo.previewOnly || repo.isPrivate ||
        repo.localPath.trimmed().isEmpty() ||
        !QDir(repo.localPath).exists()) {
        return finishEmpty();
    }

    // Managed server provisioning currently installs the canonical product
    // repository at this deterministic path. Other repositories keep using
    // their explicit ssh:// remotes until they are provisioned on the fleet.
    if (repo.owner.trimmed().compare(QLatin1String("forkmesh"),
                                     Qt::CaseInsensitive) != 0 ||
        repo.name.trimmed().compare(QLatin1String("forkmesh"),
                                    Qt::CaseInsensitive) != 0) {
        return finishEmpty();
    }

    const bool releasePush = !releaseTag.trimmed().isEmpty();
    if (releasePush &&
        !releasePublicationPending(repo.owner, repo.name, releaseTag)) {
        logSystem(QStringLiteral(
                      "Release: %1 refs were held back because a newer "
                      "publication superseded this fan-out.")
                      .arg(releaseTag));
        if (onFinished)
            QTimer::singleShot(0, this,
                               [onFinished] { onFinished(0, 1); });
        return 0;
    }
    if (!releasePush &&
        releasePublicationPendingForRepository(repo.owner, repo.name)) {
        return finishEmpty();
    }
    QList<MirrorReleaseBlob> releaseArtifacts;
    if (!collectManagedPushArtifacts(repo, releaseTag, releaseArtifactHashes,
                                     releasePush, reconcileArtifacts,
                                     &releaseArtifacts)) {
        if (onFinished)
            QTimer::singleShot(0, this,
                               [onFinished] { onFinished(0, 1); });
        return 0;
    }
    const QString managedRepoKey =
        (repo.owner + QLatin1Char('/') + repo.name).toLower();
    const bool startArtifactReconciliation =
        !releasePush && reconcileArtifacts && !releaseArtifacts.isEmpty() &&
        !m_managedArtifactReconciling.contains(managedRepoKey);

    int unavailableTargets = 0;
    QList<ManagedPushTarget> targets = managedPushTargets(
        releaseArtifacts, startArtifactReconciliation, &unavailableTargets);
    if (targets.isEmpty()) {
        if (onFinished) {
            QTimer::singleShot(
                0, this,
                [onFinished, unavailableTargets] {
                    onFinished(0, unavailableTargets);
                });
        }
        return 0;
    }

    QStringList targetHosts;
    targetHosts.reserve(targets.size());
    for (const ManagedPushTarget &target : std::as_const(targets))
        targetHosts.append(target.host);

    // A release cannot overtake an ordinary ref push that was already in
    // flight when publication began. Wait for those ref writers to drain,
    // then restart the whole artifact-before-refs sequence against current
    // state. Ordinary pushes are held at the publication gate above while the
    // release is pending, so this short retry cannot be starved by new work.
    if (releasePush &&
        m_managedRefPushGate.busy(managedRepoKey, targetHosts)) {
        QTimer::singleShot(
            250, this,
            [this, managedRepoKey, wakeIfUnchanged, releaseTag,
             releaseArtifactHashes, onFinished, reconcileArtifacts] {
                for (int candidate = 0; candidate < m_repositories.size();
                     ++candidate) {
                    const RepositoryRecord &queued =
                        m_repositories.at(candidate);
                    if ((queued.owner + QLatin1Char('/') + queued.name)
                            .toLower() != managedRepoKey) {
                        continue;
                    }
                    (void)pushRefsToManagedHosts(
                        candidate, wakeIfUnchanged, releaseTag,
                        releaseArtifactHashes, onFinished,
                        reconcileArtifacts);
                    return;
                }
                if (onFinished)
                    onFinished(0, 1);
            });
        return targets.size();
    }

    // Ordinary fan-out heals the complete retained CAS alongside its ref push.
    // Artifact inventory must never sit in front of a normal commit: hashing a
    // large retained release set can take seconds, while each managed mirror
    // should receive a new ref immediately. Releases keep the stricter
    // artifact-before-tag ordering in the separate branch below.
    if (startArtifactReconciliation) {
        startManagedArtifactReconciliation(managedRepoKey, targets,
                                           unavailableTargets,
                                           releaseArtifacts);
    }

    // A release manifest authorizes blobs that live outside Git. Advancing the
    // refs first makes every newly-current mirror advertise bytes it does not
    // have, and using the public blob route to fill that gap can only bounce
    // among the same empty mirrors. Seed the signed CAS set over the controller
    // channel first. Only an all-host success enters the ordinary ref fan-out
    // below, so visitors always see either the complete old release or the
    // complete new one.
    if (releasePush && !releaseArtifacts.isEmpty()) {
        return seedManagedReleaseArtifacts(index, wakeIfUnchanged, releaseTag,
                                           onFinished, targets,
                                           unavailableTargets,
                                           releaseArtifacts);
    }

    return pushManagedRefsToHosts(repo, managedRepoKey, targetHosts,
                                  std::move(targets), unavailableTargets,
                                  wakeIfUnchanged, pruneRefs, releaseTag,
                                  onFinished);
}

// The authenticated artifact set a managed-host push carries: the release's
// signed hashes (each verified present in the local served CAS), plus, for an
// ordinary reconciling pass, the whole retained CAS. False means the release
// must be held back; the reason has already been logged.
bool MainWindow::collectManagedPushArtifacts(
    const RepositoryRecord &repo, const QString &releaseTag,
    const QStringList &releaseArtifactHashes, bool releasePush,
    bool reconcileArtifacts, QList<MirrorReleaseBlob> *releaseArtifacts)
{
    QSet<QString> seenArtifactHashes;
    for (const QString &rawHash : releaseArtifactHashes) {
        const QString hash = rawHash.trimmed().toLower();
        if (seenArtifactHashes.contains(hash))
            continue;
        if (forkmesh::control::managedReleaseArtifactSeedCommand(hash)
                .isEmpty()) {
            logSystem(QStringLiteral(
                          "Release: %1 refs were held back because its signed "
                          "artifact list contains an invalid digest.")
                          .arg(releaseTag));
            return false;
        }
        const QString path = mirrorReleaseBlobPath(repo.mirrorPath, hash);
        const QFileInfo info(path);
        if (!info.isFile() || info.isSymLink() || info.size() <= 0) {
            logSystem(QStringLiteral(
                          "Release: %1 refs were held back because artifact "
                          "%2 is not present in the local served CAS.")
                          .arg(releaseTag, hash.left(12)));
            return false;
        }
        seenArtifactHashes.insert(hash);
        releaseArtifacts->append({hash, info.absoluteFilePath(), info.size()});
    }
    // A release push carries the newly signed set explicitly and preserves its
    // strict artifact-before-refs publication ordering below. Ordinary source
    // reconciliation also carries the historical CAS: ask each managed host
    // which objects it lacks, then stream only that difference. This is what
    // lets a newly provisioned or repaired node converge from zero artifacts
    // without retransmitting every retained binary on every safety pass.
    if (!releasePush && reconcileArtifacts) {
        for (const MirrorReleaseBlob &artifact :
             mirrorReleaseBlobs(repo.mirrorPath)) {
            const QString hash = artifact.hash.trimmed().toLower();
            if (seenArtifactHashes.contains(hash) || artifact.size <= 0 ||
                forkmesh::control::managedReleaseArtifactSeedCommand(hash)
                    .isEmpty()) {
                continue;
            }
            const QFileInfo info(artifact.path);
            if (!info.isFile() || info.isSymLink())
                continue;
            seenArtifactHashes.insert(hash);
            releaseArtifacts->append(
                {hash, info.absoluteFilePath(), info.size()});
        }
    }
    return true;
}

// Every saved, active root host that can carry a managed ref push, with its
// transport environment and the wake/artifact commands built up front. Hosts
// whose SSH transport cannot be built are counted in *unavailableTargets.
QList<MainWindow::ManagedPushTarget> MainWindow::managedPushTargets(
    const QList<MirrorReleaseBlob> &releaseArtifacts,
    bool startArtifactReconciliation, int *unavailableTargets)
{
    QList<ManagedPushTarget> targets;
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    QSet<QString> seen;
    for (const QJsonValue &value : hosts) {
        const QJsonObject saved = value.toObject();
        if (!forkmesh::control::savedHostMayBeActive(saved))
            continue;
        const QString node =
            saved.value(QStringLiteral("name")).toString().trimmed();
        const QString host =
            saved.value(QStringLiteral("ip")).toString().trimmed();
        const QString user =
            saved.value(QStringLiteral("user")).toString().trimmed();
        // Provisioned managed mirrors use root only as the SSH transport;
        // receive-pack itself runs as the locked forkmesh-node account. A
        // non-root saved machine may need an operator-specific sudo policy,
        // which an unattended Git protocol exchange cannot safely guess.
        if (node.isEmpty() || host.isEmpty() || user != QLatin1String("root") ||
            seen.contains(host.toLower())) {
            continue;
        }
        const QString credentialKey =
            forkmesh::control::savedHostCredentialKey(node, host, user);
        const QString identityFile =
            savedHostIdentityFile(node, host, user);
        QString sshError;
        forkmesh::control::HostSshCommand transport =
            forkmesh::control::buildHostSshCommand(
                host, user, m_hostSessionPasswords.value(credentialKey),
                QStringLiteral("true"), &sshError,
                identityFile);
        // A receive updates the bare repository immediately, but the daemon's
        // signed catalog publication and gateway generation advance only when
        // its cycle runs. Ring the loopback control endpoint over the same
        // authenticated SSH channel after every completed receive. This also
        // upgrades already-deployed nodes that do not yet carry the managed
        // post-receive hook installed by newer daemon releases.
        forkmesh::control::HostSshCommand wakeCommand =
            forkmesh::control::buildHostSshCommand(
                host, user, m_hostSessionPasswords.value(credentialKey),
                QStringLiteral(
                    "/usr/bin/curl -fsS --connect-timeout 2 --max-time 5 "
                    "-X POST "
                    "http://127.0.0.1:8791/v1/control/sync?local=1"),
                &sshError, identityFile);
        forkmesh::control::HostSshCommand artifactInventoryCommand;
        if (startArtifactReconciliation) {
            QStringList hashes;
            hashes.reserve(releaseArtifacts.size());
            for (const MirrorReleaseBlob &artifact :
                 std::as_const(releaseArtifacts)) {
                hashes.append(artifact.hash);
            }
            artifactInventoryCommand =
                forkmesh::control::buildHostSshCommand(
                    host, user,
                    m_hostSessionPasswords.value(credentialKey),
                    forkmesh::control::managedReleaseArtifactInventoryCommand(
                        hashes),
                    &sshError, identityFile);
        }
        QList<forkmesh::control::HostSshCommand> artifactCommands;
        for (const MirrorReleaseBlob &artifact :
             std::as_const(releaseArtifacts)) {
            artifactCommands.append(
                forkmesh::control::buildHostSshCommand(
                    host, user,
                    m_hostSessionPasswords.value(credentialKey),
                    forkmesh::control::managedReleaseArtifactSeedCommand(
                        artifact.hash),
                    &sshError, identityFile));
        }
        const bool artifactTransportInvalid =
            std::any_of(artifactCommands.cbegin(), artifactCommands.cend(),
                        [](const forkmesh::control::HostSshCommand &command) {
                            return command.program.isEmpty();
                        });
        const bool artifactInventoryInvalid =
            startArtifactReconciliation &&
            artifactInventoryCommand.program.isEmpty();
        if (transport.program.isEmpty() || transport.arguments.size() < 2 ||
            wakeCommand.program.isEmpty() || artifactInventoryInvalid ||
            artifactTransportInvalid) {
            ++*unavailableTargets;
            logSystem(QStringLiteral(
                          "Mirror: cannot open the managed SSH transport to "
                          "%1 (%2): %3")
                          .arg(node, host,
                               sshError.isEmpty()
                                   ? QStringLiteral("invalid saved host metadata")
                                   : sshError));
            continue;
        }
        // GIT_SSH_COMMAND supplies only the transport prefix; Git appends the
        // destination and git-receive-pack command itself.
        transport.arguments.removeLast(); // fixed "true" probe command
        transport.arguments.removeLast(); // root@host destination
        transport.arguments << QStringLiteral("-o")
                            << QStringLiteral("ServerAliveInterval=15")
                            << QStringLiteral("-o")
                            << QStringLiteral("ServerAliveCountMax=2");
        QProcessEnvironment environment = transport.environment;
        environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"),
                           QStringLiteral("0"));
        environment.insert(
            QStringLiteral("GIT_SSH_COMMAND"),
            forkmesh::control::hostSshCommandLine(transport));

        QUrl remote;
        remote.setScheme(QStringLiteral("ssh"));
        remote.setUserName(user);
        remote.setHost(host);
        remote.setPath(QStringLiteral(
            "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git"));
        const QString url = remote.toString(QUrl::FullyEncoded);
        if (url.isEmpty()) {
            ++*unavailableTargets;
            continue;
        }
        seen.insert(host.toLower());
        targets.append({node, host, url, environment, wakeCommand,
                        artifactInventoryCommand, artifactCommands});
    }
    return targets;
}

// Ask each managed host which retained CAS objects it lacks and stream only
// those, independently of the ref push (see pushRefsToManagedHosts).
void MainWindow::startManagedArtifactReconciliation(
    const QString &managedRepoKey, const QList<ManagedPushTarget> &targets,
    int unavailableTargets, const QList<MirrorReleaseBlob> &releaseArtifacts)
{
    m_managedArtifactReconciling.insert(managedRepoKey);
    auto remainingInventories = std::make_shared<int>(targets.size());
    auto failedInventories = std::make_shared<int>(unavailableTargets);
    auto repairedArtifacts = std::make_shared<int>(0);
    const auto finishTarget =
        [this, remainingInventories, failedInventories,
         repairedArtifacts, managedRepoKey](bool ok, int repaired) {
            if (!ok)
                ++*failedInventories;
            *repairedArtifacts += repaired;
            if (--*remainingInventories > 0)
                return;
            m_managedArtifactReconciling.remove(managedRepoKey);
            if (*failedInventories > 0) {
                logSystem(QStringLiteral(
                              "Artifacts: managed-host reconciliation "
                              "failed on %1 host%2 and will retry; normal "
                              "ref fan-out continued independently.")
                              .arg(*failedInventories)
                              .arg(*failedInventories == 1
                                       ? QString()
                                       : QStringLiteral("s")));
                return;
            }
            if (*repairedArtifacts > 0) {
                logSystem(QStringLiteral(
                              "Artifacts: restored %1 retained CAS object%2 "
                              "across the managed mirror fleet.")
                              .arg(*repairedArtifacts)
                              .arg(*repairedArtifacts == 1
                                       ? QString()
                                       : QStringLiteral("s")));
            }
        };

    // First index of each retained hash, so matching an inventory line is
    // a lookup rather than a scan of the whole retained set.
    QHash<QString, int> artifactIndexByHash;
    artifactIndexByHash.reserve(releaseArtifacts.size());
    for (int i = 0; i < releaseArtifacts.size(); ++i) {
        if (!artifactIndexByHash.contains(releaseArtifacts.at(i).hash))
            artifactIndexByHash.insert(releaseArtifacts.at(i).hash, i);
    }
    for (const ManagedPushTarget &target : std::as_const(targets)) {
        auto *inventory = new QProcess(this);
        inventory->setProcessEnvironment(
            target.artifactInventoryCommand.environment);
        auto *hardCap = new QTimer(inventory);
        hardCap->setSingleShot(true);
        hardCap->setInterval(5 * 60 * 1000);
        connect(hardCap, &QTimer::timeout, inventory,
                [inventory] { inventory->kill(); });
        auto inventoryFinished = std::make_shared<bool>(false);
        const auto concludeInventory =
            [this, inventory, inventoryFinished, finishTarget, target,
             releaseArtifacts, artifactIndexByHash](bool started, bool ok) {
                if (*inventoryFinished)
                    return;
                *inventoryFinished = true;
                const QString errors =
                    QString::fromUtf8(inventory->readAllStandardError())
                        .trimmed().left(300);
                const QString output =
                    QString::fromUtf8(inventory->readAllStandardOutput())
                        .trimmed();
                inventory->deleteLater();
                if (!started || !ok) {
                    logSystem(QStringLiteral(
                                  "Artifacts: could not inventory managed "
                                  "host %1 (%2): %3")
                                  .arg(
                                      target.node, target.host,
                                      errors.isEmpty()
                                          ? QStringLiteral(
                                                "SSH artifact inventory "
                                                "failed")
                                          : errors));
                    finishTarget(false, 0);
                    return;
                }

                QList<int> missing;
                QSet<QString> reported;
                const QStringList lines = output.split(
                    QRegularExpression(QStringLiteral("[\\r\\n]+")),
                    Qt::SkipEmptyParts);
                for (const QString &line : lines) {
                    const QString hash = line.trimmed().toLower();
                    const int artifactIndex =
                        artifactIndexByHash.value(hash, -1);
                    if (artifactIndex < 0 || reported.contains(hash)) {
                        logSystem(QStringLiteral(
                                      "Artifacts: managed host %1 (%2) "
                                      "returned an invalid inventory.")
                                      .arg(target.node, target.host));
                        finishTarget(false, 0);
                        return;
                    }
                    reported.insert(hash);
                    missing.append(artifactIndex);
                }
                if (missing.isEmpty()) {
                    finishTarget(true, 0);
                    return;
                }

                auto nextMissing = std::make_shared<int>(0);
                auto seedNext =
                    std::make_shared<std::function<void()>>();
                const std::weak_ptr<std::function<void()>> weakSeedNext =
                    seedNext;
                *seedNext =
                    [this, target, releaseArtifacts, missing, nextMissing,
                     weakSeedNext, finishTarget] {
                        const auto self = weakSeedNext.lock();
                        if (!self) {
                            finishTarget(false, 0);
                            return;
                        }
                        if (*nextMissing >= missing.size()) {
                            // The CAS changed without touching Git. Ring
                            // the daemon now so its signed artifactCount
                            // leaves zero immediately instead of waiting
                            // for the catalog lease heartbeat.
                            auto *wake = new QProcess(this);
                            wake->setProcessEnvironment(
                                target.wakeCommand.environment);
                            wake->setStandardOutputFile(
                                QProcess::nullDevice());
                            auto wakeFinished =
                                std::make_shared<bool>(false);
                            const auto concludeWake =
                                [this, wake, wakeFinished, finishTarget,
                                 target, repaired = missing.size()](
                                    bool wakeOk) {
                                    if (*wakeFinished)
                                        return;
                                    *wakeFinished = true;
                                    const QString wakeErrors =
                                        QString::fromUtf8(
                                            wake->readAllStandardError())
                                            .trimmed().left(300);
                                    wake->deleteLater();
                                    if (!wakeOk) {
                                        logSystem(QStringLiteral(
                                                      "Artifacts: restored "
                                                      "the CAS on %1 (%2) "
                                                      "but could not wake "
                                                      "its catalog publish: "
                                                      "%3")
                                                      .arg(
                                                          target.node,
                                                          target.host,
                                                          wakeErrors.isEmpty()
                                                              ? QStringLiteral(
                                                                    "sync wake "
                                                                    "failed")
                                                              : wakeErrors));
                                    }
                                    finishTarget(wakeOk, repaired);
                                };
                            connect(wake, &QProcess::finished, this,
                                    [concludeWake](
                                        int exitCode,
                                        QProcess::ExitStatus status) {
                                        concludeWake(
                                            status ==
                                                QProcess::NormalExit &&
                                            exitCode == 0);
                                    });
                            connect(wake, &QProcess::errorOccurred, this,
                                    [concludeWake](
                                        QProcess::ProcessError error) {
                                        if (error ==
                                            QProcess::FailedToStart)
                                            concludeWake(false);
                                    });
                            wake->start(target.wakeCommand.program,
                                        target.wakeCommand.arguments);
                            return;
                        }
                        const int artifactIndex =
                            missing.at((*nextMissing)++);
                        const MirrorReleaseBlob artifact =
                            releaseArtifacts.at(artifactIndex);
                        const forkmesh::control::HostSshCommand command =
                            target.artifactCommands.at(artifactIndex);
                        auto *seed = new QProcess(this);
                        seed->setProcessEnvironment(command.environment);
                        seed->setStandardInputFile(artifact.path);
                        seed->setStandardOutputFile(QProcess::nullDevice());
                        auto *seedCap = new QTimer(seed);
                        seedCap->setSingleShot(true);
                        seedCap->setInterval(5 * 60 * 1000);
                        connect(seedCap, &QTimer::timeout, seed,
                                [seed] { seed->kill(); });
                        auto seedFinished =
                            std::make_shared<bool>(false);
                        const auto concludeSeed =
                            [this, seed, seedFinished, self, finishTarget,
                             target, artifact](bool seedOk) {
                                if (*seedFinished)
                                    return;
                                *seedFinished = true;
                                const QString seedErrors =
                                    QString::fromUtf8(
                                        seed->readAllStandardError())
                                        .trimmed().left(300);
                                seed->deleteLater();
                                if (!seedOk) {
                                    logSystem(QStringLiteral(
                                                  "Artifacts: could not "
                                                  "restore %1 on managed "
                                                  "host %2 (%3): %4")
                                                  .arg(
                                                      artifact.hash.left(12),
                                                      target.node,
                                                      target.host,
                                                      seedErrors.isEmpty()
                                                          ? QStringLiteral(
                                                                "SSH artifact "
                                                                "transfer failed")
                                                          : seedErrors));
                                    finishTarget(false, 0);
                                    return;
                                }
                                (*self)();
                            };
                        connect(seed, &QProcess::finished, this,
                                [concludeSeed](
                                    int exitCode,
                                    QProcess::ExitStatus status) {
                                    concludeSeed(
                                        status == QProcess::NormalExit &&
                                        exitCode == 0);
                                });
                        connect(seed, &QProcess::errorOccurred, this,
                                [concludeSeed](
                                    QProcess::ProcessError error) {
                                    if (error == QProcess::FailedToStart)
                                        concludeSeed(false);
                                });
                        trackProcessActivity(
                            seed, QStringLiteral("release"),
                            QStringLiteral("Restoring artifact %1 on %2")
                                .arg(artifact.hash.left(12), target.node));
                        seedCap->start();
                        seed->start(command.program, command.arguments);
                    };
                (*seedNext)();
            };
        connect(inventory, &QProcess::finished, this,
                [concludeInventory](int exitCode,
                                    QProcess::ExitStatus status) {
                    concludeInventory(
                        true, status == QProcess::NormalExit &&
                                  exitCode == 0);
                });
        connect(inventory, &QProcess::errorOccurred, this,
                [concludeInventory](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart)
                        concludeInventory(false, false);
                });
        trackProcessActivity(
            inventory, QStringLiteral("release"),
            QStringLiteral("Checking release artifacts on %1")
                .arg(target.node));
        hardCap->start();
        inventory->start(target.artifactInventoryCommand.program,
                         target.artifactInventoryCommand.arguments);
    }
}

// Seed a release's signed CAS set on every managed host, then re-enter
// pushRefsToManagedHosts for the refs only once every host succeeded (see the
// ordering note at its call site).
int MainWindow::seedManagedReleaseArtifacts(
    int index, bool wakeIfUnchanged, const QString &releaseTag,
    const std::function<void(int succeeded, int failed)> &onFinished,
    const QList<ManagedPushTarget> &targets, int unavailableTargets,
    const QList<MirrorReleaseBlob> &releaseArtifacts)
{
    auto remainingSeeds = std::make_shared<int>(targets.size());
    auto failedSeeds = std::make_shared<int>(unavailableTargets);
    const auto finishTarget =
        [this, remainingSeeds, failedSeeds, onFinished, index,
         wakeIfUnchanged, releaseTag, releaseArtifacts](bool ok) {
            if (!ok)
                ++*failedSeeds;
            if (--*remainingSeeds > 0)
                return;
            if (*failedSeeds > 0) {
                logSystem(QStringLiteral(
                              "Release: %1 artifact seeding failed on %2 "
                              "managed host%3; refs and deployment remain pending.")
                              .arg(releaseTag)
                              .arg(*failedSeeds)
                              .arg(*failedSeeds == 1
                                       ? QString()
                                       : QStringLiteral("s")));
                if (onFinished)
                    onFinished(0, *failedSeeds);
                return;
            }
            logSystem(QStringLiteral(
                          "Release: seeded %1 signed artifact%2 on every "
                          "managed host before advancing refs.")
                          .arg(releaseArtifacts.size())
                          .arg(releaseArtifacts.size() == 1
                                   ? QString()
                                   : QStringLiteral("s")));
            (void)pushRefsToManagedHosts(
                index, wakeIfUnchanged, releaseTag, {}, onFinished,
                /*reconcileArtifacts=*/false);
        };
    flashMessage(QStringLiteral(
                     "Seeding %1 signed release artifact%2 to %3 installed "
                     "mirror host%4 before publishing refs…")
                     .arg(releaseArtifacts.size())
                     .arg(releaseArtifacts.size() == 1
                              ? QString()
                              : QStringLiteral("s"))
                     .arg(targets.size())
                     .arg(targets.size() == 1
                              ? QString()
                              : QStringLiteral("s")));

    for (const ManagedPushTarget &target : std::as_const(targets)) {
        auto nextArtifact = std::make_shared<int>(0);
        auto seedNext = std::make_shared<std::function<void()>>();
        // The step closure must not own itself. Clearing a self-owning
        // std::function from inside its own body destroys the running
        // closure — and with it finishTarget and every other capture —
        // and the completion path below then reads freed memory, which
        // corrupts the heap and aborts the process. Keep only a weak
        // reference here; the strong ones live in the QProcess signal
        // connections, so the chain is released once the last seed
        // process is deleted.
        const std::weak_ptr<std::function<void()>> weakSeedNext = seedNext;
        *seedNext =
            [this, target, releaseArtifacts, nextArtifact, weakSeedNext,
             finishTarget] {
                // Pin the chain for the whole body: the completion path
                // is free to drop the last strong reference, and a
                // closure must never be freed while it is executing.
                const auto self = weakSeedNext.lock();
                if (!self) {
                    finishTarget(false);
                    return;
                }
                if (*nextArtifact >= releaseArtifacts.size()) {
                    finishTarget(true);
                    return;
                }
                const int artifactIndex = (*nextArtifact)++;
                const MirrorReleaseBlob artifact =
                    releaseArtifacts.at(artifactIndex);
                const forkmesh::control::HostSshCommand command =
                    target.artifactCommands.at(artifactIndex);
                auto *seed = new QProcess(this);
                seed->setProcessEnvironment(command.environment);
                seed->setStandardInputFile(artifact.path);
                seed->setStandardOutputFile(QProcess::nullDevice());
                auto *hardCap = new QTimer(seed);
                hardCap->setSingleShot(true);
                hardCap->setInterval(5 * 60 * 1000);
                connect(hardCap, &QTimer::timeout, seed,
                        [seed] { seed->kill(); });
                auto finished = std::make_shared<bool>(false);
                const auto conclude =
                    [this, seed, finished, self, finishTarget,
                     target, artifact](bool ok) {
                        if (*finished)
                            return;
                        *finished = true;
                        const QString errors =
                            QString::fromUtf8(
                                seed->readAllStandardError())
                                .trimmed()
                                .left(300);
                        seed->deleteLater();
                        if (!ok) {
                            logSystem(QStringLiteral(
                                          "Release: could not seed artifact "
                                          "%1 on managed host %2 (%3): %4")
                                          .arg(
                                              artifact.hash.left(12),
                                              target.node, target.host,
                                              errors.isEmpty()
                                                  ? QStringLiteral(
                                                        "SSH artifact "
                                                        "transfer failed")
                                                  : errors));
                            finishTarget(false);
                            return;
                        }
                        (*self)();
                    };
                connect(seed, &QProcess::finished, this,
                        [conclude](int exitCode,
                                   QProcess::ExitStatus status) {
                            conclude(status == QProcess::NormalExit &&
                                     exitCode == 0);
                        });
                connect(seed, &QProcess::errorOccurred, this,
                        [conclude](QProcess::ProcessError error) {
                            if (error == QProcess::FailedToStart)
                                conclude(false);
                        });
                trackProcessActivity(
                    seed, QStringLiteral("release"),
                    QStringLiteral("Seeding release artifact %1 on %2")
                        .arg(artifact.hash.left(12), target.node));
                hardCap->start();
                seed->start(command.program, command.arguments);
            };
        (*seedNext)();
    }
    return targets.size();
}

// The ordinary ref fan-out to the managed hosts, after any release artifacts
// are already seeded.
int MainWindow::pushManagedRefsToHosts(
    const RepositoryRecord &repo, const QString &managedRepoKey,
    const QStringList &targetHosts, QList<ManagedPushTarget> targets,
    int unavailableTargets, bool wakeIfUnchanged, bool pruneRefs,
    const QString &releaseTag,
    const std::function<void(int succeeded, int failed)> &onFinished)
{
    const bool releasePush = !releaseTag.trimmed().isEmpty();
    // Commits, completed mirror refreshes and the periodic safety pass can all
    // request this asynchronous fan-out before the preceding one has exited.
    // Git protects each ref with a compare-and-swap; overlapping writers turn
    // that protection into noisy, partial "cannot lock ref" failures. Start
    // each repository/host once and coalesce every overlapping request into a
    // single replay of the newest state after the active batch drains.
    const MirrorRefPushGate::Reservation reservation =
        m_managedRefPushGate.reserve(managedRepoKey, targetHosts);
    QList<ManagedPushTarget> readyTargets;
    readyTargets.reserve(targets.size());
    for (const ManagedPushTarget &target : std::as_const(targets)) {
        if (reservation.readyHosts.contains(target.host.trimmed().toLower()))
            readyTargets.append(target);
    }
    if (readyTargets.isEmpty())
        return targets.size();
    targets = std::move(readyTargets);

    const auto replayLatestManagedRefs =
        [this, managedRepoKey, wakeIfUnchanged, pruneRefs] {
            QTimer::singleShot(
                0, this,
                [this, managedRepoKey, wakeIfUnchanged, pruneRefs] {
                    for (int candidate = 0;
                         candidate < m_repositories.size(); ++candidate) {
                        const RepositoryRecord &queued =
                            m_repositories.at(candidate);
                        if ((queued.owner + QLatin1Char('/') + queued.name)
                                .toLower() != managedRepoKey) {
                            continue;
                        }
                        (void)pushRefsToManagedHosts(
                            candidate, wakeIfUnchanged, QString(),
                            QStringList(), {}, /*reconcileArtifacts=*/true,
                            pruneRefs);
                        return;
                    }
                });
        };

    auto remaining = std::make_shared<int>(targets.size());
    auto succeeded = std::make_shared<int>(0);
    auto failed = std::make_shared<int>(unavailableTargets);
    auto updated = std::make_shared<int>(0);
    const QString subject = releaseTag.trimmed().isEmpty()
                                ? repo.owner + QLatin1Char('/') + repo.name
                                : releaseTag.trimmed();
    const auto complete =
        [this, remaining, succeeded, failed, updated, onFinished, subject,
         releasePush](bool ok, bool refsChanged) {
            if (ok)
                ++*succeeded;
            else
                ++*failed;
            if (refsChanged)
                ++*updated;
            if (--*remaining > 0)
                return;
            // The periodic source safety pass also calls this fan-out. An
            // all-up-to-date pass is intentionally silent and, below, never
            // wakes a daemon; otherwise adding saved hosts would turn that
            // safety net back into a fleet-wide sync poll.
            if (releasePush || *updated > 0 || *failed > 0) {
                logSystem(
                    (releasePush
                         ? QStringLiteral(
                               "Release: managed-host ref fan-out for %1 "
                               "finished (%2 succeeded, %3 failed).")
                         : QStringLiteral(
                               "Mirror: managed-host ref fan-out for %1 "
                               "finished (%2 updated, %3 failed)."))
                        .arg(subject)
                        .arg(releasePush ? *succeeded : *updated)
                        .arg(*failed));
            }
            if (onFinished)
                onFinished(*succeeded, *failed);
        };
    if (releasePush) {
        flashMessage(QStringLiteral(
                         "Pushing %1 directly to %2 installed mirror host%3…")
                         .arg(subject)
                         .arg(targets.size())
                         .arg(targets.size() == 1 ? QString()
                                                   : QStringLiteral("s")));
    }
    for (const ManagedPushTarget &target : std::as_const(targets)) {
        auto *process = new QProcess(this);
        process->setProcessEnvironment(target.environment);
        auto *hardCap = new QTimer(process);
        hardCap->setSingleShot(true);
        hardCap->setInterval(75 * 1000);
        connect(hardCap, &QTimer::timeout, process, [process] {
            process->kill();
        });
        auto gitFinished = std::make_shared<bool>(false);
        const auto finishGit =
            [this, process, gitFinished, complete, target,
             repo, wakeIfUnchanged, managedRepoKey,
             replayLatestManagedRefs](bool gitStarted, bool gitOk,
                                      bool refsChanged) {
                if (*gitFinished)
                    return;
                *gitFinished = true;
                if (m_managedRefPushGate.finish(managedRepoKey, target.host))
                    replayLatestManagedRefs();
                const QString gitErrors =
                    QString::fromUtf8(process->readAllStandardError())
                        .trimmed().left(300);
                process->deleteLater();
                if (!gitStarted) {
                    logSystem(QStringLiteral(
                                  "Mirror: could not start the direct ref push "
                                  "of %1/%2 to managed host %3 (%4).")
                                  .arg(repo.owner, repo.name, target.node,
                                       target.host));
                    complete(false, false);
                    return;
                }

                // A configured ssh:// remote may have won a parallel push to
                // this same saved host. Event-driven calls still ring the old
                // daemon in that all-up-to-date race; the periodic safety pass
                // opts out so unchanged state never becomes a fleet poll.
                if (!refsChanged && (!gitOk || !wakeIfUnchanged)) {
                    if (!gitOk) {
                        logSystem(QStringLiteral(
                                      "Mirror: direct ref update of %1/%2 on "
                                      "managed host %3 (%4) failed: %5")
                                      .arg(repo.owner, repo.name, target.node,
                                           target.host,
                                           gitErrors.isEmpty()
                                               ? QStringLiteral(
                                                     "git push failed")
                                               : gitErrors));
                    }
                    complete(gitOk, false);
                    return;
                }

                // Wake even after a non-zero Git exit: non-atomic multi-ref
                // pushes can advance main while one collaboration branch is
                // held back, and that successful subset must be published now.
                auto *wake = new QProcess(this);
                wake->setProcessEnvironment(target.wakeCommand.environment);
                wake->setStandardOutputFile(QProcess::nullDevice());
                auto *wakeCap = new QTimer(wake);
                wakeCap->setSingleShot(true);
                wakeCap->setInterval(45 * 1000);
                connect(wakeCap, &QTimer::timeout, wake, [wake] {
                    wake->kill();
                });
                auto wakeFinished = std::make_shared<bool>(false);
                const auto conclude =
                    [this, wake, wakeFinished, complete, target, repo, gitOk,
                     gitErrors, refsChanged](bool wakeOk) {
                        if (*wakeFinished)
                            return;
                        *wakeFinished = true;
                        const QString wakeErrors =
                            QString::fromUtf8(wake->readAllStandardError())
                                .trimmed().left(300);
                        wake->deleteLater();
                        if (gitOk && wakeOk) {
                            logSystem(
                                (refsChanged
                                     ? QStringLiteral(
                                           "Mirror: pushed %1/%2 to managed "
                                           "host %3 (%4) and woke its daemon.")
                                     : QStringLiteral(
                                           "Mirror: refs for %1/%2 were already "
                                           "current on managed host %3 (%4); "
                                           "woke its daemon."))
                                    .arg(repo.owner, repo.name, target.node,
                                         target.host));
                            complete(true, refsChanged);
                            return;
                        }
                        QString reason;
                        if (!gitOk) {
                            reason = gitErrors.isEmpty()
                                         ? QStringLiteral("git push failed")
                                         : gitErrors;
                        }
                        if (!wakeOk) {
                            if (!reason.isEmpty())
                                reason += QStringLiteral("; ");
                            reason += wakeErrors.isEmpty()
                                          ? QStringLiteral(
                                                "mirror daemon wake failed")
                                          : QStringLiteral(
                                                "mirror daemon wake failed: ") +
                                                wakeErrors;
                        }
                        logSystem(QStringLiteral(
                                      "Mirror: direct ref update of %1/%2 on "
                                      "managed host %3 (%4) was incomplete: %5")
                                      .arg(repo.owner, repo.name, target.node,
                                           target.host, reason));
                        complete(false, refsChanged);
                    };
                connect(wake, &QProcess::finished, this,
                        [conclude](int exitCode,
                                   QProcess::ExitStatus status) {
                            conclude(status == QProcess::NormalExit &&
                                     exitCode == 0);
                        });
                connect(wake, &QProcess::errorOccurred, this,
                        [conclude](QProcess::ProcessError error) {
                            if (error == QProcess::FailedToStart)
                                conclude(false);
                        });
                trackProcessActivity(
                    wake, QStringLiteral("sync"),
                    QStringLiteral("Waking mirror daemon on %1")
                        .arg(target.node));
                wakeCap->start();
                wake->start(target.wakeCommand.program,
                            target.wakeCommand.arguments);
            };
        connect(process, &QProcess::finished, this,
                [process, finishGit,
                 releasePush](int exitCode, QProcess::ExitStatus status) {
                    const MirrorPushOutcome outcome = classifyMirrorPush(
                        QString::fromUtf8(process->readAllStandardOutput()));
                    const bool gitOk =
                        status == QProcess::NormalExit &&
                        (exitCode == 0 ||
                         (!releasePush && outcome.onlyHeldBack));
                    finishGit(true, gitOk, !outcome.updated.isEmpty());
                });
        connect(process, &QProcess::errorOccurred, this,
                [finishGit](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart)
                        finishGit(false, false, false);
                });
        trackProcessActivity(
            process, QStringLiteral("push"),
            QStringLiteral("Pushing %1/%2 to %3")
                .arg(repo.owner, repo.name, target.node));
        hardCap->start();
        // refs/pr/* is a derived GitHub-compatibility cache. Durable pull
        // state lives in main plus refs/heads/forkmesh/pull/*, which stays
        // subject to the non-forcing policy above. After a complete reverse
        // read, repair and prune only this cache namespace so stale pointers
        // cannot make an otherwise converged mirror advertise old pull data.
        process->start(
            QStringLiteral("git"),
            {QStringLiteral("-C"), repo.localPath, QStringLiteral("push"),
             QStringLiteral("--porcelain"), QStringLiteral("--no-force"),
             pruneRefs ? QStringLiteral("--prune")
                       : QStringLiteral("--no-prune"),
             QStringLiteral(
                 "--receive-pack=runuser -u forkmesh-node -- git-receive-pack"),
             target.url, QStringLiteral("refs/heads/*:refs/heads/*"),
             QStringLiteral("refs/tags/*:refs/tags/*"),
             pruneRefs ? QStringLiteral("+refs/pr/*:refs/pr/*")
                       : QStringLiteral("refs/pr/*:refs/pr/*")});
    }
    return targets.size();
}

// Re-push to the gateways a pass had to skip, the moment the earliest of them
// finishes cooling down. Without this the skipped refs waited for the periodic
// safety pass on top of the cooldown, which is what made a single failed
// connect look like "the mirrors stopped following my pushes" — the fleet was
// healthy again long before anything tried it.
void MainWindow::scheduleSshMirrorPushRetry(const QString &repoKey,
                                            const QStringList &skippedHosts,
                                            qint64 nowMs)
{
    if (skippedHosts.isEmpty() || repoKey.trimmed().isEmpty())
        return;
    const qint64 delay = mirrorPushRetryDelayMs(
        m_sshMirrorHealth, skippedHosts, nowMs,
        qint64(mirrorSyncIntervalMinutes()) * 60 * 1000);
    if (delay <= 0)
        return;
    if (QTimer *existing = m_sshMirrorPushRetry.value(repoKey)) {
        // Keep the earliest wake-up. A later pass re-reports the same cooling
        // gateways, and re-arming on every one of them would push the retry
        // further out each time and never fire.
        if (existing->isActive() && existing->remainingTime() >= 0 &&
            existing->remainingTime() <= delay)
            return;
        existing->stop();
        existing->deleteLater();
        m_sshMirrorPushRetry.remove(repoKey);
    }
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    m_sshMirrorPushRetry.insert(repoKey, timer);
    connect(timer, &QTimer::timeout, this, [this, repoKey, timer]() {
        if (m_sshMirrorPushRetry.value(repoKey) == timer)
            m_sshMirrorPushRetry.remove(repoKey);
        timer->deleteLater();
        for (int i = 0; i < m_repositories.size(); ++i) {
            const RepositoryRecord &candidate = m_repositories.at(i);
            if (candidate.owner + "/" + candidate.name == repoKey) {
                pushToSshMirrorRemotes(i);
                return;
            }
        }
    });
    timer->start(int(qBound<qint64>(1000, delay, 60LL * 60 * 1000)));
}

namespace {
const QString kSshGatewayHealthSetting =
    QStringLiteral("mirrors/sshGatewayHealth");
} // namespace

// The outage streak has to survive a relaunch. Held in memory it restarted
// from zero on every launch, so a desktop that is updated or rebooted daily
// never reached the day-long retirement threshold, re-announced the same
// destroyed gateway as a fresh failure on the first pass after every launch,
// and never stopped paying the full retry cadence for a node that was gone.
void MainWindow::loadSshMirrorGatewayHealth()
{
    if (m_sshMirrorHealthLoaded)
        return;
    m_sshMirrorHealthLoaded = true;
    m_sshMirrorHealth.restoreState(
        QSettings().value(kSshGatewayHealthSetting).toStringList(),
        QDateTime::currentMSecsSinceEpoch());
}

void MainWindow::saveSshMirrorGatewayHealth()
{
    const QStringList state = m_sshMirrorHealth.persistedState();
    if (state.isEmpty())
        QSettings().remove(kSshGatewayHealthSetting);
    else
        QSettings().setValue(kSshGatewayHealthSetting, state);
}

// Deleting a mesh node destroyed its server, its DNS and its relay records but
// left every `git remote` pointing at it configured, so the unattended fan-out
// kept dialling a host that no longer exists. Unwire it here, where the node is
// actually being retired, instead of waiting for a human to read a log line.
int MainWindow::removeSshMirrorRemotesForNode(const QString &node)
{
    const QString target = node.trimmed();
    if (target.isEmpty())
        return 0;
    loadSshMirrorGatewayHealth();
    // A remote may name the node by its address rather than by any alias, and
    // the saved host is the only place that mapping lives. Read it before the
    // caller forgets the host.
    QString address;
    {
        QSettings settings;
        const QJsonArray hosts = forkmesh::control::loadSavedHosts(
            settings, kHostsSetting, &m_hostSessionPasswords);
        for (const QJsonValue &value : hosts) {
            const QJsonObject host = value.toObject();
            if (host.value(QStringLiteral("name")).toString().trimmed().compare(
                    target, Qt::CaseInsensitive) == 0) {
                address = host.value(QStringLiteral("ip")).toString().trimmed();
                break;
            }
        }
    }

    // By value: runGitCapture below pumps the event loop and m_repositories
    // can be rebuilt under us (git-pump UAF family).
    QStringList paths;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        for (const QString &path : {repo.localPath, repo.mirrorPath}) {
            const QString trimmed = path.trimmed();
            if (!trimmed.isEmpty() && !paths.contains(trimmed) &&
                QDir(trimmed).exists())
                paths.append(trimmed);
        }
    }

    int removed = 0;
    QSet<QString> retiredHosts;
    for (const QString &path : std::as_const(paths)) {
        QByteArray remotesOut;
        if (!cachedRepoRemotesRaw(path, &remotesOut))
            continue;
        QSet<QString> doomed;
        for (const QString &line :
             QString::fromUtf8(remotesOut).split(QLatin1Char('\n'))) {
            const QStringList parts = line.simplified().split(QLatin1Char(' '));
            if (parts.size() < 2)
                continue;
            const QString url = parts.at(1);
            if (!url.startsWith(QLatin1String("ssh://")))
                continue;
            const QString host = QUrl(url).host();
            const bool byAddress =
                !address.isEmpty() &&
                host.compare(address, Qt::CaseInsensitive) == 0;
            if (!byAddress && !mirrorGatewayHostBelongsToNode(host, target))
                continue;
            doomed.insert(parts.at(0));
            retiredHosts.insert(host);
        }
        for (const QString &name : std::as_const(doomed)) {
            if (runGitCapture(path,
                              {QStringLiteral("remote"),
                               QStringLiteral("remove"), name},
                              nullptr, nullptr)) {
                ++removed;
                logSystem(QStringLiteral("Mirror: removed git remote %1 from "
                                         "%2 — node %3 was deleted.")
                              .arg(name, path, target));
            }
        }
    }
    bool forgot = false;
    for (const QString &host : std::as_const(retiredHosts))
        forgot = m_sshMirrorHealth.forget(host) || forgot;
    if (forgot)
        saveSshMirrorGatewayHealth();
    if (removed > 0 && m_repoRemotesTable)
        reloadRepoRemotesTable();
    return removed;
}

int MainWindow::pushToSshMirrorRemotes(int index, bool userInitiated,
                                       const QString &releaseTag,
                                       QString *idleReason, bool pruneRefs)
{
    // Every "pushed nothing" exit names itself. The caller that reports the
    // outcome to a person cannot tell these apart from the return value, and
    // reporting all of them as "no SSH push remotes configured" sends the user
    // to edit a remote list that may be perfectly correct.
    const auto idle = [idleReason](const QString &reason) {
        if (idleReason)
            *idleReason = reason;
        return 0;
    };
    if (index < 0 || index >= m_repositories.size())
        return idle(QStringLiteral("the repository is no longer open"));
    loadSshMirrorGatewayHealth();
    // By value: runGitCapture below pumps the event loop, and a reference into
    // m_repositories can dangle across it (git-pump UAF family).
    const RepositoryRecord repo = m_repositories.at(index);
    const bool managedProduct =
        repo.owner.trimmed().compare(QLatin1String("forkmesh"),
                                     Qt::CaseInsensitive) == 0 &&
        repo.name.trimmed().compare(QLatin1String("forkmesh"),
                                    Qt::CaseInsensitive) == 0;
    if (repo.previewOnly || repo.isPrivate ||
        repo.localPath.trimmed().isEmpty())
        return idle(QStringLiteral(
            "this repository has no source working copy to push from"));
    if (releaseTag.trimmed().isEmpty() &&
        releasePublicationPendingForRepository(repo.owner, repo.name)) {
        return idle(QStringLiteral(
            "release refs are being held until every signed artifact is "
            "present on the managed mirrors"));
    }
    if (managedProduct && !releaseTag.trimmed().isEmpty() &&
        !releasePublicationPending(repo.owner, repo.name, releaseTag)) {
        return idle(QStringLiteral(
            "a newer release publication superseded this ref push"));
    }
    const QString pushSource = repo.localPath;
    if (!QDir(pushSource).exists())
        return idle(QStringLiteral("the working copy %1 no longer exists")
                        .arg(pushSource));
    const QString repoKey = repo.owner + "/" + repo.name;
    QByteArray remotesOut;
    if (!cachedRepoRemotesRaw(repo.localPath, &remotesOut))
        return idle(QStringLiteral("the git remotes of %1 could not be read")
                        .arg(pushSource));
    struct MirrorRemote {
        QString name;
        QString url;
        QString host;
    };
    QList<MirrorRemote> remotes;
    QSet<QString> seenUrls;
    for (const QString &line :
         QString::fromUtf8(remotesOut).split(QLatin1Char('\n'))) {
        const QString simplified = line.simplified();
        if (!simplified.endsWith(QLatin1String("(push)")))
            continue;
        const QStringList parts = simplified.split(QLatin1Char(' '));
        if (parts.size() < 2)
            continue;
        const QString url = parts.at(1);
        if (!url.startsWith(QLatin1String("ssh://")) ||
            seenUrls.contains(url))
            continue;
        seenUrls.insert(url);
        remotes.append({parts.at(0), url, QUrl(url).host()});
    }
    // The remedy travels with the reason: this is the one idle cause a person
    // can fix, and it arises without anyone editing a remote by hand, because
    // deleting a mirror node strips that node's remote from every checkout.
    if (remotes.isEmpty())
        return idle(QStringLiteral(
                        "%1 has no ssh:// push remote, so the SSH-fed mirrors "
                        "receive nothing. Add the gateway's push remote under "
                        "Remotes on this repository")
                        .arg(pushSource));
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QList<MirrorRemote> targets;
    QStringList cooling;
    for (const MirrorRemote &remote : remotes) {
        if (userInitiated ||
            m_sshMirrorHealth.readyToPush(remote.host, nowMs))
            targets.append(remote);
        else
            cooling.append(remote.host);
    }
    // The refs a cooling gateway did not get are not queued anywhere, so wake
    // up the instant the earliest one is eligible again instead of leaving it
    // to the periodic pass — that pass is the dropped-event safety net, not
    // the cadence a push is supposed to reach the fleet on.
    scheduleSshMirrorPushRetry(repoKey, cooling, nowMs);
    if (targets.isEmpty())
        return idle(QStringLiteral("all %1 mirror gateways are cooling down "
                                   "after a failed push")
                        .arg(remotes.size()));
    // Serialize per gateway, not per repository. An unreachable node holds its
    // `git push` open for as long as ssh takes to give up, and a repo-wide
    // in-flight flag made every healthy mirror wait behind it before seeing the
    // commit that just landed — one sick node set the pace for the whole fleet.
    const auto pushKeyFor = [&repoKey](const QString &url) {
        return repoKey + QLatin1Char('\x1f') + url;
    };
    QList<MirrorRemote> ready;
    for (const MirrorRemote &remote : targets) {
        if (!m_sshMirrorPushing.contains(pushKeyFor(remote.url)))
            ready.append(remote);
    }
    if (ready.size() < targets.size()) {
        m_sshMirrorPushPending.insert(repoKey);
        if (userInitiated)
            flashMessage(
                QStringLiteral(
                    "A mirror push is already running for %1; the newest "
                    "release state is queued next.")
                    .arg(repoKey));
    }
    if (ready.isEmpty())
        return targets.size();
    targets = ready;
    // The Git reads above pump the GUI event loop. Recheck the durable token
    // before starting any release process in case a newer metadata commit
    // superseded this callback while its remotes were being inspected.
    if (managedProduct && !releaseTag.trimmed().isEmpty() &&
        !releasePublicationPending(repo.owner, repo.name, releaseTag)) {
        return idle(QStringLiteral(
            "a newer release publication superseded this ref push"));
    }
    for (const MirrorRemote &remote : targets)
        m_sshMirrorPushing.insert(pushKeyFor(remote.url));
    auto remaining = std::make_shared<int>(targets.size());
    auto failures = std::make_shared<int>(0);
    auto divergent = std::make_shared<int>(0);
    const auto finishPush =
        [this, repoKey, remaining, failures, divergent, userInitiated,
         releaseTag, remoteCount = targets.size()](const QString &pushKey,
                                                   bool ok, bool diverged) {
        // Free this gateway's slot as it finishes, not when the slowest one in
        // the batch does: the next commit must be able to reach a healthy
        // mirror while a dead one is still timing out.
        m_sshMirrorPushing.remove(pushKey);
        if (!ok)
            ++*failures;
        else if (diverged)
            ++*divergent;
        if (--*remaining > 0)
            return;
        if (userInitiated) {
            const QString subject =
                releaseTag.trimmed().isEmpty() ? repoKey : releaseTag;
            if (*failures == 0) {
                QString message =
                    QStringLiteral("Pushed %1 to %2 SSH mirror%3.")
                        .arg(subject)
                        .arg(remoteCount)
                        .arg(remoteCount == 1 ? QString()
                                              : QStringLiteral("s"));
                if (*divergent > 0)
                    message += QStringLiteral(
                                   " %1 mirror%2 kept a branch that is ahead "
                                   "of this checkout; it syncs once the "
                                   "source catches up.")
                                   .arg(*divergent)
                                   .arg(*divergent == 1
                                            ? QString()
                                            : QStringLiteral("s"));
                flashMessage(message);
            } else {
                flashMessage(
                    QStringLiteral(
                        "Pushed %1 to %2 of %3 SSH mirrors; %4 failed. Check "
                        "the network log for details.")
                        .arg(subject)
                        .arg(remoteCount - *failures)
                        .arg(remoteCount)
                        .arg(*failures),
                    true);
            }
        }
        if (!m_sshMirrorPushPending.remove(repoKey))
            return;
        QTimer::singleShot(0, this, [this, repoKey]() {
            for (int i = 0; i < m_repositories.size(); ++i) {
                const RepositoryRecord &candidate = m_repositories.at(i);
                if (candidate.owner + "/" + candidate.name == repoKey) {
                    pushToSshMirrorRemotes(i);
                    return;
                }
            }
        });
    };
    if (userInitiated)
        flashMessage(
            QString::fromUtf8("Pushing %1 to %2 SSH mirror%3\xE2\x80\xA6")
                .arg(releaseTag.trimmed().isEmpty() ? repoKey : releaseTag)
                .arg(targets.size())
                .arg(targets.size() == 1 ? QString() : QStringLiteral("s")));
    for (const MirrorRemote &remote : targets) {
        const QString url = remote.url;
        auto *process = new QProcess(this);
        // Never let an unreachable/unauthorized gateway hang the push on an
        // interactive credential or host-key prompt: this runs unattended.
        // Bound the dialling too — a node whose host has gone away black-holes
        // the SYN, and ssh's default is to wait out the kernel's TCP retries
        // (minutes). That is how long the gateway then stayed marked in flight
        // and how long its first failure took to reach the health tracker, so
        // the fleet's whole cooldown machinery only engaged minutes late. The
        // keepalives cover the other half: a session that connects and then
        // stops answering ends in a minute instead of hanging indefinitely.
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
        if (!env.contains(QStringLiteral("GIT_SSH_COMMAND")))
            env.insert(QStringLiteral("GIT_SSH_COMMAND"),
                       QStringLiteral("ssh -oBatchMode=yes -oConnectTimeout=15 "
                                      "-oServerAliveInterval=15 "
                                      "-oServerAliveCountMax=4"));
        process->setProcessEnvironment(env);
        const QString gatewayHost = remote.host;
        const QString gatewayRemote = remote.name;
        const QString pushKey = pushKeyFor(url);
        connect(process, &QProcess::finished, this,
                [this, process, repoKey, pushKey, finishPush, gatewayHost,
                 gatewayRemote](int exitCode, QProcess::ExitStatus) {
                    const QString output =
                        QString::fromUtf8(process->readAllStandardOutput());
                    const QString errors =
                        QString::fromUtf8(process->readAllStandardError())
                            .trimmed();
                    process->deleteLater();
                    const MirrorPushOutcome outcome =
                        classifyMirrorPush(output);
                    if (exitCode != 0 && !outcome.onlyHeldBack) {
                        QStringList detail;
                        for (const QString &line :
                             errors.split(QLatin1Char('\n'))) {
                            if (!line.trimmed().startsWith(
                                    QLatin1String("hint:")))
                                detail.append(line.trimmed());
                        }
                        const QString reason = detail.join(QLatin1Char(' '))
                                                   .trimmed()
                                                   .left(300);
                        if (mirrorGatewayUnreachable(errors)) {
                            const MirrorGatewayHealth::Notice notice =
                                m_sshMirrorHealth.noteUnreachable(
                                    gatewayHost,
                                    QDateTime::currentMSecsSinceEpoch());
                            if (notice.announce)
                                logSystem(
                                    QStringLiteral(
                                        "Mirror: SSH gateway %1 did not answer "
                                        "for %2: %3. Skipping it for about %4 "
                                        "minute%5 while the rest of the fleet "
                                        "keeps syncing.")
                                        .arg(gatewayHost, repoKey, reason)
                                        .arg(qMax<qint64>(
                                            1, notice.cooldownMs / 60000))
                                        .arg(notice.cooldownMs < 120000
                                                 ? QString()
                                                 : QStringLiteral("s")));
                            if (notice.retired)
                                logSystem(
                                    QStringLiteral(
                                        "Mirror: SSH gateway %1 has not "
                                        "answered for over a day, so it is now "
                                        "probed every %2 hours instead of every "
                                        "pass and this is the last notice about "
                                        "it. If that node was retired for good, "
                                        "drop it from the fan-out — delete the "
                                        "\"%3\" remote on the repository's "
                                        "Remotes tab, or run \"git remote "
                                        "remove %3\" in the working copy.")
                                        .arg(gatewayHost)
                                        .arg(MirrorGatewayHealth::
                                                 kRetiredCooldownMs /
                                             (60 * 60 * 1000))
                                        .arg(gatewayRemote));
                            if (notice.persist)
                                saveSshMirrorGatewayHealth();
                            // This failed pass is the event that starts the
                            // cooldown. Queue its retry now; waiting for some
                            // later commit to discover the cooling gateway
                            // stranded the current refs until the periodic
                            // safety sweep.
                            scheduleSshMirrorPushRetry(
                                repoKey, QStringList{gatewayHost},
                                QDateTime::currentMSecsSinceEpoch());
                            finishPush(pushKey, false, false);
                            return;
                        }
                        logSystem(QStringLiteral(
                                      "Mirror: SSH mirror push of %1 to %2 "
                                      "failed: %3")
                                      .arg(repoKey, gatewayHost, reason));
                        finishPush(pushKey, false, false);
                        return;
                    }
                    if (m_sshMirrorHealth.noteReachable(gatewayHost)) {
                        logSystem(QStringLiteral("Mirror: SSH gateway %1 is "
                                                 "answering again.")
                                      .arg(gatewayHost));
                        saveSshMirrorGatewayHealth();
                    }
                    if (!outcome.updated.isEmpty())
                        logSystem(QStringLiteral(
                                      "Mirror: pushed %1 to SSH mirror %2.")
                                      .arg(repoKey, gatewayHost));
                    static QHash<QString, QString> s_heldBackReported;
                    const QString heldKey =
                        repoKey + QLatin1Char(' ') + gatewayHost;
                    const QString held =
                        outcome.heldBack.join(QStringLiteral(", "));
                    if (held.isEmpty()) {
                        s_heldBackReported.remove(heldKey);
                    } else if (s_heldBackReported.value(heldKey) != held) {
                        s_heldBackReported.insert(heldKey, held);
                        logSystem(
                            QStringLiteral(
                                "Mirror: SSH mirror %1 is ahead of this node on "
                                "%2 for %3, so those branches were left alone "
                                "(a mirror push never rewinds one). Every other "
                                "branch synced; they retry once this node "
                                "catches up.")
                                .arg(gatewayHost, held, repoKey));
                    }
                    finishPush(pushKey, true, !outcome.heldBack.isEmpty());
                });
        connect(process, &QProcess::errorOccurred, this,
                [this, process, repoKey, pushKey, finishPush,
                 gatewayHost](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart)
                        return;
                    process->deleteLater();
                    logSystem(QStringLiteral("Mirror: could not run git to "
                                             "push %1 to SSH mirror %2.")
                                  .arg(repoKey, gatewayHost));
                    finishPush(pushKey, false, false);
                });
        // Push from the served bare mirror (or the working copy when this is the
        // source of truth), but never let an unattended desktop rewind or delete
        // a branch that advanced on the gateway while this checkout was offline.
        // Automatic propagation therefore uses ordinary fast-forward refspecs
        // and explicitly disables force. Pruning is enabled only after the
        // source has successfully read every exact mirror route; ordinary
        // event-driven pushes leave mirror-only refs intact. Keep the update
        // non-atomic: a mirror can
        // legitimately advance one collaboration branch (for example
        // forkmesh/pulls) before the source consumes it. That one protected
        // non-fast-forward must not reject an otherwise clean main update and
        // leave the entire headless fleet stale. Git still rejects only that
        // divergent ref while advancing every safe ref, and the next sync retries
        // it after the source converges. An intentional rewrite or branch
        // deletion must go through an explicit, reviewed Git operation;
        // otherwise one stale safety sync can undo a clean main merge on
        // every headless mirror.
        trackProcessActivity(process, QStringLiteral("push"),
                             QStringLiteral("Pushing %1/%2 to %3")
                                 .arg(repo.owner, repo.name, url));
        // Pull compatibility refs are rebuildable pointers. A completed
        // reverse-sync pass may force/prune that namespace to the canonical
        // source while all durable heads and tags remain non-forcing.
        process->start(QStringLiteral("git"),
                       {QStringLiteral("-C"), pushSource,
                        QStringLiteral("push"), QStringLiteral("--porcelain"),
                        QStringLiteral("--no-force"),
                        pruneRefs ? QStringLiteral("--prune")
                                  : QStringLiteral("--no-prune"),
                        url, QStringLiteral("refs/heads/*:refs/heads/*"),
                        QStringLiteral("refs/tags/*:refs/tags/*"),
                        pruneRefs ? QStringLiteral("+refs/pr/*:refs/pr/*")
                                  : QStringLiteral("refs/pr/*:refs/pr/*")});
    }
    return targets.size();
}
