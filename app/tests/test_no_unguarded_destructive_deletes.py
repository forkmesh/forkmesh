import functools
import os
import re
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

SCANNED = ("desktop", "app", "world", "tools", "deploy", "www/public", "marketplace",
           "native", "server", "conformance")

SOURCE_SUFFIXES = {".py", ".sh", ".bash", ".cpp", ".cc", ".h", ".hpp", ".go", ".swift", ".kt",
                   ".kts", ".dart", ".js", ".mjs", ".cjs", ".ts", ".rs", ".ps1", ".yml", ".yaml"}

TEST_PATH = re.compile(
    r"(^|/)(tests?|tests_[^/]+|__tests__|Tests|[A-Za-z]+Tests|testdata|fixtures)/"
    r"|(^|/)test_[^/]*$|_test\.(go|dart|py)$|(Test|Tests)\.(kt|swift)$|/src/test/"
    r"|\.test\.(js|ts|mjs)$")

HELPERS = {
    "tools/forkmesh_safe_delete.py",
    "tools/forkmesh-safe-rm.sh",
    "desktop/src/SafeDelete.h",
    "server/safedelete.go",
}


def _tok(word: str) -> str:
    return r"(?:QStringLiteral\(|QLatin1String\(|QByteArrayLiteral\()?[\"']" + word + r"[\"']\)?"


_SEP = r"\s*,\s*(?:QStringList\s*\{\s*|\{\s*|\[\s*)?"

RULES = {
    "rm -r": re.compile(
        r"\brm\s+(?:-[a-zA-Z]*\s+)*-[a-zA-Z]*[rR]"
        r"|" + _tok("rm") + _SEP + _tok("-[a-zA-Z]*[rR][a-zA-Z]*")
        + r"|" + _tok("rm") + r"\s*\)?\s*,\s*(?:QStringList\s*\{\s*|\{\s*)" + _tok("-[a-zA-Z]*[rR][a-zA-Z]*")),
    "shutil.rmtree": re.compile(r"\brmtree\s*\("),
    "QDir::removeRecursively": re.compile(r"\bremoveRecursively\s*\("),
    "std::filesystem::remove_all": re.compile(r"\bremove_all\s*\("),
    "os.RemoveAll": re.compile(r"\bRemoveAll\s*\("),
    "recursive file delete": re.compile(
        r"\bdeleteRecursively\s*\(|\bremoveItem\s*\(\s*at(Path)?\s*:|\b(rmSync|rmdirSync|rm)\s*\([^)]*recursive\s*:\s*true"
        r"|\.delete\s*\(\s*recursive\s*:\s*true|\brimraf\b|Remove-Item\b[^\n]*-Recurse"),
    "git clean -f": re.compile(r"\bgit\s+clean\s+-[a-zA-Z]*f|" + _tok("clean") + _SEP + _tok("-[a-zA-Z]*f[a-zA-Z]*")),
    "git reset --hard": re.compile(r"\breset\s+--hard\b|" + _tok("reset") + _SEP + _tok("--hard")),
    "git checkout -f": re.compile(r"\bcheckout\s+(-f|--force)\b|" + _tok("checkout") + _SEP + _tok("(-f|--force)")),
    "git worktree remove --force": re.compile(
        r"\bworktree\s+remove\s+(-f|--force)\b|" + _tok("remove") + _SEP + _tok("(-f|--force)")),
    "git branch -D": re.compile(r"\bbranch\s+-D\b|" + _tok("branch") + _SEP + _tok("-D")),
    "git push --force": re.compile(
        r"\bpush\s+(?:-\S+\s+)*(--force\b|-f\b|--mirror\b)|" + _tok("push") + r"[^\n]*" + _tok("(--force|-f|--mirror)")
        + r"|" + _tok("push") + r"[^\n]*[\"']\+refs/"),
    "git stash drop/clear": re.compile(r"\bstash\s+(drop|clear)\b|" + _tok("stash") + _SEP + _tok("(drop|clear)")),
    "git gc --prune=now": re.compile(r"--prune=now"),
    "git filter-branch": re.compile(r"\bfilter-branch\b|" + _tok("filter-branch")),
}

KEYWORDS = re.compile(
    r"\brm\b|rmtree|removeRecursively|remove_all|RemoveAll|deleteRecursively|removeItem|rmSync|rmdirSync"
    r"|recursive|rimraf|Remove-Item|\bclean\b|--hard|checkout|\bremove\b|\bbranch\b|\bpush\b|\bstash\b"
    r"|prune=now|filter-branch")

