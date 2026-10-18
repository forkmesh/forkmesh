package com.forkmesh.nativeapp.workflows

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

class WorkflowsTest {
    private val cases = Json.parseObject(resourceText("workflows/workflow-cases.json"))
    private val engine = WorkflowEngine.bundled()

    private fun withoutMs(value: Any?): Any? = when (value) {
        is Map<*, *> -> value.entries.filter { it.key != "ms" }.associate { it.key.toString() to withoutMs(it.value) }
        is List<*> -> value.map { withoutMs(it) }
        else -> value
    }

    @Suppress("UNCHECKED_CAST")
    private fun maps(value: Any?): List<Map<String, Any?>> = (value as? List<*>)?.map { it as Map<String, Any?> } ?: emptyList()

    fun testTheEngineRunsEveryCaseLikeTheServer() {
        val failures = mutableListOf<String>()
        for (item in maps(cases["runs"])) {
            val replies = maps(item["responses"]).map { WorkflowReply((it["status"] as Number).toInt(), it["text"].toString()) }.toMutableList()
            val sent = mutableListOf<Map<String, Any?>>()
            val waits = mutableListOf<Long>()
            @Suppress("UNCHECKED_CAST")
            val report = engine.run(
                WorkflowEngine.Input(
                    item["workflow"], item["payload"], item["credentials"] as Map<String, Any?>, item["trigger"].toString(),
                    cases["workflowId"].toString(), (cases["nowMs"] as Number).toLong(), clock = { 0L },
                    randoms = (item["randoms"] as? List<*>)?.map { (it as Number).toDouble() } ?: emptyList(),
                    workflows = maps(item["workflows"]),
                ),
                sleep = { waits.add(it) },
            ) { request ->
                sent.add(request.toJson())
                if (replies.isEmpty()) WorkflowReply(200, "{\"ok\": true, \"id\": 42, \"items\": [{\"n\": 1}, {\"n\": 2}]}") else replies.removeAt(0)
            }
            @Suppress("UNCHECKED_CAST")
            val expect = item["expect"] as Map<String, Any?>
            if (withoutMs(report) != withoutMs(expect["report"])) {
                failures.add(item["name"].toString() + "\n  got  " + Json.stringify(withoutMs(report)) + "\n  want " + Json.stringify(withoutMs(expect["report"])))
            } else if (sent != expect["requests"]) {
                failures.add(item["name"].toString() + " requests\n  got  " + Json.stringify(sent) + "\n  want " + Json.stringify(expect["requests"]))
            } else if (waits != (expect["waits"] as? List<*>)?.map { (it as Number).toLong() }.orEmpty()) {
                failures.add(item["name"].toString() + " waits\n  got  " + waits + "\n  want " + expect["waits"])
            }
        }
        expect(failures.isEmpty(), "${failures.size} cases differ:\n" + failures.take(40).joinToString("\n"))
    }

    fun testCleaningUrlsAndTheCatalogMatchTheServer() {
        for (item in maps(cases["clean"])) {
            val cleaned = engine.clean(item["input"])
            assertEq(item["error"], cleaned.error, Json.stringify(item["input"]).take(60))
            if (item["workflow"] != null) {
                assertEq(item["workflow"], cleaned.workflow)
                @Suppress("UNCHECKED_CAST")
                assertEq((item["scheduleMinutes"] as Number).toInt(), engine.scheduleMinutes(item["workflow"] as Map<String, Any?>))
            }
        }
        for (item in maps(cases["safeUrl"])) assertEq(item["safe"], engine.safeUrl(item["url"]), item["url"].toString())
        @Suppress("UNCHECKED_CAST")
        val catalog = cases["catalog"] as Map<String, Any?>
        val built = engine.catalog()
        for (key in catalog.keys) assertEq(catalog[key], built[key], "catalog $key")
        assertEq(100, WorkflowCatalog(built).integrations.size)
    }

