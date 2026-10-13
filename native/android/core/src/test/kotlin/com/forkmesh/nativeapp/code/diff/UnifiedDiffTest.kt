package com.forkmesh.nativeapp.code.diff

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.ZoneOffset
import java.time.ZonedDateTime

class UnifiedDiffTest {
    private val patch = listOf(
        "diff --git a/lib/app.dart b/lib/app.dart",
        "index 1111111..2222222 100644",
        "--- a/lib/app.dart",
        "+++ b/lib/app.dart",
        "@@ -1,5 +1,6 @@ class App {",
        " import 'a.dart';",
        "-final name = 'old';",
        "+final name = 'new';",
        "+final extra = 1;",
        "",
        " -- not a header",
        "-- removed dashes",
        "@@ -20,2 +21,2 @@ void run() {",
        "-\ttabbed();",
        "+\ttabbed(2);",
        " end();",
        "\\ No newline at end of file",
        "diff --git a/docs/old name.md b/docs/new name.md",
        "similarity index 90%",
        "rename from docs/old name.md",
        "rename to docs/new name.md",
        "index 3333333..4444444 100644",
        "--- a/docs/old name.md",
        "+++ b/docs/new name.md",
        "@@ -1 +1 @@",
        "-Hello",
        "+Hello there",
        "diff --git a/assets/logo.png b/assets/logo.png",
        "new file mode 100644",
        "index 0000000..5555555",
        "GIT binary patch",
        "literal 10",
        "Rcmd;L0000000000",
        "",
        "literal 0",
        "HcmV?d00001",
        "",
        "diff --git a/gone.txt b/gone.txt",
        "deleted file mode 100644",
        "index 6666666..0000000",
        "--- a/gone.txt",
        "+++ /dev/null",
        "@@ -1,2 +0,0 @@",
        "-one",
        "-two",
        "diff --git a/run.sh b/run.sh",
        "old mode 100644",
        "new mode 100755",
    ).joinToString("\n", postfix = "\n")

    fun testParsesFilesStatusesAndNumberedLines() {
        val files = parseUnifiedDiff(patch)
        assertEq(listOf("lib/app.dart", "docs/new name.md", "assets/logo.png", "gone.txt", "run.sh"), files.map { it.path })

        val app = files[0]
        assertEq(DiffFileStatus.Modified, app.status)
        assertEq(2, app.hunks.size)
        assertEq("class App {", app.hunks.first().section)
        val first = app.hunks.first().lines
        assertEq(
            listOf(
                DiffLineKind.Context, DiffLineKind.Removed, DiffLineKind.Added, DiffLineKind.Added,
                DiffLineKind.Context, DiffLineKind.Context, DiffLineKind.Removed,
            ),
            first.map { it.kind },
        )
        assertEq("-- not a header", first[5].text)
        assertEq("- removed dashes", first[6].text)
        assertEq(2, first[1].oldNumber)
        assertEq(0, first[1].newNumber)
        assertEq(3, first[3].newNumber)
        assertEq(3, first[4].oldNumber)
        assertEq(4, first[4].newNumber)
        assertEq(3, app.additions)
        assertEq(3, app.deletions)

        val second = app.hunks[1].lines
        assertEq("    tabbed();", second.first().text)
        expect(second.last().noNewlineAtEnd)
        assertEq(21, second.last().oldNumber)
        assertEq(22, second.last().newNumber)

        val renamed = files[1]
        assertEq(DiffFileStatus.Renamed, renamed.status)
        assertEq("docs/old name.md", renamed.oldPath)
        assertEq(90, renamed.similarity)

        val binary = files[2]
        expect(binary.binary)
        assertEq(DiffFileStatus.Added, binary.status)
        expect(binary.hunks.isEmpty())

        val gone = files[3]
        assertEq(DiffFileStatus.Deleted, gone.status)
        assertEq("gone.txt", gone.path)
        assertEq(2, gone.deletions)

        val mode = files[4]
        expect(mode.modeChanged)
        assertEq("100755", mode.newMode)
        expect(mode.raw.startsWith("diff --git a/run.sh b/run.sh"))
    }

    fun testPairsChangedLinesForSideBySideAndMarksWhatChanged() {
        val app = parseUnifiedDiff(patch).first()
        val rows = app.splitRows
        val paired = rows.first { it.left?.text == "final name = 'old';" }
        assertEq("final name = 'new';", paired.right?.text)
        assertEq(listOf(DiffRange(14, 17)), paired.left!!.emphasis)
        assertEq(listOf(DiffRange(14, 17)), paired.right!!.emphasis)
        val lone = rows.first { it.right?.text == "final extra = 1;" }
        assertEq(null, lone.left)
        assertEq(2, rows.count { it.hunk != null })
    }

    fun testLeavesWholesaleRewritesUnmarked() {
        assertEq(null, intraLineChanges("alpha beta", "gamma delta"))
        assertEq(null, intraLineChanges("same", "same"))
    }

    fun testQuotedPathsAreUnescaped() {
        val files = parseUnifiedDiff(
            "diff --git \"a/caf\\303\\251.txt\" \"b/caf\\303\\251.txt\"\n" +
                "--- \"a/caf\\303\\251.txt\"\n+++ \"b/caf\\303\\251.txt\"\n@@ -1 +1 @@\n-a\n+b\n",
        )
        assertEq("café.txt", files.single().path)
    }

    fun testParsesAFormatPatchSeriesIntoCommits() {
        val series = listOf(
            "From 0123456789abcdef0123456789abcdef01234567 Mon Sep 17 00:00:00 2001",
            "From: =?UTF-8?q?Ren=C3=A9e=20Agent?= <agent@example.com>",
            "Date: Thu, 18 Sep 2026 10:04:05 +0200",
            "Subject: [PATCH 1/2] Add the thing",
            " across two lines",
            "",
            "Longer explanation.",
            "---",
            " a.txt | 1 +",
            " 1 file changed, 1 insertion(+)",
            "",
            "diff --git a/a.txt b/a.txt",
            "new file mode 100644",
            "index 0000000..1111111",
            "--- /dev/null",
            "+++ b/a.txt",
            "@@ -0,0 +1 @@",
            "+hello",
            "",
            "From 89abcdef0123456789abcdef0123456789abcdef Mon Sep 17 00:00:00 2001",
            "From: Bot <bot@example.com>",
            "Date: Thu, 18 Sep 2026 11:00:00 +0000",
            "Subject: [PATCH 2/2] Tweak",
            "",
            "---",
            "diff --git a/a.txt b/a.txt",
            "--- a/a.txt",
            "+++ b/a.txt",
            "@@ -1 +1 @@",
            "-hello",
            "+hello world",
            "",
        ).joinToString("\n", postfix = "\n")
        val commits = parseFormatPatchSeries(series)
        assertEq(2, commits.size)
        val first = commits.first()
        assertEq("0123456", first.shortHash)
        assertEq("Add the thing across two lines", first.subject)
        assertEq("Renée Agent", first.author)
        assertEq("agent@example.com", first.email)
        assertEq(ZonedDateTime.of(2026, 9, 18, 8, 4, 5, 0, ZoneOffset.UTC).toInstant().toEpochMilli(), first.date)
        assertEq("Longer explanation.", first.body)
        val files = parseUnifiedDiff(first.patch)
        assertEq(DiffFileStatus.Added, files.single().status)
        assertEq(1, files.single().additions)
        assertEq(1, parseUnifiedDiff(commits[1].patch).single().deletions)
        assertEq("Tweak", commits[1].subject)
    }
}