ALLOW: list[tuple[str, str, str]] = [
    ("desktop/src/DestructiveCommandReview.cpp",
     '^\\.arg\\(code\\(',
     'prompt text that names the command being reviewed; this file runs nothing'),
    ("desktop/src/DestructiveCommandReview.cpp",
     'QLatin1String\\("filter-(branch|repo)"\\)',
     'classifier match for the destructive-command review, not a command'),
    ("desktop/src/DestructiveCommandReview.cpp",
     'filter-branch keeps the old branches',
     'undo hint shown in the destructive-command review prompt'),
    ("desktop/src/DestructiveCommandReview.cpp",
     '^\\{?QStringLiteral\\("[a-z ]+"\\),\\s+QStringLiteral\\(',
     'kind label table for the destructive-command review settings, not commands'),
    ("desktop/src/DestructiveCommandReview.cpp",
     'QStringLiteral\\("(rm -r of folders|filter-branch, reflog expire)',
     'kind label table for the destructive-command review settings, not commands'),
    ("desktop/src/DestructiveCommandReview.cpp",
     'kind == QLatin1String\\("stash drop"\\)',
     'classifier label for the destructive-command review, not a command'),
    ("www/public/install-instance.sh",
     'git -C "\\$SRC" checkout -f -q --detach FETCH_HEAD',
     'the installer-owned deploy checkout of an instance; any local edits are first saved to a patch beside it'),
    ("desktop/src/MainWindowGlobalAgentRules.cpp",
     'Recursive deletes \\(rm -r, find -delete\\)',
     'user-facing help text naming the commands the destructive-command rule catches'),
    ("desktop/src/MainWindowGlobalAgentRules.cpp",
     'git reset --hard, git clean -f, git branch -D, dropped stashes',
     'user-facing help text naming the commands the destructive-command rule catches'),
    ("desktop/src/AgentNetworkGate.cpp",
     'QStringLiteral\\("stash drop"\\)',
     'classifier label for the destructive-command rule, not a command'),
    ("desktop/src/AgentNetworkGate.cpp",
     'sub == QLatin1String\\("filter-branch"\\)',
     'classifier match for the destructive-command rule, not a command'),
    ("desktop/src/AgentNetworkGate.cpp",
     'QLatin1String\\("--prune=now"\\)',
     'classifier match for the destructive-command rule, not a command'),
    ("desktop/src/TaskWorkItems.cpp",
     'QStringLiteral\\("remove"\\), QStringLiteral\\("--force"\\), inspection\\.worktreePath',
     'task delete removes its worktree only after backing it up to a trash ref'),
    ("server/intake.go",
     '"worktree", "remove", "--force", dir',
     'removes the temporary collaboration worktree this intake created'),
    ('app/src/crisis_preservation.py',
     'git push --mirror from your local clone',
     'user-facing guidance text, not a command'),
    ('deploy/core/join_agent.py',
     'rm -rf /tmp/fmnext-daemon',
     'literal /tmp staging folder on the joining fleet node'),
    ('desktop/install.sh',
     'rm -rf -- "\\$d" && echo "  removed \\$d"',
     'explicit --purge after agent-shell, HOME and checkout checks'),
    ('desktop/src/AgentRunner.cpp',
     'QStringLiteral\\("remove"\\), QStringLiteral\\("--force"\\), leaked\\}',
     'only agent worktrees under agentwt root, checked by refusal'),
    ('desktop/src/IssueStore.cpp',
     'filter-branch',
     'user-confirmed Delete with history of one issue'),
    ('desktop/src/IssueStore.cpp',
     '"rm", "-r", "--ignore-unmatch"|git rm -r --cached',
     'git rm of one issue folder, committed and recoverable'),
    ('desktop/src/IssueStore.cpp',
     '"gc", "--prune=now"',
     'user-confirmed Delete with history must prune the rewritten objects'),
    ('desktop/src/MainWindowAccounts.cpp',
     'sleep 1; rm -rf ',
     'confirmed uninstall of folders vetted by uninstallMayErase'),
    ('desktop/src/MainWindowAccounts.cpp',
     'QDir\\(d\\)\\.removeRecursively\\(\\);',
     'confirmed uninstall of folders vetted by uninstallMayErase'),
    ('desktop/src/AgentWorktree.h',
     'git worktree remove --force %1 2>/dev/null',
     'clears the agent worktree path this launcher creates'),
    ('desktop/src/MainWindowAgentLanding.cpp',
     'QStringLiteral\\("remove"\\), QStringLiteral\\("--force"\\), wtPath\\}',
     'runs only after wtPath is proven inside an agent root'),
    ('desktop/src/MainWindowBranches.cpp',
     '"branch", "-D", branch|QStringLiteral\\("-D"\\), branch',
     'user-confirmed delete branch, worktree or merged branches dialogs'),
    ('desktop/src/MainWindowBranches.cpp',
     'QStringLiteral\\("remove"\\), QStringLiteral\\("--force"\\), worktreePath',
     'user-confirmed removal of a linked agent worktree'),
    ('desktop/src/MainWindowBranches.cpp',
     '"reset", "--hard", beforeHead',
     'rolls back its own merge only after local edits are held in the autostash'),
    ('desktop/src/MainWindowBranches.cpp',
     '"stash", "drop", "stash@\\{0\\}"',
     'drops only the autostash it just re-applied, verified by object id'),
    ('desktop/src/MainWindowData.cpp',
     'QDir\\((path|p)\\)\\.removeRecursively\\(\\)',
     'typed or confirmed data wipe after userDataWipeRefusal check'),
    ('desktop/src/MainWindowHostInstall.cpp',
     'trap \'rm -rf \\\\\\"\\$stage\\\\\\"',
     'remote mktemp stage directory on the host'),
    ('desktop/src/MainWindowPulls.cpp',
     'filter-branch \\+ gc \\+ reflog expire',
     'comment describing the opt-in pull history purge'),
    ('desktop/src/MainWindowPulls.cpp',
     '"branch", "-D"',
     'explicit Delete pull request and its head branch action'),
    ('desktop/src/MainWindowReleases.cpp',
     '\\{"worktree", "remove", "--force", scratchTree\\}',
     'scratch worktree inside a QTemporaryDir this code made'),
    ('desktop/src/MainWindowRepoDetail.cpp',
     '"reset", "--hard"\\}',
     'undoes its own failed file commit on a tree verified clean'),
    ('desktop/src/MainWindowRepoDetail.cpp',
     '"worktree", "remove", "--force", work',
     'scratch worktree inside its own QTemporaryDir'),
    ('desktop/src/MainWindowRepoDetail.cpp',
     '`git branch -D` in a',
     'comment listing what can remove an agent branch'),
    ('desktop/src/MainWindowRepoDetail.cpp',
     '"filter-branch", "--force", "--env-filter"',
     'user-confirmed Reassign attribution identity rewrite'),
    ('desktop/src/MainWindowSetup.cpp',
     'runUpdateStepUser\\("rm", (QStringList\\{"-rf"\\} \\+ staleEntries|\\{"-rf", scratchDir\\})',
     'operands prechecked with safedelete refusal against buildDir'),
    ('desktop/src/MainWindowSourceControl.cpp',
     'QStringLiteral\\("clean"\\)|"clean", "-fd"',
     'user-confirmed discard of untracked changes; git keeps nested repositories'),
    ('desktop/src/PullStore.cpp',
     'filter-branch',
     'opt-in delete-with-history purge of pull ledger refs only'),
    ('desktop/src/PullStore.cpp',
     '"checkout", "-f", "HEAD", "--"',
     'restores only stale .forkmesh/pulls ledger paths from HEAD'),
    ('desktop/src/PullStore.cpp',
     'git rm -r --cached --ignore-unmatch',
     'index filter of the opt-in pull history purge'),
    ('desktop/src/PullStore.cpp',
     '"gc", "--prune=now"',
     'opt-in pull purge must drop the rewritten objects'),
    ('desktop/src/PullStore.cpp',
     '"worktree", "remove", "--force", (scratchDir|dir)',
     'removes scratch, edit and review worktrees this store created'),
    ('desktop/src/PullStore.cpp',
     '"branch", "-D", m_amBranch',
     'deletes only a pull branch this flow created; existing tips are restored'),
    ('desktop/src/PullStore.cpp',
     '"rm", "-r", "--ignore-unmatch"',
     'git rm of one pull folder in the ledger, committed and recoverable'),
    ('desktop/src/TaskWorkItems.cpp',
     'QStringLiteral\\("-D"\\)|git branch -D %2',
     'task delete runs only after snapshotting the branch to a trash ref'),
    ('desktop/src/UpstreamCheckoutSync.cpp',
     'QStringLiteral\\("reset"\\), QStringLiteral\\("--hard"\\)',
     'forced ledger branch on a clean worktree; old tip kept in a trash ref'),
    ('desktop/src/UpstreamCheckoutSync.cpp',
     'QStringLiteral\\("-D"\\)',
     'prunes merged or pull branches after saving their tips to a trash ref'),
    ('marketplace/packages/backups/content/desktop/BackupsFeature.cpp',
     'QDir\\(chosen\\.path\\)\\.removeRecursively\\(\\)',
     'moved-aside data folder validated by BackupShelf canDelete'),
    ('marketplace/packages/vault/tools/make_openssl_vectors.js',
     'fs\\.rmSync\\(dir, \\{ recursive: true \\}\\)',
     'deletes its own mkdtempSync vector scratch folder'),
    ('marketplace/tools/marketplace_lib.py',
     'shutil\\.rmtree\\(destination\\)',
     'replaces a verified materialized catalog folder only after a successful clone; marketplace_lib ships standalone in deploy bundles'),
    ('marketplace/tools/marketplace_lib.py',
     'shutil\\.rmtree\\((temporary|staging), ignore_errors=True\\)',
     'mkdtemp staging folder this call created'),
    ('marketplace/tools/marketplace_lib.py',
     'shutil\\.rmtree\\(target\\)',
     'rolls back the package folder this install just moved into place'),
    ('marketplace/tools/marketplace_lib.py',
     'shutil\\.rmtree\\(quarantine\\)',
     'uninstall quarantine folder renamed by this call after down migrations commit'),
    ('native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/account/projects/ProjectsService.kt',
     'dir\\.deleteRecursively\\(\\)',
     'erases one project folder strictly inside the app projects root'),
    ('native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/account/settings/AccountToolsScreens.kt',
     'cacheDir\\.listFiles\\(\\)',
     'clears the app sandbox cache directory on user request'),
    ('native/ios/ForkMesh/Features/Agents/AgentAttachmentViews.swift',
     'removeItem\\(at: folder\\)',
     'removes its own uuid temp share folder'),
    ('native/ios/ForkMesh/Features/Chat/Threads/ChatMessageParts.swift',
     'removeItem\\(at: file\\)',
     'deletes decrypted chat attachment files this app wrote in its own temporary folder'),
    ('native/ios/ForkMesh/Features/Code/RepoSettings/RepoSettingsScreen.swift',
     'removeItem\\(at: url\\)',
     'replaces a temp zip file in the app temporary directory'),
    ('native/ios/ForkMeshCore/Sources/FMAccount/ProjectStorage.swift',
     'removeItem\\(at: dir\\)',
     'erases one project folder directly under the sandbox projects root'),
    ('native/ios/ForkMeshCore/Sources/FMChat/LocalControl/LocalControlService.swift',
     '\\.part"\\)',
     'removes a partial download file in the sandbox'),
    ('native/ios/ForkMeshCore/Sources/FMChat/Threads/ChatHistoryStore.swift',
     'removeItem\\(at: file\\(scope: scope\\)\\)',
     'erases one chat history file in the sandbox'),
    ('native/ios/ForkMeshCore/Sources/FMCode/Local/LocalRepoStore.swift',
     'removeItem\\(at: directory\\)',
     'user-confirmed local repo delete, or cleanup of a folder this call just created'),
    ('native/ios/ForkMeshCore/Sources/FMGit/Engine/EngineFS.swift',
     '^/// ',
     'doc comment naming the Dart API it mirrors'),
    ('native/ios/ForkMeshCore/Sources/FMGit/Support/FileSystem.swift',
     '^/// ',
     'doc comment naming the Dart API it mirrors'),
    ('native/ios/ForkMeshCore/Sources/FMLocalAI/FileDownloading.swift',
     'removeItem\\(at: partialFile\\)',
     'removes a partial model download file in the sandbox'),
    ('native/ios/ForkMeshCore/Sources/FMLocalAI/HuggingFaceSnapshot.swift',
     'removeItem\\(at: partial\\)',
     'removes a partial model download file in the sandbox'),
    ('native/ios/ForkMeshCore/Sources/FMLocalAI/LocalModelStore.swift',
     'fileManager\\.removeItem\\(at:',
     'deletes downloaded local AI model files in the sandbox'),
    ('native/ios/ForkMeshCore/Sources/FMSurfaces/SurfaceSnapshot.swift',
     'removeItem\\(at: url\\)',
     'removes one surface snapshot file in the sandbox'),
    ('native/ios/ForkMesh/Features/Chat/Messages/MessagesMedia.swift',
     'removeItem\\(at: fileURL\\)',
     'cleans up temporary audio recording files in the app sandbox'),
    ('server/agent_workspace.go',
     '"checkout", "--force", "--quiet", "-B", branch, start',
     'runs only after saveAgentWorkspace kept unpushed commits and the dirty tree in trash refs'),
    ('server/agent_workspace.go',
     '"clean", "-fdq"',
     'untracked files were saved in the wip trash ref just before'),
    ('server/merge.go',
     '"worktree", "remove", "--force", work',
     'removes its own detached apply scratch worktree'),
    ('tools/clean_machine_transcript.sh',
     'trap \'rm -rf -- \\"\\$WORK\\"\' EXIT',
     'mktemp -d transcript work dir removed on exit'),
    ('tools/forkmesh-release-host.py',
     'rm -rf ci/lock',
     'build lock folder in the office Mac CI account'),
    ('tools/forkmesh-release-host.py',
     'rm -rf src-\\* build-src-\\* stage-src-\\*',
     'previous desktop release scratch trees on the office Mac'),
    ('tools/forkmesh-release-host.py',
     'rm -rf tree$',
     'office Mac CI scratch folder that has no .git yet'),
    ('tools/forkmesh-release-host.py',
     'git clean -ffdxq',
     'office Mac sparse CI clone owned by the release host'),
    ('tools/forkmesh-release-host.py',
     'release/ios-\\* 2>/dev/null \\| tail -n \\+3 \\| xargs rm -rf|rm -rf \\"\\$1\\"',
     'office Mac iOS run folders, name checked as release/ios-*'),
    ('tools/forkmesh-release-host.py',
     'rm -rf \\"\\$build(/install)?\\"',
     'Windows cross-build folder under the release work dir, required variable'),
    ('tools/forkmesh-release-host.py',
     '\\"worktree\\", \\"remove\\", \\"--force\\", tree',
     'release build worktrees this host created under its work folder'),
    ('tools/forkmesh-release-host.py',
     'git\\(repo, \\"branch\\", \\"-D\\", branch\\)',
     'release promote branch recreated by this publisher each run'),
    ('tools/forkmesh_disaster_recovery.py',
     'shutil\\.rmtree\\(path, on(exc|error)=make_writable\\)',
     'drill copies inside a forkmesh-drill- mkdtemp folder, checked before removal'),
    ('tools/forkmesh_offline_sync.py',
     'shutil\\.rmtree\\(self\\.work( / \\"chunks\\")?',
     'fixed .forkmesh-transfer folder inside the destination; ships in the offline recovery kit without the helper'),
    ('tools/git_conformance.py',
     '\\"push\\", \\"-q\\", \\"--force\\"',
     "conformance probe against the suite's own fixture server"),
    ('tools/github_forkmesh_bridge.py',
     '\\"worktree\\", \\"remove\\", \\"--force\\", str\\(pulls_tree\\)|shutil\\.rmtree\\(pulls_tree',
     'bridge-owned pulls worktree inside .git; the bridge ships standalone into GitHub repositories'),
    ('tools/headless_mirror_identity.py',
     'shutil\\.rmtree\\(staging, ignore_errors=True\\)',
     'archive staging folder this restore created on the mirror server'),
    ('tools/headless_mirror_refresh.py',
     'shutil\\.rmtree\\(path\\)',
     '64-hex merge quarantine folder under its root, owner and mode checked; server bundle'),
    ('tools/hosted_repository_sync.py',
     '\\"--prune=now\\"',
     'server-side public import mirrors compacted after a ref change'),
    ('tools/hosted_repository_sync.py',
     'shutil\\.rmtree\\(staging, ignore_errors=True\\)',
     'hidden staging clone this import created'),
    ('tools/hosted_repository_sync.py',
     'shutil\\.rmtree\\(path\\)',
     'server import mirrors with a sourceUrl marker absent from a present sidecar'),
    ('tools/loopback-netns.sh',
     'trap \'rm -rf \\"\\$_fm_tmp\\"\' EXIT',
     'mktemp -d scratch dir removed on exit'),
    ('tools/mirror_gateway.py',
     'shutil\\.rmtree\\(path\\)',
     'runtime materialization folders matching a fixed pattern, owner checked; server bundle'),
    ('tools/next_autodeploy.py',
     '\\"worktree\\", \\"remove\\", \\"--force\\", str\\(path\\)',
     'dedicated node deploy worktree owned by the autodeploy tool'),
    ('tools/next_autoscale.py',
     'rm -rf /var/lib/fmnext-ids /etc/wireguard/wg0.key',
     'explicit snapshot preparation on a fleet image builder, never this machine'),
    ('tools/next_autoscale.py',
     'rm -rf \\\\\\"\\$stage\\\\\\" /tmp/',
     'remote mktemp stage and /tmp tarball on a fleet node'),
    ('tools/next_deploy.py',
     'rm -rf \\.tmp-\\{(key|rev)\\}|rm -rf \\{key\\}',
     'remote release and wheelhouse folders named by validated hex keys'),
    ('tools/next_deploy.py',
     'rm -rf \\"\\$venv\\"|rm -rf -- \\"\\$old\\"',
     'remote venv and old release folders under fixed fleet roots'),
    ('tools/next_fleet.py',
     'rm -rf /tmp/%s /tmp/%s\\.tar\\.gz|rm -rf /tmp/fmnext-daemon',
     'literal /tmp staging on a fleet node'),
    ('tools/next_fleet.py',
     'rm -rf \\\\?\\"?\\$stage',
     'remote mktemp or fixed /tmp stage folder on a fleet node'),
    ('tools/node_operator_policy.py',
     'shutil\\.rmtree\\(git_dir\\)',
     'operator --confirm removal of a quarantined repository under the node root'),
    ('tools/package/appimage-toolchain.sh',
     'rm -rf "\\$_fm_appimage_stage"',
     'mktemp -d stage under TMPDIR removed on exit'),
    ('tools/package/appimage.sh',
     'trap \'rm -rf \\"\\$workdir\\"\' EXIT',
     'mktemp -d work dir removed on exit'),
    ('tools/package/linux-baseline.sh',
     'rm -rf "\\$dest"|rm -rf "\\$\\{rootfs:\\?\\}\\$prefix"|rm -rf "\\$scratch"',
     'paths checked by inside_cache against the baseline cache'),
    ('tools/package/linux-baseline.sh',
     'rm -rf /var/lib/apt/lists/\\*',
     'runs inside the bubblewrap smoke rootfs, not the host'),
    ('tools/package/linux-baseline/build-forkmesh.sh',
     'rm -rf "\\$(build|install_root)"',
     'runs inside the baseline build container under /work'),
    ('tools/package/linux-baseline/smoke-test.sh',
     'rm -rf "\\$home"',
     'mktemp /tmp home inside the bubblewrap smoke sandbox'),
    ('tools/package/linux-baseline/stage-nghttp3.sh',
     'rm -rf "\\$src"',
     'mktemp source dir inside the baseline builder rootfs'),
    ('tools/package/linux-baseline/stage-openssl.sh',
     'rm -rf "\\$src"',
     'mktemp source dir inside the baseline builder rootfs'),
    ('tools/package/linux-baseline/stage-packages.sh',
     'rm -rf /var/lib/apt/lists/\\*',
     'apt cache inside the baseline builder rootfs'),
    ('tools/package/linux-baseline/stage-qt.sh',
     'rm -rf "(\\$\\{work:\\?\\}/\\$module|\\$work)"',
     'runs inside the baseline builder rootfs on its work dir'),
    ('tools/package/macos.sh',
     'trap \'rm -rf \\"\\$workdir\\"\' EXIT',
     'mktemp -d work dir removed on exit'),
    ('tools/package/windows.sh',
     'trap \'rm -rf \\"\\$workdir\\"\' EXIT',
     'mktemp -d work dir removed on exit'),
    ('tools/provision_ssh_gateway.sh',
     'trap \'rm -rf \\"\\$(stage|staging)\\"\' EXIT',
     'mktemp stage locally, fixed /root staging on remote gateway'),
    ('tools/provision_ssh_gateway.sh',
     'rm -rf /root/\\.forkmesh-gateway-stage',
     'fixed staging path on the remote gateway host'),
    ('tools/secret_gate.py',
     'shutil\\.rmtree\\(root, ignore_errors=True\\)',
     'corpus mkdtemp folder this self-test owns; server bundle'),
    ('tools/sign_ssh_host_certs.sh',
     'trap \'rm -rf \\"\\$work\\"\' EXIT',
     'mktemp -d certificate work dir removed on exit'),
    ('www/public/install.sh',
     'rm -rf -- "\\$target" \\|\\| return 1',
     'embedded copy of the fm_safe_rm_tree helper'),
    ('www/public/install.sh',
     'forkmesh-tools\\.\\?\\?\\?\\?\\?\\?\\) rm -rf -- "\\$scratch"',
     'only mktemp installer scratch clones directly under TMPDIR'),
    ('www/public/install.sh',
     '-exec rm -rf -- \\{\\} \\+',
     'stale service mirror tmp prefixes inside marker-checked /var/lib/forkmesh/tmp'),
    ('www/public/install.sh',
     'rm -rf -- "\\$SYSTEMD_STATE_DIR"',
     'purge only, root-owned state marker verified first'),
    ('www/public/install.sh',
     'rm -rf -- "\\$d" && say "Removed \\$d"',
     'confirmed purge behind guard_user_data_wipe and agent-shell refusal'),
    ('www/public/install.sh',
     'rm -rf -- "\\$state_dir/\\.config/ForkMesh"',
     'retired installer service migration, pinned by test_install_lifecycle'),
    ('www/public/uninstall.sh',
     'rm -rf -- "\\$target" \\|\\| return 1',
     'embedded copy of the fm_safe_rm_tree helper'),
    ('www/public/uninstall.sh',
     'rm -rf -- "\\$d" && say "Removed \\$d"',
     'confirmed purge behind guard_user_data_wipe and agent-shell refusal'),
]


