package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RecycleBinTest {
    private fun binJson(ids: List<Int>, retention: Int = 24, extra: Map<String, Any?> = emptyMap()): Map<String, Any?> = linkedMapOf<String, Any?>(
        "agents" to ids.map { id ->
            mapOf(
                "id" to id.toLong(), "title" to "Agent $id", "owner" to "me", "name" to "app", "branch" to "agent/$id",
                "provider" to "claude-code", "deletedAtMs" to 1_000L, "removeInMs" to id * 3_600_000L,
            )
        } + listOf(mapOf("id" to 0L, "title" to "junk")),
        "count" to ids.size.toLong(),
        "truncated" to false,
        "retentionHours" to retention.toLong(),
        "minHours" to 1L,
        "maxHours" to 720L,
    ).apply { putAll(extra) }

    fun testBinIsParsedAndJunkRowsDropped() {
        val channel = FakeControlChannel()
        channel.reply("agentops.bin", binJson(listOf(5, 2)))
        val bin = AgentOpsClient(channel).recycleBin()
        assertEq(listOf(5, 2), bin.ids)
        assertEq("me/app", bin.agents[0].repo)
        assertEq("Removed for good in 5 h", bin.agents[0].removal)
        assertEq(24, bin.retentionHours)
        assertEq("Removed for good in under an hour", RecycledAgent(1, removeInMs = 60_000).removal)
        assertEq("Removed for good at the next sweep", RecycledAgent(1).removal)
        assertEq("Agent #9", RecycledAgent(9).headline)
    }

    fun testRestoreReportsWhatCameBack() {
        val channel = FakeControlChannel()
        channel.reply("agentops.bin", binJson(listOf(5, 2, 1)))
        channel.reply("agentops.bin.restore", binJson(listOf(2), extra = mapOf("restored" to listOf(1L, 5L), "failed" to emptyList<Long>())))
        val model = RecycleBinModel(AgentOpsClient(channel))
        model.refresh()
        model.selectAll()
        assertEq(setOf(1, 2, 5), model.selectedIds.toSet())
        expect(model.restore(listOf(5, 1)))
        assertEq(listOf(1L, 5L), channel.params("agentops.bin.restore")!!["ids"])
        assertEq(listOf(2), model.bin.ids)
        assertEq(listOf(2), model.selectedIds, "restored agents leave the selection")
        assertEq("2 agents restored.", model.note)
    }

    fun testPurgeNeedsTheDesktopsConfirmation() {
        val channel = FakeControlChannel()
        channel.reply("agentops.bin", binJson(listOf(3, 4)))
        channel.reply("agentops.prepare", confirmationJson("purge", token = "bin-tok"))
        channel.reply("agentops.bin.purge", binJson(emptyList(), extra = mapOf("purged" to listOf(3L, 4L), "skipped" to emptyList<Long>())))
        val model = RecycleBinModel(AgentOpsClient(channel))
        model.refresh()
        expect(!model.purge(), "nothing is purged before a confirmation")
        model.preparePurge(model.bin.ids)
        assertEq("purge", channel.params("agentops.prepare").str("action"))
        assertEq(listOf(3L, 4L), channel.params("agentops.prepare")!!["ids"])
        assertEq(AgentOpsAction.Purge, model.confirmation?.action)
        expect(model.purge())
        assertEq("bin-tok", channel.params("agentops.bin.purge").str("token"))
        expect(model.bin.isEmpty)
        expect(model.confirmation == null)
        assertEq("2 agents deleted for good.", model.note)
        expect(AgentOpsAction.Purge.isDestructive)
    }

    fun testOldDesktopSaysUpdate() {
        val channel = FakeControlChannel()
        channel.failures["agentops.bin"] = LocalControlException("unsupported")
        val model = RecycleBinModel(AgentOpsClient(channel))
        model.refresh()
        expect(model.unsupported)
        expect(model.errorText.contains("Update ForkMesh"))
    }

    fun testRetentionIsClampedAndSent() {
        val channel = FakeControlChannel()
        channel.reply("agentops.bin", binJson(listOf(1)))
        channel.reply("agentops.bin.retention", binJson(listOf(1), retention = 720))
        val model = RecycleBinModel(AgentOpsClient(channel))
        model.refresh()
        model.setRetention(5_000)
        assertEq(720, channel.params("agentops.bin.retention").int("hours"))
        assertEq(720, model.bin.retentionHours)
    }

    fun testChangeMessages() {
        assertEq("Agent restored.", RecycleBinChange.Restored(listOf(1), emptyList()).message)
        assertEq(
            "Could not restore 1 agent; a live agent may already use that id.",
            RecycleBinChange.Restored(emptyList(), listOf(2)).message,
        )
        assertEq("1 agent deleted for good. 2 were already gone.", RecycleBinChange.Purged(listOf(1), listOf(2, 3)).message)
    }
}
