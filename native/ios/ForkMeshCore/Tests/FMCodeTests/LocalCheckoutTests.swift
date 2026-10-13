import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMGit
import FMNet
import Foundation
import XCTest
@testable import FMCode

private let checkoutOrigin = "https://forkmesh.test"

private func checkoutCredentials() -> ForkMeshRemoteCredentials {
    ForkMeshRemoteCredentials(origin: checkoutOrigin, owner: "forkmesh", repo: "demo")
}

private func commitAll(_ origin: LocalGitOrigin, _ message: String) throws {
    try LocalGitOrigin.check(origin.git.run(["-C", origin.work, "add", "-A"]))
    try LocalGitOrigin.check(origin.git.run(["-C", origin.work, "commit", "-q", "-m", message]))
    try origin.publish("main")
}

final class LocalProgressLabels: @unchecked Sendable {
    private let lock = NSLock()
    private var labels = [String]()

    func add(_ label: String) {
        lock.lock()
        labels.append(label)
        lock.unlock()
    }

    var all: [String] {
        lock.lock()
        defer { lock.unlock() }
        return labels
    }
}

final class LocalCheckoutTests: XCTestCase {
    private func workspace(_ server: LocalGitSmartServer) throws -> GitWorkspace {
        let directory = try localTempDirectory(self, "fm-checkout").appendingPathComponent("demo")
        let workspace = GitWorkspace(worktree: directory)
        workspace.transportFactory = { server }
        addTeardownBlock { workspace.dispose() }
        return workspace
    }

    func testACloneReportsIndexingAndCheckoutCounts() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 1)
        for index in 0..<70 {
            try localWrite(URL(fileURLWithPath: origin.work).appendingPathComponent("src/file\(index).txt"), "file \(index)\n")
        }
        try commitAll(origin, "Many files")
        let server = LocalGitSmartServer(git: origin.git)
        server.serve("\(checkoutOrigin)/forkmesh/demo", origin.bare)
        let workspace = try workspace(server)
        let labels = LocalProgressLabels()

        let result = try await workspace.clone(credentials: checkoutCredentials()) { progress in labels.add(progress.label) }

        XCTAssertEqual(result.branch, "main")
        XCTAssertTrue(labels.all.contains("Indexing \(result.objects) objects"), "\(labels.all)")
        XCTAssertTrue(labels.all.contains("Checking out 64 of 71 files"), "\(labels.all)")
        XCTAssertFalse(labels.all.contains("Indexing 0 objects"))
        let status = try await workspace.status()
        XCTAssertTrue(status.isClean)
        let file = try await workspace.readFile("src/file7.txt")
        XCTAssertEqual(file.text, "file 7\n")
    }

    func testSyncRewritesOnlyChangedFilesAndPrunesEmptiedFolders() async throws {
        let origin = try LocalGitOrigin.make(self, commits: 1)
        try localWrite(URL(fileURLWithPath: origin.work).appendingPathComponent("docs/guide.md"), "guide\n")
        try localWrite(URL(fileURLWithPath: origin.work).appendingPathComponent("README.md"), "readme\n")
        try commitAll(origin, "Docs")
        let server = LocalGitSmartServer(git: origin.git)
        server.serve("\(checkoutOrigin)/forkmesh/demo", origin.bare)
        let workspace = try workspace(server)
        _ = try await workspace.clone(credentials: checkoutCredentials())
        let readme = workspace.worktree.appendingPathComponent("README.md").path
        let old = Date(timeIntervalSince1970: 1_000_000_000)
        try FileManager.default.setAttributes([.modificationDate: old], ofItemAtPath: readme)
        try FileManager.default.removeItem(atPath: "\(origin.work)/docs/guide.md")
        try localWrite(URL(fileURLWithPath: origin.work).appendingPathComponent("step.txt"), "synced\n")
        try commitAll(origin, "Drop the guide")
        let labels = LocalProgressLabels()

        let result = try await workspace.pull(credentials: checkoutCredentials(), branch: "main") { progress in
            labels.add(progress.label)
        }

        guard case .fastForwarded = result.outcome else { return XCTFail("\(result.outcome)") }
        XCTAssertEqual(try localRead(workspace.worktree.appendingPathComponent("step.txt")), "synced\n")
        XCTAssertEqual(EngineFS.entityType(workspace.worktree.appendingPathComponent("docs").path), .notFound)
        let modified = try FileManager.default.attributesOfItem(atPath: readme)[.modificationDate] as? Date
        XCTAssertEqual(modified, old)
        XCTAssertTrue(labels.all.contains("Checking out 1 file"), "\(labels.all)")
        let status = try await workspace.status()
        XCTAssertTrue(status.isClean)
    }

    func testProgressLabelsCountObjectsAndFiles() {
        XCTAssertEqual(GitTransferProgress(phase: "indexingPack", objects: 5, total: 10).label, "Indexing 5 of 10 objects")
        XCTAssertEqual(GitTransferProgress(phase: "indexingPack", objects: 10, total: 10).label, "Indexing 10 objects")
        XCTAssertEqual(GitTransferProgress(phase: "indexingPack", total: 10).label, "Indexing 10 objects")
        XCTAssertEqual(GitTransferProgress(phase: "indexingPack", objects: 12).label, "Indexing 12 objects")
        XCTAssertEqual(GitTransferProgress(phase: "indexingPack", total: 1).label, "Indexing 1 object")
        XCTAssertEqual(GitTransferProgress(phase: "indexingPack").label, "Indexing objects")
        XCTAssertEqual(GitTransferProgress(phase: "checkingOut", objects: 3, total: 9).label, "Checking out 3 of 9 files")
        XCTAssertEqual(GitTransferProgress(phase: "checkingOut", total: 1).label, "Checking out 1 file")
        XCTAssertEqual(GitTransferProgress(phase: "checkingOut").label, "Checking out files")
    }
}
