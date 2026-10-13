import Foundation
import XCTest

@testable import FMApi

final class MessageHandoffTests: XCTestCase {
    func testThePromptQuotesTheMessageAndAsksForAFix() {
        let prompt = MessageHandoff.agentPrompt("  Assigned tasks could not be loaded.\n")
        XCTAssertEqual(prompt, "The ForkMesh iPhone app showed this message:\n\n> Assigned tasks could not be loaded.\n\nFind out what caused it and fix it.")
    }

    func testEveryLineOfAMultiLineMessageIsQuoted() {
        let prompt = MessageHandoff.agentPrompt("Pings could not be loaded.\n\nAssigned tasks could not be loaded.")
        XCTAssertTrue(prompt.contains("> Pings could not be loaded.\n> \n> Assigned tasks could not be loaded."))
    }

    func testABlankMessageCannotBeHandedOff() {
        XCTAssertFalse(MessageHandoff.canHandOff(" \n "))
        XCTAssertTrue(MessageHandoff.canHandOff("Offline"))
    }

    func testALongMessageIsCut() {
        let long = String(repeating: "x", count: MessageHandoff.maxMessageLength + 50)
        XCTAssertEqual(MessageHandoff.cleaned(long).count, MessageHandoff.maxMessageLength + 1)
        XCTAssertTrue(MessageHandoff.cleaned(long).hasSuffix("…"))
    }

    func testTheComposeTargetCarriesOnlyThePrompt() {
        let target = MessageHandoff.composeTarget("Offline")
        XCTAssertEqual(target.text, MessageHandoff.agentPrompt("Offline"))
        XCTAssertNil(target.repo)
        XCTAssertNil(target.agentId)
    }
}