    fun testTheTransportChecksEveryRedirectAndSwitchesPostToGet() {
        val calls = mutableListOf<Triple<String, String, ByteArray?>>()
        val answers = mutableListOf(
            WorkflowExchange(302, "/next", ByteArray(0)),
            WorkflowExchange(200, null, "done".toByteArray()),
        )
        val transport = HttpWorkflowTransport("ForkMesh-Workflows/1.0", exchange = { method, url, headers, body, _, _ ->
            calls.add(Triple(method, url, body))
            assertEq("ForkMesh-Workflows/1.0", headers["User-Agent"])
            answers.removeAt(0)
        })
        val reply = transport.send(WorkflowRequest("POST", "https://ok.example.com/start", mapOf("Content-Type" to "application/json"), "{}")) {
            it.startsWith("https://ok.example.com")
        }
        assertEq(WorkflowReply(200, "done"), reply)
        assertEq(listOf("POST", "GET"), calls.map { it.first })
        assertEq(listOf("https://ok.example.com/start", "https://ok.example.com/next"), calls.map { it.second })
        assertEq(null, calls[1].third)

        val blocked = HttpWorkflowTransport("UA", exchange = { _, _, _, _, _, _ -> WorkflowExchange(301, "https://10.0.0.1/", ByteArray(0)) })
            .send(WorkflowRequest("GET", "https://ok.example.com/x", emptyMap(), null)) { engine.safeUrl(it).isNotEmpty() }
        assertEq(WorkflowReply.NO_RESPONSE, blocked)
        val failed = HttpWorkflowTransport("UA", exchange = { _, _, _, _, _, _ -> throw java.io.IOException("down") })
            .send(WorkflowRequest("GET", "https://ok.example.com/x", emptyMap(), null)) { true }
        assertEq(WorkflowReply.NO_RESPONSE, failed)
    }

    fun testDraftsAddAndRemoveStepsAndKeepOrder() {
        val catalog = WorkflowCatalog(engine.catalog())
        var draft = WorkflowDraft.BLANK
        val (withIf, branch) = draft.addStep(catalog.kind("core.if")!!, "trigger")
        draft = withIf
        assertEq("equals", branch.params["operator"])
        val (withYes, yes) = draft.addStep(catalog.kind("core.noop")!!, branch.id, "true")
        val (withNo, no) = withYes.addStep(catalog.kind("core.noop")!!, branch.id, "false")
        draft = withNo
        assertEq(listOf("No operation", "No operation 2"), listOf(yes.name, no.name))
        assertEq(WorkflowDraft.ROW, no.y - yes.y)
        assertEq(listOf("trigger", branch.id, yes.id, no.id), draft.ordered.map { it.id })
        val (withSlack, slack) = draft.addStep(catalog.kind("app.slack")!!, yes.id)
        assertEq("postMessage", slack.operation)
        assertEq("Send a message", catalog.summary(slack))
        draft = withSlack.remove(yes.id)
        expect(WorkflowConnection(branch.id, "true", slack.id) in draft.connections)
        assertEq("If: true", catalog.branch(draft.inputs(slack.id).first(), draft))
        draft = draft.setTrigger(catalog.kind("core.webhook")!!)
        assertEq("core.webhook", draft.trigger?.type)
        assertEq(engine.clean(draft.graph).error, "")
    }

    fun testThePhoneRunnerUsesOnlyCredentialsSavedOnThePhone() {
        val catalog = WorkflowCatalog(engine.catalog())
        val credential = "c".repeat(32)
        val (added, slack) = WorkflowDraft.BLANK.addStep(catalog.kind("app.slack")!!, "trigger")
        val draft = added.update(slack.withParam("channel", "#general").withParam("text", "hi").copy(credential = credential))
        val store = MemorySecretStore()
        val phone = WorkflowPhoneCredentials(store)
        assertEq(emptyMap<String, Any?>(), phone.credentials(draft))
        phone.save(credential, "slack", mapOf("token" to "xoxb-1"))
        expect(phone.has(credential))
        val seen = mutableListOf<Map<String, String>>()
        val runner = WorkflowPhoneRunner(engine, HttpWorkflowTransport("UA", exchange = { _, _, headers, _, _, _ ->
            seen.add(headers)
            WorkflowExchange(200, null, "{\"ok\": true}".toByteArray())
        }))
        val run = runner.run(draft, phone.credentials(draft), clock = { 1_000L })
        expect(run.succeeded, run.error)
        expect(run.phone)
        assertEq("Bearer xoxb-1", seen.single()["Authorization"])
        assertEq(listOf("Run manually", slack.name), run.steps(draft).map { it.name })
        phone.remove(credential)
        val missing = runner.run(draft, phone.credentials(draft), clock = { 1_000L })
        assertEq("${slack.name}: Choose a Slack credential for this step", missing.error)
    }

