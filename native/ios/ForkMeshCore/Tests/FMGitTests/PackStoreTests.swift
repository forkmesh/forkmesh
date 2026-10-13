import Foundation
import XCTest
@testable import FMGit

private func blobId(_ body: [UInt8]) -> GitObjectId { GitObjectId.sha1Of(GitObjectFrame.encode(.blob, body)) }

private struct HandPack {
    var bytes = utf8("PACK") + uint32Bytes(2) + uint32Bytes(0)
    var entries = [GitPackIndexWriteEntry]()
    var count = 0

    mutating func add(_ id: GitObjectId, header: [UInt8], payload: [UInt8]) throws -> Int {
        let offset = bytes.count
        bytes += header + (try GitZlib.deflate(payload))
        entries.append(GitPackIndexWriteEntry(id, offset: offset, crc32: gitCrc32(bytes, offset, bytes.count)))
        count += 1
        return offset
    }

    func finished() -> (pack: [UInt8], checksum: [UInt8]) {
        var pack = bytes
        pack.replaceSubrange(8..<12, with: uint32Bytes(UInt32(count)))
        let checksum = ForkMeshSha1.hash(pack)
        return (pack + checksum, checksum)
    }
}

private func sizeHeader(_ type: Int, _ size: Int) -> [UInt8] {
    var header = [UInt8]()
    var remaining = size >> 4
    var first = UInt8((type << 4) | (size & 0x0f))
    if remaining > 0 { first |= 0x80 }
    header.append(first)
    while remaining > 0 {
        var byte = UInt8(remaining & 0x7f)
        remaining >>= 7
        if remaining > 0 { byte |= 0x80 }
        header.append(byte)
    }
    return header
}

final class PackStoreTests: GitTestCase {
    private func install(_ pack: [UInt8], _ index: [UInt8], in gitDirectory: String) throws {
        let directory = gitDirectory + "/objects/pack"
        try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
        let name = "pack-" + encodeLowerHex(Array(pack.suffix(20)))
        try Data(pack).write(to: URL(fileURLWithPath: "\(directory)/\(name).pack"))
        try Data(index).write(to: URL(fileURLWithPath: "\(directory)/\(name).idx"))
    }

    private func chainPack() throws -> (pack: [UInt8], index: [UInt8], ids: [GitObjectId], bodies: [[UInt8]]) {
        let base = utf8("hello\n")
        let middle = utf8("hello\nworld\n")
        let top = utf8("hello\nworld\nagain\n")
        let ofsDelta: [UInt8] = [6, 12, 0x90, 6, 6] + utf8("world\n")
        let refDelta: [UInt8] = [12, 18, 0x90, 12, 6] + utf8("again\n")
        var hand = HandPack()
        let baseOffset = try hand.add(blobId(base), header: sizeHeader(3, base.count), payload: base)
        let distance = hand.bytes.count - baseOffset
        XCTAssertLessThan(distance, 128)
        _ = try hand.add(blobId(middle), header: sizeHeader(6, ofsDelta.count) + [UInt8(distance)], payload: ofsDelta)
        _ = try hand.add(blobId(top), header: sizeHeader(7, refDelta.count) + blobId(middle).bytes, payload: refDelta)
        let (pack, checksum) = hand.finished()
        return (pack, try GitPackIndexWriter.write(hand.entries, checksum), [blobId(base), blobId(middle), blobId(top)], [base, middle, top])
    }

    func testReadsOffsetAndReferenceDeltaChainsOnDemand() throws {
        let git = try makeTemp("forkmesh-pack-store-") + "/.git"
        let (pack, index, ids, bodies) = try chainPack()
        try install(pack, index, in: git)
        let parsed = try GitPackReader.parse(pack, .standard)
        let store = GitPackObjectStore(git, .standard)
        for (id, body) in zip(ids, bodies).reversed() {
            XCTAssertTrue(try store.contains(id))
            let frame = try store.read(id)
            XCTAssertEqual(frame.type, .blob)
            XCTAssertEqual(frame.body, body)
            XCTAssertEqual(parsed.lookup(id)?.body, body)
        }
        XCTAssertFalse(try store.contains(fixtureId(9)))
        assertGitError(.invalidObject) { _ = try store.read(fixtureId(9)) }
        let screened = GitPackObjectStore(git, .standard, detector: GitSha1DCDetector())
        XCTAssertEqual(try screened.read(ids[2]).body, bodies[2])
    }

    func testRejectsAnIndexThatPointsAtTheWrongObject() throws {
        let git = try makeTemp("forkmesh-pack-store-") + "/.git"
        let first = utf8("first\n")
        let second = utf8("second\n")
        var hand = HandPack()
        _ = try hand.add(blobId(first), header: sizeHeader(3, first.count), payload: first)
        _ = try hand.add(blobId(second), header: sizeHeader(3, second.count), payload: second)
        let (pack, checksum) = hand.finished()
        let swapped = [
            GitPackIndexWriteEntry(blobId(first), offset: hand.entries[1].offset, crc32: hand.entries[1].crc32),
            GitPackIndexWriteEntry(blobId(second), offset: hand.entries[0].offset, crc32: hand.entries[0].crc32),
        ]
        try install(pack, try GitPackIndexWriter.write(swapped, checksum), in: git)
        assertGitError(.objectHashMismatch) { _ = try GitPackObjectStore(git, .standard).read(blobId(first)) }
    }

