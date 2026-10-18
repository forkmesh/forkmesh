import FMCodeInsight
import FMGit
import Foundation
import XCTest
@testable import FMCode

let insightNow: Int = 1_792_000_000

struct InsightTestRepo {
    let root: URL
    let repository: GitRepository

    static func create(_ test: XCTestCase) throws -> InsightTestRepo {
        let root = try localTempDirectory(test, "insight-repo")
        let repository = try GitRepository.initialize(GitRepositoryInit(root.path, initialBranch: try GitBranchName("main")))
        return InsightTestRepo(root: root, repository: repository)
    }

    @discardableResult
    func commit(_ files: [String: String?], message: String, time: Int, author: String = "Ada Tester") throws -> GitObjectId {
        try commitBytes(files.mapValues { $0.map { Array($0.utf8) } }, message: message, time: time, author: author)
    }

    @discardableResult
    func commitBytes(_ files: [String: [UInt8]?], message: String, time: Int, author: String = "Ada Tester") throws -> GitObjectId {
        let tree = GitWorktree(root.path)
        var added: [GitWorktreePath] = []
        var removed: [GitWorktreePath] = []
        for path in files.keys.sorted() {
            let worktreePath = try GitWorktreePath(path)
            if let bytes = files[path] ?? nil {
                try tree.writeFile(worktreePath, bytes)
                added.append(worktreePath)
            } else {
                try tree.deleteFile(worktreePath)
                removed.append(worktreePath)
            }
        }
        if !added.isEmpty { try repository.add(added) }
        if !removed.isEmpty { try repository.remove(removed) }
        let identity = try GitIdentity(name: author, email: "tester@example.com", timestampSeconds: time, timezoneOffsetMinutes: 0)
        return try repository.commit(GitCommitRequest(message: message, author: identity, committer: identity))
    }

    func source(readsWorkingFiles: Bool = false, now: Int = insightNow) throws -> GitInsightSource {
        try GitInsightSource.open(
            worktree: root, name: root.lastPathComponent, description: root.path, readsWorkingFiles: readsWorkingFiles,
            now: { Date(timeIntervalSince1970: TimeInterval(now)) })
    }
}

struct InsightTestGit {
    let path: String

    static func require() throws -> InsightTestGit {
        guard let git = LocalTestGit.locate() else { throw XCTSkip("git binary not on PATH") }
        return InsightTestGit(path: git.path)
    }

    @discardableResult
    func run(_ arguments: [String], in directory: URL, date: Int? = nil) throws -> String {
        #if os(iOS) || os(tvOS) || os(watchOS)
        throw XCTSkip("no subprocesses on this platform")
        #else
        let process = Process()
        process.executableURL = URL(fileURLWithPath: path)
        process.arguments = arguments
        process.currentDirectoryURL = directory
        let home = NSTemporaryDirectory() + "forkmesh-insight-git-home-\(ProcessInfo.processInfo.processIdentifier)"
        try? FileManager.default.createDirectory(atPath: home, withIntermediateDirectories: true)
        var environment = [
            "PATH": "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin",
            "HOME": home,
            "XDG_CONFIG_HOME": home,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": "/dev/null",
            "GIT_ATTR_NOSYSTEM": "1",
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_AUTHOR_NAME": "Ada Tester", "GIT_AUTHOR_EMAIL": "tester@example.com",
            "GIT_COMMITTER_NAME": "Ada Tester", "GIT_COMMITTER_EMAIL": "tester@example.com",
            "LC_ALL": "C",
        ]
        if let date {
            environment["GIT_AUTHOR_DATE"] = "@\(date) +0000"
            environment["GIT_COMMITTER_DATE"] = "@\(date) +0000"
        }
        process.environment = environment
        let output = Pipe()
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        process.standardInput = FileHandle.nullDevice
        try process.run()
        let data = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        if process.terminationStatus != 0 {
            throw XCTSkip("git \(arguments.joined(separator: " ")) failed")
        }
        return String(decoding: data, as: UTF8.self)
        #endif
    }