def _tracked_files() -> list[str]:
    try:
        out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--", *SCANNED],
                             capture_output=True, check=True, timeout=60).stdout
        return [p for p in out.decode().split("\0") if p]
    except (OSError, subprocess.SubprocessError):
        files = []
        for top in SCANNED:
            for dirpath, dirnames, filenames in os.walk(REPO / top):
                dirnames[:] = [d for d in dirnames if d not in {".git", "node_modules", "__pycache__"}
                               and not d.startswith("build")]
                files.extend(os.path.relpath(os.path.join(dirpath, f), REPO) for f in filenames)
        return files


@functools.lru_cache(maxsize=1)
def _scanned_files() -> tuple[str, ...]:
    result = []
    for rel in _tracked_files():
        path = REPO / rel
        suffix = path.suffix
        if suffix not in SOURCE_SUFFIXES and not (suffix == "" and path.is_file()
                                                  and path.read_bytes()[:2] == b"#!"):
            continue
        if TEST_PATH.search(rel) or rel in HELPERS or not path.is_file():
            continue
        result.append(rel)
    return tuple(result)


@functools.lru_cache(maxsize=1)
def _hits() -> tuple[tuple[str, int, str, str], ...]:
    hits = []
    for rel in _scanned_files():
        try:
            text = (REPO / rel).read_text(errors="replace")
        except OSError:
            continue
        if not KEYWORDS.search(text):
            continue
        lines = text.splitlines()
        for number, line in enumerate(lines, 1):
            if len(line) > 4000 or not KEYWORDS.search(line):
                continue
            following = lines[number].strip() if number < len(lines) else ""
            joined = line.strip() + " " + following
            for rule, pattern in RULES.items():
                if pattern.search(line):
                    hits.append((rel, number, rule, line.strip()))
                elif (line.rstrip().endswith((",", "(", "{", "[", "\\")) and pattern.search(joined)
                      and not pattern.search(following)):
                    hits.append((rel, number, rule, joined))
    return tuple(hits)


