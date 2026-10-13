package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentLaunchTest {
    private fun json(text: String): Map<String, Any?> = Json.parse(text).asJsonMap()

    fun testSessionReadsAgentInfoAndSeconds() {
        val session = AgentLaunchSession.fromJson(
            json(
                """{"id":"s1","org":"alice","repo":"app","status":"running","createdAt":1700000000,
                "agentInfo":{"branchName":"agent/x","baseRef":"main","prNumber":7,"createPr":true},
                "history":[{"role":"user","author":"alice","text":"fix login\nplease","attachments":[
                {"id":"0123456789abcdef0123456789abcdef","name":"a/b/shot.png","size":12}]}],
                "diagnostic":{"message":"boom"},"lastError":"boom","localAgentId":3}""",
            ),
        )
        assertEq("s1", session.sessionId)
        assertEq(1_700_000_000_000L, session.createdAtMs)
        assertEq("agent/x", session.branchName)
        assertEq("main", session.baseBranch)
        assertEq(7, session.prNumber)
        expect(session.createPr)
        expect(session.isActive)
        expect(session.isPromptable)
        assertEq("fix login", session.displayTitle)
        assertEq("fix login\nplease", session.retryPrompt)
        assertEq("", session.diagnosticNote)
        val attachment = session.history.first().attachments.single()
        assertEq("shot.png", attachment.name)
        assertEq("image/png", attachment.mediaType)
        val ref = session.attachmentRefs(session.history.first(), "alice", "app").single()
        assertEq("relay:owner:alice/app:s1:0123456789abcdef0123456789abcdef", ref.cacheKey)
    }

    fun testRetryableAndResume() {
        val failed = AgentLaunchSession(sessionId = "x", status = "failed", title = "Do it", localAgentId = 0)
        expect(failed.isRetryable)
        expect(!failed.canResume)
        expect(agentRetryDetail(failed).startsWith("No node ever started"))
        val claimed = failed.copy(localAgentId = 4, targetNode = "mirror35")
        expect(claimed.canResume)
        assertEq(
            "Sends the same task back to mirror35, which picks this session up on the branch it already cut.",
            agentRetryDetail(claimed),
        )
        expect(!AgentLaunchSession(sessionId = "y", status = "failed").isRetryable)
    }

    fun testRelayStoppedRunIsStoppedAndRetriesItsTask() {
        val stopped = AgentLaunchSession.fromJson(
            json(
                """{"id":"s","status":"failed","title":"Fix login","localAgentId":4,"stopRequestedAt":1700000000000,
                "history":[{"role":"user","text":"fix the login form"},{"role":"agent","text":"working"},
                {"role":"user","text":"Stop this run.","state":"stopping"}]}""",
            ),
        )
        assertEq(1_700_000_000_000L, stopped.stopRequestedAtMs)
        expect(stopped.isStopped)
        expect(stopped.isRetryable)
        expect(stopped.canResume)
        assertEq("stopped", stopped.statusLabel)
        assertEq("fix the login form", stopped.retryPrompt)
        assertEq("stopped", AgentRunLog(AgentTaskChain(stopped), "r").status)

        val trimmed = stopped.copy(history = listOf(AgentLaunchMessage(role = "user", text = "Stop this run.", state = "stopping")), title = "")
        assertEq("", trimmed.retryPrompt)
        expect(!trimmed.isRetryable)
        assertEq("Agent session", trimmed.displayTitle)
        assertEq("Fix login", trimmed.copy(title = "Fix login").retryPrompt)

        val failed = stopped.copy(stopRequestedAtMs = 0)
        expect(!failed.isStopped)
        assertEq("failed", failed.statusLabel)
        expect(!stopped.copy(status = "running").isStopped)
    }

    fun testPreferredTargetOrder() {
        val options = AgentLaunchOptions(
            canQueue = true,
            targets = listOf(
                AgentLaunchTarget("desk", online = true, source = "true"),
                AgentLaunchTarget("mirror1", online = false, runtimeMode = "headless"),
                AgentLaunchTarget("mirror2", online = true, runtimeMode = "headless"),
            ),
        )
        assertEq("mirror2", options.preferredTarget?.node)
        assertEq("desk", options.copy(targets = options.targets.take(1)).preferredTarget?.node)
        assertEq("desk", options.copy(targets = options.targets.take(2)).preferredTarget?.node)
        assertEq("mirror1", options.copy(targets = options.targets.drop(1).take(1)).preferredTarget?.node)
    }

    fun testOptionsParseModelsAndOrgScope() {
        val options = AgentLaunchOptions.fromJson(
            json("""{"canQueueAgent":true,"models":{"claude-code":["opus","haiku"]},"sessions":[{"id":"a"}],"targets":[{"node":"m","online":true}]}"""),
            orgScope = true,
        )
        expect(options.canQueue)
        assertEq(listOf("opus", "haiku"), options.models["claude-code"])
        expect(options.sessions.single().orgScope)
        assertEq(listOf("opus", "haiku"), AgentModelCatalog.nodeChoices(options.models).map { it.alias })
        assertEq(9, AgentModelCatalog.nodeChoices(mapOf("x" to listOf("nope"))).size)
    }

    fun testTaskChainStages() {
        val queued = AgentLaunchSession(sessionId = "q", status = "queued", targetNode = "m1", createPr = true, createdBy = "al")
        val stages = AgentTaskChain(queued).stages
        assertEq(listOf("Done", "Active", "Pending", "Pending", "Pending"), stages.map { it.state.name })
        assertEq("Sent by @al", stages[0].detail)
        assertEq("Waiting for m1 to pick it up", stages[1].detail)

        val running = queued.copy(status = "running", localAgentId = 2, branchName = "agent/b", baseBranch = "main")
        val chain = agentTaskChain(running, listOf(PublishedPull(number = 9, title = "t", status = "merged", head = "agent/b")))
        assertEq(9, chain.prNumber)
        expect(chain.isMerged)
        assertEq("Merged", chain.summary)
        assertEq("agent/b from main", chain.stages[2].detail)

        val failed = running.copy(status = "failed")
        val failedChain = AgentTaskChain(failed)
        assertEq(AgentStageState.Failed, failedChain.stages[3].state)
        assertEq("On agent/b", failedChain.summary)
    }

    fun testCloudDetection() {
        val targets = listOf(AgentLaunchTarget("Mirror35", runtimeMode = "headless"), AgentLaunchTarget("desk", runtimeMode = "desktop"))
        expect(launchRunsInCloud(AgentLaunchSession("a", targetNode = "mirror35"), targets))
        expect(!launchRunsInCloud(AgentLaunchSession("a", targetNode = "desk"), targets))
        expect(!launchRunsInCloud(AgentLaunchSession("a", targetNode = "unknown"), targets))
        expect(launchRunsInCloud(AgentLaunchSession("a", targetNode = "desk", runner = "fmnext-r1"), targets))
    }

    fun testQueueAndStopResults() {
        val waiting = AgentQueueResult.fromJson(json("""{"queueState":"waiting_for_desktop","session":{"id":"n"}}"""))
        assertEq("Queued. It starts as soon as the node connects.", waiting.label)
        assertEq("n", waiting.session?.sessionId)
        assertEq("Sent back to the mirror. It picks the session up on its branch.", AgentRetryResult(AgentQueueResult(targetOnline = true), true).label)
        assertEq("Stopped.", AgentStopResult.fromJson(json("""{"stopped":true}""")).label)
    }

    fun testModelLabels() {
        assertEq("Opus 5.5", AgentModelCatalog.shortLabel("claude-opus-5-5"))
        assertEq("Sonnet 5", AgentModelCatalog.shortLabel("sonnet"))
        assertEq("custom-model", AgentModelCatalog.shortLabel("custom-model"))
        assertEq("", AgentModelCatalog.shortLabel("  "))
    }
}