    func history(_ directory: URL, extra: [String] = []) throws -> [InsightCommit] {
        let log = try run(
            ["log", "--no-merges", "--no-renames", "--numstat", "--format=%x1e%H%x1f%an%x1f%at%x1f%s"] + extra, in: directory)
        return Insight.parseGitLog(log).map { commit in
            InsightCommit(
                hash: commit.hash, author: commit.author, time: commit.time, subject: commit.subject,
                files: commit.files.filter { GitInsightSource.analysable($0.path) })
        }
    }
}

final class CodeInsightSourceTests: XCTestCase {
    private func lines(_ count: Int, _ prefix: String) -> String {
        (1...count).map { "\(prefix) \($0)" }.joined(separator: "\n") + "\n"
    }

    private func sampleRepo() throws -> (InsightTestRepo, [GitObjectId]) {
        let repo = try InsightTestRepo.create(self)
        var ids: [GitObjectId] = []
        let swift = (1...10).map { "let value\($0) = \($0)" }.joined(separator: "\n") + "\n"
        ids.append(try repo.commit([
            "src/a.swift": swift,
            "src/b.py": lines(5, "# line"),
            "vendor/lib.js": lines(3, "// vendored"),
            "README.md": "# Sample\n",
        ], message: "Initial import\n\nWith a body line.\n", time: insightNow - 40 * 86400))
        var edited = swift.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        edited[1] = "let value2 = 200"
        edited[5] = "let value6 = 600"
        edited.insert(contentsOf: ["let extra1 = 1", "let extra2 = 2", "let extra3 = 3"], at: 8)
        ids.append(try repo.commit([
            "src/a.swift": edited.joined(separator: "\n"),
            "src/b.py": nil,
        ], message: "Fix the values\n", time: insightNow - 30 * 86400, author: "Grace Coder"))
        ids.append(try repo.commitBytes([
            "src/c.go": Array("package main\n\nfunc main() {\n}\n".utf8),
            "src/blob.c": [0x63, 0x00, 0x01, 0x02, 0x0a],
        ], message: "Add go and a binary\n", time: insightNow - 20 * 86400))
        ids.append(try repo.commit(["README.md": "# Sample\n\nMore.\n"], message: "Docs only\n", time: insightNow - 10 * 86400))
        ids.append(try repo.commit([
            "src/c.go": "package main\n\nfunc main() {\n\tprintln(1)\n}",
        ], message: "Print\n", time: insightNow - 5 * 86400))
        return (repo, ids)
    }

    func testHistoryCountsLinesLikeGit() throws {
        let (repo, ids) = try sampleRepo()
        let commits = try XCTUnwrap(try repo.source().history(InsightOptions(), cancel: { false }))
        XCTAssertEqual(commits.map(\.hash), ids.reversed().map(\.hex))
        XCTAssertEqual(commits[0].files, [InsightCommitFile(path: "src/c.go", added: 2, deleted: 1)])
        XCTAssertEqual(commits[1].files, [])
        XCTAssertEqual(commits[2].files, [
            InsightCommitFile(path: "src/blob.c", added: nil, deleted: nil),
            InsightCommitFile(path: "src/c.go", added: 4, deleted: 0),
        ])
        XCTAssertEqual(commits[3].files, [
            InsightCommitFile(path: "src/a.swift", added: 5, deleted: 2),
            InsightCommitFile(path: "src/b.py", added: 0, deleted: 5),
        ])
        XCTAssertEqual(commits[3].author, "Grace Coder")
        XCTAssertEqual(commits[4].files, [
            InsightCommitFile(path: "src/a.swift", added: 10, deleted: 0),
            InsightCommitFile(path: "src/b.py", added: 5, deleted: 0),
        ])
        XCTAssertEqual(commits[4].subject, "Initial import")
        XCTAssertEqual(commits[4].time, Int64(insightNow - 40 * 86400))
        let git = try InsightTestGit.require()
        XCTAssertEqual(commits, try git.history(repo.root))
    }

