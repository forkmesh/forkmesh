package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import java.util.concurrent.CopyOnWriteArrayList

class NodeSyncApplyException(override val message: String) : Exception(message) {
    override fun toString(): String = message
}

enum class NodeSyncApplyOutcome {
    APPLIED,

    ALREADY_APPLIED,

    LEFT,
}

data class NodeSyncApplyRecord(
    val repoFullName: String,
    val kind: NodeSyncKind,
    val outcome: NodeSyncApplyOutcome,
    val reason: String = "",
    val commitId: String = "",
)

data class NodeSyncAuthor(val name: String, val email: String)

data class NodeSyncRepoRecord(
    val name: String,
    val path: String,
    val publishedAs: String?,
)

data class NodeSyncFileEntry(val name: String, val path: String, val isDirectory: Boolean)

data class NodeSyncFileContent(val text: String, val isBinary: Boolean = false, val isTooLarge: Boolean = false)

class NodeSyncNothingStagedException(message: String = "Nothing is staged") : Exception(message)

interface NodeSyncRepositories {
    fun ensureLoaded()

    val records: List<NodeSyncRepoRecord>

    fun workspace(record: NodeSyncRepoRecord): NodeSyncWorkspace

    fun refresh(name: String)
}

interface NodeSyncWorkspace {
    fun currentBranch(): String

    fun readFile(path: String): NodeSyncFileContent

    fun listFiles(directory: String): List<NodeSyncFileEntry>

    fun writeFile(path: String, text: String)

    fun deleteFile(path: String)

    fun stage(paths: List<String>)

    fun commit(message: String, author: NodeSyncAuthor): String

    fun dispose()
}

data class NodeSyncApplierSummary(
    val appliedTotal: Int,
    val leftTotal: Int,
    val leftReasons: List<Pair<String, Int>>,
)

