import XCTest
@testable import FMAgents

final class PromptFixTests: XCTestCase {
    func testFixLeadsWithTheAskAndKeepsWhatWasTyped() {
        XCTAssertEqual(PromptFix.prompt("the login button overlaps the logo"), "Please fix: the login button overlaps the logo")
        XCTAssertEqual(PromptFix.prompt("  crash on save\n"), "Please fix: crash on save")
        XCTAssertEqual(PromptFix.prompt("line one\nline two"), "Please fix: line one\nline two")
    }

    func testFixWithOnlyPhotosSendsTheAskAlone() {
        XCTAssertEqual(PromptFix.prompt(""), "Please fix")
        XCTAssertEqual(PromptFix.prompt(" \n "), "Please fix")
    }
}
