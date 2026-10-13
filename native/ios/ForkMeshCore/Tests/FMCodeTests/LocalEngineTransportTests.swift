// The fail-closed transport and the behaviour checks of the Kotlin port's
// GitIoHttpTransportTest.kt, run
// against FMNet's FakeHTTPClient and a stalling HTTPClient instead of a TLS
// server (the certificate-challenge cases have no HTTPClient counterpart).

import FMGit
import FMNet
import Foundation
import XCTest
@testable import FMCode

/// Answers with a head and one chunk, then never sends another byte.
final class LocalEngineStallingHTTPClient: HTTPClient, @unchecked Sendable {
    private let lock = NSLock()
    private var continuations = [AsyncThrowingStream<Data, Error>.Continuation]()
    let contentType: String

    init(contentType: String) { self.contentType = contentType }

    func send(_ request: HTTPRequest) async throws -> HTTPResponse {
        throw HTTPError.transport("stalling client streams only")
    }

    func stream(_ request: HTTPRequest) async throws -> HTTPStreamedResponse {
        let body = AsyncThrowingStream<Data, Error> { continuation in
            continuation.yield(Data([1, 2, 3]))
            keep(continuation)
        }
        return HTTPStreamedResponse(status: 200, headers: ["content-type": contentType], body: body)
    }

    private func keep(_ continuation: AsyncThrowingStream<Data, Error>.Continuation) {
        lock.lock()
        continuations.append(continuation)
        lock.unlock()
    }
}

/// Never answers the head.
final class LocalEngineSilentHTTPClient: HTTPClient, @unchecked Sendable {
    func send(_ request: HTTPRequest) async throws -> HTTPResponse {
        while !Task.isCancelled { try? await Task.sleep(nanoseconds: 20_000_000) }
        throw HTTPError.cancelled
    }
}

final class LocalEngineTransportTests: LocalEngineTestCase {
    private let advertisementType = "application/x-git-upload-pack-advertisement"

    private func request(
        _ url: String = "https://forkmesh.test/repo.git/info/refs",
        maxResponseBytes: Int = 1 << 20,
        deadline: Date? = nil
    ) throws -> GitHttpRequest {
        try GitHttpRequest(
            method: "GET",
            url: URL(string: url)!,
            headers: ["Git-Protocol": "version=2", "accept": "*/*"],
            maxResponseBytes: maxResponseBytes,
            expectedContentType: advertisementType,
            deadline: deadline
        )
    }

    // MARK: fail-closed transport