def _allowed(rel: str, line: str) -> bool:
    return any(rel == path and re.search(pattern, line) for path, pattern, _ in ALLOW)


def test_every_destructive_primitive_is_guarded_or_allow_listed():
    unguarded = [f"{rel}:{number}: [{rule}] {line[:160]}"
                 for rel, number, rule, line in _hits() if not _allowed(rel, line)]
    assert not unguarded, (
        "Raw destructive deletes or git history operations outside the guarded helpers "
        "(docs/engineering/destructive-actions.md). Route them through removeTree / "
        "remove_tree / fm_safe_rm_tree / removeTreeUnder, or add an ALLOW entry with a "
        "reason:\n" + "\n".join(unguarded))


def test_every_allow_entry_has_a_reason_and_still_matches():
    hits = _hits()
    stale = []
    for path, pattern, reason in ALLOW:
        assert len(reason.split()) >= 4, f"{path}: allow-list reason too short: {reason!r}"
        assert (REPO / path).is_file(), f"allow-listed file is gone: {path}"
        if not any(rel == path and re.search(pattern, line) for rel, _, _, line in hits):
            stale.append(f"{path}: {pattern}")
    assert not stale, "Allow-list entries that no longer match anything:\n" + "\n".join(stale)


def test_allow_list_stays_small_per_file():
    per_file: dict[str, int] = {}
    for path, _, _ in ALLOW:
        per_file[path] = per_file.get(path, 0) + 1
    crowded = {path: count for path, count in per_file.items() if count > 12}
    assert not crowded, f"Too many allow-list entries in one file; add a helper instead: {crowded}"


