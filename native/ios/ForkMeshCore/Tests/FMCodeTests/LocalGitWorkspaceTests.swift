import FMAccount
import FMGit
import Foundation
import XCTest
@testable import FMCode

final class LocalGitWorkspaceTests: XCTestCase {
    func testGitAuthorKeepsUtf8ButStripsAngleBracketsAndControls() throws {
        let author = GitAuthor.fromAccount(nodeName: "ålice <x>", email: "alïce@example.com")
        XCTAssertEqual(author.name, "ålice x")
        XCTAssertEqual(author.email, "alïce@example.com")
        XCTAssertNoThrow(try author.toIdentity(Date()))
        let fallback = GitAuthor.fromAccount(nodeName: "", email: "")
        XCTAssertEqual(fallback.name, "forkmesh")
        XCTAssertEqual(fallback.email, "forkmesh@mobile.forkmesh")
        XCTAssertNoThrow(try fallback.toIdentity(Date()))
        XCTAssertEqual(GitAuthor.fromAccount(nodeName: "bob\u{7}", email: "no-at").email, "bob@mobile.forkmesh")
    }

    func testRoundTripInitWriteStageCommitBranchCheckoutLog() async throws {
        let temp = try localTempDirectory(self, "fm-git-workspace")
        let worktree = temp.appendingPathComponent("demo")
        let workspace = GitWorkspace(worktree: worktree)
        defer { workspace.dispose() }

        let first = try await workspace.initialize(
            files: ["README.md": "# demo\n"], commitMessage: "Initial commit\n", author: localTestAuthor)
        XCTAssertEqual(first?.count, 40)

        // Verify the commit with the engine's own readers.
        let repo = try GitRepository.open(GitRepositoryOpen(worktree.path))
        let head = try XCTUnwrap(try repo.readHeadObjectId())
        XCTAssertEqual(head.hex, first)
        let commit = try repo.readCommit(head)
        XCTAssertEqual(String(decoding: commit.message, as: UTF8.self), "Initial commit\n")
        XCTAssertTrue(commit.parents.isEmpty)
        let tree = try repo.readTree(commit.tree)
        XCTAssertEqual(tree.entries.map(\.nameText), ["README.md"])
        XCTAssertEqual(String(decoding: try repo.readBlob(tree.entries[0].objectId), as: UTF8.self), "# demo\n")

        // Worktree listing + status after an edit and a new file.
        try await workspace.writeFile("README.md", "# demo\n\nHello.\n")
        try await workspace.writeFile("src/main.dart", "void main() {}\n")
        let files = try await workspace.listFiles()
        XCTAssertEqual(files.map(\.path), ["src", "README.md"])
        XCTAssertTrue(files[0].isDirectory)
        XCTAssertFalse(files[0].tracked)
        XCTAssertTrue(files[1].tracked)
        let nested = try await workspace.listFiles("src")
        XCTAssertEqual(nested.map(\.path), ["src/main.dart"])

        var status = try await workspace.status()
        XCTAssertTrue(status.staged.isEmpty)
        XCTAssertEqual(status.unstaged, [LocalChange(path: "README.md", kind: .modified, staged: false)])
        XCTAssertEqual(status.untracked.map(\.path), ["src/main.dart"])

        // Stage one, unstage it, then stage all.
        try await workspace.stage(["README.md"])
        status = try await workspace.status()
        XCTAssertEqual(status.staged.map(\.path), ["README.md"])
        XCTAssertEqual(status.staged.first?.kind, .modified)
        XCTAssertTrue(status.unstaged.isEmpty)
        try await workspace.unstage(["README.md"])
        status = try await workspace.status()
        XCTAssertTrue(status.staged.isEmpty)
        XCTAssertEqual(status.unstaged.map(\.path), ["README.md"])
        try await workspace.stageAll()
        status = try await workspace.status()
        XCTAssertEqual(status.staged.map(\.path), ["README.md", "src/main.dart"])
        XCTAssertFalse(status.hasWorktreeChanges)

        // Commit must refuse an empty message and succeed otherwise.
        await localAssertThrows(try await workspace.commit(message: "   ", author: localTestAuthor)) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.message, "Enter a commit message.")
        }
        let second = try await workspace.commit(message: "Add main", author: localTestAuthor)
        XCTAssertNotEqual(second, first)
        let clean = try await workspace.status()
        XCTAssertTrue(clean.isClean)

        // Branches: create, list, checkout, and commit on the new branch.
        var branches = try await workspace.branches()
        XCTAssertEqual(branches.map(\.name), ["main"])
        XCTAssertTrue(branches[0].isCurrent)
        XCTAssertEqual(branches[0].tipId, second)
        try await workspace.createBranch("feature/x")
        branches = try await workspace.branches()
        XCTAssertEqual(branches.map(\.name), ["feature/x", "main"])
        XCTAssertEqual(branches.first { $0.name == "main" }?.isCurrent, true)
        try await workspace.checkout("feature/x")
        branches = try await workspace.branches()
        XCTAssertEqual(branches.first { $0.name == "feature/x" }?.isCurrent, true)
        try await workspace.writeFile("src/main.dart", "void main() { print(1); }\n")
        try await workspace.stage(["src/main.dart"])
        let third = try await workspace.commit(message: "Feature work\n\nBody line.\n", author: localTestAuthor)

        // Dirty-worktree guard on checkout maps to a user-facing message.
        try await workspace.writeFile("README.md", "dirty\n")
        await localAssertThrows(try await workspace.checkout("main")) { error in
            let failure = error as? GitWorkspaceException
            XCTAssertEqual(failure?.code, .operationInterrupted)
            XCTAssertTrue(failure?.message.contains("Commit or discard") ?? false, "\(error)")
        }
        try await workspace.discard(["README.md"])
        let afterDiscard = try await workspace.status()
        XCTAssertTrue(afterDiscard.isClean)
        try await workspace.checkout("main")
        let mainFile = try await workspace.readFile("src/main.dart")
        XCTAssertEqual(mainFile.text, "void main() {}\n")
        try await workspace.checkout("feature/x")

        // History walks newest first with parents.
        let log = try await workspace.log()
        XCTAssertEqual(log.map(\.id), [third, second, first!])
        XCTAssertEqual(log[0].subject, "Feature work")
        XCTAssertEqual(log[0].message, "Feature work\n\nBody line.\n")
        XCTAssertEqual(log[0].authorName, "Alice Mobile")
        XCTAssertEqual(log[0].authorEmail, "alice@example.com")
        XCTAssertEqual(log[0].parents, [second])
        XCTAssertTrue(log[2].parents.isEmpty)
        XCTAssertLessThan(abs(log[0].time.timeIntervalSinceNow), 300)
        let limited = try await workspace.log(limit: 2)
        XCTAssertEqual(limited.count, 2)

        let summary = try await workspace.summary()
        XCTAssertEqual(summary.branch, "feature/x")
        XCTAssertEqual(summary.headId, third)
        XCTAssertEqual(summary.lastCommitSubject, "Feature work")

        // Engine readers agree on the final state.
        let reopened = try GitRepository.open(GitRepositoryOpen(worktree.path))
        XCTAssertEqual(try reopened.readHead().target, "refs/heads/feature/x")
        XCTAssertEqual(try reopened.readHeadObjectId()?.hex, third)
        XCTAssertTrue(try reopened.status().isClean)
    }

    func testReadFileFlagsBinaryContent() async throws {
        let worktree = try localTempDirectory(self, "fm-bin").appendingPathComponent("bin")
        let workspace = GitWorkspace(worktree: worktree)
        defer { workspace.dispose() }
        try await workspace.initialize()
        try Data([0, 1, 2, 3]).write(to: worktree.appendingPathComponent("blob.bin"))
        let content = try await workspace.readFile("blob.bin")
        XCTAssertTrue(content.isBinary)
        XCTAssertFalse(content.isEditable)
        XCTAssertEqual(content.bytes, 4)
        XCTAssertEqual(content.path, "blob.bin")
        // Invalid UTF-8 without NUL is binary too.
        try Data([0xC3, 0x28]).write(to: worktree.appendingPathComponent("latin.txt"))
        let latin = try await workspace.readFile("latin.txt")
        XCTAssertTrue(latin.isBinary)
    }

    func testRejectsUnsafePathsAndGitWrites() async throws {
        let worktree = try localTempDirectory(self, "fm-safe").appendingPathComponent("safe")
        let workspace = GitWorkspace(worktree: worktree)
        defer { workspace.dispose() }
        try await workspace.initialize()
        await localAssertThrows(try await workspace.writeFile("../escape.txt", "x")) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.code, .pathEscapesWorktree)
        }
        await localAssertThrows(try await workspace.writeFile(".git/config", "x")) { error in
            XCTAssertTrue(error is GitWorkspaceException)
        }
        await localAssertThrows(try await workspace.listFiles("nope")) { error in
            XCTAssertTrue(error is GitWorkspaceException)
        }
    }

    func testImportFromDirectoryCopiesFilesSkipsSymlinksAndGit() async throws {
        let temp = try localTempDirectory(self, "fm-import")
        let source = temp.appendingPathComponent("source")
        try localWrite(source.appendingPathComponent("a.txt"), "A")
        try localWrite(source.appendingPathComponent("lib/b.dart"), "B")
        try localWrite(source.appendingPathComponent(".git/HEAD"), "ref: refs/heads/x\n")
        try FileManager.default.createSymbolicLink(
            atPath: source.appendingPathComponent("link").path, withDestinationPath: "a.txt")

        let worktree = temp.appendingPathComponent("imported")
        let workspace = GitWorkspace(worktree: worktree)
        defer { workspace.dispose() }
        let result = try await workspace.importFromDirectory(source, author: localTestAuthor, message: "Import\n")
        XCTAssertEqual(result.importedFiles, 2)
        XCTAssertEqual(result.skippedEntries, 2)
        XCTAssertNil(try? FileManager.default.destinationOfSymbolicLink(atPath: worktree.appendingPathComponent("link").path))
        XCTAssertEqual(try localRead(worktree.appendingPathComponent(".git/HEAD")), "ref: refs/heads/main\n")
        let log = try await workspace.log()
        XCTAssertEqual(log.map(\.subject), ["Import"])
        let status = try await workspace.status()
        XCTAssertTrue(status.isClean)
        let files = try await workspace.listFiles()
        XCTAssertEqual(files.map(\.path), ["lib", "a.txt"])
    }

    func testImportFromDirectoryRefusesAMissingFolder() async throws {
        let temp = try localTempDirectory(self, "fm-import-missing")
        let workspace = GitWorkspace(worktree: temp.appendingPathComponent("w"))
        defer { workspace.dispose() }
        await localAssertThrows(
            try await workspace.importFromDirectory(temp.appendingPathComponent("nope"), author: localTestAuthor)
        ) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.message, "The selected folder could not be read.")
        }
    }

    func testImportFromZipStripsASharedRootFolder() async throws {
        let temp = try localTempDirectory(self, "fm-zip")
        let zip = temp.appendingPathComponent("proj.zip")
        let bytes = try LocalTestZip.build([
            .init(name: "proj/README.md", bytes: Array("# p\n".utf8)),
            .init(name: "proj/src/x.txt", bytes: Array(String(repeating: "x", count: 500).utf8), deflate: true),
            .init(name: "__MACOSX/proj/._README.md", bytes: Array("junk".utf8)),
        ])
        try Data(bytes).write(to: zip)

        let worktree = temp.appendingPathComponent("fromzip")
        let workspace = GitWorkspace(worktree: worktree)
        defer { workspace.dispose() }
        let result = try await workspace.importFromZip(zip.path, author: localTestAuthor)
        XCTAssertEqual(result.importedFiles, 2)
        let files = try await workspace.listFiles()
        XCTAssertEqual(files.map(\.path), ["src", "README.md"])
        let readme = try await workspace.readFile("README.md")
        XCTAssertEqual(readme.text, "# p\n")
        let deflated = try await workspace.readFile("src/x.txt")
        XCTAssertEqual(deflated.text, String(repeating: "x", count: 500))
    }

    func testImportFromZipSkipsSymlinksAndKeepsDistinctRoots() async throws {
        let temp = try localTempDirectory(self, "fm-zip2")
        let zip = temp.appendingPathComponent("mixed.zip")
        try Data(try LocalTestZip.build([
            .init(name: "a.txt", bytes: Array("a".utf8)),
            .init(name: "dir/b.txt", bytes: Array("b".utf8)),
            .init(name: "dir/link", bytes: Array("b.txt".utf8), unixMode: 0o120777),
            .init(name: ".git/config", bytes: Array("x".utf8)),
        ])).write(to: zip)
        let workspace = GitWorkspace(worktree: temp.appendingPathComponent("w"))
        defer { workspace.dispose() }
        let result = try await workspace.importFromZip(zip.path, author: localTestAuthor)
        XCTAssertEqual(result.importedFiles, 2)
        XCTAssertEqual(result.skippedEntries, 1)
        let files = try await workspace.listFiles()
        XCTAssertEqual(files.map(\.path), ["dir", "a.txt"])
    }

    func testImportFromZipRejectsGarbage() async throws {
        let temp = try localTempDirectory(self, "fm-zip3")
        let zip = temp.appendingPathComponent("bad.zip")
        try Data("not a zip".utf8).write(to: zip)
        let workspace = GitWorkspace(worktree: temp.appendingPathComponent("w"))
        defer { workspace.dispose() }
        await localAssertThrows(try await workspace.importFromZip(zip.path, author: localTestAuthor)) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.message, "The archive is not a valid zip file.")
        }
    }

    func testCancelSurfacesACancelledErrorAndLeavesTheWorkspaceUsable() async throws {
        let worktree = try localTempDirectory(self, "fm-cancel").appendingPathComponent("cancel")
        let workspace = GitWorkspace(worktree: worktree)
        defer { workspace.dispose() }
        try await workspace.initialize()
        let payload = Data(String(repeating: "x", count: 2000).utf8)
        for index in 0..<400 {
            try payload.write(to: worktree.appendingPathComponent("f\(index).txt"))
        }
        let pending = Task { try await workspace.status() }
        // Wait until the operation is actually in flight.
        for _ in 0..<200 where !workspace.isBusy { try await Task.sleep(nanoseconds: 1_000_000) }
        workspace.cancel()
        do {
            _ = try await pending.value
        } catch let error as GitWorkspaceException {
            XCTAssertEqual(error.code, .cancelled)
        }
        XCTAssertFalse(workspace.isBusy)
        let status = try await workspace.status()
        XCTAssertEqual(status.untracked.count, 400)
    }

    func testRunnerRecordsDurationsAndSerializesOperations() async throws {
        let names = LockedNames()
        let runner = GitOperationRunner(onDuration: { name, _, _ in names.append(name) })
        let worktree = try localTempDirectory(self, "fm-perf").appendingPathComponent("perf")
        let workspace = GitWorkspace(worktree: worktree, runner: runner)
        defer { workspace.dispose() }
        // Queued in order; each waits for the previous one.
        async let a: String? = workspace.initialize()
        try await Task.sleep(nanoseconds: 5_000_000)
        async let b: LocalStatus = workspace.status()
        try await Task.sleep(nanoseconds: 5_000_000)
        async let c: [LocalBranch] = workspace.branches()
        _ = try await (a, b, c)
        XCTAssertEqual(names.values, ["init", "status", "branches"])
    }

    func testMonitorHookReceivesGitOperationEvents() async throws {
        let monitor = PerformanceMonitorService(enabled: true, threshold: .zero, logSink: { _ in })
        let worktree = try localTempDirectory(self, "fm-mon").appendingPathComponent("mon")
        let workspace = GitWorkspace(worktree: worktree, performanceMonitor: monitor)
        defer { workspace.dispose() }
        try await workspace.initialize()
        XCTAssertTrue(monitor.events.map(\.name).contains("git.init"))
    }

    func testDisposedWorkspaceRefusesWork() async throws {
        let worktree = try localTempDirectory(self, "fm-disposed").appendingPathComponent("w")
        let workspace = GitWorkspace(worktree: worktree)
        workspace.dispose()
        await localAssertThrows(try await workspace.initialize()) { error in
            XCTAssertEqual((error as? GitWorkspaceException)?.code, .cancelled)
        }
    }

    func testTransferProgressLabelsAndSyncStateLabels() {
        XCTAssertEqual(GitTransferProgress(phase: "receivingPack", bytes: 2048).label, "Downloading 2 KB")
        XCTAssertEqual(GitTransferProgress(phase: "sendingPack", bytes: 3 * 1024 * 1024 / 2).label, "Uploading 1.5 MB")
        XCTAssertEqual(GitTransferProgress(phase: "receivingPack").label, "Downloading")
        XCTAssertEqual(GitTransferProgress(phase: "indexingPack", objects: 12).label, "Indexing 12 objects")
        XCTAssertEqual(GitTransferProgress(phase: "mystery").label, "Working")
        XCTAssertEqual(LocalSyncState.unknown("main").label, "Not pushed yet")
        XCTAssertEqual(LocalSyncState(branch: "m", localTip: "a", remoteTip: "a", ahead: 0, behind: 0).label, "In sync with ForkMesh")
        XCTAssertEqual(LocalSyncState(branch: "m", localTip: "a", remoteTip: "b", ahead: 2, behind: 1).label, "2 to push · 1 to fetch")
        XCTAssertEqual(LocalSyncState(branch: "m", localTip: "a", remoteTip: "b", ahead: nil, behind: 0).label, "500+ to push")
        XCTAssertEqual(LocalCommit(id: "0123456789", parents: [], subject: "", message: "", authorName: "", authorEmail: "", time: Date()).shortId, "0123456")
    }

    func testRepositoryCreatedByTheAppPassesGitFsckStrict() async throws {
        let git = try LocalTestGit.require()
        let worktree = try localTempDirectory(self, "fm-interop").appendingPathComponent("interop")
        let workspace = GitWorkspace(worktree: worktree)
        defer { workspace.dispose() }
        try await workspace.initialize(
            files: ["README.md": "# interop\n", "docs/guide.md": "guide\n"],
            commitMessage: "Initial commit\n", author: localTestAuthor)
        try await workspace.writeFile("docs/guide.md", "guide v2\n")
        try await workspace.stageAll()
        _ = try await workspace.commit(message: "Update guide", author: localTestAuthor)
        try await workspace.createBranch("topic")

        let fsck = try git.run(["-C", worktree.path, "fsck", "--strict", "--no-dangling"])
        XCTAssertEqual(fsck.status, 0, "\(fsck.out)\n\(fsck.err)")
        let log = try git.run(["-C", worktree.path, "log", "--format=%s"])
        XCTAssertEqual(log.out.split(separator: "\n").map(String.init), ["Update guide", "Initial commit"])
        let branches = try git.run(["-C", worktree.path, "branch", "--format=%(refname:short)"])
        XCTAssertTrue(Set(branches.out.split(separator: "\n").map(String.init)).isSuperset(of: ["main", "topic"]))
        let status = try git.run(["-C", worktree.path, "status", "--porcelain"])
        XCTAssertEqual(status.status, 0, status.err)
        XCTAssertEqual(status.out.trimmingCharacters(in: .whitespacesAndNewlines), "", "index must agree")
    }
}

final class LockedNames: @unchecked Sendable {
    private let lock = NSLock()
    private var names: [String] = []
    func append(_ name: String) {
        lock.lock()
        names.append(name)
        lock.unlock()
    }
    var values: [String] {
        lock.lock()
        defer { lock.unlock() }
        return names
    }
}
