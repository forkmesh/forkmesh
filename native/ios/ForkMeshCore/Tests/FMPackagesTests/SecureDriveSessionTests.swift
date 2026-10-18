import FMBase
import Foundation
import XCTest
@testable import FMPackages

final class SecureDriveSessionTests: XCTestCase {
    func testServiceRunsTheDriveOffTheCaller() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("secure-drive-test-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer {
            if root.lastPathComponent.hasPrefix("secure-drive-test-") && root.path.hasPrefix(FileManager.default.temporaryDirectory.path) {
                try? FileManager.default.removeItem(at: root)
            }
        }
        let kdf = SecureDriveKdfCost(memoryKiB: 8192, passes: 1)
        let timing = SecureDriveLeaseTiming(settleMs: 0, staleMs: 300, pollMs: 1)
        let alice = SecureDriveService(directory: root.appendingPathComponent("alice"), testingKdf: kdf, timing: timing)
        let bob = SecureDriveService(directory: root.appendingPathComponent("bob"), testingKdf: kdf, timing: timing)
        let exists = try await alice.run { $0.exists }
        XCTAssertFalse(exists)
        try await alice.run { try $0.setup(name: "Laptop", passphrase: "correct horse battery") }
        try await bob.run { try $0.setup(name: "Phone", passphrase: "correct horse battery") }
        let container = root.appendingPathComponent("Shared.fmsd").path
        let id = try await alice.run { try $0.create(label: "Team", at: container) }
        var shot = try await alice.run { try $0.snapshot(id, scan: true) }
        XCTAssertEqual(shot.label, "Team")
        XCTAssertEqual(shot.members, 1)
        XCTAssertTrue(shot.issues.isEmpty)
        let name = SecureDriveEditorText.untitled("document", existing: [])
        XCTAssertEqual(name, "Untitled document.html")
        XCTAssertEqual(SecureDriveEditorText.untitled("document", existing: ["untitled document.html"]), "Untitled document 2.html")
        let fileId = try await alice.run { try $0.add(id, name: name, content: .bytes(SecureDriveEditorText.newDocument)) }
        shot = try await alice.run { try $0.snapshot(id, scan: false) }
        let base = try XCTUnwrap(shot.file(fileId)?.current?.blob)
        try await alice.run { try $0.save(id, file: fileId, bytes: Array("<p>hi</p>".utf8), base: base) }
        let copy = root.appendingPathComponent("copy.html")
        try await alice.run { try $0.read(id, file: fileId, version: -1, purpose: "external", to: copy) }
        XCTAssertEqual(try Data(contentsOf: copy), Data("<p>hi</p>".utf8))
        SecureDriveScratchFiles.shred(copy)
        XCTAssertFalse(FileManager.default.fileExists(atPath: copy.path))

        let code = try await alice.run { try $0.invite(id) }
        let joined = try await bob.run { try $0.join(path: container, bookmark: nil, code: code) }
        let pending = try await bob.run { try $0.snapshot(joined.id, scan: true) }
        XCTAssertTrue(pending.pending)
        XCTAssertEqual(pending.joinSafety, joined.safety)
        shot = try await alice.run { try $0.snapshot(id, scan: true, force: true) }
        XCTAssertEqual(shot.requests.first?.safety, joined.safety)
        let mailbox = try XCTUnwrap(shot.requests.first?.mailbox)
        try await alice.run { try $0.approve(id, mailbox: mailbox) }
        let member = try await bob.run { try $0.snapshot(joined.id, scan: true) }
        XCTAssertFalse(member.pending)
        XCTAssertEqual(member.files.count, 1)
        XCTAssertEqual(member.devices.count, 2)
        XCTAssertTrue(member.devices.contains { $0.isThisDevice && $0.name == "Phone (this device)" })
        let csv = try await alice.run { try $0.exportActivity(id) }
        XCTAssertTrue(csv.contains("Phone"))
        shot = try await alice.run { try $0.snapshot(id, scan: true, force: true) }
        XCTAssertEqual(shot.records.first?.action, "log_exported")
        try await alice.run { try $0.cancelInvites(id) }
        shot = try await alice.run { try $0.snapshot(id, scan: false) }
        XCTAssertTrue(shot.invites.isEmpty)
        try await alice.run { $0.lock() }
        let unlocked = try await alice.run { $0.isUnlocked }
        XCTAssertFalse(unlocked)
        do {
            _ = try await alice.run { try $0.snapshot(id, scan: true) }
            XCTFail("a locked service opened the drive")
        } catch {
            XCTAssertEqual((error as? SecureDriveError)?.code, "locked")
        }
    }

    func testScratchFoldersAreClearedOnlyInsideTheirParent() throws {
        let parent = FileManager.default.temporaryDirectory.appendingPathComponent("secure-drive-test-\(UUID().uuidString)", isDirectory: true)
        defer {
            if parent.lastPathComponent.hasPrefix("secure-drive-test-") { try? FileManager.default.removeItem(at: parent) }
        }
        let folder = try XCTUnwrap(SecureDriveScratchFiles.makeFolder(in: parent))
        let file = folder.appendingPathComponent("secret.txt")
        try Data("plain".utf8).write(to: file)
        let outside = parent.deletingLastPathComponent().appendingPathComponent("secure-drive-test-outside-\(UUID().uuidString)")
        try Data("keep".utf8).write(to: outside)
        defer { SecureDriveScratchFiles.shred(outside) }
        SecureDriveScratchFiles.clear(parent.deletingLastPathComponent(), inside: parent)
        XCTAssertTrue(FileManager.default.fileExists(atPath: outside.path))
        SecureDriveScratchFiles.clearAll(parent)
        XCTAssertFalse(FileManager.default.fileExists(atPath: file.path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: folder.path))
    }
}