def test_rules_catch_the_incident_shapes():
    samples = {
        "rm -r": ['runUpdateStepUser("rm", {"-rf", repoDir}, parent,', "rm -rf \"$SRC\"",
                  "subprocess.run([\"rm\", \"-rf\", path])"],
        "shutil.rmtree": ["shutil.rmtree(checkout)"],
        "QDir::removeRecursively": ["QDir(path).removeRecursively();"],
        "std::filesystem::remove_all": ["std::filesystem::remove_all(p);"],
        "os.RemoveAll": ["_ = os.RemoveAll(dir)"],
        "git clean -f": ["runGitCapture(dir, {\"clean\", \"-fd\"}, nullptr, nullptr)", "git clean -ffdxq"],
        "git reset --hard": ["runGitCapture(dir, {\"reset\", \"--hard\"}, nullptr, nullptr);"],
        "git worktree remove --force": ["{\"worktree\", \"remove\", \"--force\", held}"],
        "git branch -D": ["{QStringLiteral(\"branch\"), QStringLiteral(\"-D\"), branch}"],
        "git push --force": ["git push --force origin main", "[\"git\", \"push\", \"--force\", url]"],
        "git stash drop/clear": ["{\"stash\", \"drop\", \"stash@{0}\"}"],
        "git gc --prune=now": ["{\"gc\", \"--prune=now\"}"],
        "recursive file delete": ["dir.deleteRecursively()", "try FileManager.default.removeItem(at: url)",
                                  "fs.rmSync(dir, { recursive: true });"],
    }
    for rule, lines in samples.items():
        for line in lines:
            assert RULES[rule].search(line), (rule, line)
    for harmless in ('{"ls-tree", "-r", ref}', "QStringLiteral(\"--numstat\"), QStringLiteral(\"-r\"),",
                     "fs.mkdirSync(dir, { recursive: true });"):
        assert not any(pattern.search(harmless) for pattern in RULES.values()), harmless


