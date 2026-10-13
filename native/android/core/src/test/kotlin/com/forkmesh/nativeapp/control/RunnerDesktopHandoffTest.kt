package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentLaunchMessage
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentQueueApi
import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class RunnerDesktopHandoffTest {
    private fun failed(): AgentLaunchSession = AgentLaunchSession(
        sessionId = "r9",
        owner = "fmnext-n1",
        repo = "site",
        provider = "claude-code",
        status = "failed",
        targetNode = "fmnext-n1",
        title = "Fix the login page",
        requestedModel = "opus",
        localAgentId = 4,
        branchName = "agent/9-fix-login",
        baseBranch = "main",
        runner = "fmnext-r1",
        lastError = "OAuth session expired\nand could not be refreshed",
        history = listOf(
            AgentLaunchMessage(role = "user", text = "Fix the login redirect"),
            AgentLaunchMessage(role = "assistant", text = "Found the bug in\nauth.ts."),
            AgentLaunchMessage(role = "user", text = "Also add a test"),
        ),
    )

    fun testTheTaskNamesTheRunnerTheBranchAndWhatHappened() {
        val task = RunnerDesktopHandoff.task(failed())
        expect(task.startsWith("Continue a task that the ForkMesh runner fmnext-r1 started but did not finish"))
        expect(task.contains("(session r9, it ended as \"failed\")."))
        expect(task.contains("It stopped with: OAuth session expired and could not be refreshed"))
        expect(task.contains("pushed to branch agent/9-fix-login (started from main)."))
        expect(task.contains("Task:\nFix the login redirect"))
        expect(task.endsWith("What happened on the runner:\nAgent: Found the bug in auth.ts.\nMe: Also add a test"))
        assertEq("Continue: Fix the login page", RunnerDesktopHandoff.title(failed()))
    }

    fun testANeverPushedRunStartsFromTheBaseAndLongHistoryIsTrimmed() {
        val session = failed().copy(
            branchName = "",
            history = failed().history + (0 until 40).map { AgentLaunchMessage(role = "assistant", text = "step $it " + "x".repeat(900)) },
        )
        val task = RunnerDesktopHandoff.task(session)
        expect(task.contains("It left no branch, so start from the base branch."))
        expect(task.length <= RunnerDesktopHandoff.MAX_TASK_CHARS)
        expect(task.contains("… earlier steps omitted"))
        expect(task.contains("step 39 "))
        expect(!task.contains("step 0 "))
    }

    fun testOnlyARelayRunWithARepositoryCanMove() {
        val row = UnifiedAgentRow.fromCloud(failed())
        expect(RunnerDesktopHandoff.canMove(row, desktopPaired = true))
        expect(!RunnerDesktopHandoff.canMove(row, desktopPaired = false))
        expect(!RunnerDesktopHandoff.canMove(row.copy(merged = true), desktopPaired = true))
    }

    fun testFailuresSayWhatToDo() {
        assertEq(
            "The desktop has no working copy of alice/site. Clone it there first, then move the agent. The run on fmnext-r1 is stopped; Continue restarts it there.",
            RunnerDesktopHandoff.failureText(LocalControlException("no_checkout"), "alice/site", "fmnext-r1"),
        )
        assertEq("Update ForkMesh on the desktop to move runner agents to it.", RunnerDesktopHandoff.failureText(LocalControlException("unsupported"), "alice/site"))
        assertEq(
            "The desktop could not start the agent. Branch agent/9 is not on this node. The run on fmnext-r1 is stopped; Continue restarts it there.",
            RunnerDesktopHandoff.failureText(LocalControlException("not_started", "Branch agent/9 is not on this node."), "alice/site", "fmnext-r1"),
        )
        assertEq(
            "The desktop could not start the agent. It says why in its Agents list.",
            RunnerDesktopHandoff.failureText(LocalControlException("not_started"), "alice/site"),
        )
        assertEq("Continuing on studio as agent #42, on agent/9-fix-login.", RunnerDesktopMove(42, "studio", "agent/9-fix-login").message)
    }

    private fun queue(requests: MutableList<String>, deleteStatus: Int = 200): AgentQueueApi {
        val transport = HttpTransport { request ->
            requests.add("${request.method} ${request.url.substringAfter("https://x")} ${request.body?.let { String(it) } ?: ""}".trim())
            if (request.method == "DELETE") {
                val body = if (deleteStatus == 200) """{"ok":true}""" else """{"ok":false,"error":"session_delete_failed"}"""
                return@HttpTransport HttpClient.Response(deleteStatus, emptyMap(), body.toByteArray())
            }
            val body = if (request.method == "POST") {
                """{"ok":true,"stopped":true}"""
            } else {
                """{"session":{"id":"r9","org":"fmnext-n1","repo":"site","provider":"claude-code","status":"running","targetNode":"fmnext-n1",""" +
                    """"runner":"fmnext-r1","branch":"agent/9-fix-login","baseBranch":"main","requestedModel":"opus","localAgentId":4,""" +
                    """"history":[{"role":"user","text":"Fix the login redirect"}]}}"""
            }
            HttpClient.Response(200, emptyMap(), body.toByteArray())
        }
        return AgentQueueApi({ "https://x" }, { "t" }, transport)
    }

    private fun hub(desktop: LocalControlTest.FakeDesktop): DesktopControlHub {
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        return DesktopControlHub(
            null,
            { target -> RelayControlConnection.open(LocalControlTest.LoopChannel(desktop), identity, target.key, target.name, timeoutMs = 5000) },
            accountDesktopsLoader = { listOf(PairedDesktop(desktop.key, "studio", "alice")) },
        ) { emptyList() }
    }

    fun testTheRunIsCheckedStoppedThenTakenOver() {
        val desktop = LocalControlTest.FakeDesktop()
        val requests = ArrayList<String>()
        val center = AgentControlCenter(hub(desktop), queue(requests))
        val row = UnifiedAgentRow.fromCloud(failed().copy(status = "running"))
        val move = center.continueOnDesktop(row)
        expect(requests.any { it.startsWith("POST ") && it.contains("/r9") && it.contains("\"stop\"") })
        val calls = desktop.requests.filter { it["method"] == "agents.takeover" }.map { it["params"] as Map<*, *> }
        assertEq(2, calls.size)
        assertEq(true, calls[0]["check"])
        expect(calls[1]["check"] == null)
        assertEq("r9", calls[1]["cloudSession"])
        assertEq("fmnext-n1", calls[1]["owner"])
        assertEq("site", calls[1]["name"])
        assertEq("agent/9-fix-login", calls[1]["branch"])
        assertEq("main", calls[1]["base"])
        assertEq("fmnext-r1", calls[1]["runner"])
        assertEq("claude-code", calls[1]["provider"])
        assertEq("opus", calls[1]["model"])
        assertEq(42, move.agentId)
        assertEq("Continuing on studio as agent #42, on agent/9-fix-login.", move.message)
        expect(requests.any { it.startsWith("DELETE ") && it.contains("/r9") })
        expect(!move.runnerKept)
        center.dispose()
    }

    fun testARunnerCopyThatCannotBeDeletedIsReported() {
        val desktop = LocalControlTest.FakeDesktop()
        val requests = ArrayList<String>()
        val center = AgentControlCenter(hub(desktop), queue(requests, deleteStatus = 503))
        val move = center.continueOnDesktop(UnifiedAgentRow.fromCloud(failed()))
        expect(move.runnerKept)
        assertEq(
            "Continuing on studio as agent #42, on agent/9-fix-login. Its run on the runner could not be deleted; delete it from Agents.",
            move.message,
        )
        center.dispose()
    }

    fun testADesktopWithoutACheckoutLeavesTheRunnerAlone() {
        val desktop = LocalControlTest.FakeDesktop()
        desktop.takeoverError = "no_checkout"
        val requests = ArrayList<String>()
        val center = AgentControlCenter(hub(desktop), queue(requests))
        val failure = assertFails<RunnerDesktopMoveException> {
            center.continueOnDesktop(UnifiedAgentRow.fromCloud(failed().copy(status = "running")))
        }
        assertEq("The desktop has no working copy of fmnext-n1/site. Clone it there first, then move the agent.", failure.message)
        expect(requests.none { it.startsWith("POST ") || it.startsWith("DELETE ") })
        center.dispose()
    }
}
