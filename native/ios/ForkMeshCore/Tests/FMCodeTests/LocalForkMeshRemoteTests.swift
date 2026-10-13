import FMBase
import FMCrypto
import FMGit
import Foundation
import XCTest
@testable import FMCode

/// sha256 of the empty byte string — what the advertisement GET signs.
private let emptyBodySha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

/// Records the request the signing transport hands to the real transport.
private final class CapturingTransport: GitHttpTransport, @unchecked Sendable {
    var last: GitHttpRequest?
    var closed = 0

    func send(_ request: GitHttpRequest, cancellation: GitCancellationToken?, limits: GitResourceLimits?) async throws -> GitHttpResponse {
        last = request
        return GitHttpResponse(statusCode: 200, headers: ["content-type": "application/x-git-receive-pack-result"], body: [])
    }

    func close() async { closed += 1 }
}

final class LocalForkMeshRemoteTests: XCTestCase {
    private let seed: [UInt8] = (0..<32).map { UInt8($0 + 1) }

    private func sha(_ text: String) -> String { Hex.encode(FMCrypto.Sha256.hash(Array(text.utf8))) }

    // MARK: Push canonical string

    func testCanonicalIsExactlyTheSixNewlineJoinedFields() {
        let canonical = forkMeshPushCanonical(owner: "alice", repo: "demo-repo", method: "POST", ts: "1750000000000", body: Array("pack".utf8))
        XCTAssertEqual(canonical, "forkmesh-git-push-v1\nalice\ndemo-repo\nPOST\n1750000000000\n\(sha("pack"))")
        XCTAssertEqual(canonical.filter { $0 == "\n" }.count, 5)
        XCTAssertFalse(canonical.hasSuffix("\n"))
    }

    func testCanonicalNormalizesOwnerAndMethodLikeEntryPy() {
        XCTAssertEqual(
            forkMeshPushCanonical(owner: "  ALICE ", repo: " demo ", method: "get", ts: "17", body: []),
            "forkmesh-git-push-v1\nalice\ndemo\nGET\n17\n\(emptyBodySha)")
    }

    func testAdvertisementGetSignsTheEmptyBodyDigest() {
        XCTAssertEqual(forkMeshBodyDigest([]), emptyBodySha)
        XCTAssertEqual(
            forkMeshPushCanonical(owner: "alice", repo: "demo", method: "GET", ts: "1", body: []).split(separator: "\n").last.map(String.init),
            emptyBodySha)
    }

    // MARK: Credentials

    func testCredentialsNormalizeAWsRelayUrlOntoTheHttpsOrigin() {
        let creds = ForkMeshRemoteCredentials(origin: "ws://relay.test:8787/ws", owner: "alice", repo: "demo")
        XCTAssertEqual(creds.origin, "http://relay.test:8787")
        XCTAssertEqual(creds.remoteUrl, "http://relay.test:8787/alice/demo")
        XCTAssertEqual(ForkMeshRemoteCredentials(origin: "wss://forkmesh.test/ws", owner: "a", repo: "b").origin, "https://forkmesh.test")
        XCTAssertEqual(ForkMeshRemoteCredentials(origin: "   ", owner: "a", repo: "b").origin, "")
    }

    func testCanSignNeedsASeedAndAPubkey() {
        XCTAssertFalse(ForkMeshRemoteCredentials(origin: "https://f.test", owner: "a", repo: "b").canSign)
        let signed = ForkMeshRemoteCredentials(origin: "https://f.test", owner: "a", repo: "b", seed: seed, pubkeyB64url: "pub")
        XCTAssertTrue(signed.canSign)
        XCTAssertFalse(signed.anonymous.canSign)
        XCTAssertEqual(signed.anonymous.origin, signed.origin)
    }

    // MARK: Signing transport

    private func build(_ inner: CapturingTransport, signed: Bool = true) throws -> (ForkMeshSigningTransport, [UInt8]) {
        let publicKey = try Ed25519.publicKey(seed: seed)
        let transport = ForkMeshSigningTransport(
            credentials: ForkMeshRemoteCredentials(
                origin: "https://forkmesh.test", owner: "Alice", repo: "demo",
                seed: signed ? seed : nil, pubkeyB64url: signed ? B64.encodeUrlNoPadding(publicKey) : ""),
            inner: inner,
            clock: { Date(epochMillis: 1_750_000_000_000) })
        return (transport, publicKey)
    }

