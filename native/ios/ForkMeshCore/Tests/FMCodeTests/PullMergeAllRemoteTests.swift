import FMBase
import FMChat
import FMControl
import XCTest

@testable import FMCode

final class PullMergeAllRemoteTests: XCTestCase {

    @MainActor func testMergeAllIsTokenConfirmed() async throws {
        let channel = FakeMergeQueueChannel()
        channel.replies["pulls.prepare"] = ["token": "T1", "summary": "Start an AI agent", "expiresInMs": 60000]
        channel.replies["pulls.run"] = [
            "message": "Agent #9 is reviewing and merging 2 pull request(s).", "sessionId": 9, "pulls": 2,
            "skipped": ["#5 (its code is not on this node)"],
        ]
        let remote = PullMergeAllRemote(channel: channel)
        let confirmation = try await remote.prepare(owner: "o", name: "r")
        XCTAssertEqual(confirmation.summary, "Start an AI agent")
        let result = try await remote.run(owner: "o", name: "r", token: confirmation.token)
        XCTAssertEqual(channel.requests.map(\.0), ["pulls.prepare", "pulls.run"])
        XCTAssertEqual(channel.requests[0].1.str("action"), "merge-all-ai")
        XCTAssertEqual(channel.requests[0].1.str("owner"), "o")
        XCTAssertEqual(channel.requests[0].1.str("name"), "r")
        XCTAssertNil(channel.requests[0].1["token"])
        XCTAssertEqual(channel.requests[1].1.str("token"), "T1")
        XCTAssertEqual(result.sessionId, 9)
        XCTAssertEqual(result.pulls, 2)
        XCTAssertEqual(result.skipped, ["#5 (its code is not on this node)"])
    }

    @MainActor func testRefusalsReadAsSentences() async {
        let channel = FakeMergeQueueChannel()
        channel.failures["pulls.prepare"] = "unknown_method"
        let remote = PullMergeAllRemote(channel: channel)
        do {
            _ = try await remote.prepare(owner: "o", name: "r")
            XCTFail("expected a refusal")
        } catch {
            XCTAssertTrue(PullMergeAllRemote.message(for: error).contains("Update ForkMesh"))
        }
        channel.failures = [:]
        channel.replies["pulls.prepare"] = [:]
        do {
            _ = try await remote.prepare(owner: "o", name: "r")
            XCTFail("expected a missing token")
        } catch {
            XCTAssertEqual(PullMergeAllRemote.message(for: error), "The confirmation expired. Try again.")
        }
        XCTAssertEqual(PullMergeAllRules.errorMessage("busy"), "An agent is already merging this repository's pull requests.")
        XCTAssertEqual(PullMergeAllRules.errorMessage("not_repo_owner").prefix(4), "Only")
    }

    func testPullsFamilyOwnsItsMethods() {
        XCTAssertEqual(DesktopControlFamilyName.owning("pulls.prepare"), .pulls)
        XCTAssertEqual(DesktopControlFamilyName.owning("mergequeue.run"), .mergeQueue)
    }
}
