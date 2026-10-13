import FMBase
import FMGit
import Foundation
import XCTest
@testable import FMCode

private func repoRecord(publishedAs: String? = "alice/proj") -> LocalRepoRecord {
    LocalRepoRecord(name: "proj", path: "/tmp/proj", createdAt: Date(epochMillis: 1_767_225_600_000), publishedAs: publishedAs)
}

/// Records what the service would have sent.
@MainActor
private final class RecordingTransport {
    var pushedBranches: [String] = []
    var submitted: [[String: String]] = []
    var rejectPush = false

    var service: PullRequestService {
        PullRequestService(
            pushBranch: { [self] _, _, _, branch in
                pushedBranches.append(branch)
                return LocalPushResult(
                    refs: [LocalPushRef(
                        remoteRef: "refs/heads/\(branch)", newId: String(repeating: "a", count: 40),
                        outcome: rejectPush ? "rejected" : "updated")],
                    packBytes: 10, objects: 1)
            },
            submitPull: { [self] owner, name, title, base, head, patch in
                submitted.append(["owner": owner, "repo": name, "title": title, "base": base, "head": head, "patch": patch])
            })
    }
}

@MainActor
final class LocalPullRequestServiceTests: XCTestCase {
    private func repoWithBranch(
        baseFiles: [String: String],
        branchEdits: [String: String],
        branch: String = "agent/work",
        baseMovesOnWith: [String: String]? = nil
    ) async throws -> GitWorkspace {
        let worktree = try localTempDirectory(self, "fm-pr").appendingPathComponent("repo")
        let workspace = GitWorkspace(worktree: worktree)
        addTeardownBlock { workspace.dispose() }
        try await workspace.initialize(files: baseFiles, commitMessage: "base\n", author: localTestAuthor)
        let baseName = try await workspace.branches().first { $0.isCurrent }!.name
        try await workspace.createBranch(branch, checkout: true)
        for (path, text) in branchEdits.sorted(by: { $0.key < $1.key }) { try await workspace.writeFile(path, text) }
        try await workspace.stageAll()
        _ = try await workspace.commit(message: "agent work\n", author: localTestAuthor)
        if let baseMovesOnWith {
            // Someone else commits to the base branch while the agent works.
            try await workspace.checkout(baseName)
            for (path, text) in baseMovesOnWith { try await workspace.writeFile(path, text) }
            try await workspace.stageAll()
            _ = try await workspace.commit(message: "meanwhile\n", author: localTestAuthor)
            try await workspace.checkout(branch)
        }
        return workspace
    }

    private func baseBranchOf(_ workspace: GitWorkspace) async throws -> String {
        try await workspace.branches().first { !$0.name.hasPrefix("agent/") }!.name
    }

    func testABranchWithCommitsBecomesASubmittablePatch() async throws {
        let workspace = try await repoWithBranch(baseFiles: ["a.txt": "one\ntwo\nthree\n"], branchEdits: ["a.txt": "one\nTWO\nthree\n"])
        let prepared = try await RecordingTransport().service.prepare(
            workspace: workspace, record: repoRecord(), headBranch: "agent/work", baseBranch: try await baseBranchOf(workspace))
        XCTAssertTrue(prepared.isReady, prepared.message)
        XCTAssertEqual(prepared.owner, "alice")
        XCTAssertEqual(prepared.repo, "proj")
        XCTAssertEqual(prepared.changedPaths, ["a.txt"])
        XCTAssertTrue(prepared.patch.contains("-two"))
        XCTAssertTrue(prepared.patch.contains("+TWO"))
        XCTAssertTrue(prepared.summary.contains("1 file"))
    }

    func testTheDiffStartsWhereTheBranchesDiverged() async throws {
        let workspace = try await repoWithBranch(
            baseFiles: ["a.txt": "one\n"], branchEdits: ["a.txt": "one changed\n"],
            baseMovesOnWith: ["unrelated.txt": "added by someone else\n"])
        let prepared = try await RecordingTransport().service.prepare(
            workspace: workspace, record: repoRecord(), headBranch: "agent/work", baseBranch: try await baseBranchOf(workspace))
        XCTAssertTrue(prepared.isReady)
        XCTAssertEqual(prepared.changedPaths, ["a.txt"])
        // A tip-to-tip diff would try to delete this.
        XCTAssertFalse(prepared.patch.contains("unrelated.txt"))
    }

