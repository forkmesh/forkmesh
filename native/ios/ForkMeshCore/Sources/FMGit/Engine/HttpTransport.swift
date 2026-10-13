// Left out: GitCertificateChallenge, GitCertificateDecision and the
// challenge handler. The production transport is built on FMNet's
// HTTPClient, which exposes no TLS challenge hook (URLSession validates the
// chain with the platform trust store and fails closed), so a caller can
// neither see nor accept a failing certificate.

import Foundation

/// Supplies extra request headers for one HTTP request.
///
/// The engine calls the provider once per request with the final method,
/// URL, and body bytes, so a caller can sign the exact request (for example
/// a ForkMesh device signature over the body digest) or attach a bearer
/// token. Returned header names are case-insensitive; protocol headers set
/// by the engine (`Content-Type`, `Accept`, `Git-Protocol`) cannot be
/// overridden. The engine never logs, journals, or persists these values.
public typealias GitHttpHeaderProvider = (_ method: String, _ url: URL, _ body: [UInt8]) throws -> [String: String]

/// Ready-made header providers for common authentication schemes.
///
/// For an invalid token or credential the returned provider throws a
/// GitException on its first use, so no request is ever sent with it.
public enum GitHttpHeaders {
    /// Returns a provider that sends the same headers with every request.
    public static func `static`(_ headers: [String: String]) -> GitHttpHeaderProvider {
        let copy = headers
        return { _, _, _ in copy }
    }

    /// Returns a provider that sends `Authorization: Bearer <token>`.
    public static func bearer(_ token: String) -> GitHttpHeaderProvider {
        if let failure = bearerFailure(token) { return { _, _, _ in throw failure } }
        return `static`(["authorization": "Bearer \(token)"])
    }

    /// Returns a provider that sends HTTP basic authentication.
    public static func basic(_ user: String, _ password: String) -> GitHttpHeaderProvider {
        if let failure = basicFailure(user, password) { return { _, _, _ in throw failure } }
        let encoded = Data("\(user):\(password)".utf8).base64EncodedString()
        return `static`(["authorization": "Basic \(encoded)"])
    }

    /// The check behind `bearer`: nil when the token is usable.
    public static func bearerFailure(_ token: String) -> GitException? {
        let bytes = Array(token.utf8)
        // ^[\x20-\x7e]+$
        if bytes.isEmpty || bytes.contains(where: { $0 < 0x20 || $0 > 0x7e }) {
            return GitException(.authenticationFailed, "bearer token is not a valid header value")
        }
        return nil
    }

    /// The check behind `basic`: nil when the pair is usable.
    public static func basicFailure(_ user: String, _ password: String) -> GitException? {
        if user.engContains(":") || user.engContains("\n") || password.engContains("\n") {
            return GitException(.authenticationFailed, "basic credentials contain invalid characters")
        }
        return nil
    }
}

public struct GitHttpRequest: Sendable {
    public let method: String
    public let url: URL
    /// Lower-case names.
    public let headers: [String: String]
    public let body: [UInt8]

    /// Maximum accepted response body bytes; exceeding it fails the request
    /// with `resourceLimitExceeded`.
    public let maxResponseBytes: Int

    /// Exact `Content-Type` the response must carry. Every request the engine
    /// builds names one; nil (only a hand-built request) skips the check.
    public let expectedContentType: String?

    /// Absolute end-to-end deadline for the operation, if any.
    public let deadline: Date?

    public let onBodyBytes: (@Sendable (Int) -> Void)?

