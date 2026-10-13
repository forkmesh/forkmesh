// The account event channel, with the cases of native/android
// core/src/test/.../api/AccountEventLoopTest.kt (backoff, keepalive, lifecycle, restart,
// dispose). The ticket endpoint is a scripted `AccountEventTicketSource` whose URL comes from a
// real ApiService; FMNet's
// FakeWebSocketConnector plays the relay; `SvcSleeper` stands in for the reconnect and keepalive
// timers, so every wait is released by the test rather than timed.
//
// Kotlin's `setForeground(false)` pauses the socket; this port ignores every state
// but `resumed`, so the background test keeps only the resume half.

import FMAccount
import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

/// `TicketApiService`: the ticket status is scripted, the URL is ApiService's.
@MainActor
private final class TicketSource: AccountEventTicketSource {
    var status: EventTicketStatus
    var ticketCalls = 0
    var holding = false
    private var held: [CheckedContinuation<Void, Never>] = []
    let api: ApiService

    init(_ status: EventTicketStatus, serverUrl: String = "wss://app.forkmesh.com/ws") {
        self.status = status
        api = svcApi(FakeHTTPClient(), token: { "session-token" }, serverUrl: serverUrl)
    }

    func accountEventTicket() async -> EventTicket {
        ticketCalls += 1
        if holding { await withCheckedContinuation { held.append($0) } }
        return status == .ok ? EventTicket(.ok, "ticket-abc") : EventTicket(status)
    }

    func accountEventsURL(_ ticket: String, node: String) -> URL { api.accountEventsURL(ticket, node: node) }

    func release() {
        holding = false
        let waiting = held
        held = []
        for continuation in waiting { continuation.resume() }
    }
}

@MainActor
final class AccountEventChannelTests: XCTestCase {
    private let ping = "{\"type\":\"ping\"}"
    private var channels: [AccountEventChannel] = []

    override func tearDown() async throws {
        for channel in channels { await channel.dispose() }
        channels = []
        try await super.tearDown()
    }

    private func make(
        _ source: AccountEventTicketSource, _ ws: FakeWebSocketConnector, _ sleeper: SvcSleeper,
        token: @escaping @MainActor () -> String = { "session-token" }, keepalive: TimeInterval = 240, nodeName: String = ""
    ) -> AccountEventChannel {
        let channel = AccountEventChannel(
            api: source, sessionToken: token, connector: ws, keepalive: keepalive, nodeName: nodeName, sleep: sleeper.sleep)
        channels.append(channel)
        return channel
    }

    // MARK: account event channel

    func testAnEventFrameForTheInboxReachesOnTopic() async {
        let ws = FakeWebSocketConnector()
        let channel = make(TicketSource(.ok), ws, SvcSleeper())
        var topics: [String] = []
        var frames: [JSONObject] = []
        var connects = 0
        channel.onTopic = { topics.append($0) }
        channel.onFrame = { frames.append($0) }
        channel.onConnected = { connects += 1 }

        channel.start()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        // Every connect fires one catch-up read; there is no poll behind this socket, so anything
        // raised while it was down would be lost otherwise.
        XCTAssertEqual(connects, 1)

        let server = ws.lastConnection!
        // The relay's capability handshake carries no topic and must not reach onTopic.
        server.receive(json: ["type": "hello", "accepts": ["agent-status"]])
        server.receive(json: ["type": "event", "topic": "pings"])
        server.receive(json: ["type": "event", "topic": "issues"])
        // Kotlin: an empty topic, a frame that is not JSON and a binary frame.
        server.receive(json: ["type": "event", "topic": ""])
        server.receive(text: "not json")
        server.receive(.data(Data(JSON.encode(["type": "event", "topic": "direct-messages"] as JSONValue).utf8)))
        let delivered = await svcEventually { topics.count == 3 }
        XCTAssertTrue(delivered)
        await svcSettle()

        XCTAssertEqual(topics, ["pings", "issues", "direct-messages"])
        XCTAssertEqual(frames, [["type": "hello", "accepts": ["agent-status"]]])
    }

