// A fake `HTTPClient` for tests: scripted responses and a record of what was
// asked. Public and in the module itself, so every other module's tests can
// use it.

import FMBase
import Foundation

public final class FakeHTTPClient: HTTPClient, @unchecked Sendable {
    public typealias Handler = @Sendable (HTTPRequest) async throws -> HTTPResponse

    private let lock = NSLock()
    private var recorded: [HTTPRequest] = []
    private var queue: [Result<HTTPResponse, Error>] = []
    private var routes: [(method: String?, match: @Sendable (HTTPRequest) -> Bool, handler: Handler)] = []
    private let fallback: Handler?

    /// `handler` answers whatever no queued response or route does; without
    /// one such a request throws `HTTPError.transport`.
    public init(_ handler: Handler? = nil) { fallback = handler }

    /// Every request sent, oldest first.
    public var requests: [HTTPRequest] {
        lock.lock()
        defer { lock.unlock() }
        return recorded
    }

    public var lastRequest: HTTPRequest? { requests.last }

    public func reset() {
        lock.lock()
        recorded.removeAll()
        queue.removeAll()
        routes.removeAll()
        lock.unlock()
    }

    /// Answers the next unanswered request, in order, before any route is tried.
    public func enqueue(_ response: HTTPResponse) {
        lock.lock()
        queue.append(.success(response))
        lock.unlock()
    }

    public func enqueue(status: Int = 200, json: JSONValue) { enqueue(HTTPResponse(status: status, json: json)) }

    public func enqueue(status: Int, text: String = "") { enqueue(HTTPResponse(status: status, text: text)) }

    public func enqueue(error: Error) {
        lock.lock()
        queue.append(.failure(error))
        lock.unlock()
    }

    /// Answers requests whose URL path equals `path` (any method when `method` is nil). Later routes win.
    public func on(_ method: String? = nil, path: String, _ handler: @escaping Handler) {
        on(method, where: { $0.url.path == path }, handler)
    }

    public func on(_ method: String? = nil, path: String, status: Int = 200, json: JSONValue) {
        on(method, path: path) { _ in HTTPResponse(status: status, json: json) }
    }

    public func on(
        _ method: String? = nil, where match: @escaping @Sendable (HTTPRequest) -> Bool, _ handler: @escaping Handler
    ) {
        lock.lock()
        routes.append((method?.uppercased(), match, handler))
        lock.unlock()
    }

    private func record(_ request: HTTPRequest) -> (Result<HTTPResponse, Error>?, Handler?) {
        lock.lock()
        defer { lock.unlock() }
        recorded.append(request)
        let queued = queue.isEmpty ? nil : queue.removeFirst()
        let route = routes.last { ($0.method == nil || $0.method == request.method) && $0.match(request) }
        return (queued, route?.handler)
    }

    public func send(_ request: HTTPRequest) async throws -> HTTPResponse {
        try Task.checkCancellation()
        let (queued, route) = record(request)
        let response: HTTPResponse
        if let queued {
            response = try queued.get()
        } else if let route {
            response = try await route(request)
        } else if let fallback {
            response = try await fallback(request)
        } else {
            throw HTTPError.transport("FakeHTTPClient: no response for \(request.method) \(request.url.absoluteString)")
        }
        if response.body.count > request.maxResponseBytes {
            throw HTTPError.responseTooLarge(limit: request.maxResponseBytes)
        }
        return response
    }
}
