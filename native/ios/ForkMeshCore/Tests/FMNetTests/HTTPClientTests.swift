// The request/response surface of native/android core/net/HttpClient.kt, the
// fake other modules script, and URLSessionHTTPClient driven through a
// URLProtocol stub (no sockets are opened).

import FMBase
import XCTest
@testable import FMNet
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

final class HTTPClientTests: XCTestCase {
    func testForkMeshHostsAreAskedForHTTP3() {
        XCTAssertTrue(HTTP3Hosts.serves(URL(string: "https://app.forkmesh.com/api/version")!))
        XCTAssertTrue(HTTP3Hosts.serves(URL(string: "https://forkmesh.com/")!))
        XCTAssertFalse(HTTP3Hosts.serves(URL(string: "http://app.forkmesh.com/api/version")!))
        XCTAssertFalse(HTTP3Hosts.serves(URL(string: "https://notforkmesh.com/")!))
        XCTAssertFalse(HTTP3Hosts.serves(URL(string: "https://api.github.com/")!))
    }

    private let url = URL(string: "https://app.forkmesh.com/api/status?x=1")!

    func testHeadersAreCaseInsensitive() {
        var headers: HTTPHeaders = ["Content-Type": "application/json", "X-Trace": "a"]
        XCTAssertEqual(headers["content-type"], "application/json")
        XCTAssertEqual(headers["CONTENT-TYPE"], "application/json")
        XCTAssertNil(headers["accept"])
        headers["content-type"] = "text/plain"
        XCTAssertEqual(headers.fields.count, 2)
        XCTAssertEqual(headers.fields[0].value, "text/plain", "replaced in place")
        headers.add("Set-Cookie", "a=1")
        headers.add("set-cookie", "b=2")
        XCTAssertEqual(headers["Set-Cookie"], "a=1, b=2")
        XCTAssertEqual(headers.values("SET-COOKIE"), ["a=1", "b=2"])
        XCTAssertEqual(headers.dictionary["set-cookie"], "a=1, b=2")
        headers["x-trace"] = nil
        XCTAssertNil(headers["X-Trace"])
        XCTAssertEqual(HTTPHeaders(["B": "2", "a": "1"]), ["A": "1", "b": "2"])
        XCTAssertTrue(HTTPHeaders().isEmpty)
    }

