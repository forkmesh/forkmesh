package com.forkmesh.nativeapp.workflows

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.security.SecretStore
import com.forkmesh.nativeapp.workflows.WorkflowPy.list
import com.forkmesh.nativeapp.workflows.WorkflowPy.map
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URI
import java.net.URL

typealias WorkflowJson = Map<String, Any?>

private fun text(value: Any?, fallback: String = ""): String = when (value) {
    null -> fallback
    is String -> value
    is Double -> if (value == Math.rint(value) && value.isFinite()) value.toLong().toString() else value.toString()
    is Map<*, *>, is List<*> -> Json.stringify(value)
    else -> value.toString()
}

private fun long(value: Any?): Long = when (value) {
    is Number -> value.toLong()
    is String -> value.toLongOrNull() ?: 0L
    else -> 0L
}

object WorkflowsText {
    const val TITLE = "Workflows"
    const val UNAVAILABLE = "Workflows are unavailable"
    const val EMPTY_TITLE = "No workflows yet"
    const val EMPTY_MESSAGE =
        "A workflow is a trigger followed by steps: HTTP requests, logic and 100 integrations. Start from an example or a blank workflow."
    const val PHONE_RUN_NOTE =
        "Runs on this phone use the built-in engine and the credentials saved on this phone. Runs on ForkMesh use your account's credentials."
    const val DESKTOP_RUN_NOTE =
        "These steps run on your desktop, in its forkmesh/forkmesh checkout. Only ForkMesh leftovers that nothing uses are deleted."
    const val DESKTOP_MISSING = "Pair this phone with your desktop to run these steps."

    val ERRORS = mapOf(
        "plugin_not_installed" to "Install the Workflows plugin from the Market to use workflows.",
        "too_many_workflows" to "You have 50 workflows, the most an account can keep.",
        "too_many_credentials" to "You have 50 credentials, the most an account can keep.",
        "rate_limited" to "This account ran 120 workflows in the last hour. Try again later.",
        "no_trigger" to "Add a trigger before running this workflow.",
        "not_found" to "This workflow no longer exists.",
        "too_many_nodes" to "A workflow can have at most 60 steps.",
        "too_many_connections" to "A workflow can have at most 120 connections.",
        "too_many_triggers" to "A workflow can have only one trigger.",
        "duplicate_node_name" to "Every step needs its own name.",
        "invalid_node" to "A step has an invalid id.",
        "unknown_node_type" to "A step uses an integration this version does not know.",
        "value_too_long" to "A value is longer than 20,000 characters.",
        "invalid_credential" to "A step points at an invalid credential.",
        "invalid_connection" to "A connection is invalid: steps cannot connect to themselves or into the trigger.",
        "cycle" to "Steps cannot connect in a loop.",
        "invalid_workflow" to "This workflow could not be read.",
        "missing_credential_field" to "Fill in every credential field.",
        "invalid_credential_url" to "Credential URLs must be public https:// addresses.",
        "unknown_integration" to "This integration is not available.",
        "no_credential_needed" to "This integration does not need a credential.",
    )

    fun error(code: String): String = ERRORS[code] ?: code.replace('_', ' ')

    fun describe(error: Throwable, fallback: String): String {
        val refusal = error as? OrgApiException ?: return fallback
        return ERRORS[refusal.code] ?: refusal.message.ifEmpty { null } ?: fallback
    }

    fun trigger(type: String): String = when (type) {
        "core.manual" -> "Manual"
        "core.webhook" -> "Webhook"
        "core.schedule" -> "Schedule"
        else -> "No trigger"
    }

    fun status(status: String): String = when (status) {
        "success" -> "Succeeded"
        "error" -> "Failed"
        "skipped" -> "Skipped"
        "" -> "Never run"
        else -> status.replaceFirstChar { it.uppercase() }
    }

    fun every(minutes: Int): String = when {
        minutes <= 0 -> ""
        minutes % 1440 == 0 -> if (minutes == 1440) "Every day" else "Every ${minutes / 1440} days"
        minutes % 60 == 0 -> if (minutes == 60) "Every hour" else "Every ${minutes / 60} hours"
        else -> "Every $minutes minutes"
    }
}

