package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.jsonText

enum class NodeSyncMode {
    DISABLED,

    STANDBY,

    ACTIVE,
}

enum class NodeSyncConnection { OFFLINE, CONNECTING, LIVE }

enum class NodeSyncKind(val wireName: String) {
    ISSUES("issues"),
    PULLS("pulls"),
    DISCUSSIONS("discussions"),
    AGENT_PROMPTS("agentPrompts"),
    ABOUT("aboutUpdate"),
    BRANCH_OPS("branchOps"),
    ;

    fun label(count: Int): String = when (this) {
        ISSUES -> if (count == 1) "issue" else "issues"
        PULLS -> if (count == 1) "pull" else "pulls"
        DISCUSSIONS -> if (count == 1) "discussion" else "discussions"
        AGENT_PROMPTS -> if (count == 1) "agent prompt" else "agent prompts"
        ABOUT -> if (count == 1) "About edit" else "About edits"
        BRANCH_OPS -> if (count == 1) "branch operation" else "branch operations"
    }

    val isAckable: Boolean get() = this == ISSUES || this == PULLS || this == DISCUSSIONS
}

private fun textOf(value: Any?): String = jsonText(value)

data class NodeSyncItem(
    val owner: String,
    val repo: String,
    val kind: NodeSyncKind,
    val id: Long,
    val data: Map<String, Any?>,
) {
    val repoFullName: String get() = "$owner/$repo"

    val title: String
        get() {
            for (key in listOf("title", "titleIfNew", "name", "about")) {
                val value = dartTrimText(textOf(data[key]))
                if (value.isNotEmpty()) return value
            }
            val event = data["event"]
            if (event is Map<*, *>) {
                for (key in listOf("title", "body")) {
                    val value = dartTrimText(textOf(event[key]))
                    if (value.isNotEmpty()) return value
                }
            }
            val number = numberOrZero
            if (number > 0) return "#$number"
            return kind.label(1)
        }

    val numberOrZero: Long
        get() {
            val raw = data["number"] ?: data["pullNumber"] ?: data["issueNumber"]
            if (raw is Number) return raw.toLong()
            return dartIntTryParse(textOf(raw)) ?: 0L
        }

    fun toJson(): LinkedHashMap<String, Any?> = linkedMapOf(
        "owner" to owner,
        "repo" to repo,
        "kind" to kind.wireName,
        "id" to id,
        "data" to data,
    )

    companion object {
        fun fromJson(json: Map<String, Any?>): NodeSyncItem? {
            val wire = textOf(json["kind"])
            val kind = NodeSyncKind.entries.lastOrNull { it.wireName == wire } ?: return null
            val data = json["data"]
            val rawId = json["id"]
            return NodeSyncItem(
                owner = textOf(json["owner"]),
                repo = textOf(json["repo"]),
                kind = kind,
                id = if (rawId is Number) rawId.toLong() else 0L,
                data = if (data is Map<*, *>) stringKeyed(data) else emptyMap(),
            )
        }
    }
}

internal fun stringKeyed(map: Map<*, *>): LinkedHashMap<String, Any?> {
    val out = LinkedHashMap<String, Any?>()
    for ((key, value) in map) out[key.toString()] = value
    return out
}

data class NodeSyncRepoState(
    val owner: String,
    val name: String,
    val stateHash: String = "",
    val updatedAt: String = "",
    val source: String = "",
    val items: List<NodeSyncItem> = emptyList(),
    val agentPromptAckPath: String = "",
    val mirrorMerges: List<Map<String, Any?>> = emptyList(),
) {
    val fullName: String get() = "$owner/$name"

    fun ofKind(kind: NodeSyncKind): List<NodeSyncItem> = items.filter { it.kind == kind }
}

data class NodeSyncPayload(val serverTime: Long = 0, val repos: List<NodeSyncRepoState> = emptyList()) {
    val items: List<NodeSyncItem> get() = repos.flatMap { it.items }

    fun countOf(kind: NodeSyncKind): Int = items.count { it.kind == kind }

    companion object {
        private val KIND_KEYS = linkedMapOf(
            "issues" to NodeSyncKind.ISSUES,
            "pulls" to NodeSyncKind.PULLS,
            "discussions" to NodeSyncKind.DISCUSSIONS,
            "agentPrompts" to NodeSyncKind.AGENT_PROMPTS,
            "branchOps" to NodeSyncKind.BRANCH_OPS,
        )

        fun parse(decoded: Any?): NodeSyncPayload {
            if (decoded !is Map<*, *>) return NodeSyncPayload()
            val repos = ArrayList<NodeSyncRepoState>()
            for (row in decoded["repos"] as? List<*> ?: emptyList<Any?>()) {
                if (row !is Map<*, *>) continue
                val entry = stringKeyed(row)
                val owner = textOf(entry["owner"])
                val name = textOf(entry["name"])
                if (name.isEmpty()) continue
                val items = ArrayList<NodeSyncItem>()
                for ((key, kind) in KIND_KEYS) {
                    for (raw in entry[key] as? List<*> ?: emptyList<Any?>()) {
                        if (raw !is Map<*, *>) continue
                        val data = stringKeyed(raw)
                        val id = data["id"] ?: data["queueId"]
                        items.add(
                            NodeSyncItem(
                                owner = owner,
                                repo = name,
                                kind = kind,
                                id = when {
                                    kind == NodeSyncKind.AGENT_PROMPTS -> 0L
                                    id is Number -> id.toLong()
                                    else -> dartIntTryParse(textOf(id)) ?: 0L
                                },
                                data = data,
                            ),
                        )
                    }
                }
                val about = entry["aboutUpdate"]
                if (about is Map<*, *>) {
                    items.add(NodeSyncItem(owner = owner, repo = name, kind = NodeSyncKind.ABOUT, id = 0, data = stringKeyed(about)))
                }
                repos.add(
                    NodeSyncRepoState(
                        owner = owner,
                        name = name,
                        stateHash = textOf(entry["stateHash"]),
                        updatedAt = textOf(entry["updatedAt"]),
                        source = textOf(entry["source"]),
                        items = items,
                        agentPromptAckPath = textOf(entry["agentPromptAckPath"]),
                        mirrorMerges = (entry["mirrorMerges"] as? List<*> ?: emptyList<Any?>())
                            .filterIsInstance<Map<*, *>>()
                            .map { stringKeyed(it) },
                    ),
                )
            }
            val serverTime = decoded["serverTime"]
            return NodeSyncPayload(serverTime = if (serverTime is Number) serverTime.toLong() else 0L, repos = repos)
        }
    }
}

fun interface NodeSyncApplier {
    fun apply(item: NodeSyncItem): Boolean
}
