package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.time.Instant

class GitLocalWorkflowTest {
    private fun identity(seconds: Long): GitIdentity = GitIdentity(
        name = "Ada Lovelace",
        email = "ada@example.test",
        timestampSeconds = seconds,
        timezoneOffsetMinutes = 0,
    )

    private fun commit(
        repository: GitRepository,
        file: String,
        content: String,
        message: String,
        seconds: Long = 0,
    ): GitObjectId {
        repository.worktree.child(file).writeText(content)
        repository.add(listOf(GitWorktreePath(file)))
        return repository.commit(
            GitCommitRequest(message = message, author = identity(seconds), committer = identity(seconds)),
        )
    }

    private inline fun withRepository(block: (root: File, repository: GitRepository) -> Unit) =
        withTempDir("forkmesh-local-") { root -> block(root, GitRepository.init(GitRepositoryInit(root))) }

    fun testLogWalksNewestFirstWithTypedIdentitiesAndALimit() = withRepository { _, repository ->
        val first = commit(repository, "a.txt", "a\n", "one\n")
        val second = commit(repository, "b.txt", "b\n", "two\n", seconds = 100)
        val third = commit(repository, "c.txt", "c\n", "three\n", seconds = 200)

        val entries = repository.log(GitLogRequest())
        assertEq(listOf(third, second, first), entries.map { it.id })
        val identity = entries.first().commit.author!!
        assertEq("Ada Lovelace", identity.name)
        assertEq("ada@example.test", identity.email)
        assertEq(200L, identity.timestampSeconds)
        assertEq(0, identity.timezoneOffsetMinutes)
        assertEq(Instant.ofEpochMilli(200000), identity.timeUtc)

        assertEq(2, repository.log(GitLogRequest(limit = 2)).size)
        val fromRef = repository.log(GitLogRequest(ref = "refs/heads/main", limit = 1))
        assertEq(third, fromRef.single().id)
    }

    fun testLogAnUnbornHeadYieldsAnEmptyLog() = withRepository { _, repository ->
        expect(repository.log(GitLogRequest()).isEmpty())
    }

    fun testLogAMissingRefIsATypedInvalidRefError() = withRepository { _, repository ->
        commit(repository, "a.txt", "a\n", "one\n")
        assertGitError(GitErrorCode.invalidRef) { repository.log(GitLogRequest(ref = "refs/heads/nope")) }
    }

    fun testRefEnumerationListsLooseAndPackedBranchesWithLooseShadowingPacked() = withRepository { _, repository ->
        val tip = commit(repository, "a.txt", "a\n", "one\n")
        repository.createBranch(GitBranchName("feature/x"))
        val stale = "0".repeat(39)
        repository.gitDirectory.child("packed-refs").writeText(
            "# pack-refs with: peeled fully-peeled sorted \n" +
                "${stale}1 refs/heads/main\n" +
                "${tip.hex} refs/heads/packed-only\n" +
                "${tip.hex} refs/tags/v1\n",
        )

        val branches = repository.listBranches()
        assertEq(
            listOf("refs/heads/feature/x", "refs/heads/main", "refs/heads/packed-only"),
            branches.map { it.name.value },
        )
        assertEq(listOf("feature/x", "main", "packed-only"), branches.map { it.shortName })
        assertEq(tip, branches.first { it.shortName == "main" }.objectId, "loose value shadows the stale packed one")

        val tags = repository.listTags()
        assertEq("v1", tags.single().shortName)
        assertEq(tip, tags.single().objectId)
    }

    fun testResetPathsRestoresStagedEditsToHeadAndDropsNewlyStagedPaths() = withRepository { root, repository ->
        commit(repository, "a.txt", "old\n", "one\n")
        root.child("a.txt").writeText("new\n")
        root.child("b.txt").writeText("b\n")
        repository.add(listOf(GitWorktreePath("a.txt"), GitWorktreePath("b.txt")))
        assertEq(2, repository.status().staged.size)

        repository.resetPaths(listOf(GitWorktreePath("a.txt"), GitWorktreePath("b.txt")))

        val status = repository.status()
        expect(status.staged.isEmpty())
        expect(status.worktree.map { it.path.value }.containsAll(listOf("a.txt", "b.txt")), "${status.worktree}")
        assertEq("new\n", root.child("a.txt").readText())
    }

    fun testCheckoutFastPathSwitchesToASameCommitBranchWithoutACleanWorktree() = withRepository { root, repository ->
        commit(repository, "a.txt", "a\n", "one\n")
        repository.createBranch(GitBranchName("topic"))
        root.child("a.txt").writeText("dirty\n")

        repository.checkout(GitCheckoutRequest.branch(GitBranchName("topic")))

        assertEq(GitSymbolicRef("refs/heads/topic"), repository.readHead())
        assertEq("dirty\n", root.child("a.txt").readText(), "no materialization happens for a same-commit switch")
    }

    fun testCheckoutFastPathStillRefusesADirtySwitchBetweenDifferentCommits() = withRepository { root, repository ->
        commit(repository, "a.txt", "a\n", "one\n")
        repository.createBranch(GitBranchName("topic"))
        repository.checkout(GitCheckoutRequest.branch(GitBranchName("topic")))
        commit(repository, "b.txt", "b\n", "two\n")
        root.child("a.txt").writeText("dirty\n")

        assertGitError(GitErrorCode.operationInterrupted) {
            repository.checkout(GitCheckoutRequest.branch(GitBranchName("main")))
        }
    }

