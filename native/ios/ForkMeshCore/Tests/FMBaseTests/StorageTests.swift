// The preferences and secure store surfaces, including the secure store migration, plus Clock,
// Log and Services.

import XCTest
@testable import FMBase

final class StorageTests: XCTestCase {
    private func exercise(_ store: KeyValueStore) {
        XCTAssertNil(store.string(forKey: "server/url"))
        XCTAssertNil(store.bool(forKey: "flag"))
        XCTAssertEqual(store.bool(forKey: "flag", or: true), true)
        store.set("wss://x", forKey: "server/url")
        store.set(true, forKey: "flag")
        store.set(7, forKey: "count")
        store.set(1.0, forKey: "scale")
        store.set(["a", "b"], forKey: "list")
        XCTAssertEqual(store.string(forKey: "server/url"), "wss://x")
        XCTAssertEqual(store.bool(forKey: "flag"), true)
        XCTAssertEqual(store.int(forKey: "count"), 7)
        XCTAssertEqual(store.double(forKey: "scale"), 1.0)
        XCTAssertEqual(store.stringList(forKey: "list"), ["a", "b"])
        // Another type reads as absent.
        XCTAssertNil(store.string(forKey: "flag"))
        XCTAssertNil(store.bool(forKey: "server/url"))
        XCTAssertNil(store.int(forKey: "scale"))
        XCTAssertNil(store.stringList(forKey: "count"))
        XCTAssertEqual(store.keys, ["server/url", "flag", "count", "scale", "list"])
        XCTAssertTrue(store.contains("flag"))
        store.remove("flag")
        XCTAssertFalse(store.contains("flag"))
        store.set(nil as String?, forKey: "server/url")
        XCTAssertNil(store.string(forKey: "server/url"))
        XCTAssertEqual(store.string(forKey: "server/url", or: "fallback"), "fallback")
    }

    func testInMemoryKeyValueStore() {
        let store = InMemoryKeyValueStore(["profile/name": "alice"])
        XCTAssertEqual(store.string(forKey: "profile/name"), "alice")
        store.remove("profile/name")
        exercise(store)
        store.clear()
        XCTAssertTrue(store.keys.isEmpty)
    }

    func testFileKeyValueStorePersists() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("fmbase-\(UUID().uuidString)")
        let url = dir.appendingPathComponent("nested/prefs.json")
        defer { try? FileManager.default.removeItem(at: dir) }

        exercise(FileKeyValueStore(url: url))
        let reopened = FileKeyValueStore(url: url)
        XCTAssertEqual(reopened.int(forKey: "count"), 7)
        // A whole double is still a double after the round trip through JSON.
        XCTAssertEqual(reopened.double(forKey: "scale"), 1.0)
        XCTAssertNil(reopened.int(forKey: "scale"))
        XCTAssertEqual(reopened.stringList(forKey: "list"), ["a", "b"])
        XCTAssertFalse(reopened.contains("flag"))
        reopened.clear()
        XCTAssertTrue(FileKeyValueStore(url: url).keys.isEmpty)

        try Data("not json".utf8).write(to: url)
        XCTAssertTrue(FileKeyValueStore(url: url).keys.isEmpty)
    }

    func testSecureStoreSurface() {
        let store = InMemorySecureStore(["a": "1"])
        XCTAssertEqual(store.read("a"), "1")
        XCTAssertNil(store.read("b"))
        store.write("b", "2")
        XCTAssertEqual(store.readAll(), ["a": "1", "b": "2"])
        store.delete("a")
        XCTAssertNil(store.read("a"))
        store.delete("a")
    }

    func testMigratesSecretsOffPreferences() {
        let prefs = InMemoryKeyValueStore([
            "identity/seed": "AAAA", "import/github_token": "ghp_legacy", "empty": "", "profile/name": "alice",
            "already": "stale",
        ])
        let secure = InMemorySecureStore(["already": "fresh"])
        secure.migrateFromPrefs(prefs, keys: ["identity/seed", "import/github_token", "empty", "already", "missing"])
        XCTAssertEqual(secure.read("identity/seed"), "AAAA")
        XCTAssertEqual(secure.read("import/github_token"), "ghp_legacy")
        XCTAssertEqual(secure.read("already"), "fresh")
        XCTAssertNil(secure.read("empty"))
        XCTAssertNil(prefs.string(forKey: "identity/seed"))
        XCTAssertNil(prefs.string(forKey: "import/github_token"))
        XCTAssertNil(prefs.string(forKey: "already"))
        XCTAssertEqual(prefs.string(forKey: "profile/name"), "alice")
    }

    func testClocks() {
        let clock = FixedClock(epochMillis: 1_786_636_663_064)
        XCTAssertEqual(clock.nowMillis, 1_786_636_663_064)
        XCTAssertEqual(clock.nowSeconds, 1_786_636_663)
        clock.advance(by: 1.5)
        XCTAssertEqual(clock.nowMillis, 1_786_636_664_564)
        clock.now = Date(epochMillis: 5)
        XCTAssertEqual(clock.nowMillis, 5)
        XCTAssertGreaterThan(SystemClock().nowMillis, 1_700_000_000_000)
        XCTAssertEqual(Date(epochMillis: -1).epochMillis, -1)
    }

    func testLogSinkAndLevel() {
        final class Lines: @unchecked Sendable { var items = [String]() }
        let lines = Lines()
        let oldSink = Log.sink
        let oldLevel = Log.minimumLevel
        defer { Log.sink = oldSink; Log.minimumLevel = oldLevel }
        Log.sink = { level, tag, message in lines.items.append("\(level.label) \(tag) \(message)") }
        Log.minimumLevel = .info
        var built = false
        Log.debug("relay", { built = true; return "dropped" }())
        Log.info("relay", "connected")
        Log.error("api", "failed")
        XCTAssertFalse(built, "a dropped line never builds its message")
        XCTAssertEqual(lines.items, ["info relay connected", "error api failed"])
    }

    func testServicesRegistry() {
        defer { Services.clear() }
        XCTAssertNil(Services.get(InMemorySecureStore.self))
        let store = Services.put(InMemorySecureStore())
        XCTAssertTrue(Services.get(InMemorySecureStore.self) === store)
        let required: InMemorySecureStore = Services.require()
        XCTAssertTrue(required === store)
        // Registering under a protocol is explicit.
        XCTAssertNil(Services.get(SecureStore.self))
        Services.put(store, as: SecureStore.self)
        XCTAssertTrue(Services.get(SecureStore.self) === store)
        Services.remove(InMemorySecureStore.self)
        XCTAssertNil(Services.get(InMemorySecureStore.self))
        Services.clear()
        XCTAssertNil(Services.get(SecureStore.self))
    }
}