    func testALocalOnlyRepositoryHasNowhereToSendAPullRequest() async throws {
        let workspace = try await repoWithBranch(baseFiles: ["a.txt": "one\n"], branchEdits: ["a.txt": "two\n"])
        let prepared = try await RecordingTransport().service.prepare(
            workspace: workspace, record: repoRecord(publishedAs: nil), headBranch: "agent/work",
            baseBranch: try await baseBranchOf(workspace))
        XCTAssertEqual(prepared.blocker, .notPublished)
        XCTAssertTrue(prepared.message.contains("Publish it to ForkMesh"))
    }

    func testABranchWithNoNewCommitsIsRefused() async throws {
        let worktree = try localTempDirectory(self, "fm-pr-empty").appendingPathComponent("repo")
        let workspace = GitWorkspace(worktree: worktree)
        defer { workspace.dispose() }
        try await workspace.initialize(files: ["a.txt": "one\n"], commitMessage: "base\n", author: localTestAuthor)
        let base = try await workspace.branches().first { $0.isCurrent }!.name
        try await workspace.createBranch("agent/empty", checkout: true)
        let prepared = try await RecordingTransport().service.prepare(
            workspace: workspace, record: repoRecord(), headBranch: "agent/empty", baseBranch: base)
        XCTAssertEqual(prepared.blocker, .noCommits)
    }

    func testAnUnknownBranchIsNamedInTheRefusal() async throws {
        let workspace = try await repoWithBranch(baseFiles: ["a.txt": "one\n"], branchEdits: ["a.txt": "two\n"])
        let prepared = try await RecordingTransport().service.prepare(
            workspace: workspace, record: repoRecord(), headBranch: "agent/does-not-exist",
            baseBranch: try await baseBranchOf(workspace))
        XCTAssertEqual(prepared.blocker, .missingBranch)
        XCTAssertTrue(prepared.message.contains("agent/does-not-exist"))
    }

    func testABinaryChangeIsRefusedWithThePathsAndAWayForward() async throws {
        let workspace = try await repoWithBranch(
            baseFiles: ["a.txt": "one\n"], branchEdits: ["a.txt": "two\n", "logo.png": "PNG\u{0}binary"])
        let prepared = try await RecordingTransport().service.prepare(
            workspace: workspace, record: repoRecord(), headBranch: "agent/work", baseBranch: try await baseBranchOf(workspace))
        XCTAssertEqual(prepared.blocker, .binaryChanges)
        XCTAssertEqual(prepared.binaryPaths, ["logo.png"])
        XCTAssertTrue(prepared.message.contains("logo.png"))
        XCTAssertTrue(prepared.message.contains("safe on agent/work"))
        XCTAssertTrue(prepared.message.contains("desktop"))
    }

    func testPushesTheBranchBeforeOpeningThePullRequest() async throws {
        let workspace = try await repoWithBranch(baseFiles: ["a.txt": "one\n"], branchEdits: ["a.txt": "two\n"])
        let transport = RecordingTransport()
        let service = transport.service
        let prepared = try await service.prepare(
            workspace: workspace, record: repoRecord(), headBranch: "agent/work", baseBranch: try await baseBranchOf(workspace))
        var stages: [String] = []
        try await service.submit(workspace: workspace, preparation: prepared, title: "Fix the thing") { stages.append($0) }
        XCTAssertEqual(transport.pushedBranches, ["agent/work"])
        XCTAssertEqual(transport.submitted.count, 1)
        let sent = transport.submitted[0]
        XCTAssertEqual(sent["owner"], "alice")
        XCTAssertEqual(sent["title"], "Fix the thing")
        XCTAssertEqual(sent["head"], "agent/work")
        XCTAssertTrue(sent["patch"]?.contains("diff --git") ?? false)
        XCTAssertTrue(stages.first?.contains("Pushing") ?? false)
    }

