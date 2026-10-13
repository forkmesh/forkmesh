// The login terminal's emulator and session; the widget's behaviour is covered through the session.

import FMAccount
import FMApi
import FMBase
import FMNet
import XCTest
@testable import FMAgents

final class TerminalScreenTests: XCTestCase {
    func testPrintsWrapsAndMovesTheCursor() {
        let screen = TerminalScreen(4, 10)
        screen.feed("hello\r\nworld")
        XCTAssertEqual(screen.grid[0].text, "hello")
        XCTAssertEqual(screen.grid[1].text, "world")

        screen.feed("\u{1b}[1;3HX")
        XCTAssertEqual(screen.grid[0].text, "heXlo")

        screen.feed("\u{1b}[2J\u{1b}[H0123456789ab")
        XCTAssertEqual(screen.grid[0].text, "0123456789")
        XCTAssertTrue(screen.grid[0].wrapped)
        XCTAssertEqual(screen.grid[1].text, "ab")
    }

    func testErasesToTheEndOfTheLineAndKeepsColoursPerCell() {
        let screen = TerminalScreen(2, 10)
        screen.feed("abcdef\u{1b}[4G\u{1b}[K")
        screen.feed("\r\n\u{1b}[1;31mR\u{1b}[0mn")
        XCTAssertEqual(screen.grid[0].text, "abc")
        XCTAssertEqual(screen.grid[1].cells[0].fg, 1)
        XCTAssertTrue(screen.grid[1].cells[0].bold)
        XCTAssertEqual(screen.grid[1].cells[1].fg, AgentTerminalConfig.defaultColor)
    }

    func testExtendedColoursArePacked() {
        let screen = TerminalScreen(1, 10)
        screen.feed("\u{1b}[38;5;200ma\u{1b}[48;2;1;2;3mb\u{1b}[97mc")
        XCTAssertEqual(screen.grid[0].cells[0].fg, 200)
        XCTAssertEqual(screen.grid[0].cells[1].bg, AgentTerminalConfig.trueColor + 0x010203)
        XCTAssertEqual(screen.grid[0].cells[2].fg, 15)
    }

    func testKeyboardModeSequencesLeaveTextUnstyled() {
        let screen = TerminalScreen(4, 10)
        screen.feed("\u{1b}[>4;2m\u{1b}[>1u\u{1b}[?u\u{1b}[2;3Hab")
        XCTAssertEqual(screen.grid[1].text, "  ab")
        XCTAssertFalse(screen.grid[1].cells[2].underline)
        XCTAssertFalse(screen.grid[1].cells[2].dim)
        screen.feed("\u{1b}[4:3mc\u{1b}[4:0md\u{1b}[38:2::1:2:3me")
        XCTAssertTrue(screen.grid[1].cells[4].underline)
        XCTAssertFalse(screen.grid[1].cells[5].underline)
        XCTAssertEqual(screen.grid[1].cells[6].fg, AgentTerminalConfig.trueColor + 0x010203)
    }

    func testTheAlternateScreenIsTransientAndLeavesThePrimaryIntact() {
        let screen = TerminalScreen(3, 10)
        screen.feed("keep")
        screen.feed("\u{1b}[?1049hmenu")
        XCTAssertTrue(screen.onAltScreen)
        XCTAssertEqual(screen.grid[0].text, "menu")
        screen.feed("\u{1b}[?1049l")
        XCTAssertEqual(screen.grid[0].text, "keep")
        XCTAssertEqual(screen.cy, 0)
        XCTAssertEqual(screen.cx, 4)
    }

    func testScrolledOffLinesLandInTheScrollback() {
        let screen = TerminalScreen(2, 10)
        screen.feed("one\r\ntwo\r\nthree")
        XCTAssertEqual(screen.scrollback.map(\.text), ["one"])
        XCTAssertEqual(screen.allRows.map(\.text), ["one", "two", "three"])
    }

    func testAURLTheGridWrappedIsFoundWhole() {
        let url = "https://claude.ai/oauth/authorize?code=true&client_id=abc"
        let screen = TerminalScreen(10, 20)
        screen.feed("Open: \(url).\r\n")
        XCTAssertEqual(screen.links(), [url])
    }

    func testAnOSC8HyperlinkIsKeptAndOnlyAWebOne() {
        let screen = TerminalScreen(3, 20)
        screen.feed("\u{1b}]8;;https://example.com/a\u{07}here\u{1b}]8;;\u{07} ")
        screen.feed("\u{1b}]8;;javascript:alert(1)\u{1b}\\bad\u{1b}]8;;\u{1b}\\")
        XCTAssertEqual(screen.grid[0].cells[0].link, "https://example.com/a")
        XCTAssertEqual(screen.grid[0].cells[5].link, "")
        XCTAssertEqual(screen.links(), ["https://example.com/a"])
    }

