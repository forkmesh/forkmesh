// ServerURL / DartURI and FallbackSecureStore (the platform secure store backend).

import FMBase
import XCTest
@testable import FMAccount

final class ServerURLTests: XCTestCase {
    func testDerivesHttpAndSocketEndpointsLikeDartUri() {
        let prod = ServerURL.parse(AppEnv.prodServerUrl)
        XCTAssertEqual(prod.scheme, "wss")
        XCTAssertEqual(prod.host, "app.forkmesh.com")
        XCTAssertNil(prod.port)
        XCTAssertEqual(prod.http("/api/repositories"), "https://app.forkmesh.com/api/repositories")
        XCTAssertEqual(prod.webSocket("/api/nodes/events", query: [("ticket", "a b")]), "wss://app.forkmesh.com/api/nodes/events?ticket=a+b")
        XCTAssertEqual(prod.httpOrigin, "https://app.forkmesh.com")

        let local = ServerURL.parse("ws://LocalHost:8787/x")
        XCTAssertEqual(local.host, "localhost")
        XCTAssertEqual(local.port, 8787)
        XCTAssertEqual(local.http("api/x", query: [("n", "1"), ("q", "é&")]), "http://localhost:8787/api/x?n=1&q=%C3%A9%26")
        XCTAssertEqual(local.webSocket("/ws"), "ws://localhost:8787/ws")

        let v6 = ServerURL.parse("wss://[::1]:9443/ws")
        XCTAssertEqual(v6.host, "::1")
        XCTAssertEqual(v6.http("/a"), "https://[::1]:9443/a")

        XCTAssertEqual(ServerURL.parse("wss://user:pw@relay.test:443/ws").http("/a"), "https://relay.test/a")
        XCTAssertEqual(ServerURL.parse("ws://relay.test:80/ws").port, 80, "ws keeps an explicit :80, as Dart's Uri does")
        XCTAssertEqual(ServerURL.parse("ws://relay.test:80/ws").http("/a"), "http://relay.test/a")
        XCTAssertEqual(ServerURL.parse("").host, "")
        XCTAssertEqual(ServerURL.parse("not a url").http("/a"), "https:///a")
    }

    func testDartUriEncoding() {
        XCTAssertEqual(DartURI.encodeComponent("a b/c?é"), "a%20b%2Fc%3F%C3%A9")
        XCTAssertEqual(DartURI.encodeComponent("-_.!~*'()"), "-_.!~*'()")
        XCTAssertEqual(DartURI.encodeQueryComponent("a b+c"), "a+b%2Bc")
        XCTAssertEqual(DartURI.encodePath("/a b/%2f/%zz"), "/a%20b/%2F/%25zz")
        XCTAssertEqual(DartURI.decodePercent("a%20b%zz"), "a b%zz")
    }
}

final class FallbackSecureStoreTests: XCTestCase {
    final class FlakyBackend: ThrowingSecureBackend, @unchecked Sendable {
        struct Failure: Error {}
        var values: [String: String] = [:]
        var failing = false
        var deletes: [String] = []

        func read(_ key: String) throws -> String? {
            if failing { throw Failure() }
            return values[key]
        }

        func write(_ key: String, _ value: String) throws {
            if failing { throw Failure() }
            values[key] = value
        }

        func delete(_ key: String) throws {
            deletes.append(key)
            values.removeValue(forKey: key)
        }

        func readAll() throws -> [String: String] {
            if failing { throw Failure() }
            return values
        }
    }

    func testUsesThePlatformStoreUntilItFails() {
        let backend = FlakyBackend()
        let store = FallbackSecureStore(backend: backend)
        store.write("a", "1")
        XCTAssertEqual(backend.values, ["a": "1"])
        XCTAssertEqual(store.read("a"), "1")
        XCTAssertFalse(store.isUsingFallback)

        backend.failing = true
        store.write("b", "2")
        XCTAssertTrue(store.isUsingFallback)
        XCTAssertEqual(store.read("b"), "2", "a write after the failure is kept for the session")
        XCTAssertNil(store.read("a"), "memory takes over reads")
        XCTAssertEqual(store.readAll(), ["b": "2"])

        // Deletes still reach the platform store, so a session written there before the failure
        // cannot survive Sign out.
        store.delete("a")
        store.delete("b")
        XCTAssertEqual(backend.deletes, ["a", "b"])
        XCTAssertNil(backend.values["a"])
        XCTAssertNil(store.read("b"))
    }

    func testMigrateFromPrefsWorksOverTheFallbackStore() {
        let store = FallbackSecureStore(backend: FlakyBackend())
        let prefs = InMemoryKeyValueStore(["import/githubToken": "ghp"])
        store.migrateFromPrefs(prefs, keys: ["import/githubToken"])
        XCTAssertEqual(store.read("import/githubToken"), "ghp")
        XCTAssertNil(prefs.string(forKey: "import/githubToken"))
    }
}
