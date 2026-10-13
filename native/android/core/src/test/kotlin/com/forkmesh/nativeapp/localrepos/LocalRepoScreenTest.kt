package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.time.Instant

class LocalRepoScreenTest {
    private val author = GitAuthor(name = "Alice", email = "alice@example.com")

    private inline fun withSeed(block: (LocalRepoStore, LocalRepoRecord, GitWorkspace) -> Unit) = withScratch("fm-local-screen-") { temp ->
        val store = LocalRepoStore(File(temp, "repos"))
        val record = store.create(name = "notes", description = "Scratch notes", author = author)
        val workspace = store.workspace(record)
        try {
            workspace.writeFile("docs/todo.md", "- one\n")
            workspace.stageAll()
            workspace.commit(message = "Add todo", author = author)
            block(store, record, workspace)
        } finally {
            workspace.dispose()
        }
    }

    fun testFilesTabListsTheWorktreeAndNavigatesFolders() = withSeed { _, record, workspace ->
        assertEq("main", workspace.summary().branch)
        assertEq("Local only", record.syncLabel)
        val root = workspace.listFiles()
        assertEq(listOf("docs", "README.md"), root.map { it.name })
        assertEq("Folder", localFileMeta(root.first()))
        assertEq(listOf("todo.md"), workspace.listFiles("docs").map { it.name })
        assertEq(listOf(LocalBreadcrumb("Root", "", true)), localBreadcrumbs(""))
        assertEq(
            listOf(LocalBreadcrumb("Root", "", false), LocalBreadcrumb("docs", "docs", false), LocalBreadcrumb("deep", "docs/deep", true)),
            localBreadcrumbs("docs/deep"),
        )
    }

    fun testEditorSavesAFileAndTheChangeShowsUpForStaging() = withSeed { store, record, workspace ->
        val content = workspace.readFile("README.md")
        assertEq("# notes\n\nScratch notes\n", content.text)
        expect(content.isEditable)

        val edited = "# notes\n\nEdited on phone\n"
        workspace.writeFile("README.md", edited)
        assertEq("README.md · 4 lines · unsaved", editorFooterText("README.md", edited, dirty = true))
        assertEq("README.md · 1 line", editorFooterText("README.md", "", dirty = false))

        var status = workspace.status()
        assertEq(1, status.changeCount)
        assertEq("README.md", status.unstaged.single().path)
        assertEq("Modified", localChangeLabel(status.unstaged.single()))

        workspace.stage(listOf("README.md"))
        status = workspace.status()
        assertEq(1, status.staged.size)
        val fallback = localRepoAuthor(null)
        assertEq("Committing as forkmesh <forkmesh@mobile.forkmesh>", committingAsText(fallback))
        assertEq("Commit 1", commitButtonLabel(status.staged.size))
        assertEq("Commit", commitButtonLabel(0))
        val id = workspace.commit(message = "Edit readme on phone", author = fallback)
        assertEq("Committed ${id.substring(0, 7)}", committedMessage(id))
        expect(workspace.status().isClean)

        val log = workspace.log()
        assertEq(listOf("Edit readme on phone", "Add todo", "Initial commit"), log.map { it.subject })
        assertEq("forkmesh", log.first().authorName)

        store.updateSummary(record.name, workspace.summary())
        assertEq("Edit readme on phone", store.find("notes")?.lastCommitSubject)
    }

    fun testBranchesTabCreatesAndSwitchesBranchesWithADirtyGuard() = withSeed { _, record, workspace ->
        assertEq("main", workspace.branches().single { it.isCurrent }.name)
        val clean = workspace.status().isClean
        expect(clean)
        workspace.createBranch("feature/phone", checkout = clean)
        assertEq("feature/phone", workspace.branches().single { it.isCurrent }.name)

        File(record.path, "phone.txt").writeText("from the phone\n")
        workspace.stageAll()
        workspace.commit(message = "Phone note", author = author)
        File(record.path, "README.md").writeText("dirty\n")
        expect(!workspace.status().isClean)
        assertEq("Commit or discard your changes before switching branches.", switchBranchGuardMessage)
        assertEq(switchBranchGuardMessage, assertFails<GitWorkspaceException> { workspace.checkout("main") }.message)
        assertEq("That branch does not exist.", assertFails<GitWorkspaceException> { workspace.checkout("nope") }.message)
        assertEq("That branch name is not valid.", assertFails<GitWorkspaceException> { workspace.createBranch("bad..name") }.message)
    }

