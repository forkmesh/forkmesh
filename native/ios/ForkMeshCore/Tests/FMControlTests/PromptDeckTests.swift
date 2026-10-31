import XCTest

@testable import FMControl

final class PromptDeckTests: XCTestCase {
    func testTheNewestPromptIsOnTop() {
        let deck = PromptDeck(blocks: [
            .prompt("Fix the login page"),
            .text("Done."),
            .steer(from: "ForkMesh (phone)", text: "  Also add a test \n"),
            .steer(from: "reviewer", text: "Not you"),
            .prompt("   "),
        ])
        XCTAssertEqual(deck.prompts, ["Also add a test", "Fix the login page"])
        XCTAssertEqual(deck.prompt(at: 0), "Also add a test")
    }

    func testScrollingBackStopsAtTheFirstPrompt() {
        let deck = PromptDeck(said: ["one", "two", "three"])
        XCTAssertEqual(deck.position(0), "3/3")
        XCTAssertEqual(deck.older(than: 0), 1)
        XCTAssertEqual(deck.older(than: 2), 2)
        XCTAssertEqual(deck.position(2), "1/3")
        XCTAssertEqual(deck.newer(than: 0), 0)
        XCTAssertEqual(deck.prompt(at: 9), "one")
    }

    func testTheStackShowsUpToTwoCardsBehind() {
        let deck = PromptDeck(said: ["one", "two", "three", "four"])
        XCTAssertEqual(deck.behind(0), 2)
        XCTAssertEqual(deck.behind(2), 1)
        XCTAssertEqual(deck.behind(3), 0)
        XCTAssertEqual(PromptDeck(said: ["only"]).position(0), "")
    }

    func testTheHeadlineIsOneLine() {
        let deck = PromptDeck(said: ["Fix\n\nthe   login\tpage"])
        XCTAssertEqual(deck.headline(at: 0), "Fix the login page")
        XCTAssertEqual(PromptDeck(said: []).headline(at: 0), "")
    }
}
