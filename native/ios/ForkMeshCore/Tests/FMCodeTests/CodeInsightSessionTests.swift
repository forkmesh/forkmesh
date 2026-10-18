import FMApi
import FMBase
import FMCodeInsight
import FMGit
import Foundation
import XCTest
@testable import FMCode

final class CodeInsightTargetsTests: XCTestCase {
    func testHistorySettings() {
        XCTAssertEqual(CodeInsightHistory.allCases.map(\.label), ["Files only", "Last 300 commits", "Last 2,000 commits"])
        XCTAssertEqual(CodeInsightHistory.allCases.map(\.cloneDepth), [1, 301, 2001])
        XCTAssertFalse(CodeInsightHistory.filesOnly.options.history)
        XCTAssertEqual(CodeInsightHistory.recent.options.maxCommits, 300)
        XCTAssertEqual(CodeInsightHistory.extended.options.maxCommits, 2000)
        let prefs = InMemoryKeyValueStore()
        XCTAssertEqual(CodeInsightHistory.load(prefs), .recent)
        CodeInsightHistory.extended.save(prefs)
        XCTAssertEqual(CodeInsightHistory.load(prefs), .extended)
        XCTAssertEqual(CodeInsightHistory.from("nonsense"), .recent)
    }

    func testTargetsRoundTripAndChoose() {
        let account = CodeInsightTarget.account(owner: "ada", name: "engine")
        XCTAssertEqual(account.key, "repo:ada/engine")
        XCTAssertEqual(CodeInsightTarget.parse(account.key), account)
        XCTAssertEqual(CodeInsightTarget.parse("local:notes"), .local(name: "notes", publishedAs: nil))
        XCTAssertNil(CodeInsightTarget.parse("repo:broken"))
        XCTAssertNil(CodeInsightTarget.parse("local:"))
        XCTAssertEqual(account.repoRef, RepoRef(owner: "ada", name: "engine"))
        XCTAssertEqual(CodeInsightTarget.local(name: "x", publishedAs: "ada/x").repoRef, RepoRef(owner: "ada", name: "x"))
        let record = LocalRepoRecord(name: "notes", path: "/tmp/notes", createdAt: Date(timeIntervalSince1970: 0), publishedAs: "ada/notes")
        let repository = Repository(owner: "ada", name: "engine")
        XCTAssertEqual(
            CodeInsightTarget.choices(local: [record], account: [repository, repository]).map(\.key),
            ["local:notes", "repo:ada/engine"])
        XCTAssertEqual(
            CodeInsightTarget.initial(route: RepoRef(owner: "ada", name: "engine"), preferred: .local(name: "notes", publishedAs: nil), last: nil, local: [record]),
            account)
        XCTAssertEqual(
            CodeInsightTarget.initial(route: nil, preferred: .local(name: "notes", publishedAs: nil), last: account, local: [record]),
            .local(name: "notes", publishedAs: "ada/notes"))
        XCTAssertEqual(CodeInsightTarget.initial(route: nil, preferred: nil, last: account, local: []), account)
        XCTAssertNil(CodeInsightTarget.initial(route: nil, preferred: nil, last: .local(name: "gone", publishedAs: nil), local: [record]))
        XCTAssertNil(CodeInsightTarget.initial(route: nil, preferred: nil, last: nil, local: [record]))
    }

    func testLabels() {
        XCTAssertEqual(CodeInsightLabels.sourceLabel(.local(name: "a", publishedAs: nil), .recent), "On this phone")
        XCTAssertEqual(CodeInsightLabels.sourceLabel(.account(owner: "a", name: "b"), .recent), "Analysis copy · 300 commits")
        XCTAssertEqual(CodeInsightLabels.sourceLabel(.account(owner: "a", name: "b"), .filesOnly), "Analysis copy · files only")
        XCTAssertEqual(CodeInsightLabels.statusLine(source: "On this phone", files: 1234, summary: "3 unhealthy"), "On this phone · 1,234 files · 3 unhealthy")
        XCTAssertEqual(CodeInsightLabels.statusLine(source: "On this phone", files: 1, summary: ""), "On this phone · 1 file")
        XCTAssertEqual(CodeInsightLabels.copyFolder(owner: "ada", name: "engine.v2"), "ada/engine.v2")
        XCTAssertNil(CodeInsightLabels.copyFolder(owner: "..", name: "engine"))
        XCTAssertNil(CodeInsightLabels.copyFolder(owner: "ada", name: "a/b"))
        XCTAssertNil(CodeInsightLabels.copyFolder(owner: "ada", name: ".git"))
        XCTAssertNil(CodeInsightLabels.copyFolder(owner: "", name: "x"))
    }

