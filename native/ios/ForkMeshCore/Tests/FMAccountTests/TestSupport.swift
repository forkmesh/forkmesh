import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest
@testable import FMAccount

/// The repository root (native/ios/ForkMeshCore/Tests/FMAccountTests/<file> is six levels down).
func repoRoot() -> URL {
    var url = URL(fileURLWithPath: #filePath)
    for _ in 0..<6 { url.deleteLastPathComponent() }
    return url
}

func loadConformance(_ relativePath: String) throws -> JSONObject {
    let data = try Data(contentsOf: repoRoot().appendingPathComponent(relativePath))
    return try XCTUnwrap(try JSON.parse(data).objectValue)
}

func jsonResponse(_ body: JSONObject, _ status: Int = 200) -> HTTPResponse {
    HTTPResponse(status: status, json: .object(body))
}

/// The request body as a JSON object (empty when there is none).
func bodyObject(_ request: HTTPRequest) -> JSONObject {
    request.body.map { JSON.parseObject($0) } ?? [:]
}

/// In-memory preferences and secure store, and the services the tests build from them.
@MainActor
struct Harness {
    let prefs: InMemoryKeyValueStore
    let secure: InMemorySecureStore
    let settings: SettingsService

    init(prefs seed: JSONObject = [:], secure secureSeed: [String: String] = [:], env: AppEnv = .prod) {
        prefs = InMemoryKeyValueStore(seed)
        secure = InMemorySecureStore(secureSeed)
        settings = SettingsService.create(prefs: prefs, secureStore: secure, env: env)
    }

    func identity() throws -> Identity {
        try Identity.loadOrCreate(secureStore: secure, prefs: prefs)
    }

    func auth(
        _ identity: DeviceIdentity?,
        http: HTTPClient = FakeHTTPClient { _ in HTTPResponse(status: 200, json: [:]) },
        deviceLabel: String? = nil,
        timestamp: String? = nil,
        monitor: PerformanceMonitorService? = nil
    ) -> AuthService {
        AuthService.create(
            settings: settings, identity: identity, http: http, performanceMonitor: monitor,
            deviceLabelProvider: deviceLabel.map { label in { label } },
            deviceTimestampProvider: timestamp.map { ts in { ts } })
    }

    /// `AuthService(settings, identity, prefs, session: ...)`: no startup migration or load.
    func plainAuth(_ identity: DeviceIdentity?, session: AuthSession? = nil, http: HTTPClient? = nil) -> AuthService {
        AuthService(
            settings: settings, identity: identity,
            http: http ?? FakeHTTPClient { _ in HTTPResponse(status: 200, json: [:]) }, session: session)
    }
}

/// Thread-safe counter for listener callbacks.
final class Counter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0

    func increment() {
        lock.lock()
        count += 1
        lock.unlock()
    }

    var value: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }
}

/// Thread-safe string recorder (log sinks).
final class Lines: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [String] = []

    func add(_ line: String) {
        lock.lock()
        items.append(line)
        lock.unlock()
    }

    var all: [String] {
        lock.lock()
        defer { lock.unlock() }
        return items
    }
}
