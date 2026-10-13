import FMApi
import FMBase
import Foundation
import XCTest

@testable import FMAgents

final class RemoteApprovalNotificationTests: XCTestCase {
    func testParsesTheServerAlert() {
        let payload: JSONObject = [
            "aps": ["category": "agent-approval"], "approvalId": .string(approvalA), "sessionId": "12",
        ]
        let note = AgentApprovalNotification(payload: payload)
        XCTAssertEqual(note?.approvalId, approvalA)
        XCTAssertEqual(note?.sessionId, "12")
        XCTAssertEqual(note?.route, .approval(id: approvalA))
    }

    func testIgnoresOtherCategoriesAndMissingIds() {
        XCTAssertNil(AgentApprovalNotification(payload: ["category": "ping", "approvalId": "x"]))
        XCTAssertNil(AgentApprovalNotification(payload: ["category": "agent-approval"]))
        XCTAssertNil(AgentApprovalNotification(payload: [
            "category": "agent-approval", "approvalId": .string(String(repeating: "x", count: 65)),
        ]))
        XCTAssertNotNil(AgentApprovalNotification(payload: ["alert": ["category": "agent-approval", "approvalId": "a"]]))
    }

    func testActionsFollowNavigation() {
        XCTAssertTrue(AgentApprovalNotification.Action.approve.requiresAuthentication)
        XCTAssertFalse(AgentApprovalNotification.Action.deny.requiresAuthentication)
        XCTAssertTrue(AgentApprovalNotification.Action.deny.isDestructive)
        XCTAssertTrue(AgentApprovalNotification.Action.open.opensApp)
    }

    @MainActor func testApproveFromTheNotificationAnswersWithoutOpening() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("approve"))
        let queue = ApprovalsQueue(client: AgentOpsClient(channel: channel))
        let note = AgentApprovalNotification(approvalId: approvalA)
        let route = await note.perform(.approve, queue: queue)
        XCTAssertNil(route)
        XCTAssertEqual(channel.params("agentops.answer")?.str("decision"), "allow")
        XCTAssertEqual(channel.params("agentops.answer")?.str("lifetime"), "once")
    }

    @MainActor func testFailedAnswerOpensTheApproval() async {
        let channel = FakeAgentOpsChannel()
        channel.failures["agentops.prepare"] = FakeControlError(code: "not_connected")
        let queue = ApprovalsQueue(client: AgentOpsClient(channel: channel))
        let route = await AgentApprovalNotification(approvalId: approvalA).perform(.deny, queue: queue)
        XCTAssertEqual(route, .approval(id: approvalA))
    }
}
