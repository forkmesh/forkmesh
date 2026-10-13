import XCTest
@testable import FMGit

final class BytesAndCryptoTests: XCTestCase {
    func testRejectsDeclaredLengthBeyondRemainingInput() {
        let cursor = CheckedBytesReader([0, 1, 2])
        assertGitError { _ = try cursor.readBytes(4) }
    }

    func testWritesAndReadsIntegersInNetworkByteOrder() throws {
        let writer = CheckedBytesWriter()
        try writer.writeUint32(0x5041_434b)
        let bytes = writer.takeBytes()
        XCTAssertEqual(bytes, [0x50, 0x41, 0x43, 0x4b])
        XCTAssertEqual(try CheckedBytesReader(bytes).readUint32(), 0x5041_434b)
    }

    func testRejectsNoncanonicalLowercaseHexadecimal() throws {
        assertGitError { _ = try decodeLowerHex("CE013625") }
        assertGitError { _ = try decodeLowerHex("abc") }
        XCTAssertEqual(encodeLowerHex(try decodeLowerHex("ce013625")), "ce013625")
    }

    func testCanonicalBase64Url() throws {
        XCTAssertEqual(encodeCanonicalBase64Url([0xfb, 0xff]), "-_8")
        XCTAssertEqual(try decodeCanonicalBase64Url("-_8"), [0xfb, 0xff])
        assertGitError { _ = try decodeCanonicalBase64Url("-_8=") }
        assertGitError { _ = try decodeCanonicalBase64Url("-_9") }
    }

    func testEmptyInputFipsVectors() throws {
        XCTAssertEqual(encodeLowerHex(try ForkMeshSha1().close()), "da39a3ee5e6b4b0d3255bfef95601890afd80709")
        XCTAssertEqual(
            encodeLowerHex(try ForkMeshSha256().close()),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        )
    }

    func testSha1AcrossChunkBoundaries() throws {
        let digest = ForkMeshSha1()
        try digest.add(utf8("a"))
        try digest.add(utf8("bc"))
        XCTAssertEqual(encodeLowerHex(try digest.close()), "a9993e364706816aba3e25717850c26c9cd0d89d")
    }

    func testSha256AcrossChunkBoundaries() throws {
        let digest = ForkMeshSha256()
        try digest.add(utf8("a"))
        try digest.add(utf8("bc"))
        XCTAssertEqual(
            encodeLowerHex(try digest.close()),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        )
    }

    func testMultiBlockFipsVectors() throws {
        let input = utf8("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
        let sha1 = ForkMeshSha1()
        try sha1.add(input)
        XCTAssertEqual(encodeLowerHex(try sha1.close()), "84983e441c3bd26ebaae4aa1f95129e5e54670f1")
        let sha256 = ForkMeshSha256()
        try sha256.add(input)
        XCTAssertEqual(
            encodeLowerHex(try sha256.close()),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        )
    }

    func testOneMillionBytesAcrossBoundarySizedChunks() throws {
        let sha1 = ForkMeshSha1()
        let sha256 = ForkMeshSha256()
        let chunkSizes = [1, 55, 56, 63, 64, 65, 17]
        var written = 0
        var chunk = 0
        while written < 1_000_000 {
            let length = min(chunkSizes[chunk % chunkSizes.count], 1_000_000 - written)
            let bytes = [UInt8](repeating: 0x61, count: length)
            try sha1.add(bytes)
            try sha256.add(bytes)
            written += length
            chunk += 1
        }
        XCTAssertEqual(encodeLowerHex(try sha1.close()), "34aa973cd4c4daa4f61eeb2bdbad27316534016f")
        XCTAssertEqual(
            encodeLowerHex(try sha256.close()),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
        )
    }

    func testRejectInputBeyondConfiguredLengthAccounting() {
        assertGitError(.hashImplementationFailure) { try ForkMeshSha1(maxInputBytes: 2).add([1, 2, 3]) }
        assertGitError(.hashImplementationFailure) { try ForkMeshSha256(maxInputBytes: 2).add([1, 2, 3]) }
    }

    func testClosedDigestRejectsFurtherUse() throws {
        let digest = ForkMeshSha1()
        _ = try digest.close()
        XCTAssertThrowsError(try digest.add([1])) { XCTAssertTrue($0 is GitStateError) }
        XCTAssertThrowsError(try digest.close()) { XCTAssertTrue($0 is GitStateError) }
    }

    func testCollisionScreenKeepsUntrustedTransportFailClosed() throws {
        let frame = GitObjectFrame.encode(.blob, [1])
        let result = try Sha1CollisionScreen().screenFrame(
            frame: frame, objectType: .blob, declaredBodySize: 1,
            origin: .untrustedTransport, limits: .standard
        )
        XCTAssertEqual(result.decision, .screeningUnavailable)
    }

    func testResourceLimitsRejectOutOfRangeValues() {
        XCTAssertThrowsError(try GitResourceLimits(maxObjectBytes: 0)) { XCTAssertTrue($0 is GitArgumentError) }
        XCTAssertThrowsError(try GitResourceLimits(maxObjectBytes: GitResourceLimits.hardMaxObjectBytes + 1))
        XCTAssertThrowsError(try GitResourceLimits(connectTimeout: 31))
        XCTAssertNoThrow(try GitResourceLimits(connectTimeout: 30))
    }

    func testZlibRoundTripReportsConsumedInput() throws {
        let body = [UInt8](repeating: 0x41, count: 200_000) + utf8("tail")
        let compressed = try GitZlib.deflate(body)
        let padded = [9, 9] + compressed + [7, 7, 7]
        let inflated = try GitZlib.inflate(padded, start: 2, maximumOutput: body.count)
        XCTAssertEqual(inflated.output, body)
        XCTAssertEqual(inflated.consumed, compressed.count)
        XCTAssertThrowsError(try GitZlib.inflate(compressed, maximumOutput: body.count - 1)) {
            XCTAssertEqual($0 as? GitZlibFailure, .outputLimitExceeded)
        }
        XCTAssertThrowsError(try GitZlib.inflate(Array(compressed.dropLast(3)), maximumOutput: body.count)) {
            XCTAssertEqual($0 as? GitZlibFailure, .truncated)
        }
        XCTAssertThrowsError(try GitZlib.inflate([0x78, 0x9c, 0xff, 0xff, 0xff, 0xff], maximumOutput: 10)) {
            XCTAssertEqual($0 as? GitZlibFailure, .invalidStream)
        }
    }
}