    func testCopiesLayoutAndDeepening() throws {
        let root = try localTempDirectory(self, "insight-copies")
        let copies = CodeInsightCopies(root: root)
        XCTAssertEqual(copies.directory(owner: "ada", name: "engine")?.path, root.appendingPathComponent("ada/engine").path)
        XCTAssertNil(copies.directory(owner: "ada", name: "../x"))
        XCTAssertFalse(copies.exists(owner: "ada", name: "engine"))
        XCTAssertEqual(copies.fetchedDepth(owner: "ada", name: "engine"), 0)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("ada"), withIntermediateDirectories: true)
        copies.recordDepth(301, owner: "ada", name: "engine")
        XCTAssertEqual(copies.fetchedDepth(owner: "ada", name: "engine"), 301)
        let staging = try XCTUnwrap(copies.stagingDirectory(owner: "ada", name: "engine"))
        XCTAssertEqual(staging.deletingLastPathComponent().path, root.appendingPathComponent("ada").path)
        XCTAssertTrue(staging.lastPathComponent.hasPrefix(".engine.partial-"))
        XCTAssertEqual(CodeInsightCopies.tipRef(branch: "main"), "refs/remotes/origin/main")
        XCTAssertNil(CodeInsightCopies.tipRef(branch: "HEAD"))
        XCTAssertEqual(CodeInsightCopies.deepen(fetched: 301, wanted: .extended, shallow: true), .relative(1700))
        XCTAssertNil(CodeInsightCopies.deepen(fetched: 2001, wanted: .recent, shallow: true))
        XCTAssertNil(CodeInsightCopies.deepen(fetched: 1, wanted: .extended, shallow: false))
        XCTAssertNil(CodeInsightCopies.deepen(fetched: 1, wanted: .filesOnly, shallow: true))
        XCTAssertEqual(CodeInsightCopies.deepen(fetched: 0, wanted: .recent, shallow: true), .relative(300))
    }
}

@MainActor
final class CodeInsightSessionTests: XCTestCase {
    private func waitUntil(_ condition: @MainActor () -> Bool, timeout: TimeInterval = 30) async {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition() && Date() < deadline {
            try? await Task.sleep(nanoseconds: 20_000_000)
        }
    }

    private func makeRepo() throws -> InsightTestRepo {
        let repo = try InsightTestRepo.create(self)
        try repo.commit(["src/main.swift": "func run() {\n    if true { print(1) }\n}\n"], message: "One\n", time: insightNow - 3 * 86400)
        try repo.commit(["src/main.swift": "func run() {\n    if true { print(2) }\n}\n", "src/other.py": "x = 1\n"], message: "Two\n", time: insightNow - 2 * 86400)
        return repo
    }