    func testHistoryStopsAtLimits() throws {
        let (repo, ids) = try sampleRepo()
        var options = InsightOptions()
        options.maxCommits = 2
        XCTAssertEqual(try repo.source().history(options, cancel: { false })?.map(\.hash), [ids[4].hex, ids[3].hex])
        options = InsightOptions()
        options.historyDays = 25
        XCTAssertEqual(try repo.source().history(options, cancel: { false })?.map(\.hash), [ids[4].hex, ids[3].hex, ids[2].hex])
        XCTAssertThrowsError(try repo.source().history(InsightOptions(), cancel: { true })) { error in
            XCTAssertTrue(error is CancellationError)
        }
    }

    func testMergesBranchesAndShallowBoundaryMatchGit() throws {
        let git = try InsightTestGit.require()
        let root = try localTempDirectory(self, "insight-git")
        try git.run(["init", "-q", "-b", "main"], in: root)
        func write(_ path: String, _ text: String) throws { try localWrite(root.appendingPathComponent(path), text) }
        var time = insightNow - 50 * 86400
        func commit(_ message: String) throws {
            time += 3600
            try git.run(["add", "-A"], in: root)
            try git.run(["commit", "-q", "-m", message], in: root, date: time)
        }
        try write("app/main.kt", lines(20, "val x ="))
        try write("app/util.ts", lines(8, "export const y ="))
        try commit("Start")
        try git.run(["checkout", "-q", "-b", "feature"], in: root)
        try write("app/util.ts", lines(6, "export const y =") + "export const z = 1\n")
        try write("app/new.rs", "fn main() {}\n")
        try commit("Feature work")
        try git.run(["checkout", "-q", "main"], in: root)
        try write("app/main.kt", lines(18, "val x =") + "val changed = 1\n")
        try commit("Main work")
        time += 3600
        try git.run(["merge", "-q", "--no-ff", "-m", "Merge feature", "feature"], in: root, date: time)
        try write("app/main.kt", lines(10, "val x ="))
        try git.run(["mv", "app/util.ts", "app/helpers.ts"], in: root)
        try commit("Shrink and rename")
        try write("app/helpers.ts", "export const only = 1")
        try commit("No newline at end")
        let source = try GitInsightSource.open(
            worktree: root, name: "insight-git", description: root.path, readsWorkingFiles: false,
            now: { Date(timeIntervalSince1970: TimeInterval(insightNow)) })
        let expected = try git.history(root)
        XCTAssertEqual(expected.count, 5)
        XCTAssertEqual(try source.history(InsightOptions(), cancel: { false }), expected)
        let shallow = try localTempDirectory(self, "insight-shallow")
        try git.run(["clone", "-q", "--depth", "3", "--no-local", "file://\(root.path)", shallow.path], in: root)
        try git.run(["remote", "set-url", "origin", "https://example.com/ada/insight.git"], in: shallow)
        let shallowSource = try GitInsightSource.open(
            worktree: shallow, name: "shallow", description: shallow.path, readsWorkingFiles: false,
            now: { Date(timeIntervalSince1970: TimeInterval(insightNow)) })
        let walked = try XCTUnwrap(try shallowSource.history(InsightOptions(), cancel: { false }))
        XCTAssertFalse(walked.isEmpty)
        XCTAssertEqual(walked, Array(expected.prefix(walked.count)))
        XCTAssertLessThan(walked.count, expected.count)
        XCTAssertGreaterThan(try git.history(shallow).count, walked.count)
    }

