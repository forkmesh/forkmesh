import FMApi
import FMBase
import FMDiff
import XCTest

@testable import FMCode

final class ReviewAnchoringTests: XCTestCase {
    let headA = String(repeating: "a", count: 40)
    let headB = String(repeating: "b", count: 40)
    let patchA = """
        diff --git a/src/a.cpp b/src/a.cpp
        index 000..111 100644
        --- a/src/a.cpp
        +++ b/src/a.cpp
        @@ -1,4 +1,5 @@
         int a;
        -int old;
        +int fresh;
        +int extra;
         int b;
         int c;

        """
    let patchB = """
        diff --git a/src/a.cpp b/src/a.cpp
        index 000..222 100644
        --- a/src/a.cpp
        +++ b/src/a.cpp
        @@ -1,4 +1,6 @@
        +#include <x>
        +
         int a;
        -int old;
        +int fresh;
         int b;
         int c;

        """

    func thread(_ id: String, _ start: Int, _ end: Int, _ anchor: String, patchset: String? = nil) -> ReviewEvent {
        ReviewEvent(
            type: "thread-comment", id: id, author: "rev", path: "src/a.cpp", side: "new", lineStart: start, lineEnd: end,
            threadId: id, patchset: patchset ?? headA, anchorLines: anchor, body: "note")
    }

    var events: [ReviewEvent] {
        [
            thread("t-stays", 2, 2, "int fresh;"),
            thread("t-gone", 3, 3, "int extra;"),
            thread("t-range", 4, 5, "int b;\nint c;"),
            ReviewEvent(type: "line-comment", path: "src/a.cpp", side: "new", line: 5, body: "no anchor"),
        ]
    }

    func testAnchorLinesReadEachSideWithoutPrefixes() {
        XCTAssertEqual(ReviewAnchoring.anchorLines(patch: patchA, path: "src/a.cpp", side: .new, lineStart: 2, lineEnd: 3), "int fresh;\nint extra;")
        XCTAssertEqual(ReviewAnchoring.anchorLines(patch: patchA, path: "src/a.cpp", side: .old, lineStart: 2, lineEnd: 2), "int old;")
        XCTAssertEqual(ReviewAnchoring.anchorLines(patch: patchA, path: "src/a.cpp", side: .new, lineStart: 40, lineEnd: 41), "")
        XCTAssertEqual(ReviewAnchoring.anchorLines(patch: patchA, path: "src/b.cpp", side: .new, lineStart: 1, lineEnd: 1), "")
    }

    func testAnchorLinesStopAtHunkGapsAndTheLineCap() {
        var lines = ["diff --git a/f b/f", "--- a/f", "+++ b/f", "@@ -1,0 +1,20 @@"]
        for n in 1...20 { lines.append("+line \(n)") }
        lines += ["@@ -40,1 +40,1 @@", " far"]
        let patch = lines.joined(separator: "\n")
        let capped = ReviewAnchoring.anchorLines(patch: patch, path: "f", side: .new, lineStart: 1, lineEnd: 20)
        XCTAssertEqual(capped.split(separator: "\n").count, reviewAnchorMaxLines)
        XCTAssertEqual(ReviewAnchoring.anchorLines(patch: patch, path: "f", side: .new, lineStart: 19, lineEnd: 40), "line 19\nline 20")
    }

    func testThreadsOnTheirOwnPatchsetAreCurrent() {
        let snap = ReviewSnapshotBuilder.build(events: events, patch: patchA, currentPatchset: headA)
        XCTAssertEqual(snap.threads.count, 4)
        for t in snap.threads {
            if t.opening?.type == "line-comment" {
                XCTAssertEqual(t.placement, .legacy)
                XCTAssertEqual(t.currentLineStart, 5)
            } else {
                XCTAssertEqual(t.placement, .current)
                XCTAssertEqual(t.currentLineStart, t.lineStart)
                XCTAssertEqual(t.currentLineEnd, t.lineEnd)
            }
        }
        XCTAssertEqual(snap.outdatedThreads, 0)
    }

