package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.git.ClearDetector
import com.forkmesh.nativeapp.git.GitCommitRequest
import com.forkmesh.nativeapp.git.GitIdentity
import com.forkmesh.nativeapp.git.GitObjectId
import com.forkmesh.nativeapp.git.GitRepository
import com.forkmesh.nativeapp.git.GitRepositoryInit
import com.forkmesh.nativeapp.git.GitWorktreePath
import com.forkmesh.nativeapp.git.SystemGit
import com.forkmesh.nativeapp.git.child
import com.forkmesh.nativeapp.git.runProcess
import com.forkmesh.nativeapp.git.withTempDir
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class InsightGitSourceTest {
    private class Step(val time: Long, val author: String, val subject: String, val write: Map<String, ByteArray> = emptyMap(), val remove: List<String> = emptyList())

    private fun text(value: String): ByteArray = value.toByteArray(Charsets.UTF_8)

    private fun identity(name: String, seconds: Long) = GitIdentity(name = name, email = "${name.lowercase().replace(' ', '.')}@example.test", timestampSeconds = seconds, timezoneOffsetMinutes = 0)

    private fun apply(repository: GitRepository, step: Step): GitObjectId {
        for ((path, bytes) in step.write) {
            val file = repository.worktree.child(path)
            file.parentFile.mkdirs()
            file.writeBytes(bytes)
        }
        if (step.write.isNotEmpty()) repository.add(step.write.keys.map { GitWorktreePath(it) })
        if (step.remove.isNotEmpty()) {
            repository.remove(step.remove.map { GitWorktreePath(it) })
            for (path in step.remove) repository.worktree.child(path).delete()
        }
        return repository.commit(
            GitCommitRequest(message = "${step.subject}\n\nBody text.\n", author = identity(step.author, step.time), committer = identity(step.author, step.time)),
        )
    }

    private val steps = listOf(
        Step(
            1_000_000, "Ada Lovelace", "Add the first module",
            write = mapOf(
                "src/a.py" to text("one\ntwo\nthree\n"),
                "README.md" to text("# Demo\n"),
                "vendor/lib.js" to text("var x = 1;\n"),
            ),
        ),
        Step(
            1_100_000, "Grace Hopper", "Rework a and add b",
            write = mapOf(
                "src/a.py" to text("one\nTWO\nthree\nfour\nfive\n"),
                "src/b.kt" to text("fun b() = 1\nfun c() = 2\n"),
            ),
        ),
        Step(
            1_200_000, "Ada Lovelace", "Fix crash: drop b, add a binary",
            write = mapOf("src/bin.c" to byteArrayOf(0x7f, 0x45, 0x00, 0x01, 0x0a)),
            remove = listOf("src/b.kt"),
        ),
        Step(1_300_000, "Grace Hopper", "Docs only", write = mapOf("README.md" to text("# Demo\n\nMore.\n"))),
        Step(
            1_400_000, "Ada Lovelace", "Last line without newline",
            write = mapOf("src/a.py" to text("one\nTWO\nthree\nfour\nfive")),
        ),
    )

    private val expected = listOf(
        listOf(InsightCommitFile("src/a.py", 1, 1)),
        emptyList(),
        listOf(InsightCommitFile("src/b.kt", 0, 2), InsightCommitFile("src/bin.c", null, null)),
        listOf(InsightCommitFile("src/a.py", 3, 1), InsightCommitFile("src/b.kt", 2, 0)),
        listOf(InsightCommitFile("src/a.py", 3, 0)),
    )

    private inline fun withHistory(block: (File, List<GitObjectId>) -> Unit) = withTempDir("forkmesh-insight-") { root ->
        val repository = GitRepository.init(GitRepositoryInit(root, detector = ClearDetector()))
        val ids = steps.map { apply(repository, it) }
        block(root, ids)
    }

    private fun source(root: File, now: Long = 1_400_000 + 86400L, workingFiles: Boolean = false) =
        InsightGitSource.open(root, root.name, root.path, readWorkingFiles = workingFiles, clock = { now })

    fun testHistoryMatchesNumstatOfEveryNonMergeCommit() = withHistory { root, ids ->
        val commits = source(root).history(InsightOptions(), { false })!!
        assertEq(ids.reversed().map { it.hex }, commits.map { it.hash })
        assertEq(listOf("Ada Lovelace", "Grace Hopper", "Ada Lovelace", "Grace Hopper", "Ada Lovelace"), commits.map { it.author })
        assertEq(steps.reversed().map { it.time }, commits.map { it.time })
        assertEq(steps.reversed().map { it.subject }, commits.map { it.subject })
        assertEq(expected, commits.map { it.files })
    }

    fun testHistoryStopsAtTheDayWindowAndTheCommitCap() = withHistory { root, ids ->
        val now = 1_250_000L + 86400L
        val windowed = source(root, now).history(InsightOptions(historyDays = 1), { false })!!
        assertEq(listOf(ids[4].hex, ids[3].hex), windowed.map { it.hash })
        val capped = source(root).history(InsightOptions(maxCommits = 2), { false })!!
        assertEq(listOf(ids[4].hex, ids[3].hex), capped.map { it.hash })
    }

    fun testShallowBoundaryCommitIsNotCounted() = withHistory { root, ids ->
        File(root, ".git/shallow").writeText("${ids[2].hex}\n")
        val commits = source(root).history(InsightOptions(), { false })!!
        assertEq(listOf(ids[4].hex, ids[3].hex), commits.map { it.hash })
    }

    fun testEntriesAndReadsComeFromTheHeadTreeOrTheWorkingFile() = withHistory { root, _ ->
        val fromObjects = source(root)
        assertEq(listOf("README.md", "src/a.py", "src/bin.c", "vendor/lib.js"), fromObjects.entries().map { it.path }.sorted())
        assertEq("one\nTWO\nthree\nfour\nfive", String(fromObjects.read("src/a.py", 1000)!!))
        expect(fromObjects.read("src/missing.py", 1000) == null, "missing file reads as null")
        File(root, "src/a.py").writeText("edited\n")
        assertEq("one\nTWO\nthree\nfour\nfive", String(source(root).read("src/a.py", 1000)!!))
        val working = source(root, workingFiles = true)
        assertEq("edited\n", String(working.read("src/a.py", 1000)!!))
        assertEq(7L, working.entries().first { it.path == "src/a.py" }.size)
    }

    fun testAnalyzeUsesTheGitSourceWithHistory() = withHistory { root, _ ->
        val snapshot = Insight.analyze(source(root), InsightOptions(), now = 1_400_000 + 86400L)
        assertEq("", snapshot.error)
        expect(snapshot.git, "git snapshot")
        assertEq(listOf("src/a.py"), snapshot.files.map { it.path })
        assertEq(2, snapshot.skippedFiles)
        val a = snapshot.files[snapshot.indexOf("src/a.py")]
        assertEq(3, a.commits)
        assertEq(7, a.linesAdded)
        assertEq(2, a.linesDeleted)
        assertEq(5, snapshot.commitsScanned)
    }

    fun testTrendVersionsAreTheCommitsThatTouchedTheFileOldestFirst() = withHistory { root, ids ->
        val versions = source(root).versions("src/a.py")
        assertEq(listOf(ids[0].hex, ids[1].hex, ids[4].hex), versions.map { it.commit })
        assertEq("one\ntwo\nthree\n", String(versions.first().bytes))
        val points = source(root).trend("src/a.py")
        assertEq(3, points.size)
        assertEq(listOf(1_000_000L, 1_100_000L, 1_400_000L), points.map { it.time })
        expect(source(root).versions("src/none.py").isEmpty(), "no versions for a missing file")
    }

    fun testSampleIndexesPickSixteenVersionsOfALongHistory() {
        assertEq((0 until 5).toList(), Insight.sampleIndexes(5))
        val picks = Insight.sampleIndexes(100)
        assertEq(16, picks.size)
        assertEq(0, picks.first())
        assertEq(99, picks.last())
    }

    fun testHistoryAgreesWithTheGitCommandLine() = withHistory { root, _ ->
        if (SystemGit.skipped("testHistoryAgreesWithTheGitCommandLine")) return@withHistory
        val ours = source(root).history(InsightOptions(), { false })!!
        assertEq(cliHistory(root), ours)
    }

    fun testHistoryWithMergesAgreesWithTheGitCommandLine() = withTempDir("forkmesh-insight-merge-") { root ->
        if (SystemGit.skipped("testHistoryWithMergesAgreesWithTheGitCommandLine")) return@withTempDir
        var clock = 1_000_000L
        fun git(vararg args: String) {
            clock += 1000
            val date = "@$clock +0000"
            val result = runProcess(
                listOf(SystemGit.executable!!, "-C", root.path) + args,
                environment = SystemGit.environment + mapOf(
                    "GIT_AUTHOR_NAME" to "Ada Lovelace", "GIT_AUTHOR_EMAIL" to "ada@example.test",
                    "GIT_COMMITTER_NAME" to "Ada Lovelace", "GIT_COMMITTER_EMAIL" to "ada@example.test",
                    "GIT_AUTHOR_DATE" to date, "GIT_COMMITTER_DATE" to date,
                ),
                removeEnvironment = SystemGit.removedEnvironment,
            )
            expect(result.exitCode == 0, "git ${args.joinToString(" ")}: ${result.stderr}")
        }
        fun write(path: String, value: String) {
            val file = File(root, path)
            file.parentFile.mkdirs()
            file.writeText(value)
        }
        git("init", "-q", "-b", "main")
        write("src/main.go", (1..40).joinToString("\n") { "line $it" } + "\n")
        write("src/util.go", "package util\n")
        git("add", "-A")
        git("commit", "-q", "-m", "start")
        git("checkout", "-q", "-b", "side")
        write("src/side.ts", "export const a = 1\nexport const b = 2\n")
        write("src/main.go", (1..40).joinToString("\n") { if (it % 7 == 0) "changed $it" else "line $it" } + "\n")
        git("add", "-A")
        git("commit", "-q", "-m", "side work")
        git("checkout", "-q", "main")
        write("src/util.go", "package util\n\nfunc A() {}\nfunc B() {}\n")
        write("docs/notes.md", "notes\n")
        git("add", "-A")
        git("commit", "-q", "-m", "main work")
        git("merge", "-q", "--no-ff", "-m", "merge side", "side")
        write("src/main.go", (1..30).joinToString("\n") { "line $it" } + "\nnew tail\n")
        git("rm", "-q", "src/side.ts")
        git("add", "-A")
        git("commit", "-q", "-m", "after merge")
        val ours = InsightGitSource.open(root, "merge", root.path, clock = { clock + 86400 }).history(InsightOptions(), { false })!!
        assertEq(4, ours.size)
        assertEq(cliHistory(root), ours)
    }

    private fun cliHistory(root: File): List<InsightCommit> {
        val log = SystemGit.git(root, "log", "--no-merges", "--no-renames", "--numstat", "--format=%x1e%H%x1f%an%x1f%at%x1f%s")
        return Insight.parseGitLog(log).map { commit ->
            commit.copy(files = commit.files.filter { Insight.languageForPath(it.path).isNotEmpty() && !Insight.isVendoredPath(it.path) })
        }
    }

    fun testLineDiffCountsAMinimalEditScript() {
        assertEq(InsightLineCounts(0, 0), InsightLineDiff.count(text("a\nb\n"), text("a\nb\n")))
        assertEq(InsightLineCounts(2, 0), InsightLineDiff.count(ByteArray(0), text("a\nb")))
        assertEq(InsightLineCounts(0, 3), InsightLineDiff.count(text("a\nb\nc\n"), ByteArray(0)))
        assertEq(InsightLineCounts(1, 1), InsightLineDiff.count(text("a\nb"), text("a\nb\n")))
        assertEq(InsightLineCounts(2, 2), InsightLineDiff.count(text("a\nb\nc\nd\n"), text("b\na\nd\nc\n")))
        assertEq(InsightLineCounts(1, 1), InsightLineDiff.count(text("x\ny\nz\n"), text("x\nY\nz\n")))
        assertEq(null, InsightLineDiff.count(text("a\n"), byteArrayOf(1, 0, 2)))
        val big = (1..5000).joinToString("\n") { "row $it" }
        val shuffled = (1..5000).joinToString("\n") { if (it % 10 == 0) "edit $it" else "row $it" }
        assertEq(InsightLineCounts(500, 500), InsightLineDiff.count(text(big), text(shuffled)))
    }

    fun testTargetsLabelsAndCache() {
        assertEq(InsightTarget.Account("Owner", "repo"), InsightTarget.parse(InsightTarget.Account("Owner", "repo").key))
        assertEq(InsightTarget.Local("copy"), InsightTarget.parse("local:copy"))
        assertEq(null, InsightTarget.parse("repo:bad"))
        assertEq(InsightHistoryDepth.Recent, InsightHistoryDepth.fromId(null))
        assertEq(InsightHistoryDepth.Extended, InsightHistoryDepth.fromId("2000"))
        expect(!InsightHistoryDepth.FilesOnly.options.history, "files only has no history")
        assertEq(300, InsightHistoryDepth.Recent.options.maxCommits)
        assertEq("On this phone", InsightLabels.sourceLabel(InsightTarget.Local("x"), InsightHistoryDepth.Recent))
        assertEq("Analysis copy · 2,000 commits", InsightLabels.sourceLabel(InsightTarget.Account("o", "n"), InsightHistoryDepth.Extended))
        assertEq("On this phone · 1,204 files · 3 cycles", InsightLabels.statusLine("On this phone", 1204, "3 cycles"))
        assertEq("On this phone · 1 file", InsightLabels.statusLine("On this phone", 1, ""))
        assertEq(null, InsightLabels.copyFolder("..", "x"))
        assertEq("o/n.git", InsightLabels.copyFolder("o", "n.git"))
        var now = 0L
        val cache = InsightSnapshotCache<String>(clock = { now })
        val snapshot = InsightSnapshot(name = "s")
        cache.put("k", snapshot)
        expect(cache.fresh("k") === snapshot, "fresh within ten minutes")
        now = InsightSnapshotCache.TTL_MILLIS + 1
        expect(cache.fresh("k") == null, "stale after ten minutes")
        expect(cache.latest("k") === snapshot, "latest keeps the stale snapshot")
    }
}