data class WorkflowField(
    val key: String,
    val label: String,
    val type: String,
    val required: Boolean,
    val placeholder: String,
    val defaultValue: String,
    val options: List<Pair<String, String>>,
    val secret: Boolean,
) {
    val multiline: Boolean get() = type == "json" || type == "text" || type == "code"
    val mono: Boolean get() = type == "json" || type == "code"

    companion object {
        fun of(json: Any?): WorkflowField {
            val item = map(json)
            return WorkflowField(
                text(item["key"]), text(item["label"], text(item["key"])), text(item["type"], "string"), item["required"] == true,
                text(item["placeholder"]), text(item["default"]),
                list(item["options"]).map { map(it).let { option -> text(option["value"]) to text(option["label"], text(option["value"])) } },
                item["secret"] == true,
            )
        }
    }
}

data class WorkflowOperation(val key: String, val name: String, val method: String, val fields: List<WorkflowField>)

data class WorkflowNodeKind(
    val type: String,
    val name: String,
    val group: String,
    val description: String,
    val color: String,
    val trigger: Boolean,
    val outputs: List<String>,
    val fields: List<WorkflowField>,
    val credentialFields: List<WorkflowField>,
    val operations: List<WorkflowOperation>,
) {
    val isApp: Boolean get() = type.startsWith("app.")
    val appKey: String get() = if (isApp) type.substring(4) else ""
    val needsCredential: Boolean get() = credentialFields.isNotEmpty()

    fun fields(operation: String): List<WorkflowField> =
        if (!isApp) fields else (operations.firstOrNull { it.key == operation } ?: operations.firstOrNull())?.fields ?: emptyList()
}

class WorkflowCatalog(json: WorkflowJson) {
    val core: List<WorkflowNodeKind> = list(json["core"]).map { raw ->
        val item = map(raw)
        WorkflowNodeKind(
            text(item["type"]), text(item["name"]), text(item["group"]), text(item["description"]), text(item["color"]),
            item["trigger"] == true, list(item["outputs"]).map { text(it) }, list(item["fields"]).map(WorkflowField::of),
            emptyList(), emptyList(),
        )
    }
    val integrations: List<WorkflowNodeKind> = list(json["integrations"]).map { raw ->
        val item = map(raw)
        WorkflowNodeKind(
            text(item["type"]), text(item["name"]), text(item["category"]), text(item["description"]), text(item["color"]),
            false, listOf("main"), emptyList(), list(item["credential"]).map(WorkflowField::of),
            list(item["operations"]).map { op ->
                val o = map(op)
                WorkflowOperation(text(o["key"]), text(o["name"]), text(o["method"]), list(o["fields"]).map(WorkflowField::of))
            },
        )
    }
    val categories: List<String> = list(json["categories"]).map { text(it) }
    val operators: List<Pair<String, String>> = list(json["operators"]).map { map(it).let { o -> text(o["value"]) to text(o["label"]) } }

    fun kind(type: String): WorkflowNodeKind? = core.firstOrNull { it.type == type } ?: integrations.firstOrNull { it.type == type }
    val triggers: List<WorkflowNodeKind> get() = core.filter { it.trigger }
    val steps: List<WorkflowNodeKind> get() = core.filter { !it.trigger }
    fun title(type: String): String = kind(type)?.name ?: type
    fun outputs(type: String): List<String> = kind(type)?.outputs ?: listOf("main")

    fun search(query: String): List<WorkflowNodeKind> {
        val needle = query.trim().lowercase()
        val all = steps + integrations
        if (needle.isEmpty()) return all
        return all.filter { needle in it.name.lowercase() || needle in it.group.lowercase() || needle in it.description.lowercase() }
    }

