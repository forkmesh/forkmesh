// The Hosts area: the desktop's host rows, the two-step confirmed changes, the add-host form's
// choices, and the live log viewer's buffer and stream.

import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMHosts

@MainActor
final class FakeHostsChannel: HostsChannel {
    var reach: DesktopReach = .connected(.relay, "desk")
    var calls: [(String, JSONObject)] = []
    var answers: [String: JSONObject] = [:]
    var failures: [String: String] = [:]
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        calls.append((method, params))
        if let code = failures[method] { throw LocalControlException(code) }
        return answers[method] ?? [:]
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    var tapCount: Int { taps.count }

    func emit(_ name: String, _ data: JSONObject = [:]) {
        for tap in taps.values { tap(name, data) }
    }

    func calls(_ method: String) -> [JSONObject] { calls.filter { $0.0 == method }.map(\.1) }
}

private let listAnswer: JSONObject = [
    "hosts": [
        [
            "name": "mirror35", "address": "203.0.113.5", "user": "root", "status": "installed", "provider": "Vultr",
            "region": "ewr", "plan": "vc2-1c-1gb", "monthlyCost": 5, "health": "online",
            "healthLabel": "● Online · Mirror node", "version": "0.8.6", "online": true,
            "actions": ["update", "installBinary", "restart", "stop", "start", "terminal", "destroy"],
        ],
        [
            "name": "box", "address": "10.0.0.2", "user": "pi", "status": "installed", "health": "offline",
            "healthLabel": "● Offline · unreachable", "actions": ["update", "teleport"],
        ],
    ],
    "online": 1, "attention": 1, "canCreate": true, "busy": false, "desktop": "desk",
]

final class HostModelTests: XCTestCase {
    func testRowsAndSummaryReadTheDesktopsList() {
        let snapshot = HostsSnapshot(json: listAnswer)
        XCTAssertEqual(snapshot.sorted.map(\.name), ["box", "mirror35"], "what needs a look comes first")
        let mirror = snapshot.hosts[0]
        XCTAssertTrue(mirror.isVultr)
        XCTAssertEqual(mirror.planLine, "vc2-1c-1gb · ewr · $5/mo")
        XCTAssertEqual(mirror.healthLine, "Online · Mirror node")
        XCTAssertEqual(mirror.actions, [.update, .installBinary, .restart, .stop, .start, .terminal, .destroy])
        XCTAssertEqual(snapshot.hosts[1].actions, [.update], "unknown verbs are dropped")
        XCTAssertEqual(snapshot.summary, "2 saved · 1 online · 1 needs attention")
        XCTAssertEqual(HostsSnapshot(json: ["hosts": [listAnswer["hosts"]![0]!], "online": 1]).summary, "All 1 host online")
        XCTAssertEqual(HostHealth(wire: "weird"), .unknown)
        XCTAssertEqual(HostFormat.monthly(3.5), "$3.50/mo")
        XCTAssertEqual(HostFormat.memory(2048), "2 GB RAM")
    }

    func testDestroyNeedsTheNameTypedBackExactly() {
        let confirmation = HostConfirmation(json: [
            "token": "t", "action": "destroy", "host": "mirror35", "summary": "gone", "typeToConfirm": "mirror35",
        ])!
        XCTAssertTrue(confirmation.needsTypedName)
        XCTAssertFalse(confirmation.accepts(typed: ""))
        XCTAssertFalse(confirmation.accepts(typed: "MIRROR35"))
        XCTAssertTrue(confirmation.accepts(typed: " mirror35 "))
        let restart = HostConfirmation(json: ["token": "t", "action": "restart", "host": "mirror35"])!
        XCTAssertTrue(restart.accepts(typed: ""))
        XCTAssertNil(HostConfirmation(json: ["token": "", "action": "restart"]))
        XCTAssertNil(HostConfirmation(json: ["token": "t", "action": "format-disk"]))
        XCTAssertTrue(HostAction.destroy.needsTypedName && !HostAction.update.needsTypedName)
    }

    func testTheAddHostFormOffersTheDesktopsOwnChoices() {
        let options = HostCreateOptions(json: [
            "vultr": true, "defaultPlan": "vhp-1c-2gb", "defaultRegion": "ewr", "suggestedName": "mirror39",
            "plans": [
                ["id": "vhp-1c-2gb", "monthlyCost": 5, "ramMb": 2048, "diskGb": 50, "vcpus": 1, "regions": ["ewr", "atl"]],
                ["id": "vc2-2c-4gb", "monthlyCost": 20, "ramMb": 4096, "regions": ["ord"]],
            ],
        ])
        var form = AddHostForm(options: options)
        XCTAssertEqual(form.planId, "vhp-1c-2gb")
        XCTAssertEqual(form.region, "ewr")
        XCTAssertNil(form.problem)
        XCTAssertEqual(form.effectiveName, "mirror39")
        XCTAssertEqual(form.requestedPlan, "", "the defaults are sent as no choice at all")
        XCTAssertEqual(form.requestedRegion, "")
        XCTAssertEqual(form.plan?.label, "$5/mo · 1 vCPU · 2 GB RAM · 50 GB disk")

        form.region = "atl"
        XCTAssertEqual(form.requestedRegion, "atl")
        form.choosePlan("vc2-2c-4gb")
        XCTAssertEqual(form.region, "ord", "a region the new plan lacks is replaced")
        XCTAssertEqual(form.requestedPlan, "vc2-2c-4gb")

        form.name = "bad name"
        XCTAssertNotNil(form.problem)
        form.name = "mirror-40"
        XCTAssertNil(form.problem)
        XCTAssertEqual(form.effectiveName, "mirror-40")
        XCTAssertFalse(AddHostForm.validName("-x"))

        let noKey = AddHostForm(options: HostCreateOptions(json: ["vultr": false]))
        XCTAssertNotNil(noKey.problem)
    }

