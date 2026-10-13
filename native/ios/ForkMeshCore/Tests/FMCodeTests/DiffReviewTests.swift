import FMBase
import FMDiff
import XCTest
@testable import FMCode

private let reviewPatch = """
    diff --git a/src/app.py b/src/app.py
    --- a/src/app.py
    +++ b/src/app.py
    @@ -1,2 +1,4 @@
     import os
    -x = 1
    +x = 2  # TODO tighten
    +requests.get(url, verify=False)
    @@ -20,1 +22,2 @@
     def main():
    +    print("http://api.internal.corp/v1")
    diff --git a/tests/test_app.py b/tests/test_app.py
    --- a/tests/test_app.py
    +++ b/tests/test_app.py
    @@ -1 +1,2 @@
     def test_x():
    +    assert True

    """

private let flaggedPatch = """
    diff --git a/src/run.py b/src/run.py
    --- a/src/run.py
    +++ b/src/run.py
    @@ -1,2 +1,2 @@
     import os
    -run(cmd)
    +eval(cmd)  # TODO drop

    """

final class DiffHealthTests: XCTestCase {
    @MainActor func testAQuietDiffIsGreenButMergeWaitsForGit() async {
        let files = UnifiedDiff.parse("""
            diff --git a/README.md b/README.md
            --- a/README.md
            +++ b/README.md
            @@ -1 +1 @@
            -Inbox
            +Signed inbox

            """)
        let report = DiffHealth.report(files)
        XCTAssertEqual(report.map(\.key), ["merge", "complexity", "quality", "security", "network", "practices"])
        XCTAssertEqual(report.first { $0.key == "quality" }?.score, 100)
        XCTAssertEqual(report.first { $0.key == "merge" }?.score, 84)
        XCTAssertEqual(report.first { $0.key == "merge" }?.tone, .orange)
        XCTAssertEqual(
            report.first { $0.key == "quality" }?.detail,
            "no source-code files changed. Tests are inferred from paths and were not executed.")
        XCTAssertEqual(DiffHealth.report(files, git: .merged).first?.score, 100)
        XCTAssertEqual(report.first { $0.key == "network" }?.pill, "Network 100")
    }

    @MainActor func testRiskyLinesLowerTheirScores() async {
        let files = UnifiedDiff.parse(reviewPatch)
        let report = Dictionary(uniqueKeysWithValues: DiffHealth.report(files).map { ($0.key, $0) })
        XCTAssertEqual(report["security"]?.score, 65)
        XCTAssertTrue(report["security"]?.detail.hasPrefix("1 TLS/certificate bypasses.") == true)
        XCTAssertEqual(report["quality"]?.score, 95)
        XCTAssertTrue(report["quality"]?.detail.hasPrefix("1 test files changed; 1 new TODO/FIXME markers.") == true)
        XCTAssertEqual(report["network"]?.score, 100 - 4 - 18 - 2 - 40)
        XCTAssertEqual(report["practices"]?.score, 97)
        XCTAssertEqual(report["merge"]?.score, 55)
    }

    @MainActor func testSecretsSkipPlaceholdersAndTheIgnoreMarker() async {
        XCTAssertEqual(DiffHealth.secretMatches("token = ghp_" + String(repeating: "a", count: 36)), ["GitHub token"])
        XCTAssertEqual(DiffHealth.secretMatches("password = \"correct-horse-battery-staple-xyz\""), [])
        XCTAssertEqual(DiffHealth.secretMatches("api_key: \"Zq8vN2mL5pR7tW1xY4bC6d\""), ["Secret/token assignment"])
        XCTAssertEqual(DiffHealth.secretMatches("api_key: \"Zq8vN2mL5pR7tW1xY4bC6d\" # forkmesh-secret-scan:ignore-line"), [])
        XCTAssertEqual(DiffHealth.flags(path: "a.swift", text: "<<<<<<< HEAD"), [.conflictMarker])
        XCTAssertEqual(DiffHealth.flags(path: "a.swift", text: "let x = 1 "), [.trailingWhitespace])
        XCTAssertEqual(DiffHealth.flags(path: "a.md", text: "let x = 1 "), [])
        XCTAssertEqual(DiffHealth.flags(path: "a.js", text: "console.log(x) // FIXME"), [.todo, .debug])
        XCTAssertEqual(DiffHealth.urls("see http://localhost:8080 and http://example.com and http://a.b").insecure, 1)
        XCTAssertTrue(DiffHealth.isTest("pkg/foo_test.go"))
        XCTAssertTrue(DiffHealth.isGenerated("web/dist/app.js"))
        XCTAssertEqual(DiffHealthTone(score: 85), .green)
        XCTAssertEqual(DiffHealthTone(score: 59), .red)
    }
}

