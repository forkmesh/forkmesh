import XCTest

@testable import FMControl

final class PendingPromptsTests: XCTestCase {
    func testASentPromptShowsUntilTheTranscriptCarriesIt() {
        var pending = PendingPrompts()
        let before: [TranscriptBlock] = [.prompt("Fix the login page"), .text("Working on it.")]
        pending.add("  Also add a test \n", shown: before)
        XCTAssertEqual(pending.blocks, [.prompt("Also add a test")])
        pending.settle(before + [.text("Still working.")])
        XCTAssertEqual(pending.texts, ["Also add a test"])
        pending.settle(before + [.prompt("Also add a test")])
        XCTAssertTrue(pending.isEmpty)
    }

    func testARepeatedPromptWaitsForItsOwnEcho() {
        var pending = PendingPrompts()
        let before: [TranscriptBlock] = [.prompt("continue"), .text("Done.")]
        pending.add("continue", shown: before)
        pending.settle(before)
        XCTAssertEqual(pending.texts, ["continue"])
        pending.settle(before + [.steer(from: "ForkMesh (phone)", text: "continue")])
        XCTAssertTrue(pending.isEmpty)
    }

    func testAFailedSendIsTakenBack() {
        var pending = PendingPrompts()
        let id = pending.add("Ship it", shown: [])
        pending.add("", shown: [])
        XCTAssertEqual(pending.texts, ["Ship it"])
        pending.remove(id)
        XCTAssertTrue(pending.isEmpty)
    }
}
