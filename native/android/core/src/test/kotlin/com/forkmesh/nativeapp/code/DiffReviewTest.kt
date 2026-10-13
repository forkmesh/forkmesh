package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.code.diff.parseUnifiedDiff
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DiffReviewTest {
    private val patch = "diff --git a/src/app.py b/src/app.py\n--- a/src/app.py\n+++ b/src/app.py\n" +
        "@@ -1,2 +1,4 @@\n import os\n-x = 1\n+x = 2  # TODO tighten\n+requests.get(url, verify=False)\n" +
        "@@ -20,1 +22,2 @@\n def main():\n+    print(\"http://api.internal.corp/v1\")\n" +
        "diff --git a/tests/test_app.py b/tests/test_app.py\n--- a/tests/test_app.py\n+++ b/tests/test_app.py\n" +
        "@@ -1 +1,2 @@\n def test_x():\n+    assert True\n"

    fun testAQuietDiffIsGreenButMergeWaitsForGit() {
        val files = parseUnifiedDiff("diff --git a/README.md b/README.md\n--- a/README.md\n+++ b/README.md\n@@ -1 +1 @@\n-Inbox\n+Signed inbox\n")
        val report = DiffHealth.report(files)
        assertEq(listOf("merge", "complexity", "quality", "security", "network", "practices"), report.map { it.key })
        assertEq(100, report.first { it.key == "quality" }.score)
        assertEq(84, report.first().score)
        assertEq(DiffHealthTone.Orange, report.first().tone)
        assertEq("no source-code files changed. Tests are inferred from paths and were not executed.", report.first { it.key == "quality" }.detail)
        assertEq(100, DiffHealth.report(files, DiffHealthGit.Merged).first().score)
        assertEq("Network 100", report.first { it.key == "network" }.pill)
    }

    fun testRiskyLinesLowerTheirScores() {
        val report = DiffHealth.report(parseUnifiedDiff(patch)).associateBy { it.key }
        assertEq(65, report.getValue("security").score)
        expect(report.getValue("security").detail.startsWith("1 TLS/certificate bypasses."))
        assertEq(95, report.getValue("quality").score)
        expect(report.getValue("quality").detail.startsWith("1 test files changed; 1 new TODO/FIXME markers."))
        assertEq(36, report.getValue("network").score)
        assertEq(97, report.getValue("practices").score)
        assertEq(55, report.getValue("merge").score)
    }

    fun testSecretsSkipPlaceholdersAndTheIgnoreMarker() {
        assertEq(listOf("GitHub token"), DiffHealth.secretMatches("token = ghp_" + "a".repeat(36)))
        assertEq(emptyList<String>(), DiffHealth.secretMatches("password = \"correct-horse-battery-staple-xyz\""))
        assertEq(listOf("Secret/token assignment"), DiffHealth.secretMatches("api_key: \"Zq8vN2mL5pR7tW1xY4bC6d\""))
        assertEq(emptyList<String>(), DiffHealth.secretMatches("api_key: \"Zq8vN2mL5pR7tW1xY4bC6d\" # forkmesh-secret-scan:ignore-line"))
        assertEq(listOf(DiffLineFlag.ConflictMarker), DiffHealth.flags("a.swift", "<<<<<<< HEAD"))
        assertEq(listOf(DiffLineFlag.TrailingWhitespace), DiffHealth.flags("a.swift", "let x = 1 "))
        assertEq(emptyList<DiffLineFlag>(), DiffHealth.flags("a.md", "let x = 1 "))
        assertEq(listOf(DiffLineFlag.Todo, DiffLineFlag.Debug), DiffHealth.flags("a.js", "console.log(x) // FIXME"))
        assertEq(1, DiffHealth.urls("see http://localhost:8080 and http://example.com and http://a.b").second)
        expect(DiffHealth.isTest("pkg/foo_test.go"))
        expect(DiffHealth.isGenerated("web/dist/app.js"))
        assertEq(DiffHealthTone.Green, DiffHealthTone.of(85))
        assertEq(DiffHealthTone.Red, DiffHealthTone.of(59))
    }

    fun testStreamFoldsViewedFilesAndRemembersThem() {
        val files = parseUnifiedDiff(patch)
        val store = MemoryPrefStore()
        val review = DiffReviewModel.of(files, "o/r/branch/x", store = store)
        var changes = 0
        review.onChange = { changes++ }
        assertEq("0 of 2 files viewed", review.viewedLabel)
        assertEq("1 modified, 1 added", DiffReviewText.kinds(listOf(files[0], parseUnifiedDiff("diff --git a/n.txt b/n.txt\nnew file mode 100644\n--- /dev/null\n+++ b/n.txt\n@@ -0,0 +1 @@\n+hi\n")[0])))
        assertEq(DiffReviewRow.Header(0), review.rows.first())
        assertEq(DiffReviewRow.End(1), review.rows.last())
        assertEq(1, review.toggleViewed(0))
        expect(review.isFolded(0))
        assertEq(DiffReviewRow.Header(1), review.rows[1])
        review.toggleFold(0)
        expect(!review.isFolded(0) && review.isViewed(0))
        assertEq(null, review.toggleViewed(1))
        assertEq("every file reviewed", review.remainingLabel)
        review.toggleViewed(1)
        expect(changes >= 4)

        val reopened = DiffReviewModel.of(files, "o/r/branch/x", store = store)
        expect(reopened.isViewed(0) && reopened.isFolded(0))
        expect(!reopened.isViewed(1))
        val edited = parseUnifiedDiff(patch.replace("assert True", "assert False"))
        DiffReviewModel.of(files, "o/r/branch/x", store = store).toggleViewed(1)
        val again = DiffReviewModel.of(edited, "o/r/branch/x", store = store)
        expect(again.isViewed(0))
        expect(!again.isViewed(1))

        review.split = true
        expect(review.rows.any { it is DiffReviewRow.Pair && it.row.left?.text == "x = 1" && it.row.right?.text == "x = 2  # TODO tighten" })
    }

    fun testChangesFindAndAutoMark() {
        val files = parseUnifiedDiff(patch)
        val review = DiffReviewModel.of(files, "o/r/branch/y", store = MemoryPrefStore())
        val first = review.nextChange(-1)!!
        expect(review.rows[first] is DiffReviewRow.Hunk)
        val second = review.nextChange(first)!!
        assertEq(1, (review.rows[second] as DiffReviewRow.Hunk).index)
        assertEq(first, review.previousChange(second))
        assertEq(null, review.previousChange(first))
        assertEq(1, review.fileAtPosition(review.position(DiffReviewRow.endId(1))!!))

        review.query = "TODO"
        assertEq(1, review.matches.size)
        assertEq("1/1", review.findLabel)
        review.query = "zzz"
        assertEq("No results", review.findLabel)
        review.query = "x"
        expect(review.matches.size > 2)
        expect(review.matches(review.matches[0].row, true).isNotEmpty())
        assertEq(review.matches.last().row, review.stepMatch(false))

        review.endAppeared(0)
        review.endAppeared(1)
        review.endDisappeared(1)
        review.settleScroll()
        expect(review.isViewed(0) && !review.isFolded(0))
        expect(!review.isViewed(1))
        assertEq(1, review.nextUnviewed(0))
        val risky = files[0].hunks[0].lines.last()
        assertEq(listOf(DiffLineFlag.TlsBypass, DiffLineFlag.Request), review.flags(0, risky))
    }

    fun testScrollingPastAFilesEndMarksIt() {
        val review = DiffReviewModel.of(parseUnifiedDiff(patch), "o/r/pull/4", store = MemoryPrefStore())
        val firstLine = review.rows.indexOfFirst { it is DiffReviewRow.Line && it.file == 0 }
        val secondLine = review.rows.indexOfFirst { it is DiffReviewRow.Line && it.file == 1 }
        val secondHeader = review.position(DiffReviewRow.headerId(1))!!
        assertEq(emptyList<Int>(), review.autoMarkPassed(secondLine, firstLine))
        assertEq(listOf(0), review.autoMarkPassed(firstLine, secondHeader))
        expect(review.isViewed(0) && !review.isFolded(0))
        expect(!review.isViewed(1))
        assertEq(emptyList<Int>(), review.autoMarkPassed(-1, secondLine))
        assertEq(listOf(1), review.autoMarkPassed(secondLine, review.position(DiffReviewRow.endId(1))!!))
    }

    fun testScrollCountsCodeLinesTheChangedOnesAndTheFlaggedOnes() {
        val review = DiffReviewModel.of(parseUnifiedDiff(patch), "o/r/pull/5", store = MemoryPrefStore())
        val lines = review.rows.indices.filter { review.rows[it] is DiffReviewRow.Line && review.rows[it].file == 0 }
        val top = review.position(DiffReviewRow.headerId(0))!!
        val next = review.position(DiffReviewRow.headerId(1))!!
        assertEq(6, lines.size)
        val whole = review.scrollPass(top, next)
        assertEq(8, whole.lines)
        assertEq(4, whole.changed)
        assertEq(whole, review.scrollPass(next, top))
        assertEq(3, review.scrollPass(lines[0], lines[3]).lines)
        assertEq(DiffScrollPass(), review.scrollPass(lines[3], lines[3]))
        review.toggleViewed(0)
        expect(review.isFolded(0))
        assertEq(DiffScrollPass(), review.scrollPass(review.position(DiffReviewRow.headerId(0))!!, review.position(DiffReviewRow.headerId(1))!!))

        val flagged = DiffReviewModel.of(
            parseUnifiedDiff("diff --git a/src/run.py b/src/run.py\n--- a/src/run.py\n+++ b/src/run.py\n@@ -1,2 +1,2 @@\n import os\n-run(cmd)\n+eval(cmd)  # TODO drop\n"),
            "o/r/pull/6",
            store = MemoryPrefStore(),
        )
        assertEq(DiffScrollPass(4, 2, 1), flagged.scrollPass(flagged.position(DiffReviewRow.headerId(0))!!, flagged.position(DiffReviewRow.endId(0))!!))
        val added = flagged.rows.filterIsInstance<DiffReviewRow.Line>().first { it.line.text.contains("eval") }.line
        assertEq(listOf(DiffLineMark("security", true), DiffLineMark("quality", false)), flagged.marks(0, added))
    }

    fun testLineMarksFollowTheHealthPills() {
        assertEq(emptyList<DiffLineMark>(), DiffLineMark.of(emptyList()))
        assertEq(listOf(DiffLineMark("quality", false)), DiffLineMark.of(listOf(DiffLineFlag.Todo, DiffLineFlag.Debug)))
        assertEq(
            listOf(DiffLineMark("network", true), DiffLineMark("complexity", false)),
            DiffLineMark.of(listOf(DiffLineFlag.Request, DiffLineFlag.DeepNesting, DiffLineFlag.InsecureUrl)),
        )
        for (flag in DiffLineFlag.entries) {
            expect(flag.metric in DiffLineMark.metricOrder && flag.metric != "merge")
        }
    }

    fun testSectionsKeepTheirMarksApart() {
        val before = parseUnifiedDiff(patch)
        val agent = DiffReviewModel(
            listOf(DiffReviewSection("Committed", before), DiffReviewSection("Empty", emptyList()), DiffReviewSection("Uncommitted", listOf(before[0]))),
            "agent/7",
        )
        assertEq(2, agent.sections.size)
        assertEq(listOf("0:src/app.py", "0:tests/test_app.py", "1:src/app.py"), agent.files.map { it.key })
        assertEq(DiffReviewRow.Section(0), agent.rows.first())
        expect(DiffReviewRow.Section(1) in agent.rows)
        agent.toggleViewed(2)
        expect(!agent.isViewed(0))
        assertEq("patch/".length + 16, DiffReviewModel.patchScope("abc").length)
    }
}
