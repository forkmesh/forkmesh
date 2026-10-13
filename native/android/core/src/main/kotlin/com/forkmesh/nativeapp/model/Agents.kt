package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

data class AgentTranscript(
    val status: String,
    val transcript: String,
    val sealedForOtherDevice: Boolean = false,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): AgentTranscript =
            AgentTranscript(status = json.str("status"), transcript = json.str("transcript"))
    }
}

data class AgentSessionPage(
    val sessions: List<AgentSession> = emptyList(),
    val sealedForOthers: Int = 0,
)

data class AgentSession(
    val id: Int,
    val issueNumber: Int = 0,
    val issueTitle: String = "",
    val status: String = "",
    val provider: String = "",
    val model: String = "",
    val branchName: String = "",
    val lastError: String = "",
    val createdAtMs: Long = 0,
    val startedAtMs: Long = 0,
    val finishedAtMs: Long = 0,
    val numTurns: Int = 0,
    val durationMs: Long = 0,
    val costUsd: Double = 0.0,
    val createPr: Boolean = false,
    val prNumber: Int = 0,
    val baseRef: String = "",
    val baseBranch: String = "",
    val merged: Boolean = false,
    val mergedAtMs: Long = 0,
    val promptTokens: Int = 0,
    val completionTokens: Int = 0,
    val totalTokens: Int = 0,
    val contextTokens: Int = 0,
    val contextWindow: Int = 0,
    val maxOutputTokens: Int = 0,
    val estimatedCredits: Int = 0,
    val spendBeforeUsd: Double = 0.0,
    val spendAfterUsd: Double = 0.0,
    val filesChanged: Int = -1,
    val ahead: Int = -1,
    val behind: Int = -1,
    val conflicted: Boolean = false,
    val statusReason: String = "",
    val rateLimitResetAtMs: Long = 0,
    val rateLimitResetAfterMs: Long = 0,
    val priority: Int = 50,
) {
    private val normalizedStatus: String get() = status.lowercase().replace('_', '-')

    val isActive: Boolean
        get() {
            if (merged) return false
            val clean = normalizedStatus
            return clean == "queued" || clean == "running" || clean == "waiting"
        }

    val displayTitle: String
        get() = when {
            issueTitle.isNotEmpty() -> issueTitle
            issueNumber > 0 -> "Issue #$issueNumber"
            else -> "Agent session #$id"
        }

    val statusLabel: String
        get() {
            if (merged) return "Merged"
            return when (normalizedStatus) {
                "queued" -> "Queued"
                "running" -> "Running"
                "waiting", "waiting-for-input" -> "Waiting"
                "done", "completed", "success" -> "Done"
                "failed", "error" -> "Failed"
                "stopped", "cancelled", "canceled" -> "Stopped"
                else -> if (status.isEmpty()) "Unknown" else modelTitleCase(status)
            }
        }

    val isSuccessful: Boolean
        get() {
            if (merged) return true
            val normalized = normalizedStatus
            return normalized == "done" || normalized == "completed" || normalized == "success"
        }

    val isFailed: Boolean
        get() {
            val normalized = normalizedStatus
            return normalized == "failed" || normalized == "error"
        }

    val statusReasonText: String get() = if (statusReason.isNotEmpty()) statusReason else lastError

    fun successSummaryLabel(): String {
        val parts = ArrayList<String>()
        if (numTurns > 0) parts.add("$numTurns turns")
        if (durationLabel.isNotEmpty()) parts.add(durationLabel)
        if (costLabel.isNotEmpty()) parts.add(costLabel)
        if (diffLabel.isNotEmpty()) parts.add(diffLabel)
        val suffix = if (parts.isEmpty()) "" else " · " + parts.joinToString(" · ")
        return "Done$suffix"
    }

    fun latestStatusSummary(maxReasonLength: Int = 120): String {
        if (isSuccessful) return successSummaryLabel()
        if (isFailed) {
            val reason = statusReasonText.trim()
            if (reason.isEmpty()) return "Failed"
            return "Failed: " + modelTruncate(reason, maxReasonLength)
        }
        return statusLabel
    }

    fun statusFailureReason(maxLength: Int): String = modelTruncate(statusReasonText, maxLength)

    fun remainingRateLimitResetMs(nowMs: Long = System.currentTimeMillis()): Long {
        val atMs = rateLimitResetAtMs
        if (atMs <= 0) return 0
        return atMs - nowMs
    }

    fun rateLimitCountdownLabel(nowMs: Long = System.currentTimeMillis()): String {
        val remainingMs = remainingRateLimitResetMs(nowMs)
        if (remainingMs <= 0) return ""
        return "Limit resets in " + modelCompactDuration(remainingMs)
    }

    val providerLabel: String
        get() = when (provider.lowercase()) {
            "claude-code" -> "Claude Code"
            "claude-api" -> "Claude API"
            "openai" -> "OpenAI"
            "codex" -> "Codex"
            "cloudflare-ai" -> "Cloudflare AI"
            else -> if (provider.isEmpty()) "Agent" else modelTitleCase(provider)
        }

    val durationLabel: String
        get() {
            if (durationMs <= 0) return ""
            val totalSeconds = durationMs / 1000
            val minutes = totalSeconds / 60
            val seconds = totalSeconds % 60
            if (minutes <= 0) return "${seconds}s"
            return "${minutes}m ${seconds}s"
        }

    val costLabel: String
        get() {
            if (costUsd <= 0) return ""
            if (costUsd < 0.01) return "<$0.01"
            return "$" + dartToStringAsFixed(costUsd, 2)
        }

    val prLabel: String get() = if (prNumber > 0) "PR #$prNumber" else ""

    val diffBase: String get() = baseBranch.ifEmpty { baseRef }

    val tokenLabel: String get() = if (totalTokens > 0) "$totalTokens tokens" else ""

    val diffLabel: String
        get() {
            val parts = ArrayList<String>()
            if (filesChanged >= 0) parts.add(if (filesChanged == 1) "1 file" else "$filesChanged files")
            if (ahead >= 0 && behind >= 0 && (ahead > 0 || behind > 0)) parts.add("↑$ahead ↓$behind")
            return parts.joinToString(" · ")
        }

    companion object {
        private val RESET_AT_KEYS = listOf(
            "rateLimitResetAtMs",
            "limitResetAtMs",
            "rateLimitResetAt",
            "limitResetAt",
            "ratelimitResetAt",
        )

        private val RESET_AFTER_KEYS = listOf(
            "rateLimitResetAfterMs",
            "retryAfterMs",
            "limitResetAfterMs",
            "retryAfter",
            "rateLimitResetAfter",
            "ratelimitResetAfter",
            "xRateLimitResetAfter",
            "x-ratelimit-reset-after",
            "xrateLimitResetAfter",
        )

        fun fromJson(json: Map<String, Any?>): AgentSession {
            val diffStats = json.obj("diffStats")
            val nowMs = System.currentTimeMillis()
            val parsedStatusReason = firstModelString(json, listOf("statusReason", "failureReason", "reason"))
            var resetAtMs = 0L
            for (key in RESET_AT_KEYS) {
                val value = modelEpochMs(json[key])
                if (value > 0) {
                    resetAtMs = value
                    break
                }
            }
            var resetAfterMs = 0L
            for (key in RESET_AFTER_KEYS) {
                val lower = key.lowercase()
                val value = modelInt(json[key])
                if (value > 0) {
                    resetAfterMs = if (lower.contains("after") && !lower.endsWith("ms")) value * 1000 else value
                    break
                }
            }
            if (resetAtMs <= 0 && resetAfterMs > 0) resetAtMs = nowMs + resetAfterMs
            return AgentSession(
                id = modelInt(json["id"]).toInt(),
                issueNumber = modelInt(json["issueNumber"]).toInt(),
                issueTitle = json.str("issueTitle"),
                status = json.str("status"),
                provider = json.str("provider"),
                model = json.str("model"),
                branchName = json.str("branchName"),
                lastError = json.str("lastError"),
                createdAtMs = modelInt(json["createdAtMs"]),
                startedAtMs = modelInt(json["startedAtMs"]),
                finishedAtMs = modelInt(json["finishedAtMs"]),
                numTurns = modelInt(json["numTurns"]).toInt(),
                durationMs = modelInt(json["durationMs"]),
                costUsd = modelDouble(json["costUsd"]),
                createPr = json["createPr"] == true,
                prNumber = modelInt(json["prNumber"]).toInt(),
                baseRef = json.str("baseRef"),
                baseBranch = json.str("baseBranch"),
                merged = json["merged"] == true,
                mergedAtMs = modelInt(json["mergedAtMs"]),
                promptTokens = modelInt(json["promptTokens"]).toInt(),
                completionTokens = modelInt(json["completionTokens"]).toInt(),
                totalTokens = modelInt(json["totalTokens"]).toInt(),
                contextTokens = modelInt(json["contextTokens"]).toInt(),
                contextWindow = modelInt(json["contextWindow"]).toInt(),
                maxOutputTokens = modelInt(json["maxOutputTokens"]).toInt(),
                estimatedCredits = modelInt(json["estimatedCredits"]).toInt(),
                spendBeforeUsd = modelDouble(json["spendBeforeUsd"]),
                spendAfterUsd = modelDouble(json["spendAfterUsd"]),
                filesChanged = modelInt(diffStats["files"] ?: json["filesChanged"] ?: json["changedFiles"], -1).toInt(),
                ahead = modelInt(diffStats["ahead"] ?: json["ahead"], -1).toInt(),
                behind = modelInt(diffStats["behind"] ?: json["behind"], -1).toInt(),
                conflicted = diffStats["conflicted"] == true || json["conflicted"] == true,
                statusReason = parsedStatusReason,
                rateLimitResetAtMs = resetAtMs,
                rateLimitResetAfterMs = resetAfterMs,
                priority = modelInt(json["priority"]).toInt().let { if (it <= 0) 50 else it.coerceIn(1, 99) },
            )
        }
    }
}
