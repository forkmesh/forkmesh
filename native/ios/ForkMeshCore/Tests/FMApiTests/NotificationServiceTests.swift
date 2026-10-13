// The notification service, as in native/android
// core/src/test/.../api/NotificationInboxTest.kt. The Kotlin tests subclass ApiService;
// ApiService is final here, so a FakeHTTPClient plays the Worker's /api/notifications.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

/// The Worker's `/api/notifications` for one test.
private final class PingsServer: @unchecked Sendable {
    /// Row ids of the page, each suffixed with `-node`, all unread.
    let ids = SvcBox(["a", "b"])
    /// Explicit rows instead of `ids` (fixed pages).
    let rows = SvcBox<[JSONValue]?>(nil)
    let unread = SvcBox<Int?>(nil)
    let total = SvcBox<Int?>(nil)
    /// Answer reads with this HTTP status instead of a page.
    let failReads = SvcBox<Int?>(nil)
    /// Answer deletes with this HTTP status instead of ok.
    let refuseDeletes = SvcBox<Int?>(nil)
    /// Holds each read until the test opens it.
    let gate = SvcBox<SvcGate?>(nil)
    let reads = SvcBox(0)
    let readNodes = SvcBox<[String]>([])
    let marked = SvcBox<[JSONObject]>([])
    let deleted = SvcBox<[JSONObject]>([])

    lazy var http = FakeHTTPClient { request in try await self.answer(request) }

    private func answer(_ request: HTTPRequest) async throws -> HTTPResponse {
        guard request.url.path == "/api/notifications" else { return HTTPResponse(status: 404, text: "not found") }
        switch request.method {
        case "GET":
            reads.mutate { $0 += 1 }
            let node = URLComponents(url: request.url, resolvingAgainstBaseURL: false)?
                .queryItems?.first(where: { $0.name == "node" })?.value ?? ""
            readNodes.mutate { $0.append(node) }
            if let gate = gate.value { await gate.wait() }
            if let status = failReads.value { return HTTPResponse(status: status, text: "down") }
            let list = rows.value ?? ids.value.map { id -> JSONValue in
                .object(["id": .string("\(id)-\(node)"), "kind": "mention", "title": .string("Notification for \(node)")])
            }
            var page: JSONObject = ["notifications": .array(list), "unread": .int(Int64(unread.value ?? list.count))]
            if let total = total.value { page["total"] = .int(Int64(total)) }
            return HTTPResponse(status: 200, json: .object(page))
        case "POST":
            marked.mutate { $0.append(JSON.parseObject(request.bodyText)) }
            return HTTPResponse(status: 200, json: ["ok": true])
        case "DELETE":
            deleted.mutate { $0.append(JSON.parseObject(request.bodyText)) }
            if let status = refuseDeletes.value { return HTTPResponse(status: status, text: "nope") }
            return HTTPResponse(status: 200, json: ["ok": true])
        default:
            return HTTPResponse(status: 405, text: "")
        }
    }
}

@MainActor
final class NotificationServiceTests: XCTestCase {
    private func make(
        _ server: PingsServer, node: @escaping @MainActor () -> String, clock: Clock = FixedClock(epochMillis: svcEpochMillis)
    ) -> NotificationService {
        let api = svcApi(server.http, token: { "session-token" })
        return NotificationService(api: api, nodeName: node, clock: clock)
    }

    /// DeleteAllApiService's page: one unread mention, one read host row, nine in the inbox.
    private func deleteAllPage(_ server: PingsServer) {
        server.rows.value = [
            ["id": "n1", "kind": "mention", "title": "Mentioned"],
            ["id": "n2", "kind": "host_offline", "title": "Host offline", "readAt": 5],
        ]
        server.unread.value = 1
        server.total.value = 9
    }

    // MARK: notification service