    func testEntriesReadTheTreeAndWorkingFiles() throws {
        let (repo, ids) = try sampleRepo()
        let source = try repo.source()
        XCTAssertEqual(source.head, ids.last?.hex)
        XCTAssertEqual(source.branch, "main")
        XCTAssertTrue(source.git)
        XCTAssertEqual(try source.entries().map(\.path), ["README.md", "src/a.swift", "src/blob.c", "src/c.go", "vendor/lib.js"])
        XCTAssertEqual(try source.read("src/c.go", limit: 1 << 20), Array("package main\n\nfunc main() {\n\tprintln(1)\n}".utf8))
        XCTAssertNil(try source.read("src/c.go", limit: 4))
        XCTAssertNil(try source.read("missing.swift", limit: 1 << 20))
        try localWrite(repo.root.appendingPathComponent("src/c.go"), "package changed\n")
        XCTAssertEqual(try source.read("src/c.go", limit: 1 << 20), Array("package main\n\nfunc main() {\n\tprintln(1)\n}".utf8))
        let working = try repo.source(readsWorkingFiles: true)
        let entries = try working.entries()
        XCTAssertEqual(entries.first { $0.path == "src/c.go" }?.size, 16)
        XCTAssertEqual(try working.read("src/c.go", limit: 1 << 20), Array("package changed\n".utf8))
    }

    func testAnalyzeRunsTheEngineOverTheRepository() throws {
        let (repo, _) = try sampleRepo()
        let snapshot = try Insight.analyze(source: try repo.source(), now: Int64(insightNow))
        XCTAssertEqual(snapshot.error, "")
        XCTAssertTrue(snapshot.git)
        XCTAssertEqual(snapshot.branch, "main")
        XCTAssertEqual(snapshot.files.map(\.path), ["src/a.swift", "src/c.go"])
        XCTAssertEqual(snapshot.commitsScanned, 5)
        XCTAssertEqual(snapshot.files[0].commits, 2)
        XCTAssertEqual(snapshot.files[0].linesAdded, 15)
        XCTAssertEqual(snapshot.files[0].linesDeleted, 2)
        XCTAssertEqual(snapshot.files[1].commits, 2)
    }

    func testTrendSamplesCommitsThatTouchedTheFile() throws {
        let repo = try InsightTestRepo.create(self)
        var touched: [GitObjectId] = []
        for index in 0..<20 {
            let body = (0...index).map { "    if x > \($0) { return \($0) }" }.joined(separator: "\n")
            touched.append(try repo.commit(
                ["lib/score.swift": "func score(_ x: Int) -> Int {\n\(body)\n    return 0\n}\n"],
                message: "Step \(index)\n", time: insightNow - (40 - index) * 86400))
            try repo.commit(["notes.txt": "step \(index)\n"], message: "Notes \(index)\n", time: insightNow - (40 - index) * 86400 + 60)
        }
        try repo.commit(["lib/score.swift": nil], message: "Remove\n", time: insightNow - 86400)
        let source = try repo.source()
        let versions = try source.trendVersions("lib/score.swift")
        let order = Array(touched.reversed())
        let picks = Insight.trendPicks(commitCount: 21)
        XCTAssertEqual(versions.map(\.commit), picks.compactMap { $0 == 0 ? nil : order[$0 - 1].hex })
        XCTAssertEqual(versions.first?.commit, touched.first?.hex)
        let points = try source.trend("lib/score.swift")
        XCTAssertEqual(points.count, versions.count)
        XCTAssertEqual(points.first?.complexity, 2)
        XCTAssertEqual(points.last?.complexity, 21)
        XCTAssertTrue(zip(points, points.dropFirst()).allSatisfy { $0.time <= $1.time })
        let git = try InsightTestGit.require()
        let logged = try git.run(["log", "--no-merges", "-n", "400", "--format=%H", "--", "lib/score.swift"], in: repo.root)
            .split(separator: "\n").map(String.init)
        XCTAssertEqual(logged.count, 21)
        XCTAssertEqual(versions.map(\.commit), Insight.trendPicks(commitCount: logged.count).map { logged[$0] }.filter { $0 != logged[0] })
    }

    func testSubjectJoinsTheFirstParagraph() {
        XCTAssertEqual(GitInsightSource.subject(Array("\n\nFirst line\nsecond line\n\nBody\n".utf8)), "First line second line")
        XCTAssertEqual(GitInsightSource.subject(Array("Only".utf8)), "Only")
        XCTAssertEqual(GitInsightSource.subject([]), "")
    }
}