    func testRequestBuilders() {
        let request = HTTPRequest(method: "post", url: url).bearer("tok").json(["b": 1, "a": "é"])
        XCTAssertEqual(request.method, "POST")
        XCTAssertEqual(request.headers["authorization"], "Bearer tok")
        XCTAssertEqual(request.headers["content-type"], "application/json")
        XCTAssertEqual(request.bodyText, #"{"b":1,"a":"é"}"#)
        XCTAssertNil(HTTPRequest(url: url).bearer("").headers["Authorization"])
        XCTAssertNil(HTTPRequest(url: url).bearer(nil).headers["Authorization"])
        XCTAssertEqual(HTTPRequest(url: url).timeout, 30)
        XCTAssertTrue(HTTPRequest(url: url).followRedirects)
    }

    func testResponseAccessors() throws {
        let response = HTTPResponse(status: 403, headers: ["Content-Type": "application/json"], text: #"{"error":"forbidden","n":1}"#)
        XCTAssertFalse(response.ok)
        XCTAssertEqual(response.errorCode, "forbidden")
        XCTAssertEqual(try response.json()["n"], 1)
        XCTAssertEqual(response.header("content-type"), "application/json")
        let plain = HTTPResponse(status: 204)
        XCTAssertTrue(plain.ok)
        XCTAssertEqual(plain.errorCode, "")
        XCTAssertEqual(plain.jsonObject.count, 0)
        XCTAssertThrowsError(try plain.json())
        XCTAssertEqual(HTTPResponse(status: 200, body: Data([0x61, 0xFF])).text, "a\u{FFFD}")
        XCTAssertEqual(HTTPResponse(json: ["ok": true]).text, #"{"ok":true}"#)
    }

    func testFakeQueueRoutesAndFallback() async throws {
        let client = FakeHTTPClient { request in HTTPResponse(status: 404, text: request.url.path) }
        client.on("GET", path: "/api/status", json: ["ok": true])
        client.on(path: "/api/echo") { request in HTTPResponse(status: 201, text: request.bodyText) }
        client.enqueue(status: 503, text: "first")

        let queued = try await client.get(url)
        XCTAssertEqual(queued.status, 503, "queued answers come first")
        let routed = try await client.get(url)
        XCTAssertEqual(routed.jsonObject.bool("ok"), true)
        let wrongMethod = try await client.post(url)
        XCTAssertEqual(wrongMethod.status, 404)
        let echo = try await client.postJSON(URL(string: "https://h/api/echo")!, ["a": 1], headers: ["X-A": "b"])
        XCTAssertEqual(echo.status, 201)
        XCTAssertEqual(echo.text, #"{"a":1}"#)
        let deleted = try await client.delete(URL(string: "https://h/other")!)
        XCTAssertEqual(deleted.text, "/other")

        XCTAssertEqual(client.requests.map(\.method), ["GET", "GET", "POST", "POST", "DELETE"])
        XCTAssertEqual(client.lastRequest?.url.path, "/other")
        XCTAssertEqual(client.requests[3].headers["x-a"], "b")
        XCTAssertEqual(client.requests[3].headers["Content-Type"], "application/json")
    }

    func testFakeErrorsAndLimits() async throws {
        let client = FakeHTTPClient()
        client.enqueue(error: HTTPError.timedOut)
        do {
            _ = try await client.get(url)
            XCTFail("expected a timeout")
        } catch {
            XCTAssertEqual(error as? HTTPError, .timedOut)
        }
        do {
            _ = try await client.get(url)
            XCTFail("expected no scripted response")
        } catch HTTPError.transport(let message) {
            XCTAssertTrue(message.contains("GET https://app.forkmesh.com/api/status?x=1"))
        }
        client.enqueue(status: 200, text: "0123456789")
        do {
            _ = try await client.send(HTTPRequest(url: url, maxResponseBytes: 4))
            XCTFail("expected the size limit")
        } catch {
            XCTAssertEqual(error as? HTTPError, .responseTooLarge(limit: 4))
        }
        XCTAssertEqual(client.requests.count, 3)
        client.reset()
        XCTAssertTrue(client.requests.isEmpty)
    }

    func testDefaultStreamYieldsTheBody() async throws {
        let client = FakeHTTPClient()
        client.enqueue(status: 200, text: "pack")
        let streamed = try await client.stream(HTTPRequest(url: url))
        var body = Data()
        for try await chunk in streamed.body { body.append(chunk) }
        XCTAssertEqual(streamed.status, 200)
        XCTAssertEqual(body, Data("pack".utf8))
    }

    // MARK: URLSessionHTTPClient

    private func stubbedClient() -> URLSessionHTTPClient {
        let configuration = URLSessionHTTPClient.defaultConfiguration
        configuration.protocolClasses = [StubProtocol.self]
        return URLSessionHTTPClient(configuration: configuration)
    }

    func testURLSessionClientMapsRequestAndResponse() async throws {
        StubProtocol.handler = { request in
            let seen: JSONValue = [
                "method": JSONValue(request.httpMethod ?? ""),
                "auth": JSONValue(request.value(forHTTPHeaderField: "Authorization")),
                "type": JSONValue(request.value(forHTTPHeaderField: "Content-Type")),
                "url": JSONValue(request.url?.absoluteString),
            ]
            return .success((202, ["X-Relay": "one", "Content-Type": "application/json"], [JSON.encodeData(seen)]))
        }
        let client = stubbedClient()
        let response = try await client.send(HTTPRequest(method: "PATCH", url: url).bearer("tok").json(["a": 1]))
        XCTAssertEqual(response.status, 202)
        XCTAssertEqual(response.header("x-relay"), "one")
        let seen = response.jsonObject
        XCTAssertEqual(seen.str("method"), "PATCH")
        XCTAssertEqual(seen.str("auth"), "Bearer tok")
        XCTAssertEqual(seen.str("type"), "application/json")
        XCTAssertEqual(seen.str("url"), url.absoluteString)
    }

    func testURLSessionClientReturnsErrorStatusesAsResponses() async throws {
        StubProtocol.handler = { _ in .success((500, [:], [Data(#"{"error":"boom"}"#.utf8)])) }
        let response = try await stubbedClient().get(url)
        XCTAssertEqual(response.status, 500)
        XCTAssertEqual(response.errorCode, "boom")
    }

    func testURLSessionClientStreamsChunksAndEnforcesTheLimit() async throws {
        StubProtocol.handler = { _ in .success((200, [:], [Data("ab".utf8), Data("cd".utf8), Data("ef".utf8)])) }
        let client = stubbedClient()
        let streamed = try await client.stream(HTTPRequest(url: url))
        var body = Data()
        for try await chunk in streamed.body { body.append(chunk) }
        XCTAssertEqual(String(decoding: body, as: UTF8.self), "abcdef")

        do {
            _ = try await client.send(HTTPRequest(url: url, maxResponseBytes: 3))
            XCTFail("expected the size limit")
        } catch {
            XCTAssertEqual(error as? HTTPError, .responseTooLarge(limit: 3))
        }
    }

    func testURLSessionClientMapsFailures() async throws {
        let client = stubbedClient()
        StubProtocol.handler = { _ in .failure(URLError(.timedOut)) }
        do {
            _ = try await client.get(url)
            XCTFail("expected a timeout")
        } catch {
            XCTAssertEqual(error as? HTTPError, .timedOut)
        }
        StubProtocol.handler = { _ in .failure(URLError(.cannotConnectToHost)) }
        do {
            _ = try await client.get(url)
            XCTFail("expected a transport error")
        } catch HTTPError.transport {
        }
    }

    func testURLSessionClientDeadlineCoversTheWholeExchange() async throws {
        StubProtocol.handler = { _ in nil } // never answers
        let started = Date()
        do {
            _ = try await stubbedClient().send(HTTPRequest(url: url, timeout: 0.2))
            XCTFail("expected a timeout")
        } catch {
            XCTAssertEqual(error as? HTTPError, .timedOut)
        }
        XCTAssertLessThan(Date().timeIntervalSince(started), 5)
    }

    func testURLSessionClientCancellation() async throws {
        StubProtocol.handler = { _ in nil }
        let client = stubbedClient()
        let target = url
        let work = Task { try await client.send(HTTPRequest(url: target, timeout: 30)) }
        try await Task.sleep(nanoseconds: 100_000_000)
        work.cancel()
        do {
            _ = try await work.value
            XCTFail("expected cancellation")
        } catch {
            XCTAssertTrue(error is CancellationError || (error as? HTTPError) == .cancelled, "\(error)")
        }
    }
}

/// Answers every request of a stubbed session from `handler`; nil leaves the request hanging.
final class StubProtocol: URLProtocol {
    typealias Answer = Result<(Int, [String: String], [Data]), Error>
    private static let lock = NSLock()
    private static var current: (@Sendable (URLRequest) -> Answer?)?

    static var handler: (@Sendable (URLRequest) -> Answer?)? {
        get { lock.lock(); defer { lock.unlock() }; return current }
        set { lock.lock(); current = newValue; lock.unlock() }
    }

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        guard let answer = StubProtocol.handler?(request) else { return }
        switch answer {
        case .failure(let error):
            client?.urlProtocol(self, didFailWithError: error)
        case .success(let (status, headers, chunks)):
            let response = HTTPURLResponse(url: request.url!, statusCode: status, httpVersion: "HTTP/1.1", headerFields: headers)!
            client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            for chunk in chunks { client?.urlProtocol(self, didLoad: chunk) }
            client?.urlProtocolDidFinishLoading(self)
        }
    }

    override func stopLoading() {}
}
