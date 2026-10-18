package com.forkmesh.nativeapp.workflows

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.workflows.WorkflowPy.dumps
import com.forkmesh.nativeapp.workflows.WorkflowPy.get
import com.forkmesh.nativeapp.workflows.WorkflowPy.isMap
import com.forkmesh.nativeapp.workflows.WorkflowPy.list
import com.forkmesh.nativeapp.workflows.WorkflowPy.map
import com.forkmesh.nativeapp.workflows.WorkflowPy.or
import com.forkmesh.nativeapp.workflows.WorkflowPy.str
import com.forkmesh.nativeapp.workflows.WorkflowPy.stringify
import com.forkmesh.nativeapp.workflows.WorkflowPy.truthy

data class WorkflowRequest(val method: String, val url: String, val headers: Map<String, String>, val body: String?) {
    fun toJson(): Map<String, Any?> = linkedMapOf("method" to method, "url" to url, "headers" to headers, "body" to body)
}

data class WorkflowReply(val status: Int, val text: String) {
    companion object {
        val NO_RESPONSE = WorkflowReply(0, "")
    }
}

class WorkflowNodeError(message: String) : Exception(message)

class WorkflowEngine(val spec: Map<String, Any?>) {
    private val limits = map(spec["limits"])
    private val coreNodes = map(spec["coreNodes"])
    private val integrations = map(spec["integrations"])
    private val maxItems = limit("maxItems")

    val version: String get() = spec["version"] as? String ?: ""
    val userAgent: String get() = spec["userAgent"] as? String ?: ""
    val requestTimeoutMs: Int get() = limit("requestTimeoutSeconds") * 1000
    val maxResponseChars: Int get() = limit("maxResponseChars")

    private fun limit(key: String): Int = (limits[key] as? Number)?.toInt() ?: 0

    companion object {
        const val SPEC_RESOURCE = "/workflows/workflow-spec.json"
        private val NODE_ID_RE = Regex("^[A-Za-z0-9_-]{1,32}$")
        private val CREDENTIAL_ID_RE = Regex("^[0-9a-f]{32}$")
        private val EXPRESSION_RE = Regex("\\{\\{\\s*(.*?)\\s*\\}\\}", RegexOption.DOT_MATCHES_ALL)
        private val PLACEHOLDER_RE = Regex("\\{([A-Za-z][A-Za-z0-9_]*)\\}")
        private val ROOT_RE = Regex("^\\$([A-Za-z_]+)")
        private val STEP_RE = java.util.regex.Pattern.compile("\\.([A-Za-z_$][\\p{L}\\p{N}_$-]*)|\\[\\s*(-?[0-9]+)\\s*\\]|\\[\\s*\"([^\"]*)\"\\s*\\]|\\[\\s*'([^']*)'\\s*\\]")
        private val BODY_METHODS = setOf("POST", "PUT", "PATCH", "DELETE")
        private val TRIGGERS = setOf("core.manual", "core.webhook", "core.schedule")

        fun bundled(): WorkflowEngine {
            val stream = WorkflowEngine::class.java.getResourceAsStream(SPEC_RESOURCE)
                ?: throw IllegalStateException("workflow spec is missing")
            return WorkflowEngine(Json.parseObject(stream.use { String(it.readBytes(), Charsets.UTF_8) }))
        }
    }

    private fun fail(message: String): Nothing = throw WorkflowNodeError(message)

    private fun appKey(type: String) = if (type.startsWith("app.")) type.substring(4) else ""

    private fun app(key: String): Map<String, Any?> = map(integrations[key])

    fun nodeSpec(type: String): Map<String, Any?>? {
        if (coreNodes.containsKey(type)) return map(coreNodes[type])
        val key = appKey(type)
        if (key.isEmpty() || !integrations.containsKey(key)) return null
        return mapOf("outputs" to listOf("main"), "app" to integrations[key])
    }

    fun isTrigger(type: String): Boolean = coreNodes.containsKey(type) && get(coreNodes[type], "trigger") == true

    fun outputsFor(type: String): List<String> = nodeSpec(type)?.let { spec -> list(spec["outputs"]).map { it.toString() } } ?: emptyList()

    private fun fieldsFor(type: String, params: Map<String, Any?>): List<Map<String, Any?>> {
        if (coreNodes.containsKey(type)) return list(get(coreNodes[type], "fields")).map { map(it) }
        val key = appKey(type)
        if (key.isEmpty() || !integrations.containsKey(key)) return emptyList()
        val operations = map(app(key)["operations"])
        val op = str(or(params["operation"], ""))
        return if (operations.containsKey(op)) list(get(operations[op], "fields")).map { map(it) } else emptyList()
    }

    private fun hostOf(url: String): String {
        val match = Regex("^[A-Za-z][A-Za-z0-9+.-]*://([^/?#]*)").find(url) ?: return ""
        val netloc = match.groupValues[1]
        val hostinfo = netloc.substring(netloc.lastIndexOf('@') + 1)
        if (hostinfo.contains('[')) return hostinfo.substring(hostinfo.indexOf('[') + 1).substringBefore(']').lowercase()
        return hostinfo.substringBefore(':').lowercase()
    }

    fun catalog(): Map<String, Any?> {
        val core = coreNodes.map { (key, raw) -> LinkedHashMap(map(raw)).apply { put("type", key) } }
        val apps = integrations.map { (key, raw) ->
            val app = map(raw)
            val base = app["base"] as? String ?: ""
            linkedMapOf(
                "key" to key,
                "type" to "app.$key",
                "name" to app["name"],
                "category" to app["category"],
                "color" to app["color"],
                "description" to app["description"],
                "host" to if (base.startsWith("https://")) hostOf(base) else "",
                "credential" to list(get(app["auth"], "fields")).map { field ->
                    val f = map(field)
                    linkedMapOf("key" to f["key"], "label" to f["label"], "secret" to (f["secret"] == true), "placeholder" to (f["placeholder"] ?: ""))
                },
                "operations" to map(app["operations"]).map { (opKey, opRaw) ->
                    val op = map(opRaw)
                    linkedMapOf("key" to opKey, "name" to op["name"], "method" to op["method"], "fields" to op["fields"])
                },
            )
        }
        val categories = apps.map { it["category"].toString() }.distinct().sorted()
        val found = LinkedHashMap<String, MutableList<String>>()
        for ((key, raw) in integrations) found.getOrPut(get(raw, "category").toString()) { mutableListOf() }.add(key)
        val prefix = spec["packPrefix"] as? String ?: ""
        val packs = found.map { (category, keys) ->
            linkedMapOf(
                "id" to prefix + category.lowercase().replace(Regex("[^a-z0-9]+"), "-").trim('-'),
                "category" to category, "integrations" to keys,
            )
        }
        val operators = list(spec["operators"]).map { pair -> list(pair).let { linkedMapOf("value" to it[0], "label" to it[1]) } }
        return linkedMapOf("core" to core, "integrations" to apps, "categories" to categories, "packs" to packs, "operators" to operators)
    }

    private fun cleanText(value: Any?, limit: Int): String {
        val text = str(value ?: "").replace("\u0000", "").trim()
        val count = text.codePointCount(0, text.length)
        return if (count <= limit) text else text.substring(0, text.offsetByCodePoints(0, limit))
    }

    private fun cleanValue(value: Any?): Any? = when (value) {
        is Boolean, is Number -> value
        is Map<*, *>, is List<*> -> if (dumps(value).length > limit("maxValueChars")) Invalid else value
        null -> ""
        else -> str(value).replace("\u0000", "").let { if (it.length > limit("maxValueChars")) Invalid else it }
    }

    private object Invalid

