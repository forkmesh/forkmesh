package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.testing.assertEq

class PatchFilesTest {
    fun testParsesFilesKindsAndCounts() {
        val diff = listOf(
            "diff --git a/lib/a.dart b/lib/a.dart",
            "index 1..2 100644",
            "--- a/lib/a.dart",
            "+++ b/lib/a.dart",
            "@@ -1 +1,2 @@",
            "-old",
            "+new",
            "+more",
            "diff --git a/new.txt b/new.txt",
            "new file mode 100644",
            "--- /dev/null",
            "+++ b/new.txt",
            "+hello",
            "diff --git a/gone.txt b/gone.txt",
            "deleted file mode 100644",
            "--- a/gone.txt",
            "+++ /dev/null",
            "-bye",
            "diff --git a/old/name.kt b/new/name.kt",
            "similarity index 90%",
            "rename from old/name.kt",
            "rename to new/name.kt",
        ).joinToString("\r\n") + "\n"
        assertEq(
            listOf(
                PatchFile("lib/a.dart", PatchChangeKind.Modified, 2, 1),
                PatchFile("new.txt", PatchChangeKind.Added, 1, 0),
                PatchFile("gone.txt", PatchChangeKind.Deleted, 0, 1),
                PatchFile("new/name.kt", PatchChangeKind.Renamed, 0, 0),
            ),
            parsePatchFiles(diff),
        )
    }

    fun testTheCollaborationFixtureAndEmptyDiffs() {
        assertEq(
            listOf(PatchFile("lib/a.dart", PatchChangeKind.Modified, 1, 0)),
            parsePatchFiles("diff --git a/lib/a.dart b/lib/a.dart\n+review"),
        )
        assertEq(emptyList<PatchFile>(), parsePatchFiles("  \n"))
        assertEq(emptyList<PatchFile>(), parsePatchFiles("+orphan line"))
    }

    fun testLineSplittingAndTitles() {
        assertEq(listOf("a", "b", "", "c"), dartLines("a\r\nb\n\rc"))
        assertEq(listOf("a", ""), dartLines("a\n\n"))
        assertEq(emptyList<String>(), dartLines(""))
        assertEq("1 changed file", changedFilesTitle(1))
        assertEq("3 changed files", changedFilesTitle(3))
        assertEq(DiffLineKind.Added, diffLineKind("+x"))
        assertEq(DiffLineKind.Context, diffLineKind("+++ b/x"))
        assertEq(DiffLineKind.Removed, diffLineKind("-x"))
        assertEq(DiffLineKind.Context, diffLineKind("--- a/x"))
        assertEq(DiffLineKind.Hunk, diffLineKind("@@ -1 +1 @@"))
        assertEq(DiffLineKind.Context, diffLineKind(" same"))
    }
}
