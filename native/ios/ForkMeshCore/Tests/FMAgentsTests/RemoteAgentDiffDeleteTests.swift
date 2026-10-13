import FMBase
import FMCode
import FMControl
import Foundation
import XCTest

@testable import FMAgents

final class RemoteAgentDiffDeleteTests: XCTestCase {
    @MainActor func testDiffIsReadAndSplitIntoLines() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply(
            "agentops.diff",
            [
                "id": 7, "branch": "fm/fix", "base": "main", "merged": false,
                "files": [
                    ["path": "a.cpp", "added": 2, "removed": 1, "binary": false],
                    ["path": "img.png", "added": 0, "removed": 0, "binary": true],
                ],
                "committed": "diff --git a/a.cpp b/a.cpp\n@@ -1 +1,2 @@\n-old\n+new\n+more\n ctx\n",
                "uncommitted": "", "untracked": ["notes.txt"], "hasWorktree": true, "truncated": true,
            ])
        let model = AgentDiffModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        await model.load()
        XCTAssertEqual(channel.params("agentops.diff")?["id"], 7)
        let diff = try XCTUnwrap(model.diff)
        XCTAssertEqual(diff.branch, "fm/fix")
        XCTAssertEqual(diff.added, 2)
        XCTAssertEqual(diff.removed, 1)
        XCTAssertTrue(diff.files[1].binary)
        XCTAssertEqual(diff.untracked, ["notes.txt"])
        XCTAssertTrue(diff.truncated)
        XCTAssertFalse(diff.isEmpty)
        XCTAssertEqual(
            AgentDiff.lines(diff.committed).map(\.kind),
            [.header, .hunk, .removed, .added, .added, .context])
        let review = try XCTUnwrap(model.review)
        XCTAssertEqual(review.scope, "agent/fm/fix")
        XCTAssertEqual(review.sections.map(\.title), ["Committed on the branch"])
        XCTAssertEqual(review.files.map(\.file.path), ["a.cpp"])
        XCTAssertEqual(review.additions, 2)
    }

    @MainActor func testImageBytesAreReadFromTheRightSide() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply(
            "agentops.diff",
            [
                "id": 7, "branch": "fm/fix", "base": "main",
                "committed": "diff --git a/logo.png b/logo.png\nBinary files a/logo.png and b/logo.png differ\n",
                "uncommitted": "",
            ])
        channel.reply("agentops.file", ["path": "logo.png", "mime": "image/png", "size": 3, "data": "AQID"])
        let model = AgentDiffModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        await model.load()
        let review = try XCTUnwrap(model.review)
        XCTAssertTrue(review.rows.contains(.preview(0)))
        await review.loadImages(0)
        let params = try XCTUnwrap(channel.params("agentops.file"))
        XCTAssertEqual(params["id"], 7)
        XCTAssertEqual(params.str("path"), "logo.png")
        XCTAssertEqual(params.str("change"), "committed")
        XCTAssertEqual(review.imageResults[review.imageRequests(0)[1]], .loaded(Data([1, 2, 3])))
    }

    @MainActor func testDiffOnAnOldDesktopSaysToUpdate() async {
        let channel = FakeAgentOpsChannel()
        channel.failures["agentops.diff"] = AgentOpsError("unsupported")
        let model = AgentDiffModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        await model.load()
        XCTAssertNil(model.diff)
        XCTAssertTrue(model.errorText.contains("Update ForkMesh"))
    }

    @MainActor func testDeletePreparesThenSpendsTheToken() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("delete", token: "tok-del"))
        channel.reply("agentops.delete", [
            "id": 7, "settled": true,
            "parts": [
                ["kind": "agent", "status": "deleted"], ["kind": "branch", "name": "agent/7-x", "status": "deleted"],
                ["kind": "work", "name": "/w/7", "status": "deleted"], ["kind": "issue", "name": "12", "status": "closed"],
            ],
        ])
        let model = AgentDeleteModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        await model.prepare()
        XCTAssertEqual(channel.params("agentops.prepare")?.str("action"), "delete")
        XCTAssertEqual(channel.params("agentops.prepare")?["id"], 7)
        XCTAssertEqual(model.confirmation?.action, .delete)
        let ran = await model.run()
        XCTAssertTrue(ran)
        XCTAssertTrue(model.deleted)
        XCTAssertEqual(channel.params("agentops.delete")?.str("token"), "tok-del")
        XCTAssertEqual(
            model.report?.summary(place: "on the desktop"),
            "Deleted the agent on the desktop with branch agent/7-x and its worktree. Closed issue #12. "
                + "No pull request or task was attached.")
        let again = await model.run()
        XCTAssertFalse(again, "a token is spent once")
        XCTAssertTrue(AgentOpsAction.delete.isDestructive)
    }

    @MainActor func testADeleteTheDesktopKeptIsAFailureThatSaysWhy() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("delete", token: "tok-del"))
        channel.reply("agentops.delete", [
            "id": 7, "settled": true,
            "parts": [
                ["kind": "agent", "status": "kept"],
                ["kind": "task", "name": "t1", "status": "kept", "reason": "relay down"],
            ],
        ])
        let model = AgentDeleteModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        await model.prepare()
        let ran = await model.run()
        XCTAssertFalse(ran)
        XCTAssertFalse(model.deleted)
        XCTAssertTrue(model.errorText.contains("Kept the agent."))
        XCTAssertTrue(model.errorText.contains("Kept task t1: relay down."))
    }

    @MainActor func testAnOldDesktopAnswersWithoutAReport() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("delete", token: "tok-del"))
        channel.reply("agentops.delete", ["id": 7])
        let model = AgentDeleteModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        await model.prepare()
        let ran = await model.run()
        XCTAssertTrue(ran)
        XCTAssertNil(model.report)
    }

    @MainActor func testDeleteOfARunningAgentShowsTheDesktopsReason() async {
        let channel = FakeAgentOpsChannel()
        channel.failures["agentops.prepare"] = AgentOpsError("agent_running")
        let model = AgentDeleteModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        await model.prepare()
        XCTAssertNil(model.confirmation)
        XCTAssertEqual(model.errorText, AgentOpsClient.describe(code: "agent_running"))
    }
}