    func testRejectsADeltaCycleAndAMismatchedIndex() throws {
        let git = try makeTemp("forkmesh-pack-store-") + "/.git"
        let loop = fixtureId(7)
        var hand = HandPack()
        let delta: [UInt8] = [1, 1, 1] + utf8("x")
        _ = try hand.add(loop, header: sizeHeader(7, delta.count) + loop.bytes, payload: delta)
        let (pack, checksum) = hand.finished()
        try install(pack, try GitPackIndexWriter.write(hand.entries, checksum), in: git)
        assertGitError(.packCorrupt) { _ = try GitPackObjectStore(git, .standard).read(loop) }

        let other = try makeTemp("forkmesh-pack-store-") + "/.git"
        let (chain, _, ids, _) = try chainPack()
        let foreign = try GitPackIndexWriter.write(
            [GitPackIndexWriteEntry(ids[0], offset: 12, crc32: 0)], [UInt8](repeating: 0xaa, count: 20))
        try install(chain, foreign, in: other)
        assertGitError(.packChecksumMismatch) { _ = try GitPackObjectStore(other, .standard).read(ids[0]) }
    }

    func testFindsAPackInstalledAfterAnEarlierLookup() throws {
        let git = try makeTemp("forkmesh-pack-store-") + "/.git"
        let store = GitPackObjectStore(git, .standard)
        let (pack, index, ids, bodies) = try chainPack()
        XCTAssertFalse(try store.contains(ids[0]))
        try install(pack, index, in: git)
        XCTAssertTrue(try store.contains(ids[0]))
        XCTAssertEqual(try store.read(ids[1]).body, bodies[1])
    }

    func testReadsEveryObjectOfADeepGitDeltaPackLikeTheFullParser() throws {
        guard let git = GitBinary.locate() else { throw XCTSkip("git is not installed") }
        let source = try makeTemp("forkmesh-pack-store-git-")
        try git.run(["init", "--quiet", source])
        var lines = (0..<120).map { "line \($0) of a file that changes a little in every commit\n" }
        for round in 0..<24 {
            lines[(round * 5) % lines.count] = "round \(round) rewrote this line\n"
            try Data(utf8(lines.joined())).write(to: URL(fileURLWithPath: source + "/big.txt"))
            try Data(utf8("small \(round)\n")).write(to: URL(fileURLWithPath: source + "/small.txt"))
            try git.run(["-C", source, "add", "."])
            try git.run(["-C", source, "commit", "--quiet", "-m", "round \(round)"])
        }
        try git.run(["-C", source, "repack", "-a", "-d", "-f", "--depth=50", "--window=50", "--quiet"])
        let packDirectory = source + "/.git/objects/pack"
        let packName = try XCTUnwrap(try FileManager.default.contentsOfDirectory(atPath: packDirectory).first { $0.hasSuffix(".pack") })
        let pack = try GitPackReader.parse([UInt8](try Data(contentsOf: URL(fileURLWithPath: "\(packDirectory)/\(packName)"))), .standard)
        let store = GitPackObjectStore(source + "/.git", .standard)
        for object in pack.objects {
            let frame = try store.read(object.objectId)
            XCTAssertEqual(frame.type, object.type)
            XCTAssertEqual(frame.body, object.body)
        }
        XCTAssertGreaterThan(pack.objects.count, 70)
        let verified = try git.run(["-C", source, "verify-pack", "-v", "\(packDirectory)/\(packName.replacingOccurrences(of: ".pack", with: ".idx"))"])
        XCTAssertTrue(verified.contains("chain length = 2"), verified)
    }

    func testUntrustedObjectsScreenedWithSha1DCKeepTheirSha1() throws {
        for size in [0, 1, 55, 56, 63, 64, 65, 1000, 100_003] {
            let body = (0..<size).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ 7) }
            let screened = try Sha1CollisionScreen(detector: GitSha1DCDetector()).screenObject(
                type: .blob, body: body, origin: .untrustedTransport, limits: .standard)
            XCTAssertEqual(screened.decision, .approved, "size \(size)")
            XCTAssertEqual(screened.digest, blobId(body).bytes, "size \(size)")
            let session = try GitSha1DCDetector().start()
            try session.add(Array(body.prefix(size / 2)))
            try session.add(body[(size / 2)...])
            XCTAssertEqual(try session.close(), .clear)
        }
        let unscreened = try Sha1CollisionScreen().screenObject(
            type: .blob, body: utf8("x\n"), origin: .untrustedTransport, limits: .standard)
        XCTAssertEqual(unscreened.decision, .screeningUnavailable)
    }

    func testPackParsingReportsResolvedObjects() throws {
        let (pack, _, _, _) = try chainPack()
        var reported = [(Int, Int)]()
        _ = try GitPackReader.parse(pack, .standard) { resolved, total in reported.append((resolved, total)) }
        XCTAssertEqual(reported.first?.0, 0)
        XCTAssertEqual(reported.last?.0, 3)
        XCTAssertEqual(reported.last?.1, 3)
    }
}
