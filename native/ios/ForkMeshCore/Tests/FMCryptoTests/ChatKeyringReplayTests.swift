import FMBase
import Foundation
import XCTest

@testable import FMCrypto

final class ChatKeyringReplayTests: XCTestCase {
    final class WatchedStore: SecureStore, @unchecked Sendable {
        let base = InMemorySecureStore()
        private let lock = NSLock()
        private var writes: [(key: String, onCaller: Bool, bytes: Int)] = []
        var caller: Thread?

        var seenWrites: [(key: String, onCaller: Bool, bytes: Int)] {
            lock.lock()
            defer { lock.unlock() }
            return writes.filter { $0.key.hasPrefix("chat/e2ee/seenV3/") }
        }

        func read(_ key: String) -> String? { base.read(key) }

        func write(_ key: String, _ value: String) {
            lock.lock()
            writes.append((key, Thread.current == caller, value.utf8.count))
            lock.unlock()
            base.write(key, value)
        }

        func delete(_ key: String) { base.delete(key) }

        func readAll() -> [String: String] { base.readAll() }
    }

    private let room = "world-general"
    private let seed = [UInt8](repeating: 7, count: Ed25519.seedBytes)

    func testABurstOfFramesWritesTheReplayBookOnceAndOffTheCallingThread() throws {
        let store = WatchedStore()
        store.caller = Thread.current
        let keyring = try ChatKeyring.fromSecrets(identitySeed: seed, store: store)
        for sender in 0..<65 {
            XCTAssertTrue(keyring.recordSeen(room, "peer\(sender)", 1))
        }
        XCTAssertFalse(keyring.recordSeen(room, "peer3", 1))
        XCTAssertTrue(keyring.isReplay(room, "peer3", 1))
        XCTAssertTrue(store.seenWrites.isEmpty, "a hello burst wrote the replay book \(store.seenWrites.count) times")

        let written = expectation(description: "replay book written")
        DispatchQueue.global().async {
            while store.seenWrites.isEmpty { Thread.sleep(forTimeInterval: 0.02) }
            written.fulfill()
        }
        wait(for: [written], timeout: 5)
        Thread.sleep(forTimeInterval: 0.1)
        XCTAssertEqual(store.seenWrites.count, 1)
        XCTAssertEqual(store.seenWrites.first?.onCaller, false)

        let reloaded = try ChatKeyring.fromSecrets(identitySeed: seed, store: store)
        XCTAssertTrue(reloaded.isReplay(room, "peer64", 1))
        XCTAssertFalse(reloaded.isReplay(room, "peer64", 2))
    }

    func testFlushWritesWhatIsPendingAndNothingWhenClean() throws {
        let store = WatchedStore()
        let keyring = try ChatKeyring.fromSecrets(identitySeed: seed, store: store)
        keyring.flushReplay()
        XCTAssertTrue(store.seenWrites.isEmpty)
        XCTAssertTrue(keyring.recordSeen(room, "peer", 5))
        XCTAssertTrue(keyring.recordSeen("other-room", "peer", 9))
        keyring.flushReplay()
        XCTAssertEqual(Set(store.seenWrites.map(\.key)).count, 2)
        Thread.sleep(forTimeInterval: 0.4)
        XCTAssertEqual(store.seenWrites.count, 2, "the scheduled write found nothing left to write")
        XCTAssertTrue(try ChatKeyring.fromSecrets(identitySeed: seed, store: store).isReplay("other-room", "peer", 9))
    }

    func testMeasureTheReplayBookOfABusyRoom() throws {
        let store = WatchedStore()
        let keyring = try ChatKeyring.fromSecrets(identitySeed: seed, store: store)
        for sender in 0..<60 {
            for counter in 1...300 { _ = keyring.recordSeen(room, "peer\(sender)-0123456789", Int64(counter)) }
        }
        keyring.flushReplay()
        let bytes = try XCTUnwrap(store.seenWrites.last?.bytes)
        _ = keyring.recordSeen(room, "peer0-0123456789", 301)
        let encodeStarted = Date()
        keyring.flushReplay()
        let encodeMs = Date().timeIntervalSince(encodeStarted) * 1000
        let started = Date()
        for sender in 0..<65 { _ = keyring.recordSeen(room, "peer\(sender % 60)-0123456789", 302 + Int64(sender)) }
        let burstMs = Date().timeIntervalSince(started) * 1000
        print(String(format: "replay book %d bytes; one write %.1f ms; 65 frames now %.1f ms on the caller", bytes, encodeMs, burstMs))
        XCTAssertGreaterThan(bytes, 50_000)
    }
}
