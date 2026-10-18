import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMPackages

final class SecureDriveCryptoTests: XCTestCase {
    func testInviteCodes() {
        let made = SecureDriveCrypto.newInviteCode()
        XCTAssertEqual(made.code.count, 35)
        XCTAssertEqual(made.code.split(separator: "-").count, 9)
        XCTAssertEqual(SecureDriveCrypto.parseInviteCode(made.code), made.secret)
        XCTAssertEqual(SecureDriveCrypto.parseInviteCode(made.code.lowercased()), made.secret)
        XCTAssertEqual(SecureDriveCrypto.parseInviteCode(made.code.replacingOccurrences(of: "-", with: " ")), made.secret)
        var typo = Array(made.code)
        typo[0] = typo[0] == "A" ? "B" : "A"
        XCTAssertNil(SecureDriveCrypto.parseInviteCode(String(typo)))
        XCTAssertNil(SecureDriveCrypto.parseInviteCode(String(made.code.dropLast())))
        let ambiguous = made.code.replacingOccurrences(of: "0", with: "O").replacingOccurrences(of: "1", with: "l")
        XCTAssertEqual(SecureDriveCrypto.parseInviteCode(ambiguous), made.secret)
        for _ in 0..<64 {
            let code = SecureDriveCrypto.newInviteCode()
            XCTAssertEqual(SecureDriveCrypto.parseInviteCode(code.code), code.secret)
            XCTAssertTrue((0..<64).contains(SecureDrive.mailbox(forInvite: code.secret)))
        }
    }

    func testSealToDevice() throws {
        let alice = SecureDriveDeviceKeys.generate()
        let bob = SecureDriveDeviceKeys.generate()
        let alicePublic = try alice.publicKeys()
        XCTAssertTrue(alicePublic.isValid)
        XCTAssertEqual(SecureDrivePublicKeys.parse(alicePublic.toJSON()), alicePublic)
        var sealed = try SecureDriveCrypto.sealToDevice(alicePublic, label: sdBytes("t"), sdBytes("hello"))
        XCTAssertEqual(sealed.count, 32 + 1088 + 12 + 5 + 16)
        XCTAssertEqual(try SecureDriveCrypto.openFromDevice(alice, label: sdBytes("t"), sealed), sdBytes("hello"))
        XCTAssertThrowsError(try SecureDriveCrypto.openFromDevice(bob, label: sdBytes("t"), sealed))
        XCTAssertThrowsError(try SecureDriveCrypto.openFromDevice(alice, label: sdBytes("u"), sealed))
        sealed[40] ^= 1
        XCTAssertThrowsError(try SecureDriveCrypto.openFromDevice(alice, label: sdBytes("t"), sealed))
        var badKey = alicePublic
        badKey.mlkem768[0] = 0xff
        badKey.mlkem768[1] = 0xff
        XCTAssertNil(SecureDrivePublicKeys.parse(badKey.toJSON()))
    }

    func testSignaturesAndKeys() throws {
        let keys = SecureDriveDeviceKeys.generate()
        let parsed = try SecureDriveDeviceKeys.parse(keys.toJSON())
        XCTAssertEqual(parsed, keys)
        let published = try keys.publicKeys()
        let signature = try SecureDriveCrypto.sign(keys.ed25519, sdBytes("message"))
        XCTAssertTrue(SecureDriveCrypto.verify(published.ed25519, sdBytes("message"), signature))
        XCTAssertFalse(SecureDriveCrypto.verify(published.ed25519, sdBytes("massage"), signature))
        let safety = SecureDriveCrypto.safetyNumber(sdBytes("device"), published)
        XCTAssertEqual(safety.count, 23)
        XCTAssertEqual(safety.split(separator: " ").map(\.count), [5, 5, 5, 5])
        XCTAssertNotEqual(safety, SecureDriveCrypto.safetyNumber(sdBytes("other"), published))
    }

