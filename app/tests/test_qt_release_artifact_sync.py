from pathlib import Path


# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "desktop" / "src" / "MainWindow.cpp"
INTERNAL = ROOT / "desktop" / "src" / "MainWindowInternal.h"
REPOS = ROOT / "desktop" / "src" / "MainWindowRepos.cpp"
RELEASES = ROOT / "desktop" / "src" / "MainWindowReleases.cpp"
HEADER = ROOT / "desktop" / "src" / "MainWindow.h"


def test_roster_artifact_adverts_trigger_release_blob_replication():
    source = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    start = source.index("void MainWindow::syncMirrorsBehindRoster()")
    end = source.index("void MainWindow::propagateRepoUpdate(int index)")
    body = source[start:end]

    assert "localArtifactCount = mirrorArtifactCount(repo.mirrorPath);" in body
    assert "bool artifactsBehind = false;" in body
    assert "m.artifactCount > localArtifactCount" in body
    assert "replicateReleaseArtifacts(i);" in body
    assert body.index("syncRepository(i, /*quiet=*/true);") < body.index(
        "replicateReleaseArtifacts(i);"
    )


def test_release_blob_no_node_hosts_is_an_answer_remembered_for_a_day():
    # candidate-1.2.2's uploads never landed on any node. Every launch and every
    # repository open asked the relay for its four blobs again, and each 404
    # was logged "net GET ERR 404 ... release fetch" and badged ERROR.
    source = RELEASES.read_text(encoding="utf-8")
    start = source.index("void MainWindow::replicateReleaseArtifacts(int index)")
    end = source.index("void MainWindow::pushReleaseToMirrors(const QString &tag)")
    body = source[start:end]

    assert "forkmesh::markExpectedHttpStatus(request, 404);" in body
    assert "if (releaseBlobRecentlyMissing(" in body
    assert body.index("if (releaseBlobRecentlyMissing(") < body.index(
        "missing.append({hash, downloadRepo});"
    )
    assert "noteReleaseBlobMissing(hash);" in body
    assert "QFile::remove(releaseBlobMissMarkerPath(hash));" in body
    # The marker lives outside the served CAS, which gateways and the mirror
    # push walk.
    marker = source[source.index("QString releaseBlobMissMarkerPath("):]
    marker = marker[:marker.index("}")]
    assert "forkmesh::paths::cacheRoot()" in marker
    assert "forkmesh-releases" not in marker


def test_direct_mirror_sync_uses_the_configured_jittered_interval():
    main = MAIN.read_text(encoding="utf-8")
    internal = INTERNAL.read_text(encoding="utf-8")
    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )

    assert "constexpr int kMirrorSyncIntervalDefaultMinutes = 1;" in internal
    assert "qint64 mirrorSyncIntervalMs()" in internal
    assert "constexpr int kMirrorSyncJitterPercent = 15;" in internal
    timer = main[
        main.index("void MainWindow::restartMirrorSyncTimer()")
        : main.index("MainWindow::MainWindow(QWidget *parent)")
    ]
    assert "const qint64 configuredIntervalMs = mirrorSyncIntervalMs();" in timer
    assert "kEventDrivenMirrorSafetyIntervalMs" in timer
    assert "m_nodeEventSocket->isConnected()" in timer
    assert "qMin(configuredIntervalMs, kEventDrivenMirrorSafetyIntervalMs)" in timer
    assert "baseIntervalMs * kMirrorSyncJitterPercent / 100" in timer
    assert "bounded(-jitterSpanMs, jitterSpanMs + 1)" in timer
    assert "m_mirrorSyncTimer->start" in timer
    assert main.count("m_mirrorSyncTimer = new QTimer(this);") == 1
    assert "autoSyncMirrors();" in main
    assert "autoSyncMirrorsIfRelayHealthy" not in main
    assert "restartMirrorSyncTimer();" in main

    periodic = repos[
        repos.index("void MainWindow::autoSyncMirrors()")
        : repos.index("void MainWindow::syncMirrorsBehindRoster()")
    ]
    roster = repos[
        repos.index("void MainWindow::syncMirrorsBehindRoster()")
        : repos.index("void MainWindow::propagateRepoUpdate(int index)")
    ]
    peer = repos[
        repos.index("void MainWindow::onPeerMirrorUpdated(")
        : repos.index("void MainWindow::onPeerMirrorSynced(")
    ]
    assert "m_syncingRepos.contains(i)" in periodic
    assert "isSourceOfTruthRepo(repo)" in periodic
    assert "convergeSourceRepoFromMesh(i);" in periodic
    assert periodic.index("convergeSourceRepoFromMesh(i);") < periodic.index(
        "else if (!m_syncingRepos.contains(i))"
    )
    assert "*reconciliationFetched && index >= 0" in repos
    assert "m_syncingRepos.contains(i)" in roster
    assert "syncRepository(i, /*quiet=*/true);" in roster
    assert "if (!m_syncingRepos.contains(matchIndex))" in peer
    assert "syncRepository(matchIndex, /*quiet=*/true);" in peer


