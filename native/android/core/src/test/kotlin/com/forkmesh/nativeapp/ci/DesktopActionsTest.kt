package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class FakeActionsChannel : ControlCalling {
    val calls = ArrayList<Pair<String, Map<String, Any?>>>()
    val replies = HashMap<String, Map<String, Any?>>()
    val failures = HashMap<String, Exception>()
    val taps = LinkedHashMap<Any, (ControlEvent) -> Unit>()

    val methods: List<String> get() = calls.map { it.first }

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        calls.add(method to params)
        failures[method]?.let { throw it }
        return replies[method] ?: emptyMap()
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any {
        val token = Any()
        taps[token] = tap
        return token
    }

    override fun removeEventTap(token: Any) {
        taps.remove(token)
    }

    fun ring(event: String) {
        for (tap in taps.values.toList()) tap(ControlEvent(event, emptyMap()))
    }
}

class DesktopActionsTest {
    private val repo = RepoRef("alice", "tool")
    private val commit = "b".repeat(40)

    private fun unavailable() = FakeCiClient().also { it.summary = ActionsSummary.fromJson(linkedMapOf("ok" to false)) }

    private fun reply(runs: List<Map<String, Any?>>): Map<String, Any?> = linkedMapOf(
        "repo" to "alice/tool",
        "enabled" to true,
        "workflows" to listOf(
            linkedMapOf(
                "path" to ".forkmesh/ci.yml", "name" to "CI", "valid" to true, "triggers" to listOf("push", "manual"),
                "manual" to true, "disabled" to false, "here" to true, "runsOn" to "desktop, linux",
            ),
            linkedMapOf("path" to ".forkmesh/old.yml", "name" to "Old", "valid" to true, "manual" to true, "disabled" to true),
        ),
        "runs" to runs,
    )

    private fun running(): Map<String, Any?> = linkedMapOf(
        "id" to 41L, "workflow" to "CI", "path" to ".forkmesh/ci.yml", "status" to "running", "commit" to commit,
        "branch" to "main", "createdAtMs" to 5_000L, "startedAtMs" to 6_000L, "finishedAtMs" to 0L, "currentStep" to 1L,
        "steps" to listOf(
            linkedMapOf("name" to "build", "status" to "success"),
            linkedMapOf("name" to "test", "status" to "running"),
            linkedMapOf("name" to "deploy", "status" to "pending"),
        ),
        "pull" to 9L,
    )

    private fun model(channel: FakeActionsChannel, client: CiClient = unavailable(), background: (Runnable) -> Unit = { it.run() }) =
        RepoActionsModel(client, repo, DesktopActions(channel, "studio"), background = background)

    fun testDesktopRunsShowWhenNoNodeAnswers() {
        val channel = FakeActionsChannel()
        channel.replies["repos.actions"] = reply(listOf(running()))
        val model = model(channel)
        model.load()
        assertEq(listOf("repos.actions"), channel.methods)
        assertEq("alice/tool", channel.calls[0].second["repo"])
        expect(model.fromDesktop)
        expect(!model.isUnavailable)
        expect(model.canControl)
        expect(model.hasActiveRuns)
        assertEq("studio", model.summary?.node)
        val run = model.run(41)!!
        assertEq(ActionsRunStatus.Running, run.status)
        assertEq(".forkmesh/ci.yml", run.workflowPath)
        assertEq("refs/heads/main", run.ref)
        assertEq(9, run.pullNumber)
        assertEq(listOf("build", "test", "deploy"), run.steps.map { it.name })
        assertEq(1, run.stepsDone)
        assertEq(listOf(ActionsControlKind.Cancel), model.actions(run))
        assertEq(listOf(true, false), model.workflows.map { it.enabled })
        assertEq(listOf("desktop", "linux"), model.workflows[0].runsOn)
        assertEq(listOf("push", "manual"), model.workflows[0].events)
        assertEq(listOf(".forkmesh/ci.yml"), model.dispatchableWorkflows.map { it.path })
        assertEq(1, channel.taps.size)
        model.stop()
        assertEq(0, channel.taps.size)
    }