    func testTheLogBufferPausesSearchesCopiesAndStaysBounded() {
        var buffer = HostLogBuffer(limit: 100)
        buffer.appendBacklog(["[mirror] started", "plain line"])
        XCTAssertEqual(buffer.lines.map(\.source), ["mirror", ""])
        XCTAssertEqual(buffer.lines.map(\.text), ["started", "plain line"])

        buffer.append(batch: ["seq": 1, "dropped": 3, "lines": [
            ["t": 1, "src": "ssh", "text": "Accepted publickey"],
            ["t": 2, "src": "system", "text": "kernel: eth0 up"],
        ]])
        XCTAssertEqual(buffer.dropped, 3)
        XCTAssertEqual(buffer.filtered("PUBLICKEY").map(\.text), ["Accepted publickey"])
        XCTAssertEqual(buffer.filtered("ssh").count, 1, "search matches the source too")
        XCTAssertEqual(buffer.filtered("").count, 4)
        XCTAssertEqual(buffer.copyText("kernel"), "[system] kernel: eth0 up")

        buffer.setPaused(true)
        buffer.append(batch: ["lines": [["src": "node", "text": "held"]]])
        XCTAssertEqual(buffer.lines.count, 4, "paused: nothing moves under the reader")
        XCTAssertEqual(buffer.held.count, 1)
        buffer.setPaused(false)
        XCTAssertEqual(buffer.lines.last?.text, "held")

        for i in 0..<250 { buffer.append(batch: ["lines": [["src": "node", "text": .string("l\(i)")]]]) }
        XCTAssertEqual(buffer.lines.count, 100)
        XCTAssertEqual(buffer.lines.last?.text, "l249")
        XCTAssertEqual(buffer.dropped, 3 + 155)
        let ids = buffer.lines.map(\.id)
        XCTAssertEqual(Set(ids).count, ids.count, "ids stay unique for the list")
    }
}

final class HostsStoreTests: XCTestCase {
    @MainActor func testListRefreshesOnTheDesktopsBell() async {
        let channel = FakeHostsChannel()
        channel.answers["hosts.list"] = listAnswer
        let store = HostsStore(channel: channel)
        await store.refresh()
        XCTAssertEqual(store.snapshot?.hosts.count, 2)
        XCTAssertEqual(store.host(named: "MIRROR35")?.version, "0.8.6")
        channel.emit("hosts.changed")
        let refreshed = await waitUntilHosts { channel.calls("hosts.list").count == 2 }
        XCTAssertTrue(refreshed)

        channel.failures["hosts.list"] = "not_owner"
        await store.refresh()
        XCTAssertEqual(store.error, "Only the account that owns this desktop can manage its hosts.")
    }

    @MainActor func testEveryChangeIsPreparedThenRunWithItsToken() async throws {
        let channel = FakeHostsChannel()
        channel.answers["hosts.prepare"] = [
            "token": "tok-1", "action": "destroy", "host": "mirror35", "summary": "Permanently DELETE",
            "typeToConfirm": "mirror35",
        ]
        channel.answers["hosts.run"] = ["started": true, "host": "mirror35", "message": "Destroying the server."]
        let store = HostsStore(channel: channel, now: { 42 })
        let confirmation = try await store.prepare(.destroy, host: "mirror35")
        XCTAssertEqual(channel.calls("hosts.prepare").first?.str("action"), "destroy")

        do {
            try await store.run(confirmation, typedName: "mirror3")
            XCTFail("a wrong name never reaches the desktop")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "name_mismatch")
        }
        XCTAssertTrue(channel.calls("hosts.run").isEmpty)

        let message = try await store.run(confirmation, typedName: "mirror35")
        XCTAssertEqual(message, "Destroying the server.")
        let run = channel.calls("hosts.run")[0]
        XCTAssertEqual(run.str("token"), "tok-1")
        XCTAssertEqual(run.str("confirmName"), "mirror35")
        XCTAssertTrue(store.isRunning(.destroy, host: "Mirror35"))