    private fun cleanParams(type: String, raw: Any?): Map<String, Any?>? {
        val params = map(raw)
        val clean = LinkedHashMap<String, Any?>()
        if (type.startsWith("app.")) {
            val operations = map(app(appKey(type))["operations"])
            var operation = str(or(params["operation"], ""))
            if (!operations.containsKey(operation)) operation = operations.keys.first()
            clean["operation"] = operation
        }
        for (field in fieldsFor(type, if (clean.isNotEmpty()) clean else params)) {
            val key = field["key"].toString()
            if (!params.containsKey(key)) continue
            val value = cleanValue(params[key])
            if (value === Invalid) return null
            clean[key] = value
        }
        return clean
    }

    private fun toInt(value: Any?, fallback: Int): Int {
        val number = WorkflowPy.toFloat(value) ?: return fallback
        if (!number.isFinite()) return fallback
        return number.toLong().toInt()
    }

    data class Cleaned(val workflow: Map<String, Any?>?, val error: String)

    fun clean(data: Any?): Cleaned {
        if (!isMap(data)) return Cleaned(null, "invalid_workflow")
        val input = map(data)
        val name = cleanText(input["name"], limit("maxName")).ifEmpty { "Untitled workflow" }
        val rawNodes = or(input["nodes"], emptyList<Any?>())
        val rawConnections = or(input["connections"], emptyList<Any?>())
        if (rawNodes !is List<*> || rawConnections !is List<*>) return Cleaned(null, "invalid_workflow")
        if (rawNodes.size > limit("maxNodes")) return Cleaned(null, "too_many_nodes")
        if (rawConnections.size > limit("maxConnections")) return Cleaned(null, "too_many_connections")
        val nodes = mutableListOf<Map<String, Any?>>()
        val ids = HashSet<String>()
        val names = HashSet<String>()
        var triggers = 0
        val position = limit("positionLimit")
        for (rawNode in rawNodes) {
            if (!isMap(rawNode)) return Cleaned(null, "invalid_node")
            val raw = map(rawNode)
            val id = str(or(raw["id"], ""))
            val type = str(or(raw["type"], ""))
            if (!NODE_ID_RE.matches(id) || id in ids) return Cleaned(null, "invalid_node")
            if (nodeSpec(type) == null) return Cleaned(null, "unknown_node_type")
            val nodeName = cleanText(raw["name"], limit("maxNodeName"))
            if (nodeName.isEmpty() || nodeName in names) return Cleaned(null, "duplicate_node_name")
            val params = cleanParams(type, raw["params"]) ?: return Cleaned(null, "value_too_long")
            val credential = str(or(raw["credential"], ""))
            if (credential.isNotEmpty() && !CREDENTIAL_ID_RE.matches(credential)) return Cleaned(null, "invalid_credential")
            if (isTrigger(type)) triggers++
            ids.add(id)
            names.add(nodeName)
            nodes.add(linkedMapOf(
                "id" to id, "type" to type, "name" to nodeName,
                "x" to toInt(raw["x"], 0).coerceIn(-position, position).toLong(),
                "y" to toInt(raw["y"], 0).coerceIn(-position, position).toLong(),
                "params" to params,
                "credential" to if (type.startsWith("app.")) credential else "",
                "continueOnFail" to (raw["continueOnFail"] == true),
                "disabled" to (raw["disabled"] == true),
            ))
        }
        if (triggers > 1) return Cleaned(null, "too_many_triggers")
        val byId = nodes.associateBy { it["id"].toString() }
        val connections = mutableListOf<Map<String, Any?>>()
        val seen = HashSet<Triple<String, String, String>>()
        for (rawConnection in rawConnections) {
            if (!isMap(rawConnection)) return Cleaned(null, "invalid_connection")
            val conn = map(rawConnection)
            val source = str(or(conn["from"], ""))
            val target = str(or(conn["to"], ""))
            val output = str(or(conn["output"], "main"))
            val from = byId[source]
            val to = byId[target]
            if (from == null || to == null || source == target) return Cleaned(null, "invalid_connection")
            if (output !in outputsFor(from["type"].toString())) return Cleaned(null, "invalid_connection")
            if (isTrigger(to["type"].toString())) return Cleaned(null, "invalid_connection")
            if (!seen.add(Triple(source, output, target))) continue
            connections.add(linkedMapOf("from" to source, "output" to output, "to" to target))
        }
        if (topologicalOrder(nodes, connections) == null) return Cleaned(null, "cycle")
        return Cleaned(linkedMapOf("name" to name, "nodes" to nodes, "connections" to connections), "")
    }

    fun topologicalOrder(nodes: List<Map<String, Any?>>, connections: List<Map<String, Any?>>): List<String>? {
        val incoming = LinkedHashMap<String, Int>()
        val edges = LinkedHashMap<String, MutableList<String>>()
        for (node in nodes) {
            incoming[node["id"].toString()] = 0
            edges[node["id"].toString()] = mutableListOf()
        }
        for (conn in connections) {
            edges.getValue(conn["from"].toString()).add(conn["to"].toString())
            incoming[conn["to"].toString()] = incoming.getValue(conn["to"].toString()) + 1
        }
        val ready = ArrayDeque(nodes.map { it["id"].toString() }.filter { incoming[it] == 0 })
        val order = mutableListOf<String>()
        while (ready.isNotEmpty()) {
            val current = ready.removeFirst()
            order.add(current)
            for (target in edges.getValue(current)) {
                incoming[target] = incoming.getValue(target) - 1
                if (incoming[target] == 0) ready.addLast(target)
            }
        }
        return if (order.size == nodes.size) order else null
    }

    fun triggerNode(workflow: Map<String, Any?>): Map<String, Any?>? =
        list(or(workflow["nodes"], emptyList<Any?>())).map { map(it) }.firstOrNull { isTrigger(it["type"].toString()) }

    fun scheduleMinutes(workflow: Map<String, Any?>): Int {
        val node = triggerNode(workflow) ?: return 0
        if (node["type"] != "core.schedule" || node["disabled"] == true) return 0
        val minutes = toInt(get(node["params"], "every"), 0)
        if (minutes <= 0) return 0
        return minutes.coerceIn(limit("scheduleMinMinutes"), limit("scheduleMaxMinutes"))
    }

    fun manualPayload(workflow: Map<String, Any?>, payload: Any?): Map<String, Any?> {
        if (isMap(payload)) return map(payload)
        val trigger = triggerNode(workflow)
        if (trigger != null && trigger["type"] == "core.manual") {
            val raw = get(trigger["params"], "payload")
            if (isMap(raw)) return map(raw)
            if (raw is String && raw.isNotBlank()) {
                val parsed = parseJson(raw) ?: return emptyMap()
                return if (isMap(parsed.value)) map(parsed.value) else linkedMapOf("value" to parsed.value)
            }
        }
        if (trigger != null && trigger["type"] == "core.webhook") {
            return linkedMapOf("method" to "POST", "query" to LinkedHashMap<String, Any?>(), "body" to LinkedHashMap<String, Any?>())
        }
        return emptyMap()
    }

    private class Parsed(val value: Any?)

    private fun parseJson(text: String): Parsed? = try {
        Parsed(Json.parse(text.trim()))
    } catch (_: Json.JsonException) {
        null
    } catch (_: RuntimeException) {
        null
    }

