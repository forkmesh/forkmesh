// The branch diff screen through BranchDiffModel, DiffViewSettings, DiffReviewModel and
// DiffFileLayout (taps become model calls), plus the diff viewer's rules.

import FMApi
import FMBase
import FMDiff
import XCTest
@testable import FMCode

let branchPatch = """
    diff --git a/lib/inbox.dart b/lib/inbox.dart
    index 1111111..2222222 100644
    --- a/lib/inbox.dart
    +++ b/lib/inbox.dart
    @@ -1,3 +1,5 @@ class Inbox {
     import 'a.dart';
    -final retries = 1;
    +final retries = 3;
    +final backoff = true;
     void main() {}
    +final veryLongLineThatGoesOnAndOn = 'lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua';
    diff --git a/README.md b/README.md
    index 3333333..4444444 100644
    --- a/README.md
    +++ b/README.md
    @@ -1 +1 @@
    -Inbox
    +Signed inbox

    """

let branchCommits = """
    From 0123456789abcdef0123456789abcdef01234567 Mon Sep 17 00:00:00 2001
    From: Agent <agent@example.com>
    Date: Thu, 18 Sep 2026 10:04:05 +0000
    Subject: [PATCH] Retry the signed inbox

    ---
    diff --git a/lib/inbox.dart b/lib/inbox.dart
    --- a/lib/inbox.dart
    +++ b/lib/inbox.dart
    @@ -1,3 +1,4 @@
     import 'a.dart';
    -final retries = 1;
    +final retries = 3;
    +final backoff = true;
     void main() {}


    """

final class CollabBranchDiffTests: XCTestCase {
    private let repo = Repository(owner: "alice", name: "mobile")

    override func setUp() async throws {
        await MainActor.run { DiffViewSettings.shared.reset() }
    }

    @MainActor private func compareAPI(fail: Bool = false) -> FakeCollabAPI {
        let api = FakeCollabAPI()
        api.comparison = RepoComparison(
            baseOid: String(repeating: "a", count: 40), headOid: String(repeating: "b", count: 40),
            mergeBaseOid: String(repeating: "c", count: 40), commitCount: 1, patch: branchPatch, commits: branchCommits)
        if fail { api.failure = ApiException("repository ref was not found") }
        return api
    }

    @MainActor private func screen(_ api: FakeCollabAPI, base: String = "") async -> BranchDiffModel {
        let model = BranchDiffModel(
            repo: repo, head: "agent/signed-inbox", base: base, title: "Stabilize signed inbox", api: api)
        await model.start()
        return model
    }

    @MainActor func testComparesTheBranchAgainstTheDefaultBranch() async throws {
        let api = compareAPI()
        let model = await screen(api)
        XCTAssertEqual(api.compareRequests, ["alice/mobile main...agent/signed-inbox"])
        let comparison = try XCTUnwrap(model.comparison)
        XCTAssertTrue(model.showsTabs)
        XCTAssertFalse(model.loading)
        XCTAssertEqual(model.title, "Stabilize signed inbox")
        XCTAssertEqual(BranchDiff.filesTab(comparison), "Files · 2")
        XCTAssertEqual(BranchDiff.commitsTab(comparison), "Commits · 1")
        XCTAssertEqual(BranchDiff.summary(commits: comparison.raw.commitCount, files: comparison.files.count), "1 commit · 2 files")
        XCTAssertEqual(comparison.files.map(DiffFileRowText.name), ["inbox.dart", "README.md"])
        XCTAssertEqual(model.reviewScope, "alice/mobile/branch/agent/signed-inbox")
        XCTAssertEqual(BranchDiff.headLine(comparison.raw), "head bbbbbbb · from ccccccc")
        XCTAssertEqual(comparison.additions, 4)
        XCTAssertEqual(comparison.deletions, 2)
        XCTAssertEqual(model.subtitle, "agent/signed-inbox → main")
    }

    @MainActor func testUsesTheBaseTheSessionNames() async {
        let api = compareAPI()
        _ = await screen(api, base: "release")
        XCTAssertEqual(api.compareRequests, ["alice/mobile release...agent/signed-inbox"])
        XCTAssertEqual(BranchDiff.base("", repo: Repository(owner: "o", name: "n", defaultBranch: " ")), "main")
        XCTAssertEqual(BranchDiff.base(" dev ", repo: repo), "dev")
    }