    func testAFreshTrackedEmailPingPartiesOnceAndOldOrReadOnesNever() async {
        let server = PingsServer()
        let now = Int64(svcEpochMillis)
        let party: JSONValue = .object(["celebrate": "crm-email-tracked"])
        server.rows.value = [
            ["id": "fresh", "kind": "plugin_activity", "title": "Email tracked", "ts": .int(now - 20_000), "meta": party],
            ["id": "older", "kind": "plugin_activity", "title": "Earlier", "ts": .int(now - 60_000), "meta": party],
            ["id": "read", "kind": "plugin_activity", "title": "Read", "ts": .int(now - 1_000), "readAt": .int(now), "meta": party],
            ["id": "stale", "kind": "plugin_activity", "title": "Stale", "ts": .int(now - 10 * 60_000), "meta": party],
            ["id": "plain", "kind": "mention", "title": "Mention", "ts": .int(now)],
        ]
        let service = make(server, node: { "alice" })
        await service.refresh()
        XCTAssertEqual(service.celebrationCount, 1)
        XCTAssertEqual(service.celebration?.id, "fresh")

        await service.refresh()
        XCTAssertEqual(service.celebrationCount, 1)

        server.rows.value = [
            ["id": "plain", "kind": "mention", "title": "Mention", "ts": .int(now)],
        ]
        await service.refresh()
        XCTAssertEqual(service.celebrationCount, 1)
    }

    func testTheInboxIsReadOnStartAndOnResumeNeverOnATimer() async {
        let server = PingsServer()
        server.ids.value = []
        server.unread.value = 0
        let service = make(server, node: { "alice" })

        // Resuming before the shell started the service reads nothing.
        service.appResumed()
        await svcSettle()
        XCTAssertEqual(server.reads.value, 0)

        service.start()
        let firstRead = await svcEventually { server.reads.value == 1 && !service.loading }
        XCTAssertTrue(firstRead)

        // Time passing costs nothing: there is no inbox timer to fire.
        try? await Task.sleep(nanoseconds: 120_000_000)
        XCTAssertEqual(server.reads.value, 1)

        // Coming back catches up exactly once.
        service.appResumed()
        let resumed = await svcEventually { server.reads.value == 2 && !service.loading }
        XCTAssertTrue(resumed)
        await svcSettle()
        XCTAssertEqual(server.reads.value, 2)

        // Catch-up reads asked for in the same moment (resume + the event channel's connect)
        // share one request.
        let gate = SvcGate()
        server.gate.value = gate
        let first = Task { await service.refresh(silent: true, catchUp: true) }
        let heldFirst = await svcEventually { gate.arrived == 1 }
        XCTAssertTrue(heldFirst)
        let second = Task { await service.refresh(silent: true, catchUp: true) }
        await svcSettle()
        gate.release()
        await first.value
        await second.value
        XCTAssertEqual(server.reads.value, 3)

        // A pushed event that lands mid-read is not folded into it: the read in flight may
        // predate what the event announces, so one more read follows.
        let gate2 = SvcGate()
        server.gate.value = gate2
        let running = Task { await service.refresh(silent: true, catchUp: true) }
        let heldRunning = await svcEventually { gate2.arrived == 1 }
        XCTAssertTrue(heldRunning)
        let pushed = Task { await service.refresh(silent: true) }
        let pushedAgain = Task { await service.refresh(silent: true) }
        await svcSettle()
        gate2.release()
        await running.value
        await pushed.value
        await pushedAgain.value
        XCTAssertEqual(server.reads.value, 5)
    }

    func testServiceHidesStaleNotificationsImmediatelyWhenNodeChanges() async {
        let server = PingsServer()
        server.ids.value = ["n"]
        let node = SvcBox("alice")
        let service = make(server, node: { node.value })

        await service.refresh()
        XCTAssertEqual(service.unreadCount, 1)
        XCTAssertEqual(service.notifications.map(\.title), ["Notification for alice"])

        node.value = "bob"

        XCTAssertEqual(service.unreadCount, 0)
        // Alice's count says nothing about Bob's badge.
        XCTAssertFalse(service.unreadCountKnown)
        XCTAssertTrue(service.notifications.isEmpty)

        await service.refresh()
        XCTAssertEqual(service.unreadCount, 1)
        XCTAssertEqual(service.notifications.map(\.title), ["Notification for bob"])
        XCTAssertEqual(server.readNodes.value, ["alice", "bob"])
    }

