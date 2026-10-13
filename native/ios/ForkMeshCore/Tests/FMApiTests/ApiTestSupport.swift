// Test doubles shared by the ApiService tests: the counterpart of
// native/android core/src/test/.../api/ApiTestSupport.kt (FakeTransport, jsonResponse,
// textResponse, request path/query/body readers) on top of FMNet's FakeHTTPClient.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

/// The epoch the ApiService tests start their `FixedClock` at.
let apiTestEpochMillis: Int64 = 1_770_000_000_000

/// An ApiService on `http` with a fixed clock and no waits, as the Kotlin tests' `api(...)`.
@MainActor
func makeTestApi(
    _ http: FakeHTTPClient, token: String = "", url: String = "ws://test.local/ws",
    clock: FixedClock = FixedClock(epochMillis: apiTestEpochMillis)
) -> ApiService {
    let api = ApiService(settings: StaticApiSettings(serverUrl: url), http: http, clock: clock, sleep: { _ in })
    if !token.isEmpty { api.sessionTokenProvider = { token } }
    return api
}

/// A JSON answer (`jsonResponse` in Kotlin).
func apiJSON(_ json: JSONValue, status: Int = 200) -> HTTPResponse {
    HTTPResponse(status: status, json: json)
}

/// A plain-text answer with no content type (`textResponse` in Kotlin).
func apiText(_ text: String, status: Int = 200) -> HTTPResponse {
    HTTPResponse(status: status, text: text)
}

/// A FakeHTTPClient that routes by `(path, query, request)`; nil means `404 not found`
/// (the Kotlin `routeByPath`).
func apiRouteByPath(
    _ routes: @escaping @Sendable (String, [String: String], HTTPRequest) -> HTTPResponse?
) -> FakeHTTPClient {
    FakeHTTPClient { request in
        routes(request.apiPath, request.apiQuery, request) ?? apiText("not found", status: 404)
    }
}

extension FakeHTTPClient {
    /// Replaces how every later request is answered (Kotlin's `transport.handler = ...`): a
    /// catch-all route, and later routes win.
    func answerAll(_ handler: @escaping @Sendable (HTTPRequest) throws -> HTTPResponse) {
        on(where: { _ in true }) { request in try handler(request) }
    }
}

extension HTTPRequest {
    /// The decoded path.
    var apiPath: String { url.path }

    /// The raw query string, "" when there is none.
    var apiRawQuery: String {
        URLComponents(url: url, resolvingAgainstBaseURL: false)?.percentEncodedQuery ?? ""
    }

    /// The query: `+` is a space, then percent-decoding.
    var apiQuery: [String: String] {
        var out: [String: String] = [:]
        for pair in apiRawQuery.split(separator: "&") where !pair.isEmpty {
            let parts = pair.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            func decode(_ text: Substring) -> String {
                let spaced = text.replacingOccurrences(of: "+", with: " ")
                return spaced.removingPercentEncoding ?? spaced
            }
            out[decode(parts[0])] = parts.count > 1 ? decode(parts[1]) : ""
        }
        return out
    }

    /// The body as a JSON object (empty when it is not one).
    var apiBodyJSON: JSONObject { JSON.parseObject(bodyText) }

    func apiHeader(_ name: String) -> String? { headers[name] }
}

/// A counter the `@Sendable` fake handlers can bump.
final class ApiCallCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var current = 0

    @discardableResult
    func next() -> Int {
        lock.lock()
        defer { lock.unlock() }
        current += 1
        return current
    }

    var value: Int {
        lock.lock()
        defer { lock.unlock() }
        return current
    }
}

/// A value the `@Sendable` fake handlers read while the test changes it.
final class ApiShared<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var current: T

    init(_ value: T) { current = value }

    var value: T {
        get {
            lock.lock()
            defer { lock.unlock() }
            return current
        }
        set {
            lock.lock()
            current = newValue
            lock.unlock()
        }
    }
}

/// Awaits `body` and returns what it threw, failing the test when it did not throw.
@MainActor
func apiAssertThrows<T>(
    _ body: @MainActor () async throws -> T, _ message: String = "", file: StaticString = #filePath, line: UInt = #line
) async -> Error? {
    do {
        _ = try await body()
        XCTFail("expected an error. \(message)", file: file, line: line)
        return nil
    } catch {
        return error
    }
}

/// `dartErrorText` of whatever was thrown ("" when nothing was).
func apiErrorText(_ error: Error?) -> String {
    guard let error else { return "" }
    return dartErrorText(error)
}

/// The bare message of an `ApiException` (Kotlin's `DartException.message`), else `apiErrorText`.
func apiErrorMessage(_ error: Error?) -> String {
    if let api = error as? ApiException { return api.message }
    return apiErrorText(error)
}
