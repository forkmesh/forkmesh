import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMPackages

let secureDrivePass = "correct horse battery"
let secureDriveTestKdf = SecureDriveKdfCost(memoryKiB: 8192, passes: 1)
let secureDriveFastLease = SecureDriveLeaseTiming(settleMs: 0, staleMs: 300, pollMs: 1)

final class SecureDriveScratch {
    let root: URL

    init() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent("secure-drive-test-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    func url(_ name: String) -> URL { root.appendingPathComponent(name) }

    func state(_ name: String) -> SecureDriveLocalState {
        SecureDriveLocalState(directory: url(name), testingKdf: secureDriveTestKdf)
    }

    deinit {
        let temp = FileManager.default.temporaryDirectory.standardizedFileURL.path
        let path = root.standardizedFileURL.path
        if path.hasPrefix(temp + "/") && root.lastPathComponent.hasPrefix("secure-drive-test-") {
            try? FileManager.default.removeItem(at: root)
        }
    }
}

func sdRead(_ drive: SecureDrive, _ id: String, version: Int = -1) -> [UInt8]? {
    try? drive.read(id, version: version)
}

func sdHasIssue(_ drive: SecureDrive, _ code: String) -> Bool { drive.issues.contains { $0.code == code } }

func sdHasAction(_ drive: SecureDrive, _ action: String) -> Bool { drive.records.contains { $0.action == action } }

func sdBytes(_ text: String) -> [UInt8] { Array(text.utf8) }

func sdPatch(_ url: URL, _ offset: Int64, _ bytes: [UInt8]) throws {
    let handle = try FileHandle(forUpdating: url)
    defer { try? handle.close() }
    try handle.seek(toOffset: UInt64(offset))
    try handle.write(contentsOf: Data(bytes))
}

func sdContents(_ url: URL) -> [UInt8] { (try? Data(contentsOf: url)).map { Array($0) } ?? [] }

func sdContains(_ haystack: [UInt8], _ needle: [UInt8]) -> Bool {
    guard !needle.isEmpty, haystack.count >= needle.count else { return false }
    let first = needle[0]
    var i = 0
    let last = haystack.count - needle.count
    while i <= last {
        if haystack[i] == first && Array(haystack[i..<(i + needle.count)]) == needle { return true }
        i += 1
    }
    return false
}

final class SecureDriveCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0

    func add() {
        lock.lock()
        count += 1
        lock.unlock()
    }

    var value: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }
}

final class SecureDriveFlowTests: XCTestCase {
    func onlyOneOpaqueFile(_ folder: URL, _ container: URL, _ needles: [String]) -> Bool {
        guard let entries = try? FileManager.default.contentsOfDirectory(atPath: folder.path), entries == [container.lastPathComponent] else {
            return false
        }
        let bytes = sdContents(container)
        if bytes.count % SecureDriveLayout.pageBytes != 0 { return false }
        return !needles.contains { sdContains(bytes, sdBytes($0)) }
    }