    func testDeleteAllEmptiesTheInboxOnceTheWorkerAccepts() async throws {
        let server = PingsServer()
        deleteAllPage(server)
        let service = make(server, node: { "Alice" })

        // Nothing loaded yet: no inbox to clear, so no request either.
        try await service.deleteAll()
        XCTAssertTrue(server.deleted.value.isEmpty)
        // Nor is there a count yet that the app icon badge could follow.
        XCTAssertFalse(service.unreadCountKnown)

        await service.refresh()
        XCTAssertTrue(service.unreadCountKnown)
        XCTAssertEqual(service.notifications.count, 2)
        // The page is capped; the inbox total is what the whole-inbox action quotes.
        XCTAssertEqual(service.inboxTotal, 9)

        try await service.deleteAll()
        XCTAssertEqual(server.deleted.value, [["node": "alice", "all": true]])
        XCTAssertTrue(service.notifications.isEmpty)
        XCTAssertEqual(service.unreadCount, 0)
        // Still a known count, so the app icon badge drops to zero with it.
        XCTAssertTrue(service.unreadCountKnown)
        XCTAssertEqual(service.inboxTotal, 0)
    }

    func testARefusedDeleteAllKeepsTheLoadedInbox() async {
        let server = PingsServer()
        deleteAllPage(server)
        server.refuseDeletes.value = 503
        let service = make(server, node: { "alice" })

        await service.refresh()
        let error = await svcThrown { try await service.deleteAll() }
        XCTAssertTrue(error is ApiException, "\(String(describing: error))")

        XCTAssertEqual(server.deleted.value.map { $0.str("node") }, ["alice"])
        XCTAssertEqual(service.notifications.count, 2)
        XCTAssertEqual(service.unreadCount, 1)
        XCTAssertEqual(service.inboxTotal, 9)
    }

    // MARK: NotificationInboxTest.kt

    func testASilentRefreshDoesNotFlashTheSpinnerOverALoadedList() async {
        let server = PingsServer()
        let service = make(server, node: { "Alice" })
        let loadingSeen = SvcBox<[Bool]>([])
        service.addListener {
            let loading = MainActor.assumeIsolated { service.loading }
            loadingSeen.mutate { $0.append(loading) }
        }

        // Nothing on screen yet: even a silent read shows the spinner.
        await service.refresh(silent: true)
        XCTAssertTrue(loadingSeen.value.contains(true))
        loadingSeen.value = []

        await service.refresh(silent: true)
        XCTAssertFalse(loadingSeen.value.contains(true), "\(loadingSeen.value)")
        loadingSeen.value = []
        await service.refresh()
        XCTAssertEqual(loadingSeen.value.first, true)
        XCTAssertFalse(service.loading)
    }

    func testErrorsAreKeptAsTheirDartStringAndClearedOnSuccess() async {
        let server = PingsServer()
        let service = make(server, node: { "alice" })
        server.failReads.value = 503
        await service.refresh()
        XCTAssertEqual(service.error, "Exception: HTTP 503 for /api/notifications")
        XCTAssertTrue(service.hasLoaded)
        server.failReads.value = nil
        await service.refresh(silent: true)
        XCTAssertEqual(service.error, "")
        XCTAssertEqual(service.unreadCount, 2)
    }

    func testNoAccountMeansNoRead() async throws {
        let server = PingsServer()
        server.failReads.value = 500
        let service = make(server, node: { "  " })
        await service.refresh()
        XCTAssertEqual(service.error, "")
        XCTAssertFalse(service.hasLoaded)
        try await service.markAllRead()
        XCTAssertTrue(server.http.requests.isEmpty)
    }