    fun testEmptyDesktopDefersToTheServerThenBacksItUp() {
        val channel = FakeActionsChannel()
        channel.replies["repos.actions"] = reply(emptyList())
        val model = model(channel)
        model.load()
        expect(model.fromDesktop)
        expect(!model.isUnavailable)
        assertEq("No workflow runs yet.", model.emptyMessage)
        assertEq(2, model.workflows.size)
    }

    fun testUnknownRepoOnTheDesktopFallsBackToTheServer() {
        val channel = FakeActionsChannel()
        channel.failures["repos.actions"] = LocalControlException("unknown_repo")
        val model = model(channel)
        model.load()
        expect(!model.fromDesktop)
        expect(model.isUnavailable)
        assertEq(
            "No node that hosts this repository is answering right now. Your desktop does not have this repository.",
            model.emptyMessage,
        )
    }

    fun testDesktopLogAppendsResetsAndFollowsBells() {
        val channel = FakeActionsChannel()
        channel.replies["repos.actions"] = reply(listOf(running()))
        val model = model(channel)
        var bumps = 0
        val remove = model.addChangeListener { bumps++ }
        model.load()
        channel.replies["repos.actions.log"] = linkedMapOf("text" to "one\n", "from" to 0L, "next" to 4L, "reset" to false)
        model.follow(41)
        assertEq("one\n", model.run(41)?.logTail)
        expect(!channel.calls.last().second.containsKey("since"))
        channel.replies["repos.actions.log"] = linkedMapOf("text" to "two\n", "from" to 4L, "next" to 8L, "reset" to false)
        channel.ring("repos.changed")
        assertEq(1, bumps)
        assertEq(listOf("repos.actions", "repos.actions.log", "repos.actions", "repos.actions.log"), channel.methods)
        assertEq(4L, channel.calls.last().second["since"])
        assertEq("one\ntwo\n", model.run(41)?.logTail)
        channel.replies["repos.actions.log"] = linkedMapOf("text" to "fresh\n", "from" to 0L, "next" to 6L, "reset" to true)
        model.loadLog(41)
        assertEq("fresh\n", model.run(41)?.logTail)
        remove()
        model.unfollow(41)
        channel.ring("repos.changed")
        assertEq(1, bumps)
        assertEq("repos.actions", channel.methods.last())
    }

    fun testControlsGoThroughPrepareAndRun() {
        val channel = FakeActionsChannel()
        channel.replies["repos.actions"] = reply(listOf(running()))
        channel.replies["repos.prepare"] = linkedMapOf("token" to "tok-1")
        channel.replies["repos.run"] = linkedMapOf("started" to true, "message" to "Stopping run #41.")
        val client = unavailable()
        val model = model(channel, client)
        model.load()
        expect(model.send(ActionsControlKind.Cancel, runId = 41))
        assertEq(listOf("repos.actions", "repos.prepare", "repos.run", "repos.actions"), channel.methods)
        assertEq(mapOf<String, Any?>("repo" to "alice/tool", "action" to "run.stop", "run" to 41L), channel.calls[1].second)
        assertEq(mapOf<String, Any?>("token" to "tok-1"), channel.calls[2].second)
        assertEq(CiNotice("Stopping run #41.", true), model.notice)
        expect(!model.needsFollowUp)
        assertEq(0, client.controls.size)

        channel.replies["repos.prepare"] = linkedMapOf("token" to "tok-2")
        expect(model.dispatch(model.dispatchableWorkflows[0], "feature/x"))
        assertEq(
            mapOf<String, Any?>("repo" to "alice/tool", "action" to "workflow.run", "path" to ".forkmesh/ci.yml", "branch" to "feature/x"),
            channel.calls[4].second,
        )

        channel.failures["repos.prepare"] = LocalControlException("wrong_state")
        expect(!model.send(ActionsControlKind.Approve, runId = 41))
        assertEq(CiNotice("Approve: That run has already moved on.", false), model.notice)
        expect(!model.send(ActionsControlKind.SetWorkflowEnabled, workflow = ".forkmesh/ci.yml", enabled = false))
    }
}
