import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMAgents

@MainActor
private final class FakeTerminalDesktop: DesktopTerminalChannel {
    final class Link {}

    var link: Link? = Link()
    var requests: [(method: String, params: JSONObject)] = []
    var handlers: [String: @MainActor (JSONObject) async throws -> JSONObject] = [:]
    var inFlight: [String: Int] = [:]
    var maxInFlight: [String: Int] = [:]
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]
    private var listeners: [UUID: @MainActor () -> Void] = [:]

    var linkIdentity: ObjectIdentifier? { link.map { ObjectIdentifier($0) } }

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        requests.append((method, params))
        inFlight[method, default: 0] += 1
        maxInFlight[method] = max(maxInFlight[method] ?? 0, inFlight[method] ?? 0)
        defer { inFlight[method, default: 0] -= 1 }
        if link == nil { throw LocalControlException("not_connected") }
        if let handler = handlers[method] { return try await handler(params) }
        return [:]
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    func addLinkListener(_ listener: @escaping @MainActor () -> Void) -> UUID {
        let id = UUID()
        listeners[id] = listener
        return id
    }

    func removeLinkListener(_ id: UUID) { listeners.removeValue(forKey: id) }

    var tapCount: Int { taps.count }

    func event(_ name: String, _ data: JSONObject) {
        for tap in taps.values { tap(name, data) }
    }

    func output(_ terminal: String, _ text: String, reset: Bool = false) {
        event(
            "terminal.output",
            ["terminal": .string(terminal), "data": .string(B64.encode(Array(text.utf8))), "reset": .bool(reset)])
    }

    func changeLink(_ next: Link?) {
        link = next
        for listener in listeners.values { listener() }
    }

    func calls(_ method: String) -> [JSONObject] {
        requests.filter { $0.method == method }.map(\.params)
    }

    func typed() -> String {
        calls("terminal.input").map { String(decoding: (try? B64.decode($0.str("data"))) ?? Data(), as: UTF8.self) }
            .joined()
    }
}

private func terminalObject(_ id: String, replay: String = "") -> JSONObject {
    [
        "terminal": .string(id), "provider": "claude-code", "label": "Claude Code", "cli": "claude",
        "repositoryOwner": "alice", "repositoryName": "repo", "cwd": "/home/alice/repo", "startedAtMs": 5,
        "device": "Alice's iPhone", "running": true, "cols": 80, "rows": 24,
        "replay": .string(B64.encode(Array(replay.utf8))),
    ]
}

@MainActor
private func screenText(_ terminal: DesktopAgentTerminal) -> String {
    terminal.screen.allRows.map(\.text).joined(separator: "\n")
}

@MainActor
private func eventually(_ timeout: TimeInterval = 5, _ predicate: @MainActor () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if predicate() { return true }
        try? await Task.sleep(nanoseconds: 5_000_000)
    }
    return predicate()
}