final class DiffReviewTests: XCTestCase {
    @MainActor func testChangesFindAndAutoMark() async throws {
        let files = UnifiedDiff.parse(reviewPatch)
        let review = DiffReviewModel(files: files, scope: "o/r/branch/x", store: InMemoryKeyValueStore())
        let first = try XCTUnwrap(review.nextChange(from: nil))
        XCTAssertEqual(review.rows[try XCTUnwrap(review.position(of: first))], .hunk(0, 0))
        let second = try XCTUnwrap(review.nextChange(from: first))
        XCTAssertEqual(review.rows[try XCTUnwrap(review.position(of: second))], .hunk(0, 1))
        XCTAssertEqual(review.previousChange(from: second), first)
        XCTAssertNil(review.previousChange(from: first))
        XCTAssertEqual(review.fileAt(rowId: DiffReviewRow.endId(1)), 1)

        review.query = "TODO"
        XCTAssertEqual(review.matches.count, 1)
        XCTAssertEqual(review.findLabel, "1/1")
        review.query = "zzz"
        XCTAssertEqual(review.findLabel, "No results")
        review.query = "x"
        XCTAssertGreaterThan(review.matches.count, 2)
        let rowOfFirst = review.matches[0].row
        XCTAssertFalse(review.matches(in: rowOfFirst, right: true).isEmpty)
        XCTAssertEqual(review.stepMatch(false), review.matches.last?.row)

        review.endAppeared(0)
        review.endAppeared(1)
        review.endDisappeared(1)
        review.settleScroll()
        XCTAssertTrue(review.isViewed(0))
        XCTAssertFalse(review.isFolded(0))
        XCTAssertFalse(review.isViewed(1))
        XCTAssertEqual(review.nextUnviewed(after: 0), 1)
        XCTAssertEqual(review.flags(0, try XCTUnwrap(files[0].hunks[0].lines.last)), [.tlsBypass, .request])
    }

    @MainActor func testScrollingPastAFilesEndMarksIt() async throws {
        let review = DiffReviewModel(files: UnifiedDiff.parse(reviewPatch), scope: "o/r/pull/4")
        let firstLine = try XCTUnwrap(review.rows.first { if case .line(0, _) = $0 { return true } else { return false } }).id
        let secondLine = try XCTUnwrap(review.rows.first { if case .line(1, _) = $0 { return true } else { return false } }).id
        XCTAssertEqual(review.autoMarkPassed(from: secondLine, to: firstLine), [])
        XCTAssertEqual(review.autoMarkPassed(from: nil, to: secondLine), [])
        XCTAssertEqual(review.autoMarkPassed(from: firstLine, to: DiffReviewRow.headerId(1)), [0])
        XCTAssertTrue(review.isViewed(0))
        XCTAssertFalse(review.isFolded(0))
        XCTAssertFalse(review.isViewed(1))
        XCTAssertEqual(review.autoMarkPassed(from: -1, to: secondLine), [])
        XCTAssertEqual(review.autoMarkPassed(from: secondLine, to: DiffReviewRow.endId(1)), [1])
        review.endAppeared(1)
        XCTAssertEqual(review.settleScroll(), [])
    }

    @MainActor func testPagesAndFilesTurnByVoice() async throws {
        let review = DiffReviewModel(files: UnifiedDiff.parse(reviewPatch), scope: "o/r/pull/9")
        let rows = review.rows.map(\.id)
        let firstLine = try XCTUnwrap(review.rows.first { if case .line(0, _) = $0 { return true } else { return false } }).id
        let secondLine = try XCTUnwrap(review.rows.first { if case .line(1, _) = $0 { return true } else { return false } }).id

        XCTAssertEqual(review.pageDown(top: nil, bottom: nil), rows[0])
        XCTAssertEqual(review.pageDown(top: DiffReviewRow.headerId(0), bottom: secondLine), secondLine)
        XCTAssertEqual(review.pageDown(top: firstLine, bottom: firstLine), rows[try XCTUnwrap(review.position(of: firstLine)) + 1])
        XCTAssertEqual(review.pageDown(top: rows[rows.count - 3], bottom: nil), rows[rows.count - 2])
        XCTAssertNil(review.pageDown(top: secondLine, bottom: DiffReviewRow.endId(1)))
        XCTAssertNil(review.pageDown(top: rows[rows.count - 2], bottom: nil))

        XCTAssertEqual(review.pageUp(top: secondLine), secondLine)
        XCTAssertNil(review.pageUp(top: nil))
        XCTAssertNil(review.pageUp(top: -1))

        XCTAssertEqual(review.nextFile(from: nil), 0)
        XCTAssertEqual(review.nextFile(from: -1), 0)
        XCTAssertEqual(review.nextFile(from: firstLine), 1)
        XCTAssertEqual(review.nextFile(from: DiffReviewRow.endId(0)), 1)
        XCTAssertNil(review.nextFile(from: secondLine))
        XCTAssertEqual(review.previousFile(from: secondLine), 0)
        XCTAssertEqual(review.previousFile(from: DiffReviewRow.headerId(1)), 0)
        XCTAssertNil(review.previousFile(from: firstLine))
        XCTAssertNil(review.previousFile(from: nil))
        XCTAssertEqual(DiffReviewText.nextFile(review.files[1].file), "Next file, test_app.py.")
    }

