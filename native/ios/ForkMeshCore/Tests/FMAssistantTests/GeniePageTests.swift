import FMApi
import FMBase
import FMLocalAI
import XCTest
@testable import FMAssistant

@MainActor
private final class PageActions: GenieActions {
    var me = "ada"
    var paletteContext = PaletteContext()
    var diffOpen = true
    var turned: [GeniePageTurn] = []

    func run(_ command: PaletteCommand) {}

    func startAgent(prompt: String, repository: String?) async -> String {
        "Started an agent."
    }

    var canTurnPages: Bool { diffOpen }

    func turnPage(_ turn: GeniePageTurn) -> String? {
        turned.append(turn)
        return turn.title + "."
    }
}

@MainActor
private final class PlainActions: GenieActions {
    var me = "ada"
    var paletteContext = PaletteContext()

    func run(_ command: PaletteCommand) {}

    func startAgent(prompt: String, repository: String?) async -> String {
        "Started an agent."
    }
}

@MainActor
private final class PageVoice: GenieVoice {
    var overheard: [GenieHeard] = []
    var said: [String] = []

    func say(_ text: String) async {
        said.append(text)
    }

    func listen(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        .silence
    }

    func listenAmbient(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        guard !overheard.isEmpty else {
            try? await Task.sleep(for: .milliseconds(20))
            return .silence
        }
        let next = overheard.removeFirst()
        if case .words(let words) = next { onWords(words) }
        return next
    }

    func stopListening() {}
    func stopSpeaking() {}
}

@MainActor
final class GeniePageTests: XCTestCase {
    private func turn(_ text: String) -> String? {
        GenieSpotter.spot(text, paging: true).flatMap { $0.tool == GeniePages.tool ? $0.arguments["direction"] : nil }
    }

    func testSpotsPageTurnsWhileADiffIsOpen() {
        let cases: [(String, String?)] = [
            ("next page", "next"), ("Next page.", "next"), ("genie, next page", "next"), ("next page please", "next"),
            ("can you go to the next page", "next"), ("page down", "next"), ("scroll down a page", "next"),
            ("turn the page", "next"), ("next page genie", "next"), ("show me the next page", "next"),
            ("previous page", "previous"), ("go back a page", "previous"), ("page up", "previous"),
            ("next file", "next_file"), ("skip to the next file", "next_file"), ("previous file", "previous_file"),
            ("go back a file", "previous_file"),
            ("page", nil), ("next", nil), ("I read the next page of the book", nil), ("what's on the next page", nil),
            ("what's my next meeting", nil),
        ]
        for (said, expected) in cases {
            XCTAssertEqual(turn(said), expected, said)
        }
        XCTAssertNil(GenieSpotter.spot("next page"))
        XCTAssertEqual(GenieSpotter.spot("what's my next meeting", paging: true)?.tool, "read_latest")
    }

    func testAPageTurnRunsOnItsOwn() async throws {
        let spot = try XCTUnwrap(GenieSpotter.spot("next page", paging: true))
        XCTAssertTrue(spot.addressed)
        XCTAssertTrue(spot.runsAlone)
        XCTAssertEqual(spot.title, "Next page")
        XCTAssertEqual(spot.phrase, 0..<9)
    }

    func testDirectionsParse() {
        XCTAssertEqual(GeniePageTurn.parse("next"), .next)
        XCTAssertEqual(GeniePageTurn.parse("down"), .next)
        XCTAssertEqual(GeniePageTurn.parse("Previous"), .previous)
        XCTAssertEqual(GeniePageTurn.parse("back"), .previous)
        XCTAssertEqual(GeniePageTurn.parse("next_file"), .nextFile)
        XCTAssertEqual(GeniePageTurn.parse("previous file"), .previousFile)
        XCTAssertNil(GeniePageTurn.parse("sideways"))
    }