    @MainActor func testStreamsEveryFileSwitchesLayoutAndFoldsViewedFiles() async throws {
        let model = await screen(compareAPI())
        let files = try XCTUnwrap(model.comparison?.files)
        let store = InMemoryKeyValueStore()
        let review = DiffReviewModel(files: files, scope: model.reviewScope, store: store)
        XCTAssertEqual(review.viewedLabel, "0 of 2 files viewed")
        XCTAssertEqual(review.kindsLabel, "2 modified")
        XCTAssertEqual(review.remainingLabel, "2 left to review")
        XCTAssertEqual(review.rows.first, .header(0))
        XCTAssertEqual(review.rows.last, .end(1))
        let lineText = review.rows.compactMap { row -> String? in
            if case .line(_, let line) = row { return line.text }
            return nil
        }
        XCTAssertTrue(lineText.contains { $0.contains("backoff") })
        XCTAssertTrue(lineText.contains("Signed inbox"))
        XCTAssertEqual(review.layout(0).hunkHeader(0), "@@ -1,3 +1,5 @@ class Inbox {")

        let settings = DiffViewSettings.shared
        XCTAssertEqual(settings.layoutToggleLabel, "Side-by-side view")
        settings.split.toggle()
        review.split = settings.split
        XCTAssertEqual(settings.layoutToggleIcon, "view_agenda_outlined")
        let paired = review.rows.compactMap { row -> DiffSplitRow? in
            if case .pair(_, let pair) = row { return pair }
            return nil
        }
        XCTAssertEqual(paired.first { $0.left?.text.contains("retries = 1") == true }?.right?.text, "final retries = 3;")

        settings.setFontSize(40)
        XCTAssertEqual(settings.fontSize, 18)
        XCTAssertFalse(settings.canGrow)
        settings.setFontSize(1)
        XCTAssertEqual(settings.fontSize, 9)
        XCTAssertFalse(settings.canShrink)

        XCTAssertEqual(review.toggleViewed(0), 1)
        XCTAssertTrue(review.isFolded(0))
        XCTAssertEqual(Array(review.rows.prefix(2)), [.header(0), .header(1)])
        XCTAssertEqual(review.viewedLabel, "1 of 2 files viewed")
        review.toggleFold(0)
        XCTAssertFalse(review.isFolded(0))
        XCTAssertTrue(review.isViewed(0))
        XCTAssertNil(review.toggleViewed(1))
        XCTAssertEqual(review.remainingLabel, "every file reviewed")
        XCTAssertNil(review.toggleViewed(1))
        XCTAssertFalse(review.isViewed(1))

        let reopened = DiffReviewModel(files: files, scope: model.reviewScope, store: store)
        XCTAssertTrue(reopened.isViewed(0))
        XCTAssertTrue(reopened.isFolded(0))
        XCTAssertFalse(reopened.isViewed(1))
    }

    @MainActor func testListsCommitsAndOpensOne() async throws {
        let model = await screen(compareAPI())
        let comparison = try XCTUnwrap(model.comparison)
        let commit = try XCTUnwrap(comparison.commits.first)
        XCTAssertEqual(commit.subject, "Retry the signed inbox")
        XCTAssertEqual(commit.shortHash, "0123456")
        let files = comparison.filesOf(commit)
        XCTAssertEqual(files.map(DiffFileRowText.name), ["inbox.dart"])
        XCTAssertEqual(diffAdditions(files), 2)
        XCTAssertEqual(diffDeletions(files), 1)
        let date = try XCTUnwrap(commit.date)
        XCTAssertEqual(
            BranchDiff.commitMeta(commit, now: date.addingTimeInterval(3 * 86_400)), "0123456 · Agent · 3d ago")
    }

    @MainActor func testExplainsABranchTheMirrorDoesNotHave() async {
        let api = compareAPI(fail: true)
        let model = await screen(api)
        XCTAssertNil(model.comparison)
        XCTAssertFalse(model.showsTabs)
        XCTAssertEqual(BranchDiff.errorTitle, "Could not load the diff")
        XCTAssertTrue(model.errorMessage?.contains("not on a serving mirror yet") == true)
        XCTAssertEqual(BranchDiff.retry, "Try again")

        // Try again after the mirror caught up.
        api.failure = nil
        await model.load()
        XCTAssertNil(model.error)
        XCTAssertNotNil(model.comparison)
    }