    fun summary(node: WorkflowNode): String {
        val kind = kind(node.type)
        val parts = mutableListOf<String>()
        when (node.type) {
            "core.http" -> parts.add(listOf(node.text("method").ifEmpty { "GET" }, node.text("url")).filter { it.isNotEmpty() }.joinToString(" "))
            "core.if", "core.filter" -> {
                val op = operators.firstOrNull { it.first == node.text("operator") }?.second ?: node.text("operator")
                parts.add(listOf(node.text("left"), op, node.text("right")).filter { it.isNotEmpty() }.joinToString(" "))
            }
            "core.schedule" -> parts.add(WorkflowsText.every(node.text("every").toIntOrNull() ?: 0))
            "core.limit" -> parts.add("Keep " + node.text("max").ifEmpty { "10" })
            "core.split" -> parts.add(node.text("field"))
            else -> parts.add(
                if (kind?.isApp == true) {
                    kind.operations.firstOrNull { it.key == node.operation }?.name ?: kind.operations.firstOrNull()?.name ?: ""
                } else {
                    kind?.description ?: ""
                },
            )
        }
        if (node.disabled) parts.add("Off")
        return parts.filter { it.isNotEmpty() }.joinToString(" · ")
    }

    fun branch(connection: WorkflowConnection, draft: WorkflowDraft): String {
        val from = draft.node(connection.from)?.name ?: connection.from
        return if (connection.output == "main") "After $from" else "$from: ${connection.output}"
    }

    companion object {
        val EMPTY = WorkflowCatalog(emptyMap())
    }
}

data class WorkflowSummary(
    val id: String,
    val name: String,
    val active: Boolean,
    val trigger: String,
    val steps: Int,
    val scheduleMinutes: Int,
    val lastRunAt: Long,
    val lastStatus: String,
    val runsOnDesktop: Boolean = false,
) {
    companion object {
        fun of(json: WorkflowJson) = WorkflowSummary(
            text(json["id"]), text(json["name"], "Untitled workflow"), json["active"] == true, text(json["trigger"]),
            (json["nodes"] as? List<*>)?.size ?: long(json["nodes"]).toInt(), long(json["scheduleMinutes"]).toInt(),
            long(json["lastRunAt"]), text(json["lastStatus"]), text(json["runsOn"]) == "desktop",
        )
    }
}

data class WorkflowConnection(val from: String, val output: String = "main", val to: String) {
    fun toJson(): WorkflowJson = linkedMapOf("from" to from, "output" to output, "to" to to)
}

data class WorkflowNode(
    val id: String,
    val type: String,
    val name: String,
    val x: Long = 0,
    val y: Long = 0,
    val params: Map<String, Any?> = emptyMap(),
    val credential: String = "",
    val disabled: Boolean = false,
    val continueOnFail: Boolean = false,
) {
    val isTrigger: Boolean get() = type in setOf("core.manual", "core.webhook", "core.schedule")
    val operation: String get() = text(params["operation"])

    fun text(key: String): String {
        val value = params[key] ?: return ""
        return if (value is String) value else Json.stringify(value)
    }

    fun withParam(key: String, value: Any?): WorkflowNode = copy(params = LinkedHashMap(params).apply { put(key, value) })

    fun toJson(): WorkflowJson = linkedMapOf(
        "id" to id, "type" to type, "name" to name, "x" to x, "y" to y, "params" to params, "credential" to credential,
        "disabled" to disabled, "continueOnFail" to continueOnFail,
    )

    companion object {
        fun of(json: WorkflowJson) = WorkflowNode(
            text(json["id"]), text(json["type"]), text(json["name"]), long(json["x"]), long(json["y"]), map(json["params"]),
            text(json["credential"]), json["disabled"] == true, json["continueOnFail"] == true,
        )
    }
}

