import FMAgentWatch
import FMApi
import FMBase
import XCTest

@testable import FMAgents

final class AgentWatchTargetsTests: XCTestCase {
    func testBeatsFromAgentEventsCoverARunOnThePhone() {
        var args = JSONObject()
        args["path"] = .string("a")
        let beats = beatsFromAgentEvents([
            .userMessage("add a test"),
            .toolCall(AgentToolCall(id: "1", name: "read_file", arguments: args)),
            .toolResult(AgentToolResult(callId: "1", name: "read_file", output: "x")),
            .toolResult(AgentToolResult(callId: "2", name: "grep", output: "no", isError: true)),
            .toolResult(AgentToolResult(callId: "3", name: "bash", output: "Permission denied", isError: true)),
            .assistantMessage("Done."),
            .done(AgentDone(reason: .completed, turns: 2, inputTokens: 1, outputTokens: 1)),
        ])
        XCTAssertEqual(beats.map(\.motion), [.prompt, .read, .arrive, .fumble, .denied, .speak, .finish])
        XCTAssertEqual(beats[1].text, "read_file a")
        XCTAssertEqual(beats.last?.text, "completed after 2 turns")
        XCTAssertEqual(beats[5].said, "Done.")
    }

    func testLocalNetworkTargetPrefersTheListsStatus() async throws {
        let session = AgentSession(id: 9, status: "running")
        let target = localNetworkWatchTarget(
            repoLabel: "me/app", name: "app", title: "fix", prompt: "fix it", session: session,
            transcript: { _ in "[Bash ls]\n" }, currentStatus: { _ in "done" })
        XCTAssertEqual(target.key, "lan:me/app#9")
        XCTAssertEqual(target.projectName, "app")
        let snapshot = try await target.load()
        XCTAssertEqual(snapshot.status, "done")
        XCTAssertEqual(snapshot.beats.map(\.motion), [.run])
    }
}
