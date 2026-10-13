package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.testing.assertEq

class AgentDeleteReportTest {
    fun testEverythingAttachedIsNamed() {
        val report = AgentDeleteReport(
            listOf(
                AgentDeletePart("agent", status = "deleted"),
                AgentDeletePart("branch", "agent/9-x", "deleted"),
                AgentDeletePart("work", "/w/9", "deleted"),
                AgentDeletePart("issue", "40", "closed"),
                AgentDeletePart("pull", "8", "deleted"),
                AgentDeletePart("task", "ab12", "deleted"),
            ),
        )
        assertEq(
            "Deleted the agent on the desktop with branch agent/9-x, its worktree, pull request #8 and task ab12. Closed issue #40.",
            report.summary("on the desktop"),
        )
    }

    fun testNothingAttachedIsSaid() {
        val report = AgentDeleteReport(listOf(AgentDeletePart("agent", status = "deleted")))
        assertEq("Deleted the agent. No branch, issue, pull request or task was attached.", report.summary())
    }

    fun testKeptAndPendingPartsSayWhy() {
        val report = AgentDeleteReport(
            listOf(
                AgentDeletePart("agent", status = "deleted"),
                AgentDeletePart("branch", "b", "pending"),
                AgentDeletePart("pull", "3", "kept", "merged"),
                AgentDeletePart("task", "t", "kept", "owner_only"),
                AgentDeletePart("issue", "5", "absent"),
            ),
            settled = false,
        )
        assertEq(
            "Deleted the agent. Already gone: issue #5. Kept pull request #3: it was merged. " +
                "Kept task t: only an organization owner can delete it. Still deleting branch b.",
            report.summary(),
        )
        assertEq(false, report.agentKept)
    }

    fun testTheDesktopsAnswerParsesAndAKeptAgentFailsTheDelete() {
        val channel = FakeControlChannel()
        channel.reply("agentops.prepare", confirmationJson("delete"))
        channel.reply(
            "agentops.delete",
            mapOf(
                "id" to 7L,
                "settled" to true,
                "parts" to listOf(
                    mapOf("kind" to "agent", "status" to "kept"),
                    mapOf("kind" to "task", "name" to "t1", "status" to "kept", "reason" to "relay down"),
                    mapOf("kind" to "", "status" to "deleted"),
                ),
            ),
        )
        val model = AgentDeleteModel(AgentOpsClient(channel), 7)
        model.prepare()
        assertEq(false, model.run())
        assertEq(false, model.deleted)
        assertEq(2, model.report?.parts?.size)
        assertEq("Kept the agent. Kept task t1: relay down. No branch, issue or pull request was attached.", model.errorText)
        assertEq(null, AgentDeleteReport.from(mapOf("id" to 7L)))
    }
}