data class WorkflowDraft(
    val id: String = "",
    val name: String = "Untitled workflow",
    val active: Boolean = false,
    val nodes: List<WorkflowNode> = emptyList(),
    val connections: List<WorkflowConnection> = emptyList(),
    val hookPath: String = "",
) {
    val graph: WorkflowJson
        get() = linkedMapOf("name" to name, "nodes" to nodes.map { it.toJson() }, "connections" to connections.map { it.toJson() })

    val body: WorkflowJson get() = LinkedHashMap(graph).apply { put("active", active) }

    val trigger: WorkflowNode? get() = nodes.firstOrNull { it.isTrigger }

    fun node(id: String): WorkflowNode? = nodes.firstOrNull { it.id == id }
    fun inputs(id: String): List<WorkflowConnection> = connections.filter { it.to == id }
    fun outputs(id: String): List<WorkflowConnection> = connections.filter { it.from == id }

    val ordered: List<WorkflowNode>
        get() {
            val incoming = LinkedHashMap<String, Int>()
            val edges = LinkedHashMap<String, MutableList<String>>()
            for (node in nodes) {
                incoming[node.id] = 0
                edges[node.id] = mutableListOf()
            }
            for (c in connections) {
                if (c.to !in incoming || c.from !in edges) continue
                edges.getValue(c.from).add(c.to)
                incoming[c.to] = incoming.getValue(c.to) + 1
            }
            val ready = ArrayDeque(nodes.filter { incoming[it.id] == 0 }.map { it.id })
            val order = mutableListOf<String>()
            while (ready.isNotEmpty()) {
                val current = ready.removeFirst()
                order.add(current)
                for (target in edges.getValue(current)) {
                    incoming[target] = incoming.getValue(target) - 1
                    if (incoming[target] == 0) ready.addLast(target)
                }
            }
            val seen = order.toSet()
            return order.mapNotNull { node(it) } + nodes.filter { it.id !in seen }
        }

    fun uniqueName(base: String): String {
        val names = nodes.map { it.name }.toSet()
        val root = base.take(56)
        if (root !in names) return root
        var index = 2
        while ("$root $index" in names) index++
        return "$root $index"
    }

    fun newId(type: String): String {
        val stem = type.substringAfterLast('.').filter { it.isLetterOrDigit() }.ifEmpty { "step" }.take(24)
        val ids = nodes.map { it.id }.toSet()
        var index = 1
        while ("$stem$index" in ids) index++
        return "$stem$index"
    }

    fun nameTaken(name: String, except: String): Boolean = nodes.any { it.id != except && it.name == name }

    fun addStep(kind: WorkflowNodeKind, after: String?, output: String = "main"): Pair<WorkflowDraft, WorkflowNode> {
        val anchor = after?.let { node(it) } ?: ordered.lastOrNull()
        val siblings = anchor?.let { outputs(it.id).size } ?: 0
        val params = LinkedHashMap<String, Any?>()
        if (kind.isApp) kind.operations.firstOrNull()?.let { params["operation"] = it.key }
        for (field in kind.fields(text(params["operation"]))) if (field.defaultValue.isNotEmpty()) params[field.key] = field.defaultValue
        val step = WorkflowNode(
            newId(kind.type), kind.type, uniqueName(kind.name), (anchor?.x ?: 0) + COLUMN, (anchor?.y ?: 0) + siblings * ROW, params,
        )
        val links = if (anchor != null && !kind.trigger) connections + WorkflowConnection(anchor.id, output, step.id) else connections
        return copy(nodes = nodes + step, connections = links) to step
    }

    fun setTrigger(kind: WorkflowNodeKind): WorkflowDraft {
        if (!kind.trigger) return this
        val params = LinkedHashMap<String, Any?>()
        for (field in kind.fields) if (field.defaultValue.isNotEmpty()) params[field.key] = field.defaultValue
        val index = nodes.indexOfFirst { it.isTrigger }
        if (index < 0) return copy(nodes = listOf(WorkflowNode(newId(kind.type), kind.type, uniqueName(kind.name), params = params)) + nodes)
        val current = nodes[index]
        val name = if (nameTaken(kind.name, current.id)) uniqueName(kind.name) else kind.name
        return copy(nodes = nodes.toMutableList().apply { set(index, current.copy(type = kind.type, params = params, name = name)) })
    }

    fun remove(id: String): WorkflowDraft {
        val parents = inputs(id)
        val children = outputs(id)
        val kept = connections.filter { it.from != id && it.to != id }.toMutableList()
        for (parent in parents) for (child in children) {
            val joined = WorkflowConnection(parent.from, parent.output, child.to)
            if (joined !in kept) kept.add(joined)
        }
        return copy(nodes = nodes.filter { it.id != id }, connections = kept)
    }

    fun update(step: WorkflowNode): WorkflowDraft = copy(nodes = nodes.map { if (it.id == step.id) step else it })

    companion object {
        const val COLUMN = 288L
        const val ROW = 120L

        val BLANK = WorkflowDraft(nodes = listOf(WorkflowNode("trigger", "core.manual", "Run manually")))

        fun of(json: WorkflowJson) = WorkflowDraft(
            text(json["id"]), text(json["name"], "Untitled workflow"), json["active"] == true,
            list(json["nodes"]).map { WorkflowNode.of(map(it)) },
            list(json["connections"]).map { map(it).let { c -> WorkflowConnection(text(c["from"]), text(c["output"], "main"), text(c["to"])) } },
            text(json["hookPath"]),
        )
    }
}