    func testToolboxTurnsThePageOnScreen() async {
        let actions = PageActions()
        let toolbox = GenieToolbox(data: nil, actions: actions)
        XCTAssertTrue(toolbox.canTurnPages)
        XCTAssertEqual(toolbox.spot("next page")?.tool, GeniePages.tool)
        let next = await toolbox.invoke(LocalToolCall(id: "1", name: "next_page", arguments: [:]))
        XCTAssertEqual(next.text, "Next page.")
        XCTAssertTrue(toolbox.navigated)
        let back = await toolbox.invoke(LocalToolCall(id: "2", name: "turn_page", arguments: ["way": "previous_file"]))
        XCTAssertEqual(back.text, "Previous file.")
        let odd = await toolbox.invoke(LocalToolCall(id: "3", name: "turn_page", arguments: ["direction": "sideways"]))
        XCTAssertTrue(odd.text.hasPrefix("turn_page needs a direction"), odd.text)
        XCTAssertEqual(actions.turned, [.next, .previousFile])
        let approval = await toolbox.approval(for: LocalToolCall(id: "4", name: "turn_page", arguments: [:]))
        XCTAssertNil(approval)
        let up = await toolbox.invoke(LocalToolCall(id: "7", name: "previous_page", arguments: [:]))
        XCTAssertEqual(up.text, "Previous page.")

        actions.diffOpen = false
        XCTAssertNil(toolbox.spot("next page"))
        let closed = await toolbox.invoke(LocalToolCall(id: "5", name: "turn_page", arguments: ["direction": "next"]))
        XCTAssertEqual(closed.text, GeniePages.nothingOpen)
        XCTAssertEqual(actions.turned, [.next, .previousFile, .previous])

        let plain = GenieToolbox(data: nil, actions: PlainActions())
        XCTAssertFalse(plain.canTurnPages)
        let none = await plain.invoke(LocalToolCall(id: "6", name: "turn_page", arguments: [:]))
        XCTAssertEqual(none.text, GeniePages.nothingOpen)
    }

    func testBrainTurnsThePageWithoutAModel() async {
        let actions = PageActions()
        let brain = GenieBrain(toolbox: GenieToolbox(data: nil, actions: actions), memory: GenieMemory(store: nil), model: nil)
        let outcome = await brain.begin("next page")
        XCTAssertEqual(outcome, .reply("Next page."))
        XCTAssertEqual(actions.turned, [.next])
        XCTAssertEqual(brain.steps.map(\.tool), [GeniePages.tool])
    }

    func testAlwaysListeningTurnsPagesOnlyOverADiff() async {
        let actions = PageActions()
        let voice = PageVoice()
        voice.overheard = [.words("next page"), .words("go back a page")]
        let session = GenieSession(
            brain: GenieBrain(toolbox: GenieToolbox(data: nil, actions: actions), memory: GenieMemory(store: nil), model: nil),
            voice: voice
        ) { _ in "" }
        var spotted: [String] = []
        session.onSpot = { spot, runs in spotted.append("\(spot.arguments["direction"] ?? spot.tool):\(runs)") }
        session.setAlwaysListening(true)
        let deadline = Date().addingTimeInterval(3)
        while voice.said.count < 2 && Date() < deadline {
            try? await Task.sleep(for: .milliseconds(10))
        }
        await session.waitUntilIdle()
        session.setAlwaysListening(false)
        XCTAssertEqual(actions.turned, [.next, .previous])
        XCTAssertEqual(voice.said, ["Next page.", "Previous page."])
        XCTAssertEqual(spotted, ["next:true", "previous:true"])

        actions.diffOpen = false
        voice.overheard = [.words("next page")]
        spotted = []
        session.setAlwaysListening(true)
        let quiet = Date().addingTimeInterval(0.3)
        while !voice.overheard.isEmpty && Date() < quiet {
            try? await Task.sleep(for: .milliseconds(10))
        }
        await session.waitUntilIdle()
        session.setAlwaysListening(false)
        XCTAssertEqual(actions.turned, [.next, .previous])
        XCTAssertEqual(spotted, [])
    }

    func testTheModelSeesTheToolForAPageRequest() {
        let picked = GenieToolText.pick(GenieToolbox.specs, for: "flip forward a page in this diff").map(\.name)
        XCTAssertTrue(picked.contains(GeniePages.tool), picked.joined(separator: ", "))
        XCTAssertEqual(GenieCapabilities.ability("diffs")?.tools, [GeniePages.tool])
    }
}