    func testAttachesTheHeadersAndAVerifiableSignature() async throws {
        let inner = CapturingTransport()
        let (transport, publicKey) = try build(inner)
        let body = Array("0000PACK".utf8)
        _ = try await transport.send(
            try GitHttpRequest(
                method: "POST",
                url: URL(string: "https://forkmesh.test/Alice/demo/git-receive-pack")!,
                headers: ["content-type": "application/x-git-receive-pack"],
                body: body,
                maxResponseBytes: 1024,
                expectedContentType: "application/x-git-receive-pack-result"),
            cancellation: nil, limits: nil)
        let sent = try XCTUnwrap(inner.last)
        XCTAssertEqual(sent.headers["x-forkmesh-pubkey"], B64.encodeUrlNoPadding(publicKey))
        XCTAssertEqual(sent.headers["x-forkmesh-ts"], "1750000000000")
        XCTAssertEqual(sent.headers["x-forkmesh-body-sha256"], Hex.encode(FMCrypto.Sha256.hash(body)))
        XCTAssertEqual(sent.headers["content-type"], "application/x-git-receive-pack")
        XCTAssertEqual(sent.body, body)

        let canonical = "forkmesh-git-push-v1\nalice\ndemo\nPOST\n1750000000000\n\(Hex.encode(FMCrypto.Sha256.hash(body)))"
        let signature = [UInt8](try B64.decodeUnpadded(try XCTUnwrap(sent.headers["x-forkmesh-sig"])))
        XCTAssertTrue(Ed25519.verify(publicKey: publicKey, message: Array(canonical.utf8), signature: signature))
        XCTAssertFalse(Ed25519.verify(publicKey: publicKey, message: Array("\(canonical) ".utf8), signature: signature))
    }

    func testSignsTheEmptyBodyDigestForTheAdvertisementGet() async throws {
        let inner = CapturingTransport()
        let (transport, publicKey) = try build(inner)
        _ = try await transport.send(
            try GitHttpRequest(
                method: "GET",
                url: URL(string: "https://forkmesh.test/Alice/demo/info/refs?service=git-receive-pack")!,
                maxResponseBytes: 1024,
                expectedContentType: "application/x-git-receive-pack-advertisement"),
            cancellation: nil, limits: nil)
        let signature = [UInt8](try B64.decodeUnpadded(try XCTUnwrap(inner.last?.headers["x-forkmesh-sig"])))
        XCTAssertTrue(Ed25519.verify(
            publicKey: publicKey,
            message: Array("forkmesh-git-push-v1\nalice\ndemo\nGET\n1750000000000\n\(emptyBodySha)".utf8),
            signature: signature))
    }

    func testAnAnonymousReadCarriesNoForkMeshHeaders() async throws {
        let inner = CapturingTransport()
        let (transport, _) = try build(inner, signed: false)
        _ = try await transport.send(
            try GitHttpRequest(
                method: "GET",
                url: URL(string: "https://forkmesh.test/Alice/demo/info/refs")!,
                maxResponseBytes: 1024,
                expectedContentType: "application/x-git-upload-pack-advertisement"),
            cancellation: nil, limits: nil)
        XCTAssertTrue(inner.last?.headers.keys.filter { $0.hasPrefix("x-forkmesh") }.isEmpty ?? false)
    }

    func testCloseClosesTheInnerTransport() async throws {
        let inner = CapturingTransport()
        let (transport, _) = try build(inner)
        await transport.close()
        XCTAssertEqual(inner.closed, 1)
    }

    // MARK: Failure classification

    private func push(_ code: GitErrorCode, _ message: String = "") -> ForkMeshRemoteFailure {
        describeRemoteFailure(GitException(code, message), push: true)
    }

    func testRecoversTheWorkerStatusFromTheEngineMessage() {
        XCTAssertEqual(forkMeshHttpStatus("remote returned HTTP 413"), 413)
        XCTAssertNil(forkMeshHttpStatus("nothing to see"))
    }

    func test413IsPushTooLargeAndNamesThe8MBCap() {
        let failure = push(.remoteRejected, "remote returned HTTP 413")
        XCTAssertEqual(failure.code, "push_too_large")
        XCTAssertTrue(failure.message.contains("8 MB"))
    }

