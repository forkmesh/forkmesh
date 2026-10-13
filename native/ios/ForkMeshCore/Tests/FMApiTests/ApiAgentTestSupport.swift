// Shared helpers for the ApiService-level ports in ApiAgentsTests, ApiMergeTests,
// ApiOwnerSealedTests, ApiOrgsTests, ApiUserStatusTests, ApiSponsoredAdTests,
// ApiBugReportTests and ApiMobileNodeTests: ApiService builders over a fake client
// and small readers over recorded `HTTPRequest`s. Names carry an `api`/`Api`
// prefix so they do not collide with other helpers in this test target.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

enum ApiPortFixture {
    /// `_api(client, token:)`: an ApiService on `serverUrl` with the waits skipped. An empty
    /// `token` leaves the client signed out.
    @MainActor
    static func api(
        _ http: FakeHTTPClient, token: String = "session-token", serverUrl: String = "ws://relay.test/ws",
        clock: Clock = FixedClock(epochMillis: 1_700_000_000_000)
    ) -> ApiService {
        let api = ApiService(settings: StaticApiSettings(serverUrl: serverUrl), http: http, clock: clock, sleep: { _ in })
        if !token.isEmpty { api.sessionTokenProvider = { token } }
        return api
    }
}

/// `http.Response(jsonEncode(json), status, headers: {'content-type': 'application/json'})`.
func portJSON(_ json: JSONValue, _ status: Int = 200) -> HTTPResponse {
    HTTPResponse(status: status, json: json)
}

/// The percent-encoded path.
func portRawPath(_ request: HTTPRequest) -> String {
    URLComponents(url: request.url, resolvingAgainstBaseURL: false)?.percentEncodedPath ?? request.url.path
}

/// `"METHOD /path"`.
func portMethodPath(_ request: HTTPRequest) -> String { "\(request.method) \(portRawPath(request))" }

func portQuery(_ request: HTTPRequest) -> [String: String] {
    var out: [String: String] = [:]
    for item in URLComponents(url: request.url, resolvingAgainstBaseURL: false)?.queryItems ?? [] {
        out[item.name] = item.value ?? ""
    }
    return out
}

/// `jsonDecode(request.body) as Map`.
func portBody(_ request: HTTPRequest?) -> JSONObject {
    JSON.parseObject(request?.bodyText ?? "")
}

/// The error `body` threw, or nil when it returned.
@MainActor
func portCaughtError<T>(_ body: @MainActor () async throws -> T) async -> Error? {
    do {
        _ = try await body()
        return nil
    } catch {
        return error
    }
}

/// A lock-protected value for `@Sendable` fake-server handlers that need state.
final class PortBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: T

    init(_ value: T) { stored = value }

    var value: T {
        get { lock.lock(); defer { lock.unlock() }; return stored }
        set { lock.lock(); stored = newValue; lock.unlock() }
    }
}

/// `jsonEncode(value)` for a JSON literal (JSON.encode is overloaded for objects).
func portEncode(_ value: JSONValue) -> String { JSON.encode(value) }
