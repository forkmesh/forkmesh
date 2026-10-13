import FMBase
import XCTest
@testable import FMAgents

final class AgentDeleteReportTests: XCTestCase {
    func testEverythingAttachedIsNamed() {
        let report = AgentDeleteReport(parts: [
            AgentDeletePart(kind: "agent", status: "deleted"),
            AgentDeletePart(kind: "branch", name: "agent/9-x", status: "deleted"),
            AgentDeletePart(kind: "work", name: "/w/9", status: "deleted"),
            AgentDeletePart(kind: "issue", name: "40", status: "closed"),
            AgentDeletePart(kind: "pull", name: "8", status: "deleted"),
            AgentDeletePart(kind: "task", name: "ab12", status: "deleted"),
        ])
        XCTAssertEqual(
            report.summary(place: "on the desktop"),
            "Deleted the agent on the desktop with branch agent/9-x, its worktree, pull request #8 and task ab12. "
                + "Closed issue #40.")
    }

    func testNothingAttachedIsSaid() {
        let report = AgentDeleteReport(parts: [AgentDeletePart(kind: "agent", status: "deleted")])
        XCTAssertEqual(report.summary(), "Deleted the agent. No branch, issue, pull request or task was attached.")
    }

    func testKeptAndPendingPartsSayWhy() {
        let report = AgentDeleteReport(
            parts: [
                AgentDeletePart(kind: "agent", status: "deleted"),
                AgentDeletePart(kind: "branch", name: "b", status: "pending"),
                AgentDeletePart(kind: "pull", name: "3", status: "kept", reason: "merged"),
                AgentDeletePart(kind: "task", name: "t", status: "kept", reason: "owner_only"),
                AgentDeletePart(kind: "issue", name: "5", status: "absent"),
            ], settled: false)
        XCTAssertEqual(
            report.summary(),
            "Deleted the agent. Already gone: issue #5. Kept pull request #3: it was merged. "
                + "Kept task t: only an organization owner can delete it. Still deleting branch b.")
        XCTAssertFalse(report.agentKept)
    }

    func testTheDesktopsAnswerParses() {
        let json: JSONObject = [
            "id": 4, "settled": false,
            "parts": [["kind": "agent", "status": "kept"], ["kind": "", "status": "deleted"], ["kind": "branch"]],
        ]
        let report = AgentDeleteReport(json: json)
        XCTAssertEqual(report?.parts, [AgentDeletePart(kind: "agent", status: "kept")])
        XCTAssertEqual(report?.settled, false)
        XCTAssertEqual(report?.agentKept, true)
        XCTAssertNil(AgentDeleteReport(json: ["id": 4]))
    }
}