    func test401And403ArePushUnauthorized() {
        XCTAssertEqual(push(.remoteRejected, "remote returned HTTP 401").code, "push_unauthorized")
        XCTAssertEqual(push(.remoteRejected, "remote returned HTTP 403").code, "push_unauthorized")
        XCTAssertEqual(push(.authenticationFailed).code, "push_unauthorized")
    }

    func test501IsPushUnsupportedAndNamesTheRelay() {
        let failure = describeRemoteFailure(
            GitException(.remoteRejected, "remote returned HTTP 501"), push: true, host: "forkmesh-relay.example.workers.dev")
        XCTAssertEqual(failure.code, "push_unsupported")
        XCTAssertTrue(failure.message.contains("does not accept pushes from mobile"))
        XCTAssertTrue(failure.message.contains("forkmesh-relay.example.workers.dev"))
        XCTAssertTrue(push(.remoteRejected, "remote returned HTTP 501").message.contains("This relay does not accept pushes"))
    }

    func testRemoteHostKeepsANonDefaultPortAndDropsThePath() {
        XCTAssertEqual(forkMeshRemoteHost("https://app.forkmesh.com"), "app.forkmesh.com")
        XCTAssertEqual(forkMeshRemoteHost("http://127.0.0.1:8787"), "127.0.0.1:8787")
        XCTAssertEqual(forkMeshRemoteHost("   "), "")
    }

    func test503IsPushUnavailableOnAPushOfflineOnARead() {
        let pushed = push(.remoteRejected, "remote returned HTTP 503")
        XCTAssertEqual(pushed.code, "push_unavailable")
        XCTAssertTrue(pushed.message.contains("accept this push"))
        XCTAssertTrue(describeRemoteFailure(
            GitException(.remoteRejected, "remote returned HTTP 503"), push: true, host: "app.forkmesh.com"
        ).message.contains("app.forkmesh.com"))
        let read = describeRemoteFailure(GitException(.remoteRejected, "remote returned HTTP 503"))
        XCTAssertEqual(read.code, "push_unavailable")
        XCTAssertTrue(read.message.contains("serving this repository"))
    }

    func testNonFastForwardTellsTheUserToFetchAndRetry() {
        let failure = push(.nonFastForward)
        XCTAssertEqual(failure.code, "non_fast_forward")
        XCTAssertEqual(failure.message, "Someone else pushed first — fetch and retry.")
    }

    func testResourceLimitsDifferBetweenPushAndClone() {
        XCTAssertTrue(push(.resourceLimitExceeded).message.contains("8 MB"))
        XCTAssertTrue(describeRemoteFailure(GitException(.resourceLimitExceeded, "")).message.contains("too large to clone"))
    }

    func testStableCodesFor404NetworkTlsCancelled() {
        XCTAssertEqual(push(.remoteRejected, "remote returned HTTP 404").code, "not_found")
        XCTAssertEqual(push(.networkUnavailable).code, "network_unavailable")
        XCTAssertEqual(push(.tlsRejected).code, "tls_rejected")
        XCTAssertEqual(push(.cancelled).code, "cancelled")
        XCTAssertEqual(push(.protocolUnsupported).code, "protocol_unsupported")
    }

    func testANonGitErrorIsPassedThroughAsRemoteRejected() {
        struct Boom: Error, CustomStringConvertible { var description: String { "Bad state: boom" } }
        let failure = describeRemoteFailure(Boom())
        XCTAssertEqual(failure.code, "remote_rejected")
        XCTAssertTrue(failure.message.contains("boom"))
    }

    // MARK: Blob ids

    func testBlobIdMatchesGitHashObject() {
        // `printf 'hello\n' | git hash-object --stdin`
        XCTAssertEqual(GitBlobId.of(Array("hello\n".utf8)).hex, "ce013625030ba8dba906f756967f9e9ca394464a")
        // The empty blob.
        XCTAssertEqual(GitBlobId.of([UInt8]()).hex, "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391")
    }

    func testServedBlobIdAgreement() {
        let id = GitBlobId.of(Array("hello\n".utf8))
        XCTAssertTrue(GitBlobId.servedAgrees("", id))
        XCTAssertTrue(GitBlobId.servedAgrees("not-a-sha", id))
        XCTAssertTrue(GitBlobId.servedAgrees(" CE013625030BA8DBA906F756967F9E9CA394464A ", id))
        XCTAssertFalse(GitBlobId.servedAgrees(String(repeating: "a", count: 40), id))
    }
}
