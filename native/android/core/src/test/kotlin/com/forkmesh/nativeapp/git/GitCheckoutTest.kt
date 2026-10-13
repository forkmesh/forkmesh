package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Paths

class GitCheckoutTest {
    private val identity = GitIdentity(name = "Ada", email = "ada@example.test", timestampSeconds = 0, timezoneOffsetMinutes = 0)

    private fun commitAll(repository: GitRepository, paths: List<String>, message: String): GitObjectId {
        repository.add(paths.map { GitWorktreePath(it) })
        return repository.commit(GitCommitRequest(message = message, author = identity, committer = identity))
    }

    private fun switchTo(repository: GitRepository, branch: String) =
        repository.checkout(GitCheckoutRequest.branch(GitBranchName(branch)))

    private fun isSymlink(file: File): Boolean = Files.isSymbolicLink(file.toPath())

    private fun exists(file: File): Boolean = Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS)

    fun testCheckoutRewritesOnlyChangedFilesAndPrunesEmptiedFolders() = withTempDir("forkmesh-checkout-") { root ->
        val repository = GitRepository.init(GitRepositoryInit(root))
        root.child("docs").mkdirs()
        root.child("docs/guide.md").writeText("guide\n")
        root.child("README.md").writeText("readme\n")
        Files.createSymbolicLink(root.child("link").toPath(), Paths.get("README.md"))
        commitAll(repository, listOf("docs/guide.md", "README.md", "link"), "Docs\n")
        repository.createBranch(GitBranchName("topic"))
        switchTo(repository, "topic")
        root.child("docs/guide.md").delete()
        Files.delete(root.child("link").toPath())
        repository.remove(listOf(GitWorktreePath("docs/guide.md"), GitWorktreePath("link")))
        root.child("step.txt").writeText("synced\n")
        commitAll(repository, listOf("step.txt"), "Drop the guide\n")
        root.child("docs").delete()
        switchTo(repository, "main")
        val readme = root.child("README.md")
        val old = 1_000_000_000_000L
        expect(readme.setLastModified(old), "README mtime can be set")

        switchTo(repository, "topic")

        assertEq("synced\n", root.child("step.txt").readText())
        expect(!exists(root.child("docs")), "the emptied docs folder is pruned")
        expect(!exists(root.child("link")), "the removed symlink is deleted")
        assertEq(old, readme.lastModified(), "an unchanged file is not rewritten")
        expect(repository.status().isClean, "the switch leaves a clean worktree")

        switchTo(repository, "main")

        assertEq("guide\n", root.child("docs/guide.md").readText())
        expect(isSymlink(root.child("link")), "the symlink is restored")
        expect(!exists(root.child("step.txt")), "the topic file is removed")
        assertEq(old, readme.lastModified(), "an unchanged file is still not rewritten")
        expect(repository.status().isClean, "the switch back leaves a clean worktree")
    }

    fun testCheckoutSwapsAFileForASymlinkAndAFolderForAFile() = withTempDir("forkmesh-checkout-") { root ->
        val repository = GitRepository.init(GitRepositoryInit(root))
        root.child("a").writeText("a\n")
        root.child("target.txt").writeText("target\n")
        root.child("dir").mkdirs()
        root.child("dir/b").writeText("b\n")
        commitAll(repository, listOf("a", "target.txt", "dir/b"), "Files\n")
        repository.createBranch(GitBranchName("topic"))
        switchTo(repository, "topic")
        Files.delete(root.child("a").toPath())
        Files.createSymbolicLink(root.child("a").toPath(), Paths.get("target.txt"))
        Files.delete(root.child("dir/b").toPath())
        Files.delete(root.child("dir").toPath())
        root.child("dir").writeText("now a file\n")
        repository.remove(listOf(GitWorktreePath("dir/b")))
        commitAll(repository, listOf("a", "dir"), "Swap\n")

        switchTo(repository, "main")

        expect(!isSymlink(root.child("a")), "a is a regular file again")
        assertEq("a\n", root.child("a").readText())
        assertEq("b\n", root.child("dir/b").readText())
        expect(repository.status().isClean, "main is clean")

        switchTo(repository, "topic")

        expect(isSymlink(root.child("a")), "a is a symlink on topic")
        assertEq("target.txt", Files.readSymbolicLink(root.child("a").toPath()).toString())
        assertEq("now a file\n", root.child("dir").readText())
        expect(repository.status().isClean, "topic is clean")
    }

    fun testCheckoutRefusesATreeThatWritesIntoTheGitDirectory() = withTempDir("forkmesh-checkout-") { root ->
        val repository = GitRepository.init(GitRepositoryInit(root))
        root.child("a.txt").writeText("a\n")
        val parent = commitAll(repository, listOf("a.txt"), "Start\n")
        val config = root.child(".git/config").readText()
        val head = root.child(".git/HEAD").readText()
        val store = LooseObjectStore(root.child(".git"), GitResourceLimits())
        val planted = store.write(GitObjectType.blob, utf8("[core]\n\tbare = true\n"))
        val readme = store.write(GitObjectType.blob, utf8("a\n"))
        for ((index, name) in listOf(".git", ".GIT", ".Git").withIndex()) {
            val inner = store.write(
                GitObjectType.tree,
                GitTree(listOf(GitTreeEntry.regular(utf8("config"), planted), GitTreeEntry.regular(utf8("HEAD"), planted))).encode(),
            )
            val top = store.write(
                GitObjectType.tree,
                GitTree(listOf(GitTreeEntry.directory(utf8(name), inner), GitTreeEntry.regular(utf8("a.txt"), readme))).encode(),
            )
            val commit = store.write(
                GitObjectType.commit,
                utf8(
                    "tree ${top.hex}\nparent ${parent.hex}\n" +
                        "author Ada <ada@example.test> 0 +0000\ncommitter Ada <ada@example.test> 0 +0000\n\nPlant\n",
                ),
            )
            val branch = "evil$index"
            root.child(".git/refs/heads/$branch").writeText("${commit.hex}\n")

            val error = assertFails<GitException> { switchTo(repository, branch) }

            assertEq(GitErrorCode.pathEscapesWorktree, error.code, name)
            assertEq(config, root.child(".git/config").readText(), "$name leaves .git/config alone")
            assertEq(head, root.child(".git/HEAD").readText(), "$name leaves HEAD alone")
        }
    }
}
