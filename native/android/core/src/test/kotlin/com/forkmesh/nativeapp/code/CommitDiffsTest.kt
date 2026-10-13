package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.model.RepoCommitFile
import com.forkmesh.nativeapp.testing.assertEq

class CommitDiffsTest {
    private val now = 1_786_636_663_064L
    private val day = 86_400_000L

    fun testCommitDetailDerivesChangeLettersFromTheDiff() {
        val diff = "diff --git a/lib/new.dart b/lib/new.dart\n" +
            "new file mode 100644\n" +
            "--- /dev/null\n" +
            "+++ b/lib/new.dart\n" +
            "+a\n+b\n+c\n+d\n" +
            "diff --git a/lib/old.dart b/lib/old.dart\n" +
            "deleted file mode 100644\n" +
            "--- a/lib/old.dart\n" +
            "+++ /dev/null\n" +
            "-x\n"
        assertEq(
            listOf(PatchFile("lib/new.dart", ChangeKind.Added, 4, 0), PatchFile("lib/old.dart", ChangeKind.Deleted, 0, 1)),
            Patches.parsePatchFiles(diff),
        )
        val files = listOf(RepoCommitFile(path = "lib/new.dart", adds = "4", dels = "0"), RepoCommitFile(path = "lib/old.dart", adds = "0", dels = "9"))
        assertEq(4 to 9, Patches.commitFileTotals(files))
        assertEq("2 changed files", Patches.changedFilesTitle(files.size))
        assertEq("", Patches.visibleCount("0"))
        assertEq("9", Patches.visibleCount("9"))
    }

    fun testPullPatchSummarisesFilesChanged() {
        val patch = "diff --git a/app/a.py b/app/a.py\n" +
            "--- a/app/a.py\n" +
            "+++ b/app/a.py\n" +
            "@@ -1,2 +1,2 @@\n" +
            "-old\n" +
            "+new\n" +
            "+more\n" +
            "diff --git a/app/b.py b/app/b.py\n" +
            "new file mode 100644\n" +
            "--- /dev/null\n" +
            "+++ b/app/b.py\n" +
            "+hello\n"
        val files = Patches.parsePatchFiles(patch)
        assertEq(listOf("app/a.py", "app/b.py"), files.map { it.path })
        assertEq(listOf(ChangeKind.Modified, ChangeKind.Added), files.map { it.kind })
        assertEq(3, files.sumOf { it.adds })
        assertEq(1, files.sumOf { it.dels })
    }

    fun testRenamesAndHeaderlessPatches() {
        val renamed = "diff --git a/old name.txt b/new name.txt\nsimilarity index 90%\nrename from old name.txt\nrename to docs/new name.txt\n"
        assertEq(listOf(PatchFile("docs/new name.txt", ChangeKind.Renamed)), Patches.parsePatchFiles(renamed))
        assertEq(emptyList<PatchFile>(), Patches.parsePatchFiles("  \n"))
        assertEq(listOf(PatchFile("x", ChangeKind.Modified, 1, 0)), Patches.parsePatchFiles("--- a/x\n+++ b/x\n+y\n"))
    }

    fun testCommitDetailTotalsFromTheBranchTest() {
        val diff = "diff --git a/lib/main.dart b/lib/main.dart\n+hello\n-old"
        assertEq(listOf(PatchFile("lib/main.dart", ChangeKind.Modified, 1, 1)), Patches.parsePatchFiles(diff))
        assertEq(12 to 3, Patches.commitFileTotals(listOf(RepoCommitFile(path = "lib/main.dart", adds = "12", dels = "3"))))
        assertEq("1 changed file", Patches.changedFilesTitle(1))
        assertEq(0 to 0, Patches.commitFileTotals(listOf(RepoCommitFile(path = "bin", adds = "-", dels = ""))))
    }

    fun testDiffLineKinds() {
        assertEq(DiffLineKind.Added, Patches.diffLineKind("+hello"))
        assertEq(DiffLineKind.Context, Patches.diffLineKind("+++ b/x"))
        assertEq(DiffLineKind.Removed, Patches.diffLineKind("-old"))
        assertEq(DiffLineKind.Context, Patches.diffLineKind("--- a/x"))
        assertEq(DiffLineKind.Hunk, Patches.diffLineKind("@@ -1 +1 @@"))
        assertEq(DiffLineKind.Context, Patches.diffLineKind("diff --git a/x b/x"))
        assertEq("lib/" to "main.dart", Patches.splitPath("lib/main.dart"))
        assertEq("" to "README.md", Patches.splitPath("README.md"))
    }

    fun testCommitFieldFallbacks() {
        val commit = mapOf<String, Any?>("hash" to "abcdef1234567890", "author" to "Mona", "subject" to "Add mobile commit diff\nbody")
        assertEq("abcdef1234567890", CommitFields.hash(commit))
        assertEq("abcdef1", CommitFields.short(commit))
        assertEq("Add mobile commit diff", CommitFields.subject(commit))
        assertEq("Mona", CommitFields.author(commit))
        assertEq("abc", CommitFields.hash(mapOf("sha" to "abc")))
        assertEq("abc", CommitFields.short(mapOf("id" to "abc")))
        assertEq("Commit", CommitFields.subject(mapOf("message" to "  \nsecond")))
        assertEq("Commit", CommitFields.subject(emptyMap()))
        assertEq("Ada", CommitFields.author(mapOf("author" to mapOf("email" to "Ada"))))
        assertEq("Grace", CommitFields.author(mapOf("committer" to "Grace")))
        assertEq("Shows changed files.", CommitFields.body(mapOf("description" to "Shows changed files.")))
        assertEq(mapOf("hash" to "abc", "message" to "Commit abc").let { CommitFields.subject(it) }, "Commit abc")
    }

    fun testCommitDatesAndCompactAges() {
        assertEq(1_700_000_000_000L, CommitFields.dateMs(mapOf("date" to 1_700_000_000L)))
        assertEq(1_700_000_000_123L, CommitFields.dateMs(mapOf("ts" to 1_700_000_000_123L)))
        assertEq(1_700_000_000_000L, CommitFields.dateMs(mapOf("timestamp" to "1700000000")))
        assertEq(1_700_000_000_000L, CommitFields.dateMs(mapOf("time" to 1_699_999_999.6)))
        assertEq(null, CommitFields.dateMs(mapOf("date" to "yesterday")))
        assertEq("", CommitFields.relativeTime(emptyMap(), now))
        assertEq("3d", CommitFields.relativeTime(mapOf("date" to now - 3 * day), now))
        assertEq("now", compactAge(now + 5_000, now))
        assertEq("59m", compactAge(now - 59 * 60_000, now))
        assertEq("23h", compactAge(now - 23 * 3_600_000, now))
        assertEq("1mo", compactAge(now - 45 * day, now))
        assertEq("2y", compactAge(now - 800 * day, now))
    }
}