    func testUntrustedPackRejectionLeavesRepositoryRefsAndObjectStoreUnchanged() throws {
        let root = try makeTemp("forkmesh-fail-closed-")
        let repository = try GitRepository.initialize(GitRepositoryInit(root))
        let head = "\(repository.gitDirectory)/HEAD"
        let headBefore = try leReadFile(head)
        let objects = "\(repository.gitDirectory)/objects"
        let before = try FileManager.default.contentsOfDirectory(atPath: objects)
        let pack = try GitPackWriter.write([
            try GitPackWriteObject(.blob, [1]),
            try GitPackWriteObject(.blob, [2]),
        ])

        leAssertGitError(.hashScreeningUnavailable) {
            _ = try GitFetchResponse.validateUntrustedPack(pack.packBytes, GitResourceLimits.standard)
        }

        XCTAssertEqual(try leReadFile(head), headBefore)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: objects).count, before.count)
    }

    // MARK: request shape

    func testSendsAFailClosedRequestThroughHTTPClient() async throws {
        let http = FakeHTTPClient()
        http.enqueue(HTTPResponse(status: 200, headers: ["Content-Type": "\(advertisementType); charset=x"], body: Data([7])))
        let transport = GitNetHttpTransport(http: http, headerProvider: { method, url, body in
            XCTAssertEqual(method, "GET")
            XCTAssertEqual(url.path, "/repo.git/info/refs")
            XCTAssertEqual(body, [])
            return ["Authorization": "Bearer t"]
        })

        let response = try await transport.send(try request())

        XCTAssertEqual(response.statusCode, 200)
        XCTAssertEqual(response.body, [7])
        XCTAssertEqual(response.contentType, "\(advertisementType); charset=x")
        let sent = try XCTUnwrap(http.lastRequest)
        XCTAssertFalse(sent.followRedirects)
        XCTAssertEqual(sent.maxResponseBytes, 1 << 20)
        XCTAssertEqual(sent.headers["authorization"], "Bearer t")
        XCTAssertEqual(sent.headers["git-protocol"], "version=2")
        XCTAssertEqual(sent.headers["accept-encoding"], "identity")
        XCTAssertEqual(sent.headers["user-agent"], "forkmesh-git/0.0.0")
        XCTAssertNil(sent.headers["cookie"])
        XCTAssertNil(sent.body)
        XCTAssertLessThanOrEqual(sent.timeout, GitResourceLimits.defaultConnectTimeout)
    }

    // MARK: GitIoHttpTransportTest.kt

    func testRedirectsAreNeverFollowed() async throws {
        let http = FakeHTTPClient()
        http.enqueue(HTTPResponse(status: 302, headers: ["Location": "https://elsewhere.test/repo"]))
        http.enqueue(HTTPResponse(status: 301, headers: ["Location": "https://forkmesh.test/other"]))
        http.enqueue(HTTPResponse(status: 307))
        let transport = GitNetHttpTransport(http: http)

        let different = await leAssertGitErrorAsync(.remoteRejected) { _ = try await transport.send(try self.request()) }
        XCTAssertEqual(
            different?.message,
            "remote redirected (302) to different host; redirects are never followed"
        )
        let same = await leAssertGitErrorAsync(.remoteRejected) { _ = try await transport.send(try self.request()) }
        XCTAssertTrue(same?.message.contains("same host") ?? false)
        let unknown = await leAssertGitErrorAsync(.remoteRejected) { _ = try await transport.send(try self.request()) }
        XCTAssertTrue(unknown?.message.contains("relative or unknown host") ?? false)
    }

    func testStatusesAndContentTypesMapToTypedErrors() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 401, text: "no")
        http.enqueue(status: 407, text: "no")
        http.enqueue(status: 500, text: "no")
        http.enqueue(HTTPResponse(status: 200, headers: ["content-type": "text/html"], text: "<html>"))
        http.enqueue(HTTPResponse(status: 200, text: "no content type"))
        let transport = GitNetHttpTransport(http: http)

        let auth = await leAssertGitErrorAsync(.authenticationFailed) { _ = try await transport.send(try self.request()) }
        XCTAssertEqual(auth?.message, "remote refused credentials (401)")
        await leAssertGitErrorAsync(.authenticationFailed) { _ = try await transport.send(try self.request()) }
        let broken = await leAssertGitErrorAsync(.remoteRejected) { _ = try await transport.send(try self.request()) }
        XCTAssertEqual(broken?.message, "remote returned HTTP 500")
        let html = await leAssertGitErrorAsync(.protocolUnsupported) { _ = try await transport.send(try self.request()) }
        XCTAssertEqual(html?.message, "response content type is not the expected Git service type")
        await leAssertGitErrorAsync(.protocolUnsupported) { _ = try await transport.send(try self.request()) }
    }

    func testResponseHeadersAndBodiesAreBounded() async throws {
        let http = FakeHTTPClient()
        http.enqueue(HTTPResponse(
            status: 200, headers: ["content-type": advertisementType, "content-length": "2048"], body: Data(count: 10)
        ))
        http.enqueue(HTTPResponse(status: 200, headers: ["content-type": advertisementType], body: Data(count: 2048)))
        http.enqueue(HTTPResponse(status: 200, headers: ["content-type": advertisementType], body: Data(count: 8)))
        let transport = GitNetHttpTransport(http: http)

        let declared = await leAssertGitErrorAsync(.resourceLimitExceeded) {
            _ = try await transport.send(try self.request(maxResponseBytes: 1024))
        }
        XCTAssertEqual(declared?.message, "response body exceeds the configured limit")
        await leAssertGitErrorAsync(.resourceLimitExceeded) {
            _ = try await transport.send(try self.request(maxResponseBytes: 1024))
        }
        let headers = await leAssertGitErrorAsync(.resourceLimitExceeded) {
            _ = try await transport.send(
                try self.request(), cancellation: nil, limits: try GitResourceLimits(maxResponseHeaderBytes: 16)
            )
        }
        XCTAssertEqual(headers?.message, "response headers exceed the configured limit")
    }

    func testCancellationAbortsARequestStalledMidBody() async throws {
        let cancellation = GitCancellationToken()
        Task {
            try? await Task.sleep(nanoseconds: 300_000_000)
            cancellation.cancel()
        }
        let started = Date()
        let transport = GitNetHttpTransport(http: LocalEngineStallingHTTPClient(contentType: advertisementType))

        await leAssertGitErrorAsync(.cancelled) {
            _ = try await transport.send(try self.request(), cancellation: cancellation, limits: nil)
        }
        XCTAssertLessThan(Date().timeIntervalSince(started), 5)
    }

    func testCancellationAbortsARequestWaitingForTheHead() async throws {
        let cancellation = GitCancellationToken()
        Task {
            try? await Task.sleep(nanoseconds: 200_000_000)
            cancellation.cancel()
        }
        let started = Date()
        let transport = GitNetHttpTransport(http: LocalEngineSilentHTTPClient())

        await leAssertGitErrorAsync(.cancelled) {
            _ = try await transport.send(try self.request(), cancellation: cancellation, limits: nil)
        }
        XCTAssertLessThan(Date().timeIntervalSince(started), 5)
        let already = GitCancellationToken()
        already.cancel()
        await leAssertGitErrorAsync(.cancelled) {
            _ = try await transport.send(try self.request(), cancellation: already, limits: nil)
        }
    }

    func testAStalledBodyFailsAfterTheIdleReadTimeoutOrTheDeadline() async throws {
        var started = Date()
        let idle = await leAssertGitErrorAsync(.networkUnavailable) {
            _ = try await GitNetHttpTransport(http: LocalEngineStallingHTTPClient(contentType: self.advertisementType))
                .send(try self.request(), cancellation: nil, limits: try GitResourceLimits(idleReadTimeout: 0.5))
        }
        XCTAssertEqual(idle?.message, "remote stalled beyond the idle read timeout")
        XCTAssertLessThan(Date().timeIntervalSince(started), 8)

        started = Date()
        let deadline = await leAssertGitErrorAsync(.networkUnavailable) {
            _ = try await GitNetHttpTransport(http: LocalEngineStallingHTTPClient(contentType: self.advertisementType))
                .send(try self.request(deadline: Date().addingTimeInterval(0.5)))
        }
        XCTAssertTrue(deadline?.message.contains("deadline") ?? false, deadline?.message ?? "")
        XCTAssertLessThan(Date().timeIntervalSince(started), 8)

        let expired = await leAssertGitErrorAsync(.networkUnavailable) {
            _ = try await GitNetHttpTransport(http: FakeHTTPClient())
                .send(try self.request(deadline: Date().addingTimeInterval(-1)))
        }
        XCTAssertEqual(expired?.message, "operation deadline exceeded")
    }

    func testTransportFailuresMapToTypedErrors() async throws {
        let http = FakeHTTPClient()
        http.enqueue(error: HTTPError.timedOut)
        http.enqueue(error: HTTPError.transport("Could not connect to the server.\u{1}"))
        http.enqueue(error: HTTPError.transport("An SSL error has occurred"))
        http.enqueue(error: HTTPError.invalidResponse)
        let transport = GitNetHttpTransport(http: http)

        let timeout = await leAssertGitErrorAsync(.networkUnavailable) { _ = try await transport.send(try self.request()) }
        XCTAssertEqual(timeout?.message, "remote did not respond within the connect timeout")
        let refused = await leAssertGitErrorAsync(.networkUnavailable) { _ = try await transport.send(try self.request()) }
        XCTAssertEqual(refused?.message, "connection failed: Could not connect to the server.")
        let tls = await leAssertGitErrorAsync(.tlsRejected) { _ = try await transport.send(try self.request()) }
        XCTAssertEqual(tls?.message, "TLS failure: An SSL error has occurred")
        await leAssertGitErrorAsync(.networkUnavailable) { _ = try await transport.send(try self.request()) }
    }

    func testHeaderProvidersRequestsAndClosedTransportsFailClosedBeforeConnecting() async throws {
        let http = FakeHTTPClient()
        let reserved = await leAssertGitErrorAsync(.authenticationFailed) {
            _ = try await GitNetHttpTransport(http: http, headerProvider: { _, _, _ in ["Content-Type": "text/plain"] })
                .send(try self.request())
        }
        XCTAssertEqual(reserved?.message, "header provider returned an invalid or reserved header")
        await leAssertGitErrorAsync(.authenticationFailed) {
            _ = try await GitNetHttpTransport(http: http, headerProvider: { _, _, _ in ["x-sig": "line\nbreak"] })
                .send(try self.request())
        }
        let failed = await leAssertGitErrorAsync(.authenticationFailed) {
            _ = try await GitNetHttpTransport(http: http, headerProvider: { _, _, _ in
                throw LocalEngineTestFailure(description: "signer offline")
            }).send(try self.request())
        }
        XCTAssertEqual(failed?.message, "header provider failed")
        let bearer = await leAssertGitErrorAsync(.authenticationFailed) {
            _ = try await GitNetHttpTransport(http: http, headerProvider: GitHttpHeaders.bearer("")).send(try self.request())
        }
        XCTAssertEqual(bearer?.message, "bearer token is not a valid header value")
        let basic = await leAssertGitErrorAsync(.authenticationFailed) {
            _ = try await GitNetHttpTransport(http: http, headerProvider: GitHttpHeaders.basic("ada:x", "secret"))
                .send(try self.request())
        }
        XCTAssertEqual(basic?.message, "basic credentials contain invalid characters")
        let url = URL(string: "https://forkmesh.test/")!
        XCTAssertEqual(try GitHttpHeaders.basic("ada", "secret")("GET", url, [])["authorization"], "Basic YWRhOnNlY3JldA==")
        XCTAssertEqual(try GitHttpHeaders.bearer("tok")("GET", url, [])["authorization"], "Bearer tok")
        XCTAssertEqual(try GitHttpHeaders.static(["x-a": "b"])("POST", url, [1])["x-a"], "b")
        XCTAssertTrue(http.requests.isEmpty, "nothing was sent")

        let closed = GitNetHttpTransport(http: http)
        await closed.close()
        let closedError = await leAssertGitErrorAsync(.networkUnavailable) { _ = try await closed.send(try self.request()) }
        XCTAssertEqual(closedError?.message, "transport is closed")

        leAssertGitError(.protocolUnsupported) { _ = try self.request("http://forkmesh.test/repo.git") }
        leAssertGitError(.protocolUnsupported) { _ = try self.request("https://ada@forkmesh.test/repo.git") }
        XCTAssertThrowsError(try GitHttpRequest(
            method: "PUT", url: URL(string: "https://forkmesh.test/")!, maxResponseBytes: 1,
            expectedContentType: advertisementType
        )) { XCTAssertTrue($0 is GitArgumentError) }
        XCTAssertThrowsError(try GitHttpRequest(
            method: "GET", url: URL(string: "https://forkmesh.test/")!, maxResponseBytes: 0,
            expectedContentType: advertisementType
        )) { XCTAssertTrue($0 is GitArgumentError) }
    }

    func testRequestAndResponseHeadersAreLowerCased() throws {
        let request = try GitHttpRequest(
            method: "POST", url: URL(string: "https://forkmesh.test/")!, headers: ["Content-Type": "a"],
            body: [1, 2], maxResponseBytes: 1, expectedContentType: nil
        )
        XCTAssertEqual(request.headers, ["content-type": "a"])
        let response = GitHttpResponse(statusCode: 200, headers: ["Content-Type": "b"], body: [])
        XCTAssertEqual(response.contentType, "b")
        XCTAssertEqual(classifyHttpFailure(request, statusCode: 403).code, .authenticationFailed)
        XCTAssertEqual(classifyHttpFailure(request, statusCode: 404).message, "remote returned HTTP 404")
    }
}