data class WorkflowStepResult(
    val id: String,
    val name: String,
    val status: String,
    val error: String,
    val ms: Long,
    val outputs: List<Triple<String, Long, List<Any?>>>,
)

data class WorkflowRun(
    val id: String,
    val phone: Boolean,
    val trigger: String,
    val status: String,
    val error: String,
    val requests: Long,
    val startedAt: Long,
    val finishedAt: Long,
    val nodes: Map<String, Any?>,
) {
    val succeeded: Boolean get() = status == "success"
    val desktop: Boolean get() = trigger == "desktop"
    val hasDetail: Boolean get() = nodes.isNotEmpty()

    fun steps(draft: WorkflowDraft): List<WorkflowStepResult> {
        val known = draft.ordered.map { it.id to it.name }
        val extra = nodes.keys.filter { key -> known.none { it.first == key } }.map { it to it }
        return (known + extra).mapNotNull { (id, name) ->
            val report = nodes[id] as? Map<*, *> ?: return@mapNotNull null
            WorkflowStepResult(
                id, name, text(report["status"]), text(report["error"]), long(report["ms"]),
                map(report["outputs"]).map { (output, value) -> Triple(output, long(map(value)["count"]), list(map(value)["items"])) },
            )
        }
    }

    companion object {
        fun of(json: WorkflowJson, phone: Boolean, id: String? = null, startedAt: Long? = null, finishedAt: Long? = null) = WorkflowRun(
            id ?: text(json["id"]), phone, text(json["trigger"], "manual"), text(json["status"]), text(json["error"]),
            long(json["requests"]), startedAt ?: long(json["startedAt"]), finishedAt ?: long(json["finishedAt"]), map(json["nodes"]),
        )
    }
}

data class WorkflowCredential(val id: String, val integration: String, val name: String, val hint: Map<String, Any?>) {
    val hintText: String get() = hint.entries.joinToString(" · ") { "${it.key} ${text(it.value)}" }

    companion object {
        fun of(json: WorkflowJson) = WorkflowCredential(text(json["id"]), text(json["integration"]), text(json["name"]), map(json["hint"]))
    }
}

data class WorkflowTemplate(val key: String, val name: String, val description: String, val steps: Int) {
    companion object {
        fun of(json: WorkflowJson) = WorkflowTemplate(text(json["key"]), text(json["name"]), text(json["description"]), long(json["steps"]).toInt())
    }
}

class WorkflowsApi(private val call: (method: String, suffix: String, body: WorkflowJson?) -> WorkflowJson) {
    constructor(api: ApiService) : this({ method, suffix, body -> api.accountApiCall(method, "/api/workflows$suffix", body) })

    fun list(): List<WorkflowSummary> = list(call("GET", "", null)["workflows"]).map { WorkflowSummary.of(map(it)) }
    fun catalog(): WorkflowCatalog = WorkflowCatalog(call("GET", "/catalog", null))
    fun open(id: String): WorkflowDraft = WorkflowDraft.of(map(call("GET", "/$id", null)["workflow"]))

    fun save(draft: WorkflowDraft): WorkflowDraft =
        WorkflowDraft.of(map(call("POST", if (draft.id.isEmpty()) "" else "/${draft.id}", draft.body)["workflow"]))

    fun delete(id: String) {
        call("POST", "/$id/delete", null)
    }

    fun run(id: String): WorkflowRun = WorkflowRun.of(map(call("POST", "/$id/run", LinkedHashMap())["run"]), phone = false)
    fun runs(id: String): List<WorkflowRun> = list(call("GET", "/$id/runs", null)["runs"]).map { WorkflowRun.of(map(it), phone = false) }
    fun record(id: String, body: WorkflowJson): WorkflowRun = WorkflowRun.of(map(call("POST", "/$id/record", body)["run"]), phone = false)
    fun runDetail(id: String, runId: String): WorkflowRun = WorkflowRun.of(map(call("GET", "/$id/runs/$runId", null)["run"]), phone = false)
    fun templates(): List<WorkflowTemplate> = list(call("GET", "/templates", null)["templates"]).map { WorkflowTemplate.of(map(it)) }

