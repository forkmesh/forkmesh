// The git HTTP transport over FMNet's HTTPClient.
//
// * Only `https://` URLs without user-info are accepted (GitHttpRequest).
// * Redirects are never followed (`followRedirects: false`); a 3xx becomes
//   `remoteRejected`.
// * No cookies and no URL cache (URLSessionHTTPClient's configuration), no
//   compression (`Accept-Encoding: identity`), the Cookie header removed.
// * Response headers and bodies are bounded by GitResourceLimits; the
//   cancellation token and the overall deadline are checked after every body
//   chunk, and a watchdog enforces the idle-read timeout between chunks.
//
// Left out, because HTTPClient cannot express them: the TLS certificate
// challenge (a failing chain is simply rejected by the platform) and
// `trustedRootCertificatesPem`. URLSession reports every transport failure
// as `HTTPError.transport(text)`, so a TLS failure is recognised by its text.

import FMGit
import FMNet
import Foundation

final class GitNetHttpTransport: GitHttpTransport, @unchecked Sendable {
    /// Makes this the engine's transport for a `GitRemoteAccess` that names
    /// none (FMGit has no HTTP client). Evaluated once, by `GitWorkspace.init`.
    static let registeredAsDefault: Void = {
        GitDefaultHttpTransport.register { GitNetHttpTransport(headerProvider: $0) }
    }()

    let headerProvider: GitHttpHeaderProvider?
    let surfaceRedirects: Bool
    private let http: HTTPClient
    private let userAgent: String
    private let lock = NSLock()
    private var closed = false

    init(
        http: HTTPClient = URLSessionHTTPClient(), headerProvider: GitHttpHeaderProvider? = nil, userAgent: String? = nil,
        surfaceRedirects: Bool = false
    ) {
        self.http = http
        self.headerProvider = headerProvider
        self.surfaceRedirects = surfaceRedirects
        self.userAgent = userAgent ?? "forkmesh-git/0.0.0"
    }

    private var isClosed: Bool {
        lock.lock()
        defer { lock.unlock() }
        return closed
    }

    func close() async { markClosed() }

    private func markClosed() {
        lock.lock()
        closed = true
        lock.unlock()
    }

    func send(
        _ request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?
    ) async throws -> GitHttpResponse {
        if isClosed {
            throw GitException(.networkUnavailable, "transport is closed")
        }
        let effectiveLimits = limits ?? GitResourceLimits.standard
        try cancellation?.throwIfCancelled()
        try Self.checkDeadline(request.deadline)
        let extraHeaders = try providedHeaders(request)
        var headers = HTTPHeaders()
        for name in extraHeaders.keys.sorted() { headers[name] = extraHeaders[name] }
        for name in request.headers.keys.sorted() { headers[name] = request.headers[name] }
        headers["accept-encoding"] = "identity"
        headers["user-agent"] = userAgent
        headers["cookie"] = nil
        let timeout = try Self.remaining(request.deadline, effectiveLimits.connectTimeout)
        let httpRequest = HTTPRequest(
            method: request.method,
            url: request.url,
            headers: headers,
            body: request.method == "POST" ? Data(request.body) : nil,
            timeout: timeout,
            followRedirects: false,
            maxResponseBytes: request.maxResponseBytes
        )
        if request.method == "POST" { try cancellation?.throwIfCancelled() }
        let client = http
        let opener = Task { try await client.stream(httpRequest) }
        let streamed: HTTPStreamedResponse
        do {
            streamed = try await Self.poll(opener, tick: 0.05) {
                if Task.isCancelled || (cancellation?.isCancelled ?? false) {
                    return GitException(.cancelled, "operation was cancelled")
                }
                return nil
            }
        } catch {
            throw Self.mapSendFailure(error, cancellation)
        }
        let responseHeaders = try Self.boundedHeaders(streamed.headers, effectiveLimits)
        let status = streamed.status
        if status >= 300 && status < 400 {
            if surfaceRedirects { return GitHttpResponse(statusCode: status, headers: responseHeaders, body: []) }
            // Never follow; drain nothing, surface a redacted comparison.
            throw classifyHttpFailure(request, statusCode: status, location: responseHeaders["location"])
        }
        if status != 200 {
            throw classifyHttpFailure(request, statusCode: status)
        }
        if let declared = responseHeaders["content-length"].flatMap({ Int($0.engTrim()) }),
           declared > request.maxResponseBytes
        {
            throw GitException(.resourceLimitExceeded, "response body exceeds the configured limit")
        }
        if let expected = request.expectedContentType,
           Self.mediaType(responseHeaders["content-type"]) != expected.lowercased()
        {
            throw GitException(.protocolUnsupported, "response content type is not the expected Git service type")
        }
        let body = try await Self.readBody(streamed.body, request, effectiveLimits, cancellation)
        return GitHttpResponse(statusCode: status, headers: responseHeaders, body: body)
    }

