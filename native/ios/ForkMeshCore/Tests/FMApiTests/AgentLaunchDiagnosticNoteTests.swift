import XCTest
@testable import FMApi

final class AgentLaunchDiagnosticNoteTests: XCTestCase {
    func testAFailedRunShowsItsErrorOnce() {
        var session = AgentLaunchSession(sessionId: "s", status: "failed")
        session.diagnostic = "Claude Code exited with status 1."
        session.lastError = "  Claude Code exited with status 1.\n"
        XCTAssertEqual(session.diagnosticNote, "")
        session.lastError = "Something else"
        XCTAssertEqual(session.diagnosticNote, "Claude Code exited with status 1.")
        session.diagnostic = "   "
        XCTAssertEqual(session.diagnosticNote, "")
    }
}
