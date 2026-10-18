import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMPackages

final class SecureDriveInteropTests: XCTestCase {
    static var fixturesDirectory: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("marketplace/packages/secure-drive/tests/fixtures")
    }

    func testReadsEveryFixture() throws {
        let directory = Self.fixturesDirectory
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) else {
            throw XCTSkip("the repository fixtures are not reachable from here")
        }
        let found = names.filter { $0.hasPrefix("interop-from-") && $0.hasSuffix(".json") }.sorted()
        XCTAssertTrue(found.contains("interop-from-cpp.json"), "the desktop's fixture is present")
        for name in found {
            try readsFixture(directory.appendingPathComponent(name))
        }
    }

    func testOpensWhatItWrites() throws {
        let scratch = try SecureDriveScratch()
        let fixture = try writeFixture(scratch.root)
        try readsFixture(scratch.root.appendingPathComponent("interop-from-swift.json"))
        XCTAssertEqual(fixture.str("writer"), "swift")
        if let target = ProcessInfo.processInfo.environment["SECURE_DRIVE_WRITE_FIXTURE"], !target.isEmpty {
            let directory = URL(fileURLWithPath: target, isDirectory: true)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            for suffix in [".fmsd", ".json"] {
                let source = scratch.root.appendingPathComponent("interop-from-swift" + suffix)
                let destination = directory.appendingPathComponent("interop-from-swift" + suffix)
                try Data(contentsOf: source).write(to: destination, options: [.atomic])
            }
        }
    }

    func readsFixture(_ path: URL) throws {
        let fixture = JSON.parseObject(try Data(contentsOf: path))
        let writer = fixture.str("writer")
        let label = "fixture from \(writer)"
        XCTAssertEqual(fixture.str("format"), "forkmesh-secure-drive-interop-v1", label)
        let scratch = try SecureDriveScratch()
        let container = scratch.url("drive.fmsd")
        try Data(contentsOf: path.deletingLastPathComponent().appendingPathComponent(fixture.str("container"))).write(to: container)
        let pass = fixture.str("passphrase")
        func withContainer(_ device: JSONObject) -> JSONObject {
            var copy = device
            copy["drives"] = .array(device.maps("drives").map { membership in
                var m = membership
                m["container"] = JSONValue(container.path)
                return .object(m)
            })
            return copy
        }
        let expected = fixture.maps("files")
        var states = [SecureDriveLocalState]()
        var drives = [SecureDrive]()
        for (i, member) in fixture.maps("members").enumerated() {
            let state = scratch.state("member\(i)")
            try state.importDevice(withContainer(member), passphrase: pass)
            guard let first = state.drives.first else {
                XCTFail("\(label): a member has no drive")
                return
            }
            let drive = SecureDrive(state: state, id: first.id, timing: secureDriveFastLease)
            try drive.scan()
            XCTAssertEqual(drive.issues, [], "\(label): the drive verifies with no problems")
            XCTAssertGreaterThanOrEqual(drive.records.count, fixture.int("minimumRecords"), "\(label): every record is read")
            XCTAssertEqual(state.drives.first?.label, fixture.str("label"), "\(label): the drive name")
            XCTAssertEqual(drive.files.count, expected.count, "\(label): the file list")
            for want in expected {
                guard let entry = drive.file(want.str("id")) else {
                    XCTFail("\(label): \(want.str("name")) is missing")
                    continue
                }
                XCTAssertEqual(entry.name, want.str("name"), label)
                XCTAssertEqual(entry.kind, want.str("kind"), label)
                XCTAssertEqual(entry.versions.count, want.int("versions"), label)
                let content = try drive.read(entry.id)
                XCTAssertEqual(Int64(content.count), want.int64("size"), label)
                XCTAssertEqual(Sha256.hex(content), want.str("sha256"), "\(label): \(entry.name) decrypts to the written bytes")
            }
            for name in fixture.strings("deletedFiles") {
                XCTAssertFalse(drive.files.contains { $0.name == name }, "\(label): \(name) stays deleted")
            }
            for (device, safety) in fixture.obj("deviceSafety") {
                XCTAssertEqual(drive.deviceSafetyNumber(device), safety.stringValue, "\(label): safety numbers agree")
            }
            states.append(state)
            drives.append(drive)
        }
        guard drives.count >= 2 else {
            XCTFail("\(label): the fixture needs two members")
            return
        }
        let request = fixture.obj("joinRequest")
        try drives[0].scan()
        XCTAssertEqual(drives[0].requests.count, 1, label)
        XCTAssertEqual(drives[0].requests.first?.mailbox, request.int("mailbox"), label)
        XCTAssertEqual(drives[0].requests.first?.safety, request.str("safety"), "\(label): the pending request's safety number")
        XCTAssertEqual(drives[0].requests.first?.name, request.str("name"), label)
        XCTAssertNotNil(SecureDriveCrypto.parseInviteCode(fixture.str("openInviteCode")), "\(label): the open invitation code parses")
        let added = try drives[1].addFile("from-swift.txt", .bytes(sdBytes("written by the iPhone")))
        let firstId = expected[0].str("id")
        try drives[1].saveFile(firstId, .bytes(sdBytes("changed by the iPhone")))
        try drives[0].approve(mailbox: request.int("mailbox"))
        let pendingState = scratch.state("pending")
        try pendingState.importDevice(withContainer(fixture.obj("pending")), passphrase: pass)
        guard let pendingMembership = pendingState.drives.first else {
            XCTFail("\(label): the pending device has no drive")
            return
        }
        XCTAssertTrue(pendingMembership.isPending, label)
        let pending = SecureDrive(state: pendingState, id: pendingMembership.id, timing: secureDriveFastLease)
        try pending.scan()
        XCTAssertTrue(pendingState.drives.first?.isMember ?? false, "\(label): the pending device joins")
        XCTAssertEqual(try pending.read(added), sdBytes("written by the iPhone"), label)
        XCTAssertEqual(try pending.read(firstId), sdBytes("changed by the iPhone"), label)
        try drives[0].scan()
        XCTAssertEqual(drives[0].issues, [], "\(label): the drive still verifies after writes from this implementation")
        XCTAssertEqual(pending.issues, [], label)
    }

    func deviceJSON(_ state: SecureDriveLocalState, _ membershipId: String, _ containerName: String) -> JSONObject {
        var device = state.exportDevice()
        device["drives"] = .array(device.maps("drives").filter { $0.str("id") == membershipId }.map { membership in
            var m = membership
            m["container"] = JSONValue(containerName)
            return .object(m)
        })
        return device
    }

    func pattern(_ size: Int, _ seed: Int) -> [UInt8] {
        (0..<size).map { i in UInt8(truncatingIfNeeded: i * 131 + seed * 17 + (i >> 8)) }
    }

    func zeroFreeSpace(_ drive: SecureDrive, _ container: URL) throws {
        let handle = try FileHandle(forUpdating: container)
        defer { try? handle.close() }
        let pageBytes = SecureDriveLayout.pageBytes
        let pages = Int(try handle.seekToEnd()) / pageBytes
        var used = [Bool](repeating: false, count: pages)
        for run in drive.dataRunsInUse {
            for i in 0..<run.count { used[Int(run.start + i)] = true }
        }
        let zeroPage = Data(count: pageBytes)
        try handle.seek(toOffset: UInt64(SecureDriveLayout.leasePage) * UInt64(pageBytes))
        try handle.write(contentsOf: zeroPage)
        let mailboxes = Set(drive.mailboxesInUse)
        for slot in 0..<SecureDriveLayout.mailboxSlots where !mailboxes.contains(slot) {
            try handle.seek(toOffset: UInt64(SecureDriveLayout.mailboxOffset(slot)))
            try handle.write(contentsOf: Data(count: SecureDriveLayout.slotBytes))
        }
        for page in Int(SecureDriveLayout.dataPage)..<pages where !used[page] {
            try handle.seek(toOffset: UInt64(page * pageBytes))
            try handle.write(contentsOf: zeroPage)
        }
        try handle.synchronize()
    }

    func writeFixture(_ directory: URL) throws -> JSONObject {
        let name = "interop-from-swift"
        let container = directory.appendingPathComponent(name + ".fmsd")
        let states = try SecureDriveScratch()
        let alice = states.state("alice")
        let bob = states.state("bob")
        let carol = states.state("carol")
        try alice.create(passphrase: secureDrivePass, deviceName: "Office iPhone")
        try bob.create(passphrase: secureDrivePass, deviceName: "Front desk phone")
        try carol.create(passphrase: secureDrivePass, deviceName: "Spare tablet")
        let aliceId = try SecureDrive.create(state: alice, container: container.path, label: "Interop share", timing: secureDriveFastLease)
        let a = SecureDrive(state: alice, id: aliceId, timing: secureDriveFastLease)
        a.inviteLifetimeMs = 100 * 365 * 24 * 60 * 60 * 1000
        try a.scan()
        let notes = try a.addFile("Notes.html", .bytes(sdBytes("<html><body><h1>Notes</h1><p>First draft</p></body></html>")))
        let budget = try a.addFile("Budget.csv", .bytes(sdBytes("Item,Cost\r\nPaper,4.5\r\nInk,19.99\r\nTotal,\"=SUM(B2:B3)\"\r\n")))
        let drawing = SecureDriveDrawing(strokes: [
            SecureDriveStroke(color: "#0969da", width: 6, points: [SecureDrivePoint(100, 100), SecureDrivePoint(500, 100), SecureDrivePoint(500, 400)]),
        ])
        let sketch = try a.addFile("Sketch.svg", .bytes(drawing.toSvg()))
        try a.addFile("empty.txt", .bytes([]))
        try a.addFile("photo.bin", .bytes(pattern(40000, 3)))
        let old = try a.addFile("old.txt", .bytes(sdBytes("to be deleted")))
        try a.renameFile(budget, to: "Budget 2027.csv")
        try a.deleteFile(old)

        let code = try a.createInvite().code
        let bobJoin = try SecureDrive.requestJoin(state: bob, container: container.path, code: code)
        try a.scan()
        try a.approve(mailbox: a.requests[0].mailbox)
        let b = SecureDrive(state: bob, id: bobJoin.id, timing: secureDriveFastLease)
        try b.scan()
        try b.saveFile(notes, .bytes(sdBytes("<html><body><h1>Notes</h1><p>Second draft from the phone</p></body></html>")))
        _ = try b.read(sketch)

        try a.scan()
        let carolCode = try a.createInvite().code
        let carolJoin = try SecureDrive.requestJoin(state: carol, container: container.path, code: carolCode)
        try a.scan()
        let openCode = try a.createInvite().code
        try a.scan()
        try b.scan()
        XCTAssertEqual(a.issues, [])
        XCTAssertEqual(b.issues, [])
        XCTAssertEqual(a.requests.count, 1)
        try zeroFreeSpace(a, container)
        try a.scan()
        try b.scan()
        XCTAssertEqual(a.issues, [], "zeroing free space leaves the fixture intact")
        XCTAssertEqual(b.issues, [])

        var files = [JSONValue]()
        for entry in a.files {
            let v = entry.current!
            files.append(.object([
                "id": JSONValue(entry.id),
                "name": JSONValue(entry.name),
                "kind": JSONValue(entry.kind),
                "versions": JSONValue(entry.versions.count),
                "size": .int(v.size),
                "sha256": JSONValue(v.sha256),
            ]))
        }
        var safety = JSONObject()
        for device in a.devices { safety[device.id] = JSONValue(a.deviceSafetyNumber(device.id)) }
        let request = a.requests[0]
        let fileName = name + ".fmsd"
        let fixture: JSONObject = [
            "format": "forkmesh-secure-drive-interop-v1",
            "writer": "swift",
            "container": JSONValue(fileName),
            "passphrase": JSONValue(secureDrivePass),
            "drive": JSONValue(alice.membership(aliceId)!.driveId),
            "label": "Interop share",
            "members": .array([.object(deviceJSON(alice, aliceId, fileName)), .object(deviceJSON(bob, bobJoin.id, fileName))]),
            "pending": .object(deviceJSON(carol, carolJoin.id, fileName)),
            "joinRequest": ["mailbox": JSONValue(request.mailbox), "name": JSONValue(request.name), "safety": JSONValue(request.safety)],
            "openInviteCode": JSONValue(openCode),
            "files": .array(files),
            "deletedFiles": ["old.txt"],
            "minimumRecords": JSONValue(a.records.count),
            "deviceSafety": .object(safety),
        ]
        try (JSON.encodePretty(.object(fixture), indent: "    ") + "\n").write(to: directory.appendingPathComponent(name + ".json"), atomically: true, encoding: .utf8)
        return fixture
    }
}
