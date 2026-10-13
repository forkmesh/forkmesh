package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.model.modelInt
import java.net.URI

enum class SecuritySeverity(val raw: String) {
    Critical("critical"), High("high"), Medium("medium"), Low("low"), Info("info");

    val label: String get() = if (this == Info) "Info" else raw.replaceFirstChar { it.uppercase() }

    companion object {
        fun parse(raw: String): SecuritySeverity = when (raw.lowercase()) {
            "critical" -> Critical
            "high" -> High
            "medium", "moderate" -> Medium
            "low" -> Low
            else -> Info
        }
    }
}

data class SecurityFinding(
    val id: String,
    val category: String,
    val severity: SecuritySeverity,
    val ruleId: String,
    val path: String,
    val line: Int,
    val summary: String,
    val recommendation: String,
    val reviewStatus: String,
    val advisoryIds: List<String>,
) {
    val location: String get() = if (line > 0) "$path:$line" else path

    companion object {
        fun fromJson(json: Map<String, Any?>): SecurityFinding? {
            val id = json.str("id").lowercase()
            if (id.isEmpty()) return null
            return SecurityFinding(
                id = id,
                category = json.str("category"),
                severity = SecuritySeverity.parse(json.str("severity")),
                ruleId = json.str("ruleId"),
                path = json.str("path"),
                line = modelInt(json["line"]).toInt(),
                summary = json.str("summary"),
                recommendation = json.str("recommendation"),
                reviewStatus = json.str("falsePositiveStatus").ifEmpty { "unreviewed" },
                advisoryIds = json.strings("advisoryIds"),
            )
        }
    }
}

data class SecurityScanSummary(
    val id: String,
    val receivedAt: String,
    val status: String,
    val scannedAt: String,
    val scanner: String,
    val commitHash: String,
    val counts: Map<SecuritySeverity, Int>,
    val reviewStatus: String,
    val findings: List<SecurityFinding>,
    val hasDetail: Boolean,
) {
    val total: Int get() = counts.values.sum()
    val shortCommit: String get() = commitHash.take(8)

    val headline: String
        get() {
            if (total == 0) return "No findings"
            return SecuritySeverity.entries.mapNotNull { severity ->
                val count = counts[severity] ?: 0
                if (count > 0) "$count ${severity.label.lowercase()}" else null
            }.joinToString(" · ")
        }

    val worst: SecuritySeverity? get() = SecuritySeverity.entries.firstOrNull { (counts[it] ?: 0) > 0 }

    companion object {
        fun fromJson(json: Map<String, Any?>): SecurityScanSummary? {
            val id = json.str("scanId")
            val clipboard = json.obj("clipboard")
            if (id.isEmpty() && clipboard.isEmpty()) return null
            val raw = clipboard.obj("findings")
            val rich = json["rich"] as? Map<*, *>
            @Suppress("UNCHECKED_CAST")
            val richMap = rich as? Map<String, Any?>
            return SecurityScanSummary(
                id = id,
                receivedAt = json.str("receivedAt"),
                status = clipboard.str("status"),
                scannedAt = clipboard.str("scannedAt"),
                scanner = clipboard.str("scanner"),
                commitHash = clipboard.str("commitHash"),
                counts = linkedMapOf(
                    SecuritySeverity.Critical to modelInt(raw["critical"]).toInt(),
                    SecuritySeverity.High to modelInt(raw["high"]).toInt(),
                    SecuritySeverity.Medium to modelInt(raw["medium"]).toInt(),
                    SecuritySeverity.Low to modelInt(raw["low"]).toInt(),
                    SecuritySeverity.Info to modelInt(raw["informational"] ?: raw["info"]).toInt(),
                ),
                reviewStatus = clipboard.str("reviewStatus"),
                findings = (richMap?.maps("findings") ?: emptyList()).take(500).mapNotNull(SecurityFinding::fromJson)
                    .sortedWith(compareBy<SecurityFinding> { it.severity.ordinal }.thenBy { it.location }),
                hasDetail = richMap != null,
            )
        }
    }
}

data class SecurityScanStatus(val due: Boolean, val status: String, val commit: String, val nextAttemptAt: Long) {
    val label: String
        get() = when {
            status == "leased" || status == "running" -> "A scan is running on a node."
            due -> "A new scan is due; the next node that picks it up will run it."
            else -> "Up to date for the current commit."
        }

    companion object {
        fun fromJson(json: Map<String, Any?>) = SecurityScanStatus(
            due = json["due"] == true,
            status = json.str("status"),
            commit = json.str("commit"),
            nextAttemptAt = modelInt(json["nextAttemptAt"]),
        )
    }
}