    // MARK: - Body

    private final class Outcome<T>: @unchecked Sendable {
        private let lock = NSLock()
        private var stored: Result<T, Error>?

        func set(_ result: Result<T, Error>) {
            lock.lock()
            stored = result
            lock.unlock()
        }

        var result: Result<T, Error>? {
            lock.lock()
            defer { lock.unlock() }
            return stored
        }
    }

    private final class ReadState: @unchecked Sendable {
        private let lock = NSLock()
        private var lastProgress = Date()
        private var stopped = false

        func touch() {
            lock.lock()
            lastProgress = Date()
            lock.unlock()
        }

        func idleFor() -> TimeInterval {
            lock.lock()
            defer { lock.unlock() }
            return Date().timeIntervalSince(lastProgress)
        }

        func stop() {
            lock.lock()
            stopped = true
            lock.unlock()
        }

        var isStopped: Bool {
            lock.lock()
            defer { lock.unlock() }
            return stopped
        }
    }

    static func poll<T>(
        _ task: Task<T, Error>,
        tick: TimeInterval,
        _ check: () -> GitException?
    ) async throws -> T {
        let outcome = Outcome<T>()
        let waiter = Task {
            do { outcome.set(.success(try await task.value)) } catch { outcome.set(.failure(error)) }
        }
        let nanoseconds = UInt64(max(tick, 0.001) * 1_000_000_000)
        while true {
            if let result = outcome.result { return try result.get() }
            if let failure = check() {
                task.cancel()
                waiter.cancel()
                throw failure
            }
            try? await Task.sleep(nanoseconds: nanoseconds)
        }
    }

    private static func readBody(
        _ stream: AsyncThrowingStream<Data, Error>,
        _ request: GitHttpRequest,
        _ limits: GitResourceLimits,
        _ cancellation: GitCancellationToken?
    ) async throws -> [UInt8] {
        let state = ReadState()
        let maxBytes = request.maxResponseBytes
        let deadline = request.deadline
        let reader = Task { () throws -> [UInt8] in
            var output = [UInt8]()
            for try await chunk in stream {
                if state.isStopped { break }
                if output.count + chunk.count > maxBytes {
                    throw GitException(.resourceLimitExceeded, "response body exceeds the configured limit")
                }
                output.append(contentsOf: chunk)
                request.onBodyBytes?(output.count)
                if cancellation?.isCancelled ?? false {
                    throw GitException(.cancelled, "operation was cancelled")
                }
                if let deadline, Date() > deadline {
                    throw GitException(.networkUnavailable, "operation deadline exceeded while streaming")
                }
                state.touch()
            }
            return output
        }
        let idleTimeout = limits.idleReadTimeout
        do {
            return try await poll(reader, tick: min(0.05, idleTimeout / 4)) {
                if Task.isCancelled || (cancellation?.isCancelled ?? false) {
                    state.stop()
                    return GitException(.cancelled, "operation was cancelled")
                }
                if let deadline, Date() > deadline {
                    state.stop()
                    return GitException(.networkUnavailable, "operation deadline exceeded while streaming")
                }
                if state.idleFor() >= idleTimeout {
                    state.stop()
                    return GitException(.networkUnavailable, "remote stalled beyond the idle read timeout")
                }
                return nil
            }
        } catch let error as GitException {
            throw error
        } catch {
            if error is CancellationError || (error as? HTTPError) == .cancelled {
                throw GitException(.cancelled, "operation was cancelled")
            }
            if case HTTPError.responseTooLarge = error {
                throw GitException(.resourceLimitExceeded, "response body exceeds the configured limit")
            }
            throw GitException(
                .networkUnavailable,
                "response stream failed: \(redactOsError(String(describing: error)))"
            )
        }
    }

    // MARK: - Headers

