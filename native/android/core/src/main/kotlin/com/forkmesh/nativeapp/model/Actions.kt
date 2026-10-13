package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str

private fun dartString(value: Any?): String = if (value == null) "null" else jsonText(value)

data class RepoActions(
    val workflows: List<ActionWorkflow> = emptyList(),
    val runs: List<ActionRun> = emptyList(),
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): RepoActions = RepoActions(
            workflows = json.maps("workflows").map { ActionWorkflow.fromJson(LinkedHashMap(it)) },
            runs = json.maps("runs").map { ActionRun.fromJson(LinkedHashMap(it)) },
        )
    }
}

data class ActionWorkflow(
    val path: String = "",
    val name: String = "",
    val triggers: List<String> = emptyList(),
    val stepCount: Int = 0,
    val manual: Boolean = false,
    val valid: Boolean = true,
    val error: String = "",
) {
    val displayName: String get() = if (name.isNotEmpty()) name else path

    val triggerLabel: String get() = if (triggers.isEmpty()) "No triggers" else triggers.joinToString(", ")

    companion object {
        fun fromJson(json: Map<String, Any?>): ActionWorkflow {
            val on = json["on"]
            val rawTriggers: List<Any?> = when {
                on is List<*> -> on
                json["triggers"] is List<*> -> json["triggers"] as List<*>
                on is String -> listOf(on)
                else -> emptyList()
            }
            return ActionWorkflow(
                path = json.str("path"),
                name = json.str("name"),
                triggers = rawTriggers.map(::dartString),
                stepCount = modelInt(json["stepCount"] ?: json["steps"]).toInt(),
                manual = json["manual"] == true,
                valid = json["valid"] != false,
                error = json.str("error"),
            )
        }
    }
}

data class ActionRun(
    val id: Int,
    val workflowPath: String = "",
    val workflowName: String = "",
    val commit: String = "",
    val ref: String = "",
    val status: String = "",
    val createdAtMs: Long = 0,
    val startedAtMs: Long = 0,
    val finishedAtMs: Long = 0,
) {
    val isActive: Boolean
        get() {
            val clean = status.lowercase().replace('_', '-')
            return clean == "queued" || clean == "running" || clean == "awaiting-approval"
        }

    val displayName: String get() = if (workflowName.isNotEmpty()) workflowName else "Action run #$id"

    val statusLabel: String
        get() = when (status.lowercase().replace('_', '-')) {
            "awaiting-approval" -> "Awaiting approval"
            "queued" -> "Queued"
            "running" -> "Running"
            "success", "passed" -> "Success"
            "failed", "failure", "error" -> "Failed"
            "rejected" -> "Rejected"
            "cancelled", "canceled", "stopped" -> "Cancelled"
            else -> if (status.isEmpty()) "Unknown" else modelTitleCase(status)
        }

    val refLabel: String
        get() {
            if (ref.startsWith("refs/heads/")) return ref.substring("refs/heads/".length)
            if (ref.startsWith("refs/tags/")) return ref.substring("refs/tags/".length)
            return ref
        }

    val shortCommit: String get() = if (commit.length <= 8) commit else commit.substring(0, 8)

    val durationLabel: String
        get() {
            if (startedAtMs <= 0 || finishedAtMs <= startedAtMs) return ""
            val totalSeconds = (finishedAtMs - startedAtMs) / 1000
            val minutes = totalSeconds / 60
            val seconds = totalSeconds % 60
            if (minutes <= 0) return "${seconds}s"
            return "${minutes}m ${seconds}s"
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): ActionRun = ActionRun(
            id = modelInt(json["id"]).toInt(),
            workflowPath = json.str("workflowPath"),
            workflowName = json.str("workflowName"),
            commit = json.str("commit"),
            ref = json.str("ref"),
            status = json.str("status"),
            createdAtMs = modelInt(json["createdAtMs"]),
            startedAtMs = modelInt(json["startedAtMs"]),
            finishedAtMs = modelInt(json["finishedAtMs"]),
        )
    }
}

data class ActionLog(
    val id: Int,
    val status: String = "",
    val log: String = "",
) {
    val statusLabel: String get() = ActionRun(id = id, status = status).statusLabel

    companion object {
        fun fromJson(json: Map<String, Any?>): ActionLog = ActionLog(
            id = modelInt(json["id"]).toInt(),
            status = json.str("status"),
            log = json.str("log"),
        )
    }
}

data class DesktopCommandResult(
    val ok: Boolean,
    val queued: Int = 0,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): DesktopCommandResult =
            DesktopCommandResult(ok = json["ok"] == true, queued = modelInt(json["queued"]).toInt())
    }
}