    fun safeUrl(value: Any?): String {
        val raw = str(or(value, "")).trim()
        if (raw.isEmpty() || raw.length > 4096 || raw.any { it == '\r' || it == '\n' || it == '\t' || it == ' ' || it == '\\' }) return ""
        val match = Regex("^([A-Za-z][A-Za-z0-9+.-]*)://([^/?#]*)").find(raw) ?: return ""
        if (match.groupValues[1].lowercase() != "https") return ""
        val netloc = match.groupValues[2]
        if (netloc.contains('@')) return ""
        val open = netloc.indexOf('[')
        val close = netloc.indexOf(']')
        var host: String
        val port: String
        if (open >= 0 || close >= 0) {
            if (open < 0 || close < open) return ""
            host = netloc.substring(open + 1, close)
            val rest = netloc.substring(close + 1)
            port = if (rest.contains(':')) rest.substringAfter(':') else ""
        } else {
            host = netloc.substringBefore(':')
            port = if (netloc.contains(':')) netloc.substringAfter(':') else ""
        }
        host = host.lowercase().trimEnd('.')
        if (host.isEmpty()) return ""
        if (port.isNotEmpty()) {
            if (!port.all { it in '0'..'9' }) return ""
            val number = port.toBigInteger()
            if (number < 1.toBigInteger() || number > 65535.toBigInteger()) return ""
        }
        for (suffix in list(spec["blockedHostSuffixes"]).map { it.toString() }) {
            if (suffix.startsWith(".")) {
                if (host.endsWith(suffix)) return ""
            } else if (host == suffix || host.endsWith(".$suffix")) {
                return ""
            }
        }
        ipv4(host)?.let { return if (globalV4(it)) raw else "" }
        if (host.contains(':')) {
            val v6 = ipv6(host)
            return if (v6 != null && globalV6(v6)) raw else ""
        }
        if (host.all { it in '0'..'9' || it == '.' } || Regex("^0x[0-9a-f]+$").matches(host)) return ""
        if (!host.contains('.')) return ""
        return raw
    }

    private fun ipv4(host: String): IntArray? {
        val parts = host.split(".")
        if (parts.size != 4) return null
        val bytes = IntArray(4)
        for ((index, part) in parts.withIndex()) {
            if (!Regex("^(0|[1-9][0-9]{0,2})$").matches(part)) return null
            val value = part.toInt()
            if (value > 255) return null
            bytes[index] = value
        }
        return bytes
    }

    private fun globalV4(b: IntArray): Boolean {
        val blocked = listOf(
            intArrayOf(0, 0, 0, 0, 8), intArrayOf(10, 0, 0, 0, 8), intArrayOf(100, 64, 0, 0, 10), intArrayOf(127, 0, 0, 0, 8),
            intArrayOf(169, 254, 0, 0, 16), intArrayOf(172, 16, 0, 0, 12), intArrayOf(192, 0, 0, 0, 24), intArrayOf(192, 0, 2, 0, 24),
            intArrayOf(192, 88, 99, 0, 24), intArrayOf(192, 168, 0, 0, 16), intArrayOf(198, 18, 0, 0, 15), intArrayOf(198, 51, 100, 0, 24),
            intArrayOf(203, 0, 113, 0, 24), intArrayOf(224, 0, 0, 0, 4), intArrayOf(240, 0, 0, 0, 4),
        )
        val address = (b[0].toLong() shl 24) + (b[1] shl 16) + (b[2] shl 8) + b[3]
        for (net in blocked) {
            val base = (net[0].toLong() shl 24) + (net[1] shl 16) + (net[2] shl 8) + net[3]
            val size = 1L shl (32 - net[4])
            if (address >= base && address < base + size) return false
        }
        return true
    }

    private fun ipv6(host: String): IntArray? {
        val text = host.substringBefore('%')
        if (text.any { it !in '0'..'9' && it !in 'a'..'f' && it != ':' && it != '.' }) return null
        val halves = text.split("::")
        if (halves.size > 2) return null
        fun groups(part: String): List<Int>? {
            if (part.isEmpty()) return emptyList()
            val out = mutableListOf<Int>()
            val pieces = part.split(":")
            for ((index, piece) in pieces.withIndex()) {
                if (piece.contains('.') && index == pieces.size - 1) {
                    val v4 = ipv4(piece) ?: return null
                    out.add((v4[0] shl 8) or v4[1])
                    out.add((v4[2] shl 8) or v4[3])
                } else if (Regex("^[0-9a-f]{1,4}$").matches(piece)) {
                    out.add(piece.toInt(16))
                } else {
                    return null
                }
            }
            return out
        }
        val head = groups(halves[0]) ?: return null
        val tail = if (halves.size == 2) groups(halves[1]) ?: return null else emptyList()
        if (halves.size == 1) return if (head.size == 8) head.toIntArray() else null
        if (head.size + tail.size > 7) return null
        return (head + List(8 - head.size - tail.size) { 0 } + tail).toIntArray()
    }

    private fun globalV6(g: IntArray): Boolean {
        if (g[0] < 0x2000 || g[0] > 0x3fff) return false
        if (g[0] == 0x2001 && (g[1] == 0x0db8 || g[1] < 0x0200)) return false
        if (g[0] == 0x2002) return false
        if (g[0] == 0x3fff && g[1] < 0x1000) return false
        return true
    }

    private fun step(value: Any?, key: Any): Any? {
        if (value is List<*>) {
            if (key !is Long) return null
            val size = value.size.toLong()
            if (key >= -size && key < size) return value[(if (key < 0) size + key else key).toInt()]
            return null
        }
        if (value is Map<*, *> && key is String) return value[key]
        return null
    }

    fun evaluate(expr: Any?, context: Map<String, Any?>): Any? {
        val text = str(or(expr, "")).trim()
        val match = ROOT_RE.find(text)
            ?: fail("Expressions start with \$json, \$node, \$trigger, \$now, \$today, \$item, \$vars or \$workflow: " + text.take(80))
        val root = match.groupValues[1]
        if (!context.containsKey(root)) fail("Unknown expression variable \$$root")
        var value = context[root]
        var position = match.range.last + 1
        val matcher = STEP_RE.matcher(text)
        while (position < text.length) {
            matcher.region(position, text.length)
            if (!matcher.lookingAt()) fail("Cannot read expression: " + text.take(80))
            value = when {
                matcher.group(1) != null -> step(value, matcher.group(1))
                matcher.group(2) != null -> step(value, matcher.group(2).toLong())
                matcher.group(3) != null -> step(value, matcher.group(3))
                else -> step(value, matcher.group(4))
            }
            position = matcher.end()
        }
        return value
    }

    fun resolve(value: Any?, context: Map<String, Any?>): Any? {
        if (value is List<*>) return value.map { resolve(it, context) }
        if (value is Map<*, *>) return value.entries.associateTo(LinkedHashMap()) { it.key.toString() to resolve(it.value, context) }
        if (value !is String || !value.contains("{{")) return value
        val trimmed = value.trim()
        val whole = EXPRESSION_RE.matchEntire(trimmed)
        if (whole != null) return evaluate(whole.groupValues[1], context)
        return EXPRESSION_RE.replace(value) { stringify(evaluate(it.groupValues[1], context)) }
    }

    private fun coerce(field: Map<String, Any?>, value: Any?): Any? {
        val kind = field["type"] as? String ?: "string"
        val label = field["label"].toString()
        val empty = value == null || (value is String && value.isBlank())
        if (empty) {
            if (field["required"] == true) fail("$label is required")
            return null
        }
        return when (kind) {
            "number" -> {
                if (value is Boolean) fail("$label must be a number")
                val number = WorkflowPy.toFloat(value) ?: fail("$label must be a number")
                if (value is Long) value else WorkflowPy.numberValue(number)
            }
            "boolean" -> if (value is Boolean) value else str(value).trim().lowercase() in setOf("true", "1", "yes", "on")
            "json" -> if (value is Map<*, *> || value is List<*>) value else (parseJson(str(value)) ?: fail("$label is not valid JSON")).value
            "value" -> value
            "select" -> {
                val allowed = list(field["options"]).map { get(it, "value").toString() }
                val text = stringify(value)
                if (allowed.isNotEmpty() && text !in allowed) fail("$label must be one of " + allowed.joinToString(", "))
                text
            }
            else -> stringify(value)
        }
    }

    private fun fieldValues(fields: List<Map<String, Any?>>, params: Map<String, Any?>, context: Map<String, Any?>): LinkedHashMap<String, Any?> {
        val values = LinkedHashMap<String, Any?>()
        for (field in fields) {
            val key = field["key"].toString()
            var raw = if (params.containsKey(key)) params[key] else field["default"]
            if (field["type"] == "code") {
                values[key] = coerce(field, raw)
                continue
            }
            if (field["type"] == "json" && raw is String && raw.isNotBlank()) {
                parseJson(raw)?.let { raw = it.value }
            }
            values[key] = coerce(field, resolve(raw, context))
        }
        return values
    }

