// Shared doubles for NodeSyncServiceTests and NodeSyncApplierTests: sessions, a sync body and the
// harness, a sleeper the test releases by hand, and polling waits. Everything carries a
// `nodeSync`/`NodeSync` prefix so it cannot collide with the other FMChat tests in this target.

import FMAccount
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest
@testable import FMChat

/// `_mobileSession`: a `mobile_node` bind (owner_sign, no host_repo), no desktop by default.
func nodeSyncMobileSession(name: String = "alice", withDesktop: Bool = false) -> AuthSession {
    var devices: [JSONValue] = [
        [
            "id": "dev-mobile-1",
            "kind": "mobile_node",
            "label": "iPhone",
            "capabilities": ["publish_repo", "owner_sign"],
            "enabled": true,
            "current": true,
        ],
    ]
    if withDesktop {
        devices.append([
            "id": "desk-1",
            "kind": "desktop_node",
            "label": "MacBook Pro",
            "capabilities": ["host_repo", "publish_repo", "owner_sign"],
            "enabled": true,
        ])
    }
    return AuthSession.fromJSON([
        "nodeName": .string(name),
        "email": .string("\(name)@example.com"),
        "status": "active",
        "sessionToken": "mobile-session",
        "sessionKind": "mobile_node",
        "deviceKind": "mobile_node",
        "deviceId": "dev-mobile-1",
        "desktopCapable": false,
        "capabilities": ["browse", "comment", "submit_issue", "submit_pr", "publish_repo", "owner_sign"],
        "devices": .array(devices),
    ])
}

/// `_plainSession`: an ordinary account session that cannot owner-sign.
func nodeSyncPlainSession() -> AuthSession {
    AuthSession.fromJSON([
        "nodeName": "alice",
        "email": "alice@example.com",
        "status": "active",
        "sessionToken": "plain-session",
        "sessionKind": "account",
        "capabilities": ["browse", "comment", "submit_issue", "submit_pr"],
    ])
}

/// `_syncBody`: a one-repo `/api/sync` body carrying one of every kind the Worker sends.
func nodeSyncBody(issueId: Int = 11, pullId: Int = 21, discussionId: Int = 31) -> JSONValue {
    [
        "ok": true,
        "serverTime": 1_760_000_000_000,
        "repos": [
            [
                "owner": "alice",
                "name": "notes",
                "stateHash": "abc123",
                "updatedAt": "1760000000000",
                "source": "local-node",
                "issues": [["id": .int(Int64(issueId)), "number": 4, "title": "Crash on launch"]],
                "pulls": [["id": .int(Int64(pullId)), "number": 9, "title": "Fix the crash"]],
                "discussions": [["id": .int(Int64(discussionId)), "number": 2, "title": "Roadmap"]],
                "agentPrompts": [["queueId": 41, "agentId": 3, "queuedAt": 5, "encrypted": true]],
                "aboutUpdate": ["about": "A notes repository"],
                // Branch management typed on the website. No phone can run Git for the
                // repository, so this is carried rather than applied.
                "branchOps": [
                    [
                        "id": 7,
                        "op": "delete",
                        "branch": "agent/old-work",
                        "expectedOid": .string(String(repeating: "b", count: 40)),
                        "requestedBy": "alice",
                        "queuedAt": 5,
                    ],
                ],
                "agentPromptAckPath": "/api/repo/alice/notes/agents/ack",
                "mirrorMerges": [["requestId": "req-1", "pullNumber": 9]],
            ],
        ],
    ]
}

/// A value the `@Sendable` fakes read while the test changes it.
final class NodeSyncBox<T>: @unchecked Sendable {
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

/// The query of `url`, decoded ('+' is a space).
func nodeSyncQuery(_ url: URL?) -> [String: String] {
    guard let url, let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems else { return [:] }
    var out: [String: String] = [:]
    for item in items { out[item.name] = (item.value ?? "").replacingOccurrences(of: "+", with: " ") }
    return out
}

/// Polls `condition` on the main actor until it holds (true) or `timeout` passes (false).
@MainActor
func nodeSyncEventually(_ timeout: TimeInterval = 2, _ condition: @MainActor () -> Bool) async -> Bool {
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
func nodeSyncSettle(_ rounds: Int = 30) async {
    for _ in 0..<rounds {
        await Task.yield()
        try? await Task.sleep(nanoseconds: 300_000)
    }
}

/// A `sleep` the test drives: every call parks until the test fires it (or the sleeping task is
/// cancelled, which throws `CancellationError` like `Task.sleep`). Records each duration.
final class NodeSyncSleeper: @unchecked Sendable {
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

    /// Wakes every parked sleep whose duration matches. Returns how many woke.
    @discardableResult
    func fire(where matches: (TimeInterval) -> Bool = { _ in true }) -> Int {
        let ids: [Int] = locked { waiters.filter { $0.outcome == nil && matches($0.seconds) }.map(\.id) }
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

/// `_harness`: a node on `ws://relay.test/ws` with "Serve as a node" on, a fake HTTP client
/// answering `_syncBody()` by default, a fake socket connector and a hand-driven sleeper.
@MainActor
final class NodeSyncHarness {
    let prefs: InMemoryKeyValueStore
    let secure: InMemorySecureStore
    let settings: SettingsService
    let identity: Identity
    let http: FakeHTTPClient
    let ws: FakeWebSocketConnector
    let sleeper: NodeSyncSleeper
    let node: NodeSyncService

    init(
        session: @escaping @MainActor () -> AuthSession? = { nodeSyncMobileSession() },
        handler: FakeHTTPClient.Handler? = nil,
        serveAsNode: Bool = true,
        random: @escaping @MainActor (Int) -> Int = { $0 / 2 },
        prefs: InMemoryKeyValueStore? = nil,
        secure: InMemorySecureStore? = nil,
        identity: Identity? = nil
    ) throws {
        let prefs = prefs ?? InMemoryKeyValueStore([:])
        let secure = secure ?? InMemorySecureStore([:])
        self.prefs = prefs
        self.secure = secure
        settings = SettingsService(prefs: prefs, secureStore: secure)
        settings.setServerUrl("ws://relay.test/ws")
        if serveAsNode { settings.setServeAsNode(true) }
        self.identity = try identity ?? Identity.loadOrCreate(secureStore: secure, prefs: prefs)
        http = FakeHTTPClient(handler ?? { _ in HTTPResponse(status: 200, json: nodeSyncBody()) })
        ws = FakeWebSocketConnector()
        sleeper = NodeSyncSleeper()
        node = NodeSyncService(
            settings: settings, identity: self.identity, session: session, http: http, connector: ws,
            random: random, clock: FixedClock(epochMillis: 1_760_000_000_000), sleep: sleeper.sleep)
    }

    /// Requests seen so far.
    var requests: [HTTPRequest] { http.requests }
    var methods: [String] { http.requests.map(\.method) }
}