    func testThreadsFollowTheirTextOrGoOutdatedAfterARepush() {
        var all = events
        all.append(ReviewEvent(type: "line-comment", path: "src/a.cpp", side: "new", line: 9, body: "gone"))
        let snap = ReviewSnapshotBuilder.build(events: all, patch: patchB, currentPatchset: headB)
        let stays = snap.thread("t-stays")!
        XCTAssertEqual(stays.placement, .moved)
        XCTAssertEqual(stays.currentLineStart, 4)
        XCTAssertEqual(stays.currentLineEnd, 4)
        XCTAssertEqual(stays.placementLabel, "Moved from line 2")
        let gone = snap.thread("t-gone")!
        XCTAssertEqual(gone.placement, .outdated)
        XCTAssertEqual(gone.currentLineStart, 0)
        XCTAssertEqual(gone.placementLabel, "Outdated")
        let range = snap.thread("t-range")!
        XCTAssertEqual(range.placement, .moved)
        XCTAssertEqual(range.currentLineStart, 5)
        XCTAssertEqual(range.currentLineEnd, 6)
        XCTAssertEqual(range.placementLabel, "Moved from lines 4–5")
        let legacy = snap.threads.filter { $0.opening?.type == "line-comment" }
        XCTAssertEqual(legacy.filter { $0.placement == .legacy && $0.currentLineStart == $0.lineStart }.count, 1)
        XCTAssertEqual(legacy.filter { $0.placement == .outdated }.count, 1)
        XCTAssertEqual(snap.outdatedThreads, 2)
        XCTAssertEqual(snap.files["src/a.cpp"]?.outdatedThreads, 2)
        XCTAssertEqual(snap.files["src/a.cpp"]?.totalThreads, 5)
    }

    func testMatchingPatchsetSkipsTextSearch() {
        let snap = ReviewSnapshotBuilder.build(events: events, patch: patchB, currentPatchset: headA.uppercased())
        for t in snap.threads where t.opening?.type == "thread-comment" { XCTAssertEqual(t.placement, .current) }
    }

    func testPatchsetWithoutTextIsOutdatedUnlessThePatchsetIsUnnamed() {
        let blind = thread("t-blind", 2, 2, "")
        XCTAssertTrue(ReviewSnapshotBuilder.build(events: [blind], patch: patchB, currentPatchset: headB).threads[0].isOutdated)
        XCTAssertEqual(ReviewSnapshotBuilder.build(events: [blind], patch: patchB, currentPatchset: "").threads[0].placement, .current)
    }

    func testRepliesStateAndSuggestionsFoldIntoTheirThread() {
        var all = events
        all.append(ReviewEvent(type: "thread-reply", id: "r1", threadId: "t-stays", parentId: "t-stays", body: "ok"))
        all.append(ReviewEvent(type: "thread-state", id: "s1", state: "resolved", threadId: "t-stays"))
        all.append(ReviewEvent(type: "comment", body: "top"))
        all.append(ReviewEvent(type: "review", author: "rev", state: "approve"))
        var suggestion = thread("t-sugg", 2, 3, "int fresh;\nint extra;")
        suggestion.suggestionPatch = "@@ -2 +2 @@\n-int fresh;\n+int fresher;\n"
        all.append(suggestion)
        all.append(ReviewEvent(type: "suggestion-state", state: "applied", threadId: "t-sugg", appliedCommit: "abc"))
        let snap = ReviewSnapshotBuilder.build(events: all, patch: patchA, currentPatchset: headA)
        let stays = snap.thread("t-stays")!
        XCTAssertTrue(stays.resolved)
        XCTAssertEqual(stays.events.map(\.type), ["thread-comment", "thread-reply", "thread-state"])
        XCTAssertEqual(stays.lastEventId, "s1")
        XCTAssertEqual(snap.topLevelItems, 2)
        XCTAssertEqual(snap.resolvedThreads, 1)
        XCTAssertEqual(snap.suggestions, 1)
        XCTAssertEqual(snap.thread("t-sugg")?.suggestionState, "applied")
        XCTAssertEqual(snap.thread("t-sugg")?.appliedCommit, "abc")
        XCTAssertEqual(snap.reviewSummary, "approved")
    }

    func testLegacyLineCommentIdsMatchTheDesktop() {
        let snap = ReviewSnapshotBuilder.build(events: events, patch: patchA, currentPatchset: headA)
        XCTAssertEqual(snap.threads.last?.id, "legacy:src/a.cpp:new:5:3")
    }

    func testThreadsGroupByPaintedLineWithOutdatedApart() {
        let snap = ReviewSnapshotBuilder.build(events: events, patch: patchB, currentPatchset: headB)
        let grouped = snap.threads(inFile: "src/a.cpp")
        XCTAssertEqual(grouped.byLine[.new]?[4]?.map(\.id), ["t-stays"])
        XCTAssertEqual(grouped.byLine[.new]?[6]?.map(\.id), ["t-range"])
        XCTAssertEqual(grouped.outdated.map(\.id), ["t-gone"])
    }

    func testNearestMatchPrefersTheClosestOccurrence() {
        let side = DiffSideIndex(lines: [1, 2, 3, 10, 11, 20], texts: ["x", "y", "z", "x", "y", "x"])
        XCTAssertEqual(ReviewAnchoring.nearestMatch(side, needle: "x\ny", preferred: 9), 10)
        XCTAssertEqual(ReviewAnchoring.nearestMatch(side, needle: "x\ny", preferred: 2), 1)
        XCTAssertEqual(ReviewAnchoring.nearestMatch(side, needle: "y\nz\nx", preferred: 2), -1)
        XCTAssertEqual(ReviewAnchoring.nearestMatch(side, needle: "x", preferred: 18), 20)
    }

