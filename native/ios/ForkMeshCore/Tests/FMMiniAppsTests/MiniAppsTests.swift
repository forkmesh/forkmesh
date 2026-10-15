import FMBase
import FMChat
import FMControl
@testable import FMMiniApps
import XCTest

@MainActor
final class FakeLink: DesktopControlLink {
    var transport: ControlTransport = .lan
    var isOpen = true
    var desktopName = "desk"
    var calls: [(String, JSONObject)] = []
    var replies: [String: Result<JSONObject, Error>] = [:]
    var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        calls.append((method, params))
        switch replies[method] {
        case .success(let value): return value
        case .failure(let error): throw error
        case nil: return [:]
        }
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps[id] = nil }

    func fire(_ event: String, _ data: JSONObject) {
        for tap in taps.values { tap(event, data) }
    }
}

let weather = MiniAppDescriptor(id: "weather", name: "Weather", methods: ["card", "refresh", "wipe"], events: ["changed"])

final class MiniAppContractTests: XCTestCase {
    func testNamesRoundTrip() {
        XCTAssertEqual(MiniAppContract.method("weather", "card"), "plugin.weather.card")
        XCTAssertEqual(MiniAppContract.split(event: "plugin.weather.changed")?.plugin, "weather")
        XCTAssertNil(MiniAppContract.split(event: "core.daemon.changed"))
        XCTAssertNil(MiniAppContract.split(event: "plugin..changed"))
        XCTAssertTrue(MiniAppContract.isPermanent("not_owner"))
        XCTAssertFalse(MiniAppContract.isPermanent("desktop_unavailable"))
    }

    func testCatalogParsesAndSorts() {
        let result: JSONObject = ["plugins": [
            ["id": "zeta", "name": "Zeta", "methods": ["card"], "running": true],
            ["id": "alpha", "name": "alpha", "methods": ["ping"]],
            ["name": "no id"],
        ]]
        let catalog = MiniAppDescriptor.catalog(result)
        XCTAssertEqual(catalog.map(\.id), ["alpha", "zeta"])
        XCTAssertTrue(catalog[1].hasCard)
        XCTAssertTrue(catalog[1].running)
        XCTAssertFalse(catalog[0].hasCard)
    }
}

final class MiniAppCardTests: XCTestCase {
    func testActionsAreLimitedToRemoteMethods() {
        let object: JSONObject = [
            "title": "Forecast",
            "rows": [["label": "Today", "value": "Sunny", "tone": "good"], ["label": "Odd", "tone": "purple"]],
            "actions": [
                ["title": "Refresh", "method": "refresh"],
                ["title": "Install", "method": "install"],
                ["title": "Loop", "method": "card"],
                ["title": "Wipe", "method": "wipe", "destructive": true, "confirm": "Sure?"],
            ],
            "refreshSeconds": 5,
        ]
        let card = MiniAppCard(object, for: weather)
        XCTAssertEqual(card.title, "Forecast")
        XCTAssertEqual(card.rows.map(\.tone), [.good, .neutral])
        XCTAssertEqual(card.actions.map(\.method), ["refresh", "wipe"])
        XCTAssertTrue(card.actions[1].destructive)
        XCTAssertEqual(card.refreshSeconds, MiniAppCard.minRefreshSeconds)
        XCTAssertEqual(MiniAppCard(card.json, for: weather), card)
    }

    func testBoundsHostileCards() {
        let rows = (0..<500).map { _ in JSONValue.object(["label": .string(String(repeating: "x", count: 500))]) }
        let card = MiniAppCard(["rows": .array(rows)], for: weather)
        XCTAssertEqual(card.rows.count, MiniAppCard.maxRows)
        XCTAssertEqual(card.rows[0].label.count, 80)
        XCTAssertEqual(card.title, "Weather")
    }
}

final class MiniAppOfflineTests: XCTestCase {
    func testOutboxPersistsAndFlushesInOrder() async {
        let store = InMemoryKeyValueStore()
        let outbox = MiniAppOutbox(store: store)
        XCTAssertTrue(outbox.enqueue(MiniAppIntent(id: "a", plugin: "weather", method: "refresh", createdMs: 1000)))
        XCTAssertTrue(outbox.enqueue(MiniAppIntent(id: "b", plugin: "weather", method: "wipe", createdMs: 1000)))
        XCTAssertFalse(outbox.enqueue(MiniAppIntent(id: "a", plugin: "weather", method: "refresh", createdMs: 1000)))
        XCTAssertEqual(MiniAppOutbox(store: store).pending.map(\.id), ["a", "b"])

        var sent: [String] = []
        let report = await outbox.flush(nowMs: 2000) { method, _ in
            sent.append(method)
            if method.hasSuffix("wipe") { throw LocalControlException("not_owner") }
            return [:]
        }
        XCTAssertEqual(sent, ["plugin.weather.refresh", "plugin.weather.wipe"])
        XCTAssertEqual(report.sent, ["a"])
        XCTAssertEqual(report.dropped["b"], "not_owner")
        XCTAssertTrue(outbox.pending.isEmpty)
    }

