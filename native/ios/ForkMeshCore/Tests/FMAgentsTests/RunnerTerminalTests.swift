import FMApi
import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMAgents

private let terminalId = String(repeating: "e", count: 32)

@MainActor
final class RunnerTerminalTests: XCTestCase {
    private func makeApi(_ http: FakeHTTPClient) -> ApiService {
        let api = ApiService(settings: StaticApiSettings(serverUrl: "ws://test.local/ws"), http: http, sleep: { _ in })
        api.sessionTokenProvider = { "tok" }
        return api
    }

    private func body(_ request: HTTPRequest) -> JSONObject {
        JSON.parseObject(String(decoding: request.body ?? Data(), as: UTF8.self))
    }

    private func settle(_ http: FakeHTTPClient, count: Int) async {
        for _ in 0..<200 where http.requests.count < count {
            await Task.yield()
        }
    }

    func testOpensATerminalDrawsItsOutputAndSendsKeys() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 201, json: .object([
            "ok": .bool(true), "id": .string(terminalId),
            "session": .object(["state": .string("pending"), "kind": .string("terminal")]),
        ]))
        let terminal = RunnerTerminal(
            api: makeApi(http), runner: "fmnext-r1", provider: "claude-code", accountLabel: "Work", sleep: { _ in })
        terminal.resize(cols: 80, rows: 24)
        await terminal.openSession()
        let start: HTTPRequest = http.requests[0]
        XCTAssertEqual(start.method, "POST")
        XCTAssertEqual(start.url.path, "/api/runners/fmnext-r1/login")
        XCTAssertEqual(start.headers["Authorization"], "Bearer tok")
        XCTAssertEqual(body(start).str("kind"), "terminal")
        XCTAssertEqual(body(start).str("provider"), "claude-code")
        XCTAssertEqual(body(start).int("cols"), 80)
        XCTAssertEqual(body(start).int("rows"), 24)
        XCTAssertEqual(terminal.statusLevel, .pending)
        XCTAssertTrue(terminal.statusText.hasPrefix("Waiting for fmnext-r1 to open Claude Code as Work."))

        http.enqueue(status: 200, json: .object([
            "ok": .bool(true), "state": .string("running"), "nextSeq": .int(1),
            "chunks": .array([.object(["seq": .int(1), "data": .string(Data("hello\r\nworld".utf8).base64EncodedString())])]),
        ]))
        _ = await terminal.pollOnce()
        XCTAssertEqual(http.requests[1].url.path, "/api/runners/fmnext-r1/login/\(terminalId)/output")
        XCTAssertEqual(http.requests[1].url.query, "after=0")
        let text: String = terminal.screen.allRows.map { $0.text }.joined(separator: "\n")
        XCTAssertTrue(text.contains("hello"))
        XCTAssertTrue(text.contains("world"))
        XCTAssertEqual(terminal.statusLevel, .ok)
        XCTAssertEqual(terminal.session.afterSeq, 1)

        http.enqueue(status: 200, json: .object(["ok": .bool(true), "seq": .int(1)]))
        terminal.key(.up)
        terminal.input("x")
        await settle(http, count: 3)
        XCTAssertEqual(http.requests[2].url.path, "/api/runners/fmnext-r1/login/\(terminalId)/input")
        XCTAssertEqual(body(http.requests[2]).str("data"), "\u{1b}[Ax")

        http.enqueue(status: 200, json: .object(["ok": .bool(true), "seq": .int(2)]))
        http.enqueue(status: 200, json: .object(["ok": .bool(true), "seq": .int(3)]))
        terminal.submit("ls -la")
        await settle(http, count: 5)
        XCTAssertEqual(body(http.requests[3]).str("data"), "ls -la")
        XCTAssertEqual(body(http.requests[4]).str("data"), "\r")

        http.enqueue(status: 200, json: .object(["ok": .bool(true)]))
        terminal.resize(cols: 100, rows: 30)
        await terminal.sendResize()
        XCTAssertEqual(http.requests[5].url.path, "/api/runners/fmnext-r1/login/\(terminalId)/resize")
        XCTAssertEqual(body(http.requests[5]).int("cols"), 100)
        await terminal.sendResize()
        XCTAssertEqual(http.requests.count, 6)

        http.enqueue(status: 200, json: .object([
            "ok": .bool(true), "state": .string("succeeded"), "exitCode": .int(0), "message": .string(""),
            "chunks": .array([]), "nextSeq": .int(1),
        ]))
        _ = await terminal.pollOnce()
        XCTAssertFalse(terminal.open)
        XCTAssertEqual(terminal.exitCode, 0)
        XCTAssertEqual(terminal.statusText, "Claude Code exited.")
        await terminal.close()
        XCTAssertEqual(http.requests.count, 7)
    }

    func testAttachesToAnOpenTerminalAndCancelsOnClose() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: .object(["ok": .bool(true)]))
        http.enqueue(status: 200, json: .object(["ok": .bool(true), "state": .string("cancelled")]))
        let terminal = RunnerTerminal(
            api: makeApi(http), runner: "fmnext-r1", provider: "codex", openId: terminalId, sleep: { _ in })
        await terminal.openSession()
        XCTAssertEqual(http.requests.map { "\($0.method) \($0.url.path)" }, [
            "POST /api/runners/fmnext-r1/login/\(terminalId)/resize",
        ])
        XCTAssertEqual(body(http.requests[0]).int("cols"), 100)
        await terminal.close()
        XCTAssertEqual(http.requests[1].url.path, "/api/runners/fmnext-r1/login/\(terminalId)/cancel")
        XCTAssertEqual(terminal.session.state, "cancelled")
    }

    func testAnOldRunnerIsAskedToUpdate() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 409, json: .object([
            "ok": .bool(false), "error": .string("terminal_unsupported"),
        ]))
        let terminal = RunnerTerminal(api: makeApi(http), runner: "fmnext-r1", provider: "codex", sleep: { _ in })
        await terminal.openSession()
        XCTAssertEqual(terminal.statusText, "fmnext-r1 needs an update before it can open a terminal.")
        XCTAssertEqual(terminal.statusLevel, .error)
        await terminal.close()
        XCTAssertEqual(http.requests.count, 1)
    }

    func testInputIsSplitOnCharacterBoundaries() {
        var pending: String = String(repeating: "é", count: 3000)
        let first: String = RunnerTerminal.take(&pending)
        XCTAssertEqual(first.utf8.count, 4096)
        XCTAssertEqual(pending.count, 952)
        XCTAssertEqual(RunnerTerminal.clampCols(1000), 300)
        XCTAssertEqual(RunnerTerminal.clampRows(1), 5)
    }
}

final class RunnerUsageMetersTests: XCTestCase {
    func testRunnerWindowsBecomeMeters() {
        let meter = UsageWindowMeter(runner: RunnerUsageWindow(
            id: "five_hour", label: "5-hour session", usedPercent: 41.6, resetsAtMs: 5_000))
        XCTAssertEqual(meter.usedPercent, 42)
        XCTAssertEqual(meter.resetAtMs, 5_000)
        XCTAssertEqual(meter.state, .ok)
        XCTAssertEqual(UsageWindowMeter(runner: RunnerUsageWindow(id: "w", usedPercent: 75)).state, .high)
        XCTAssertEqual(UsageWindowMeter(runner: RunnerUsageWindow(id: "w", usedPercent: 90)).state, .exhausted)
        XCTAssertNil(UsageWindowMeter(runner: RunnerUsageWindow(id: "w")).resetAtMs)
    }
}