    func testBinaryFilesAndOddHunkHeadersAreNotIndexed() {
        let patch = "diff --git a/img.png b/img.png\nGIT binary patch\nliteral 3\n+abc\n"
        XCTAssertEqual(DiffSideIndexes(patch: patch).side("img.png", .new)?.lines ?? [], [])
        XCTAssertNil(DiffSideIndexes.hunkStarts("@@ -x +1 @@"[...]))
        XCTAssertEqual(DiffSideIndexes.hunkStarts("@@ -3 +4,2 @@ func"[...])?.0, 3)
    }

    func testEmptyContextLineCountsOnBothSides() {
        let patch = "diff --git a/f b/f\n--- a/f\n+++ b/f\n@@ -1,3 +1,3 @@\n a\n\n-b\n+c"
        let index = DiffSideIndexes(patch: patch)
        XCTAssertEqual(index.side("f", .new)?.lines, [1, 2, 3])
        XCTAssertEqual(index.side("f", .old)?.texts, ["a", "", "b"])
    }
}

final class ReviewEventParsingTests: XCTestCase {
    func testEventFileFrontMatterRoundTripsBlobs() {
        let anchor = "int x = 1;\nreturn x;"
        let md = """
            ---
            type: thread-comment
            id: t-1
            author: KEY
            authorName: mona
            ts: 1700
            threadId: t-1
            path: src/x.cpp
            side: old
            lineStart: 4
            lineEnd: 5
            suggestionPatchB64: \(ReviewEvent.encodeBlob("@@ -1 +1 @@\n-a\n+b\n"))
            patchset: \(String(repeating: "c", count: 40))
            anchorLinesB64: \(ReviewEvent.encodeBlob(anchor))
            sig: SIG
            ---

            Please rename.
            """
        let ev = ReviewEvent(markdown: md)
        XCTAssertEqual(ev.type, "thread-comment")
        XCTAssertEqual(ev.displayAuthor, "mona")
        XCTAssertEqual(ev.side, "old")
        XCTAssertEqual(ev.lineStart, 4)
        XCTAssertEqual(ev.lineEnd, 5)
        XCTAssertEqual(ev.anchorLines, anchor)
        XCTAssertEqual(ev.suggestionPatch, "@@ -1 +1 @@\n-a\n+b\n")
        XCTAssertEqual(ev.body, "Please rename.")
        XCTAssertEqual(ev.ts, 1700)
        XCTAssertEqual(ReviewEvent.decodeBlob("!!"), "")
        XCTAssertFalse(ReviewEvent.encodeBlob("é?>").contains("="))
    }

    func testPullRecordReadsCreationOidsAndInlinePatch() {
        let md = """
            ---
            schema: forkmesh-pull-v1
            number: 12
            title: Add review
            base: main
            head: feature
            status: open
            derive: branch
            creationBaseOid: \(String(repeating: "1", count: 40))
            creationHeadOid: \(String(repeating: "F", count: 40))
            author: KEY
            authorName: mona
            sig: S
            ---

            Body text
            """
        let record = PullReviewRecord(number: 12, markdown: md)
        XCTAssertEqual(record.title, "Add review")
        XCTAssertTrue(record.branchBacked)
        XCTAssertEqual(record.currentPatchset, String(repeating: "f", count: 40))
        XCTAssertEqual(record.description, "Body text")
        XCTAssertTrue(record.isOpen)
        XCTAssertEqual(ReviewRecordRules.commitOid("abc"), "")
        XCTAssertEqual(ReviewRecordRules.commitOid(String(repeating: "g", count: 40)), "")
    }

    func testReviewGateCountsIndependentStandings() {
        let events = [
            ReviewEvent(type: "review", author: "author", state: "approve"),
            ReviewEvent(type: "review", author: "b", authorName: "bea", state: "request-changes"),
            ReviewEvent(type: "review", author: "c", state: "approved"),
        ]
        XCTAssertEqual(ReviewGate.summary(events), "changes_requested")
        XCTAssertEqual(ReviewGate.changesRequestedBy(events, author: "author"), ["bea"])
        XCTAssertFalse(ReviewGate.satisfied(events, author: "author"))
        let cleared = events + [ReviewEvent(type: "review", author: "b", state: "comment")]
        XCTAssertTrue(ReviewGate.satisfied(cleared, author: "author"))
        XCTAssertEqual(ReviewGate.independentApprovals(cleared, author: "author"), 1)
        XCTAssertFalse(ReviewGate.satisfied([ReviewEvent(type: "review", author: "author", state: "approve")], author: "author"))
    }
}