    fun testUtf8IdentityRoundTripsANonAsciiAuthorThroughCommitAndLog() = withRepository { root, repository ->
        root.child("a.txt").writeText("a\n")
        repository.add(listOf(GitWorktreePath("a.txt")))
        val identity = GitIdentity(
            name = "Ada Ærlig Ødegård",
            email = "ada@ærlig.example",
            timestampSeconds = 42,
            timezoneOffsetMinutes = 330,
        )
        val id = repository.commit(GitCommitRequest(message = "unicode\n", author = identity, committer = identity))

        val commit = repository.readCommit(id)
        val author = commit.author!!
        assertEq("Ada Ærlig Ødegård", author.name)
        assertEq("ada@ærlig.example", author.email)
        assertEq(42L, author.timestampSeconds)
        assertEq(330, author.timezoneOffsetMinutes)
    }

    fun testUtf8IdentityRejectsAngleBracketsControlCharactersAndEmptyFields() {
        fun make(name: String, email: String) =
            GitIdentity(name = name, email = email, timestampSeconds = 0, timezoneOffsetMinutes = 0)

        assertFails<GitException> { make("Ada <x>", "a@b") }
        assertFails<GitException> { make("Ada", "a@b>") }
        assertFails<GitException> { make("Ada\nEve", "a@b") }
        assertFails<GitException> { make("Ada" + 7.toChar(), "a@b") }
        assertFails<GitException> { make("", "a@b") }
        make("Ünïcode is fine", "a@b")
    }

    fun testUtf8IdentityGitCommitIdentityParsesOffsetsAndRejectsMalformedValues() {
        val parsed = GitCommitIdentity.parse("Ada <ada@example.test> 1700000000 -0530")
        assertEq(-330, parsed.timezoneOffsetMinutes)
        assertEq(null, GitCommitIdentity.tryParse("no email here 12 +0000"))
        assertEq(null, GitCommitIdentity.tryParse("A <a@b> notatime +0000"))
        assertEq(null, GitCommitIdentity.tryParse("A <a@b> 12 +0299"))
        assertGitError(GitErrorCode.invalidObject) { GitCommitIdentity.parse("garbage") }
    }

    fun testStaleLocksRemovesOnlyLocksOlderThanTheThreshold() = withRepository { _, repository ->
        commit(repository, "a.txt", "a\n", "one\n")
        val gitPath = repository.gitDirectory
        val staleIndex = gitPath.child("index.lock")
        val staleRef = gitPath.child("refs").child("heads").child("main.lock")
        val freshHead = gitPath.child("HEAD.lock")
        staleIndex.writeText("")
        staleRef.writeText("")
        freshHead.writeText("")
        val old = System.currentTimeMillis() - 60 * 60 * 1000
        expect(staleIndex.setLastModified(old))
        expect(staleRef.setLastModified(old))

        val removed = repository.recoverStaleLocks()

        assertEq(2, removed)
        expect(!staleIndex.exists())
        expect(!staleRef.exists())
        expect(freshHead.exists(), "fresh locks survive")
        freshHead.delete()
    }

    fun testStaleLocksOpenClearsStaleLocksAutomatically() = withRepository { root, repository ->
        commit(repository, "a.txt", "a\n", "one\n")
        val stale = repository.gitDirectory.child("index.lock")
        stale.writeText("")
        expect(stale.setLastModified(System.currentTimeMillis() - 60 * 60 * 1000))

        val reopened = GitRepository.open(GitRepositoryOpen(root))

        expect(!stale.exists())
        root.child("b.txt").writeText("b\n")
        reopened.add(listOf(GitWorktreePath("b.txt")))
    }

    fun testCancellationAddStatusCommitAndCheckoutHonorACancelledToken() = withRepository { root, repository ->
        val tip = commit(repository, "a.txt", "a\n", "one\n")
        repository.createBranch(GitBranchName("other"))
        root.child("b.txt").writeText("b\n")
        repository.add(listOf(GitWorktreePath("b.txt")))
        repository.commit(GitCommitRequest(message = "two\n", author = identity(1), committer = identity(1)))
        val cancelled = GitCancellationToken()
        cancelled.cancel()

        assertGitError(GitErrorCode.cancelled) {
            repository.add(listOf(GitWorktreePath("a.txt")), cancellation = cancelled)
        }
        assertGitError(GitErrorCode.cancelled) { repository.status(cancellation = cancelled) }
        assertGitError(GitErrorCode.cancelled) {
            repository.commit(
                GitCommitRequest(message = "nope\n", author = identity(2), committer = identity(2)),
                cancellation = cancelled,
            )
        }
        assertGitError(GitErrorCode.cancelled) {
            repository.checkout(GitCheckoutRequest.branch(GitBranchName("other")), cancellation = cancelled)
        }
        assertGitError(GitErrorCode.cancelled) { repository.log(GitLogRequest(), cancellation = cancelled) }
        expect(repository.readHeadObjectId() != tip)
        expect(repository.status().isClean)
    }
}