    @MainActor func testDescribesTheOtherFailures() async {
        XCTAssertEqual(
            BranchDiff.describeError("Exception: no merge base", head: "h", base: "main"),
            "h and main share no history, so there is no diff between them to show.")
        XCTAssertEqual(
            BranchDiff.describeError("output limit exceeded", head: "h", base: "main"),
            "This branch changes more than a phone-sized comparison can carry. Open it on a desktop node instead.")
        XCTAssertEqual(BranchDiff.describeError("Exception: boom", head: "h", base: "main"), "Exception: boom")
        XCTAssertEqual(
            BranchDiff.emptyMessage(head: "h", base: "main", commitCount: 0),
            "h has nothing that main does not already have — it may have been merged.")
        XCTAssertEqual(
            BranchDiff.emptyMessage(head: "h", base: "main", commitCount: 2),
            "The commits on h do not change any files.")
        XCTAssertEqual(BranchDiff.commitListEmpty(0).title, "No commits")
        XCTAssertEqual(BranchDiff.commitListEmpty(3).title, "3 commits")
        XCTAssertEqual(BranchDiff.fileCount(1), "1 file")
        XCTAssertEqual(BranchDiff.fileCount(2), "2 files")
    }

    @MainActor func testAgeReadsLikeTheCommitLists() async {
        let now = Date(timeIntervalSince1970: 1_800_000_000)
        func ago(_ seconds: Double) -> String { diffAge(now.addingTimeInterval(-seconds), now: now) }
        XCTAssertEqual(ago(30), "now")
        XCTAssertEqual(ago(-600), "now")
        XCTAssertEqual(ago(90), "1m ago")
        XCTAssertEqual(ago(7200), "2h ago")
        XCTAssertEqual(ago(86_400 * 29), "29d ago")
        XCTAssertEqual(ago(86_400 * 65), "2mo ago")
        XCTAssertEqual(ago(86_400 * 800), "2y ago")
    }

    @MainActor func testFileListFilterAndViewedSet() async throws {
        let files = UnifiedDiff.parse(branchPatch)
        XCTAssertEqual(DiffFileListLogic.filter(files, query: " README ").map(\.path), ["README.md"])
        XCTAssertEqual(DiffFileListLogic.filter(files, query: "").count, 2)
        XCTAssertEqual(DiffFileListLogic.filter(files, query: "nothing"), [])
        XCTAssertEqual(DiffFileListLogic.noMatch("nothing"), "No files match \"nothing\".")
        XCTAssertFalse(DiffFileListLogic.showsFilter(5))
        XCTAssertTrue(DiffFileListLogic.showsFilter(6))
        XCTAssertEqual(DiffFileListLogic.progress(seen: 1, total: 2), 0.5)
        XCTAssertEqual(DiffFileListLogic.progress(seen: 0, total: 0), 0)
    }
}

final class CollabDiffViewingTests: XCTestCase {
    @MainActor func testStatBarSharesFiveSquares() async {
        XCTAssertEqual(diffStatBar(additions: 0, deletions: 0), Array(repeating: .empty, count: 5))
        XCTAssertEqual(diffStatBar(additions: 5, deletions: 2), [.added, .added, .added, .added, .removed])
        XCTAssertEqual(diffStatBar(additions: 1, deletions: 1), [.added, .added, .added, .removed, .removed])
        XCTAssertEqual(diffStatBar(additions: 0, deletions: 3), Array(repeating: .removed, count: 5))
    }

    @MainActor func testChangeKindsAndRowText() async {
        XCTAssertEqual(diffChangeKind(.added), .added)
        XCTAssertEqual(diffChangeKind(.copied), .added)
        XCTAssertEqual(diffChangeKind(.deleted), .deleted)
        XCTAssertEqual(diffChangeKind(.renamed), .renamed)
        XCTAssertEqual(diffChangeKind(.modified), .modified)
        let renamed = DiffFile(oldPath: "old/a.txt", newPath: "new/a.txt", status: .renamed, similarity: 90)
        XCTAssertEqual(DiffFileRowText.detail(renamed), "old/a.txt →")
        XCTAssertEqual(DiffFileRowText.name(renamed), "a.txt")
        let plain = DiffFile(oldPath: "lib/b.dart", newPath: "lib/b.dart", status: .modified)
        XCTAssertEqual(DiffFileRowText.detail(plain), "lib/")
        XCTAssertEqual(DiffFileRowText.viewedLabel(true), "Mark as not viewed")
        XCTAssertEqual(DiffFileLayout(renamed).notices, ["Renamed from old/a.txt (90% similar)"])
    }

    @MainActor func testNoticesForBinaryModeAndEmptyFiles() async {
        let binary = DiffFile(oldPath: "a.png", newPath: "a.png", status: .modified, binary: true, oldMode: "100644", newMode: "100755")
        XCTAssertEqual(
            DiffFileLayout(binary).notices,
            ["Binary file — contents are not shown.", "Mode changed 100644 → 100755"])
        XCTAssertEqual(DiffFileLayout(DiffFile(oldPath: "e", newPath: "e", status: .added)).notices, ["New empty file."])
        XCTAssertEqual(DiffFileLayout(DiffFile(oldPath: "e", newPath: "e", status: .deleted)).notices, ["Deleted empty file."])
        let copied = DiffFile(oldPath: "a", newPath: "b", status: .copied)
        XCTAssertEqual(DiffFileLayout(copied).notices, ["Copied from a"])
    }