def test_reverse_convergence_survives_busy_writers_and_reads_exact_nodes():
    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    header = HEADER.read_text(encoding="utf-8")
    converge = repos[
        repos.index("void MainWindow::convergeSourceRepoFromMesh(")
        : repos.index("void MainWindow::propagateRepoUpdate(int index)")
    ]

    assert "if (!m_sourceConvergePending.contains(key))" in converge
    assert "if (force && !m_sourceConvergePending.contains(key))" not in converge
    assert 'saved.value(QStringLiteral("publicOrigin"))' in converge
    assert 'saved.value(QStringLiteral("dnsHostname"))' in converge
    assert 'publicOrigin.setPath(QStringLiteral("/%1/%2.git")' in converge
    assert "addMirrorRoute(publicOrigin.toString(QUrl::FullyEncoded), {}," in converge
    assert 'QStringLiteral("managed-host:") + node.toLower()' in converge
    assert "environment, fallbackGroup" in converge
    assert "QUrl::FullyEncoded), {},\n                               fallbackGroup" in converge
    assert "mirrorGatewayHostBelongsToNode(candidateHost," in converge
    assert "addMirrorRoute(candidate, {}, fallbackGroup);" in converge
    assert "QHash<QString, int> m_mirrorRefRaceRetries;" in header


def test_concurrent_source_ref_update_is_retried_before_reporting_failure():
    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    fetch = repos[
        repos.index("void MainWindow::startSyncFetch(")
        : repos.index("void MainWindow::onProfileNameChanged(")
    ]

    assert 'QStringLiteral("cannot lock ref")' in fetch
    assert 'QStringLiteral("but expected")' in fetch
    assert "refRaceAttempt < 3" in fetch
    assert "250 * (refRaceAttempt + 1)" in fetch
    assert "syncRepository(i, /*quiet=*/true);" in fetch
    assert fetch.index("if (refTransactionRaced && refRaceAttempt < 3)") < fetch.index(
        "const bool transient = isTransientSyncError(errors);"
    )


def test_mirror_table_exposes_only_pending_source_backlog_columns():
    releases = RELEASES.read_text(encoding="utf-8")
    for name in (
        "MirrorNodeColIssuesToSource",
        "MirrorNodeColCommitsToSource",
        "MirrorNodeColBranchesToSource",
        "MirrorNodeColPullsToSource",
        "MirrorNodeColDiscussionsToSource",
    ):
        assert name in releases
    assert "An \\xE2\\x86\\x91 column is populated only when a mirror has unique work" in releases
    assert 'mirror.insert(QStringLiteral("branchHeads")' in releases
    assert "name=='forkmesh/pulls'" in releases
    assert "name.startswith('forkmesh/pull/')" in releases
    assert "not is_internal" in releases
    assert "isVisibleMirrorBranch(name)" in releases
    assert "!nodeHeads.isEmpty()" in releases
    assert "!catHeads.isEmpty()" in releases
    assert "makeSyncBackCell(backlog." in releases