    private fun fill(template: String, values: Map<String, Any?>, encode: Boolean, slashes: Set<String> = emptySet()): String =
        PLACEHOLDER_RE.replace(template) { match ->
            val key = match.groupValues[1]
            if (!values.containsKey(key)) return@replace match.value
            val text = stringify(values[key])
            if (!encode) text else WorkflowPy.quote(text, if (key in slashes) "/" else "")
        }

    private fun placeholders(template: String): List<String> = PLACEHOLDER_RE.findAll(template).map { it.groupValues[1] }.toList()

    private fun fillPresent(templates: Any?, cred: Map<String, Any?>): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        for ((name, template) in map(templates)) {
            val text = template.toString()
            if (placeholders(text).any { str(or(cred[it], "")).isEmpty() }) continue
            out[name] = fill(text, cred, false)
        }
        return out
    }

    private fun render(template: Any?, values: Map<String, Any?>): Any? = when (template) {
        is List<*> -> template.mapNotNull { render(it, values) }
        is Map<*, *> -> {
            val out = LinkedHashMap<String, Any?>()
            for ((key, item) in template) {
                val rendered = render(item, values)
                if (rendered != null) out[key.toString()] = rendered
            }
            out
        }
        is String -> {
            val whole = PLACEHOLDER_RE.matchEntire(template)
            if (whole != null && values.containsKey(whole.groupValues[1])) values[whole.groupValues[1]] else fill(template, values, false)
        }
        else -> template
    }

    private fun templateKeys(template: Any?, found: MutableSet<String> = HashSet()): Set<String> {
        when (template) {
            is List<*> -> template.forEach { templateKeys(it, found) }
            is Map<*, *> -> template.values.forEach { templateKeys(it, found) }
            is String -> found.addAll(placeholders(template))
        }
        return found
    }

    @Suppress("UNCHECKED_CAST")
    private fun setPath(target: MutableMap<String, Any?>, dotted: String, value: Any?) {
        val parts = dotted.split(".")
        var current = target
        for (part in parts.dropLast(1)) {
            val next = current[part]
            current = if (next is MutableMap<*, *>) {
                next as MutableMap<String, Any?>
            } else if (next is Map<*, *>) {
                LinkedHashMap(next as Map<String, Any?>).also { current[part] = it }
            } else {
                LinkedHashMap<String, Any?>().also { current[part] = it }
            }
        }
        current[parts.last()] = value
    }

    private fun formPairs(body: Map<*, *>, prefix: String = ""): List<Pair<Any?, Any?>> {
        val pairs = mutableListOf<Pair<Any?, Any?>>()
        for ((key, value) in body) {
            val name = if (prefix.isNotEmpty()) "$prefix[$key]" else key.toString()
            when (value) {
                is Map<*, *> -> pairs.addAll(formPairs(value, name))
                is List<*> -> value.forEach { pairs.add(name to stringify(it)) }
                else -> pairs.add(name to stringify(value))
            }
        }
        return pairs
    }

    private fun baseUrl(app: Map<String, Any?>, cred: Map<String, Any?>): String {
        var base = app["base"].toString()
        for (raw in list(get(app["auth"], "fields"))) {
            val field = map(raw)
            val token = "{" + field["key"] + "}"
            if (!base.contains(token)) continue
            val value = str(or(cred[field["key"].toString()], ""))
            base = base.replace(token, if (field["url"] == true) value.trimEnd('/') else WorkflowPy.quote(value, ":"))
        }
        return base
    }

    fun buildAppRequest(key: String, params: Map<String, Any?>, values: Map<String, Any?>, cred: Map<String, Any?>): WorkflowRequest {
        val app = app(key)
        val op = map(get(app["operations"], params["operation"].toString()))
        val method = op["method"].toString()
        val path = op["path"].toString()
        val pathKeys = placeholders(path).distinct()
        for (name in pathKeys) {
            val value = values[name]
            if (value == null || value == "") fail("Missing $name")
        }
        val fields = list(op["fields"]).map { map(it) }
        val slashes = fields.filter { it["slashes"] == true }.map { it["key"].toString() }.toSet()
        var url = baseUrl(app, cred) + fill(path, values, true, slashes)
        val headers = LinkedHashMap<String, String>()
        map(app["headers"]).forEach { (name, value) -> headers[name] = value.toString() }
        val query = LinkedHashMap<String, Any?>(map(op["query"]))
        val auth = map(app["auth"])
        headers.putAll(fillPresent(auth["headers"], cred))
        query.putAll(fillPresent(auth["query"], cred))
        auth["basic"]?.let { basic ->
            val pair = list(basic)
            headers["Authorization"] = "Basic " + WorkflowPy.base64(fill(pair[0].toString(), cred, false) + ":" + fill(pair[1].toString(), cred, false))
        }
        map(op["headers"]).forEach { (name, template) -> headers[name] = fill(template.toString(), values, false) }
        val bodyFields = mutableListOf<Pair<String, Any?>>()
        for (field in fields) {
            val fieldKey = field["key"].toString()
            val value = values[fieldKey]
            if (fieldKey in pathKeys || value == null || field["in"] == "template") continue
            val where = field["in"] as? String ?: if (method in BODY_METHODS) "body" else "query"
            when (where) {
                "query" -> query[fieldKey.split(".").last()] = stringify(value)
                "query_object" -> {
                    if (value !is Map<*, *>) fail(field["label"].toString() + " must be a JSON object")
                    for ((name, item) in value) query[name.toString()] = stringify(item)
                }
                "header" -> headers[(field["header"] as? String)?.ifEmpty { null } ?: fieldKey] = stringify(value)
                else -> bodyFields.add(fieldKey to value)
            }
        }
        var body: Any? = null
        if (op["body"] != null) {
            body = render(op["body"], values)
            if (body is MutableMap<*, *>) {
                @Suppress("UNCHECKED_CAST")
                val target = body as MutableMap<String, Any?>
                val keys = templateKeys(op["body"])
                for ((name, value) in bodyFields) if (name !in keys) setPath(target, name, value)
            }
        } else if (bodyFields.isNotEmpty()) {
            val target = LinkedHashMap<String, Any?>()
            for ((name, value) in bodyFields) setPath(target, name, value)
            body = target
        }
        if (auth["body"] != null && method in BODY_METHODS) {
            if (body == null) body = LinkedHashMap<String, Any?>()
            if (body is MutableMap<*, *>) {
                @Suppress("UNCHECKED_CAST")
                val target = body as MutableMap<String, Any?>
                for ((name, template) in map(auth["body"])) target[name] = fill(template.toString(), cred, false)
            }
        }
        val encoding = (op["encoding"] as? String)?.ifEmpty { null } ?: (app["encoding"] as? String)?.ifEmpty { null } ?: "json"
        var text: String? = null
        if (body != null && method in BODY_METHODS) {
            if (encoding == "form") {
                if (body !is Map<*, *>) fail("A form body must be a JSON object")
                text = WorkflowPy.urlencode(formPairs(body))
                headers.putIfAbsent("Content-Type", "application/x-www-form-urlencoded")
            } else {
                text = dumps(body)
                headers.putIfAbsent("Content-Type", "application/json")
            }
        }
        if (query.isNotEmpty()) url += (if (url.contains("?")) "&" else "?") + WorkflowPy.urlencode(query.map { it.key to it.value })
        headers.putIfAbsent("Accept", "application/json")
        return WorkflowRequest(method, url, headers, text)
    }

    fun buildHttpRequest(values: Map<String, Any?>): WorkflowRequest {
        val method = or(values["method"], "GET").toString()
        var url = str(or(values["url"], ""))
        val rawHeaders = or(values["headers"], emptyMap<String, Any?>())
        if (rawHeaders !is Map<*, *>) fail("Headers must be a JSON object")
        val headers = LinkedHashMap<String, String>()
        for ((name, value) in rawHeaders) headers[name.toString()] = stringify(value)
        val query = or(values["query"], emptyMap<String, Any?>())
        if (query !is Map<*, *>) fail("Query parameters must be a JSON object")
        if (query.isNotEmpty()) {
            url += (if (url.contains("?")) "&" else "?") + WorkflowPy.urlencode(query.map { it.key to stringify(it.value) })
        }
        var text: String? = null
        var body = values["body"]
        if (method in BODY_METHODS && body != null && body != "") {
            val kind = or(values["bodyType"], "json").toString()
            if (kind == "text") {
                text = stringify(body)
                headers.putIfAbsent("Content-Type", "text/plain")
            } else {
                if (body is String) body = (parseJson(body) ?: fail("Body is not valid JSON")).value
                if (kind == "form") {
                    if (body !is Map<*, *>) fail("A form body must be a JSON object")
                    text = WorkflowPy.urlencode(formPairs(body))
                    headers.putIfAbsent("Content-Type", "application/x-www-form-urlencoded")
                } else {
                    text = dumps(body)
                    headers.putIfAbsent("Content-Type", "application/json")
                }
            }
        }
        return WorkflowRequest(method, url, headers, text)
    }

    fun parseResponse(text: String): Any? {
        val body = text.take(maxResponseChars)
        val stripped = body.trim()
        if (stripped.startsWith("{") || stripped.startsWith("[")) parseJson(stripped)?.let { return it.value }
        return if (stripped.isNotEmpty()) linkedMapOf("body" to body) else LinkedHashMap<String, Any?>()
    }

    private fun texts(blocks: Any?): String {
        if (blocks !is List<*>) return ""
        return blocks.filter { it is Map<*, *> && (if (it.containsKey("type")) it["type"] else "text") == "text" }
            .joinToString("") { str(or(get(it, "text"), "")) }
    }

    private fun withText(output: String?, parsed: Any?): Any? {
        if (output.isNullOrEmpty() || parsed !is Map<*, *>) return parsed
        val text: Any? = when (output) {
            "anthropic" -> texts(parsed["content"])
            "openai" -> get(or(step(or(step(parsed["choices"], 0L), emptyMap<String, Any?>()), "message"), emptyMap<String, Any?>()), "content")
            "gemini" -> texts(get(or(step(or(step(parsed["candidates"], 0L), emptyMap<String, Any?>()), "content"), emptyMap<String, Any?>()), "parts"))
            "cohere" -> texts(get(or(parsed["message"], emptyMap<String, Any?>()), "content"))
            else -> null
        }
        if (text == null) return parsed
        @Suppress("UNCHECKED_CAST")
        return LinkedHashMap(parsed as Map<String, Any?>).apply { put("text", text) }
    }

    private fun itemsFrom(value: Any?): List<Any?> = when (value) {
        is List<*> -> value.take(maxItems).map { if (it is Map<*, *>) it else linkedMapOf("value" to it) }
        is Map<*, *> -> listOf(value)
        else -> listOf(linkedMapOf("value" to value))
    }

    private fun isEmptyValue(value: Any?): Boolean =
        value == null || value == "" || (value is List<*> && value.isEmpty()) || (value is Map<*, *> && value.isEmpty())

    fun checkCondition(left: Any?, operator: Any?, right: Any?): Boolean {
        when (operator) {
            "isEmpty" -> return isEmptyValue(left)
            "isNotEmpty" -> return !isEmptyValue(left)
            "isTrue" -> return left == true || stringify(left).lowercase() == "true"
            "isFalse" -> return left == false || stringify(left).lowercase() in setOf("false", "")
            "gt", "gte", "lt", "lte" -> {
                val a = if (left is Boolean) null else WorkflowPy.toFloat(left)
                val b = if (right is Boolean) null else WorkflowPy.toFloat(right)
                if (a == null || b == null) return false
                return when (operator) {
                    "gt" -> a > b
                    "gte" -> a >= b
                    "lt" -> a < b
                    else -> a <= b
                }
            }
        }
        val x = stringify(left)
        val y = stringify(right)
        return when (operator) {
            "equals" -> x == y
            "notEquals" -> x != y
            "contains" -> if (left is List<*>) left.any { stringify(it) == y } else x.contains(y)
            "notContains" -> if (left is List<*>) left.none { stringify(it) == y } else !x.contains(y)
            "startsWith" -> x.startsWith(y)
            "endsWith" -> x.endsWith(y)
            "matches", "notMatches" -> {
                val found = regexSearch(WorkflowActions.regex(y, false).compiled, x, 0) != null
                if (operator == "matches") found else !found
            }
            else -> fail("Unknown operator " + str(operator))
        }
    }

    private fun summarizeItems(items: List<Any?>): List<Any?> = items.take(limit("maxRunItems")).map { item ->
        val encoded = dumps(item)
        if (encoded.length > limit("maxRunItemChars")) linkedMapOf("_truncated" to encoded.take(limit("maxRunItemChars"))) else item
    }

    private fun isoTime(ms: Long): String =
        java.time.Instant.ofEpochSecond(Math.floorDiv(ms, 1000L)).toString().let { if (it.length == 20) it else it.take(19) + "Z" }

    class Input(
        val workflow: Any?,
        val payload: Any? = null,
        val credentials: Map<String, Any?> = emptyMap(),
        val trigger: String = "manual",
        val workflowId: String = "",
        val nowMs: Long? = null,
        val clock: () -> Long = { System.currentTimeMillis() },
        val randoms: List<Double>? = null,
        val workflows: List<Map<String, Any?>> = emptyList(),
    )

    private class Shared(
        val http: (WorkflowRequest) -> WorkflowReply,
        val sleep: (Long) -> Unit,
        val random: () -> Double,
        val workflows: List<Map<String, Any?>>,
        val credentials: Map<String, Any?>,
        val maxRequests: Int,
        val deadline: Long,
        val clock: () -> Long,
        val now: Long,
    ) {
        var requests = 0
    }

    fun run(input: Input, sleep: (Long) -> Unit = { Thread.sleep(it) }, http: (WorkflowRequest) -> WorkflowReply): Map<String, Any?> {
        val runLimits = map(spec["runLimits"])
        val limitPair = list(runLimits[input.trigger] ?: runLimits["manual"])
        val checked = clean(input.workflow)
        val workflow = checked.workflow
            ?: return linkedMapOf("status" to "error", "error" to "Workflow: " + checked.error, "requests" to 0L,
                                  "nodes" to LinkedHashMap<String, Any?>(), "trigger" to input.trigger)
        val queue = input.randoms?.let { ArrayDeque(it) }
        val random: () -> Double = if (queue != null) {
            { if (queue.isEmpty()) 0.5 else queue.removeFirst() }
        } else {
            { Math.random() }
        }
        val shared = Shared(
            http, sleep, random, input.workflows, input.credentials, (limitPair[0] as Number).toInt(),
            input.clock() + (limitPair[1] as Number).toLong(), input.clock, input.nowMs ?: input.clock(),
        )
        val execution = Execution(workflow, input.credentials, input.workflowId, shared, 0)
        val report = execution.run(manualPayload(workflow, input.payload))
        report["trigger"] = input.trigger
        return report
    }

    private fun loadWorkflow(shared: Shared, ref: String): Pair<Map<String, Any?>, String>? {
        val entry = shared.workflows.firstOrNull { str(or(it["id"], "")) == ref }
            ?: shared.workflows.firstOrNull { str(or(it["name"], "")).trim().lowercase() == ref.lowercase() }
            ?: return null
        val child = clean(entry["workflow"]).workflow ?: return null
        return child to str(or(entry["id"], ""))
    }

    @Suppress("UNCHECKED_CAST")
    private inner class Execution(
        val workflow: Map<String, Any?>,
        val credentials: Map<String, Any?>,
        val workflowId: String,
        val shared: Shared,
        val depth: Int,
    ) {
        val clock: () -> Long get() = shared.clock
        val deadline: Long get() = shared.deadline
        val now: Long get() = shared.now
        val outputs = LinkedHashMap<String, Map<String, List<Any?>>>()
        var triggerItem: Map<String, Any?> = emptyMap()
        var byName = LinkedHashMap<String, String>()
        var variables: MutableMap<String, Any?> = LinkedHashMap()
        val logs = HashMap<String, List<String>>()
        var stopped = ""

        fun firstOutput(id: String): List<Any?> = outputs[id]?.values?.firstOrNull { it.isNotEmpty() } ?: emptyList()

        fun context(item: Any?, index: Int): Map<String, Any?> {
            val nodes = LinkedHashMap<String, Any?>()
            for ((name, id) in byName) {
                val main = firstOutput(id)
                if (main.isNotEmpty()) nodes[name] = linkedMapOf("json" to if (index < main.size) main[index] else main[0])
            }
            val stamp = isoTime(now)
            return linkedMapOf(
                "json" to item, "item" to linkedMapOf("index" to index.toLong()), "node" to nodes, "trigger" to triggerItem,
                "now" to stamp, "today" to stamp.take(10), "vars" to variables,
                "workflow" to linkedMapOf("id" to workflowId, "name" to (workflow["name"] as? String ?: "")),
            )
        }

        fun values(node: Map<String, Any?>, item: Any?, index: Int): LinkedHashMap<String, Any?> {
            val params = map(or(node["params"], emptyMap<String, Any?>()))
            return fieldValues(fieldsFor(node["type"].toString(), params), params, context(item, index))
        }

        fun readPath(path: Any?, item: Any?, index: Int): Any? {
            val text = str(or(path, "")).trim()
            if (text.isEmpty()) return null
            return evaluate(if (text.startsWith("$")) text else "\$json.$text", context(item, index))
        }

        fun request(request: WorkflowRequest): Any? {
            if (safeUrl(request.url).isEmpty()) fail("Only public https:// URLs can be called")
            if (shared.requests >= shared.maxRequests) fail("This run reached its limit of ${shared.maxRequests} requests")
            if (clock() > deadline) fail("This run ran out of time")
            shared.requests += 1
            val reply = shared.http(request)
            if (reply.status == 0) fail("No response from " + hostOf(request.url).ifEmpty { request.url })
            val parsed = parseResponse(reply.text)
            if (reply.status >= 400) fail("HTTP ${reply.status}: " + reply.text.trim().take(300))
            return parsed
        }

        fun runNode(node: Map<String, Any?>, items: List<Any?>): Map<String, List<Any?>> {
            val type = node["type"].toString()
            val params = map(or(node["params"], emptyMap<String, Any?>()))
            if (type in TRIGGERS) return linkedMapOf("main" to listOf(triggerItem))
            if (node["disabled"] == true) return linkedMapOf(outputsFor(type)[0] to items)
            if (type == "core.merge" || type == "core.noop" || type == "core.comment") return linkedMapOf("main" to items.take(maxItems))
            if (type == "core.limit") {
                val values = fieldValues(fieldsFor(type, params), params, context(LinkedHashMap<String, Any?>(), 0))
                val max = (values["max"] as Number).toDouble().toLong().coerceAtLeast(0)
                return linkedMapOf("main" to items.take(max.coerceAtMost(Int.MAX_VALUE.toLong()).toInt()))
            }
            if (type in WorkflowActions.TYPES) return runAction(node, items.take(maxItems))
            val out = LinkedHashMap<String, MutableList<Any?>>()
            for (name in outputsFor(type)) out[name] = mutableListOf()
            for ((index, item) in items.take(maxItems).withIndex()) {
                val context = context(item, index)
                val values = fieldValues(fieldsFor(type, params), params, context)
                when (type) {
                    "core.set" -> {
                        val extra = values["values"]
                        if (extra !is Map<*, *>) fail("Fields must be a JSON object")
                        val merged = if (values["keep"] != false) LinkedHashMap(map(item)) else LinkedHashMap<String, Any?>()
                        for ((key, value) in extra) setPath(merged, key.toString(), value)
                        out.getValue("main").add(merged)
                    }
                    "core.if", "core.filter" -> {
                        val passed = checkCondition(values["left"], values["operator"], values["right"])
                        if (type == "core.filter") {
                            if (passed) out.getValue("main").add(item)
                        } else {
                            out.getValue(if (passed) "true" else "false").add(item)
                        }
                    }
                    "core.split" -> {
                        val field = values["field"].toString()
                        val found = evaluate(if (field.startsWith("$")) field else "\$json.$field", context)
                        if (found is List<*>) out.getValue("main").addAll(itemsFrom(found))
                    }
                    "core.http" -> out.getValue("main").addAll(itemsFrom(request(buildHttpRequest(values))))
                    else -> {
                        val key = appKey(type)
                        val app = app(key)
                        var cred: Map<String, Any?> = emptyMap()
                        if (list(get(app["auth"], "fields")).isNotEmpty()) {
                            val record = credentials[str(or(node["credential"], ""))]
                            if (record !is Map<*, *> || record["integration"] != key) fail("Choose a " + app["name"] + " credential for this step")
                            cred = map(or(record["values"], emptyMap<String, Any?>()))
                        }
                        val parsed = request(buildAppRequest(key, params, values, cred))
                        val output = get(get(app["operations"], params["operation"].toString()), "output") as? String
                        out.getValue("main").addAll(itemsFrom(withText(output, parsed)))
                    }
                }
                for (list in out.values) while (list.size > maxItems) list.removeAt(list.size - 1)
            }
            return out
        }

        fun runAction(node: Map<String, Any?>, items: List<Any?>): Map<String, List<Any?>> {
            val type = node["type"].toString()
            val params = map(or(node["params"], emptyMap<String, Any?>()))
            when (type) {
                "core.code" -> return linkedMapOf("main" to runScript(node, items).take(maxItems))
                "core.wait" -> {
                    waitFor(values(node, items[0], 0)["seconds"])
                    return linkedMapOf("main" to items)
                }
                "core.stop" -> {
                    val values = values(node, items[0], 0)
                    if (values["outcome"] == "error") fail((values["message"] as? String)?.ifEmpty { null } ?: "The workflow was stopped")
                    throw WorkflowStop(linkedMapOf("main" to items))
                }
                "core.pick" -> return linkedMapOf("main" to pick(values(node, items[0], 0), items))
                "core.sort" -> {
                    val values = values(node, items[0], 0)
                    val keys = items.mapIndexed { index, item -> WorkflowActions.sortKey(sortValue(params["by"], item, index)) }
                    return linkedMapOf("main" to WorkflowActions.sortItems(items, keys, values["order"].toString(), shared.random))
                }
                "core.dedupe" -> {
                    val by = params["by"]
                    val seen = HashSet<String>()
                    val kept = ArrayList<Any?>()
                    for ((index, item) in items.withIndex()) {
                        val key = dumps(if (str(or(by, "")).trim().isNotEmpty()) readPath(by, item, index) else item)
                        if (seen.add(key)) kept.add(item)
                    }
                    return linkedMapOf("main" to kept)
                }
                "core.statistics" -> {
                    val numbers = items.mapIndexed { index, item -> values(node, item, index)["value"] }
                    val values = values(node, items[0], 0)
                    val result = WorkflowActions.statistics(numbers, values["operation"].toString())
                    return linkedMapOf("main" to listOf(WorkflowActions.withField(LinkedHashMap<String, Any?>(), values["field"].toString(), WorkflowActions.numberOut(result))))
                }
                "core.combineItems" -> {
                    val whole = str(or(params["value"], "")).trim().isEmpty()
                    val gathered = ArrayList(items.mapIndexed { index, item -> if (whole) WorkflowActions.copyJson(item) else values(node, item, index)["value"] })
                    val field = values(node, items[0], 0)["field"].toString()
                    return linkedMapOf("main" to listOf(WorkflowActions.withField(LinkedHashMap<String, Any?>(), field, gathered)))
                }
            }
            val out = LinkedHashMap<String, MutableList<Any?>>()
            for (name in outputsFor(type)) out[name] = mutableListOf()
            for ((index, item) in items.withIndex()) {
                val values = values(node, item, index)
                when (type) {
                    "core.switch" -> {
                        val text = stringify(values["value"])
                        var routed = false
                        for (case in listOf("case1", "case2", "case3")) {
                            val expected = values[case]
                            if (expected != null && expected != "" && expected == text) {
                                out.getValue(case).add(item)
                                routed = true
                                break
                            }
                        }
                        if (!routed) out.getValue("otherwise").add(item)
                    }
                    "core.repeat" -> {
                        val times = (values["times"] as Number).toDouble().coerceIn(0.0, WorkflowActions.MAX_REPEAT.toDouble()).toInt()
                        for (count in 1..times) out.getValue("main").add(WorkflowActions.withField(item, values["field"].toString(), count.toLong()))
                    }
                    "core.variable" -> {
                        setVariable(values)
                        out.getValue("main").add(item)
                    }
                    "core.runWorkflow" -> out.getValue("main").addAll(runChild(values["workflow"], item))
                    else -> out.getValue("main").add(WorkflowActions.withField(item, values["field"].toString(), compute(type, values)))
                }
                for (list in out.values) while (list.size > maxItems) list.removeAt(list.size - 1)
            }
            return out
        }

        fun compute(type: String, values: Map<String, Any?>): Any? {
            val text = values["value"].let { if (it == null) "" else stringify(it) }
            fun s(key: String) = (values[key] as? String) ?: ""
            return when (type) {
                "core.textCase" -> WorkflowActions.changeCase(text, s("case"))
                "core.textReplace" -> WorkflowActions.replaceText(text, s("find"), s("replace"), values["regex"] == true, values["ignoreCase"] == true)
                "core.textMatch" -> WorkflowActions.matchText(text, s("pattern"), s("result"), values["ignoreCase"] == true)
                "core.textSplit" -> WorkflowActions.splitText(text, s("separator"), s("custom"), values["skipEmpty"] != false)
                "core.textCombine" -> WorkflowActions.combineText(values["value"], s("separator"), s("custom"))
                "core.textEncode" -> WorkflowActions.encodeText(text, s("method"))
                "core.hash" -> WorkflowActions.hashText(text, s("algorithm"), s("key"), s("encoding"))
                "core.textExtract" -> WorkflowActions.extract(text, s("kind"))
                "core.count" -> WorkflowActions.count(values["value"], s("what"))
                "core.math" -> {
                    val second = values["b"]
                    WorkflowActions.numberOut(WorkflowActions.calculate(
                        WorkflowActions.toNumber(values["a"], "Number"), s("operator"),
                        if (second == null) null else WorkflowActions.toNumber(second, "Second number"),
                    ))
                }
                "core.round" -> WorkflowActions.numberOut(WorkflowActions.roundNumber(WorkflowActions.toNumber(values["value"], "Number"), values["decimals"], s("mode")))
                "core.formatNumber" -> WorkflowActions.formatNumber(WorkflowActions.toNumber(values["value"], "Number"), values["decimals"], values["grouping"] != false)
                "core.random" -> WorkflowActions.numberOut(randomNumber(values))
                "core.json" -> jsonValue(values)
                "core.url" -> WorkflowActions.urlParts(text)
                "core.dateFormat" -> WorkflowActions.formatDate(values["value"], s("format"), s("pattern"), s("timeZone"))
                "core.dateAdjust" -> WorkflowActions.adjustDate(values["value"], s("operation"), values["amount"], s("unit"), s("timeZone"))
                "core.dateDiff" -> WorkflowActions.dateDifference(values["start"], values["end"], s("unit"))
                else -> fail("Unknown step $type")
            }
        }

        fun jsonValue(values: Map<String, Any?>): Any? {
            val value = values["value"]
            if (values["operation"] == "stringify") return if (values["pretty"] == true) WorkflowActions.pyDumpsIndent(value) else dumps(value)
            if (value == null) fail("There is no JSON text to read")
            if (value !is String) return value
            return try {
                PyJson.loads(value)
            } catch (_: PyJson.Bad) {
                fail("The text is not valid JSON")
            }
        }

        fun randomNumber(values: Map<String, Any?>): Any {
            val low = (values["min"] as Number).toDouble()
            val high = (values["max"] as Number).toDouble()
            if (values["whole"] != false) {
                val lo = Math.ceil(low)
                val hi = Math.floor(high)
                if (lo > hi) fail("From must not be larger than To")
                return lo.toLong() + Math.floor(shared.random() * (hi - lo + 1)).toLong()
            }
            if (low > high) fail("From must not be larger than To")
            return low + shared.random() * (high - low)
        }

        fun setVariable(values: Map<String, Any?>) {
            val name = (values["name"] as? String) ?: ""
            if (!WorkflowActions.VARIABLE_RE.matches(name)) fail("Variable names use letters, digits and _")
            val value = WorkflowActions.copyJson(values["value"])
            val next = variables
            when (values["mode"]) {
                "set" -> next[name] = value
                "append" -> {
                    val existing = next[name]
                    val items: ArrayList<Any?> = if (existing is List<*>) ArrayList<Any?>(existing) else if (existing == null) ArrayList() else arrayListOf(existing)
                    items.add(value)
                    next[name] = ArrayList(items.takeLast(maxItems * 10))
                }
                else -> {
                    val existing = next[name]
                    val base = if (existing == null) 0.0 else WorkflowActions.toNumber(existing, "The variable $name")
                    next[name] = WorkflowActions.numberOut(base + WorkflowActions.toNumber(value, "Value"))
                }
            }
        }

        fun sortValue(by: Any?, item: Any?, index: Int): Any? {
            if (str(or(by, "")).trim().isNotEmpty()) return readPath(by, item, index)
            return map(item).values.firstOrNull()
        }

        fun pick(values: Map<String, Any?>, items: List<Any?>): List<Any?> {
            when (values["which"]) {
                "first" -> return items.take(1)
                "last" -> return items.takeLast(1)
                "random" -> return listOf(items[Math.floor(shared.random() * items.size).toInt()])
            }
            var position = (values["index"] as? Number)?.toDouble()?.toLong() ?: 0L
            if (values["which"] == "index") {
                if (position < 0) position += items.size + 1
                return if (position >= 1 && position <= items.size) listOf(items[(position - 1).toInt()]) else emptyList()
            }
            val end = (values["to"] as? Number)?.toDouble()?.toLong() ?: 0L
            val from = maxOf(0L, position - 1).coerceAtMost(items.size.toLong()).toInt()
            val to = maxOf(0L, end).coerceAtMost(items.size.toLong()).toInt()
            return if (from < to) items.subList(from, to).toList() else emptyList()
        }

        fun waitFor(raw: Any?) {
            val seconds = (raw as Number).toDouble()
            if (seconds < 0) fail("Seconds must be 0 or more")
            if (seconds > WorkflowActions.MAX_WAIT_SECONDS) fail("Wait at most ${WorkflowActions.MAX_WAIT_SECONDS} seconds")
            val ms = (seconds * 1000).toLong()
            if (clock() + ms > deadline) fail("Waiting would run past this run's time limit")
            shared.sleep(ms)
        }

        fun runChild(raw: Any?, item: Any?): List<Any?> {
            val ref = str(or(raw, "")).trim()
            if (depth + 1 >= WorkflowActions.MAX_WORKFLOW_DEPTH) {
                fail("Workflows can run other workflows at most ${WorkflowActions.MAX_WORKFLOW_DEPTH - 1} deep")
            }
            val (child, childId) = loadWorkflow(shared, ref) ?: fail("No workflow named $ref")
            val execution = Execution(child, shared.credentials, childId, shared, depth + 1)
            val report = execution.run(map(item))
            if (report["status"] != "success") fail((child["name"] as? String ?: ref) + " failed: " + report["error"])
            return execution.finalItems(report)
        }

        fun finalItems(report: Map<String, Any?>): List<Any?> {
            val nodes = list(or(workflow["nodes"], emptyList<Any?>())).map { map(it) }
            val connections = list(or(workflow["connections"], emptyList<Any?>())).map { map(it) }
            val ids = if (stopped.isNotEmpty()) {
                listOf(byName.getValue(stopped))
            } else {
                val sources = connections.map { it["from"].toString() }.toSet()
                (topologicalOrder(nodes, connections) ?: emptyList()).filter { it !in sources }
            }
            val found = ArrayList<Any?>()
            for (id in ids) {
                if (get(map(report["nodes"])[id], "status") != "success") continue
                for (produced in (outputs[id] ?: emptyMap()).values) found.addAll(produced)
            }
            return found.take(maxItems)
        }

        fun runScript(node: Map<String, Any?>, items: List<Any?>): List<Any?> {
            val values = values(node, items[0], 0)
            val program = try {
                parseScript(values["code"].toString())
            } catch (error: ScriptError) {
                fail(error.message ?: "")
            }
            val interp = JsInterpreter(now, shared.random, clock, deadline)
            val scriptItems = items.map { fromJson(it) }
            val vars = fromJson(variables)
            val stamp = isoTime(now)
            val sharedValues = linkedMapOf(
                "\$trigger" to fromJson(triggerItem), "\$vars" to vars, "\$now" to stamp, "\$today" to stamp.take(10),
                "\$workflow" to fromJson(linkedMapOf("id" to workflowId, "name" to (workflow["name"] as? String ?: ""))),
                "\$utils" to WorkflowActions.utilsObject(interp),
            )
            val itemsArray = JsArray(scriptItems)
            val out = ArrayList<Any?>()
            val nextVariables: Any?
            try {
                if (values["mode"] == "each") {
                    for ((index, item) in scriptItems.withIndex()) {
                        val current = LinkedHashMap<String, Any?>(sharedValues)
                        current["items"] = itemsArray
                        current["item"] = item
                        current["\$json"] = item
                        current["\$index"] = index.toDouble()
                        current["\$node"] = fromJson(context(items[index], index)["node"])
                        current["\$input"] = scriptInput(itemsArray, item)
                        out.addAll(unwrapItems(interp.run(program, current), item))
                    }
                } else {
                    val current = LinkedHashMap<String, Any?>(sharedValues)
                    current["items"] = itemsArray
                    current["\$node"] = fromJson(context(items[0], 0)["node"])
                    current["\$input"] = scriptInput(itemsArray, null)
                    out.addAll(unwrapItems(interp.run(program, current), itemsArray))
                }
                nextVariables = toJsonValue(vars)
            } catch (error: ScriptError) {
                fail(error.message ?: "")
            } catch (error: ScriptLimit) {
                fail(error.message ?: "")
            } finally {
                if (interp.logs.isNotEmpty()) logs[node["id"].toString()] = ArrayList(interp.logs)
            }
            variables = if (nextVariables is MutableMap<*, *>) nextVariables as MutableMap<String, Any?> else LinkedHashMap()
            return out
        }

        fun scriptInput(scriptItems: JsArray, current: Any?): JsObject {
            fun wrap(item: Any?): JsObject = JsObject().also { it["json"] = item }
            val out = JsObject()
            out["all"] = native("all") { _, _ -> JsArray(scriptItems.map { wrap(it) }) }
            out["first"] = native("first") { _, _ -> if (scriptItems.isNotEmpty()) wrap(scriptItems[0]) else Undefined }
            out["last"] = native("last") { _, _ -> if (scriptItems.isNotEmpty()) wrap(scriptItems[scriptItems.size - 1]) else Undefined }
            if (current != null) out["item"] = wrap(current)
            return out
        }

        fun report(id: String, out: Map<String, List<Any?>>, started: Long, error: String): Map<String, Any?> {
            val entry = linkedMapOf<String, Any?>(
                "status" to "success", "ms" to (clock() - started).coerceAtLeast(0),
                "outputs" to out.entries.associateTo(LinkedHashMap()) { (name, items) ->
                    name to linkedMapOf("count" to items.size.toLong(), "items" to summarizeItems(items))
                },
            )
            if (error.isNotEmpty()) entry["error"] = error
            logs[id]?.let { entry["logs"] = it }
            return entry
        }

        fun run(trigger: Map<String, Any?>): LinkedHashMap<String, Any?> {
            triggerItem = trigger
            val nodes = list(or(workflow["nodes"], emptyList<Any?>())).map { map(it) }
            val connections = list(or(workflow["connections"], emptyList<Any?>())).map { map(it) }
            val byId = nodes.associateBy { it["id"].toString() }
            byName = LinkedHashMap(nodes.associate { it["name"].toString() to it["id"].toString() })
            val order = topologicalOrder(nodes, connections) ?: emptyList()
            val start = triggerNode(workflow)
            val nodeReports = LinkedHashMap<String, Any?>()
            var failed = ""
            for (id in order) {
                val node = byId.getValue(id)
                val type = node["type"].toString()
                val started = clock()
                if (failed.isNotEmpty() || stopped.isNotEmpty()) {
                    nodeReports[id] = linkedMapOf("status" to "skipped")
                    continue
                }
                val items: List<Any?>
                if (isTrigger(type)) {
                    if (start == null || id != start["id"]) {
                        nodeReports[id] = linkedMapOf("status" to "skipped")
                        continue
                    }
                    items = listOf(triggerItem)
                } else {
                    val gathered = mutableListOf<Any?>()
                    var fed = false
                    for (conn in connections) {
                        if (conn["to"] != id) continue
                        val produced = outputs[conn["from"].toString()]?.get(conn["output"].toString())
                        if (produced != null) {
                            fed = true
                            gathered.addAll(produced)
                        }
                    }
                    if (!fed || gathered.isEmpty()) {
                        nodeReports[id] = linkedMapOf("status" to "skipped")
                        continue
                    }
                    items = gathered
                }
                val out = try {
                    runNode(node, items)
                } catch (stop: WorkflowStop) {
                    outputs[id] = stop.out
                    nodeReports[id] = report(id, stop.out, started, "")
                    stopped = node["name"].toString()
                    continue
                } catch (error: WorkflowNodeError) {
                    val message = error.message ?: ""
                    if (node["continueOnFail"] == true) {
                        val failedOut = LinkedHashMap<String, List<Any?>>()
                        for (name in outputsFor(type)) failedOut[name] = emptyList()
                        failedOut[outputsFor(type)[0]] = listOf(linkedMapOf("error" to message))
                        outputs[id] = failedOut
                        nodeReports[id] = report(id, failedOut, started, message)
                        continue
                    }
                    val entry = linkedMapOf<String, Any?>("status" to "error", "error" to message, "ms" to (clock() - started).coerceAtLeast(0))
                    logs[id]?.let { entry["logs"] = it }
                    nodeReports[id] = entry
                    failed = node["name"].toString() + ": " + message
                    continue
                }
                outputs[id] = out
                nodeReports[id] = report(id, out, started, "")
            }
            val result = linkedMapOf<String, Any?>("status" to if (failed.isNotEmpty()) "error" else "success", "error" to failed,
                                                   "requests" to shared.requests.toLong(), "nodes" to nodeReports)
            if (stopped.isNotEmpty()) result["stopped"] = stopped
            return result
        }
    }
}