class RepoSecurityModel(private val client: CiClient, val repo: RepoRef) {
    @Volatile var latest: SecurityScanSummary? = null
        private set
    @Volatile var history: List<SecurityScanSummary> = emptyList()
        private set
    @Volatile var scanStatus: SecurityScanStatus? = null
        private set
    @Volatile var loading: Boolean = false
        private set
    @Volatile var error: String? = null
        private set
    @Volatile var busy: Boolean = false
        private set
    @Volatile var severityFilter: SecuritySeverity? = null

    val findings: List<SecurityFinding>
        get() {
            val all = latest?.findings.orEmpty()
            val filter = severityFilter ?: return all
            return all.filter { it.severity == filter }
        }

    fun load() {
        loading = true
        try {
            val rich = client.securityScans(repo, "history", listOf("limit" to "10", "detail" to "rich"))
            history = rich.maps("history").mapNotNull(SecurityScanSummary::fromJson)
            latest = history.firstOrNull()
            error = null
        } catch (_: Exception) {
            try {
                val data = client.securityScans(repo, "latest", emptyList())
                @Suppress("UNCHECKED_CAST")
                latest = (data["latest"] as? Map<String, Any?>)?.let(SecurityScanSummary::fromJson)
                history = listOfNotNull(latest)
                error = null
            } catch (failure: Exception) {
                error = failure.message ?: "$failure"
            }
        }
        try {
            scanStatus = SecurityScanStatus.fromJson(client.securityScans(repo, "lease", emptyList()))
        } catch (_: Exception) {
        }
        loading = false
    }

    fun triage(finding: SecurityFinding, status: String, note: String = ""): Boolean {
        val scan = latest
        if (status !in TRIAGE_STATUSES || scan == null || busy) return false
        busy = true
        return try {
            client.securityTriage(repo, scan.id, finding.id, status, note.take(500))
            latest = scan.copy(findings = scan.findings.map { if (it.id == finding.id) it.copy(reviewStatus = status) else it })
            error = null
            true
        } catch (failure: Exception) {
            error = failure.message ?: "$failure"
            false
        } finally {
            busy = false
        }
    }

    companion object {
        val TRIAGE_STATUSES = listOf("unreviewed", "confirmed", "dismissed")
    }
}

data class RepoWebhook(
    val id: String,
    val url: String,
    val events: List<String>,
    val description: String,
    val active: Boolean,
    val managed: Boolean,
    val lastStatus: String,
    val lastDeliveryAtMs: Long,
    val consecutiveFailures: Int,
    val secret: String,
) {
    val healthLabel: String
        get() = when {
            !active -> "Paused"
            consecutiveFailures > 0 -> "$consecutiveFailures failed ${if (consecutiveFailures == 1) "delivery" else "deliveries"}"
            lastDeliveryAtMs > 0 -> if (lastStatus.isEmpty()) "Delivered" else lastStatus.replaceFirstChar { it.uppercase() }
            else -> "No deliveries yet"
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): RepoWebhook? {
            val id = json.str("id")
            if (id.isEmpty()) return null
            return RepoWebhook(
                id = id,
                url = json.str("url"),
                events = json.strings("events"),
                description = json.str("description"),
                active = json["active"] != false,
                managed = json["managed"] == true,
                lastStatus = json.str("lastStatus"),
                lastDeliveryAtMs = modelInt(json["lastDeliveryAt"]),
                consecutiveFailures = modelInt(json["consecutiveFailures"]).toInt(),
                secret = json.str("secret"),
            )
        }
    }
}

data class RepoWebhookEventType(val type: String, val label: String, val hint: String) {
    companion object {
        val DEFAULTS = listOf(
            RepoWebhookEventType("repository.push", "Repository push", "Any push to the repository"),
            RepoWebhookEventType("branch.push", "Branch push", "A push to one branch"),
            RepoWebhookEventType("pull.merged", "Pull request merged", "A pull request was merged"),
            RepoWebhookEventType("release.published", "Release published", "A release was announced"),
        )
    }
}

data class RepoWebhookDelivery(
    val webhookId: String,
    val eventType: String,
    val status: String,
    val attempts: Int,
    val outcome: String,
    val tsMs: Long,
)

data class RepoWebhookDraft(val url: String = "", val events: Set<String> = emptySet(), val description: String = "") {
    val validationError: String?
        get() {
            val trimmed = url.trim()
            val parsed = try {
                URI(trimmed)
            } catch (_: Exception) {
                null
            }
            if (parsed == null || parsed.scheme?.lowercase() != "https" || parsed.host.isNullOrEmpty()) return "Use an https:// address."
            if (trimmed.length > 2048) return "That address is too long."
            if (events.isEmpty()) return "Choose at least one event."
            if (description.length > 200) return "Keep the description under 200 characters."
            return null
        }