    fun template(key: String): WorkflowDraft {
        val template = map(call("GET", "/templates/$key", null)["template"])
        val draft = WorkflowDraft.of(map(template["workflow"]))
        return if (draft.name == "Untitled workflow") draft.copy(name = text(template["name"], draft.name)) else draft
    }

    fun credentials(): List<WorkflowCredential> = list(call("GET", "/credentials", null)["credentials"]).map { WorkflowCredential.of(map(it)) }

    fun createCredential(integration: String, name: String, values: Map<String, String>): WorkflowCredential = WorkflowCredential.of(
        map(call("POST", "/credentials", linkedMapOf("integration" to integration, "name" to name, "values" to values.toSortedMap()))["credential"]),
    )

    fun deleteCredential(id: String) {
        call("POST", "/credentials/$id/delete", null)
    }
}

class WorkflowPhoneCredentials(private val store: SecretStore) {
    fun has(id: String): Boolean = record(id) != null

    fun record(id: String): WorkflowJson? {
        if (id.isEmpty()) return null
        val record = Json.parseObject(store.read(PREFIX + id) ?: return null)
        return if (text(record["integration"]).isEmpty()) null else record
    }

    fun save(id: String, integration: String, values: Map<String, String>) {
        store.write(PREFIX + id, Json.stringify(linkedMapOf("integration" to integration, "values" to values.toSortedMap())))
    }

    fun remove(id: String) = store.delete(PREFIX + id)

    fun credentials(draft: WorkflowDraft): WorkflowJson {
        val out = LinkedHashMap<String, Any?>()
        for (node in draft.nodes) if (node.credential.isNotEmpty()) record(node.credential)?.let { out[node.credential] = it }
        return out
    }

    companion object {
        const val PREFIX = "workflows.credential."
    }
}

class WorkflowExchange(val status: Int, val location: String?, val body: ByteArray)

class HttpWorkflowTransport(
    private val userAgent: String,
    private val timeoutMs: Int = 30_000,
    private val maxBytes: Int = 4 * 262_144,
    private val exchange: (method: String, url: String, headers: Map<String, String>, body: ByteArray?, timeoutMs: Int, maxBytes: Int) -> WorkflowExchange =
        ::urlConnectionExchange,
) {
    constructor(engine: WorkflowEngine) : this(engine.userAgent, engine.requestTimeoutMs, engine.maxResponseChars * 4)

    fun send(request: WorkflowRequest, allowed: (String) -> Boolean): WorkflowReply {
        var method = request.method
        var address = request.url
        var body = request.body?.toByteArray(Charsets.UTF_8)
        val headers = LinkedHashMap(request.headers)
        if (headers.keys.none { it.equals("User-Agent", ignoreCase = true) }) headers["User-Agent"] = userAgent
        repeat(MAX_REDIRECTS + 1) {
            if (!allowed(address)) return WorkflowReply.NO_RESPONSE
            val response = try {
                exchange(method, address, headers, body, timeoutMs, maxBytes)
            } catch (_: IOException) {
                return WorkflowReply.NO_RESPONSE
            } catch (_: IllegalArgumentException) {
                return WorkflowReply.NO_RESPONSE
            }
            val location = response.location
            if (response.status in setOf(301, 302, 303, 307, 308) && !location.isNullOrEmpty()) {
                val next = try {
                    URI(address).resolve(location).toString()
                } catch (_: IllegalArgumentException) {
                    return WorkflowReply.NO_RESPONSE
                }
                if (response.status == 303 || (response.status in setOf(301, 302) && method == "POST")) {
                    method = "GET"
                    body = null
                    headers.keys.removeAll { it.equals("Content-Type", ignoreCase = true) }
                }
                address = next
                return@repeat
            }
            val bytes = if (response.body.size > maxBytes) response.body.copyOf(maxBytes) else response.body
            return WorkflowReply(response.status, String(bytes, Charsets.UTF_8))
        }
        return WorkflowReply.NO_RESPONSE
    }

    companion object {
        const val MAX_REDIRECTS = 5

        fun urlConnectionExchange(
            method: String, url: String, headers: Map<String, String>, body: ByteArray?, timeoutMs: Int, maxBytes: Int,
        ): WorkflowExchange {
            val connection = URL(url).openConnection() as HttpURLConnection
            try {
                connection.requestMethod = method
                connection.instanceFollowRedirects = false
                connection.connectTimeout = timeoutMs
                connection.readTimeout = timeoutMs
                connection.useCaches = false
                for ((name, value) in headers) connection.setRequestProperty(name, value)
                if (body != null) {
                    connection.doOutput = true
                    connection.setFixedLengthStreamingMode(body.size)
                    connection.outputStream.use { it.write(body) }
                }
                val status = connection.responseCode
                val stream = if (status >= 400) connection.errorStream else connection.inputStream
                val bytes = stream?.use { input ->
                    val out = java.io.ByteArrayOutputStream()
                    val buffer = ByteArray(16 * 1024)
                    while (out.size() < maxBytes) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        out.write(buffer, 0, read)
                    }
                    out.toByteArray()
                } ?: ByteArray(0)
                return WorkflowExchange(status, connection.getHeaderField("Location"), bytes)
            } finally {
                connection.disconnect()
            }
        }
    }
}

