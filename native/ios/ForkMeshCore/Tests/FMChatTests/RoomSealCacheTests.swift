import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMChat

private final class WatchedSecureStore: SecureStore, @unchecked Sendable {
    private let inner = InMemorySecureStore()
    private let lock = NSLock()
    private var mainThreadCalls = 0
    private var writes = 0

    var onMain: Int { lock.withLock { mainThreadCalls } }
    var writeCount: Int { lock.withLock { writes } }

    private func note(write: Bool = false) {
        lock.withLock {
            if Thread.isMainThread { mainThreadCalls += 1 }
            if write { writes += 1 }
        }
    }

    func read(_ key: String) -> String? {
        note()
        return inner.read(key)
    }

    func write(_ key: String, _ value: String) {
        note(write: true)
        inner.write(key, value)
    }

    func delete(_ key: String) {
        note()
        inner.delete(key)
    }

    func readAll() -> [String: String] {
        note()
        return inner.readAll()
    }
}

@MainActor
final class RoomSealCacheTests: XCTestCase {
    func testTheKeychainIsReadOffTheMainThread() async throws {
        let store = WatchedSecureStore()
        let cache = RoomSealCache(secureStore: store)
        XCTAssertNil(cache["room-a"])
        let seal = try await cache.seal("room-a", storageScope: "direct|example.test")
        XCTAssertEqual(store.onMain, 0)
        XCTAssertGreaterThan(store.writeCount, 0)
        XCTAssertTrue(cache["room-a"] === seal)
        let again = try await cache.seal("room-a", storageScope: "direct|example.test")
        XCTAssertTrue(again === seal)
    }

    func testConcurrentFirstLoadsShareOneKey() async throws {
        let store = WatchedSecureStore()
        let cache = RoomSealCache(secureStore: store)
        async let first = cache.seal("room-b", storageScope: "office|example.test")
        async let second = cache.seal("room-b", storageScope: "office|example.test")
        let (a, b) = try await (first, second)
        XCTAssertTrue(a === b)
        let single = WatchedSecureStore()
        _ = try await RoomSealCache(secureStore: single).seal("room-b", storageScope: "office|example.test")
        XCTAssertEqual(store.writeCount, single.writeCount)
    }

    func testAnEmptyRoomFailsWithoutCaching() async {
        let cache = RoomSealCache(secureStore: WatchedSecureStore())
        do {
            _ = try await cache.seal(" ", storageScope: "direct|example.test")
            XCTFail("expected the error")
        } catch {}
        XCTAssertNil(cache[" "])
    }
}
