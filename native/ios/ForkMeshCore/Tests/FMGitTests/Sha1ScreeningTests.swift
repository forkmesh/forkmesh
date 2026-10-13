import XCTest
@testable import FMGit

final class Sha1ScreeningTests: XCTestCase {
    func testInjectedClearDetectorPermitsCanonicalUntrustedFrame() throws {
        let frame = GitObjectFrame.encode(.blob, [1])
        let result = try Sha1CollisionScreen(detector: ClearDetector()).screenFrame(
            frame: frame, objectType: .blob, declaredBodySize: 1,
            origin: .untrustedTransport, limits: .standard
        )
        XCTAssertEqual(result.decision, .approved)
    }

    func testScreensCompleteFrameWithTrustedAndUntrustedPolicy() throws {
        let frame = GitObjectFrame.encode(.blob, utf8("hello\n"))
        let screen = Sha1CollisionScreen()
        let trusted = try screen.screenFrame(
            frame: frame, objectType: .blob, declaredBodySize: 6, origin: .trustedLocal, limits: .standard
        )
        let untrusted = try screen.screenFrame(
            frame: frame, objectType: .blob, declaredBodySize: 6, origin: .untrustedTransport, limits: .standard
        )
        XCTAssertEqual(trusted.decision, .approved)
        XCTAssertEqual(trusted.digest, GitObjectId.sha1Of(frame).bytes)
        XCTAssertEqual(trusted.inspectedBytes, frame.count)
        XCTAssertEqual(untrusted.decision, .screeningUnavailable)
    }

    func testReportsMalformedFramesAndCancellationAsDistinctDecisions() throws {
        let cancellation = GitCancellationToken()
        cancellation.cancel()
        let screen = Sha1CollisionScreen()
        let malformed = try screen.screenFrame(
            frame: utf8("blob 7\u{0}hello\n"), objectType: .blob, declaredBodySize: 7,
            origin: .trustedLocal, limits: .standard
        )
        let cancelled = try screen.screenFrame(
            frame: GitObjectFrame.encode(.blob, [1]), objectType: .blob, declaredBodySize: 1,
            origin: .trustedLocal, limits: .standard, cancellation: cancellation
        )
        XCTAssertEqual(malformed.decision, .malformedFrame)
        XCTAssertEqual(cancelled.decision, .cancelled)
        XCTAssertEqual(try cancelled.toException().code, .cancelled)
    }

    func testCancellationBetweenStreamingChunksPreventsApproval() throws {
        let frame = GitObjectFrame.encode(.blob, [1, 2])
        let cancellation = GitCancellationToken()
        let session = Sha1CollisionScreen().start(
            objectType: .blob, declaredBodySize: 2, origin: .trustedLocal,
            limits: .standard, cancellation: cancellation
        )
        try session.add(Array(frame[0..<4]))
        cancellation.cancel()
        try session.add(Array(frame[4...]))
        XCTAssertEqual(try session.close().decision, .cancelled)
    }

    func testDeclaredBodiesBeyondObjectLimitDoNotReceiveApproval() throws {
        let frame = GitObjectFrame.encode(.blob, [1, 2])
        let result = try Sha1CollisionScreen().screenFrame(
            frame: frame, objectType: .blob, declaredBodySize: 2,
            origin: .trustedLocal, limits: try GitResourceLimits(maxObjectBytes: 1)
        )
        XCTAssertEqual(result.decision, .resourceLimitExceeded)
    }

    func testPreservesFullFrameDecisionAcrossIrregularChunks() throws {
        let body = utf8("tree 0000000000000000000000000000000000000000\n\nmessage\n")
        let frame = GitObjectFrame.encode(.commit, body)
        let session = Sha1CollisionScreen().start(
            objectType: .commit, declaredBodySize: body.count, origin: .trustedLocal, limits: .standard
        )
        var offset = 0
        while offset < frame.count {
            let length = min(frame.count - offset, 5)
            try session.add(Array(frame[offset..<(offset + length)]))
            offset += length
        }
        let result = try session.close()
        XCTAssertEqual(result.decision, .approved)
        XCTAssertEqual(result.digest, GitObjectId.sha1Of(frame).bytes)
    }

    func testDetectedCollisionRejectsEvenTrustedFrames() throws {
        final class Detected: GitSha1CollisionDetector, GitSha1CollisionDetectorSession {
            func start() throws -> GitSha1CollisionDetectorSession { self }
            func add(_ bytes: [UInt8]) throws {}
            func close() throws -> GitSha1CollisionDetectorStatus { .detected }
            func dispose() throws {}
        }
        let frame = GitObjectFrame.encode(.blob, [1])
        let result = try Sha1CollisionScreen(detector: Detected()).screenFrame(
            frame: frame, objectType: .blob, declaredBodySize: 1, origin: .trustedLocal, limits: .standard
        )
        XCTAssertEqual(result.decision, .collisionDetected)
    }
}