    private func providedHeaders(_ request: GitHttpRequest) throws -> [String: String] {
        guard let provider = headerProvider else { return [:] }
        let provided: [String: String]
        do {
            provided = try provider(request.method, request.url, request.body)
        } catch let error as GitException {
            throw error
        } catch {
            throw GitException(.authenticationFailed, "header provider failed")
        }
        var result = [String: String]()
        for (key, value) in provided {
            let name = key.lowercased()
            if !Self.isHeaderName(name) || !Self.isHeaderValue(value) || Self.reservedHeaders.contains(name) {
                throw GitException(.authenticationFailed, "header provider returned an invalid or reserved header")
            }
            result[name] = value
        }
        return result
    }

    private static func boundedHeaders(_ headers: HTTPHeaders, _ limits: GitResourceLimits) throws -> [String: String] {
        var result = [String: String]()
        var total = 0
        for field in headers {
            total += field.name.utf8.count + field.value.utf8.count + 4
        }
        for field in headers { result[field.name.lowercased()] = headers.values(field.name).joined(separator: ", ") }
        if total > limits.maxResponseHeaderBytes {
            throw GitException(.resourceLimitExceeded, "response headers exceed the configured limit")
        }
        return result
    }

    static func mediaType(_ contentType: String?) -> String {
        guard let contentType else { return "" }
        let separator = contentType.engIndexOf(";")
        let type = separator == -1 ? contentType : contentType.engSubstring(0, separator)
        return type.engTrim().lowercased()
    }

    /// `^[a-z0-9!#$%&*+.^_`|~-]+$`
    private static func isHeaderName(_ name: String) -> Bool {
        let bytes = Array(name.utf8)
        if bytes.isEmpty { return false }
        let extra = Array("!#$%&*+.^_`|~-".utf8)
        return bytes.allSatisfy { ($0 >= 0x61 && $0 <= 0x7a) || ($0 >= 0x30 && $0 <= 0x39) || extra.contains($0) }
    }

    /// `^[\x20-\x7e]*$`
    private static func isHeaderValue(_ value: String) -> Bool {
        value.utf8.allSatisfy { $0 >= 0x20 && $0 <= 0x7e }
    }

    static let reservedHeaders: Set<String> = [
        "content-type", "accept", "git-protocol", "content-length", "transfer-encoding",
        "host", "cookie", "accept-encoding", "connection", "upgrade",
    ]

    // MARK: - Failures and deadlines

    private static func mapSendFailure(_ error: Error, _ cancellation: GitCancellationToken?) -> GitException {
        if let error = error as? GitException { return error }
        if error is CancellationError {
            return GitException(.cancelled, "operation was cancelled")
        }
        guard let http = error as? HTTPError else {
            return GitException(.networkUnavailable, "I/O failure while sending the request")
        }
        switch http {
        case .timedOut:
            return GitException(.networkUnavailable, "remote did not respond within the connect timeout")
        case .cancelled:
            return GitException(.cancelled, "operation was cancelled")
        case .responseTooLarge:
            return GitException(.resourceLimitExceeded, "response body exceeds the configured limit")
        case .invalidResponse:
            return GitException(.networkUnavailable, "HTTP failure: invalid response")
        case .transport(let message):
            let lower = message.lowercased()
            if lower.contains("certificate") || lower.contains("ssl") || lower.contains("tls")
                || lower.contains("secure connection")
            {
                return GitException(.tlsRejected, "TLS failure: \(redactOsError(message))")
            }
            return GitException(.networkUnavailable, "connection failed: \(redactOsError(message))")
        }
    }

    private static func checkDeadline(_ deadline: Date?) throws {
        if let deadline, Date() > deadline {
            throw GitException(.networkUnavailable, "operation deadline exceeded")
        }
    }

    private static func remaining(_ deadline: Date?, _ fallback: TimeInterval) throws -> TimeInterval {
        guard let deadline else { return fallback }
        let remaining = deadline.timeIntervalSinceNow
        if remaining <= 0 {
            throw GitException(.networkUnavailable, "operation deadline exceeded")
        }
        return remaining < fallback ? remaining : fallback
    }
}

/// Printable ASCII only, at most 160 characters.
func redactOsError(_ message: String) -> String {
    var output = ""
    for unit in message.utf16 {
        if output.utf16.count >= 160 { break }
        if unit >= 0x20 && unit < 0x7f { output.unicodeScalars.append(Unicode.Scalar(UInt8(unit))) }
    }
    return output
}