def test_authenticated_ssh_metadata_counts_physical_release_objects():
    releases = RELEASES.read_text(encoding="utf-8")
    program = releases[
        releases.index("QByteArray sshMirrorMetadataProgram()")
        : releases.index("bool verifiedRepositoryHealth(")
    ]

    assert "release_store=str(candidate.get('releaseStore') or '')" in program
    assert "release_store=os.path.join(git_dir,'forkmesh-releases')" in program
    assert "release_root=os.path.join(release_store,'sha256')" in program
    assert "os.scandir(release_root)" in program
    assert "data_info=os.lstat(os.path.join(blob.path,'data'))" in program
    assert "stat.S_ISREG(data_info.st_mode)" in program
    assert "path.startswith('.forkmesh/artifacts/')" not in program


def test_release_dialog_defers_pruning_until_replacement_artifacts_land():
    source = RELEASES.read_text(encoding="utf-8")
    prompt = source[
        source.index("void MainWindow::promptNewRelease()")
        : source.index("void MainWindow::showReleaseDetail")
    ]
    prune = source[
        source.index("void MainWindow::pruneReleaseArtifacts(")
        : source.index("void MainWindow::loadReleasesPanel")
    ]
    actions = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(
        encoding="utf-8"
    )
    header = HEADER.read_text(encoding="utf-8")

    assert "void pruneReleaseArtifacts(int index, const QString &releaseTag," in header
    assert "new QCheckBox(" in prompt
    assert "Delete previous release artifacts from this node" in prompt
    assert "forkmesh-releases/sha256" in prompt
    assert "form->addRow(QString(), pruneArtifactsCheck);" in prompt
    assert "pruneArtifactsCheck->isChecked()" in prompt
    assert "rememberReleaseArtifactPrune(" in prompt
    assert prompt.index("user.email=actions@forkmesh.local") < prompt.index(
        "rememberReleaseArtifactPrune("
    )
    assert "keeping the current release available" in prompt
    assert "mirrorReleaseBlobs(mirrorPath)" in prune
    assert "retained.contains(blob.hash.toLower())" in prune
    assert "forkmesh::safedelete::removeTree(hashDir.absolutePath()" in prune
    assert "QLocale().formattedDataSize(bytesDeleted)" in prune
    assert "m_mirrorAdvertSig.clear();" in prune
    assert "refreshRepositoryList();" in prune
    assert "publishRepository(" not in prune
    assert "publishedRelease.artifactSha256s" in actions
    assert "releaseArtifactPrunePending(" in actions
    assert "clearReleaseArtifactPrune(" in actions


def test_release_channel_scans_ignore_files_in_the_metadata_root():
    source = RELEASES.read_text(encoding="utf-8")

    # Both the Artifacts view and background blob replication scan the Git
    # tree. The metadata root contains README.md alongside channel directories;
    # preserving ls-tree's type field lets both callers skip that blob instead
    # of probing the impossible README.md/release.json path on every refresh.
    assert source.count('!raw.left(tab).contains(" tree ")') == 2
    assert 'QStringLiteral("--name-only")' not in source
    assert source.count("QString::fromUtf8(raw.mid(tab + 1)).trimmed()") == 2


def test_release_tag_sets_a_tagger_identity_without_global_git_config():
    source = RELEASES.read_text(encoding="utf-8")
    prompt = source[
        source.index("void MainWindow::promptNewRelease()")
        : source.index("void MainWindow::showReleaseDetail")
    ]

    assert '"-c", QStringLiteral("user.email=actions@forkmesh.local")' in prompt
    assert '"-c", QStringLiteral("user.name=ForkMesh Actions")' in prompt
    assert '"tag",\n                                "-a", tag, targetRef, "-m", message' in prompt


def test_releases_table_has_per_release_push_to_mirrors_action():
    source = RELEASES.read_text(encoding="utf-8")
    build = source[
        source.index("QWidget *MainWindow::buildReleasesTab()")
        : source.index("QWidget *MainWindow::buildArtifactsTab()")
    ]
    load = source[
        source.index("void MainWindow::loadReleasesPanel()")
        : source.index("void MainWindow::fetchReleaseDownloadCounts")
    ]

    assert "new QTableWidget(0, 11)" in build
    assert '"Downloads", "Mirrors", ""' in build
    assert 'new QPushButton("Push to mirrors")' in load
    assert "pushReleaseToMirrors(tag);" in load
    assert "setCellWidget(row, 9, pushMirrors)" in load
    assert "setCellWidget(row, 10, del)" in load