    func testAResizeKeepsWhatWasDrawnAndWhereTheCursorWas() {
        let screen = TerminalScreen(4, 10)
        screen.feed("one\r\ntwo\r\nthree")
        let shorter = screen.resized(2, 6)
        // The rows above the cursor go to the scrollback; the cursor's row stays.
        XCTAssertEqual(shorter.scrollback.map(\.text), ["one"])
        XCTAssertEqual(shorter.grid.map(\.text), ["two", "three"])
        XCTAssertEqual(shorter.cy, 1)
        XCTAssertEqual(shorter.cx, 5)
        shorter.feed("\r\u{1b}[K4")
        XCTAssertEqual(shorter.grid[1].text, "4")

        let taller = TerminalScreen(2, 10)
        taller.feed("a\r\nb")
        let grown = taller.resized(4, 10)
        XCTAssertEqual(grown.grid.map(\.text), ["a", "b", "", ""])
        XCTAssertEqual(grown.cy, 1)
    }

    func testAResizeDropsBlankRowsUnderTheCursorBeforeAnyText() {
        let screen = TerminalScreen(6, 10)
        screen.feed("top\r\nprompt")
        let resized = screen.resized(3, 10)
        XCTAssertTrue(resized.scrollback.isEmpty)
        XCTAssertEqual(resized.grid.map(\.text), ["top", "prompt", ""])
    }

    func testDeleteAndInsertCharacters() {
        let screen = TerminalScreen(1, 8)
        screen.feed("abcdef\u{1b}[2G\u{1b}[2P")
        XCTAssertEqual(screen.grid[0].text, "adef")
        screen.feed("\u{1b}[2@")
        XCTAssertEqual(screen.grid[0].text, "a  def")
        XCTAssertEqual(screen.grid[0].cells.count, 8)
    }

    func testApplicationCursorModeChangesTheArrowKeys() {
        XCTAssertEqual(terminalKeyBytes(.up), "\u{1b}[A")
        XCTAssertEqual(terminalKeyBytes(.up, appCursorKeys: true), "\u{1b}OA")
        XCTAssertEqual(terminalControlBytes("c"), "\u{03}")
        XCTAssertEqual(terminalControlBytes("["), "\u{1b}")
        XCTAssertEqual(terminalControlBytes("1"), "")
        XCTAssertEqual(terminalControlBytes("ab"), "")
        let screen = TerminalScreen(1, 10)
        screen.feed("\u{1b}[?1h\u{1b}[?2004h")
        XCTAssertTrue(screen.appCursorKeys)
        XCTAssertTrue(screen.bracketedPaste)
    }

    func testProviderLabels() {
        XCTAssertEqual(agentProviderLabel("claude-code"), "Claude Code")
        XCTAssertEqual(agentProviderLabel("codex"), "Codex")
        XCTAssertEqual(agentProviderLabel("other"), "other")
        XCTAssertEqual(agentProviderLabel("local-llama"), "Gemma")
        XCTAssertFalse(isAgentLoginProvider("local-llama"))
        XCTAssertTrue(isAgentLoginProvider("codex"))
        XCTAssertFalse(isAgentLoginProvider("other"))
    }

    func testTheChunkedDecoderJoinsASplitCharacter() {
        var decoder = ChunkedUtf8Decoder()
        let bytes = Array("✓ ok".utf8)
        XCTAssertEqual(decoder.take(Data(bytes[0..<2])), "")
        XCTAssertEqual(decoder.take(Data(bytes[2...])), "✓ ok")
        XCTAssertEqual(decoder.take(Data([0xFF, 0x41])), "\u{FFFD}A")
    }
}

@MainActor
private final class TicketSource: AccountEventTicketSource {
    func accountEventTicket() async -> EventTicket { EventTicket(.ok, "ticket-abc") }

    func accountEventsURL(_ ticket: String, node: String) -> URL {
        URL(string: "wss://forkmesh.test/api/nodes/events?ticket=\(ticket)&node=\(node)")!
    }
}

@MainActor
final class AgentLoginTerminalTests: XCTestCase {
    @MainActor
    private struct Rig {
        let terminal: AgentLoginTerminal
        let channel: AccountEventChannel
        let sockets: FakeWebSocketConnector

        var socket: FakeWebSocketConnection { sockets.lastConnection! }

        /// Frames the phone sent, keepalives aside.
        var terminalFrames: [JSONObject] { socket.sentJSON.filter { $0["type"] == "terminal" } }

