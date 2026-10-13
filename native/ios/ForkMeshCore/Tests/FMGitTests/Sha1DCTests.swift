import Foundation
import XCTest
@testable import FMGit

final class Sha1DCTests: XCTestCase {
    private let shatteredPrefix = Array(Data(base64Encoded: "JVBERi0xLjMKJeLjz9MKCgoxIDAgb2JqCjw8L1dpZHRoIDIgMCBSL0hlaWdodCAzIDAgUi9UeXBlIDQgMCBSL1N1YnR5cGUgNSAwIFIvRmlsdGVyIDYgMCBSL0NvbG9yU3BhY2UgNyAwIFIvTGVuZ3RoIDggMCBSL0JpdHNQZXJDb21wb25lbnQgOD4+CnN0cmVhbQr/2P/+ACRTSEEtMSBpcyBkZWFkISEhISGFL+wJIzl1nDmxocY8TJfh//4Bc0bckWa2fhGPApq2IbJWD/nKZ8yox/hbqEx5AwwrPeIY+G2zqQkB1d9FwU8m/t+z3DjpasIv571yjw5FvOBG0jxXD+sUE5i7VS71oKgr4zH+pIA3uLXXHw4zLt+TrDUA603cDezBqGR5DHgsdiFWYN0wl5HQa9CvP5jNpLxGKbEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP/+APwAAAAAAAAAAP/gABBKRklGAAEBAQBIAEgAAP/bAEMAAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQ==")!)

    private func screen(_ chunks: [[UInt8]]) throws -> GitSha1CollisionDetectorStatus {
        let session = try GitSha1DCDetector().start()
        for chunk in chunks { try session.add(chunk) }
        return try session.close()
    }

    func testTheShatteredCollisionIsDetected() throws {
        XCTAssertEqual(shatteredPrefix.count, 640)
        XCTAssertEqual(try screen([shatteredPrefix]), .detected)
        XCTAssertEqual(try screen([Array(shatteredPrefix[..<100]), Array(shatteredPrefix[100...])]), .detected)
    }

    func testOrdinaryObjectsAreClear() throws {
        XCTAssertEqual(try screen([Array("blob 6\u{0}hello\n".utf8)]), .clear)
        XCTAssertEqual(try screen([]), .clear)
        var tampered = shatteredPrefix
        tampered[0] ^= 0x01
        XCTAssertEqual(try screen([tampered]), .clear)
    }

    func testAClosedSessionRefusesMoreInput() throws {
        let session = try GitSha1DCDetector().start()
        XCTAssertEqual(try session.close(), .clear)
        XCTAssertThrowsError(try session.add([1]))
        XCTAssertEqual(try session.close(), .failed)
        try session.dispose()
    }

    func testAnUntrustedPackParsesWithTheDetector() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("fm-sha1dc-\(UUID().uuidString)").path
        defer { try? FileManager.default.removeItem(atPath: root) }
        let repository = try GitRepository.initialize(GitRepositoryInit(root))
        let tip = try leCommit(repository, "a.txt", "a\n", "one\n")
        let pack = try repository.writePack([tip])

        let parsed = try GitPackReader.parse(pack.packBytes, .standard, origin: .untrustedTransport, detector: GitSha1DCDetector())

        XCTAssertEqual(parsed.objects.count, 3)
        XCTAssertThrowsError(try GitPackReader.parse(pack.packBytes, .standard, origin: .untrustedTransport))
    }
}