def test_helpers_share_one_disposable_folder_list():
    python = (REPO / "tools/forkmesh_safe_delete.py").read_text()
    cpp = (REPO / "desktop/src/SafeDelete.h").read_text()
    go = (REPO / "server/safedelete.go").read_text()
    block = python[python.index("DISPOSABLE_NAMES"):python.index("DISPOSABLE_PREFIXES")]
    names = set(re.findall(r'"([^"]+)"', block))
    cpp_block = cpp[cpp.index("inline bool disposable"):cpp.index("inline bool sharedProjectFolder")]
    go_block = go[go.index("var safeDeleteDisposable"):go.index("func safeDeleteClean")]
    assert names == set(re.findall(r'QStringLiteral\("([^"]+)"\)', cpp_block))
    assert names == set(re.findall(r'"([^"]+)":', go_block))
    shell = (REPO / "tools/forkmesh-safe-rm.sh").read_text()
    for name in names:
        assert f"*/{name}/*" in shell, name


def test_shell_helper_copies_are_identical():
    canonical = (REPO / "tools/forkmesh-safe-rm.sh").read_text()
    body = canonical[canonical.index("fm_safe_rm_tree() {"):]
    body = body[:body.index("\n}\n") + 3]
    for rel in _scanned_files():
        if not rel.endswith((".sh", ".py", ".cpp")):
            continue
        text = (REPO / rel).read_text(errors="replace")
        if "fm_safe_rm_tree() {" not in text:
            continue
        start = text.index("fm_safe_rm_tree() {")
        copy = text[start:text.index("\n}\n", start) + 3]
        assert copy == body, f"{rel} has a stale copy of fm_safe_rm_tree; copy it from tools/forkmesh-safe-rm.sh"