        /// A frame from the node, for this session unless told otherwise.
        func fromNode(_ frame: JSONObject, session: String? = nil) {
            var out: JSONObject = [
                "type": "terminal",
                "session": .string(session ?? terminal.session),
                "node": .string(terminal.node),
                "from": "node",
            ]
            for (key, value) in frame { out[key] = value }
            socket.receive(json: .object(out))
        }

        func output(_ text: String) {
            fromNode(["action": "output", "data": .string(B64.encode(Array(text.utf8)))])
        }
    }

    private let source = TicketSource()

    private func rig(openTimeout: TimeInterval = AgentTerminalConfig.openTimeout) -> Rig {
        let sockets = FakeWebSocketConnector()
        let channel = AccountEventChannel(api: source, sessionToken: { "token" }, connector: sockets, nodeName: "mirror-7")
        let terminal = AgentLoginTerminal(node: "mirror-7", provider: "claude-code", channel: channel, openTimeout: openTimeout)
        return Rig(terminal: terminal, channel: channel, sockets: sockets)
    }

    private func settle(_ seconds: Double = 0.06) async {
        try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
    }

    private func started(_ rig: Rig, cols: Int = 80, rows: Int = 24) async {
        rig.terminal.start(cols: cols, rows: rows)
        _ = await rig.sockets.waitForConnections(1)
        _ = await rig.socket.waitForSent(1)
        await settle()
    }

    private func decodedInputs(_ rig: Rig) -> [String] {
        rig.terminalFrames.filter { $0["action"] == "input" }.map { frame in
            String(decoding: (try? B64.decode(frame.str("data"))) ?? Data(), as: UTF8.self)
        }
    }

    func testOpensOnTheNamedNodeAndStreamsItsOutput() async {
        let rig = rig()
        XCTAssertEqual(rig.terminal.statusText, "Connecting to mirror-7…")
        await started(rig, cols: 40, rows: 12)

        let url = rig.sockets.attempts.first?.url.absoluteString ?? ""
        XCTAssertTrue(url.contains("node=mirror-7"))
        XCTAssertTrue(url.contains("ticket=ticket-abc"))
        XCTAssertEqual(rig.terminalFrames.count, 1)
        let open = rig.terminalFrames[0]
        XCTAssertEqual(open["action"], "open")
        XCTAssertEqual(open["provider"], "claude-code")
        XCTAssertEqual(open["node"], "mirror-7")
        XCTAssertEqual(open["session"], .string(rig.terminal.session))
        XCTAssertEqual(open["cols"], 40)
        XCTAssertEqual(open["rows"], 12)
        XCTAssertEqual(rig.terminal.session.count, 32)

        rig.fromNode(["action": "ready"])
        await settle()
        XCTAssertTrue(rig.terminal.live)
        XCTAssertEqual(rig.terminal.statusLevel, .ok)
        XCTAssertEqual(rig.terminal.statusText, "Live on mirror-7. Complete Claude Code's own sign-in below.")

        rig.output("Select login method:\r\n")
        // Another session's output is not this one's.
        rig.fromNode(["action": "output", "data": .string(B64.encode(Array("nope".utf8)))], session: "someone-else")
        await settle()
        XCTAssertEqual(rig.terminal.screen.grid[0].text, "Select login method:")
        XCTAssertEqual(rig.terminal.screen.grid[1].text, "")
        XCTAssertGreaterThan(rig.terminal.revision, 0)

        await rig.terminal.close()
    }

    func testACharacterSplitAcrossFramesStillDecodesWhole() async {
        let rig = rig()
        await started(rig, cols: 20, rows: 5)
        let bytes = Array("✓ ok".utf8)
        rig.fromNode(["action": "output", "data": .string(B64.encode(bytes[0..<2]))])
        rig.fromNode(["action": "output", "data": .string(B64.encode(bytes[2...]))])
        await settle()
        XCTAssertEqual(rig.terminal.screen.grid[0].text, "✓ ok")
        await rig.terminal.close()
    }

    func testKeystrokesAreBatchedIntoOneInputFrame() async {
        let rig = rig()
        await started(rig, cols: 20, rows: 5)
        rig.terminal.input("abc")
        rig.terminal.key(.enter)
        await settle()
        XCTAssertEqual(decodedInputs(rig), ["abc\r"])
        await rig.terminal.close()
    }

    func testAPasteIsBracketedWhenTheProgramAskedForIt() async {
        let rig = rig()
        await started(rig, cols: 20, rows: 5)
        rig.output("\u{1b}[?2004h")
        await settle()
        rig.terminal.paste("code#123")
        await settle()
        XCTAssertEqual(decodedInputs(rig).last, "\u{1b}[200~code#123\u{1b}[201~")
        await rig.terminal.close()
    }

    func testANodeThatNeverAnswersIsReported() async {
        let rig = rig(openTimeout: 0.02)
        await started(rig)
        await settle(0.1)
        XCTAssertEqual(rig.terminal.statusLevel, .error)
        XCTAssertTrue(rig.terminal.statusText.contains("did not answer"))
        await rig.terminal.close()
    }