    @MainActor func testScrollCountsCodeLinesTheChangedOnesAndTheFlaggedOnes() async throws {
        let review = DiffReviewModel(files: UnifiedDiff.parse(reviewPatch), scope: "o/r/pull/5")
        let lines = review.rows.compactMap { (row: DiffReviewRow) -> (Int, DiffLine)? in
            if case let .line(0, line) = row { return (row.id, line) } else { return nil }
        }
        XCTAssertEqual(lines.count, 6)
        let whole = review.scrollPass(from: DiffReviewRow.headerId(0), to: DiffReviewRow.headerId(1))
        XCTAssertEqual(whole.lines, 8)
        XCTAssertEqual(whole.changed, lines.filter { $0.1.kind != .context }.count)
        XCTAssertEqual(review.scrollPass(from: DiffReviewRow.headerId(1), to: DiffReviewRow.headerId(0)), whole)
        XCTAssertEqual(review.scrollPass(from: lines[0].0, to: lines[3].0).lines, 3)
        XCTAssertEqual(review.scrollPass(from: lines[3].0, to: lines[3].0), DiffScrollPass())
        XCTAssertEqual(review.scrollPass(from: nil, to: lines[3].0), DiffScrollPass())
        let crossed = review.scrollPass(crossing: [lines[1].0, lines[0].0, DiffReviewRow.headerId(0), DiffReviewRow.endId(0)])
        XCTAssertEqual(crossed.lines, 2)
        XCTAssertEqual(crossed.changed, 1)
        review.toggleViewed(0)
        XCTAssertTrue(review.isFolded(0))
        XCTAssertEqual(review.scrollPass(from: DiffReviewRow.headerId(0), to: DiffReviewRow.headerId(1)), DiffScrollPass())

        let flagged = DiffReviewModel(files: UnifiedDiff.parse(flaggedPatch), scope: "o/r/pull/6")
        let pass = flagged.scrollPass(from: DiffReviewRow.headerId(0), to: DiffReviewRow.endId(0))
        XCTAssertEqual(pass, DiffScrollPass(lines: 4, changed: 2, flagged: 1))
        let added = try XCTUnwrap(flagged.rows.compactMap { (row: DiffReviewRow) -> DiffLine? in
            if case let .line(_, line) = row, line.kind == .added, line.text.contains("eval") { return line } else { return nil }
        }.first)
        XCTAssertEqual(flagged.marks(0, added), [DiffLineMark(metric: "security", severe: true), DiffLineMark(metric: "quality", severe: false)])
    }

    func testLineMarksFollowTheHealthPills() {
        XCTAssertEqual(DiffLineMark.of([]), [])
        XCTAssertEqual(DiffLineMark.of([.todo, .debug]), [DiffLineMark(metric: "quality", severe: false)])
        XCTAssertEqual(
            DiffLineMark.of([.request, .deepNesting, .insecureUrl]),
            [DiffLineMark(metric: "network", severe: true), DiffLineMark(metric: "complexity", severe: false)])
        for flag in DiffLineFlag.allCases {
            XCTAssertTrue(DiffLineMark.metricOrder.contains(flag.metric), flag.rawValue)
            XCTAssertNotEqual(flag.metric, "merge")
        }
    }

    @MainActor func testAChangedFileLosesItsMarkAndSectionsKeepThemApart() async {
        let store = InMemoryKeyValueStore()
        let before = UnifiedDiff.parse(reviewPatch)
        let review = DiffReviewModel(files: before, scope: "o/r/pull/3", store: store)
        review.toggleViewed(0)
        review.toggleViewed(1)
        let edited = UnifiedDiff.parse(reviewPatch.replacingOccurrences(of: "assert True", with: "assert False"))
        let again = DiffReviewModel(files: edited, scope: "o/r/pull/3", store: store)
        XCTAssertTrue(again.isViewed(0))
        XCTAssertFalse(again.isViewed(1))

        let agent = DiffReviewModel(
            sections: [DiffReviewSection(title: "Committed", files: before), DiffReviewSection(title: "Empty", files: []),
                       DiffReviewSection(title: "Uncommitted", files: [before[0]])],
            scope: "agent/7")
        XCTAssertEqual(agent.sections.count, 2)
        XCTAssertEqual(agent.files.map(\.key), ["0:src/app.py", "0:tests/test_app.py", "1:src/app.py"])
        XCTAssertEqual(agent.rows.first, .section(0))
        XCTAssertTrue(agent.rows.contains(.section(1)))
        agent.toggleViewed(2)
        XCTAssertFalse(agent.isViewed(0))
        XCTAssertEqual(DiffReviewModel.patchScope("abc").count, "patch/".count + 16)
    }
}
