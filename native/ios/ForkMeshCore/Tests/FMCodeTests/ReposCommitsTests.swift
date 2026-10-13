// The repo explorer's git half and the commit detail, plus the commit map readers and
// `RepoDisplay.parsePatchFiles` (vectors from the Kotlin PatchFilesTest).

import FMApi
import FMBase
import XCTest
@testable import FMCode

@MainActor
final class ReposCommitsTests: XCTestCase {
    private let repo = Repository(owner: "owner", name: "repo", defaultBranch: "main")

    private func explorer() -> FakeRepoClient {
        let client = FakeRepoClient()
        client.branchesResult = {
            [
                RepoBranch(name: "feature/x", sha: "bbbbbbb1234"),
                RepoBranch(name: "main", sha: "aaaaaaa1234", isDefault: true),
                RepoBranch(name: "agent/wt", sha: "ccccccc1234", worktreePath: "/tmp/wt"),
            ]
        }
        client.commitsResult = { [["hash": "abcdef1234567890", "author": "Mona", "subject": "First commit"]] }
        client.commitDetailResult = { hash, fallback in
            var commit = fallback
            commit["hash"] = .string(hash)
            commit["subject"] = "Tip"
            return RepoCommitDetail(
                commit: commit,
                files: [
                    RepoCommitFile(path: "lib/new.dart", adds: "4", dels: "0"),
                    RepoCommitFile(path: "lib/old.dart", adds: "0", dels: "9"),
                ],
                diff: "diff --git a/lib/new.dart b/lib/new.dart\n"
                    + "new file mode 100644\n"
                    + "--- /dev/null\n"
                    + "+++ b/lib/new.dart\n"
                    + "+a\n+b\n+c\n+d\n"
                    + "diff --git a/lib/old.dart b/lib/old.dart\n"
                    + "deleted file mode 100644\n"
                    + "--- a/lib/old.dart\n"
                    + "+++ /dev/null\n"
                    + "-x\n")
        }
        return client
    }

    func testGitViewSwitchesBetweenHistoryAndBranchesWithCounts() async {
        let client = explorer()
        let model = RepoCommitsTabModel(client: client, repo: repo)
        await model.start()
        XCTAssertEqual(RepoGitView.allCases.map(\.label), ["History", "Branches", "Graph"])
        XCTAssertEqual(model.historyCount, 1)
        XCTAssertEqual(model.branchCount, 3)
        XCTAssertEqual(model.commitRows().map(\.subject), ["First commit"])
        XCTAssertEqual(model.defaultBranchLabel, "main")

        model.view = .branches
        let rows = model.branchRows
        // Default branch sorts first and carries the badge.
        XCTAssertEqual(rows.map(\.branch.name), ["main", "agent/wt", "feature/x"])
        XCTAssertEqual(rows.map(\.current), [true, false, false])
        XCTAssertEqual(rows[1].subtitle, "ccccccc · worktree")
        XCTAssertEqual(rows[2].subtitle, "bbbbbbb")

        let tip = rows[2].tipCommit!
        let detail = RepoCommitDetailModel(client: client, repo: repo, commit: tip)
        await detail.start()
        XCTAssertEqual(client.commitDetailHashes, ["bbbbbbb1234"])
        XCTAssertEqual(client.commitDetailFallbacks.first?["message"], .string("Tip of feature/x"))
    }

    func testRefreshDropsTheCacheAndReadsAgain() async {
        let client = explorer()
        let model = RepoCommitsTabModel(client: client, repo: repo)
        await model.start()
        await model.refresh()
        XCTAssertEqual(client.clearedCaches, 1)
        XCTAssertEqual(client.branchesCalls, 2)
    }

    func testCloneURLIsTheSmartHTTPEndpoint() async {
        let model = RepoCommitsTabModel(client: explorer(), repo: repo)
        XCTAssertEqual(URL(string: model.cloneURL)?.path, "/owner/repo")
        XCTAssertEqual(
            RepoCommitsTabModel(client: explorer(), repo: Repository(owner: "o", name: "r", defaultBranch: "")).defaultBranchLabel,
            "default branch")
    }

