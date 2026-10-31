package com.forkmesh.nativeapp.workflows

import com.forkmesh.nativeapp.agents.ops.FakeControlChannel
import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

class DesktopWorkflowsTest {
    private val listJson: Map<String, Any?> = mapOf(
        "desktop" to "office",
        "workflows" to listOf(
            mapOf(
                "repo" to "forkmesh/forkmesh", "id" to "cfo", "name" to "CFO", "specialAgent" to true, "enabled" to true, "steps" to 2L,
                "trigger" to "Weekly", "lastRunAt" to 1_700_000_000_000.0, "lastStatus" to "success", "lastSummary" to "Burn is fine",
            ),
            mapOf(
                "repo" to "forkmesh/forkmesh", "id" to "clean", "name" to "Clean", "specialAgent" to false, "enabled" to false,
                "running" to true, "runningStep" to "Measure the disk", "steps" to 7L,
            ),
        ),
    )

    private class Handler(private val answer: (String, Map<String, Any?>) -> Map<String, Any?>) : ControlCalling {
        val requests = ArrayList<Pair<String, Map<String, Any?>>>()

        override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
            requests.add(method to params)
            return answer(method, params)
        }

        override fun addEventTap(tap: (ControlEvent) -> Unit): Any = Any()

        override fun removeEventTap(token: Any) {}