final class DesktopAgentTerminalTests: XCTestCase {
    @MainActor func testStartAsksForTheRepositoryProviderAndSize() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.start"] = { _ in terminalObject("t1") }
        let terminal = DesktopAgentTerminal(channel: desktop, owner: "alice", name: "repo", provider: "claude-code")
        XCTAssertTrue(terminal.isStarting)
        terminal.resize(cols: 100, rows: 30)
        terminal.start(cols: 100, rows: 30)
        let live = await eventually { terminal.live }
        XCTAssertTrue(live)
        let start = desktop.calls("terminal.start").first ?? [:]
        XCTAssertEqual(start.str("owner"), "alice")
        XCTAssertEqual(start.str("name"), "repo")
        XCTAssertEqual(start.str("provider"), "claude-code")
        XCTAssertEqual(start.int("cols"), 100)
        XCTAssertEqual(start.int("rows"), 30)
        XCTAssertEqual(terminal.terminalId, "t1")
        XCTAssertEqual(terminal.info?.device, "Alice's iPhone")
        XCTAssertEqual(terminal.providerLabel, "Claude Code")
        XCTAssertEqual(terminal.statusLevel, .ok)
        XCTAssertEqual(terminal.screen.grid.first?.cells.count, 100)
        XCTAssertFalse(terminal.isStarting)
        let resized = await eventually { desktop.calls("terminal.resize").count == 1 }
        XCTAssertTrue(resized, "the desktop answered 80x24, so the phone's size is sent")
    }

    @MainActor func testASignInTerminalSendsItsModeAndAccount() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.start"] = { _ in terminalObject("t2") }
        let terminal = DesktopAgentTerminal(
            channel: desktop, owner: "", name: "", provider: "claude-code", mode: "login", account: "work")
        XCTAssertTrue(terminal.isSignIn)
        XCTAssertEqual(terminal.statusText, "Starting Claude Code sign-in on the desktop…")
        terminal.start(cols: 80, rows: 24)
        let live = await eventually { terminal.live }
        XCTAssertTrue(live)
        let start = desktop.calls("terminal.start").first ?? [:]
        XCTAssertEqual(start.str("mode"), "login")
        XCTAssertEqual(start.str("account"), "work")
        XCTAssertEqual(terminal.taskLabel, "Claude Code sign-in")
    }

    @MainActor func testAnAgentTerminalLeavesModeAndAccountOut() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.start"] = { _ in terminalObject("t3") }
        let terminal = DesktopAgentTerminal(channel: desktop, owner: "alice", name: "repo", provider: "codex")
        terminal.start(cols: 80, rows: 24)
        let live = await eventually { terminal.live }
        XCTAssertTrue(live)
        let start = desktop.calls("terminal.start").first ?? [:]
        XCTAssertNil(start["mode"])
        XCTAssertNil(start["account"])
        XCTAssertFalse(terminal.isSignIn)
    }

    @MainActor func testARefusedStartSaysWhy() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.start"] = { _ in throw LocalControlException("cli_missing") }
        let terminal = DesktopAgentTerminal(channel: desktop, owner: "alice", name: "repo", provider: "codex")
        terminal.start(cols: 80, rows: 24)
        let failed = await eventually { terminal.statusLevel == .error }
        XCTAssertTrue(failed)
        XCTAssertEqual(terminal.errorCode, "cli_missing")
        XCTAssertEqual(terminal.statusText, ControlErrorText.describe(code: "cli_missing"))
        XCTAssertFalse(terminal.live)
        XCTAssertEqual(desktop.calls("terminal.start").count, 1, "a refused start is not retried")
    }

    @MainActor func testResumeOpensTheAgentInATerminal() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.resume"] = { _ in
            var answer = terminalObject("t9", replay: "resumed screen")
            answer["session"] = 17
            return answer
        }
        let terminal = DesktopAgentTerminal(
            channel: desktop, session: 17, owner: "alice", name: "repo", provider: "claude-code")
        XCTAssertEqual(terminal.statusText, "Opening the agent in a terminal on the desktop…")
        terminal.start(cols: 90, rows: 28)
        let live = await eventually { terminal.live }
        XCTAssertTrue(live)
        let resume = desktop.calls("terminal.resume").first ?? [:]
        XCTAssertEqual(resume.int("id"), 17)
        XCTAssertEqual(resume.int("cols"), 90)
        XCTAssertEqual(resume.int("rows"), 28)
        XCTAssertTrue(desktop.calls("terminal.start").isEmpty)
        XCTAssertEqual(terminal.terminalId, "t9")
        XCTAssertTrue(screenText(terminal).hasPrefix("resumed screen"))
    }

    @MainActor func testSupportReadsTheResumeCapability() {
        let status: JSONObject = ["capabilities": ["agents", "terminal.agents", "terminal.resume"]]
        XCTAssertTrue(DesktopTerminalSupport.supported(status: status, DesktopTerminalSupport.resumeCapability))
        XCTAssertFalse(
            DesktopTerminalSupport.supported(
                status: ["capabilities": ["terminal.agents"]], DesktopTerminalSupport.resumeCapability))
        XCTAssertFalse(DesktopTerminalSupport.supported(status: status, DesktopTerminalSupport.accountsCapability))
        XCTAssertTrue(
            DesktopTerminalSupport.supported(
                status: ["capabilities": ["terminal.accounts"]], DesktopTerminalSupport.accountsCapability))
    }

    @MainActor func testOutputFeedsTheScreenAndResetStartsAgain() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.attach"] = { _ in terminalObject("t1", replay: "earlier\r\n") }
        let terminal = DesktopAgentTerminal(channel: desktop, terminal: "t1")
        terminal.start(cols: 80, rows: 24)
        let live = await eventually { terminal.live }
        XCTAssertTrue(live)
        XCTAssertEqual(desktop.calls("terminal.attach").first?.str("terminal"), "t1")
        XCTAssertTrue(screenText(terminal).contains("earlier"))

        desktop.output("t1", "and now")
        desktop.output("other", "not mine")
        XCTAssertTrue(screenText(terminal).contains("now"))
        XCTAssertFalse(screenText(terminal).contains("not mine"))

        desktop.output("t1", "fresh", reset: true)
        XCTAssertTrue(screenText(terminal).contains("fresh"))
        XCTAssertFalse(screenText(terminal).contains("earlier"))
        let repainted = await eventually { terminal.revision > 0 }
        XCTAssertTrue(repainted)
    }

    @MainActor func testASplitCharacterDecodesWhole() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.attach"] = { _ in terminalObject("t1") }
        let terminal = DesktopAgentTerminal(channel: desktop, terminal: "t1")
        terminal.start(cols: 80, rows: 24)
        _ = await eventually { terminal.live }
        let bytes = Array("café".utf8)
        desktop.event("terminal.output", ["terminal": "t1", "data": .string(B64.encode(bytes[0..<4]))])
        desktop.event("terminal.output", ["terminal": "t1", "data": .string(B64.encode(bytes[4...]))])
        XCTAssertTrue(screenText(terminal).contains("café"))
    }

    @MainActor func testExitEndsTheTerminal() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.attach"] = { _ in terminalObject("t1") }
        let terminal = DesktopAgentTerminal(channel: desktop, terminal: "t1")
        terminal.start(cols: 80, rows: 24)
        _ = await eventually { terminal.live }
        desktop.event("terminal.exit", ["terminal": "other", "code": 3])
        XCTAssertNil(terminal.exitCode)
        desktop.event("terminal.exit", ["terminal": "t1", "code": 0])
        XCTAssertEqual(terminal.exitCode, 0)
        XCTAssertFalse(terminal.live)
        XCTAssertTrue(terminal.closed)
        XCTAssertEqual(terminal.statusText, "Claude Code ended.")
        XCTAssertEqual(desktop.tapCount, 0)
        terminal.input("ignored")
        try? await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertTrue(desktop.calls("terminal.input").isEmpty)
    }

    @MainActor func testInputIsBatchedAndSentInOrderOneAtATime() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.attach"] = { _ in terminalObject("t1") }
        desktop.handlers["terminal.input"] = { _ in
            try await Task.sleep(nanoseconds: 20_000_000)
            return [:]
        }
        let terminal = DesktopAgentTerminal(channel: desktop, terminal: "t1", inputFlush: 0.01)
        terminal.start(cols: 80, rows: 24)
        _ = await eventually { terminal.live }
        terminal.input("h")
        terminal.input("i")
        let batched = await eventually { desktop.calls("terminal.input").count == 1 }
        XCTAssertTrue(batched)
        let long = String(repeating: "x", count: 9000)
        terminal.input(long)
        terminal.paste("y")
        terminal.key(.enter)
        let all = await eventually { desktop.typed() == "hi" + long + "y\r" && desktop.inFlight["terminal.input"] == 0 }
        XCTAssertTrue(all, desktop.typed().suffix(5).description)
        XCTAssertEqual(desktop.maxInFlight["terminal.input"], 1, "one input request at a time")
        XCTAssertTrue(desktop.calls("terminal.input").allSatisfy { $0.str("terminal") == "t1" })
        XCTAssertGreaterThanOrEqual(desktop.calls("terminal.input").count, 4, "a long paste is cut into pieces")
    }

    @MainActor func testEventsThatBeatTheAnswerAreKept() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.attach"] = { _ in
            desktop.output("t1", "after")
            return terminalObject("t1", replay: "before ")
        }
        let terminal = DesktopAgentTerminal(channel: desktop, terminal: "t1")
        terminal.start(cols: 80, rows: 24)
        _ = await eventually { terminal.live }
        XCTAssertTrue(screenText(terminal).contains("before after"))
    }

    @MainActor func testANewLinkAttachesAgainAndRedrawsFromTheReplay() async {
        let desktop = FakeTerminalDesktop()
        var replay = "first"
        desktop.handlers["terminal.attach"] = { _ in terminalObject("t1", replay: replay) }
        let terminal = DesktopAgentTerminal(
            channel: desktop, terminal: "t1", sleep: { _ in try await Task.sleep(nanoseconds: 10_000_000) })
        terminal.start(cols: 80, rows: 24)
        _ = await eventually { terminal.live }
        XCTAssertTrue(screenText(terminal).contains("first"))

        replay = "second"
        desktop.changeLink(FakeTerminalDesktop.Link())
        let again = await eventually { desktop.calls("terminal.attach").count == 2 && terminal.live }
        XCTAssertTrue(again)
        XCTAssertTrue(screenText(terminal).contains("second"))
        XCTAssertFalse(screenText(terminal).contains("first"))

        desktop.changeLink(nil)
        XCTAssertFalse(terminal.live)
        XCTAssertEqual(terminal.statusLevel, .pending)
        replay = "third"
        desktop.changeLink(FakeTerminalDesktop.Link())
        let third = await eventually { terminal.live && screenText(terminal).contains("third") }
        XCTAssertTrue(third)
    }

    @MainActor func testAGoneTerminalSaysSo() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.attach"] = { _ in throw LocalControlException("unknown_terminal") }
        let terminal = DesktopAgentTerminal(channel: desktop, terminal: "t9")
        terminal.start(cols: 80, rows: 24)
        let gone = await eventually { terminal.closed }
        XCTAssertTrue(gone)
        XCTAssertEqual(terminal.errorCode, "unknown_terminal")
        XCTAssertEqual(terminal.statusText, ControlErrorText.describe(code: "unknown_terminal"))
    }

    @MainActor func testDetachLeavesItRunningAndEndClosesIt() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.attach"] = { _ in terminalObject("t1") }
        let terminal = DesktopAgentTerminal(channel: desktop, terminal: "t1", inputFlush: 5)
        terminal.start(cols: 80, rows: 24)
        _ = await eventually { terminal.live }
        terminal.input("last words")
        terminal.detach()
        let detached = await eventually { desktop.calls("terminal.detach").count == 1 }
        XCTAssertTrue(detached)
        XCTAssertEqual(desktop.typed(), "last words", "what was typed goes before the detach")
        XCTAssertEqual(desktop.requests.last?.method, "terminal.detach")
        XCTAssertEqual(desktop.tapCount, 0)
        XCTAssertFalse(terminal.closed)

        terminal.reattach()
        let back = await eventually { desktop.calls("terminal.attach").count == 2 && terminal.live }
        XCTAssertTrue(back)
        await terminal.end()
        XCTAssertEqual(desktop.calls("terminal.close").first?.str("terminal"), "t1")
        XCTAssertTrue(terminal.closed)
        XCTAssertEqual(terminal.statusText, "You ended Claude Code.")
    }

    @MainActor func testAFailedEndKeepsTheTerminal() async {
        let desktop = FakeTerminalDesktop()
        desktop.handlers["terminal.attach"] = { _ in terminalObject("t1") }
        desktop.handlers["terminal.close"] = { _ in throw LocalControlException("not_connected") }
        let terminal = DesktopAgentTerminal(channel: desktop, terminal: "t1")
        terminal.start(cols: 80, rows: 24)
        _ = await eventually { terminal.live }
        await terminal.end()
        XCTAssertFalse(terminal.closed)
        XCTAssertEqual(terminal.statusLevel, .error)
    }
}