def test_release_links_preserve_the_manifest_asset_filename():
    source = RELEASES.read_text(encoding="utf-8")
    load = source[
        source.index("void MainWindow::loadReleasesPanel()")
        : source.index("void MainWindow::fetchReleaseDownloadCounts")
    ]

    assert "releases/blob/sha256/%3?filename=%4" in load
    assert "QUrl::toPercentEncoding(name)" in load


def test_release_push_authenticates_and_seeds_managed_hosts_before_other_refs():
    source = RELEASES.read_text(encoding="utf-8")
    push = source[
        source.index("void MainWindow::pushReleaseToMirrors")
        : source.index("void MainWindow::promptNewRelease()")
    ]

    assert 'QStringLiteral("refs/tags/") + tag' in push
    assert 'tagRef + QStringLiteral("^{commit}")' in push
    assert "publishedReleaseDeployment(index, tag, commit" in push
    assert "publishedRelease.artifactSha256s" in push
    assert "rememberReleasePublication(repo.owner, repo.name, tag);" in push
    assert "pushReleaseToManagedHosts(" in push
    assert "clearReleasePublication(owner, name, tag);" in push
    assert "propagateRepoUpdate(currentIndex," in push
    assert "pushToSshMirrorRemotes(" in push
    callback = push[push.index("const int managedTargets =") :]
    assert callback.index("pushConfigured(succeeded)") < callback.index(
        "clearReleasePublication(owner, name, tag);"
    )
    assert "notifyMirrorUpdated(" not in push
    ssh_source = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    ssh_push = ssh_source[
        ssh_source.index(
            "int MainWindow::pushToSshMirrorRemotes(int index, bool userInitiated"
        )
        : ssh_source.index("void MainWindow::syncRepository")
    ]
    assert "const QString pushSource = repo.localPath;" in ssh_push
    assert 'QStringLiteral("refs/heads/*:refs/heads/*")' in ssh_push
    assert 'QStringLiteral("refs/tags/*:refs/tags/*")' in ssh_push
    assert 'QStringLiteral("refs/pr/*:refs/pr/*")' in ssh_push


def test_release_that_reaches_no_ssh_mirror_names_the_cause_and_logs_it():
    """A release fan-out that pushed nothing must say why, and leave a trace.

    The relay broadcast wakes desktop peers only; the SSH-fed mirrors advance
    on this push alone. Reporting every idle cause as "no SSH push remotes
    configured" sent people to edit a remote list that was already correct,
    and the repo-detail notice fades, so a release that reached zero gateways
    left no record anywhere.
    """
    source = RELEASES.read_text(encoding="utf-8")
    push = source[
        source.index("void MainWindow::pushReleaseToMirrors")
        : source.index("void MainWindow::promptNewRelease()")
    ]
    idle = push[push.index("if (targets == 0) {") :]

    assert "QString idleReason;" in push
    assert "&idleReason)" in push
    # The reason reaches both surfaces, and neither hard-codes a cause.
    assert "setRepoDetailNotice(" in idle
    assert "logSystem(" in idle
    assert ".arg(tag, idleReason)" in idle
    assert idle.count(".arg(tag, idleReason)") == 2
    assert "this working copy has no" not in push
    # An empty reason must still read as a sentence rather than a dangling colon.
    assert "idleReason.trimmed().isEmpty()" in idle
    assert "pushReleaseToManagedHosts(" in push
    assert "publishedRelease.artifactSha256s" in push
    assert "const int targets = managedHosts + sshRemotes;" in push

    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    ssh_push = repos[
        repos.index(
            "int MainWindow::pushToSshMirrorRemotes(int index, bool userInitiated"
        ) : repos.index("void MainWindow::syncRepository")
    ]
    # Every "pushed nothing" exit names itself; the only bare `return 0` left
    # is the idle() helper's own, which sets the reason before returning.
    assert "QString *idleReason" in ssh_push
    assert ssh_push.count("return idle(") == 9
    assert ssh_push.count("return 0;") == 1
    assert "*idleReason = reason;" in ssh_push
    assert "no ssh:// push remote" in ssh_push
    # The one cause a person can act on carries its remedy.
    assert "Add the gateway's push remote under" in ssh_push


