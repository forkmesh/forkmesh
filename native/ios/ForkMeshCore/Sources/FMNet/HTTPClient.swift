// The HTTP client services take as a constructor argument, after
// native/android core/net/HttpClient.kt: one protocol, a URLSession
// implementation and a fake for tests.

import FMBase
import Foundation

/// Header fields with case-insensitive names, in the order they were added.
public struct HTTPHeaders: Equatable, Sendable, ExpressibleByDictionaryLiteral, Sequence {
    public private(set) var fields: [(name: String, value: String)] = []

    public init() {}

    public init(_ dictionary: [String: String]) {
        // Sorted, so a dictionary always yields the same request.
        for name in dictionary.keys.sorted() { fields.append((name, dictionary[name] ?? "")) }
    }

    public init(dictionaryLiteral elements: (String, String)...) {
        for (name, value) in elements { self[name] = value }
    }

    /// The value for `name`, several fields joined with ", " as the `http`
    /// package does. Setting replaces every field of that name; nil removes it.
    public subscript(name: String) -> String? {
        get {
            let matches = values(name)
            return matches.isEmpty ? nil : matches.joined(separator: ", ")
        }
        set {
            let index = fields.firstIndex { $0.name.caseInsensitiveCompare(name) == .orderedSame }
            fields.removeAll { $0.name.caseInsensitiveCompare(name) == .orderedSame }
            guard let newValue else { return }
            fields.insert((name, newValue), at: Swift.min(index ?? fields.count, fields.count))
        }
    }

    public func values(_ name: String) -> [String] {
        fields.filter { $0.name.caseInsensitiveCompare(name) == .orderedSame }.map(\.value)
    }

    public mutating func add(_ name: String, _ value: String) { fields.append((name, value)) }

    public var isEmpty: Bool { fields.isEmpty }

    /// Lower-case names to joined values, the shape of `http.Response.headers`.
    public var dictionary: [String: String] {
        var out = [String: String]()
        for field in fields { out[field.name.lowercased()] = self[field.name] }
        return out
    }

    public func makeIterator() -> IndexingIterator<[(name: String, value: String)]> { fields.makeIterator() }

    public static func == (lhs: HTTPHeaders, rhs: HTTPHeaders) -> Bool { lhs.dictionary == rhs.dictionary }
}

public struct HTTPRequest: Sendable {
    /// The largest body read into memory by default; bigger responses fail instead of exhausting the heap.
    public static let defaultMaxResponseBytes = 64 * 1024 * 1024

    public var method: String
    public var url: URL
    public var headers: HTTPHeaders
    public var body: Data?
    /// For the whole exchange.
    public var timeout: TimeInterval
    /// The Git transport refuses redirects; everything else follows them.
    public var followRedirects: Bool
    public var maxResponseBytes: Int

    public init(
        method: String = "GET",
        url: URL,
        headers: HTTPHeaders = [:],
        body: Data? = nil,
        timeout: TimeInterval = 30,
        followRedirects: Bool = true,
        maxResponseBytes: Int = HTTPRequest.defaultMaxResponseBytes
    ) {
        self.method = method.uppercased()
        self.url = url
        self.headers = headers
        self.body = body
        self.timeout = timeout
        self.followRedirects = followRedirects
        self.maxResponseBytes = maxResponseBytes
    }

    /// `Authorization: Bearer <token>`; nothing for an empty token.
    public func bearer(_ token: String?) -> HTTPRequest {
        guard let token, !token.isEmpty else { return self }
        var copy = self
        copy.headers["Authorization"] = "Bearer \(token)"
        return copy
    }

    /// A JSON body; sets the content type.
    public func json(_ payload: JSONValue) -> HTTPRequest {
        var copy = self
        copy.body = JSON.encodeData(payload)
        copy.headers["Content-Type"] = "application/json"
        return copy
    }

    /// The body as text, for assertions in tests.
    public var bodyText: String { body.map { String(decoding: $0, as: UTF8.self) } ?? "" }
}

public struct HTTPResponse: Sendable {
    public var status: Int
    public var headers: HTTPHeaders
    public var body: Data

    public init(status: Int, headers: HTTPHeaders = [:], body: Data = Data()) {
        self.status = status
        self.headers = headers
        self.body = body
    }