    func testARejectedPushDoesNotOpenAPullRequest() async throws {
        let workspace = try await repoWithBranch(baseFiles: ["a.txt": "one\n"], branchEdits: ["a.txt": "two\n"])
        let transport = RecordingTransport()
        transport.rejectPush = true
        let service = transport.service
        let prepared = try await service.prepare(
            workspace: workspace, record: repoRecord(), headBranch: "agent/work", baseBranch: try await baseBranchOf(workspace))
        await localAssertThrows(try await service.submit(workspace: workspace, preparation: prepared, title: "nope")) { error in
            XCTAssertTrue(error is PullRequestSubmitError)
        }
        XCTAssertTrue(transport.submitted.isEmpty)
    }

    func testAnEmptyTitleFallsBackToSomethingMeaningful() async throws {
        let workspace = try await repoWithBranch(baseFiles: ["a.txt": "one\n"], branchEdits: ["a.txt": "two\n"])
        let transport = RecordingTransport()
        let service = transport.service
        let prepared = try await service.prepare(
            workspace: workspace, record: repoRecord(), headBranch: "agent/work", baseBranch: try await baseBranchOf(workspace))
        try await service.submit(workspace: workspace, preparation: prepared, title: "   ")
        XCTAssertEqual(transport.submitted.first?["title"], "Changes from agent/work")
    }

    func testABlockedPreparationCannotBeSubmitted() async throws {
        let workspace = try await repoWithBranch(baseFiles: ["a.txt": "one\n"], branchEdits: ["a.txt": "two\n"])
        await localAssertThrows(try await RecordingTransport().service.submit(
            workspace: workspace, preparation: .blocked(.noChanges, "nothing to send"), title: "x")
        ) { error in
            XCTAssertTrue(error is PullRequestSubmitError)
        }
    }

    // MARK: LocalPatch.forFile

    func testForFileWritesAModelessIndexLineWithRealBlobIds() {
        let before = Array("one\ntwo\n".utf8)
        let after = "one\nTWO\n"
        let oldId = GitBlobId.of(before)
        let newId = GitBlobId.of(Array(after.utf8))
        let result = LocalPatch.forFile(path: "a.txt", beforeBytes: before, afterText: after, oldId: oldId, newId: newId)
        XCTAssertEqual(result.changedPaths, ["a.txt"])
        XCTAssertEqual(result.patch, """
            diff --git a/a.txt b/a.txt
            index \(oldId.hex)..\(newId.hex)
            --- a/a.txt
            +++ b/a.txt
            @@ -1,2 +1,2 @@
             one
            -two
            +TWO

            """)
        let same = LocalPatch.forFile(path: "a.txt", beforeBytes: before, afterText: "one\ntwo\n", oldId: oldId, newId: oldId)
        XCTAssertTrue(same.isEmpty)
        XCTAssertEqual(LocalPatch.forFile(path: "b", beforeBytes: [0x68, 0xff], afterText: "h", oldId: oldId, newId: newId).binaryPaths, ["b"])
        XCTAssertEqual(LocalPatch.forFile(path: "c", beforeBytes: [0, 1], afterText: "h", oldId: oldId, newId: newId).binaryPaths, ["c"])
    }

    func testForFilePatchAppliesWithRealGit() async throws {
        let git = try LocalTestGit.require()
        let dir = try localTempDirectory(self, "fm-forfile")
        XCTAssertEqual(try git.run(["init", "-q", dir.path]).status, 0)
        let before = "alpha\nbeta\ngamma\n"
        try localWrite(dir.appendingPathComponent("f.txt"), before)
        let after = "alpha\nBETA\ngamma\ndelta\n"
        let result = LocalPatch.forFile(
            path: "f.txt", beforeBytes: Array(before.utf8), afterText: after,
            oldId: GitBlobId.of(Array(before.utf8)), newId: GitBlobId.of(Array(after.utf8)))
        let patch = dir.appendingPathComponent("change.patch")
        try Data(result.patch.utf8).write(to: patch)
        let applied = try git.run(["-C", dir.path, "apply", "--check", patch.path])
        XCTAssertEqual(applied.status, 0, applied.err)
    }
}
