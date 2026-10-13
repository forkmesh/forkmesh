package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.nio.file.Files

class GitWorktreeTest {
    private val identity: GitIdentity
        get() = GitIdentity(
            name = "Ada Lovelace",
            email = "ada@example.test",
            timestampSeconds = 0,
            timezoneOffsetMinutes = 0,
        )

    fun testStagesANestedFileWritesACommitAndReturnsToCleanStatus() = withTempDir("forkmesh-authoring-") { root ->
        val repository = GitRepository.init(GitRepositoryInit(root))
        val docs = root.child("docs")
        docs.mkdir()
        docs.child("readme.txt").writeText("hello\n")

        repository.add(listOf(GitWorktreePath("docs/readme.txt")))
        val staged = repository.status()

        assertEq("docs/readme.txt", staged.staged.single().path.value)
        assertEq(GitChangeKind.added, staged.staged.single().kind)
        expect(staged.worktree.isEmpty())

        val commitId = repository.commit(
            GitCommitRequest(message = "Add a nested README\n", author = identity, committer = identity),
        )

        val commit = repository.readCommit(commitId)
        val tree = repository.readTree(commit.tree)
        assertEq(GitTreeMode.directory, tree.entries.single().mode)
        val nested = repository.readTree(tree.entries.single().objectId)
        assertEq("hello\n", utf8Text(repository.readBlob(nested.entries.single().objectId)))
        assertEq(commitId, repository.readHeadObjectId())
        expect(repository.status().isClean)
    }

    private fun commitFile(repository: GitRepository, root: File, content: String, message: String) {
        root.child("note.txt").writeText(content)
        repository.add(listOf(GitWorktreePath("note.txt")))
        repository.commit(GitCommitRequest(message = message, author = identity, committer = identity))
    }

    fun testCreatesABranchAndSafelyMaterializesItsCommittedFilesOnCheckout() = withTempDir("forkmesh-checkout-") { root ->
        val repository = GitRepository.init(GitRepositoryInit(root))

        commitFile(repository, root, "main\n", "main commit\n")
        repository.createBranch(GitBranchName("feature"))
        repository.checkout(GitCheckoutRequest.branch(GitBranchName("feature")))
        commitFile(repository, root, "feature\n", "feature commit\n")

        repository.checkout(GitCheckoutRequest.branch(GitBranchName("main")))

        assertEq("main\n", root.child("note.txt").readText())
        assertEq(GitSymbolicRef("refs/heads/main"), repository.readHead())
    }

    fun testReportsAnUntrackedFileWithoutTreatingGitMetadataAsWorktreeContent() = withTempDir("forkmesh-status-") { root ->
        val repository = GitRepository.init(GitRepositoryInit(root))
        root.child("draft.txt").writeText("draft\n")

        val status = repository.status()

        expect(status.staged.isEmpty())
        assertEq(1, status.worktree.size)
        assertEq("draft.txt", status.worktree.single().path.value)
        assertEq(GitChangeKind.untracked, status.worktree.single().kind)
    }

    fun testAcceptsAPortableUtf8RepositoryRelativePath() {
        val path = GitWorktreePath("nested/café.txt")

        assertEq(listOf("nested", "café.txt"), path.components)
        assertEq("nested/café.txt", utf8Text(path.bytes))
    }

    fun testRejectsTraversalDeviceAndPlatformAmbiguousPathNames() {
        for (value in listOf(
            "../outside",
            "/absolute",
            "nested/../outside",
            "CON",
            "name.",
            "name ",
            "name:stream",
            "nested\\backslash",
        )) {
            assertFails<GitException> { GitWorktreePath(value) }
        }
    }

    fun testRefusesToTraverseAnExistingSymlinkWhileWriting() = withTempDir("forkmesh-worktree-") { root ->
        withTempDir("forkmesh-outside-") { outside ->
            Files.createSymbolicLink(root.child("escape").toPath(), outside.toPath())
            val worktree = GitWorktree(root)

            assertGitError(GitErrorCode.symlinkEscape) {
                worktree.writeFile(GitWorktreePath("escape/owned.txt"), byteArrayOf(1))
            }
            expect(!outside.child("owned.txt").exists())
        }
    }
}
