// The pack reader, index, writer and cancellation. The pack fixtures are built in code.

import Foundation
import XCTest
@testable import FMGit

private func uint32(_ value: UInt32) -> [UInt8] { uint32Bytes(value) }

private func finishPack(_ bytes: [UInt8]) -> [UInt8] { bytes + ForkMeshSha1.hash(bytes) }

private func singleBlobPack(_ body: [UInt8]) throws -> [UInt8] {
    finishPack(utf8("PACK") + uint32(2) + uint32(1) + [UInt8(0x30 | body.count)] + (try GitZlib.deflate(body)))
}

private let deltaBytes: [UInt8] = [6, 12, 0x90, 6, 6] + utf8("world\n")

private func refDeltaPack() throws -> [UInt8] {
    let base = utf8("hello\n")
    let baseId = GitObjectId.sha1Of(GitObjectFrame.encode(.blob, base))
    var bytes = utf8("PACK") + uint32(2) + uint32(2) + [0x36] + (try GitZlib.deflate(base))
    bytes += [UInt8(0x70 | deltaBytes.count)] + baseId.bytes + (try GitZlib.deflate(deltaBytes))
    return finishPack(bytes)
}

private func ofsDeltaPack() throws -> [UInt8] {
    var bytes = utf8("PACK") + uint32(2) + uint32(2) + [0x36] + (try GitZlib.deflate(utf8("hello\n")))
    let baseDistance = bytes.count - 12
    XCTAssertLessThan(baseDistance, 128)
    bytes += [UInt8(0x60 | deltaBytes.count), UInt8(baseDistance)] + (try GitZlib.deflate(deltaBytes))
    return finishPack(bytes)
}

private func singleObjectIndex(_ id: GitObjectId) -> [UInt8] {
    var body: [UInt8] = [0xff, 0x74, 0x4f, 0x63] + uint32(2)
    for index in 0..<256 { body += uint32(index < Int(id.bytes[0]) ? 0 : 1) }
    body += id.bytes + uint32(0x1234_5678) + uint32(12)
    body += [UInt8](repeating: 0xaa, count: 20)
    return body + ForkMeshSha1.hash(body)
}

final class PackTests: GitTestCase {
    func testReadsChecksummedVersion2PackWithBlobObject() throws {
        let pack = try GitPackReader.parse(try singleBlobPack(utf8("hello\n")), .standard)
        XCTAssertEqual(pack.objects.count, 1)
        XCTAssertEqual(pack.objects[0].type, .blob)
        XCTAssertEqual(text(pack.objects[0].body), "hello\n")
        XCTAssertEqual(pack.objects[0].objectId.hex, "ce013625030ba8dba906f756967f9e9ca394464a")
        XCTAssertEqual(pack.objects[0].offset, 12)
        XCTAssertNotNil(pack.lookup(pack.objects[0].objectId))
        XCTAssertNil(pack.lookup(fixtureId(1)))
    }

    func testRejectsPackWhoseTrailerChecksumHasChanged() throws {
        var bytes = try singleBlobPack(utf8("hello\n"))
        bytes[bytes.count - 1] ^= 1
        assertGitError(.packChecksumMismatch) { _ = try GitPackReader.parse(bytes, .standard) }
    }