    func testAnalyzesCachesAndReanalyzes() async throws {
        let repo = try makeRepo()
        let prefs = InMemoryKeyValueStore()
        var clock = Date(timeIntervalSince1970: TimeInterval(insightNow))
        var prepared = 0
        let session = CodeInsightSession(prefs: prefs, now: { clock }) { target, history, progress in
            prepared += 1
            progress("Preparing \(target.title)")
            return CodeInsightPrepared(
                worktree: repo.root, tipRef: nil, readsWorkingFiles: true, name: target.title, description: repo.root.path,
                sourceLabel: CodeInsightLabels.sourceLabel(target, history))
        }
        let target = CodeInsightTarget.local(name: "sample", publishedAs: nil)
        session.attach(target)
        XCTAssertTrue(session.isRunning(target))
        XCTAssertNotNil(session.progress(for: target))
        await waitUntil { !session.isRunning(target) }
        let result = try XCTUnwrap(session.result(for: target))
        XCTAssertNil(session.error(for: target))
        XCTAssertEqual(result.snapshot.files.map(\.path), ["src/main.swift", "src/other.py"])
        XCTAssertEqual(result.sourceLabel, "On this phone")
        XCTAssertEqual(result.branch, "main")
        XCTAssertEqual(prepared, 1)
        XCTAssertEqual(session.lastTarget, target)
        XCTAssertNil(session.progress(for: target))
        session.detach(target)
        session.attach(target)
        XCTAssertFalse(session.isRunning(target))
        XCTAssertEqual(prepared, 1)
        let trend = await session.trend(target, path: "src/main.swift")
        XCTAssertEqual(trend.count, 2)
        XCTAssertLessThan(trend[0].time, trend[1].time)
        session.reanalyze(target)
        XCTAssertTrue(session.isRunning(target))
        await waitUntil { !session.isRunning(target) }
        XCTAssertEqual(prepared, 2)
        session.detach(target)
        clock = clock.addingTimeInterval(CodeInsightSession.freshSeconds + 1)
        XCTAssertFalse(session.isFresh(target))
        session.attach(target)
        XCTAssertTrue(session.isRunning(target))
        session.detach(target)
        await waitUntil { !session.isRunning(target) }
        XCTAssertNil(session.error(for: target))
        session.setHistory(.filesOnly)
        XCTAssertEqual(CodeInsightHistory.load(prefs), .filesOnly)
    }

    func testReportsPreparationErrors() async throws {
        let session = CodeInsightSession(prefs: nil) { _, _, _ in
            throw CodeInsightCopyFailure("No mirror is serving ada/engine right now.")
        }
        let target = CodeInsightTarget.account(owner: "ada", name: "engine")
        session.attach(target)
        await waitUntil { !session.isRunning(target) }
        XCTAssertEqual(session.error(for: target), "No mirror is serving ada/engine right now.")
        XCTAssertNil(session.result(for: target))
    }

    func testDetachCancelsARunningAnalysis() async throws {
        let session = CodeInsightSession(prefs: nil) { _, _, _ in
            try await Task.sleep(nanoseconds: 300_000_000)
            throw CodeInsightCopyFailure("should not surface")
        }
        let target = CodeInsightTarget.account(owner: "ada", name: "engine")
        session.attach(target)
        session.detach(target)
        await waitUntil { !session.isRunning(target) }
        XCTAssertNil(session.error(for: target))
        XCTAssertNil(session.result(for: target))
    }

    func testFetcherReadsALocalCopyInPlace() async throws {
        let repo = try makeRepo()
        let storeRoot = try localTempDirectory(self, "insight-store")
        let store = LocalRepoStore(root: storeRoot)
        let fetcher = CodeInsightFetcher(
            copies: CodeInsightCopies(root: try localTempDirectory(self, "insight-cache")), store: store, publisher: nil, session: { nil })
        await localAssertThrows(try await fetcher.prepare(.local(name: "missing", publishedAs: nil), history: .recent, progress: { _ in })) { error in
            XCTAssertEqual(CodeInsightSession.message(error), "missing is no longer on this phone.")
        }
        await localAssertThrows(try await fetcher.prepare(.account(owner: "ada", name: "engine"), history: .recent, progress: { _ in })) { error in
            XCTAssertEqual(CodeInsightSession.message(error), "Repositories cannot be copied to this phone in this build.")
        }
        await localAssertThrows(try await fetcher.prepare(.account(owner: "ada", name: "../x"), history: .recent, progress: { _ in })) { error in
            XCTAssertTrue(CodeInsightSession.message(error).contains("unusual characters"))
        }
        _ = repo
    }
}