    func testCommitDetailDerivesChangeLettersFromTheDiff() async {
        let client = explorer()
        let detail = RepoCommitDetailModel(client: client, repo: repo, commit: ["hash": "abcdef1234567890", "subject": "First commit"])
        XCTAssertEqual(detail.title, "abcdef1")
        await detail.start()
        let files = detail.changedFiles!
        XCTAssertEqual(files.title, "2 changed files")
        XCTAssertEqual(files.rows.map(\.kind), [.added, .deleted])
        XCTAssertEqual(files.rows.map(\.path), ["lib/new.dart", "lib/old.dart"])
        XCTAssertEqual(files.rows.map(\.directory), ["lib/", "lib/"])
        XCTAssertEqual(files.rows.map(\.name), ["new.dart", "old.dart"])
        // Zero counts are not rendered next to the badge.
        XCTAssertEqual(files.rows.map(\.adds), ["4", ""])
        XCTAssertEqual(files.rows.map(\.dels), ["", "9"])
        XCTAssertEqual(files.totalAdds, 4)
        XCTAssertEqual(files.totalDels, 9)
        XCTAssertTrue(files.showsTotals)
        let review = detail.review!
        XCTAssertEqual(review.scope, "owner/repo/commit/abcdef1234567890")
        XCTAssertEqual(review.files.map(\.file.path), ["lib/new.dart", "lib/old.dart"])
    }

    func testCommitDetailRendersChangedFilesAndTruncatedDiff() async {
        let client = FakeRepoClient()
        client.commitsResult = {
            [["hash": "abcdef1234567890", "author": "Mona", "date": "2026-07-04", "subject": "Add mobile commit diff"]]
        }
        client.commitDetailResult = { hash, fallback in
            var commit = fallback
            commit["hash"] = .string(hash)
            commit["subject"] = "Add mobile commit diff"
            commit["author"] = "Mona"
            commit["body"] = "Shows changed files and a unified diff."
            return RepoCommitDetail(
                commit: commit, files: [RepoCommitFile(path: "lib/main.dart", adds: "12", dels: "3")],
                diff: "diff --git a/lib/main.dart b/lib/main.dart\n+hello\n-old", truncated: true, source: "qt-node-a")
        }
        let tab = RepoCommitsTabModel(client: client, repo: repo)
        await tab.start()
        let row = tab.commitRows()[0]
        XCTAssertEqual(row.subject, "Add mobile commit diff")
        XCTAssertEqual(row.author, "Mona")
        XCTAssertTrue(row.isHead && row.isFirst && row.isLast)

        let detail = RepoCommitDetailModel(client: client, repo: repo, commit: row.commit)
        await detail.start()
        XCTAssertEqual(client.commitDetailHashes, ["abcdef1234567890"])
        let files = detail.changedFiles!
        XCTAssertEqual(files.title, "1 changed file")
        XCTAssertEqual(files.rows[0].path, "lib/main.dart")
        XCTAssertEqual(files.rows[0].adds, "12")
        XCTAssertEqual(files.rows[0].dels, "3")
        XCTAssertFalse(files.showsTotals, "one row carries its own counts")
        XCTAssertTrue(detail.shownDetail.truncated)
        XCTAssertTrue(detail.shownDetail.diff.hasPrefix("diff --git"))
        let summary = detail.summary
        XCTAssertEqual(summary.title, "Add mobile commit diff")
        XCTAssertEqual(summary.hashLabel, "abcdef1234567890")
        XCTAssertEqual(summary.sourceLabel, "served by qt-node-a")
        XCTAssertEqual(summary.body, "Shows changed files and a unified diff.")
        XCTAssertEqual(summary.repoName, "owner/repo")
    }

    func testCommitWithoutHashNeedsNoFetch() async {
        let client = FakeRepoClient()
        let detail = RepoCommitDetailModel(client: client, repo: repo, commit: ["message": "Loose"])
        await detail.start()
        XCTAssertTrue(client.commitDetailHashes.isEmpty)
        XCTAssertFalse(detail.loadsDiff)
        XCTAssertEqual(detail.title, "Commit")
        XCTAssertEqual(detail.summary.hashLabel, "unknown")
        XCTAssertEqual(detail.summary.title, "Loose")
        XCTAssertNil(detail.review)
    }

    func testCommitDetailFailureKeepsTheSummary() async {
        let client = FakeRepoClient()
        client.commitDetailResult = { _, _ in throw ApiException("HTTP 404 for /api/repo/owner/repo/commit/abc") }
        let detail = RepoCommitDetailModel(client: client, repo: repo, commit: ["hash": "abc", "subject": "S"])
        await detail.start()
        XCTAssertEqual(detail.errorMessage, "Could not load commit diff: Exception: HTTP 404 for /api/repo/owner/repo/commit/abc")
        XCTAssertEqual(detail.summary.title, "S")
        XCTAssertNil(detail.review)
    }