class WorkflowPhoneRunner(private val engine: WorkflowEngine, private val transport: HttpWorkflowTransport = HttpWorkflowTransport(engine)) {
    fun run(
        draft: WorkflowDraft,
        credentials: WorkflowJson,
        payload: Any? = null,
        clock: () -> Long = { System.currentTimeMillis() },
        workflows: List<WorkflowJson> = emptyList(),
    ): WorkflowRun {
        val started = clock()
        val report = engine.run(
            WorkflowEngine.Input(draft.graph, payload, credentials, "manual", draft.id, clock = clock, workflows = workflows),
        ) { request -> transport.send(request) { engine.safeUrl(it).isNotEmpty() } }
        return WorkflowRun.of(report, phone = true, id = "phone-$started", startedAt = started, finishedAt = clock())
    }
}

class WorkflowTargets(private val api: WorkflowsApi, private val phone: WorkflowPhoneCredentials) {
    class Resolved(val workflows: List<WorkflowJson>, val credentials: WorkflowJson)

    fun resolve(draft: WorkflowDraft): Resolved {
        val credentials = LinkedHashMap(phone.credentials(draft))
        if (refs(draft).isEmpty()) return Resolved(emptyList(), credentials)
        val summaries = try {
            api.list()
        } catch (_: Exception) {
            return Resolved(emptyList(), credentials)
        }
        val loaded = LinkedHashMap<String, WorkflowDraft>()
        var level = listOf(draft)
        repeat(WorkflowActions.MAX_WORKFLOW_DEPTH - 1) {
            val wanted = LinkedHashSet<String>()
            for (current in level) {
                for (ref in refs(current)) {
                    if (ref.contains("{{")) {
                        summaries.forEach { wanted.add(it.id) }
                    } else {
                        val match = summaries.firstOrNull { it.id == ref } ?: summaries.firstOrNull { it.name.trim().lowercase() == ref.lowercase() }
                        match?.let { wanted.add(it.id) }
                    }
                }
            }
            val next = ArrayList<WorkflowDraft>()
            for (id in wanted) {
                if (loaded.containsKey(id) || id == draft.id) continue
                val opened = try {
                    api.open(id)
                } catch (_: Exception) {
                    continue
                }
                loaded[id] = opened
                credentials.putAll(phone.credentials(opened))
                next.add(opened)
            }
            level = next
        }
        if (draft.id.isNotEmpty() && summaries.any { it.id == draft.id }) loaded.putIfAbsent(draft.id, draft)
        val ordered = summaries.mapNotNull { summary -> loaded[summary.id]?.let { summary to it } }
        return Resolved(
            ordered.map { (summary, opened) -> linkedMapOf("id" to summary.id, "name" to summary.name, "workflow" to opened.graph) },
            credentials,
        )
    }

    private fun refs(draft: WorkflowDraft): List<String> =
        draft.nodes.filter { it.type == "core.runWorkflow" }.map { it.text("workflow").trim() }.filter { it.isNotEmpty() }
}