    func testPaddingAndUnits() {
        var previous: Int64 = 0
        for n: Int64 in [0, 1, 4096, 4097, 100_000, 1_048_577, 50_000_000] {
            let padded = SecureDriveCrypto.paddedSize(n)
            XCTAssertGreaterThanOrEqual(padded, previous)
            XCTAssertGreaterThanOrEqual(padded, n)
            if n >= 4096 { XCTAssertLessThanOrEqual(padded - n, n / 8 + 1) }
            previous = padded
        }
        XCTAssertEqual(SecureDriveCrypto.paddedSize(0), 4096)
        XCTAssertEqual(SecureDrive.chunkCount(0), 1)
        XCTAssertEqual(SecureDrive.chunkCount(40000), 3)
        let key = CryptoBytes.random(32)
        let unit = SecureDriveCrypto.sealUnit(key, sdBytes("aad"), sdBytes("{\"a\":1}"), size: 8192)!
        XCTAssertEqual(unit.count, 8192)
        let plain = SecureDriveCrypto.openUnit(key, sdBytes("aad"), unit)!
        XCTAssertEqual(SecureDriveCrypto.stripPadding(plain), sdBytes("{\"a\":1}"))
        XCTAssertNil(SecureDriveCrypto.openUnit(key, sdBytes("aab"), unit))
        XCTAssertNil(SecureDriveCrypto.sealUnit(key, [], [UInt8](repeating: 1, count: 8192 - 27), size: 8192))
    }

    func testBase64AndHashing() {
        XCTAssertEqual(SecureDriveCrypto.unbase64(""), [])
        XCTAssertNil(SecureDriveCrypto.unbase64("ab=="))
        XCTAssertNil(SecureDriveCrypto.unbase64("a+b/"))
        let bytes = CryptoBytes.random(100)
        XCTAssertEqual(SecureDriveCrypto.unbase64(SecureDriveCrypto.base64(bytes)), bytes)
        for size in [0, 1, 55, 56, 63, 64, 65, 1000, 100_000] {
            let data = CryptoBytes.random(size)
            var stream = Sha256Stream()
            var offset = 0
            while offset < data.count {
                let end = min(data.count, offset + 37)
                stream.update(Array(data[offset..<end]))
                offset = end
            }
            XCTAssertEqual(stream.finish(), Sha256.hash(data))
        }
        XCTAssertEqual(SecureDriveEpochKey.epochId([UInt8](repeating: 7, count: 32)).count, 16)
    }

    func testKdfParameters() throws {
        let fresh = SecureDriveKdf.fresh()
        let json = fresh.toJSON()
        XCTAssertEqual(try SecureDriveKdf.parse(json), fresh)
        var weak = json
        weak["memoryKiB"] = 1024
        XCTAssertThrowsError(try SecureDriveKdf.parse(weak))
        var other = json
        other["algorithm"] = "scrypt"
        XCTAssertThrowsError(try SecureDriveKdf.parse(other))
    }

    func testTextAndKinds() {
        XCTAssertEqual(SecureDriveText.fileKind("Notes.HTML"), "document")
        XCTAssertEqual(SecureDriveText.fileKind("a.csv"), "spreadsheet")
        XCTAssertEqual(SecureDriveText.fileKind("a.svg"), "drawing")
        XCTAssertEqual(SecureDriveText.fileKind("a.md"), "text")
        XCTAssertEqual(SecureDriveText.fileKind("a.jpeg"), "image")
        XCTAssertEqual(SecureDriveText.fileKind("a.bin"), "file")
        XCTAssertEqual(SecureDriveText.fileKind("noext"), "file")
        var record = SecureDriveRecord()
        record.action = "file_renamed"
        record.data = ["from": "a.txt", "name": "b.txt"]
        XCTAssertEqual(SecureDriveText.describe(record) { $0 }, "Renamed \u{201C}a.txt\u{201D} to \u{201C}b.txt\u{201D}")
        record.action = "file_restored"
        record.data = ["version": 0, "name": "b.txt"]
        XCTAssertEqual(SecureDriveText.describe(record) { $0 }, "Restored version 1 of \u{201C}b.txt\u{201D}")
        XCTAssertEqual(SecureDriveText.issueText(SecureDriveIssue(code: "unreadable_objects", detail: "2")),
                       "2 encrypted parts of the drive could not be opened with this device's keys.")
        XCTAssertEqual(SecureDrive.csvField("a,b \"c\""), "\"a,b \"\"c\"\"\"")
    }
}