    func testPullPatchSummarisesFilesChanged() {
        let patch = "diff --git a/app/a.py b/app/a.py\n"
            + "--- a/app/a.py\n"
            + "+++ b/app/a.py\n"
            + "@@ -1,2 +1,2 @@\n"
            + "-old\n"
            + "+new\n"
            + "+more\n"
            + "diff --git a/app/b.py b/app/b.py\n"
            + "new file mode 100644\n"
            + "--- /dev/null\n"
            + "+++ b/app/b.py\n"
            + "+hello\n"
        let files = RepoChangedFiles.forPatch(patch)!
        XCTAssertEqual(files.title, "2 changed files")
        XCTAssertEqual(files.rows.map(\.path), ["app/a.py", "app/b.py"])
        XCTAssertEqual(files.rows.map { $0.kind?.letter }, ["M", "A"])
        XCTAssertEqual(files.totalAdds, 3)
        XCTAssertEqual(files.totalDels, 1)
        XCTAssertNil(RepoChangedFiles.forPatch("  \n"))
        XCTAssertNil(RepoChangedFiles.forCommit(files: [], diff: patch))
    }

    func testParsePatchFilesKindsAndCounts() {
        let diff = [
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
        ].joined(separator: "\r\n") + "\n"
        XCTAssertEqual(RepoDisplay.parsePatchFiles(diff), [
            RepoPatchFile(path: "lib/a.dart", kind: .modified, adds: 2, dels: 1),
            RepoPatchFile(path: "new.txt", kind: .added, adds: 1, dels: 0),
            RepoPatchFile(path: "gone.txt", kind: .deleted, adds: 0, dels: 1),
            RepoPatchFile(path: "new/name.kt", kind: .renamed, adds: 0, dels: 0),
        ])
        XCTAssertEqual(
            RepoDisplay.parsePatchFiles("diff --git a/lib/a.dart b/lib/a.dart\n+review"),
            [RepoPatchFile(path: "lib/a.dart", kind: .modified, adds: 1, dels: 0)])
        XCTAssertEqual(RepoDisplay.parsePatchFiles("  \n"), [])
        XCTAssertEqual(RepoDisplay.parsePatchFiles("+orphan line"), [])
        // A path containing " b/" keeps the first split, as the lazy regex does.
        XCTAssertEqual(RepoDisplay.parsePatchFiles("diff --git a/x b/y b/y\n").map(\.path), ["y b/y"])
    }

    func testLineSplittingTitlesAndDiffTones() {
        XCTAssertEqual(RepoDisplay.dartLines("a\r\nb\n\rc"), ["a", "b", "", "c"])
        XCTAssertEqual(RepoDisplay.dartLines("a\n\n"), ["a", ""])
        XCTAssertEqual(RepoDisplay.dartLines(""), [])
        XCTAssertEqual(RepoDisplay.changedFilesTitle(1), "1 changed file")
        XCTAssertEqual(RepoDisplay.changedFilesTitle(3), "3 changed files")
        XCTAssertEqual(RepoDisplay.diffLineTone("+x"), .success)
        XCTAssertNil(RepoDisplay.diffLineTone("+++ b/x"))
        XCTAssertEqual(RepoDisplay.diffLineTone("-x"), .danger)
        XCTAssertNil(RepoDisplay.diffLineTone("--- a/x"))
        XCTAssertEqual(RepoDisplay.diffLineTone("@@ -1 +1 @@"), .accent)
        XCTAssertNil(RepoDisplay.diffLineTone(" context"))
    }

    func testCommitMapReaders() {
        XCTAssertEqual(RepoDisplay.commitHash(["sha": "abc"]), "abc")
        XCTAssertEqual(RepoDisplay.commitHash(["hash": .null, "id": 42]), "42")
        XCTAssertEqual(RepoDisplay.commitHash([:]), "")
        XCTAssertEqual(RepoDisplay.commitShort(["hash": "abc"]), "abc")
        XCTAssertEqual(RepoDisplay.commitShort(["hash": "abcdefg9"]), "abcdefg")
        XCTAssertEqual(RepoDisplay.commitSubject(["message": "  Fix it  \n\nbody"]), "Fix it")
        XCTAssertEqual(RepoDisplay.commitSubject(["subject": "   "]), "Commit")
        XCTAssertEqual(RepoDisplay.commitSubject([:]), "Commit")
        XCTAssertEqual(RepoDisplay.commitAuthor(["author": ["name": "Mona", "email": "m@x"]]), "Mona")
        XCTAssertEqual(RepoDisplay.commitAuthor(["author": ["email": "m@x"]]), "m@x")
        XCTAssertEqual(RepoDisplay.commitAuthor(["authorName": "Ada"]), "Ada")
        XCTAssertEqual(RepoDisplay.commitAuthor(["committer": "Bot"]), "Bot")
        XCTAssertEqual(RepoDisplay.commitAuthor([:]), "")
        XCTAssertEqual(RepoDisplay.commitBody(["description": "d"]), "d")
    }