    val fields: Map<String, Any?>
        get() = linkedMapOf(
            "url" to url.trim(),
            "events" to events.sorted(),
            "description" to description.trim(),
        )
}

class RepoWebhooksModel(private val client: CiClient, val repo: RepoRef) {
    @Volatile var webhooks: List<RepoWebhook> = emptyList()
        private set
    @Volatile var eventTypes: List<RepoWebhookEventType> = RepoWebhookEventType.DEFAULTS
        private set
    @Volatile var deliveries: List<RepoWebhookDelivery> = emptyList()
        private set
    @Volatile var maxWebhooks: Int = 20
        private set
    @Volatile var loading: Boolean = false
        private set
    @Volatile var busy: Boolean = false
        private set
    @Volatile var error: String? = null
        private set
    @Volatile var revealedSecret: Pair<String, String>? = null
        private set

    val canAdd: Boolean get() = webhooks.size < maxWebhooks

    fun deliveries(id: String): List<RepoWebhookDelivery> = deliveries.filter { it.webhookId == id }

    fun dismissSecret() {
        revealedSecret = null
    }

    fun apply(data: Map<String, Any?>) {
        webhooks = data.maps("webhooks").take(50).mapNotNull(RepoWebhook::fromJson)
        val types = data.maps("events").mapNotNull { map ->
            val type = map.str("type")
            if (type.isEmpty()) null else RepoWebhookEventType(type, map.str("label").ifEmpty { type }, map.str("hint"))
        }
        if (types.isNotEmpty()) eventTypes = types
        deliveries = data.maps("deliveries").take(100).map {
            RepoWebhookDelivery(
                webhookId = it.str("webhookId"),
                eventType = it.str("eventType"),
                status = it.str("status"),
                attempts = modelInt(it["attempts"]).toInt(),
                outcome = it.str("outcome"),
                tsMs = modelInt(it["ts"]),
            )
        }
        val limit = modelInt(data.obj("limits")["maxWebhooks"]).toInt()
        if (limit > 0) maxWebhooks = limit
    }

    fun load() {
        loading = true
        try {
            apply(client.webhooks(repo, "list", emptyMap()))
            error = null
        } catch (failure: Exception) {
            error = failure.message ?: "$failure"
        } finally {
            loading = false
        }
    }

    private fun mutate(action: String, fields: Map<String, Any?>): Map<String, Any?>? {
        if (busy) return null
        busy = true
        return try {
            val data = client.webhooks(repo, action, fields)
            error = null
            data
        } catch (failure: Exception) {
            error = failure.message ?: "$failure"
            null
        } finally {
            busy = false
        }
    }

    private fun reveal(data: Map<String, Any?>) {
        val hook = RepoWebhook.fromJson(data.obj("webhook")) ?: return
        if (hook.secret.isNotEmpty()) revealedSecret = hook.id to hook.secret
    }

    fun create(draft: RepoWebhookDraft): Boolean {
        draft.validationError?.let {
            error = it
            return false
        }
        if (!canAdd) {
            error = "A repository can have at most $maxWebhooks webhooks."
            return false
        }
        val data = mutate("create", draft.fields) ?: return false
        reveal(data)
        load()
        return true
    }

    fun update(id: String, draft: RepoWebhookDraft): Boolean {
        draft.validationError?.let {
            error = it
            return false
        }
        val fields = LinkedHashMap(draft.fields)
        fields["id"] = id
        fields["active"] = webhooks.firstOrNull { it.id == id }?.active ?: true
        mutate("update", fields) ?: return false
        load()
        return true
    }

    fun setActive(id: String, active: Boolean): Boolean {
        mutate("update", linkedMapOf("id" to id, "active" to active)) ?: return false
        load()
        return true
    }

    fun rotate(id: String): Boolean {
        val data = mutate("rotate", linkedMapOf("id" to id)) ?: return false
        reveal(data)
        return true
    }

    fun delete(id: String): Boolean {
        mutate("delete", linkedMapOf("id" to id)) ?: return false
        webhooks = webhooks.filterNot { it.id == id }
        return true
    }

    fun test(id: String): String {
        val data = mutate("test", linkedMapOf("id" to id)) ?: return error ?: "The test was not sent."
        val outcome = data.str("outcome")
        val status = modelInt(data["status"]).toInt()
        if (status in 200..299) return "Test delivered (HTTP $status)."
        return if (outcome.isEmpty()) "Test answered HTTP $status." else "Test failed: $outcome."
    }
}
