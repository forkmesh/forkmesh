// Shared doubles for the account-service tests (InboxServiceTests, NotificationServiceTests,
// AccountEventChannelTests, PushNotificationServiceTests, UserStatusServiceTests): a sleeper the
// test releases by hand (the Kotlin delay knobs), an async gate for holding a
// fake HTTP answer mid-flight, and polling waits. Names carry an `svc`/`Svc` prefix so they do
// not collide with the other helpers in this test target.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

/// The epoch the service tests start their clocks at.
let svcEpochMillis: Int64 = 1_770_000_000_000

/// An ApiService on `http` with no retry waits.
@MainActor
func svcApi(
    _ http: FakeHTTPClient, token: @escaping @MainActor () -> String = { "" },
    serverUrl: String = "wss://app.forkmesh.com/ws", clock: Clock = FixedClock(epochMillis: svcEpochMillis)
) -> ApiService {
    let api = ApiService(settings: StaticApiSettings(serverUrl: serverUrl), http: http, clock: clock, sleep: { _ in })
    api.sessionTokenProvider = token
    return api
}

/// Polls `condition` on the main actor until it holds (true) or `timeout` passes (false).
@MainActor
func svcEventually(_ timeout: TimeInterval = 2, _ condition: @MainActor () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if condition() { return true }
        await Task.yield()
        try? await Task.sleep(nanoseconds: 1_000_000)
    }
    return condition()
}

/// Lets every task already queued behind this one run.
@MainActor
func svcSettle(_ rounds: Int = 20) async {
    for _ in 0..<rounds {
        await Task.yield()
        try? await Task.sleep(nanoseconds: 200_000)
    }
}

/// A value the `@Sendable` fakes read and write while the test changes it.
final class SvcBox<T>: @unchecked Sendable {
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

    func mutate(_ body: (inout T) -> Void) {
        lock.lock()
        body(&current)
        lock.unlock()
    }
}

/// A `sleep` the test drives: every call parks until the test fires it (or the sleeping task is
/// cancelled, which throws `CancellationError` like `Task.sleep`). Records each duration asked for.
final class SvcSleeper: @unchecked Sendable {
    private struct Waiter {
        let id: Int
        let seconds: TimeInterval
        var continuation: CheckedContinuation<Void, Error>?
        var outcome: Result<Void, Error>?
    }

    private let lock = NSLock()
    private var nextId = 0
    private var waiters: [Waiter] = []
    private var log: [TimeInterval] = []

    /// Every duration asked for, oldest first.
    var requested: [TimeInterval] { locked { log } }

    /// Durations of the sleeps still parked.
    var pending: [TimeInterval] { locked { waiters.filter { $0.outcome == nil }.map(\.seconds) } }

    /// The closure to hand to the service under test.
    var sleep: @Sendable (TimeInterval) async throws -> Void {
        { [self] seconds in try await self.park(seconds) }
    }

    func park(_ seconds: TimeInterval) async throws {
        let id: Int = locked {
            nextId += 1
            log.append(seconds)
            waiters.append(Waiter(id: nextId, seconds: seconds))
            return nextId
        }
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                attach(id, continuation)
            }
        } onCancel: {
            settle(id, .failure(CancellationError()))
        }
    }

    /// Wakes every parked sleep of `seconds` (all of them when nil). Returns how many woke.
    @discardableResult
    func fire(_ seconds: TimeInterval? = nil) -> Int {
        let ids: [Int] = locked {
            waiters.filter { $0.outcome == nil && (seconds == nil || $0.seconds == seconds) }.map(\.id)
        }
        for id in ids { settle(id, .success(())) }
        return ids.count
    }

    private func attach(_ id: Int, _ continuation: CheckedContinuation<Void, Error>) {
        let ready: Result<Void, Error>? = locked {
            guard let index = waiters.firstIndex(where: { $0.id == id }) else { return .success(()) }
            if let outcome = waiters[index].outcome {
                waiters.remove(at: index)
                return outcome
            }
            waiters[index].continuation = continuation
            return nil
        }
        if let ready { continuation.resume(with: ready) }
    }

    private func settle(_ id: Int, _ outcome: Result<Void, Error>) {
        let continuation: CheckedContinuation<Void, Error>? = locked {
            guard let index = waiters.firstIndex(where: { $0.id == id }), waiters[index].outcome == nil else { return nil }
            if let continuation = waiters[index].continuation {
                waiters.remove(at: index)
                return continuation
            }
            waiters[index].outcome = outcome
            return nil
        }
        continuation?.resume(with: outcome)
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}

/// Holds async callers (a fake HTTP handler) until the test opens it.
final class SvcGate: @unchecked Sendable {
    private let lock = NSLock()
    private var open = false
    private var waiting: [CheckedContinuation<Void, Never>] = []
    private var arrivals = 0

    /// How many callers reached `wait()` so far.
    var arrived: Int {
        lock.lock()
        defer { lock.unlock() }
        return arrivals
    }

    func wait() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            lock.lock()
            arrivals += 1
            if open {
                lock.unlock()
                continuation.resume()
                return
            }
            waiting.append(continuation)
            lock.unlock()
        }
    }

    /// Releases everyone waiting and lets later callers through.
    func release() {
        lock.lock()
        open = true
        let all = waiting
        waiting = []
        lock.unlock()
        for continuation in all { continuation.resume() }
    }
}

/// `dartErrorText` of what `body` threw, failing the test when it did not throw.
@MainActor
func svcThrown<T>(
    _ body: @MainActor () async throws -> T, file: StaticString = #filePath, line: UInt = #line
) async -> Error? {
    do {
        _ = try await body()
        XCTFail("expected an error", file: file, line: line)
        return nil
    } catch {
        return error
    }
}