    @MainActor func testHunkLabelsCountTheHiddenLines() async {
        let patch = """
            diff --git a/a.txt b/a.txt
            --- a/a.txt
            +++ b/a.txt
            @@ -10,2 +10,2 @@
             x
            -y
            +z
            @@ -20,1 +20,1 @@
            -p
            +q
            @@ -22,1 +22,1 @@
            -r
            +s

            """
        let layout = DiffFileLayout(UnifiedDiff.parse(patch)[0])
        XCTAssertEqual(layout.hunkLabel(0), "⋯ 9 unchanged lines")
        XCTAssertEqual(layout.hunkLabel(1), "⋯ 8 unchanged lines")
        XCTAssertEqual(layout.hunkLabel(2), "⋯ 1 unchanged line")
        XCTAssertEqual(layout.hunkLabel(5), "")
        XCTAssertEqual(layout.gutterDigits, 2)
        XCTAssertEqual(layout.unifiedRows.count, 3 + 7)
        XCTAssertEqual(DiffFileLayout.sign(.added), "+")
        XCTAssertEqual(DiffFileLayout.sign(.removed), "−")
        XCTAssertEqual(DiffFileLayout.sign(.context), " ")
    }

    @MainActor func testEmphasisSplitsHighlightedPieces() async {
        let runs = applyDiffEmphasis(
            [("final ", .keyword), ("retries = ", .plain), ("3", .number), (";", .plain)], [DiffRange(16, 17)])
        XCTAssertEqual(runs, [
            DiffRun("final ", .keyword), DiffRun("retries = ", .plain), DiffRun("3", .number, emphasized: true),
            DiffRun(";", .plain),
        ])
        let across = applyDiffEmphasis([("abc", .plain), ("def", .string)], [DiffRange(1, 4), DiffRange(5, 9)])
        XCTAssertEqual(across, [
            DiffRun("a", .plain), DiffRun("bc", .plain, emphasized: true), DiffRun("d", .string, emphasized: true),
            DiffRun("e", .string), DiffRun("f", .string, emphasized: true),
        ])
        XCTAssertEqual(applyDiffEmphasis([("x", .plain)], []), [DiffRun("x", .plain)])
        // UTF-16 offsets: the emoji is two units.
        XCTAssertEqual(
            applyDiffEmphasis([("😀ab", .plain)], [DiffRange(2, 3)]),
            [DiffRun("😀", .plain), DiffRun("a", .plain, emphasized: true), DiffRun("b", .plain)])
    }

    @MainActor func testLayoutRunsMarkTheChangedPartsAndFollowTheSyntaxSwitch() async throws {
        let layout = DiffFileLayout(UnifiedDiff.parse(branchPatch)[0])
        let lines = layout.unifiedRows.compactMap { row -> DiffLine? in
            if case .line(let line) = row { return line }
            return nil
        }
        let added = try XCTUnwrap(lines.first { $0.text == "final retries = 3;" })
        let runs = layout.runs(added, syntax: true)
        XCTAssertEqual(runs.map(\.text).joined(), "final retries = 3;")
        XCTAssertEqual(runs.filter(\.emphasized).map(\.text).joined(), "3")
        XCTAssertTrue(runs.contains { $0.kind == .keyword })
        let plain = layout.runs(added, syntax: false)
        XCTAssertTrue(plain.allSatisfy { $0.kind == .plain })
        XCTAssertEqual(plain.filter(\.emphasized).map(\.text).joined(), "3")
        let context = try XCTUnwrap(lines.first { $0.kind == .context })
        XCTAssertFalse(layout.runs(context, syntax: true).contains { $0.emphasized })
        XCTAssertEqual(layout.longestColumns, 159)
    }

    @MainActor func testSettingsReset() async {
        let settings = DiffViewSettings()
        settings.split = true
        settings.wrap = false
        settings.syntax = false
        settings.scrollSound = false
        settings.scrollHaptic = false
        settings.pageButtons = false
        settings.setFontSize(15)
        settings.reset()
        XCTAssertFalse(settings.split)
        XCTAssertTrue(settings.wrap)
        XCTAssertTrue(settings.syntax)
        XCTAssertTrue(settings.scrollSound)
        XCTAssertTrue(settings.scrollHaptic)
        XCTAssertTrue(settings.pageButtons)
        XCTAssertEqual(settings.fontSize, 12)
    }
}
