package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.code.diff.parseUnifiedDiff
import com.forkmesh.nativeapp.model.RepoComparison
import com.forkmesh.nativeapp.testing.assertEq

class BranchDiffTest {
    private val patch = "diff --git a/a.txt b/a.txt\n--- a/a.txt\n+++ b/a.txt\n@@ -1,2 +1,3 @@\n-x\n+y\n+z\n ctx\n" +
        "diff --git a/lib/b.kt b/lib/b.kt\n--- a/lib/b.kt\n+++ b/lib/b.kt\n@@ -10,1 +10,1 @@\n-old\n+new\n"

    fun testComparisonParsesFilesAndFallsBackToTheRawCount() {
        val comparison = BranchComparison(RepoComparison(commitCount = 3, patch = patch, headOid = "0123456789", mergeBaseOid = "abcdef0123"))
        assertEq(2, comparison.files.size)
        assertEq(3, comparison.commitTabCount)
        assertEq(3 to 2, BranchDiff.totals(comparison.files))
        assertEq("head 0123456 · from abcdef0", BranchDiff.headLine(comparison.raw))
        assertEq("3 commits · 2 files", BranchDiff.summary(3, 2))
        assertEq("1 commit · 1 file", BranchDiff.summary(1, 1))
    }

    fun testBaseDefaultsToTheRepositoryDefaultThenMain() {
        assertEq("dev", BranchDiff.base(" dev ", "main"))
        assertEq("trunk", BranchDiff.base("", "trunk"))
        assertEq("main", BranchDiff.base("", ""))
    }

    fun testErrorsReadAsSomethingToDo() {
        assertEq(
            "agent/x is not on a serving mirror yet. Branches appear here once the node that ran the agent has pushed them.",
            BranchDiff.describeError(Exception("ref was not found"), "agent/x", "main"),
        )
        assertEq("agent/x and main share no history, so there is no diff between them to show.", BranchDiff.describeError("no merge base", "agent/x", "main"))
        assertEq("boom", BranchDiff.describeError("boom", "h", "b"))
        assertEq("h has nothing that b does not already have — it may have been merged.", BranchDiff.emptyMessage(0, "h", "b"))
    }

    fun testFilterViewedAndStatBar() {
        val files = parseUnifiedDiff(patch)
        assertEq(listOf("lib/b.kt"), DiffFileFilter.apply(files, " LIB ").map { it.path })
        assertEq(files, DiffFileFilter.apply(files, ""))
        val viewed = DiffFileFilter.toggle(emptySet(), "a.txt")
        assertEq(setOf("a.txt"), viewed)
        assertEq(emptySet<String>(), DiffFileFilter.toggle(viewed, "a.txt"))
        assertEq(1, DiffFileFilter.viewedCount(files, viewed))
        assertEq(3 to 2, DiffFileFilter.statBar(3, 2))
        assertEq(0 to 0, DiffFileFilter.statBar(0, 0))
        assertEq(9, DiffFileFilter.hiddenBefore(files[1], 0))
        assertEq("⋯ 9 unchanged lines", DiffFileFilter.hunkLabel(9))
        assertEq("", DiffFileFilter.hunkLabel(0))
    }

    fun testAges() {
        val now = 1_800_000_000_000L
        assertEq("now", BranchDiff.age(now - 10_000, now))
        assertEq("5m ago", BranchDiff.age(now - 300_000, now))
        assertEq("2h ago", BranchDiff.age(now - 7_200_000, now))
        assertEq("3d ago", BranchDiff.age(now - 3 * 86_400_000L, now))
        assertEq("2mo ago", BranchDiff.age(now - 61 * 86_400_000L, now))
        assertEq("1y ago", BranchDiff.age(now - 400 * 86_400_000L, now))
    }
}