        channel.emit("hosts.action", ["action": "destroy", "host": "mirror35", "ok": true, "message": "Destroyed"])
        XCTAssertFalse(store.isRunning(.destroy, host: "mirror35"))
        XCTAssertEqual(store.outcomes.first?.headline, "Destroy server of mirror35 finished")
        XCTAssertEqual(store.outcomes.first?.atMs, 42)
    }

    @MainActor func testANewHostSendsOnlyTheChoicesThatDifferFromTheDefaults() async throws {
        let channel = FakeHostsChannel()
        channel.answers["hosts.prepare"] = ["token": "tok-2", "action": "install", "host": "", "summary": "bills monthly"]
        channel.answers["hosts.run"] = ["started": true, "host": "mirror39", "message": "Creating the server."]
        let store = HostsStore(channel: channel)
        let confirmation = try await store.prepare(.install, host: "")
        try await store.run(confirmation, plan: "vc2-2c-4gb", region: "ord")
        let run = channel.calls("hosts.run")[0]
        XCTAssertEqual(run.str("plan"), "vc2-2c-4gb")
        XCTAssertEqual(run.str("region"), "ord")
        XCTAssertNil(run["confirmName"])
        XCTAssertTrue(store.isRunning(.install, host: "mirror39"), "keyed by the name the desktop settled on")
    }

    @MainActor func testAnSshTerminalIsConfirmedThenOpenedOnTheDesktop() async throws {
        let channel = FakeHostsChannel()
        channel.answers["hosts.prepare"] = [
            "token": "tok-3", "action": "terminal", "host": "mirror35", "summary": "Open an SSH shell",
        ]
        channel.answers["hosts.run"] = [
            "started": true, "terminal": "t-ssh", "provider": "ssh", "host": "mirror35", "label": "SSH to mirror35",
        ]
        let store = HostsStore(channel: channel)
        let confirmation = try await store.prepare(.terminal, host: "mirror35")
        XCTAssertFalse(confirmation.needsTypedName)

        do {
            try await store.run(confirmation)
            XCTFail("a shell is never started as a background action")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "unsupported_action")
        }

        let terminal = try await store.openTerminal(confirmation, cols: 60, rows: 30)
        XCTAssertEqual(terminal, "t-ssh")
        let run = channel.calls("hosts.run")[0]
        XCTAssertEqual(run.str("action"), "terminal")
        XCTAssertEqual(run.str("token"), "tok-3")
        XCTAssertEqual(run.int("cols"), 60)
        XCTAssertEqual(run.int("rows"), 30)
        XCTAssertFalse(store.isRunning(.terminal, host: "mirror35"), "a shell is a screen, not a pending action")

        channel.answers["hosts.run"] = ["started": true]
        do {
            _ = try await store.openTerminal(confirmation)
            XCTFail("an answer without a terminal is a failure")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "terminal_failed")
        }

        channel.answers["terminal.list"] = ["terminals": [
            ["terminal": "a", "provider": "ssh", "host": "Mirror35", "label": "SSH to Mirror35"],
            ["terminal": "b", "provider": "ssh", "host": "box"],
            ["terminal": "c", "provider": "claude-code"],
        ]]
        let open = try await store.openTerminals(host: "mirror35")
        XCTAssertEqual(open.map(\.terminal), ["a"])
    }

    @MainActor func testTheLiveLogAcknowledgesItsStreamAndIgnoresOthers() async {
        let channel = FakeHostsChannel()
        channel.answers["hosts.logs.start"] = ["stream": "st1", "backlog": ["[node] hello"], "window": 4]
        let log = HostLogStream(channel: channel, host: "mirror35")
        await log.start()
        XCTAssertTrue(log.live)
        XCTAssertEqual(log.buffer.lines.map(\.text), ["hello"])

        channel.emit("hosts.log", ["stream": "other", "seq": 1, "lines": [["text": "not mine"]]])
        channel.emit("hosts.log", ["stream": "st1", "seq": 1, "lines": [["src": "mirror", "text": "sync ok"]]])
        XCTAssertEqual(log.buffer.lines.map(\.text), ["hello", "sync ok"])
        let acked = await waitUntilHosts { !channel.calls("hosts.logs.ack").isEmpty }
        XCTAssertTrue(acked)
        XCTAssertEqual(channel.calls("hosts.logs.ack").first?.int("seq"), 1)
        XCTAssertEqual(channel.calls("hosts.logs.ack").first?.str("stream"), "st1")

        channel.emit("__closed", ["reason": "relay_reconnected"])
        XCTAssertFalse(log.live)
        XCTAssertFalse(log.error.isEmpty)

        await log.stop()
        XCTAssertEqual(channel.tapCount, 0)
    }
}

@MainActor
func waitUntilHosts(_ timeout: TimeInterval = 5, _ predicate: @MainActor () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if predicate() { return true }
        try? await Task.sleep(nanoseconds: 10_000_000)
    }
    return predicate()
}
