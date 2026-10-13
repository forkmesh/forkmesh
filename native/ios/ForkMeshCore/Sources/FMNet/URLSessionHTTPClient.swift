// The production `HTTPClient`, over URLSession (FoundationNetworking on
// Linux). Delegate based rather than `data(for:)` / `bytes(for:)`: Linux has
// no `bytes(for:)`, and the redirect policy and size limit are per request.

import FMBase
import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

public final class URLSessionHTTPClient: HTTPClient, @unchecked Sendable {
    private let router = Router()
    private let session: URLSession

    /// Cookies and the URL cache are off: the app authenticates with headers,
    /// and a cached API answer is a stale one.
    public static var defaultConfiguration: URLSessionConfiguration {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.httpShouldSetCookies = false
        configuration.httpCookieAcceptPolicy = .never
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.urlCache = nil
        return configuration
    }

    public init(configuration: URLSessionConfiguration = URLSessionHTTPClient.defaultConfiguration) {
        session = URLSession(configuration: configuration, delegate: router, delegateQueue: nil)
    }

    deinit { session.invalidateAndCancel() }

    public func send(_ request: HTTPRequest) async throws -> HTTPResponse {
        let streamed = try await open(request, deadlineCoversBody: true)
        var body = Data()
        for try await chunk in streamed.body {
            body.append(chunk)
            // Leaving the loop drops the stream, which cancels the transfer.
            if body.count > request.maxResponseBytes {
                throw HTTPError.responseTooLarge(limit: request.maxResponseBytes)
            }
        }
        return HTTPResponse(status: streamed.status, headers: streamed.headers, body: body)
    }

    /// `timeout` bounds the wait for the status and headers; after that only
    /// URLSession's idle timeout applies, so a long download is not cut off.
    public func stream(_ request: HTTPRequest) async throws -> HTTPStreamedResponse {
        try await open(request, deadlineCoversBody: false)
    }

    private func open(_ request: HTTPRequest, deadlineCoversBody: Bool) async throws -> HTTPStreamedResponse {
        try Task.checkCancellation()
        var urlRequest = URLRequest(url: request.url)
        urlRequest.httpMethod = request.method
        urlRequest.httpBody = request.body
        // URLSession's own timeout is an idle timeout; the deadline below covers the whole exchange.
        urlRequest.timeoutInterval = request.timeout
        for field in request.headers { urlRequest.addValue(field.value, forHTTPHeaderField: field.name) }
        #if canImport(Darwin)
        if HTTP3Hosts.serves(request.url) { urlRequest.assumesHTTP3Capable = true }
        #endif

        let task = session.dataTask(with: urlRequest)
        let exchange = Exchange(followRedirects: request.followRedirects)
        router.register(exchange, for: task)

        let deadline = DispatchWorkItem { [weak task] in
            exchange.markTimedOut()
            task?.cancel()
        }
        DispatchQueue.global().asyncAfter(deadline: .now() + request.timeout, execute: deadline)
        exchange.onFinish = { deadline.cancel() }
        if !deadlineCoversBody { exchange.onHead = { deadline.cancel() } }

        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                exchange.start(head: continuation)
                task.resume()
            }
        } onCancel: {
            task.cancel()
        }
    }
}

public enum HTTP3Hosts {
    public static func serves(_ url: URL) -> Bool {
        guard url.scheme?.lowercased() == "https", let host = url.host?.lowercased() else { return false }
        return host == "forkmesh.com" || host.hasSuffix(".forkmesh.com")
    }
}

/// One request in flight: first the head (status and headers), then the body chunks.
private final class Exchange: @unchecked Sendable {
    let followRedirects: Bool
    var onFinish: (() -> Void)?
    var onHead: (() -> Void)?

    private let lock = NSLock()
    private var head: CheckedContinuation<HTTPStreamedResponse, Error>?
    private var body: AsyncThrowingStream<Data, Error>.Continuation?
    private var timedOut = false

    init(followRedirects: Bool) { self.followRedirects = followRedirects }

    func start(head: CheckedContinuation<HTTPStreamedResponse, Error>) {
        lock.lock()
        self.head = head
        lock.unlock()
    }

    func markTimedOut() {
        lock.lock()
        timedOut = true
        lock.unlock()
    }

    func received(_ response: URLResponse, cancel: @escaping @Sendable () -> Void) {
        lock.lock()
        let head = self.head
        self.head = nil
        lock.unlock()
        guard let head else { return }
        onHead?()
        guard let http = response as? HTTPURLResponse else {
            head.resume(throwing: HTTPError.invalidResponse)
            return
        }
        var headers = HTTPHeaders()
        for (name, value) in http.allHeaderFields { headers.add("\(name)", "\(value)") }
        let stream = AsyncThrowingStream<Data, Error> { continuation in
            lock.lock()
            body = continuation
            lock.unlock()
            continuation.onTermination = { reason in
                if case .cancelled = reason { cancel() }
            }
        }
        head.resume(returning: HTTPStreamedResponse(status: http.statusCode, headers: headers, body: stream))
    }

    func received(_ data: Data) {
        lock.lock()
        let body = self.body
        lock.unlock()
        body?.yield(data)
    }

    func finished(_ error: Error?) {
        lock.lock()
        let head = self.head
        let body = self.body
        let timedOut = self.timedOut
        self.head = nil
        self.body = nil
        lock.unlock()
        onFinish?()
        let mapped = error.map { Exchange.map($0, timedOut: timedOut) }
        if let head {
            head.resume(throwing: mapped ?? HTTPError.invalidResponse)
        } else if let mapped {
            body?.finish(throwing: mapped)
        } else {
            body?.finish()
        }
    }

    static func map(_ error: Error, timedOut: Bool) -> HTTPError {
        if timedOut { return .timedOut }
        let code = (error as? URLError)?.code ?? URLError.Code(rawValue: (error as NSError).code)
        switch code {
        case .timedOut: return .timedOut
        case .cancelled: return .cancelled
        default: return .transport(error.localizedDescription)
        }
    }
}

private final class Router: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var exchanges: [Int: Exchange] = [:]

    func register(_ exchange: Exchange, for task: URLSessionTask) {
        lock.lock()
        exchanges[task.taskIdentifier] = exchange
        lock.unlock()
    }

    private func exchange(for task: URLSessionTask, remove: Bool = false) -> Exchange? {
        lock.lock()
        defer { lock.unlock() }
        return remove ? exchanges.removeValue(forKey: task.taskIdentifier) : exchanges[task.taskIdentifier]
    }

    func urlSession(
        _ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
        completionHandler: @escaping (URLSession.ResponseDisposition) -> Void
    ) {
        exchange(for: dataTask)?.received(response, cancel: { [weak dataTask] in dataTask?.cancel() })
        completionHandler(.allow)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        exchange(for: dataTask)?.received(data)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        exchange(for: task, remove: true)?.finished(error)
    }

    func urlSession(
        _ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
        newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void
    ) {
        // nil hands the 3xx itself back as the response.
        completionHandler(exchange(for: task)?.followRedirects == false ? nil : request)
    }
}