        fun count(method: String) = requests.count { it.first == method }
    }

    private class Recorder(private val answer: (String, String, Map<String, Any?>?) -> Map<String, Any?>) {
        val calls = ArrayList<Triple<String, String, Map<String, Any?>?>>()
        val api = WorkflowsApi { method, suffix, body ->
            calls.add(Triple(method, suffix, body))
            answer(method, suffix, body)
        }
    }

    private fun cleanupDraft() = WorkflowDraft(
        id = "w1", name = "Free disk space",
        nodes = listOf(
            WorkflowNode("run", "core.manual", "Run"),
            WorkflowNode("usage", "desktop.diskUsage", "Measure the disk"),
            WorkflowNode("agents", "desktop.cleanAgents", "Remove agent leftovers"),
            WorkflowNode("report", "desktop.diskReport", "Report free space"),
        ),
        connections = listOf(
            WorkflowConnection("run", to = "usage"),
            WorkflowConnection("usage", to = "agents"),
            WorkflowConnection("agents", to = "report"),
        ),
    )

    fun testListParsesRowsAndRefreshesOnChange() {
        val channel = FakeControlChannel()
        channel.reply("workflows.list", listJson)
        val store = DesktopWorkflowsStore(channel)
        store.refresh()
        assertEq("office", store.desktop)
        assertEq(listOf("forkmesh/forkmesh|cfo", "forkmesh/forkmesh|clean"), store.rows.map { it.id })
        assertEq("Special agent", store.rows[0].kindLabel)
        assertEq(1_700_000_000_000L, store.rows[0].lastRunAt)
        expect(store.rows[1].running)
        expect(!store.rows[1].enabled)
        assertEq("Desktop workflow", store.rows[1].kindLabel)
        expect(store.loaded)
        assertEq("Desktop workflow · forkmesh/forkmesh · Running Measure the disk · Off",
                 DesktopWorkflowsText.subtitle(store.rows[1]) { "now" })
        channel.emit("teammates.changed")
        channel.emit("workflows.changed")
        assertEq(2, channel.methods().count { it == "workflows.list" })
        assertEq(1, store.bells)
        store.stop()
        assertEq(0, channel.tapCount)
    }

    fun testOlderDesktopIsUnsupported() {
        val channel = FakeControlChannel()
        channel.failures["workflows.list"] = LocalControlException("unsupported")
        val store = DesktopWorkflowsStore(channel)
        store.refresh()
        expect(store.unsupported)
        expect(store.rows.isEmpty())
        expect(store.error.contains("Update ForkMesh"))
    }

    fun testRunAsksForATokenThenSpendsIt() {
        val channel = FakeControlChannel()
        channel.reply("workflows.list", listJson)
        channel.reply("workflows.prepare", mapOf("token" to "t1", "summary" to "Run CFO from forkmesh/forkmesh on the desktop now."))
        channel.reply("workflows.run", mapOf("started" to true, "message" to "CFO is running."))
        val store = DesktopWorkflowsStore(channel)
        store.refresh()
        val confirmation = store.prepare("run", store.rows.first(), assignment = "  Check the burn  ")
        assertEq("Run CFO from forkmesh/forkmesh on the desktop now.", confirmation.summary)
        assertEq(mapOf("action" to "run", "repo" to "forkmesh/forkmesh", "id" to "cfo"), channel.params("workflows.prepare"))
        assertEq("CFO is running.", store.perform(confirmation))
        assertEq(
            mapOf("action" to "run", "repo" to "forkmesh/forkmesh", "id" to "cfo", "token" to "t1", "assignment" to "Check the burn"),
            channel.params("workflows.run"),
        )
        assertEq(2, channel.methods().count { it == "workflows.list" })
    }

    fun testPrepareWithoutTokenFails() {
        val channel = FakeControlChannel()
        channel.reply("workflows.list", listJson)
        val store = DesktopWorkflowsStore(channel)
        store.refresh()
        val failure = runCatching { store.prepare("stop", store.rows.last()) }.exceptionOrNull()
        assertEq("bad_answer", (failure as? LocalControlException)?.code)
    }

    fun testDetailParsesRuns() {
        val channel = FakeControlChannel()
        channel.reply("workflows.list", listJson)
        channel.reply("workflows.get", mapOf(
            "repo" to "forkmesh/forkmesh", "id" to "clean", "name" to "Clean",
            "nodes" to listOf(
                mapOf("id" to "run", "type" to "workflow.manual", "name" to "Run by hand"),
                mapOf("id" to "usage", "type" to "desktop.diskUsage", "name" to "Measure the disk"),
            ),
            "runs" to listOf(mapOf(
                "id" to "r1", "status" to "error", "error" to "Disk busy", "startedAtMs" to 1000.0, "finishedAtMs" to 4000.0,
                "steps" to listOf(mapOf(
                    "nodeId" to "usage", "name" to "Measure the disk", "status" to "error", "startedAtMs" to 1000.0,
                    "finishedAtMs" to 4000.0, "logs" to listOf("a", "b"),
                )),
            )),
            "active" to mapOf("id" to "r2", "status" to "running", "steps" to listOf(mapOf("nodeId" to "usage", "status" to "running"))),
        ))
        val store = DesktopWorkflowsStore(channel)
        store.refresh()
        val detail = store.detail(store.rows.last())
        assertEq(mapOf("repo" to "forkmesh/forkmesh", "id" to "clean"), channel.params("workflows.get"))
        assertEq(listOf("Run by hand", "Measure the disk"), detail.nodes.map { it.name })
        assertEq("Disk busy", detail.runs.first().summary)
        assertEq(3000L, detail.runs.first().steps.first().ms)
        assertEq(listOf("a", "b"), detail.runs.first().steps.first().logs)
        assertEq("r2", detail.latest?.id)
        assertEq(listOf("r2", "r1"), detail.shownRuns.map { it.id })
    }

    fun testRunOnDesktopDrivesEachStepAndRecordsTheRun() {
        val channel = Handler { method, params ->
            when (method) {
                "diag.prepare" -> mapOf("token" to "tok-" + params.str("step"))
                "diag.run" -> when (params.str("step")) {
                    "agents" -> mapOf("freedBytes" to 2048L, "items" to 3L, "freeBytes" to 4096L, "totalBytes" to 0L, "lines" to listOf("removed 3"))
                    "report" -> mapOf("freedBytes" to 0L, "freeBytes" to 5_000_000L, "totalBytes" to 9_000_000L)
                    else -> mapOf("freedBytes" to 0L, "freeBytes" to 1024L, "totalBytes" to 9_000_000L)
                }
                else -> emptyMap()
            }
        }
        val recorder = Recorder { method, suffix, _ ->
            assertEq("POST", method)
            assertEq("/w1/record", suffix)
            mapOf("run" to mapOf("id" to "run-9", "trigger" to "desktop", "status" to "success", "nodes" to emptyMap<String, Any?>()))
        }
        val lives = ArrayList<Map<String, String>>()
        val run = WorkflowDesktopRunner(channel, recorder.api).run(cleanupDraft()) { lives.add(it) }
        assertEq("run-9", run.id)
        expect(run.desktop)
        assertEq(listOf("diag.prepare", "diag.run", "diag.prepare", "diag.run", "diag.prepare", "diag.run"), channel.requests.map { it.first })
        assertEq(mapOf("action" to "freeDiskSpace", "step" to "usage", "token" to "tok-usage"), channel.requests[1].second)
        assertEq("agents", channel.requests[3].second.str("step"))
        expect(lives.any { it["usage"] == "running" })
        assertEq("success", lives.last()["report"])
        val body = recorder.calls.first().third!!
        val nodes = body.obj("nodes")
        assertEq("success", nodes.obj("run").str("status"))
        assertEq("success", nodes.obj("agents").str("status"))
        assertEq(listOf<Any?>("removed 3"), nodes.obj("agents").list("logs"))
        val item = nodes.obj("agents").obj("outputs").obj("main").maps("items").first()
        assertEq("2.0 KB", item.str("freed"))
        assertEq(3L, item.long("items"))
        assertEq(mapOf("freedBytes" to 2048L, "freeBytes" to 5_000_000L, "totalBytes" to 9_000_000L), body.obj("output"))
    }

    fun testFailedStepSkipsTheRestAndIsRecorded() {
        val channel = Handler { method, params ->
            if (method == "diag.prepare") return@Handler mapOf("token" to "t")
            if (params.str("step") == "usage") throw LocalControlException("cleanup_failed", "Permission denied")
            emptyMap()
        }
        val recorder = Recorder { _, _, body ->
            mapOf("run" to mapOf("id" to "run-1", "trigger" to "desktop", "status" to "error", "error" to body.str("error")))
        }
        WorkflowDesktopRunner(channel, recorder.api).run(cleanupDraft())
        assertEq(1, channel.count("diag.run"))
        val nodes = recorder.calls.first().third.obj("nodes")
        assertEq("error", nodes.obj("usage").str("status"))
        expect(nodes.obj("usage").str("error").contains("Permission denied"))
        assertEq("skipped", nodes.obj("agents").str("status"))
        assertEq("skipped", nodes.obj("report").str("status"))
    }

    fun testUnsavedRunStillReportsTheResult() {
        val channel = Handler { method, _ -> if (method == "diag.prepare") mapOf("token" to "t") else emptyMap() }
        val recorder = Recorder { _, _, _ -> throw OrgApiException("offline", "offline") }
        val run = WorkflowDesktopRunner(channel, recorder.api).run(cleanupDraft())
        assertEq("success", run.status)
        expect(run.desktop)
        expect(run.error.contains("could not save"))
        assertEq(4, run.nodes.keys.size)
    }

    fun testNoDesktopNoRun() {
        val recorder = Recorder { _, _, _ -> emptyMap() }
        val run = WorkflowDesktopRunner(null, recorder.api).run(cleanupDraft())
        assertEq("error", run.status)
        assertEq(WorkflowsText.DESKTOP_MISSING, run.error)
        expect(recorder.calls.isEmpty())
    }

    fun testDesktopStepsMatchTheServerSpec() {
        val spec = Json.parseObject(resourceText("workflows/workflow-spec.json"))
        val fromSpec = LinkedHashMap<String, String>()
        for ((type, value) in spec.obj("coreNodes")) {
            val step = (value as? Map<*, *>)?.get("desktopStep") as? String ?: ""
            if (step.isNotEmpty()) fromSpec[type] = step
        }
        assertEq(WorkflowDesktopSteps.steps, fromSpec.toMap())
        expect(WorkflowDesktopSteps.hasDesktopSteps(cleanupDraft()))
        expect(!WorkflowDesktopSteps.hasDesktopSteps(WorkflowDraft.BLANK))
        assertEq("512 B", WorkflowDesktopSteps.bytes(512))
        assertEq("1.5 MB", WorkflowDesktopSteps.bytes(1_572_864))
        assertEq(
            "Measure the disk, Remove agent leftovers, Report free space. " + WorkflowsText.DESKTOP_RUN_NOTE,
            WorkflowDesktopSteps.confirmMessage(cleanupDraft()),
        )
    }

    fun testSummariesAndRunsKnowTheDesktop() {
        expect(WorkflowSummary.of(mapOf("id" to "a", "runsOn" to "desktop")).runsOnDesktop)
        expect(!WorkflowSummary.of(mapOf("id" to "b")).runsOnDesktop)
        expect(WorkflowRun.of(mapOf("trigger" to "desktop"), phone = false).desktop)
        expect(!WorkflowRun.of(mapOf("trigger" to "manual"), phone = false).desktop)
    }
}