    func testTheSocketCarriesTheTicketNotTheSessionToken() async {
        let ws = FakeWebSocketConnector()
        let channel = make(TicketSource(.ok), ws, SvcSleeper())

        channel.start()
        let opened = await ws.waitForConnections(1)
        XCTAssertTrue(opened)

        let url = ws.attempts[0].url
        XCTAssertEqual(url.path, "/api/nodes/events")
        XCTAssertEqual(URLComponents(url: url, resolvingAgainstBaseURL: false)?.percentEncodedQuery, "ticket=ticket-abc")
        XCTAssertTrue(url.scheme == "ws" || url.scheme == "wss", url.absoluteString)
        XCTAssertFalse(url.absoluteString.contains("session-token"))
        XCTAssertNil(ws.attempts[0].headers["Authorization"])
    }

    func testANamedNodeJoinsThatMachinesObject() async {
        let ws = FakeWebSocketConnector()
        let channel = make(TicketSource(.ok), ws, SvcSleeper(), nodeName: "mirror-1")
        channel.start()
        let opened = await ws.waitForConnections(1)
        XCTAssertTrue(opened)
        XCTAssertEqual(
            URLComponents(url: ws.attempts[0].url, resolvingAgainstBaseURL: false)?.percentEncodedQuery,
            "ticket=ticket-abc&node=mirror-1")
    }

    func testASessionThatProvesNobodyGoesDarkInsteadOfPolling() async {
        let source = TicketSource(.signedOut)
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let channel = make(source, ws, sleeper)
        var signedOut = 0
        channel.onSignedOut = { signedOut += 1 }

        channel.start()
        let dark = await svcEventually { signedOut == 1 }
        XCTAssertTrue(dark)
        XCTAssertFalse(channel.connected)
        XCTAssertFalse(channel.started)

        // Retrying a session the relay already rejected would just be a poll of the ticket
        // endpoint.
        try? await Task.sleep(nanoseconds: 50_000_000)
        sleeper.fire()
        await svcSettle()
        XCTAssertEqual(source.ticketCalls, 1)
        XCTAssertTrue(ws.attempts.isEmpty)
        XCTAssertTrue(sleeper.requested.isEmpty)
    }

    func testATransientTicketFailureKeepsTheChannelArmedAndBacksOff() async {
        let source = TicketSource(.unavailable)
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let channel = make(source, ws, sleeper)

        channel.start()
        let armed = await svcEventually { sleeper.pending == [2] }
        XCTAssertTrue(armed, "\(sleeper.pending)")

        // Still started, so the bounded backoff will try again: going dark on a relay hiccup
        // would silently turn the app back into a poll-only client.
        XCTAssertTrue(channel.started)
        XCTAssertFalse(channel.connected)
        XCTAssertTrue(ws.attempts.isEmpty)

        // 2 s doubling to the 30 s cap.
        for expected: TimeInterval in [4, 8, 16, 30, 30] {
            sleeper.fire()
            let next = await svcEventually { sleeper.pending == [expected] }
            XCTAssertTrue(next, "waiting for \(expected), pending \(sleeper.pending)")
        }
        XCTAssertEqual(sleeper.requested, [2, 4, 8, 16, 30, 30])
        XCTAssertEqual(source.ticketCalls, 6)

        source.status = .ok
        sleeper.fire(30)
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        XCTAssertEqual(ws.attempts.count, 1)

        // A connection resets the backoff.
        ws.lastConnection!.serverClose()
        let rearmed = await svcEventually { !channel.connected && sleeper.pending.contains(2) }
        XCTAssertTrue(rearmed, "\(sleeper.pending)")
    }