    func testRejectsStructuralDamageWithTypedErrors() throws {
        let good = try singleBlobPack(utf8("hello\n"))
        func rebuilt(_ mutate: (inout [UInt8]) -> Void) -> [UInt8] {
            var body = Array(good.dropLast(20))
            mutate(&body)
            return finishPack(body)
        }
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(rebuilt { $0[0] = 0x51 }, .standard) }
        assertGitError(.unsupportedRepositoryFormat) { _ = try GitPackReader.parse(rebuilt { $0[7] = 3 }, .standard) }
        // Declared size no longer matches the inflated body.
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(rebuilt { $0[12] = 0x35 }, .standard) }
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(rebuilt { $0[12] = 0x37 }, .standard) }
        // A second object is announced but absent.
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(rebuilt { $0[11] = 2 }, .standard) }
        // Bytes between the last object and the trailer.
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(rebuilt { $0.append(0) }, .standard) }
        // A broken zlib header and a broken deflate body.
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(rebuilt { $0[13] = 0x79 }, .standard) }
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(rebuilt { $0[15] ^= 0xff }, .standard) }
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(Array(good.prefix(20)), .standard) }
        assertGitError(.resourceLimitExceeded) {
            _ = try GitPackReader.parse(good, try GitResourceLimits(maxObjectBytes: 5))
        }
    }

    func testResolvesReferenceAndOffsetDeltasWithCheckedCopyRanges() throws {
        for bytes in [try refDeltaPack(), try ofsDeltaPack()] {
            let pack = try GitPackReader.parse(bytes, .standard)
            XCTAssertEqual(pack.objects.count, 2)
            XCTAssertEqual(text(pack.objects[1].body), "hello\nworld\n")
            XCTAssertEqual(pack.objects[1].type, .blob)
        }
    }

    func testThinPackAndBadDeltasAreRejected() throws {
        let orphan = utf8("PACK") + uint32(2) + uint32(1) + [UInt8(0x70 | deltaBytes.count)] + fixtureId(7).bytes
            + (try GitZlib.deflate(deltaBytes))
        // The search for a missing base reaches the delta itself
        // first, so a thin pack surfaces as a delta cycle.
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(finishPack(orphan), .standard) }
        // Copy range past the end of the base.
        let badCopy: [UInt8] = [6, 12, 0x90, 7, 6] + utf8("world\n")
        var bytes = utf8("PACK") + uint32(2) + uint32(2) + [0x36] + (try GitZlib.deflate(utf8("hello\n")))
        let distance = bytes.count - 12
        bytes += [UInt8(0x60 | badCopy.count), UInt8(distance)] + (try GitZlib.deflate(badCopy))
        assertGitError(.packCorrupt) { _ = try GitPackReader.parse(finishPack(bytes), .standard) }
    }

    func testFailsClosedAfterReconstructingUntrustedPackObjectFrame() throws {
        let bytes = try refDeltaPack()
        assertGitError(.hashScreeningUnavailable) {
            _ = try GitPackReader.parse(bytes, .standard, origin: .untrustedTransport)
        }
    }

    func testScreensEachReconstructedUntrustedDeltaFrameWithDetector() throws {
        let pack = try GitPackReader.parse(
            try refDeltaPack(), .standard, origin: .untrustedTransport, detector: ClearDetector()
        )
        XCTAssertEqual(pack.objects.count, 2)
    }

    func testParsesIndexV2FanoutLookupAndChecksums() throws {
        let id = try GitObjectId.parseSha1("ce013625030ba8dba906f756967f9e9ca394464a")
        let index = try GitPackIndexV2.parse(singleObjectIndex(id), .standard)
        XCTAssertEqual(index.packChecksum, [UInt8](repeating: 0xaa, count: 20))
        XCTAssertEqual(index.lookup(id)?.offset, 12)
        XCTAssertEqual(index.lookup(id)?.crc32, 0x1234_5678)
        XCTAssertNil(index.lookup(fixtureId(1)))
    }

    func testRejectsIndexChecksumMismatchAndNonmonotonicFanout() throws {
        let id = try GitObjectId.parseSha1("ce013625030ba8dba906f756967f9e9ca394464a")
        var checksumChanged = singleObjectIndex(id)
        checksumChanged[0] ^= 1
        var fanoutChanged = singleObjectIndex(id)
        fanoutChanged[8 + 4] = 1
        assertGitError { _ = try GitPackIndexV2.parse(checksumChanged, .standard) }
        assertGitError { _ = try GitPackIndexV2.parse(fanoutChanged, .standard) }
    }

    func testWritesNonDeltaPackAndMatchingIndexThatReopens() throws {
        let written = try GitPackWriter.write([
            try GitPackWriteObject(.blob, utf8("hello\n")),
            try GitPackWriteObject(.blob, [0, 1, 0xff]),
        ])
        let pack = try GitPackReader.parse(written.packBytes, .standard)
        let index = try GitPackIndexV2.parse(written.indexBytes, .standard)
        XCTAssertEqual(pack.objects.count, 2)
        XCTAssertEqual(index.packChecksum, pack.checksum)
        for object in pack.objects {
            let entry = index.lookup(object.objectId)
            XCTAssertEqual(entry?.offset, object.offset)
            XCTAssertEqual(entry?.crc32, object.crc32)
        }
        assertGitError(.packCorrupt) {
            _ = try GitPackWriter.write([try GitPackWriteObject(.blob, [1]), try GitPackWriteObject(.blob, [1])])
        }
    }

    func testCrc32MatchesTheIeeeCheckValue() {
        XCTAssertEqual(gitCrc32(utf8("123456789")), 0xcbf4_3926)
        XCTAssertEqual(gitCrc32(utf8("xx123456789yy"), 2, 11), 0xcbf4_3926)
    }

    func testRefusesToReturnPackWhenCancellationWasRequested() throws {
        let cancellation = GitCancellationToken()
        cancellation.cancel()
        assertGitError(.cancelled) {
            _ = try GitPackWriter.write([try GitPackWriteObject(.blob, [1])], cancellation: cancellation)
        }
        let bytes = try refDeltaPack()
        assertGitError(.cancelled) { _ = try GitPackReader.parse(bytes, .standard, cancellation: cancellation) }
    }

    func testGitVerifiesAWrittenPackAndTheReaderReadsGitsDeltaPack() throws {
        guard let git = GitBinary.locate() else { throw XCTSkip("git is not installed") }
        let root = try makeTemp("forkmesh-pack-interop-")
        try git.run(["init", "--quiet", root])
        let written = try GitPackWriter.write([
            try GitPackWriteObject(.blob, utf8("hello\n")),
            try GitPackWriteObject(.blob, [UInt8](repeating: 7, count: 100_000)),
        ])
        let name = "pack-" + encodeLowerHex(Array(written.packBytes.suffix(20)))
        let packDirectory = root + "/.git/objects/pack"
        try FileManager.default.createDirectory(atPath: packDirectory, withIntermediateDirectories: true)
        try Data(written.packBytes).write(to: URL(fileURLWithPath: "\(packDirectory)/\(name).pack"))
        try Data(written.indexBytes).write(to: URL(fileURLWithPath: "\(packDirectory)/\(name).idx"))
        try git.run(["-C", root, "verify-pack", "-v", "\(packDirectory)/\(name).idx"])
        XCTAssertEqual(try git.run(["-C", root, "cat-file", "-p", "ce013625030ba8dba906f756967f9e9ca394464a"]), "hello\n")

        // A pack git wrote, with real deltas between two similar large files.
        let source = try makeTemp("forkmesh-pack-interop-source-")
        try git.run(["init", "--quiet", source])
        var content = [UInt8]()
        for line in 0..<2000 { content += utf8("line \(line) of a file long enough to be worth a delta\n") }
        try Data(content).write(to: URL(fileURLWithPath: source + "/big.txt"))
        try git.run(["-C", source, "add", "."])
        try git.run(["-C", source, "commit", "--quiet", "-m", "one"])
        try Data(content + utf8("one more line\n")).write(to: URL(fileURLWithPath: source + "/big.txt"))
        try git.run(["-C", source, "add", "."])
        try git.run(["-C", source, "commit", "--quiet", "-m", "two"])
        try git.run(["-C", source, "repack", "-a", "-d", "-f", "--quiet"])
        let packs = try FileManager.default.contentsOfDirectory(atPath: source + "/.git/objects/pack")
        let packName = try XCTUnwrap(packs.first { $0.hasSuffix(".pack") })
        let packBytes = [UInt8](try Data(contentsOf: URL(fileURLWithPath: source + "/.git/objects/pack/" + packName)))
        let pack = try GitPackReader.parse(packBytes, .standard)
        XCTAssertEqual(pack.objects.count, 6)
        let head = try git.run(["-C", source, "rev-parse", "HEAD"]).trimmingCharacters(in: .whitespacesAndNewlines)
        XCTAssertEqual(pack.lookup(try GitObjectId.parseSha1(head))?.type, .commit)
        let idxName = packName.replacingOccurrences(of: ".pack", with: ".idx")
        let index = try GitPackIndexV2.parse(
            [UInt8](try Data(contentsOf: URL(fileURLWithPath: source + "/.git/objects/pack/" + idxName))), .standard
        )
        for object in pack.objects {
            XCTAssertEqual(index.lookup(object.objectId)?.offset, object.offset)
            XCTAssertEqual(index.lookup(object.objectId)?.crc32, object.crc32)
        }
        // The store resolves objects through git's index.
        let store = GitPackObjectStore(source + "/.git", .standard)
        XCTAssertTrue(try store.contains(try GitObjectId.parseSha1(head)))
        XCTAssertEqual(try store.read(try GitObjectId.parseSha1(head)).type, .commit)
        XCTAssertFalse(try store.contains(fixtureId(1)))
    }
}