    public init(
        method: String,
        url: URL,
        headers: [String: String] = [:],
        body: [UInt8] = [],
        maxResponseBytes: Int,
        expectedContentType: String?,
        deadline: Date? = nil,
        onBodyBytes: (@Sendable (Int) -> Void)? = nil
    ) throws {
        if method != "GET" && method != "POST" {
            throw GitArgumentError("method")
        }
        let hasUserInfo = (url.user.map { !$0.isEmpty } ?? false) || (url.password.map { !$0.isEmpty } ?? false)
        if url.scheme?.lowercased() != "https" || hasUserInfo {
            throw GitException(.protocolUnsupported, "transport accepts only credential-free HTTPS URLs")
        }
        if maxResponseBytes <= 0 {
            throw GitArgumentError("maxResponseBytes")
        }
        self.method = method
        self.url = url
        var lowered = [String: String]()
        for (name, value) in headers { lowered[name.lowercased()] = value }
        self.headers = lowered
        self.body = body
        self.maxResponseBytes = maxResponseBytes
        self.expectedContentType = expectedContentType
        self.deadline = deadline
        self.onBodyBytes = onBodyBytes
    }
}

public struct GitHttpResponse: Sendable {
    public let statusCode: Int
    /// Lower-case names.
    public let headers: [String: String]
    public let body: [UInt8]

    public init(statusCode: Int, headers: [String: String], body: [UInt8]) {
        self.statusCode = statusCode
        var lowered = [String: String]()
        for (name, value) in headers { lowered[name.lowercased()] = value }
        self.headers = lowered
        self.body = body
    }

    public var contentType: String? { headers["content-type"] }
}

/// Minimal HTTPS request primitive used by the Git orchestration layer.
///
/// Implementations must be fail-closed: strict TLS validation, zero
/// automatic redirects, no cookies or ambient credential cache, bounded
/// headers and bodies, and cancellation checks between body chunks.
public protocol GitHttpTransport: AnyObject, Sendable {
    func send(
        _ request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?
    ) async throws -> GitHttpResponse

    func close() async
}

extension GitHttpTransport {
    public func send(_ request: GitHttpRequest) async throws -> GitHttpResponse {
        try await send(request, cancellation: nil, limits: nil)
    }
}

/// The transport a remote operation uses when its `GitRemoteAccess` names
/// none. FMGit carries no HTTP client of its own (it depends on zlib only, so
/// it builds everywhere FMBase does); FMCode registers `GitNetHttpTransport`
/// here. With nothing registered, every request fails as
/// `networkUnavailable` rather than reaching the network some other way.
public enum GitDefaultHttpTransport {
    public typealias Factory = @Sendable (_ headerProvider: GitHttpHeaderProvider?) -> GitHttpTransport

    private static let lock = NSLock()
    nonisolated(unsafe) private static var factory: Factory?

    /// Installs the default factory; the last registration wins.
    public static func register(_ make: @escaping Factory) {
        lock.lock()
        defer { lock.unlock() }
        factory = make
    }

    public static func make(headerProvider: GitHttpHeaderProvider?) -> GitHttpTransport {
        lock.lock()
        let make = factory
        lock.unlock()
        return make?(headerProvider) ?? UnavailableGitHttpTransport()
    }
}

/// Fails every request: no transport was supplied or registered.
private final class UnavailableGitHttpTransport: GitHttpTransport {
    public func send(
        _ request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?
    ) async throws -> GitHttpResponse {
        throw GitException(.networkUnavailable, "no HTTP transport is available for Git")
    }

    public func close() async {}
}

/// Maps an HTTP status and response into the typed error taxonomy.
///
/// A redirect is never followed; it becomes `remoteRejected` with a
/// redacted origin and destination host comparison.
public func classifyHttpFailure(_ request: GitHttpRequest, statusCode: Int, location: String? = nil) -> GitException {
    if statusCode >= 300 && statusCode < 400 {
        let destinationHost: String
        if let location, let destination = URLComponents(string: location),
           let host = destination.host, !host.isEmpty
        {
            destinationHost = host.lowercased() == (request.url.host ?? "").lowercased()
                ? "same host" : "different host"
        } else {
            destinationHost = "relative or unknown host"
        }
        return GitException(
            .remoteRejected,
            "remote redirected (\(statusCode)) to \(destinationHost); redirects are never followed"
        )
    }
    if statusCode == 401 || statusCode == 403 || statusCode == 407 {
        return GitException(.authenticationFailed, "remote refused credentials (\(statusCode))")
    }
    return GitException(.remoteRejected, "remote returned HTTP \(statusCode)")
}