class LocalNodeSyncApplier(
    private val repos: NodeSyncRepositories,
    private val author: () -> NodeSyncAuthor,
    private val prefs: PrefStore,
    private val now: () -> Long = System::currentTimeMillis,
    private val ledgerKey: String = APPLIED_PREFS_KEY,
) : NodeSyncApplier {
    companion object {
        const val PULL_LEDGER_BRANCH = "forkmesh/pulls"

        const val APPLIED_PREFS_KEY = "nodeSync/applied"
        const val APPLIED_LEDGER_LIMIT = 500

        const val RECENT_LIMIT = 20

        private const val ISSUES_ROOT = ".forkmesh/issues"
        private const val DISCUSSIONS_ROOT = ".forkmesh/discussions"
        private const val PULLS_ROOT = ".forkmesh/pulls"
        private val COMMENT_FILE = Regex("^\\d{4}-comment\\.md$")
        private val PULL_EVENT_FILE = Regex("^\\d{4}-[a-z-]+\\.md$")
    }

    private val lock = Any()
    private val workspaces = HashMap<String, NodeSyncWorkspace>()
    private val applied = LinkedHashSet<String>()
    private val recentRecords = ArrayList<NodeSyncApplyRecord>()
    private val leftReasonCounts = LinkedHashMap<String, Int>()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    private var ledgerLoaded = false

    @Volatile
    private var disposed = false

    @Volatile
    var appliedTotal = 0
        private set

    @Volatile
    var leftTotal = 0
        private set

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    val leftReasons: List<Pair<String, Int>>
        get() = synchronized(leftReasonCounts) { leftReasonCounts.entries.map { it.key to it.value } }
            .sortedByDescending { it.second }

    val recent: List<NodeSyncApplyRecord> get() = synchronized(recentRecords) { ArrayList(recentRecords) }

    fun summary(): NodeSyncApplierSummary = NodeSyncApplierSummary(appliedTotal, leftTotal, leftReasons)

    fun installOn(engine: NodeSyncEngine) {
        engine.applier = this
    }

    fun dispose() {
        synchronized(lock) {
            disposed = true
            for (workspace in workspaces.values) runCatching { workspace.dispose() }
            workspaces.clear()
        }
    }

    override fun apply(item: NodeSyncItem): Boolean = synchronized(lock) { applyNow(item) }

    private fun applyNow(item: NodeSyncItem): Boolean {
        if (disposed) return false
        loadLedger()
        val fingerprint = fingerprintOf(item)
        if (fingerprint.isNotEmpty() && fingerprint in applied) {
            note(item, NodeSyncApplyOutcome.ALREADY_APPLIED)
            return true
        }
        val record = recordFor(item)
        if (record == null) {
            left(item, "no local copy of ${item.repoFullName} on this phone")
            return false
        }
        val workspace = workspaces.getOrPut(record.path) { repos.workspace(record) }
        return try {
            val commit = when (item.kind) {
                NodeSyncKind.ISSUES -> applyIssue(workspace, item)
                NodeSyncKind.DISCUSSIONS -> applyDiscussion(workspace, item)
                NodeSyncKind.PULLS -> applyPull(workspace, item)
                else -> throw NodeSyncApplyException("${item.kind.wireName} items are not applied on this device")
            }
            if (fingerprint.isNotEmpty()) remember(fingerprint)
            note(item, NodeSyncApplyOutcome.APPLIED, commitId = commit)
            if (!disposed) {
                try {
                    repos.refresh(record.name)
                } catch (_: Exception) {
                }
            }
            true
        } catch (error: NodeSyncApplyException) {
            left(item, error.message)
            false
        } catch (error: Exception) {
            left(item, short(dartErrorText(error)))
            false
        }
    }

    private fun recordFor(item: NodeSyncItem): NodeSyncRepoRecord? {
        val wanted = item.repoFullName.lowercase()
        if (wanted.isEmpty() || !wanted.contains('/')) return null
        repos.ensureLoaded()
        for (record in repos.records) {
            val published = dartTrimText(record.publishedAs ?: "").lowercase()
            if (published.isNotEmpty() && published == wanted) return record
        }
        return null
    }

    private fun applyIssue(workspace: NodeSyncWorkspace, item: NodeSyncItem): String {
        val event = eventOf(item)
        val type = string(event["type"])
        if (type.isEmpty()) throw NodeSyncApplyException("issue event has no type")
        val attachments = item.data["attachmentData"]
        if (attachments is List<*> && attachments.isNotEmpty()) {
            throw NodeSyncApplyException("issue carries file attachments; a desktop must apply it")
        }
        var number = item.numberOrZero

        fun locate(candidate: Long): String? {
            for (directory in listOf("$ISSUES_ROOT/open/$candidate", "$ISSUES_ROOT/closed/$candidate", "$ISSUES_ROOT/$candidate")) {
                val path = "$directory/issue-$candidate.json"
                if (readTextOrNull(workspace, path) != null) return path
            }
            return null
        }

        val issue: LinkedHashMap<String, Any?>
        var currentPath: String? = null
        if (type == "open") {
            if (number <= 0 || locate(number) != null) {
                number = nextNumber(workspace, listOf(ISSUES_ROOT, "$ISSUES_ROOT/open", "$ISSUES_ROOT/closed"))
            }
            issue = newIssue(item, event, number)
        } else {
            currentPath = locate(number)
            if (number <= 0) throw NodeSyncApplyException("issue event has no issue number to apply to")
            issue = if (currentPath == null) placeholderIssue(item, number) else decodeIssue(readTextOrNull(workspace, currentPath))
            val events = eventList(issue)
            val id = string(event["id"])
            if (id.isNotEmpty() && events.any { string(it["id"]) == id }) return ""
            events.add(LinkedHashMap(event))
            issue["events"] = events
        }

        recomputeIssue(issue)
        val status = if (string(issue["status"]) == "closed") "closed" else "open"
        val targetPath = "$ISSUES_ROOT/$status/$number/issue-$number.json"
        val touched = arrayListOf(targetPath)
        if (currentPath != null && currentPath != targetPath) {
            val siblings = listOrEmpty(workspace, currentPath.substring(0, currentPath.lastIndexOf('/')))
            if (siblings.size > 1) {
                throw NodeSyncApplyException("issue #$number has attachments and must change status on a desktop")
            }
            workspace.deleteFile(currentPath)
            touched.add(currentPath)
        }
        workspace.writeFile(targetPath, encodeIssueJson(issue))
        val who = authorLabel(event)
        val message = if (type == "open") "issue #$number: opened (from $who)" else "issue #$number: $type (from $who)"
        return commit(workspace, touched, message)
    }

    private fun newIssue(item: NodeSyncItem, event: Map<String, Any?>, number: Long): LinkedHashMap<String, Any?> {
        val meta = item.data["meta"]
        val metaMap: Map<String, Any?> = if (meta is Map<*, *>) stringKeyed(meta) else emptyMap()
        val createdAt = int(event["ts"])
        val reporter = string(metaMap["reporter"])
        return linkedMapOf(
            "schema" to "forkmesh-issue-v1",
            "number" to number,
            "title" to string(event["title"]),
            "status" to "open",
            "labels" to stringList(metaMap["labels"]),
            "milestone" to string(metaMap["milestone"]),
            "startDate" to 0L,
            "endDate" to 0L,
            "priority" to int(metaMap["priority"]).coerceIn(0L, 8999L),
            "progress" to 0L,
            "assignees" to stringList(metaMap["assignees"]),
            "createdAt" to createdAt,
            "updatedAt" to createdAt,
            "author" to string(event["author"]),
            "authorName" to reporter.ifEmpty { string(event["authorName"]) },
            "bountyUsd" to 0L,
            "bountyAddress" to "",
            "bountyStatus" to "",
            "votes" to 0L,
            "events" to arrayListOf<Map<String, Any?>>(LinkedHashMap(event)),
        )
    }

    private fun placeholderIssue(item: NodeSyncItem, number: Long): LinkedHashMap<String, Any?> {
        val title = string(item.data["titleIfNew"])
        return linkedMapOf(
            "schema" to "forkmesh-issue-v1",
            "number" to number,
            "title" to title,
            "status" to "open",
            "labels" to emptyList<String>(),
            "milestone" to "",
            "startDate" to 0L,
            "endDate" to 0L,
            "priority" to 0L,
            "progress" to 0L,
            "assignees" to emptyList<String>(),
            "createdAt" to 0L,
            "updatedAt" to 0L,
            "author" to "",
            "authorName" to "",
            "bountyUsd" to 0L,
            "bountyAddress" to "",
            "bountyStatus" to "",
            "votes" to 0L,
            "events" to arrayListOf<Map<String, Any?>>(
                linkedMapOf(
                    "type" to "open",
                    "id" to "open-$number",
                    "title" to title,
                    "body" to "",
                    "author" to "",
                    "authorName" to "",
                    "ts" to 0L,
                    "sig" to "",
                ),
            ),
        )
    }

    private fun recomputeIssue(issue: MutableMap<String, Any?>) {
        val events = eventList(issue)
        var votes = 0L
        var updatedAt = int(issue["createdAt"])
        for (event in events) {
            val ts = int(event["ts"])
            if (ts > updatedAt) updatedAt = ts
            when (string(event["type"])) {
                "open" -> issue["title"] = string(event["title"])
                "title" -> string(event["title"]).takeIf { it.isNotEmpty() }?.let { issue["title"] = it }
                "status" -> issue["status"] = string(event["status"])
                "labels" -> issue["labels"] = stringList(event["labels"])
                "milestone" -> issue["milestone"] = string(event["milestone"])
                "dates" -> {
                    issue["startDate"] = int(event["startDate"])
                    issue["endDate"] = int(event["endDate"])
                }
                "priority" -> issue["priority"] = int(event["priority"])
                "progress" -> issue["progress"] = int(event["progress"])
                "bounty" -> {
                    issue["bountyUsd"] = event["bountyUsd"] ?: 0L
                    issue["bountyAddress"] = string(event["bountyAddress"])
                    issue["bountyStatus"] = string(event["bountyStatus"])
                }
                "assignees" -> issue["assignees"] = stringList(event["assignees"])
                "vote" -> votes++
            }
        }
        issue["votes"] = votes
        issue["updatedAt"] = updatedAt
    }

    private fun decodeIssue(text: String?): LinkedHashMap<String, Any?> {
        if (text == null) throw NodeSyncApplyException("issue record is missing")
        val decoded = try {
            Json.parse(text)
        } catch (_: Json.JsonException) {
            throw NodeSyncApplyException("issue record is not valid JSON")
        }
        if (decoded !is Map<*, *>) throw NodeSyncApplyException("issue record is not an object")
        return stringKeyed(decoded)
    }

    private fun eventList(issue: Map<String, Any?>): ArrayList<Map<String, Any?>> {
        val raw = issue["events"]
        val out = ArrayList<Map<String, Any?>>()
        if (raw is List<*>) for (entry in raw) if (entry is Map<*, *>) out.add(stringKeyed(entry))
        return out
    }

    private fun applyDiscussion(workspace: NodeSyncWorkspace, item: NodeSyncItem): String {
        val event = eventOf(item)
        val type = string(event["type"])
        if (type != "open" && type != "comment") {
            throw NodeSyncApplyException("unsupported discussion event \"${type.ifEmpty { "unknown" }}\"")
        }
        var number = item.numberOrZero
        val who = authorLabel(event)

        if (type == "open") {
            if (number <= 0 || readTextOrNull(workspace, "$DISCUSSIONS_ROOT/$number/discussion.md") != null) {
                number = nextNumber(workspace, listOf(DISCUSSIONS_ROOT))
            }
            val path = "$DISCUSSIONS_ROOT/$number/discussion.md"
            workspace.writeFile(
                path,
                frontMatterDocument(
                    listOf(
                        "schema" to "forkmesh-discussion-v1",
                        "number" to "$number",
                        "title" to string(event["title"]),
                        "category" to string(event["category"]),
                        "status" to "open",
                        "createdAt" to "${int(event["ts"])}",
                        "author" to string(event["author"]),
                        "authorName" to string(event["authorName"]),
                        "type" to "open",
                        "id" to string(event["id"]),
                        "ts" to "${int(event["ts"])}",
                        "sig" to string(event["sig"]),
                    ),
                    string(event["body"]),
                ),
            )
            return commit(workspace, listOf(path), "discussion #$number: opened (from $who)")
        }

        if (number <= 0) throw NodeSyncApplyException("discussion comment has no discussion number")
        val touched = ArrayList<String>()
        val rootPath = "$DISCUSSIONS_ROOT/$number/discussion.md"
        if (readTextOrNull(workspace, rootPath) == null) {
            workspace.writeFile(
                rootPath,
                frontMatterDocument(
                    listOf(
                        "schema" to "forkmesh-discussion-v1",
                        "number" to "$number",
                        "title" to string(item.data["titleIfNew"]),
                        "category" to "Ideas",
                        "status" to "open",
                        "createdAt" to "0",
                        "author" to "",
                        "authorName" to "",
                        "type" to "open",
                        "id" to "open-$number",
                        "ts" to "0",
                        "sig" to "",
                    ),
                    "",
                ),
            )
            touched.add(rootPath)
        }
        val id = string(event["id"])
        val existing = listOrEmpty(workspace, "$DISCUSSIONS_ROOT/$number")
        if (id.isNotEmpty()) {
            for (entry in existing) {
                if (!COMMENT_FILE.matches(entry.name)) continue
                val text = readTextOrNull(workspace, entry.path)
                if (text != null && text.contains("\nid: $id\n")) return ""
            }
        }
        val index = nextEventIndex(existing, COMMENT_FILE)
        val path = "$DISCUSSIONS_ROOT/$number/${index.toString().padStart(4, '0')}-comment.md"
        workspace.writeFile(
            path,
            frontMatterDocument(
                listOf(
                    "type" to "comment",
                    "id" to id,
                    "author" to string(event["author"]),
                    "authorName" to string(event["authorName"]),
                    "ts" to "${int(event["ts"])}",
                    "sig" to string(event["sig"]),
                ),
                string(event["body"]),
            ),
        )
        touched.add(path)
        return commit(workspace, touched, "discussion #$number: comment (from $who)")
    }

    private fun applyPull(workspace: NodeSyncWorkspace, item: NodeSyncItem): String {
        if (workspace.currentBranch() != PULL_LEDGER_BRANCH) {
            throw NodeSyncApplyException(
                "pull records live on the $PULL_LEDGER_BRANCH ledger branch, which this phone does not have checked out",
            )
        }
        val pull = item.data["pull"]
        if (pull is Map<*, *>) return applyNewPull(workspace, item, stringKeyed(pull))
        return applyPullEvent(workspace, item)
    }

    private fun applyNewPull(workspace: NodeSyncWorkspace, item: NodeSyncItem, pull: Map<String, Any?>): String {
        var number = int(pull["number"])
        if (number <= 0) number = item.numberOrZero
        if (number <= 0 || readTextOrNull(workspace, "$PULLS_ROOT/$number/pull.md") != null) {
            number = nextNumber(workspace, listOf(PULLS_ROOT))
        }
        val branchBacked = string(pull["derive"]) == "branch"
        val touched = ArrayList<String>()
        val path = "$PULLS_ROOT/$number/pull.md"
        val fields = ArrayList<Pair<String, String>>()
        fields.add("schema" to "forkmesh-pull-v1")
        fields.add("number" to "$number")
        fields.add("title" to string(pull["title"]))
        fields.add("base" to string(pull["base"]))
        fields.add("head" to string(pull["head"]))
        fields.add("status" to "open")
        if (branchBacked) fields.add("derive" to "branch")
        for (key in listOf("creationBaseOid", "creationHeadOid", "mergeBase", "mergeHead")) {
            if (string(pull[key]).isNotEmpty()) fields.add(key to string(pull[key]))
        }
        fields.add("ts" to "${int(pull["ts"])}")
        fields.add("author" to string(pull["author"]))
        fields.add("authorName" to string(pull["authorName"]))
        fields.add("sig" to string(pull["sig"]))
        if (string(pull["client"]).isNotEmpty()) fields.add("client" to string(pull["client"]))
        if (string(pull["clientSig"]).isNotEmpty()) fields.add("clientSig" to string(pull["clientSig"]))
        workspace.writeFile(path, frontMatterDocument(fields, string(pull["description"])))
        touched.add(path)
        if (!branchBacked) {
            val patch = string(pull["patch"])
            if (patch.isNotEmpty()) {
                val patchPath = "$PULLS_ROOT/$number/changes.patch"
                workspace.writeFile(patchPath, patch)
                touched.add(patchPath)
            }
            val commits = string(pull["commits"])
            if (commits.isNotEmpty()) {
                val mboxPath = "$PULLS_ROOT/$number/commits.mbox"
                workspace.writeFile(mboxPath, commits)
                touched.add(mboxPath)
            }
        }
        return commit(workspace, touched, "pull #$number: opened (from ${authorLabel(pull)})")
    }

    private fun applyPullEvent(workspace: NodeSyncWorkspace, item: NodeSyncItem): String {
        val event = eventOf(item)
        val type = string(event["type"])
        if (type != "comment" && type != "review") {
            throw NodeSyncApplyException("unsupported pull event \"${type.ifEmpty { "unknown" }}\"")
        }
        val number = item.numberOrZero
        if (number <= 0) throw NodeSyncApplyException("pull event has no pull number")
        if (readTextOrNull(workspace, "$PULLS_ROOT/$number/pull.md") == null) {
            throw NodeSyncApplyException("pull #$number is not in this repository")
        }
        val id = string(event["id"])
        val existing = listOrEmpty(workspace, "$PULLS_ROOT/$number")
        if (id.isNotEmpty()) {
            for (entry in existing) {
                if (!PULL_EVENT_FILE.matches(entry.name)) continue
                val text = readTextOrNull(workspace, entry.path)
                if (text != null && text.contains("\nid: $id\n")) return ""
            }
        }
        val index = nextEventIndex(existing, PULL_EVENT_FILE)
        val path = "$PULLS_ROOT/$number/${index.toString().padStart(4, '0')}-$type.md"
        val fields = arrayListOf(
            "type" to type,
            "id" to id,
            "author" to string(event["author"]),
            "authorName" to string(event["authorName"]),
            "ts" to "${int(event["ts"])}",
        )
        if (type == "review") fields.add("state" to string(event["state"]))
        fields.add("sig" to string(event["sig"]))
        workspace.writeFile(path, frontMatterDocument(fields, string(event["body"])))
        return commit(workspace, listOf(path), "pull #$number: $type (from ${authorLabel(event)})")
    }

    private fun commit(workspace: NodeSyncWorkspace, touched: List<String>, message: String): String {
        workspace.stage(touched)
        return try {
            workspace.commit(message, author())
        } catch (error: Exception) {
            if (error is NodeSyncNothingStagedException || dartErrorText(error).contains("Nothing is staged")) return ""
            throw NodeSyncApplyException("commit failed: ${short(dartErrorText(error))}")
        }
    }

    private fun nextNumber(workspace: NodeSyncWorkspace, roots: List<String>): Long {
        var highest = 0L
        for (root in roots) {
            for (entry in listOrEmpty(workspace, root)) {
                if (!entry.isDirectory) continue
                val value = dartIntTryParse(entry.name) ?: 0L
                if (value > highest) highest = value
            }
        }
        return highest + 1
    }

    private fun nextEventIndex(entries: List<NodeSyncFileEntry>, pattern: Regex): Long {
        var highest = 1L
        for (entry in entries) {
            if (!pattern.matches(entry.name)) continue
            val value = dartIntTryParse(entry.name.substring(0, 4)) ?: 0L
            if (value > highest) highest = value
        }
        return highest + 1
    }

    private fun listOrEmpty(workspace: NodeSyncWorkspace, directory: String): List<NodeSyncFileEntry> = try {
        workspace.listFiles(directory)
    } catch (_: Exception) {
        emptyList()
    }

    private fun readTextOrNull(workspace: NodeSyncWorkspace, path: String): String? {
        val content = try {
            workspace.readFile(path)
        } catch (_: Exception) {
            return null
        }
        if (content.isBinary || content.isTooLarge) throw NodeSyncApplyException("$path is not readable as text")
        return content.text
    }

    private fun frontMatterDocument(fields: List<Pair<String, String>>, body: String): String {
        val lines = arrayListOf("---")
        for ((key, value) in fields) lines.add("$key: ${frontMatterValue(value)}")
        lines.add("---")
        lines.add("")
        return "${lines.joinToString("\n")}\n$body\n"
    }

    private fun frontMatterValue(value: String): String =
        value.replace("\r\n", " ").replace("\n", " ").replace("\r", " ")

    private fun encodeIssueJson(issue: Map<String, Any?>): String {
        val out = StringBuilder()
        writeIndented(out, sorted(issue), "")
        return out.append('\n').toString()
    }

    private fun sorted(value: Any?): Any? = when (value) {
        is Map<*, *> -> {
            val out = LinkedHashMap<String, Any?>()
            for (key in value.keys.map { it.toString() }.sorted()) out[key] = sorted(value[key])
            out
        }
        is List<*> -> value.map(::sorted)
        else -> value
    }

    private fun writeIndented(out: StringBuilder, value: Any?, indent: String) {
        val inner = "$indent    "
        when (value) {
            is Map<*, *> -> {
                if (value.isEmpty()) {
                    out.append("{}")
                    return
                }
                out.append("{\n")
                var first = true
                for ((key, item) in value) {
                    if (!first) out.append(",\n")
                    first = false
                    out.append(inner).append(Json.stringify(key.toString())).append(": ")
                    writeIndented(out, item, inner)
                }
                out.append('\n').append(indent).append('}')
            }
            is List<*> -> {
                if (value.isEmpty()) {
                    out.append("[]")
                    return
                }
                out.append("[\n")
                var first = true
                for (item in value) {
                    if (!first) out.append(",\n")
                    first = false
                    out.append(inner)
                    writeIndented(out, item, inner)
                }
                out.append('\n').append(indent).append(']')
            }
            else -> out.append(Json.stringify(value))
        }
    }

    private fun eventOf(item: NodeSyncItem): LinkedHashMap<String, Any?> {
        val event = item.data["event"]
        if (event !is Map<*, *>) throw NodeSyncApplyException("item carries no event")
        return stringKeyed(event)
    }

    private fun fingerprintOf(item: NodeSyncItem): String {
        val prefix = "${item.repoFullName}|${item.kind.wireName}|"
        for (source in listOf(item.data["event"], item.data["pull"])) {
            if (source !is Map<*, *>) continue
            val id = string(source["id"])
            if (id.isNotEmpty()) return "$prefix$id"
            val sig = string(source["sig"])
            if (sig.isNotEmpty()) return "$prefix$sig"
        }
        return if (item.id > 0) "${prefix}row:${item.id}" else ""
    }

    private fun loadLedger() {
        if (ledgerLoaded) return
        ledgerLoaded = true
        try {
            val stored = Json.parse(prefs.getString(ledgerKey) ?: "[]") as? List<*> ?: return
            for (entry in stored) if (entry is String) applied.add(entry)
        } catch (_: Exception) {
        }
    }

    private fun remember(fingerprint: String) {
        applied.add(fingerprint)
        var stored = applied.toList()
        if (stored.size > APPLIED_LEDGER_LIMIT) {
            stored = stored.subList(stored.size - APPLIED_LEDGER_LIMIT, stored.size).toList()
            applied.clear()
            applied.addAll(stored)
        }
        try {
            prefs.putString(ledgerKey, Json.stringify(stored))
        } catch (_: Exception) {
        }
    }

    private fun left(item: NodeSyncItem, reason: String) {
        leftTotal++
        synchronized(leftReasonCounts) { leftReasonCounts[reason] = (leftReasonCounts[reason] ?: 0) + 1 }
        note(item, NodeSyncApplyOutcome.LEFT, reason = reason)
    }

    private fun note(item: NodeSyncItem, outcome: NodeSyncApplyOutcome, reason: String = "", commitId: String = "") {
        if (outcome == NodeSyncApplyOutcome.APPLIED) appliedTotal++
        synchronized(recentRecords) {
            recentRecords.add(0, NodeSyncApplyRecord(item.repoFullName, item.kind, outcome, reason, commitId))
            if (recentRecords.size > RECENT_LIMIT) recentRecords.removeAt(recentRecords.lastIndex)
        }
        if (!disposed) {
            for (listener in listeners) runCatching { listener() }
        }
    }

    val clock: Long get() = now()

    private fun short(text: String): String {
        val single = dartTrimText(text.replace("\n", " "))
        return if (single.length <= 120) single else "${single.substring(0, 119)}…"
    }

    private fun string(value: Any?): String = if (value == null) "" else dartTrimText(jsonText(value))

    private fun int(value: Any?): Long {
        if (value is Number) return value.toLong()
        return dartIntTryParse(if (value == null) "" else jsonText(value)) ?: 0L
    }

    private fun stringList(value: Any?): List<String> {
        val out = ArrayList<String>()
        if (value is List<*>) {
            for (entry in value) {
                val text = dartTrimText(if (entry == null) "" else jsonText(entry))
                if (text.isNotEmpty()) out.add(text)
            }
        }
        return out
    }

    private fun authorLabel(event: Map<String, Any?>): String {
        val name = string(event["authorName"])
        if (name.isNotEmpty()) return name
        val signer = string(event["author"])
        return if (signer.length > 8) signer.substring(0, 8) else signer
    }
}