    func testCommitDatesAndRelativeTimes() {
        let utc = TimeZone(identifier: "UTC")!
        XCTAssertEqual(RepoDisplay.commitDateMs(["date": 1_755_900_000]), 1_755_900_000_000)
        XCTAssertEqual(RepoDisplay.commitDateMs(["ts": 1_755_900_000_123]), 1_755_900_000_123)
        XCTAssertEqual(RepoDisplay.commitDateMs(["time": 1_755_900_000.4]), 1_755_900_000_000)
        XCTAssertEqual(RepoDisplay.commitDateMs(["timestamp": "1755900000"]), 1_755_900_000_000)
        XCTAssertEqual(RepoDisplay.commitDateMs(["createdAt": "2026-08-21T18:09:41Z"], timeZone: utc), 1_787_335_781_000)
        XCTAssertEqual(RepoDisplay.commitDateMs(["date": "2026-07-04"], timeZone: utc), 1_783_123_200_000)
        XCTAssertNil(RepoDisplay.commitDateMs(["date": "  ", "time": "soon"]))
        // An unparseable key does not stop the search.
        XCTAssertEqual(RepoDisplay.commitDateMs(["date": "soon", "ts": 5]), 5000)

        let now = Date(epochMillis: 1_800_000_000_000)
        let ms = 1_800_000_000_000
        XCTAssertEqual(RepoDisplay.relativeAge(ms - 30_000, now: now), "now")
        XCTAssertEqual(RepoDisplay.relativeAge(ms + 90_000, now: now), "now")
        XCTAssertEqual(RepoDisplay.relativeAge(ms - 5 * 60_000, now: now), "5m")
        XCTAssertEqual(RepoDisplay.relativeAge(ms - 2 * 3_600_000, now: now), "2h")
        XCTAssertEqual(RepoDisplay.relativeAge(ms - 3 * 86_400_000, now: now), "3d")
        XCTAssertEqual(RepoDisplay.relativeAge(ms - 65 * 86_400_000, now: now), "2mo")
        XCTAssertEqual(RepoDisplay.relativeAge(ms - 800 * 86_400_000, now: now), "2y")
        XCTAssertEqual(RepoDisplay.agoLabel(ms, now: now), "just now")
        XCTAssertEqual(RepoDisplay.agoLabel(ms - 3 * 3_600_000, now: now), "3h ago")
        XCTAssertEqual(RepoDisplay.relativeCommitTime(["ts": .int(Int64(ms - 60_000))], now: now), "1m")
        XCTAssertEqual(RepoDisplay.relativeCommitTime([:], now: now), "")
    }

    /// The model half of repo_explorer_git_test's last case.
    func testTreeEntriesParseLastCommitDateAndSubject() {
        let entry = RepoTreeEntry(json: [
            "name": ".gitignore", "path": ".gitignore", "type": "blob", "size": 2063,
            "date": "2026-08-21T18:09:41-04:00", "subject": "Stop node_modules symlinks reaching commits",
            "message": "Stop node_modules symlinks reaching commits\n\nLong body",
        ])
        XCTAssertFalse(entry.isDirectory)
        XCTAssertEqual(entry.updatedMs, 1_787_350_181_000)
        XCTAssertEqual(entry.lastMessage, "Stop node_modules symlinks reaching commits")

        let seconds = RepoTreeEntry(json: ["name": "app", "type": "tree", "date": 1_755_900_000])
        XCTAssertTrue(seconds.isDirectory)
        XCTAssertEqual(seconds.updatedMs, 1_755_900_000 * 1000)

        let prefixed = RepoTree(
            json: ["path": "mobile", "entries": [["name": "lib", "path": "lib", "type": "tree", "date": "2026-08-01T00:00:00Z"]]],
            path: "mobile")
        XCTAssertEqual(prefixed.entries.first?.path, "mobile/lib")
        XCTAssertGreaterThan(prefixed.entries.first?.updatedMs ?? 0, 0)
    }
}