    func testAnExitAndACloseAreBothReported() async {
        let rig = rig()
        await started(rig)
        rig.fromNode(["action": "exit", "code": 0])
        await settle()
        XCTAssertEqual(rig.terminal.exitCode, 0)
        XCTAssertEqual(rig.terminal.statusText, "The sign-in command finished.")

        rig.fromNode(["action": "exit", "code": 2])
        await settle()
        XCTAssertEqual(rig.terminal.statusText, "The sign-in command exited with status 2.")
        XCTAssertEqual(rig.terminal.statusLevel, .error)

        let socket = rig.socket
        await rig.terminal.close()
        XCTAssertTrue(rig.terminal.closed)
        let frames = socket.sentJSON.filter { $0["type"] == "terminal" }
        XCTAssertEqual(frames.last?["action"], "close")
    }

    func testANodeErrorIsShown() async {
        let rig = rig()
        await started(rig)
        rig.fromNode(["action": "error", "message": "  not allowed  "])
        await settle()
        XCTAssertEqual(rig.terminal.statusText, "not allowed")
        rig.fromNode(["action": "error"])
        await settle()
        XCTAssertEqual(rig.terminal.statusText, "The node refused the session.")
        await rig.terminal.close()
    }

    func testTheSignInLinkOutlivesTheRedrawThatClearsIt() async {
        let rig = rig()
        await started(rig, cols: 40, rows: 10)
        rig.output("Open https://claude.ai/oauth/authorize?x=1\r\n")
        await settle()
        XCTAssertEqual(rig.terminal.signInLink, "https://claude.ai/oauth/authorize?x=1")
        rig.output("\u{1b}[2J\u{1b}[HPaste code here if prompted >")
        await settle()
        XCTAssertTrue(rig.terminal.links.isEmpty)
        XCTAssertEqual(rig.terminal.signInLink, "https://claude.ai/oauth/authorize?x=1")
        await rig.terminal.close()
    }

    func testAResizeKeepsTheScreenAndTheNodeRedrawsOverIt() async {
        let rig = rig()
        await started(rig, cols: 40, rows: 10)
        rig.output("Sign in:\r\nhttps://claude.ai/oauth/authorize?x=1\r\n> ")
        await settle()
        rig.terminal.resize(cols: 40, rows: 7)
        XCTAssertEqual(rig.terminal.screen.grid[0].text, "Sign in:")
        XCTAssertEqual(rig.terminal.screen.cy, 2)
        XCTAssertEqual(rig.terminal.links, ["https://claude.ai/oauth/authorize?x=1"])
        await rig.terminal.close()
    }

    func testAReOpenReplaysIntoAFreshScreenNotAfterTheOldOne() async {
        let session = "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix"
        let rig = rig()
        await started(rig, cols: 20, rows: 5)
        rig.output(session)
        await settle()
        XCTAssertFalse(rig.terminal.screen.scrollback.isEmpty)

        // The socket came back after a trip to the browser, and the node
        // replays the whole session from the top.
        rig.socket.serverClose()
        await settle()
        rig.channel.appLifecycleChanged(.resumed)
        _ = await rig.sockets.waitForConnections(2)
        _ = await rig.socket.waitForSent(1)
        await settle()
        XCTAssertEqual(rig.terminalFrames.map { $0["action"] }, ["open"])
        rig.fromNode(["action": "ready"])
        rig.output(session)
        await settle()
        XCTAssertEqual(rig.terminal.screen.allRows.map(\.text).joined(separator: "\r\n"), session)
        await rig.terminal.close()
    }

    func testAResizeIsSentOnceTheGridChanges() async {
        let rig = rig()
        await started(rig, cols: 40, rows: 10)
        rig.terminal.resize(cols: 40, rows: 10)
        rig.terminal.resize(cols: 50, rows: 10)
        await settle()
        let resizes = rig.terminalFrames.filter { $0["action"] == "resize" }
        XCTAssertEqual(resizes.count, 1)
        XCTAssertEqual(resizes.first?["cols"], 50)
        await rig.terminal.close()
    }

    func testTheKeyBarSequenceReachesTheNodeInOrder() async {
        // The widget test: an arrow key, then typed text sent with Enter as
        // its own frame.
        let rig = rig()
        await started(rig, cols: 40, rows: 10)
        rig.terminal.key(.down)
        await settle()
        rig.terminal.paste("abc")
        rig.terminal.key(.enter)
        await settle()
        XCTAssertEqual(decodedInputs(rig).joined(), "\u{1b}[Babc\r")
        await rig.terminal.close()
    }
}
