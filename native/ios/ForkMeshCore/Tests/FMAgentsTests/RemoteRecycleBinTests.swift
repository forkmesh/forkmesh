import FMBase
import FMControl
import Foundation
import XCTest

@testable import FMAgents

final class RemoteRecycleBinTests: XCTestCase {
    private func binJSON(_ ids: [Int], retention: Int = 24) -> JSONObject {
        [
            "agents": .array(ids.map { id in
                .object([
                    "id": .int(Int64(id)), "title": .string("Agent \(id)"), "owner": "me", "name": "app",
                    "branch": .string("agent/\(id)"), "provider": "claude-code", "deletedAtMs": 1_000,
                    "removeInMs": .int(Int64(id) * 3_600_000),
                ])
            } + [.object(["id": 0, "title": "junk"])]),
            "count": .int(Int64(ids.count)), "truncated": false, "retentionHours": .int(Int64(retention)),
            "minHours": 1, "maxHours": 720,
        ]
    }

    @MainActor func testBinIsParsedAndJunkRowsDropped() async throws {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.bin", binJSON([5, 2]))
        let bin = try await AgentOpsClient(channel: channel).recycleBin()
        XCTAssertEqual(bin.ids, [5, 2])
        XCTAssertEqual(bin.agents[0].repo, "me/app")
        XCTAssertEqual(bin.agents[0].removal, "Removed for good in 5 h")
        XCTAssertEqual(bin.retentionHours, 24)
        XCTAssertEqual(RecycledAgent(id: 1, removeInMs: 60_000).removal, "Removed for good in under an hour")
        XCTAssertEqual(RecycledAgent(id: 1).removal, "Removed for good at the next sweep")
        XCTAssertEqual(RecycledAgent(id: 9).headline, "Agent #9")
    }

    @MainActor func testRestoreReportsWhatCameBack() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.bin", binJSON([5, 2, 1]))
        var restored = binJSON([2])
        restored["restored"] = [1, 5]
        restored["failed"] = []
        channel.reply("agentops.bin.restore", restored)
        let model = RecycleBinModel(client: AgentOpsClient(channel: channel))
        await model.refresh()
        model.selectAll()
        XCTAssertEqual(Set(model.selectedIds), [1, 2, 5])
        let ok = await model.restore([5, 1])
        XCTAssertTrue(ok)
        XCTAssertEqual(channel.params("agentops.bin.restore")?["ids"], [1, 5])
        XCTAssertEqual(model.bin.ids, [2])
        XCTAssertEqual(model.selectedIds, [2], "restored agents leave the selection")
        XCTAssertEqual(model.note, "2 agents restored.")
    }

    @MainActor func testPurgeNeedsTheDesktopsConfirmation() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.bin", binJSON([3, 4]))
        channel.reply("agentops.prepare", confirmationJSON("purge", token: "bin-tok"))
        var after = binJSON([])
        after["purged"] = [3, 4]
        after["skipped"] = []
        channel.reply("agentops.bin.purge", after)
        let model = RecycleBinModel(client: AgentOpsClient(channel: channel))
        await model.refresh()
        let early = await model.purge()
        XCTAssertFalse(early, "nothing is purged before a confirmation")
        await model.preparePurge(model.bin.ids)
        XCTAssertEqual(channel.params("agentops.prepare")?.str("action"), "purge")
        XCTAssertEqual(channel.params("agentops.prepare")?["ids"], [3, 4])
        XCTAssertEqual(model.confirmation?.action, .purge)
        let ok = await model.purge()
        XCTAssertTrue(ok)
        XCTAssertEqual(channel.params("agentops.bin.purge")?.str("token"), "bin-tok")
        XCTAssertTrue(model.bin.isEmpty)
        XCTAssertNil(model.confirmation)
        XCTAssertEqual(model.note, "2 agents deleted for good.")
        XCTAssertTrue(AgentOpsAction.purge.isDestructive)
    }

    @MainActor func testOldDesktopSaysUpdate() async {
        let channel = FakeAgentOpsChannel()
        channel.failures["agentops.bin"] = FakeControlError(code: "unsupported")
        let model = RecycleBinModel(client: AgentOpsClient(channel: channel))
        await model.refresh()
        XCTAssertTrue(model.unsupported)
        XCTAssertTrue(model.errorText.contains("Update ForkMesh"))
    }

    @MainActor func testRetentionIsClampedAndSent() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.bin", binJSON([1]))
        channel.reply("agentops.bin.retention", binJSON([1], retention: 720))
        let model = RecycleBinModel(client: AgentOpsClient(channel: channel))
        await model.refresh()
        await model.setRetention(hours: 5_000)
        XCTAssertEqual(channel.params("agentops.bin.retention")?.int("hours"), 720)
        XCTAssertEqual(model.bin.retentionHours, 720)
    }

    func testChangeMessages() {
        XCTAssertEqual(RecycleBinChange.restored(done: [1], failed: []).message, "Agent restored.")
        XCTAssertEqual(
            RecycleBinChange.restored(done: [], failed: [2]).message,
            "Could not restore 1 agent; a live agent may already use that id.")
        XCTAssertEqual(
            RecycleBinChange.purged(done: [1], skipped: [2, 3]).message, "1 agent deleted for good. 2 were already gone.")
    }
}