    func testStopClosesTheSocketAndStopsDeliveringTopics() async {
        let ws = FakeWebSocketConnector()
        let channel = make(TicketSource(.ok), ws, SvcSleeper())
        var topics: [String] = []
        channel.onTopic = { topics.append($0) }

        channel.start()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)

        await channel.stop()
        XCTAssertFalse(channel.connected)
        XCTAssertFalse(channel.started)

        let server = ws.lastConnection!
        server.receive(json: ["type": "event", "topic": "pings"])
        await svcSettle()
        XCTAssertTrue(topics.isEmpty)
        XCTAssertNotNil(server.closedByClient)
    }

    // MARK: AccountEventLoopTest.kt

    func testAServerCloseReconnectsWithACatchUp() async {
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let channel = make(TicketSource(.ok), ws, sleeper)
        var connects = 0
        channel.onConnected = { connects += 1 }

        channel.start()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        ws.connections[0].serverClose()
        let backoff = await svcEventually { !channel.connected && sleeper.pending.contains(2) }
        XCTAssertTrue(backoff)
        sleeper.fire(2)
        let back = await svcEventually { connects == 2 && channel.connected }
        XCTAssertTrue(back)
        XCTAssertEqual(ws.connections.count, 2)

        // A connect failure also goes back to the backoff.
        ws.refuseConnections(WebSocketError.transport("refused"))
        ws.connections[1].serverClose()
        let closed = await svcEventually { sleeper.pending.contains(2) }
        XCTAssertTrue(closed)
        sleeper.fire(2)
        let failed = await svcEventually { ws.attempts.count == 3 && sleeper.pending.contains(4) }
        XCTAssertTrue(failed, "\(sleeper.pending)")
        ws.refuseConnections(nil)
        sleeper.fire(4)
        let again = await svcEventually { channel.connected }
        XCTAssertTrue(again)
        XCTAssertEqual(ws.attempts.count, 4)
        XCTAssertEqual(connects, 3)
    }

    func testABrokenSocketReconnectsToo() async {
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let channel = make(TicketSource(.ok), ws, sleeper)
        channel.start()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        ws.connections[0].fail()
        let backoff = await svcEventually { !channel.connected && sleeper.pending.contains(2) }
        XCTAssertTrue(backoff)
        sleeper.fire(2)
        let back = await svcEventually { channel.connected && ws.connections.count == 2 }
        XCTAssertTrue(back)
    }

    func testKeepalivePingsAndAFailedWriteReconnects() async {
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let channel = make(TicketSource(.ok), ws, sleeper, keepalive: 20)

        channel.start()
        let armed = await svcEventually { channel.connected && sleeper.pending == [20] }
        XCTAssertTrue(armed)
        let first = ws.connections[0]
        XCTAssertTrue(first.sent.isEmpty)
        sleeper.fire(20)
        let once = await first.waitForSent(1)
        XCTAssertTrue(once)
        let rearmed = await svcEventually { sleeper.pending == [20] }
        XCTAssertTrue(rearmed)
        sleeper.fire(20)
        let twice = await first.waitForSent(2)
        XCTAssertTrue(twice)
        XCTAssertEqual(first.sentTexts, [ping, ping])

        first.failSends(with: WebSocketError.transport("broken pipe"))
        let rearmedAgain = await svcEventually { sleeper.pending == [20] }
        XCTAssertTrue(rearmedAgain)
        sleeper.fire(20)
        let backoff = await svcEventually { !channel.connected && sleeper.pending.contains(2) }
        XCTAssertTrue(backoff, "\(sleeper.pending)")
        let closed = await first.waitUntilClosed()
        XCTAssertTrue(closed)
        sleeper.fire(2)
        let back = await svcEventually { ws.connections.count == 2 && channel.connected }
        XCTAssertTrue(back)
    }

    func testSendWritesAFrameOnlyWhileASocketIsOpen() async {
        let ws = FakeWebSocketConnector()
        let channel = make(TicketSource(.ok), ws, SvcSleeper())
        XCTAssertFalse(channel.send(["type": "terminal", "data": "ls"]))
        channel.start()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        XCTAssertTrue(channel.send(["type": "terminal", "data": "ls"]))
        let sent = await ws.connections[0].waitForSent(1)
        XCTAssertTrue(sent)
        XCTAssertEqual(ws.connections[0].sentTexts, ["{\"type\":\"terminal\",\"data\":\"ls\"}"])
    }

    func testForegroundReconnectsAtOnceAndProbesALiveSocket() async {
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let source = TicketSource(.ok)
        let channel = make(source, ws, sleeper)

        // Not started: a resume does nothing.
        channel.appLifecycleChanged(.resumed)
        await svcSettle()
        XCTAssertTrue(ws.attempts.isEmpty)

        channel.start()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        // iOS killed the socket while the app was away; the backoff is frozen.
        ws.connections[0].serverClose()
        let backoff = await svcEventually { !channel.connected && sleeper.pending.contains(2) }
        XCTAssertTrue(backoff)
        // Only `resumed` counts.
        channel.appLifecycleChanged(.paused)
        channel.appLifecycleChanged(.inactive)
        await svcSettle()
        XCTAssertEqual(ws.attempts.count, 1)

        // Coming back does not serve out the backoff.
        channel.appLifecycleChanged(.resumed)
        let back = await svcEventually { channel.connected }
        XCTAssertTrue(back)
        XCTAssertEqual(ws.attempts.count, 2)
        XCTAssertFalse(sleeper.pending.contains(2), "the frozen backoff is cancelled")

        // Resume with a live socket only sends a keepalive.
        channel.appLifecycleChanged(.resumed)
        let probed = await ws.connections[1].waitForSent(1)
        XCTAssertTrue(probed)
        await svcSettle()
        XCTAssertEqual(ws.attempts.count, 2)
        XCTAssertEqual(ws.connections[1].sentTexts, [ping])
    }

    func testAResumeWhileTheTicketIsPendingDoesNotAskForASecondOne() async {
        let ws = FakeWebSocketConnector()
        let source = TicketSource(.ok)
        source.holding = true
        let channel = make(source, ws, SvcSleeper())

        channel.start()
        let asked = await svcEventually { source.ticketCalls == 1 }
        XCTAssertTrue(asked)
        channel.appLifecycleChanged(.resumed)
        channel.appLifecycleChanged(.resumed)
        await svcSettle()
        XCTAssertEqual(source.ticketCalls, 1)

        source.release()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        await svcSettle()
        XCTAssertEqual(source.ticketCalls, 1)
        XCTAssertEqual(ws.attempts.count, 1)
    }

    func testARestartWhileTheTicketIsPendingStillOpensUnderTheNewSession() async {
        let ws = FakeWebSocketConnector()
        let source = TicketSource(.ok)
        source.holding = true
        let channel = make(source, ws, SvcSleeper())

        channel.start()
        _ = await svcEventually { source.ticketCalls == 1 }
        await channel.restart()
        let again = await svcEventually { source.ticketCalls == 2 }
        XCTAssertTrue(again)

        source.release()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        await svcSettle()
        XCTAssertEqual(ws.attempts.count, 1)
    }

    func testResumeResetsTheBackoff() async {
        let source = TicketSource(.unavailable)
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let channel = make(source, ws, sleeper)
        channel.start()
        _ = await svcEventually { sleeper.pending == [2] }
        sleeper.fire()
        let grown = await svcEventually { sleeper.pending == [4] }
        XCTAssertTrue(grown)

        channel.appLifecycleChanged(.resumed)
        let reset = await svcEventually { sleeper.pending == [2] }
        XCTAssertTrue(reset, "\(sleeper.pending)")
        XCTAssertEqual(source.ticketCalls, 3)
    }

    func testRestartFollowsTheSession() async {
        let ws = FakeWebSocketConnector()
        let source = TicketSource(.ok)
        let token = SvcBox("")
        let channel = make(source, ws, SvcSleeper(), token: { token.value })

        // Signed out: starting stays dark without asking for a ticket.
        channel.start()
        let dark = await svcEventually { !channel.started }
        XCTAssertTrue(dark)
        XCTAssertEqual(source.ticketCalls, 0)

        token.value = "session-token"
        await channel.restart()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)

        // Account switch: a fresh ticket and socket.
        await channel.restart()
        let again = await svcEventually { channel.connected && ws.connections.count == 2 }
        XCTAssertTrue(again)
        let closed = await ws.connections[0].waitUntilClosed()
        XCTAssertTrue(closed)
        XCTAssertEqual(source.ticketCalls, 2)

        token.value = ""
        await channel.stop()
        await channel.restart()
        await svcSettle()
        XCTAssertFalse(channel.started)
        XCTAssertEqual(ws.connections.count, 2)
    }

    func testAFrameArrivingDuringTheConnectIsNotLost() async {
        let ws = FakeWebSocketConnector()
        ws.onConnect { connection in connection.receive(json: ["type": "event", "topic": "pings"]) }
        let channel = make(TicketSource(.ok), ws, SvcSleeper())
        var topics: [String] = []
        channel.onTopic = { topics.append($0) }

        channel.start()
        let delivered = await svcEventually { topics == ["pings"] }
        XCTAssertTrue(delivered, "\(topics)")
    }

    func testDisposeIsFinal() async {
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let channel = make(TicketSource(.ok), ws, sleeper)
        channel.start()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        await channel.dispose()
        XCTAssertFalse(channel.connected)
        XCTAssertTrue(ws.connections[0].closedByClient != nil)
        channel.start()
        await channel.restart()
        channel.appLifecycleChanged(.resumed)
        try? await Task.sleep(nanoseconds: 20_000_000)
        sleeper.fire()
        await svcSettle()
        XCTAssertEqual(ws.attempts.count, 1)
        XCTAssertFalse(channel.started)
    }

    // MARK: Over ApiService's own ticket endpoint

    func testTheTicketComesFromTheAccountEndpointWithTheSession() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/accounts/event-ticket", json: ["ticket": "t-123"])
        let api = svcApi(http, token: { "session-token" })
        let ws = FakeWebSocketConnector()
        let channel = make(api, ws, SvcSleeper())
        channel.start()
        let up = await svcEventually { channel.connected }
        XCTAssertTrue(up)
        XCTAssertEqual(http.requests.first?.headers["Authorization"], "Bearer session-token")
        XCTAssertEqual(ws.attempts[0].url.absoluteString, "wss://app.forkmesh.com/api/nodes/events?ticket=t-123")
    }

    func testARejectedSessionAtTheTicketEndpointSignsTheChannelOut() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/accounts/event-ticket", status: 401, json: ["error": "unauthorized"])
        let api = svcApi(http, token: { "session-token" })
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let channel = make(api, ws, sleeper)
        var signedOut = 0
        channel.onSignedOut = { signedOut += 1 }
        channel.start()
        let dark = await svcEventually { signedOut == 1 }
        XCTAssertTrue(dark)
        XCTAssertFalse(channel.started)
        XCTAssertTrue(ws.attempts.isEmpty)
        XCTAssertTrue(sleeper.requested.isEmpty)
    }

    func testAnUnreachableTicketEndpointBacksOff() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/accounts/event-ticket", status: 503, json: ["error": "busy"])
        let api = svcApi(http, token: { "session-token" })
        let ws = FakeWebSocketConnector()
        let sleeper = SvcSleeper()
        let channel = make(api, ws, sleeper)
        channel.start()
        let armed = await svcEventually { sleeper.pending == [2] }
        XCTAssertTrue(armed)
        XCTAssertTrue(channel.started)
        XCTAssertTrue(ws.attempts.isEmpty)
    }
}