    fun testThePhoneFindsTheWorkflowsARunWorkflowStepCalls() {
        val childId = "a".repeat(32)
        val grandchildId = "b".repeat(32)
        fun graph(name: String, vararg steps: Map<String, Any?>): Map<String, Any?> {
            val nodes = listOf(mapOf("id" to "t", "type" to "core.manual", "name" to "Start", "params" to emptyMap<String, Any?>())) + steps
            return mapOf("name" to name, "nodes" to nodes, "connections" to nodes.zipWithNext { a, b -> mapOf("from" to a["id"], "output" to "main", "to" to b["id"]) })
        }
        val child = graph("Double it", mapOf("id" to "m", "type" to "core.math", "name" to "Times", "params" to mapOf("a" to "{{ \$json.n }}", "operator" to "multiply", "b" to "2", "field" to "n")),
            mapOf("id" to "r", "type" to "core.runWorkflow", "name" to "Then", "params" to mapOf("workflow" to grandchildId)))
        val grandchild = graph("Add one", mapOf("id" to "m", "type" to "core.math", "name" to "Plus", "params" to mapOf("a" to "{{ \$json.n }}", "operator" to "add", "b" to "1", "field" to "n")))
        val opened = mutableListOf<String>()
        val api = WorkflowsApi { _, suffix, _ ->
            when (suffix) {
                "" -> mapOf("workflows" to listOf(mapOf("id" to childId, "name" to "Double it"), mapOf("id" to grandchildId, "name" to "Add one"), mapOf("id" to "c".repeat(32), "name" to "Unused")))
                "/$childId" -> mapOf("workflow" to (child + mapOf("id" to childId))).also { opened.add(childId) }
                "/$grandchildId" -> mapOf("workflow" to (grandchild + mapOf("id" to grandchildId))).also { opened.add(grandchildId) }
                else -> throw OrgApiException("not_found", "")
            }
        }
        val draft = WorkflowDraft.of(graph("Main", mapOf("id" to "r", "type" to "core.runWorkflow", "name" to "Run", "params" to mapOf("workflow" to " double IT "))))
        val phone = WorkflowPhoneCredentials(MemorySecretStore())
        val resolved = WorkflowTargets(api, phone).resolve(draft)
        assertEq(listOf(childId, grandchildId), opened)
        assertEq(listOf("Double it", "Add one"), resolved.workflows.map { it["name"] })
        val runner = WorkflowPhoneRunner(engine, HttpWorkflowTransport("UA", exchange = { _, _, _, _, _, _ -> WorkflowExchange(200, null, ByteArray(0)) }))
        val run = runner.run(draft, resolved.credentials, payload = mapOf("n" to 20L), clock = { 1_000L }, workflows = resolved.workflows)
        expect(run.succeeded, run.error)
        assertEq(listOf(mapOf("n" to 41L)), run.steps(draft).last().outputs.single().third)
        assertEq(0, WorkflowTargets(api, phone).resolve(WorkflowDraft.BLANK).workflows.size)
    }

    fun testCodeFieldsAreMonospaceAndMultiline() {
        val catalog = WorkflowCatalog(engine.catalog())
        val code = catalog.kind("core.code")!!.fields.first { it.key == "code" }
        expect(code.mono && code.multiline)
        val value = catalog.kind("core.variable")!!.fields.first { it.key == "value" }
        expect(!value.mono && !value.multiline)
    }

    fun testTheApiSpeaksTheServerRoutes() {
        val calls = mutableListOf<Triple<String, String, Map<String, Any?>?>>()
        val api = WorkflowsApi { method, suffix, body ->
            calls.add(Triple(method, suffix, body))
            when (suffix) {
                "" -> if (method == "GET") mapOf("workflows" to listOf(mapOf("id" to "w", "name" to "Daily", "nodes" to 3L, "active" to true)))
                else mapOf("workflow" to (body!! + mapOf("id" to "w")))
                "/w/run" -> mapOf("run" to mapOf("id" to "r", "status" to "success", "nodes" to emptyMap<String, Any?>()))
                else -> throw OrgApiException("plugin_not_installed", "")
            }
        }
        assertEq(listOf("Daily"), api.list().map { it.name })
        assertEq(3, api.list()[0].steps)
        val saved = api.save(WorkflowDraft.BLANK.copy(name = "Mine"))
        assertEq("w", saved.id)
        assertEq(false, calls.last().third!!["active"])
        assertEq("success", api.run("w").status)
        try {
            api.credentials()
            expect(false, "credentials should fail")
        } catch (error: OrgApiException) {
            assertEq("Install the Workflows plugin from the Market to use workflows.", WorkflowsText.describe(error, "x"))
        }
        assertEq("Every 2 hours", WorkflowsText.every(120))
    }
}
