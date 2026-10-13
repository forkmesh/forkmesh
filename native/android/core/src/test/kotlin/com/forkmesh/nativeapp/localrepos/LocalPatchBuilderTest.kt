package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.GitObjectId
import com.forkmesh.nativeapp.git.GitRepository
import com.forkmesh.nativeapp.git.GitRepositoryOpen
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class LocalPatchBuilderTest {
    private val author = GitAuthor(name = "Alice Mobile", email = "alice@example.com")

    private fun patchFor(temp: File, first: Map<String, String>, second: Map<String, String?>, dir: String = "repo"): PatchBuildResult {
        val worktree = File(temp, dir)
        val workspace = GitWorkspace(worktree)
        try {
            val baseId = workspace.init(files = first, commitMessage = "base\n", author = author)!!
            for ((path, text) in second) {
                if (text == null) workspace.deleteFile(path) else workspace.writeFile(path, text)
            }
            workspace.stageAll()
            val headId = try {
                workspace.commit(message = "head\n", author = author)
            } catch (error: GitWorkspaceException) {
                baseId
            }
            val repository = GitRepository.open(GitRepositoryOpen(worktree))
            return PatchBuilder.between(repository, GitObjectId.parseSha1(baseId), GitObjectId.parseSha1(headId))
        } finally {
            workspace.dispose()
        }
    }

    private fun baseRepo(temp: File, name: String, baseFiles: Map<String, String>): File {
        val dir = File(temp, name).apply { mkdirs() }
        HostGit.run(dir, "init", "-q")
        for ((path, text) in baseFiles) {
            val file = File(dir, path)
            file.parentFile.mkdirs()
            file.writeText(text)
        }
        HostGit.run(dir, "add", "-A")
        HostGit.run(dir, "-c", "user.email=a@b.c", "-c", "user.name=A", "commit", "-q", "-m", "base")
        return dir
    }

    private fun expectApplies(temp: File, baseFiles: Map<String, String>, patch: String, label: String) {
        val dir = baseRepo(temp, label, baseFiles)
        File(dir, "change.patch").writeText(patch)
        val result = HostGit.run(dir, "apply", "--check", "-v", "change.patch")
        assertEq(0, result.first, "git rejected the patch: ${result.second}\n--- patch ---\n$patch")
    }

    private fun gitApply(temp: File, baseFiles: Map<String, String>, patch: String, readBack: List<String>, name: String): Map<String, String> {
        val dir = baseRepo(temp, name, baseFiles)
        File(dir, "change.patch").writeText(patch)
        val applied = HostGit.run(dir, "apply", "change.patch")
        assertEq(0, applied.first, "git apply failed: ${applied.second}\n--- patch ---\n$patch")
        return readBack.associateWith { path -> File(dir, path).let { if (it.exists()) it.readText() else "<missing>" } }
    }

    private fun markers(patch: String): Int = Regex("@@").findAll(patch).count()

    fun testAModifiedFile() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "one\ntwo\nthree\nfour\nfive\n")
        val result = patchFor(temp, base, mapOf("a.txt" to "one\ntwo\nCHANGED\nfour\nfive\n"))
        assertEq(listOf("a.txt"), result.changedPaths)
        expect(result.patch.contains("diff --git a/a.txt b/a.txt"))
        expect(result.patch.contains("@@"))
        expect(result.patch.contains("-three"))
        expect(result.patch.contains("+CHANGED"))
        expect(result.patch.contains(" two"))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "mod")
        assertEq("one\ntwo\nCHANGED\nfour\nfive\n", gitApply(temp, base, result.patch, listOf("a.txt"), "mod-apply")["a.txt"])
    }

    fun testANewFile() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "keep\n")
        val result = patchFor(temp, base, mapOf("lib/new.dart" to "void main() {}\n"))
        expect(result.patch.contains("new file mode 100644"))
        expect(result.patch.contains("--- /dev/null"))
        expect(result.patch.contains("@@ -0,0 +1 @@"))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "new")
        assertEq("void main() {}\n", gitApply(temp, base, result.patch, listOf("lib/new.dart"), "new-apply")["lib/new.dart"])
    }

    fun testADeletedFile() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "keep\n", "gone.txt" to "bye\nnow\n")
        val result = patchFor(temp, base, mapOf("gone.txt" to null))
        expect(result.patch.contains("deleted file mode 100644"))
        expect(result.patch.contains("+++ /dev/null"))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "del")
        assertEq("<missing>", gitApply(temp, base, result.patch, listOf("gone.txt"), "del-apply")["gone.txt"])
    }

    fun testSeveralFilesAtOnce() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "alpha\n", "b.txt" to "beta\nbeta2\n", "c.txt" to "gamma\n")
        val result = patchFor(temp, base, mapOf("a.txt" to "alpha changed\n", "b.txt" to null, "d.txt" to "delta\n"))
        assertEq(listOf("a.txt", "b.txt", "d.txt"), result.changedPaths)
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "multi")
        val applied = gitApply(temp, base, result.patch, listOf("a.txt", "b.txt", "d.txt"), "multi-apply")
        assertEq("alpha changed\n", applied["a.txt"])
        assertEq("<missing>", applied["b.txt"])
        assertEq("delta\n", applied["d.txt"])
    }

    fun testSeparateEditsFarApartProduceSeparateHunks() = withScratch("fm-patch-") { temp ->
        val lines = (1..60).map { "line $it" }
        val base = mapOf("big.txt" to "${lines.joinToString("\n")}\n")
        val edited = lines.toMutableList()
        edited[2] = "line 3 CHANGED"
        edited[50] = "line 51 CHANGED"
        val result = patchFor(temp, base, mapOf("big.txt" to "${edited.joinToString("\n")}\n"))
        assertEq(4, markers(result.patch))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "hunks")
        assertEq("${edited.joinToString("\n")}\n", gitApply(temp, base, result.patch, listOf("big.txt"), "hunks-apply")["big.txt"])
    }

    fun testAdjacentEditsMergeIntoOneHunk() = withScratch("fm-patch-") { temp ->
        val lines = (1..20).map { "line $it" }
        val base = mapOf("near.txt" to "${lines.joinToString("\n")}\n")
        val edited = lines.toMutableList()
        edited[9] = "line 10 CHANGED"
        edited[11] = "line 12 CHANGED"
        val result = patchFor(temp, base, mapOf("near.txt" to "${edited.joinToString("\n")}\n"))
        assertEq(2, markers(result.patch))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "near")
    }

    fun testAFileWithNoTrailingNewline() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "one\ntwo")
        val result = patchFor(temp, base, mapOf("a.txt" to "one\nTWO"))
        expect(result.patch.contains("\\ No newline at end of file"))
        expect(!result.patch.contains('\u0000'))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "nonewline")
        assertEq("one\nTWO", gitApply(temp, base, result.patch, listOf("a.txt"), "nonewline-apply")["a.txt"])
    }

    fun testGainingATrailingNewline() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "only")
        val result = patchFor(temp, base, mapOf("a.txt" to "only\n"))
        expect(!result.isEmpty)
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "gainnl")
        assertEq("only\n", gitApply(temp, base, result.patch, listOf("a.txt"), "gainnl-apply")["a.txt"])
    }

    fun testAnEmptiedFile() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "gone\nsoon\n")
        val result = patchFor(temp, base, mapOf("a.txt" to ""))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "empty")
        assertEq("", gitApply(temp, base, result.patch, listOf("a.txt"), "empty-apply")["a.txt"])
    }

    fun testAWholeFileRewrite() = withScratch("fm-patch-") { temp ->
        val base = mapOf("r.txt" to "${(0 until 40).joinToString("\n") { "old $it" }}\n")
        val rewritten = "${(0 until 40).joinToString("\n") { "new $it" }}\n"
        val result = patchFor(temp, base, mapOf("r.txt" to rewritten))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "rewrite")
        assertEq(rewritten, gitApply(temp, base, result.patch, listOf("r.txt"), "rewrite-apply")["r.txt"])
    }

    fun testARewriteBeyondTheEditBudgetIsStillAValidPatch() = withScratch("fm-patch-") { temp ->
        val base = mapOf("r.txt" to "${(0 until 700).joinToString("\n") { "old $it" }}\n")
        val rewritten = "${(0 until 700).joinToString("\n") { "new $it" }}\n"
        val result = patchFor(temp, base, mapOf("r.txt" to rewritten))
        if (!HostGit.available) return@withScratch
        assertEq(rewritten, gitApply(temp, base, result.patch, listOf("r.txt"), "budget-apply")["r.txt"])
    }

    fun testANestedPath() = withScratch("fm-patch-") { temp ->
        val base = mapOf("lib/src/deep/thing.dart" to "const a = 1;\n")
        val result = patchFor(temp, base, mapOf("lib/src/deep/thing.dart" to "const a = 2;\n"))
        expect(result.patch.contains("diff --git a/lib/src/deep/thing.dart b/lib/src/deep/thing.dart"))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "nested")
    }

    fun testAnInsertionAtTheVeryStartOfAFile() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "first\nsecond\n")
        val result = patchFor(temp, base, mapOf("a.txt" to "zeroth\nfirst\nsecond\n"))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "prepend")
        assertEq("zeroth\nfirst\nsecond\n", gitApply(temp, base, result.patch, listOf("a.txt"), "prepend-apply")["a.txt"])
    }

    fun testAnAppendAtTheVeryEndOfAFile() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "first\nsecond\n")
        val result = patchFor(temp, base, mapOf("a.txt" to "first\nsecond\nthird\n"))
        if (!HostGit.available) return@withScratch
        expectApplies(temp, base, result.patch, "append")
        assertEq("first\nsecond\nthird\n", gitApply(temp, base, result.patch, listOf("a.txt"), "append-apply")["a.txt"])
    }

    fun testABinaryChangeIsReportedNeverSilentlyDropped() = withScratch("fm-patch-") { temp ->
        val worktree = File(temp, "bin")
        val workspace = GitWorkspace(worktree)
        try {
            val baseId = workspace.init(files = mapOf("a.txt" to "text\n"), commitMessage = "base\n", author = author)!!
            workspace.writeFile("logo.png", "PNG\u0000\u0000binary")
            workspace.writeFile("a.txt", "text changed\n")
            workspace.stageAll()
            val headId = workspace.commit(message = "head\n", author = author)
            val repository = GitRepository.open(GitRepositoryOpen(worktree))
            val result = PatchBuilder.between(repository, GitObjectId.parseSha1(baseId), GitObjectId.parseSha1(headId))
            expect(result.hasBinaryChanges)
            assertEq(listOf("logo.png"), result.binaryPaths)
            assertEq(listOf("a.txt"), result.changedPaths)
            expect(!result.patch.contains("logo.png"))
        } finally {
            workspace.dispose()
        }
    }

    fun testNoChangeProducesAnEmptyResult() = withScratch("fm-patch-") { temp ->
        val result = patchFor(temp, mapOf("a.txt" to "same\n"), mapOf("a.txt" to "same\n"))
        expect(result.isEmpty)
        expect(result.changedPaths.isEmpty())
    }

    fun testIndexLinesCarryRealBlobIdsSo3wayCanWork() = withScratch("fm-patch-") { temp ->
        val result = patchFor(temp, mapOf("a.txt" to "one\n"), mapOf("a.txt" to "two\n"))
        val indexLine = result.patch.split("\n").first { it.startsWith("index ") }
        val ids = indexLine.substring(6).split(" ").first().split("..")
        assertEq(2, ids.size)
        val hex = Regex("[0-9a-f]{40}")
        expect(hex.matches(ids[0]))
        expect(hex.matches(ids[1]))
        expect(ids[0] != ids[1])
        expect(indexLine.endsWith(" 100644"), indexLine)
    }

    fun testThreeWayApplyWorksWhenTheTargetHasMovedOn() = withScratch("fm-patch-") { temp ->
        val base = mapOf("a.txt" to "one\ntwo\nthree\nfour\nfive\n")
        val result = patchFor(temp, base, mapOf("a.txt" to "one\ntwo\nTHREE\nfour\nfive\n"))
        if (!HostGit.available) return@withScratch
        val dir = baseRepo(temp, "threeway", base)
        File(dir, "a.txt").writeText("one\ntwo\nthree\nfour\nfive\nsix\n")
        HostGit.run(dir, "add", "-A")
        HostGit.run(dir, "-c", "user.email=a@b.c", "-c", "user.name=A", "commit", "-q", "-m", "moved on")
        File(dir, "change.patch").writeText(result.patch)
        val applied = HostGit.run(dir, "apply", "--3way", "change.patch")
        assertEq(0, applied.first, "three-way apply failed: ${applied.second}")
        assertEq("one\ntwo\nTHREE\nfour\nfive\nsix\n", File(dir, "a.txt").readText())
    }
}