    func testTransientFailureKeepsOrderAndExpiryDrops() async {
        let outbox = MiniAppOutbox(store: InMemoryKeyValueStore())
        outbox.enqueue(MiniAppIntent(id: "old", plugin: "p", method: "m", createdMs: 0, ttlMs: 10))
        outbox.enqueue(MiniAppIntent(id: "first", plugin: "p", method: "m", createdMs: 100))
        outbox.enqueue(MiniAppIntent(id: "second", plugin: "p", method: "m", createdMs: 100))
        var calls = 0
        let report = await outbox.flush(nowMs: 1000) { _, _ in
            calls += 1
            throw LocalControlException("desktop_unavailable")
        }
        XCTAssertEqual(calls, 1)
        XCTAssertEqual(report.dropped["old"], "expired")
        XCTAssertEqual(report.kept, ["first", "second"])
        XCTAssertEqual(outbox.pending.first?.attempts, 1)
        for _ in 0..<MiniAppOutbox.maxAttempts {
            _ = await outbox.flush(nowMs: 1000) { _, _ in throw MiniAppError("timeout") }
        }
        XCTAssertFalse(outbox.pending.contains { $0.id == "first" })
    }

    func testBackgroundPlanRespectsIOSMinimum() {
        let now = Date(timeIntervalSince1970: 0)
        XCTAssertNil(MiniAppBackgroundPlan.nextRefresh(cards: [MiniAppCard(title: "x")], pendingIntents: 0, now: now))
        let fast = MiniAppCard(title: "x", refreshSeconds: 60)
        XCTAssertEqual(MiniAppBackgroundPlan.nextRefresh(cards: [fast], pendingIntents: 0, now: now),
                       now.addingTimeInterval(TimeInterval(MiniAppBackgroundPlan.minimumIntervalSeconds)))
        XCTAssertNotNil(MiniAppBackgroundPlan.nextRefresh(cards: [], pendingIntents: 2, now: now))
        XCTAssertTrue(MiniAppBackgroundPlan.stale(fetchedMs: 0, card: fast, nowMs: 61_000))
        XCTAssertFalse(MiniAppBackgroundPlan.stale(fetchedMs: 0, card: fast, nowMs: 59_000))
    }
}

@MainActor
final class MiniAppClientTests: XCTestCase {
    func testLiveCallsQueueWhenAwayAndFlushOnReturn() async throws {
        let link = FakeLink()
        let store = InMemoryKeyValueStore()
        let client = MiniAppClient(store: store, link: { link }, now: { Date(timeIntervalSince1970: 100) })
        link.replies["plugins.list"] = .success(["plugins": [["id": "weather", "name": "Weather", "methods": ["card", "refresh"], "events": ["changed"]]]])
        link.replies["plugin.weather.card"] = .success(["title": "Now", "actions": [["title": "Refresh", "method": "refresh"]]])
        let app = try await client.catalog()[0]
        let card = try await client.card(for: app)
        XCTAssertEqual(card.actions.count, 1)
        XCTAssertEqual(client.cachedCard(for: app)?.card, card)
        XCTAssertEqual(client.reach, .live(transport: "lan"))

        let live = try await client.perform(card.actions[0], on: app)
        XCTAssertEqual(live, .done([:]))
        link.isOpen = false
        XCTAssertEqual(client.reach, .desktopAway(lastSeenMs: nil))
        guard case .queued = try await client.perform(card.actions[0], on: app) else { return XCTFail("not queued") }
        XCTAssertEqual(client.outbox.pending.count, 1)
        link.isOpen = true
        let report = await client.flushOutbox()
        XCTAssertEqual(report.sent.count, 1)
        XCTAssertEqual(link.calls.last?.0, "plugin.weather.refresh")
    }

    func testObserveFiltersOtherPluginsAndUndeclaredEvents() {
        let link = FakeLink()
        let client = MiniAppClient(store: InMemoryKeyValueStore(), link: { link })
        var seen: [String] = []
        let token = client.observe(weather) { name, _ in seen.append(name) }
        link.fire("plugin.weather.changed", [:])
        link.fire("plugin.weather.secret", [:])
        link.fire("plugin.other.changed", [:])
        XCTAssertEqual(seen, ["changed"])
        client.stopObserving(token!)
        XCTAssertTrue(link.taps.isEmpty)
    }

    func testUnpairedNeverQueuesWhenAsked() async {
        let client = MiniAppClient(store: InMemoryKeyValueStore(), link: { nil })
        XCTAssertEqual(client.reach, .unpaired)
        let action = MiniAppCard.Action(id: "r", title: "R", method: "refresh", params: [:], destructive: false, confirm: "")
        do {
            _ = try await client.perform(action, on: weather, queueWhenAway: false)
            XCTFail("expected failure")
        } catch {
            XCTAssertEqual(MiniAppOutbox.code(of: error), "desktop_unavailable")
        }
    }
}