    public init(status: Int, headers: HTTPHeaders = [:], text: String) {
        self.init(status: status, headers: headers, body: Data(text.utf8))
    }

    /// A JSON answer, for fakes.
    public init(status: Int = 200, headers: HTTPHeaders = ["content-type": "application/json"], json: JSONValue) {
        self.init(status: status, headers: headers, body: JSON.encodeData(json))
    }

    public var ok: Bool { (200...299).contains(status) }

    /// The body as UTF-8, malformed bytes replaced.
    public var text: String { Utf8.decodeLossy(body) }

    /// The decoded body; throws `JSONError` when it is not JSON.
    public func json() throws -> JSONValue { try JSON.parse(body) }

    /// The body as a JSON object, or an empty one for anything else.
    public var jsonObject: JSONObject { JSON.parseObject(body) }

    /// The relay's `{"error": "..."}` code, or "".
    public var errorCode: String { jsonObject.str("error") }

    public func header(_ name: String) -> String? { headers[name] }
}

/// A response whose body arrives in pieces. Dropping the stream cancels the transfer.
public struct HTTPStreamedResponse: Sendable {
    public var status: Int
    public var headers: HTTPHeaders
    public var body: AsyncThrowingStream<Data, Error>

    public init(status: Int, headers: HTTPHeaders = [:], body: AsyncThrowingStream<Data, Error>) {
        self.status = status
        self.headers = headers
        self.body = body
    }
}

public enum HTTPError: Error, Equatable, Sendable, CustomStringConvertible {
    case timedOut
    case cancelled
    case responseTooLarge(limit: Int)
    /// The answer was not HTTP.
    case invalidResponse
    /// No route, DNS, TLS, reset.
    case transport(String)

    public var description: String {
        switch self {
        case .timedOut: return "request timed out"
        case .cancelled: return "request cancelled"
        case .responseTooLarge(let limit): return "response larger than \(limit) bytes"
        case .invalidResponse: return "invalid response"
        case .transport(let message): return message
        }
    }
}

public protocol HTTPClient: Sendable {
    /// The whole exchange. Any HTTP status is a response, not an error;
    /// failures to get one throw `HTTPError`.
    func send(_ request: HTTPRequest) async throws -> HTTPResponse

    /// The same, handing the body over as it arrives. The default reads the
    /// whole body first.
    func stream(_ request: HTTPRequest) async throws -> HTTPStreamedResponse
}

extension HTTPClient {
    public func stream(_ request: HTTPRequest) async throws -> HTTPStreamedResponse {
        let response = try await send(request)
        let body = AsyncThrowingStream<Data, Error> { continuation in
            if !response.body.isEmpty { continuation.yield(response.body) }
            continuation.finish()
        }
        return HTTPStreamedResponse(status: response.status, headers: response.headers, body: body)
    }

    public func request(
        _ method: String,
        _ url: URL,
        headers: HTTPHeaders = [:],
        body: Data? = nil,
        timeout: TimeInterval = 30
    ) async throws -> HTTPResponse {
        try await send(HTTPRequest(method: method, url: url, headers: headers, body: body, timeout: timeout))
    }

    public func get(_ url: URL, headers: HTTPHeaders = [:], timeout: TimeInterval = 30) async throws -> HTTPResponse {
        try await request("GET", url, headers: headers, timeout: timeout)
    }

    public func post(
        _ url: URL, headers: HTTPHeaders = [:], body: Data? = nil, timeout: TimeInterval = 30
    ) async throws -> HTTPResponse {
        try await request("POST", url, headers: headers, body: body, timeout: timeout)
    }

    public func delete(_ url: URL, headers: HTTPHeaders = [:], timeout: TimeInterval = 30) async throws -> HTTPResponse {
        try await request("DELETE", url, headers: headers, timeout: timeout)
    }

    /// `client.post(url, headers: {...json...}, body: jsonEncode(payload))`.
    public func postJSON(
        _ url: URL, _ payload: JSONValue, headers: HTTPHeaders = [:], timeout: TimeInterval = 30
    ) async throws -> HTTPResponse {
        try await send(HTTPRequest(method: "POST", url: url, headers: headers, timeout: timeout).json(payload))
    }
}