    fun testDeleteRepositoryRemovesItFromTheDeviceAndTheList() = withSeed { store, record, _ ->
        assertEq(1, store.list().size)
        store.delete("notes")
        expect(store.list().isEmpty())
        expect(!File(record.path).exists())
    }

    fun testPublishHeadAdvertisesTheLoadedHistory() = withSeed { _, _, workspace ->
        val log = workspace.log()
        val head = publishHeadOf(log, workspace.branches().size)
        assertEq(log.first().id, head.commit)
        assertEq(log.last().id, head.rootCommit)
        assertEq("Add todo", head.subject)
        assertEq("Alice", head.authorName)
        assertEq(log.first().time, head.at)
        assertEq(2, head.commitCount)
        assertEq(1, head.branchCount)
        assertEq(PublishHead(), publishHeadOf(emptyList(), 0))
    }

    fun testPushFetchAndBannerMessages() {
        val updated = LocalPushResult(listOf(LocalPushRef("refs/heads/main", "a", "updated")), 1, 1)
        assertEq("Pushed main to alice/notes.", pushResultMessage(updated, "main", "alice/notes"))
        val upToDate = LocalPushResult(listOf(LocalPushRef("refs/heads/main", "a", "upToDate")), 0, 0)
        assertEq("ForkMesh already has main.", pushResultMessage(upToDate, "main", "alice/notes"))
        val rejected = LocalPushResult(listOf(LocalPushRef("refs/heads/main", "a", "rejected", "stale")), 0, 0)
        assertEq("ForkMesh refused main: stale", pushResultMessage(rejected, "main", "alice/notes"))
        val bare = LocalPushResult(listOf(LocalPushRef("refs/heads/main", "a", "rejected")), 0, 0)
        assertEq("ForkMesh refused main.", pushResultMessage(bare, "main", "alice/notes"))

        assertEq("Already up to date with ForkMesh.", fetchResultMessage(LocalFetchResult(emptyList(), 0, 0, null)))
        assertEq("Fetched 1 branch.", fetchResultMessage(LocalFetchResult(listOf("refs/remotes/origin/main"), 1, 1, null)))
        assertEq("Fetched 2 branches.", fetchResultMessage(LocalFetchResult(listOf("a", "b"), 1, 1, null)))

        assertEq("alice/notes · published from this device", meshBannerText("alice/notes", LocalRepoOrigin.authored))
        assertEq("net/repo · cloned to this device", meshBannerText("net/repo", LocalRepoOrigin.cloned))
    }

    fun testFileChangeAndHistoryLabels() {
        assertEq("512 B", localRepoHumanSize(512))
        assertEq("1.5 KB", localRepoHumanSize(1536))
        assertEq("2.0 MB", localRepoHumanSize(2L * 1024 * 1024))
        assertEq("12 B · untracked", localFileMeta(LocalFileEntry("a", "a", LocalFileKind.file, 12, tracked = false)))
        assertEq("Folder · untracked", localFileMeta(LocalFileEntry("d", "d", LocalFileKind.directory, 0, tracked = false)))
        assertEq("Added", localChangeLabel(LocalChange("a", LocalChangeKind.added, staged = true)))
        assertEq("Deleted", localChangeLabel(LocalChange("a", LocalChangeKind.deleted, staged = false)))
        assertEq("Untracked", localChangeLabel(LocalChange("a", LocalChangeKind.untracked, staged = false)))

        val now = Instant.parse("2026-08-23T12:00:00Z").toEpochMilli()
        val commit = LocalCommit("0123456789abcdef", emptyList(), "s", "s\n", "Alice", "a@b", now - 120_000)
        assertEq("0123456", commit.shortId)
        assertEq("0123456 · Alice · 2m ago", commitRowSubtitle(commit, now))
        assertEq("just now", localRepoRelativeTime(now, now))
    }
}