    func testDeleteAllEmptiesTheInboxOnlyAfterTheWorkerAccepts() async throws {
        let server = PingsServer()
        server.total.value = 137
        let service = make(server, node: { "Alice" })

        // Nothing loaded: no inbox to clear, so no request either.
        try await service.deleteAll()
        XCTAssertTrue(server.deleted.value.isEmpty)

        await service.refresh()
        XCTAssertEqual(service.notifications.count, 2)
        // The page is capped; the whole-inbox count is what a clear quotes.
        XCTAssertEqual(service.inboxTotal, 137)

        server.refuseDeletes.value = 503
        let refused = await svcThrown { try await service.deleteAll() }
        XCTAssertNotNil(refused)
        XCTAssertEqual(server.deleted.value.count, 1)
        XCTAssertEqual(service.notifications.count, 2, "a refused clear keeps the list")
        XCTAssertEqual(service.unreadCount, 2)
        XCTAssertEqual(service.inboxTotal, 137)

        server.refuseDeletes.value = nil
        try await service.deleteAll()
        XCTAssertEqual(server.deleted.value.map { $0.str("node") }, ["alice", "alice"])
        XCTAssertEqual(service.notifications.count, 0)
        XCTAssertEqual(service.unreadCount, 0)
        XCTAssertEqual(service.inboxTotal, 0)

        // Already empty: nothing left to ask the Worker for.
        try await service.deleteAll()
        XCTAssertEqual(server.deleted.value.count, 2)
    }

    func testAnAccountSwitchHidesTheInboxTotalToo() async {
        let server = PingsServer()
        server.total.value = 9
        let node = SvcBox("alice")
        let service = make(server, node: { node.value })
        await service.refresh()
        XCTAssertEqual(service.inboxTotal, 9)
        node.value = "bob"
        XCTAssertEqual(service.inboxTotal, 0)
    }

    func testMarkReadAndMarkAllRead() async throws {
        let server = PingsServer()
        let service = make(server, node: { "alice" }, clock: FixedClock(epochMillis: 42))
        await service.refresh()
        let first = service.notifications[0]
        try await service.markRead(first)
        XCTAssertEqual(server.marked.value, [["node": "alice", "ids": ["a-alice"]]])
        XCTAssertEqual(service.unreadCount, 1)
        XCTAssertEqual(service.notifications[0].readAt, 42)

        // Already read, or without an id: no request.
        try await service.markRead(service.notifications[0])
        try await service.markRead(ForkNotification(id: "", kind: "mention", title: "x"))
        XCTAssertEqual(server.marked.value.count, 1)

        try await service.markAllRead()
        XCTAssertEqual(server.marked.value.last, ["node": "alice", "all": true])
        XCTAssertEqual(service.unreadCount, 0)
        XCTAssertFalse(service.notifications.contains(where: \.isUnread))
    }

    // MARK: delete

    func testASwipedRowLeavesAtOnceAndComesBackWhenRefused() async throws {
        let server = PingsServer()
        server.total.value = 5
        let service = make(server, node: { "alice" })
        await service.refresh()

        try await service.delete(service.notifications[0])
        XCTAssertEqual(server.deleted.value, [["node": "alice", "id": "a-alice"]])
        XCTAssertEqual(service.notifications.map(\.id), ["b-alice"])
        XCTAssertEqual(service.unreadCount, 1)
        XCTAssertEqual(service.inboxTotal, 4)

        server.refuseDeletes.value = 503
        let error = await svcThrown { try await service.delete(service.notifications[0]) }
        XCTAssertTrue(error is ApiException, "\(String(describing: error))")
        XCTAssertEqual(service.notifications.map(\.id), ["b-alice"])
        XCTAssertEqual(service.unreadCount, 1)
        XCTAssertEqual(service.inboxTotal, 4)
    }
}
