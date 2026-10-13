package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str

data class AgentDeletePart(val kind: String, val name: String = "", val status: String, val reason: String = "") {
    val label: String
        get() = when (kind) {
            "agent" -> "the agent"
            "branch" -> if (name.isEmpty()) "its branch" else "branch $name"
            "work" -> "its worktree"
            "issue" -> "issue #$name"
            "pull" -> "pull request #$name"
            "task" -> "task $name"
            else -> if (name.isEmpty()) kind else "$kind $name"
        }

    val reasonText: String
        get() = when (reason) {
            "" -> ""
            "merged" -> "it was merged"
            "no_checkout" -> "its repository is not on the desktop"
            "owner_only" -> "only an organization owner can delete it"
            "forbidden" -> "only an owner can delete branches"
            "running" -> "the run did not stop"
            "relay" -> "the phone cannot close it"
            else -> reason
        }

    companion object {
        fun from(json: JsonMap): AgentDeletePart? {
            val kind = json.str("kind")
            val status = json.str("status")
            if (kind.isEmpty() || status.isEmpty()) return null
            return AgentDeletePart(kind, json.str("name"), status, json.str("reason"))
        }
    }
}

data class AgentDeleteReport(
    val parts: List<AgentDeletePart>,
    val settled: Boolean = true,
    val known: List<String> = ATTACHED_KINDS,
) {
    val agentKept: Boolean get() = parts.any { it.kind == "agent" && it.status == "kept" }

    fun summary(place: String = ""): String {
        val at = if (place.isEmpty()) "" else " $place"
        val others = parts.filter { it.kind != "agent" }
        val agent = parts.firstOrNull { it.kind == "agent" }
        val lines = mutableListOf<String>()
        val deleted = others.filter { it.status == "deleted" }.map { it.label }
        if (agent == null || agent.status == "deleted") {
            lines += if (deleted.isEmpty()) "Deleted the agent$at." else "Deleted the agent$at with ${join(deleted)}."
        } else if (deleted.isNotEmpty()) {
            lines += "Deleted ${join(deleted)}$at."
        }
        val closed = others.filter { it.status == "closed" }.map { it.label }
        if (closed.isNotEmpty()) lines += "Closed ${join(closed)}."
        val gone = others.filter { it.status == "absent" }.map { it.label }
        if (gone.isNotEmpty()) lines += "Already gone: ${join(gone)}."
        for (part in parts.filter { it.status == "kept" }) {
            val why = part.reasonText
            lines += if (why.isEmpty()) "Kept ${part.label}." else "Kept ${part.label}: $why."
        }
        val pending = parts.filter { it.status == "pending" }.map { it.label }
        if (pending.isNotEmpty()) lines += "Still deleting ${join(pending)}."
        val missing = known.filter { kind -> parts.none { it.kind == kind } }.map(::noun)
        if (missing.isNotEmpty()) lines += "No ${join(missing, "or")} was attached."
        return lines.joinToString(" ")
    }

    companion object {
        val ATTACHED_KINDS = listOf("branch", "issue", "pull", "task")

        fun from(json: JsonMap): AgentDeleteReport? {
            val parts = json.maps("parts").mapNotNull(AgentDeletePart::from)
            if (parts.isEmpty()) return null
            return AgentDeleteReport(parts, json.optBool("settled") ?: true)
        }

        private fun noun(kind: String): String = if (kind == "pull") "pull request" else kind

        private fun join(items: List<String>, last: String = "and"): String = when (items.size) {
            0 -> ""
            1 -> items[0]
            else -> items.dropLast(1).joinToString(", ") + " $last " + items.last()
        }
    }
}

data class AgentDeleteOutcome(val failed: Boolean, val message: String) {
    companion object {
        fun deleted(message: String) = AgentDeleteOutcome(false, message)

        fun failed(message: String) = AgentDeleteOutcome(true, message)
    }
}