def test_checked_release_rollout_waits_for_and_deploys_the_exact_artifact():
    """The checkbox is an artifact deployment request, not only a tag push."""
    releases = RELEASES.read_text(encoding="utf-8")
    actions = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(
        encoding="utf-8"
    )
    chat = "".join(
        (ROOT / "desktop/src" / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_CHAT_SOURCES
    )
    installer = (ROOT / "www/public/install.sh").read_text(encoding="utf-8")

    prompt = releases[
        releases.index("void MainWindow::promptNewRelease()")
        : releases.index("void MainWindow::showReleaseDetail")
    ]
    assert "repo.actionsEnabled" in prompt
    assert 'QLatin1String("forkmesh")' in prompt
    assert "forkmesh::releasedraft::deployableTag(cut.tag)" in prompt
    assert (
        "rememberReleaseRollout(rolloutRepo.owner, rolloutRepo.name, tag);" in prompt
    )
    assert "pushReleaseToMirrors(tag);" not in prompt
    assert "after all signed artifacts land" in prompt

    landed = actions[
        actions.index("void MainWindow::onReleaseMetadataLanded")
        : actions.index("void MainWindow::refreshOpenPullChecks")
    ]
    for contract in (
        "releaseRolloutPending(run.owner, run.name, tag)",
        "publishedReleaseDeployment(index, tag, run.commit",
        "servedMirrorReady",
        "pushReleaseToManagedHosts(",
        "deployPendingReleaseRollout(owner, name, publishedRelease,",
        "publishedRelease.artifactSha256s",
    ):
        assert contract in landed
    assert landed.index("publishedReleaseDeployment(") < landed.index(
        "pushReleaseToManagedHosts("
    )
    assert landed.index("pushReleaseToManagedHosts(") < landed.index(
        "propagateRepoUpdate(currentIndex,"
    )
    assert landed.index("propagateRepoUpdate(currentIndex,") < landed.index(
        "deployPendingReleaseRollout("
    )
    assert "refreshRepositoryMirror(index);" not in landed

    command = chat[
        chat.index("bool MainWindow::buildHostInstallCommand(")
        : chat.index("void MainWindow::runHostInstall(")
    ]
    assert "publishedRelease.buildCommit" in command
    assert "publishedRelease.version" in command
    assert "publishedRelease.manifestSha256" in command
    assert ".linuxX86_64Sha256" in command
    assert ".linuxArm64Sha256" in command
    assert "/etc/forkmesh/mirror-node.json" in command
    assert "/var/lib/forkmesh/mirror-node.json" in command
    assert "Native mirror-node checksum verification failed" in command
    assert "systemctl restart forkmesh-mirror-node.service" in command
    assert command.index("is already deployed and running") < command.index(
        "blob_base=%3; "
    )
    assert "http://127.0.0.1:8791/v1/status" in command
    assert "shq(expectedReleaseVersion)" in command
    assert "shq(expectedReleaseManifestDigest)" in command
    assert '--set-version "$FORKMESH_EXPECTED_RELEASE_VERSION"' in installer


def test_managed_release_fanout_seeds_signed_cas_before_advancing_refs():
    """A new manifest must never outrun its out-of-Git artifact bytes."""
    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    actions = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(
        encoding="utf-8"
    )
    direct = repos[
        repos.index("int MainWindow::pushReleaseToManagedHosts(")
        : repos.index("void MainWindow::scheduleSshMirrorPushRetry")
    ]
    deployment_parser = RELEASES.read_text(encoding="utf-8")
    deployment_parser = deployment_parser[
        deployment_parser.index("bool MainWindow::publishedReleaseDeployment(")
        : deployment_parser.index("void MainWindow::promptNewRelease()")
    ]

    for contract in (
        "const QStringList &artifactHashes",
        "const QStringList &releaseArtifactHashes",
        "mirrorReleaseBlobPath(repo.mirrorPath, hash)",
        "managedReleaseArtifactSeedCommand(hash)",
        "seed->setStandardInputFile(artifact.path);",
        "artifact seeding failed",
        "refs and deployment remain pending",
        "pushRefsToManagedHosts(\n                index, wakeIfUnchanged, releaseTag, {}, onFinished,\n                /*reconcileArtifacts=*/false)",
        "releasePublicationPendingForRepository(repo.owner, repo.name)",
        "releasePublicationPending(repo.owner, repo.name, releaseTag)",
    ):
        assert contract in direct
    seed_phase = direct.index("if (releasePush && !releaseArtifacts.isEmpty())")
    ref_phase = direct.index("auto remaining = std::make_shared<int>(targets.size())")
    git_push = direct.index('process->start(\n            QStringLiteral("git")')
    assert seed_phase < ref_phase < git_push
    assert "return targets.size();" in direct[seed_phase:ref_phase]
    # Each QProcess completion continues the per-host seed chain.  The chain
    # may release its last owning signal closure while that std::function is
    # executing, so its body must pin itself through a weak reference.  The old
    # self-owning callback cleared itself in its completion branch, destroyed
    # its captures in-place, and then copied finishTarget from freed storage.
    assert (
        "const std::weak_ptr<std::function<void()>> weakSeedNext = seedNext;" in direct
    )
    assert "const auto self = weakSeedNext.lock();" in direct
    assert "[this, seed, finished, self, finishTarget," in direct
    assert "(*self)();" in direct
    assert "*seedNext = {};" not in direct
    wrapper = direct[
        direct.index("int MainWindow::pushReleaseToManagedHosts(")
        : direct.index("int MainWindow::pushRefsToManagedHosts(")
    ]
    assert "artifactHashes.isEmpty()" in wrapper
    assert "authenticated artifact set" in wrapper
    assert "(!releasePush && outcome.onlyHeldBack)" in direct
    assert "if (assets.isEmpty())" in deployment_parser
    assert "invalid signed asset" in deployment_parser
    assert "artifactSha256s.append(assetHash)" in deployment_parser

    landed = actions[
        actions.index("void MainWindow::onReleaseMetadataLanded")
        : actions.index("void MainWindow::refreshOpenPullChecks")
    ]
    assert "index, tag, publishedRelease.artifactSha256s," in landed
    assert landed.index("if (failed > 0)") < landed.index(
        "propagateRepoUpdate(currentIndex,"
    )
    failure = landed[
        landed.index("if (failed > 0)")
        : landed.index("Q_UNUSED(succeeded);")
    ]
    assert "deployment has not started" in failure
    assert "return;" in failure
    assert landed.index("propagateRepoUpdate(currentIndex,") < landed.index(
        "clearReleasePublication("
    )


def test_pending_release_publication_blocks_ordinary_ref_fanout():
    """The metadata commit hook must not race the artifact-seeding callback."""
    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    releases = RELEASES.read_text(encoding="utf-8")
    actions = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(
        encoding="utf-8"
    )
    runner = (ROOT / "desktop/src/ActionRunner.cpp").read_text(encoding="utf-8")

    configured = repos[
        repos.index(
            "int MainWindow::pushToSshMirrorRemotes(int index, bool userInitiated"
        )
        : repos.index("void MainWindow::syncRepository")
    ]
    managed = repos[
        repos.index("int MainWindow::pushRefsToManagedHosts(")
        : repos.index("void MainWindow::scheduleSshMirrorPushRetry")
    ]
    assert "releasePublicationPendingForRepository(repo.owner, repo.name)" in configured
    assert "releaseTag.trimmed().isEmpty()" in configured
    assert "releasePublicationPending(repo.owner, repo.name, releaseTag)" in configured
    assert configured.count(
        "managedProduct && !releaseTag.trimmed().isEmpty()"
    ) == 2
    assert "releasePublicationPendingForRepository(repo.owner, repo.name)" in managed
    assert "if (!releasePush" in managed

    intents = releases[
        releases.index("void MainWindow::rememberReleaseRollout(")
        : releases.index("bool MainWindow::publishedReleaseDeployment(")
    ]
    assert "kPendingReleasePublicationsSetting" in intents
    assert "clearReleasePublication" in intents
    prepared = actions[
        actions.index("void MainWindow::onReleaseMetadataPrepared(")
        : actions.index("void MainWindow::onReleaseMetadataLanded(")
    ]
    assert prepared.index("discardSupersededReleaseIntents(") < prepared.index(
        "rememberReleasePublication("
    )
    assert "rememberReleasePublication(" in prepared
    completion = runner[
        runner.index("const bool landed =") : runner.index("bool ActionRunner::isReleaseRun")
    ]
    assert completion.index("emit releaseMetadataPrepared") < completion.index(
        "finishComplete(true"
    )
    artifact_failure = completion[
        completion.index("if (!landing.artifactsOk)") : completion.index(
            "finishComplete(true"
        )
    ]
    assert "landed);" in artifact_failure


def test_unchanged_release_rerun_resumes_the_durable_publication():
    """Replenishing CAS with the same manifest must retry a pending rollout."""
    runner = (ROOT / "desktop/src/ActionRunner.cpp").read_text(encoding="utf-8")
    landing = runner[
        runner.index("bool ActionRunner::landReleaseMetadata()")
        : runner.index("bool ActionRunner::landVersionHeader()")
    ]
    unchanged = landing[
        landing.index('QStringLiteral("diff")') : landing.index(
            "const QString tag ="
        )
    ]

    assert "Release metadata unchanged; continuing" in unchanged
    assert "return true;" in unchanged


def test_new_release_metadata_discards_older_retry_intents():
    """An old failed rollout must not deploy after a newer release lands."""
    releases = RELEASES.read_text(encoding="utf-8")
    cleanup = releases[
        releases.index("void MainWindow::discardSupersededReleaseIntents(")
        : releases.index("bool MainWindow::releasePublicationPending(")
    ]

    assert "kPendingReleaseRolloutsSetting" in cleanup
    assert "kPendingReleasePublicationsSetting" in cleanup
    assert "kPendingReleaseArtifactPrunesSetting" in cleanup
    assert "parts.at(2).toString() != wantedTag" in cleanup


def test_release_fanout_targets_installed_saved_hosts_without_git_remotes():
    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    direct = repos[
        repos.index("int MainWindow::pushReleaseToManagedHosts(")
        : repos.index("void MainWindow::scheduleSshMirrorPushRetry")
    ]

    for contract in (
        "loadSavedHosts(",
        "savedHostMayBeActive(saved)",
        "buildHostSshCommand(",
        "savedHostIdentityFile(node, host, user)",
        'QStringLiteral("GIT_SSH_COMMAND")',
        'QStringLiteral("--no-force")',
        'QStringLiteral("--prune")',
        'QStringLiteral("--no-prune")',
        "pruneRefs ?",
        "--receive-pack=runuser -u forkmesh-node -- git-receive-pack",
        'QStringLiteral("refs/heads/*:refs/heads/*")',
        'QStringLiteral("refs/tags/*:refs/tags/*")',
        'QStringLiteral("refs/pr/*:refs/pr/*")',
    ):
        assert contract in direct
    assert 'QStringLiteral("--force")' not in direct


def test_every_source_push_targets_saved_hosts_and_wakes_native_daemons():
    """Ordinary code pushes use the same durable fleet as releases.

    Native hosts are not necessarily represented by a git remote, and writing
    their bare repository does not itself run the daemon cycle. Both edges must
    be explicit or the new refs wait for the two-minute reconciliation pass.
    """
    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    automatic = repos[
        repos.index("void MainWindow::pushToSshMirrorRemotes(int index)")
        : repos.index("int MainWindow::pushReleaseToManagedHosts(")
    ]
    managed = repos[
        repos.index("int MainWindow::pushRefsToManagedHosts(")
        : repos.index("void MainWindow::scheduleSshMirrorPushRetry")
    ]

    assert "/*reconcileArtifacts=*/true, pruneRefs" in automatic
    assert "loadSavedHosts(" in managed
    assert 'QStringLiteral("refs/heads/*:refs/heads/*")' in managed
    assert 'QStringLiteral("refs/tags/*:refs/tags/*")' in managed
    assert 'QStringLiteral("refs/pr/*:refs/pr/*")' in managed
    assert "http://127.0.0.1:8791/v1/control/sync" in managed
    # The reconciliation body lives in its own helper; the call site in
    # pushRefsToManagedHosts must not return in front of the Git phase either.
    ordinary_artifacts = managed[
        managed.index("// Ordinary fan-out heals the complete retained CAS")
        : managed.index("// A release manifest authorizes blobs")
    ] + managed[
        managed.index("void MainWindow::startManagedArtifactReconciliation(")
        : managed.index("int MainWindow::seedManagedReleaseArtifacts(")
    ]
    assert "managedReleaseArtifactInventoryCommand(" in managed
    assert "m_managedArtifactReconciling.contains(managedRepoKey)" in managed
    assert "m_managedArtifactReconciling.insert(managedRepoKey);" in managed
    assert "m_managedArtifactReconciling.remove(managedRepoKey);" in managed
    assert "inventory->start(" in ordinary_artifacts
    assert "Restoring artifact" in ordinary_artifacts
    # Inventory and repair are asynchronous side work. They cannot return in
    # front of the ordinary Git phase and make a just-created commit wait for
    # retained release blobs to be hashed.
    assert "return targets.size();" not in ordinary_artifacts
    ref_fanout = managed[
        managed.index("auto remaining = std::make_shared<int>(targets.size())") :
    ]
    assert "finishGit(true" in ref_fanout
    assert "wake->start(target.wakeCommand.program" in ref_fanout
    assert "classifyMirrorPush(" in ref_fanout
    assert "!outcome.updated.isEmpty()" in ref_fanout
    assert "if (!refsChanged && (!gitOk || !wakeIfUnchanged))" in ref_fanout
    assert ref_fanout.index("if (!refsChanged &&") < ref_fanout.index(
        "auto *wake = new QProcess"
    )
    # A partial multi-ref push can still advance main, so its daemon is woken
    # even when Git reports a held-back collaboration branch.
    assert ref_fanout.index("auto *wake = new QProcess") < ref_fanout.index(
        "if (gitOk && wakeOk)"
    )
    periodic = repos[repos.index("void MainWindow::syncRepository") :]
    assert (
        "pushToSshMirrorRemotes(\n"
        "                                index, /*wakeManagedIfUnchanged=*/false);"
        in periodic
    )

    actions = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(
        encoding="utf-8"
    )
    release_rollout = actions[
        actions.index("void MainWindow::onReleaseMetadataLanded")
        : actions.index("void MainWindow::refreshOpenPullChecks")
    ]
    assert (
        "propagateRepoUpdate(currentIndex,\n"
        "                                /*includeManagedHosts=*/false);"
        in release_rollout
    )


def test_source_convergence_reads_every_saved_managed_host_before_pruning():
    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    converge = repos[
        repos.index("void MainWindow::convergeSourceRepoFromMesh(")
        : repos.index("void MainWindow::propagateRepoUpdate(int index)")
    ]

    assert "loadSavedHosts(" in converge
    assert "buildHostSshCommand(" in converge
    assert "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git" in converge
    assert "convergeSourceCheckoutFromRoutes(" in converge
    assert "outcome.fetchOk && outcome.error.isEmpty()" in converge
    assert "*reconciliationComplete);" in converge


def test_pending_release_resume_prefers_the_newest_successful_publication():
    actions = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(
        encoding="utf-8"
    )
    restore = actions[
        actions.index("void MainWindow::restoreActionRuns()")
        : actions.index("void MainWindow::ensurePushHook(")
    ]

    assert "m_actionRuns.cbegin()" in restore
    assert "m_actionRuns.cend()" in restore
    assert "releaseRolloutPending(run.owner, run.name" in restore
    assert "releaseArtifactPrunePending(" in restore
    assert "run.status != ActionStatus::Success &&" in restore
    assert "!publicationPending" in restore
    assert "discardSupersededReleaseIntents(" in restore
    assert "rememberReleasePublication(" in restore
    assert "onReleaseMetadataLanded(runId);" in restore