    func testDriveFlow() throws {
        let scratch = try SecureDriveScratch()
        let folder = scratch.url("router-usb")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let container = folder.appendingPathComponent("Shared.fmsd")
        let storage = SecureDriveLocalStorage()

        let alice = scratch.state("alice")
        try alice.create(passphrase: secureDrivePass, deviceName: "Office laptop")
        XCTAssertThrowsError(try scratch.state("x").create(passphrase: "short", deviceName: "")) {
            XCTAssertEqual(($0 as? SecureDriveError)?.code, "passphrase_too_short")
        }
        let aliceId = try SecureDrive.create(state: alice, storage: storage, container: container.path, label: "Team share", timing: secureDriveFastLease)
        let aliceDrive = SecureDrive(state: alice, id: aliceId, timing: secureDriveFastLease)
        try aliceDrive.scan()
        XCTAssertTrue(sdHasAction(aliceDrive, "drive_created") && sdHasAction(aliceDrive, "drive_opened"))
        XCTAssertThrowsError(try SecureDrive.create(state: alice, container: container.path, label: "", timing: secureDriveFastLease))
        let emptySize = sdContents(container).count

        let letter = sdBytes("<html><body>Quarterly secret plan</body></html>")
        let letterId = try aliceDrive.addFile("plan.html", .bytes(letter))
        XCTAssertEqual(sdRead(aliceDrive, letterId), letter)
        XCTAssertEqual(sdContents(container).count, emptySize, "a small file does not change the drive's size")
        var big = [UInt8](repeating: 0, count: 1024 * 1024 + 7)
        for i in big.indices { big[i] = UInt8(truncatingIfNeeded: i &* 31) }
        let bigId = try aliceDrive.addFile("photo.raw", .bytes(big))
        XCTAssertEqual(sdRead(aliceDrive, bigId), big, "a multi-page file round-trips")
        let grown = sdContents(container).count
        XCTAssertTrue(grown > emptySize && grown % SecureDriveLayout.pageBytes == 0)
        let emptyId = try aliceDrive.addFile("empty.txt", .bytes([]))
        XCTAssertEqual(sdRead(aliceDrive, emptyId), [])
        XCTAssertEqual(aliceDrive.files.count, 3)
        let plan = aliceDrive.file(letterId)
        XCTAssertEqual(plan?.kind, "document")
        XCTAssertEqual(plan?.opens, 1)
        XCTAssertTrue(onlyOneOpaqueFile(folder, container, ["Quarterly", "plan.html", "Team share", "Office laptop", "photo.raw"]))

        let bob = scratch.state("bob")
        try bob.create(passphrase: secureDrivePass, deviceName: "Front desk")
        let invite = try aliceDrive.createInvite()
        let joined = try SecureDrive.requestJoin(state: bob, container: container.path, code: invite.code)
        let bobDrive = SecureDrive(state: bob, id: joined.id, timing: secureDriveFastLease)
        try bobDrive.scan()
        XCTAssertTrue(bob.membership(joined.id)?.isPending ?? false)
        XCTAssertTrue(bobDrive.files.isEmpty)
        XCTAssertFalse(bobDrive.joinRequestMissing)
        XCTAssertEqual(bobDrive.joinSafetyNumber, joined.safety)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: folder.path).count, 1)

        try aliceDrive.scan()
        XCTAssertEqual(aliceDrive.requests.count, 1)
        XCTAssertEqual(aliceDrive.requests.first?.safety, joined.safety)
        XCTAssertEqual(aliceDrive.requests.first?.name, "Front desk")
        try aliceDrive.approve(mailbox: aliceDrive.requests[0].mailbox)

        try bobDrive.scan()
        XCTAssertTrue(bob.membership(joined.id)?.isMember ?? false)
        XCTAssertEqual(bobDrive.files.count, 3)
        XCTAssertEqual(bob.membership(joined.id)?.label, "Team share")
        XCTAssertEqual(sdRead(bobDrive, letterId), letter)
        XCTAssertEqual(bobDrive.devices.count, 2)
        let edited = sdBytes("<html><body>Revised plan</body></html>")
        try bobDrive.saveFile(letterId, .bytes(edited))
        try aliceDrive.scan()
        XCTAssertEqual(sdRead(aliceDrive, letterId), edited)
        XCTAssertEqual(aliceDrive.file(letterId)?.versions.count, 2)
        XCTAssertEqual(sdRead(aliceDrive, letterId, version: 0), letter)
        XCTAssertEqual(aliceDrive.issues, [])
        XCTAssertTrue(aliceDrive.records.contains { $0.device == bob.deviceId && $0.action == "file_opened" })
        XCTAssertTrue(aliceDrive.activityCsv().contains("Front desk"))
        XCTAssertEqual(aliceDrive.deviceSafetyNumber(bob.deviceId), bobDrive.thisDeviceSafetyNumber)

        let mallory = scratch.state("mallory")
        try mallory.create(passphrase: secureDrivePass, deviceName: "Stranger")
        var wrong = SecureDriveCrypto.newInviteCode()
        while aliceDrive.mailboxesInUse.contains(SecureDrive.mailbox(forInvite: wrong.secret)) { wrong = SecureDriveCrypto.newInviteCode() }
        _ = try? SecureDrive.requestJoin(state: mallory, container: container.path, code: wrong.code)
        try aliceDrive.scan()
        XCTAssertTrue(aliceDrive.requests.isEmpty, "a request without a valid code is invisible")
        XCTAssertThrowsError(try SecureDrive.requestJoin(state: mallory, container: container.path, code: invite.code))

        try bobDrive.scan()
        let bobStarted = bobDrive.file(letterId)!.current!.blob
        try aliceDrive.saveFile(letterId, .bytes(sdBytes("<p>alice</p>")))
        try bobDrive.saveFile(letterId, .bytes(sdBytes("<p>bob</p>")), base: bobStarted)
        try aliceDrive.scan()
        XCTAssertEqual(aliceDrive.file(letterId)?.conflicts, 1)

        do {
            let timing = SecureDriveLeaseTiming(settleMs: 15, staleMs: 3000, pollMs: 2)
            let aliceWriter = SecureDrive(state: alice, id: aliceId, timing: timing)
            let bobWriter = SecureDrive(state: bob, id: joined.id, timing: timing)
            try aliceWriter.scan()
            try bobWriter.scan()
            let aliceFailures = SecureDriveCounter()
            let done = DispatchSemaphore(value: 0)
            let thread = Thread {
                for _ in 0..<8 {
                    do { try aliceWriter.note("log_exported", [:]) } catch { aliceFailures.add() }
                }
                done.signal()
            }
            thread.start()
            var bobOk = true
            for _ in 0..<8 {
                do { try bobWriter.note("log_exported", [:]) } catch { bobOk = false }
            }
            done.wait()
            XCTAssertTrue(aliceFailures.value == 0 && bobOk, "two devices writing at the same time both succeed")
            try aliceDrive.scan()
            XCTAssertEqual(aliceDrive.records.filter { $0.action == "log_exported" }.count, 16)
            XCTAssertEqual(aliceDrive.issues, [])
        }

        let tamperId = try aliceDrive.addFile("ledger.csv", .bytes(sdBytes("a,b\n1,2\n")))
        do {
            let page = aliceDrive.file(tamperId)!.current!.pages[0].start
            let offset = page * Int64(SecureDriveLayout.pageBytes) + 20
            let byte = sdContents(container)[Int(offset)]
            try sdPatch(container, offset, [byte ^ 0x40])
        }
        XCTAssertThrowsError(try aliceDrive.read(tamperId)) {
            XCTAssertEqual(($0 as? SecureDriveError)?.code, "content_damaged")
        }
        XCTAssertTrue(sdHasAction(aliceDrive, "integrity_alert"))

        let beforeBob = sdContents(container)
        try bobDrive.scan()
        try bobDrive.note("log_exported", [:])
        try aliceDrive.scan()
        try Data(beforeBob).write(to: container)
        try aliceDrive.scan()
        XCTAssertTrue(sdHasIssue(aliceDrive, "index_restored") || sdHasIssue(aliceDrive, "rolled_back"))
        try bobDrive.scan()
        XCTAssertTrue(sdHasIssue(bobDrive, "own_records_missing") || sdHasIssue(bobDrive, "index_restored"))

        let rootBytes = Int(SecureDriveLayout.rootPages) * SecureDriveLayout.pageBytes
        try sdPatch(container, SecureDriveLayout.rootAPage * Int64(SecureDriveLayout.pageBytes), CryptoBytes.random(rootBytes))
        try sdPatch(container, SecureDriveLayout.rootBPage * Int64(SecureDriveLayout.pageBytes), CryptoBytes.random(rootBytes))
        try aliceDrive.scan()
        XCTAssertTrue(sdHasIssue(aliceDrive, "index_restored"))
        XCTAssertGreaterThanOrEqual(aliceDrive.files.count, 3)
        try aliceDrive.note("log_exported", [:])
        try bobDrive.scan()
        XCTAssertGreaterThanOrEqual(bobDrive.files.count, 3)

        alice.lock()
        XCTAssertFalse(alice.isUnlocked)
        XCTAssertTrue(alice.drives.isEmpty)
        XCTAssertThrowsError(try alice.unlock(passphrase: "wrong passphrase!")) {
            XCTAssertEqual(($0 as? SecureDriveError)?.code, "wrong_passphrase")
        }
        try alice.unlock(passphrase: secureDrivePass)
        XCTAssertEqual(alice.drives.count, 1)
        try alice.changePassphrase(current: secureDrivePass, next: "another long passphrase")
        alice.lock()
        try alice.unlock(passphrase: "another long passphrase")
        let aliceAgain = SecureDrive(state: alice, id: aliceId, timing: secureDriveFastLease)
        try aliceAgain.scan()

        let carol = scratch.state("carol")
        try carol.create(passphrase: secureDrivePass, deviceName: "Spare tablet")
        let carolInvite = try aliceAgain.createInvite()
        let carolJoin = try SecureDrive.requestJoin(state: carol, container: container.path, code: carolInvite.code)
        try aliceAgain.scan()
        XCTAssertEqual(aliceAgain.requests.count, 1)
        try aliceAgain.approve(mailbox: aliceAgain.requests[0].mailbox)
        let carolDrive = SecureDrive(state: carol, id: carolJoin.id, timing: secureDriveFastLease)
        try carolDrive.scan()
        XCTAssertEqual(sdRead(carolDrive, bigId), big)
        let oldPages = aliceAgain.file(bigId)!.current!.pages
        let pageBytes = SecureDriveLayout.pageBytes
        let oldStart = Int(oldPages[0].start) * pageBytes
        let oldFirst = Array(sdContents(container)[oldStart..<(oldStart + pageBytes)])

        try aliceAgain.scan()
        let issuesBefore = aliceAgain.issues.count
        let oldEpoch = alice.membership(aliceId)!.currentEpoch!
        var progressCalls = 0
        try aliceAgain.removeDevice(carol.deviceId) { _, _ in progressCalls += 1 }
        let rootCount = Int(SecureDriveLayout.rootPages) * pageBytes
        for page in [SecureDriveLayout.rootAPage, SecureDriveLayout.rootBPage] {
            let start = Int(page) * pageBytes
            let slot = Array(sdContents(container)[start..<(start + rootCount)])
            for slotA in [true, false] {
                XCTAssertNil(SecureDriveCrypto.openUnit(SecureDriveCrypto.hkdf(oldEpoch.key, "root"), SecureDrive.rootAad(slotA), slot),
                             "no index under the old key survives a removal")
            }
        }
        XCTAssertGreaterThan(progressCalls, 0)
        XCTAssertNotEqual(Array(sdContents(container)[oldStart..<(oldStart + pageBytes)]), oldFirst, "content under the old key is overwritten")
        try aliceAgain.scan()
        XCTAssertEqual(sdRead(aliceAgain, bigId), big)
        try bobDrive.scan()
        XCTAssertEqual(sdRead(bobDrive, bigId), big, "bob picks up the new key")
        XCTAssertEqual(bob.membership(joined.id)?.epochs.count, 2)
        let carolScanned = (try? carolDrive.scan()) != nil
        XCTAssertTrue(!carolScanned || sdRead(carolDrive, bigId) == nil, "carol cannot open anything stored after her removal")
        XCTAssertTrue(!carolScanned || (carolDrive.removedFromDrive && sdHasIssue(carolDrive, "keyring_missing")))
        XCTAssertThrowsError(try carolDrive.note("drive_opened", [:]), "carol can no longer write records")
        try aliceAgain.scan()
        XCTAssertEqual(aliceAgain.issues.count, issuesBefore, "carol's attempts leave the drive intact")
        XCTAssertTrue(aliceAgain.devices.first { $0.id == carol.deviceId }?.removed ?? false)

        try aliceAgain.renameFile(letterId, to: "plan-v2.html")
        XCTAssertEqual(aliceAgain.file(letterId)?.name, "plan-v2.html")
        try aliceAgain.restoreVersion(letterId, version: 0)
        XCTAssertEqual(sdRead(aliceAgain, letterId), letter)
        let letterStart = Int(aliceAgain.file(letterId)!.current!.pages[0].start) * pageBytes
        let letterBefore = Array(sdContents(container)[letterStart..<(letterStart + pageBytes)])
        try aliceAgain.deleteFile(letterId)
        XCTAssertNil(aliceAgain.file(letterId))
        XCTAssertNotEqual(Array(sdContents(container)[letterStart..<(letterStart + pageBytes)]), letterBefore, "a deleted file's pages are overwritten")
        XCTAssertTrue(onlyOneOpaqueFile(folder, container, ["Quarterly", "Revised", "plan-v2", "Spare tablet", "Front desk"]))
    }

    func testOverwrittenKeyringSlotIsRewritten() throws {
        let scratch = try SecureDriveScratch()
        let container = scratch.url("Drive.fmsd")
        let alice = scratch.state("alice")
        try alice.create(passphrase: secureDrivePass, deviceName: "Laptop")
        let aliceId = try SecureDrive.create(state: alice, container: container.path, label: "Team", timing: secureDriveFastLease)
        let drive = SecureDrive(state: alice, id: aliceId, timing: secureDriveFastLease)
        try drive.scan()
        XCTAssertTrue(alice.membership(aliceId)!.keyringSeen)
        try drive.scan()
        XCTAssertFalse(drive.changedSinceScan)
        let mailbox = alice.membership(aliceId)!.mailbox
        try sdPatch(container, SecureDriveLayout.mailboxOffset(mailbox), CryptoBytes.random(SecureDriveLayout.slotBytes))
        XCTAssertTrue(drive.changedSinceScan)
        try drive.scan()
        XCTAssertFalse(drive.removedFromDrive, "an index read from the file keeps the device a member")
        XCTAssertTrue(sdHasIssue(drive, "keyring_restored"))
        try drive.note("log_exported", [:])
        try drive.scan()
        XCTAssertEqual(drive.issues, [], "the next commit wrote the slot back")

        try sdPatch(container, SecureDriveLayout.mailboxOffset(mailbox), CryptoBytes.random(SecureDriveLayout.slotBytes))
        let rootBytes = Int(SecureDriveLayout.rootPages) * SecureDriveLayout.pageBytes
        try sdPatch(container, SecureDriveLayout.rootAPage * Int64(SecureDriveLayout.pageBytes), CryptoBytes.random(rootBytes))
        try sdPatch(container, SecureDriveLayout.rootBPage * Int64(SecureDriveLayout.pageBytes), CryptoBytes.random(rootBytes))
        try drive.scan()
        XCTAssertTrue(drive.removedFromDrive, "with only the cached index left the device is treated as removed")
        XCTAssertTrue(sdHasIssue(drive, "keyring_missing"))
        XCTAssertThrowsError(try drive.note("log_exported", [:]))
    }

    func testCancelJoinAndDeny() throws {
        let scratch = try SecureDriveScratch()
        let container = scratch.url("Drive.fmsd")
        let alice = scratch.state("alice")
        try alice.create(passphrase: secureDrivePass, deviceName: "Laptop")
        let aliceId = try SecureDrive.create(state: alice, container: container.path, label: "", timing: secureDriveFastLease)
        let drive = SecureDrive(state: alice, id: aliceId, timing: secureDriveFastLease)
        try drive.scan()
        XCTAssertEqual(drive.label, "Drive")
        let bob = scratch.state("bob")
        try bob.create(passphrase: secureDrivePass, deviceName: "Phone")
        let first = try drive.createInvite()
        XCTAssertEqual(drive.openInvites.count, 1)
        let request = try SecureDrive.requestJoin(state: bob, container: container.path, code: first.code)
        let pending = SecureDrive(state: bob, id: request.id, timing: secureDriveFastLease)
        try pending.cancelJoin()
        XCTAssertNil(bob.membership(request.id))
        try drive.scan()
        XCTAssertTrue(drive.requests.isEmpty, "a cancelled request disappears")

        let again = try SecureDrive.requestJoin(state: bob, container: container.path, code: first.code)
        try drive.scan()
        XCTAssertEqual(drive.requests.count, 1)
        try drive.deny(mailbox: drive.requests[0].mailbox)
        XCTAssertTrue(drive.requests.isEmpty)
        XCTAssertTrue(drive.openInvites.isEmpty)
        XCTAssertTrue(sdHasAction(drive, "join_denied"))
        let denied = SecureDrive(state: bob, id: again.id, timing: secureDriveFastLease)
        try denied.scan()
        XCTAssertTrue(denied.joinRequestMissing)

        let second = try drive.createInvite()
        try drive.revokeInvite(second.inviteId)
        XCTAssertTrue(drive.openInvites.isEmpty)
        XCTAssertEqual(drive.issues, [])
        XCTAssertTrue(drive.activityCsv().hasPrefix("time,device,activity,record,hash\n"))
    }

    func testStandardPassphraseCost() throws {
        let scratch = try SecureDriveScratch()
        let state = SecureDriveLocalState(directory: scratch.url("device"))
        try state.create(passphrase: secureDrivePass, deviceName: "  ")
        XCTAssertEqual(state.deviceName, "This iPhone")
        let saved = JSON.parseObject(try Data(contentsOf: state.fileURL))
        XCTAssertEqual(saved.obj("kdf").int("memoryKiB"), 65536)
        XCTAssertEqual(saved.obj("kdf").int("passes"), 3)
        XCTAssertEqual(saved.str("suite"), "fmsd1")
        let id = state.deviceId
        state.bookmarks["x"] = [1, 2, 3]
        try state.save()
        state.lock()
        let reopened = SecureDriveLocalState(directory: scratch.url("device"))
        try reopened.unlock(passphrase: secureDrivePass)
        XCTAssertEqual(reopened.deviceId, id)
        XCTAssertEqual(reopened.bookmarks["x"], [1, 2, 3])
        XCTAssertNil(reopened.exportDevice()["bookmarks"])
        let weak = SecureDriveLocalState(directory: scratch.url("weak"), testingKdf: secureDriveTestKdf)
        try weak.create(passphrase: secureDrivePass, deviceName: "Weak")
        XCTAssertThrowsError(try SecureDriveLocalState(directory: scratch.url("weak")).unlock(passphrase: secureDrivePass)) {
            XCTAssertEqual(($0 as? SecureDriveError)?.code, "unsafe_kdf_parameters")
        }
    }
}
