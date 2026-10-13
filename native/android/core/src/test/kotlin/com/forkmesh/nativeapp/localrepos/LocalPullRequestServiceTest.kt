package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class LocalPullRequestServiceTest {
    private val author = GitAuthor(name = "Alice Mobile", email = "alice@example.com")

    private fun record(publishedAs: String? = "alice/proj") =
        LocalRepoRecord(name = "proj", path = "/tmp/proj", createdAt = 0, publishedAs = publishedAs)

    private class RecordingTransport {
        val pushedBranches = ArrayList<String>()
        val submitted = ArrayList<Map<String, String>>()
        var rejectPush = false

        val push = PushBranch { _, _, _, branch ->
            pushedBranches.add(branch)
            LocalPushResult(
                refs = listOf(
                    LocalPushRef(
                        remoteRef = "refs/heads/$branch",
                        newId = "a".repeat(40),
                        outcome = if (rejectPush) "rejected" else "updated",
                    ),
                ),
                packBytes = 10,
                objects = 1,
            )
        }

        val submit = SubmitPull { owner, name, title, base, head, patch ->
            submitted.add(mapOf("owner" to owner, "repo" to name, "title" to title, "base" to base, "head" to head, "patch" to patch))
        }
    }

    private fun service(transport: RecordingTransport = RecordingTransport()) =
        PullRequestService(pushBranch = transport.push, submitPull = transport.submit)

    private inline fun <T> withBranch(
        baseFiles: Map<String, String>,
        branchEdits: Map<String, String>,
        branch: String = "agent/work",
        baseMovesOnWith: Map<String, String>? = null,
        block: (GitWorkspace) -> T,
    ): T = withScratch("fm-pr-") { temp ->
        val workspace = GitWorkspace(File(temp, "repo"))
        try {
            workspace.init(files = baseFiles, commitMessage = "base\n", author = author)
            val baseName = workspace.branches().first { it.isCurrent }.name
            workspace.createBranch(branch, checkout = true)
            for ((path, text) in branchEdits) workspace.writeFile(path, text)
            workspace.stageAll()
            workspace.commit(message = "agent work\n", author = author)
            if (baseMovesOnWith != null) {
                workspace.checkout(baseName)
                for ((path, text) in baseMovesOnWith) workspace.writeFile(path, text)
                workspace.stageAll()
                workspace.commit(message = "meanwhile\n", author = author)
                workspace.checkout(branch)
            }
            block(workspace)
        } finally {
            workspace.dispose()
        }
    }

    private fun baseBranchOf(workspace: GitWorkspace): String = workspace.branches().first { !it.name.startsWith("agent/") }.name

    fun testABranchWithCommitsBecomesASubmittablePatch() =
        withBranch(baseFiles = mapOf("a.txt" to "one\ntwo\nthree\n"), branchEdits = mapOf("a.txt" to "one\nTWO\nthree\n")) { workspace ->
            val prepared = service().prepare(workspace, record(), headBranch = "agent/work", baseBranch = baseBranchOf(workspace))
            expect(prepared.isReady)
            assertEq("alice", prepared.owner)
            assertEq("proj", prepared.repo)
            assertEq(listOf("a.txt"), prepared.changedPaths)
            expect(prepared.patch.contains("-two"))
            expect(prepared.patch.contains("+TWO"))
            expect(prepared.summary.contains("1 file"))
            assertEq("1 file · agent/work → main", prepared.summary)
        }

    fun testTheDiffStartsWhereTheBranchesDivergedNotAtTheBaseTip() = withBranch(
        baseFiles = mapOf("a.txt" to "one\n"),
        branchEdits = mapOf("a.txt" to "one changed\n"),
        baseMovesOnWith = mapOf("unrelated.txt" to "added by someone else\n"),
    ) { workspace ->
        val prepared = service().prepare(workspace, record(), headBranch = "agent/work", baseBranch = baseBranchOf(workspace))
        expect(prepared.isReady)
        assertEq(listOf("a.txt"), prepared.changedPaths)
        expect(!prepared.patch.contains("unrelated.txt"))
    }

    fun testALocalOnlyRepositoryHasNowhereToSendAPullRequest() =
        withBranch(baseFiles = mapOf("a.txt" to "one\n"), branchEdits = mapOf("a.txt" to "two\n")) { workspace ->
            val prepared = service().prepare(workspace, record(publishedAs = null), headBranch = "agent/work", baseBranch = baseBranchOf(workspace))
            assertEq(PullRequestBlocker.notPublished, prepared.blocker)
            expect(prepared.message.contains("Publish it to ForkMesh"))
            assertEq(prepared.message, prepared.summary)
        }

    fun testABranchWithNoNewCommitsIsRefused() = withScratch("fm-pr-") { temp ->
        val workspace = GitWorkspace(File(temp, "repo"))
        try {
            workspace.init(files = mapOf("a.txt" to "one\n"), commitMessage = "base\n", author = author)
            val base = workspace.branches().first { it.isCurrent }.name
            workspace.createBranch("agent/empty", checkout = true)
            val prepared = service().prepare(workspace, record(), headBranch = "agent/empty", baseBranch = base)
            assertEq(PullRequestBlocker.noCommits, prepared.blocker)
        } finally {
            workspace.dispose()
        }
    }

    fun testAnUnknownBranchIsNamedInTheRefusal() =
        withBranch(baseFiles = mapOf("a.txt" to "one\n"), branchEdits = mapOf("a.txt" to "two\n")) { workspace ->
            val prepared = service().prepare(workspace, record(), headBranch = "agent/does-not-exist", baseBranch = baseBranchOf(workspace))
            assertEq(PullRequestBlocker.missingBranch, prepared.blocker)
            expect(prepared.message.contains("agent/does-not-exist"))
        }

    fun testABinaryChangeIsRefusedWithThePathsAndAWayForward() = withBranch(
        baseFiles = mapOf("a.txt" to "one\n"),
        branchEdits = mapOf("a.txt" to "two\n", "logo.png" to "PNG\u0000binary"),
    ) { workspace ->
        val prepared = service().prepare(workspace, record(), headBranch = "agent/work", baseBranch = baseBranchOf(workspace))
        assertEq(PullRequestBlocker.binaryChanges, prepared.blocker)
        assertEq(listOf("logo.png"), prepared.binaryPaths)
        expect(prepared.message.contains("logo.png"))
        expect(prepared.message.contains("safe on agent/work"))
        expect(prepared.message.contains("desktop"))
    }

    fun testPushesTheBranchBeforeOpeningThePullRequest() =
        withBranch(baseFiles = mapOf("a.txt" to "one\n"), branchEdits = mapOf("a.txt" to "two\n")) { workspace ->
            val transport = RecordingTransport()
            val service = service(transport)
            val prepared = service.prepare(workspace, record(), headBranch = "agent/work", baseBranch = baseBranchOf(workspace))
            val stages = ArrayList<String>()
            service.submit(workspace, prepared, title = "Fix the thing", onProgress = { stages.add(it) })
            assertEq(listOf("agent/work"), transport.pushedBranches)
            assertEq(1, transport.submitted.size)
            val sent = transport.submitted.single()
            assertEq("alice", sent["owner"])
            assertEq("Fix the thing", sent["title"])
            assertEq("agent/work", sent["head"])
            expect(sent["patch"]!!.contains("diff --git"))
            expect(stages.first().contains("Pushing"))
            assertEq(listOf("Pushing agent/work…", "Opening the pull request…"), stages)
        }

    fun testARejectedPushDoesNotOpenAPullRequest() =
        withBranch(baseFiles = mapOf("a.txt" to "one\n"), branchEdits = mapOf("a.txt" to "two\n")) { workspace ->
            val transport = RecordingTransport().apply { rejectPush = true }
            val service = service(transport)
            val prepared = service.prepare(workspace, record(), headBranch = "agent/work", baseBranch = baseBranchOf(workspace))
            assertFails<IllegalStateException> { service.submit(workspace, prepared, title = "nope") }
            expect(transport.submitted.isEmpty())
        }

    fun testAnEmptyTitleFallsBackToSomethingMeaningful() =
        withBranch(baseFiles = mapOf("a.txt" to "one\n"), branchEdits = mapOf("a.txt" to "two\n")) { workspace ->
            val transport = RecordingTransport()
            val service = service(transport)
            val prepared = service.prepare(workspace, record(), headBranch = "agent/work", baseBranch = baseBranchOf(workspace))
            service.submit(workspace, prepared, title = "   ")
            assertEq("Changes from agent/work", transport.submitted.single()["title"])
        }

    fun testABlockedPreparationCannotBeSubmitted() =
        withBranch(baseFiles = mapOf("a.txt" to "one\n"), branchEdits = mapOf("a.txt" to "two\n")) { workspace ->
            val transport = RecordingTransport()
            assertFails<IllegalStateException> {
                service(transport).submit(
                    workspace,
                    PullRequestPreparation.blocked(PullRequestBlocker.noChanges, "nothing to send"),
                    title = "x",
                )
            }
            expect(transport.pushedBranches.isEmpty())
        }
}
