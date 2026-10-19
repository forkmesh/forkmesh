package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.util.Locale

enum class ApprovalLifetime(val wire: String, val title: String, val detail: String) {
    Once("once", "Allow once", "Only this request."),
    Session("session", "Allow for this session", "Matching requests until this run ends."),
    Worktree("worktree", "Allow for this worktree", "Matching requests in this worktree."),
    Persistent("persistent", "Always allow", "Saved on the desktop until you revoke it."),
    ;

    companion object {
        fun of(wire: String): ApprovalLifetime? = entries.firstOrNull { it.wire == wire }
    }
}

enum class ApprovalKind(val wire: String, val title: String, val icon: String) {
    Command("command", "Run a command", "terminal"),
    Network("network", "Reach the network", "public"),
    File("file", "Use a file", "description"),
    Secret("secret", "Read a secret", "key"),
    Tool("tool", "Use a tool", "build"),
    Other("other", "Permission", "back_hand"),
    ;

    companion object {
        fun of(wire: String): ApprovalKind = entries.firstOrNull { it.wire == wire } ?: Other
    }
}

object AgentOpsIds {
    fun isApprovalId(id: String): Boolean =
        id.length == 32 && id.all { it in '0'..'9' || it in 'a'..'f' }
}

data class PendingApproval(
    val id: String,
    val sessionId: Int,
    val title: String = "",
    val repo: String = "",
    val provider: String = "",
    val capability: String = "",
    val kind: ApprovalKind = ApprovalKind.Other,
    val question: String = "",
    val command: String = "",
    val askedAtMs: Long = 0,
    val lifetimes: List<ApprovalLifetime> = ApprovalLifetime.entries,
) {
    val headline: String
        get() {
            val firstLine = { text: String -> text.split('\n').firstOrNull { it.isNotEmpty() } ?: "" }
            if (command.isNotEmpty()) return firstLine(command)
            val line = firstLine(question)
            return line.ifEmpty { kind.title }
        }

    val subtitle: String get() = listOf(title, repo).filter { it.isNotEmpty() }.joinToString(" · ")

    companion object {
        fun from(json: Map<String, Any?>): PendingApproval? {
            val id = json.str("id")
            if (!AgentOpsIds.isApprovalId(id)) return null
            val lifetimes = json.strings("lifetimes").mapNotNull(ApprovalLifetime::of)
            return PendingApproval(
                id = id,
                sessionId = json.int("sessionId"),
                title = json.str("title"),
                repo = json.str("repo"),
                provider = json.str("provider"),
                capability = json.str("capability"),
                kind = ApprovalKind.of(json.str("kind")),
                question = json.str("question"),
                command = json.str("command"),
                askedAtMs = json.long("askedAtMs"),
                lifetimes = lifetimes.ifEmpty { listOf(ApprovalLifetime.Once) },
            )
        }
    }
}

enum class AgentOpsAction(val wire: String, val title: String) {
    Approve("approve", "Approve"),
    Deny("deny", "Deny"),
    Merge("merge", "Merge & clean up"),
    Stop("stop", "Stop"),
    ContinueRun("continue", "Continue"),
    Revoke("revoke", "Revoke"),
    Delete("delete", "Delete"),
    Purge("purge", "Delete permanently"),
    ;

    val isDestructive: Boolean get() = this == Merge || this == Delete || this == Purge

    companion object {
        fun of(wire: String): AgentOpsAction? = entries.firstOrNull { it.wire == wire }
    }
}

enum class BulkAgentAction(val wire: String, val title: String) {
    Stop("stop", "Stop"),
    ContinueRun("continue", "Continue"),
    ;

    val opsAction: AgentOpsAction get() = if (this == Stop) AgentOpsAction.Stop else AgentOpsAction.ContinueRun
}

data class AgentOpsConfirmation(
    val token: String,
    val action: AgentOpsAction,
    val summary: String,
    val destructive: Boolean = false,
    val typeToConfirm: String = "",
    val expiresInMs: Int = 60_000,
    val issuedAtMs: Long = 0,
) {
    fun isExpired(nowMs: Long): Boolean = nowMs - issuedAtMs >= expiresInMs.toLong()

    companion object {
        fun from(json: Map<String, Any?>, issuedAtMs: Long): AgentOpsConfirmation? {
            val action = AgentOpsAction.of(json.str("action")) ?: return null
            val token = json.str("token")
            if (token.isEmpty()) return null
            return AgentOpsConfirmation(
                token = token,
                action = action,
                summary = json.str("summary"),
                destructive = json.bool("destructive"),
                typeToConfirm = json.str("typeToConfirm"),
                expiresInMs = json.int("expiresInMs", 60_000),
                issuedAtMs = issuedAtMs,
            )
        }
    }
}

data class BulkOutcome(val action: BulkAgentAction, val done: List<Int>, val skipped: List<Int>) {
    val message: String
        get() {
            val verb = if (action == BulkAgentAction.Stop) "Stopped" else "Continued"
            val base = if (done.size == 1) "$verb 1 agent" else "$verb ${done.size} agents"
            if (skipped.isEmpty()) return "$base."
            val others = if (skipped.size == 1) "1 was" else "${skipped.size} were"
            return "$base; $others not in a state to ${action.title.lowercase()}."
        }
}

data class RecycledAgent(
    val id: Int,
    val title: String = "",
    val owner: String = "",
    val name: String = "",
    val branch: String = "",
    val provider: String = "",
    val deletedAtMs: Long = 0,
    val removeInMs: Long = -1,
) {
    val repo: String get() = if (owner.isEmpty()) name else "$owner/$name"

    val headline: String get() = title.ifEmpty { "Agent #$id" }

    val removal: String
        get() {
            val hourMs = 60L * 60 * 1000
            return when {
                removeInMs >= hourMs -> "Removed for good in ${removeInMs / hourMs} h"
                removeInMs > 0 -> "Removed for good in under an hour"
                else -> "Removed for good at the next sweep"
            }
        }

    companion object {
        fun from(json: Map<String, Any?>): RecycledAgent? {
            val id = json.int("id")
            if (id <= 0) return null
            return RecycledAgent(
                id = id,
                title = json.str("title"),
                owner = json.str("owner"),
                name = json.str("name"),
                branch = json.str("branch"),
                provider = json.str("provider"),
                deletedAtMs = json.long("deletedAtMs"),
                removeInMs = json.long("removeInMs", -1),
            )
        }
    }
}

data class RecycleBin(
    val agents: List<RecycledAgent> = emptyList(),
    val count: Int = 0,
    val truncated: Boolean = false,
    val retentionHours: Int = 24,
    val minHours: Int = 1,
    val maxHours: Int = 720,
) {
    val isEmpty: Boolean get() = agents.isEmpty()

    val ids: List<Int> get() = agents.map { it.id }

    val hint: String
        get() = if (isEmpty) {
            "The recycle bin on the desktop is empty."
        } else {
            "Deleted agents are kept on the desktop for $retentionHours hours. Restoring brings back the session and its " +
                "transcript; a pull request or issue assignment removed by the delete is not recreated."
        }

    companion object {
        fun from(json: Map<String, Any?>): RecycleBin {
            val agents = json.maps("agents").mapNotNull(RecycledAgent::from)
            return RecycleBin(
                agents = agents,
                count = maxOf(json.int("count", agents.size), agents.size),
                truncated = json.bool("truncated"),
                retentionHours = json.int("retentionHours", 24),
                minHours = json.int("minHours", 1),
                maxHours = json.int("maxHours", 720),
            )
        }
    }
}

sealed class RecycleBinChange {
    abstract val message: String

    data class Restored(val done: List<Int>, val failed: List<Int>) : RecycleBinChange() {
        override val message: String
            get() {
                var text = when {
                    done.isEmpty() -> ""
                    done.size == 1 -> "Agent restored."
                    else -> "${done.size} agents restored."
                }
                if (failed.isNotEmpty()) {
                    val lead = if (failed.size == 1) "1 agent" else "${failed.size} agents"
                    text += (if (text.isEmpty()) "" else " ") + "Could not restore $lead; a live agent may already use that id."
                }
                return text
            }
    }

    data class Purged(val done: List<Int>, val skipped: List<Int>) : RecycleBinChange() {
        override val message: String
            get() {
                var text = if (done.size == 1) "1 agent deleted for good." else "${done.size} agents deleted for good."
                if (skipped.isNotEmpty()) text += if (skipped.size == 1) " 1 was already gone." else " ${skipped.size} were already gone."
                return text
            }
    }
}

data class BulkCandidate(val id: Int, val isActive: Boolean, val stoppable: Boolean, val merged: Boolean = false) {
    fun allows(action: BulkAgentAction): Boolean = when (action) {
        BulkAgentAction.Stop -> isActive && stoppable
        BulkAgentAction.ContinueRun -> !isActive && !merged
    }
}

data class AgentWorktree(
    val name: String,
    val branch: String,
    val head: String = "",
    val isMain: Boolean = false,
    val detached: Boolean = false,
    val locked: Boolean = false,
    val prunable: Boolean = false,
    val ahead: Int = 0,
    val behind: Int = 0,
    val sessionId: Int? = null,
) {
    val id: String get() = "$name|$branch"

    val syncLabel: String
        get() = when {
            isMain -> "Main checkout"
            detached -> "Detached"
            ahead == 0 && behind == 0 -> "Up to date"
            behind == 0 -> "$ahead ahead"
            ahead == 0 -> "$behind behind"
            else -> "$ahead ahead, $behind behind"
        }

    companion object {
        fun from(json: Map<String, Any?>): AgentWorktree {
            val session = json.int("sessionId")
            return AgentWorktree(
                name = json.str("name"),
                branch = json.str("branch"),
                head = json.str("head"),
                isMain = json.bool("isMain"),
                detached = json.bool("detached"),
                locked = json.bool("locked"),
                prunable = json.bool("prunable"),
                ahead = json.int("ahead"),
                behind = json.int("behind"),
                sessionId = if (session > 0) session else null,
            )
        }
    }
}

data class WorktreeList(val repo: String = "", val base: String = "", val worktrees: List<AgentWorktree> = emptyList()) {
    companion object {
        fun from(json: Map<String, Any?>): WorktreeList =
            WorktreeList(json.str("repo"), json.str("base"), json.maps("worktrees").map(AgentWorktree::from))
    }
}

enum class UsageState(val wire: String) {
    Ok("ok"),
    High("high"),
    Exhausted("exhausted"),
    Unknown("unknown"),
    ;

    companion object {
        fun of(wire: String): UsageState = entries.firstOrNull { it.wire == wire } ?: Unknown
    }
}

data class UsageWindowMeter(
    val id: String,
    val label: String,
    val usedPercent: Int?,
    val resetAtMs: Long? = null,
    val state: UsageState,
    val windowMs: Long? = inferredWindowMs(id, label),
) {
    val remainingPercent: Int? get() = usedPercent?.let { 100 - it }

    fun resetLabel(nowMs: Long): String {
        val reset = resetAtMs ?: return ""
        if (reset <= nowMs) return ""
        return "resets in " + UsageFormat.remaining(reset - nowMs)
    }

    fun resetClockLabel(nowMs: Long, zone: java.time.ZoneId = java.time.ZoneId.systemDefault()): String =
        resetAtMs?.let { UsageFormat.resetClock(it, nowMs, zone) }.orEmpty()

    val stepMs: Long? get() = windowMs?.takeIf { it > 0 }?.let { if (it > DAY_MS) DAY_MS else HOUR_MS }

    val segments: Int get() {
        val window = windowMs ?: return 1
        val step = stepMs ?: return 1
        return ((window + step - 1) / step).toInt()
    }

    fun elapsedFraction(nowMs: Long): Double? {
        val window = windowMs?.takeIf { it > 0 } ?: return null
        val reset = resetAtMs?.takeIf { it > nowMs } ?: return null
        return (1.0 - (reset - nowMs).toDouble() / window).coerceIn(0.0, 1.0)
    }

    fun periodLabel(nowMs: Long): String {
        val window = windowMs ?: return ""
        val step = stepMs ?: return ""
        val elapsed = elapsedFraction(nowMs) ?: return ""
        val current = minOf(segments, (elapsed * window / step).toInt() + 1)
        return "${if (step == DAY_MS) "Day" else "Hour"} $current of $segments"
    }

    fun paceLabel(nowMs: Long): String {
        val used = usedPercent ?: return ""
        val elapsed = elapsedFraction(nowMs) ?: return ""
        val ahead = used - Math.round(elapsed * 100).toInt()
        if (ahead == 0) return "on pace"
        return "${kotlin.math.abs(ahead)}% ${if (ahead > 0) "over" else "under"} pace"
    }

    companion object {
        const val HOUR_MS: Long = 3_600_000
        const val DAY_MS: Long = 86_400_000

        fun inferredWindowMs(id: String, label: String): Long? {
            val text = "$id $label".lowercase()
            if ("5h" in text || "5-hour" in text || "five_hour" in text) return 5 * HOUR_MS
            if ("week" in text || "seven_day" in text || "fable" in text) return 7 * DAY_MS
            return null
        }

        fun from(json: Map<String, Any?>): UsageWindowMeter {
            val used = json.int("usedPercent", -1)
            val reset = json.long("resetAtMs")
            val window = json.long("windowMs")
            val id = json.str("id")
            val label = json.str("label")
            return UsageWindowMeter(
                id = id,
                label = label,
                usedPercent = if (used >= 0) minOf(used, 100) else null,
                resetAtMs = if (reset > 0) reset else null,
                state = UsageState.of(json.str("state")),
                windowMs = if (window > 0) window else inferredWindowMs(id, label),
            )
        }
    }
}

data class UsageAccount(
    val id: String,
    val label: String,
    val active: Boolean,
    val signedIn: Boolean? = null,
    val identity: String = "",
    val windows: List<UsageWindowMeter> = emptyList(),
) {
    val signedOut: Boolean get() = signedIn == false

    val isDefault: Boolean get() = id == DEFAULT_ID

    val name: String get() = label.ifEmpty { identity.ifEmpty { "Account" } }

    val statusLabel: String
        get() = when {
            signedOut -> "Not signed in"
            active -> "In use"
            else -> "Available"
        }

    val detail: String get() = identity.ifEmpty { if (signedOut) "Not signed in" else "Signed in" }

    companion object {
        const val DEFAULT_ID = "default"

        fun from(json: Map<String, Any?>): UsageAccount = UsageAccount(
            id = json.str("id"),
            label = json.str("label"),
            active = json.bool("active"),
            signedIn = json["signedIn"] as? Boolean,
            identity = json.str("identity"),
            windows = json.maps("windows").map(UsageWindowMeter::from),
        )
    }
}

data class UsageCredits(val used: Double?, val limit: Double?, val currency: String) {
    val label: String
        get() {
            val unit = if (currency.isEmpty()) "" else " $currency"
            return when {
                used != null && limit != null -> String.format(Locale.US, "%.2f of %.2f", used, limit) + unit
                used != null -> String.format(Locale.US, "%.2f used", used) + unit
                else -> "Extra usage on"
            }
        }
}

data class UsageProviderMeter(
    val id: String,
    val label: String,
    val windows: List<UsageWindowMeter> = emptyList(),
    val accounts: List<UsageAccount> = emptyList(),
    val credits: UsageCredits? = null,
) {
    val activeAccount: UsageAccount? get() = accounts.firstOrNull { it.active }

    fun windowsFor(account: UsageAccount): List<UsageWindowMeter> =
        if (account.windows.isNotEmpty() || !account.active) account.windows else windows

    val worstState: UsageState
        get() {
            val states = windows.map { it.state }
            return when {
                UsageState.Exhausted in states -> UsageState.Exhausted
                UsageState.High in states -> UsageState.High
                UsageState.Ok in states -> UsageState.Ok
                else -> UsageState.Unknown
            }
        }

    companion object {
        fun from(json: Map<String, Any?>): UsageProviderMeter {
            val accounts = json.maps("accounts").map(UsageAccount::from)
            val creditsJson = json["credits"] as? Map<*, *>
            val credits = creditsJson?.let {
                @Suppress("UNCHECKED_CAST")
                val map = it as Map<String, Any?>
                UsageCredits(numberOrNull(map["used"]), numberOrNull(map["limit"]), map.str("currency"))
            }
            return UsageProviderMeter(
                id = json.str("id"),
                label = json.str("label"),
                windows = json.maps("windows").map(UsageWindowMeter::from),
                accounts = accounts,
                credits = credits,
            )
        }
    }
}

internal fun numberOrNull(value: Any?): Double? = when (value) {
    is Number -> value.toDouble()
    is String -> value.toDoubleOrNull()
    else -> null
}

data class UsageReminder(val id: String, val title: String, val body: String, val fireAtMs: Long)

data class UsageSummary(
    val providers: List<UsageProviderMeter> = emptyList(),
    val claudeSpendUsd: Double? = null,
    val openAiSpendUsd: Double? = null,
    val atMs: Long = 0,
    val activeProvider: String = "",
) {
    val totalSpendUsd: Double?
        get() {
            if (claudeSpendUsd == null && openAiSpendUsd == null) return null
            return (claudeSpendUsd ?: 0.0) + (openAiSpendUsd ?: 0.0)
        }

    val isEmpty: Boolean get() = providers.isEmpty() && totalSpendUsd == null

    fun provider(id: String): UsageProviderMeter? = providers.firstOrNull { it.id == id }

    fun usedForNewAgents(providerId: String): Boolean = activeProvider.isNotEmpty() && activeProvider == providerId

    fun spendUsd(providerId: String): Double? = when (providerId) {
        "claude-code" -> claudeSpendUsd
        "codex" -> openAiSpendUsd
        else -> null
    }

    fun reminders(nowMs: Long): List<UsageReminder> {
        val out = ArrayList<UsageReminder>()
        for (provider in providers) {
            for (window in provider.windows) {
                if (window.state != UsageState.Exhausted) continue
                val reset = window.resetAtMs ?: continue
                if (reset <= nowMs) continue
                out += UsageReminder(
                    id = "usage.${provider.id}.${window.id}",
                    title = "${provider.label} is ready again",
                    body = "The ${window.label.lowercase()} usage limit has reset. Agents waiting on it can continue.",
                    fireAtMs = reset,
                )
            }
        }
        return out.sortedBy { it.fireAtMs }
    }

    companion object {
        fun from(json: Map<String, Any?>): UsageSummary {
            val spend = json.obj("spend")
            return UsageSummary(
                providers = json.maps("providers").map(UsageProviderMeter::from),
                claudeSpendUsd = numberOrNull(spend["claudeUsd"]),
                openAiSpendUsd = numberOrNull(spend["openAiUsd"]),
                atMs = json.long("atMs"),
                activeProvider = json.str("activeProvider"),
            )
        }
    }
}

object UsageFormat {
    fun remaining(ms: Long): String {
        val minutes = maxOf(1L, (ms + 59_999) / 60_000)
        if (minutes < 60) return "${minutes}m"
        val hours = minutes / 60
        if (hours < 48) {
            val rest = minutes % 60
            return if (rest == 0L) "${hours}h" else "${hours}h ${rest}m"
        }
        return "${hours / 24}d ${hours % 24}h"
    }

    fun usd(value: Double): String = String.format(Locale.US, "$%.2f", value)

    fun resetClock(resetAtMs: Long, nowMs: Long, zone: java.time.ZoneId = java.time.ZoneId.systemDefault()): String {
        if (resetAtMs <= 0 || resetAtMs <= nowMs) return ""
        val reset = java.time.Instant.ofEpochMilli(resetAtMs).atZone(zone)
        val today = java.time.Instant.ofEpochMilli(nowMs).atZone(zone).toLocalDate()
        val days = java.time.temporal.ChronoUnit.DAYS.between(today, reset.toLocalDate())
        val clock = pattern("h:mm a").format(reset)
        return when {
            days == 0L -> "Resets $clock"
            days < 7 -> "Resets ${pattern("EEE").format(reset)} $clock"
            else -> "Resets ${pattern("MMM d").format(reset)}, $clock"
        }
    }

    private fun pattern(text: String) = java.time.format.DateTimeFormatter.ofPattern(text, Locale.US)
}

data class CapabilityGrant(
    val id: String,
    val capability: String,
    val kind: ApprovalKind = ApprovalKind.Other,
    val resource: String = "",
    val decision: String = "",
    val lifetime: String = "",
    val worktree: String = "",
) {
    val allows: Boolean get() = decision == "allow"

    companion object {
        fun from(json: Map<String, Any?>): CapabilityGrant = CapabilityGrant(
            id = json.str("id"),
            capability = json.str("capability"),
            kind = ApprovalKind.of(json.str("kind")),
            resource = json.str("resource"),
            decision = json.str("decision"),
            lifetime = json.str("lifetime"),
            worktree = json.str("worktree"),
        )
    }
}

data class MergeVelocityWindow(
    val id: String,
    val label: String,
    val count: Int,
    val top: Int,
    val avg: Double,
    val series: List<Int> = emptyList(),
) {
    val avgText: String
        get() = if (avg >= 10.0 || avg == 0.0) Math.round(avg).toString() else String.format(Locale.US, "%.1f", avg)

    val detail: String get() = "top $record \u00B7 avg $avgText"

    val peak: Int get() = series.maxOrNull() ?: 0

    val record: Int get() = maxOf(count, top)

    val share: Float get() = if (record > 0) count.toFloat() / record else 0f

    val avgShare: Float get() = if (record > 0) minOf(avg.toFloat() / record, 1f) else 0f

    val spoken: String
        get() = "$label: ${if (count == 1) "1 merge" else "$count merges"}, best $record, average $avgText"
}

data class MergeStats(
    val base: String = "",
    val commit: String = "",
    val commits: Int? = null,
    val files: Int = 0,
    val insertions: Int = 0,
    val deletions: Int = 0,
    val velocity: List<MergeVelocityWindow> = emptyList(),
) {
    val shortCommit: String get() = commit.take(7)
    val hasLanding: Boolean get() = commit.isNotEmpty()
    val commitsText: String
        get() = when {
            commits == null || commits <= 0 -> ""
            commits == 1 -> "1 commit"
            else -> "$commits commits"
        }
    val filesText: String get() = if (files == 1) "1 file" else "$files files"
    val insertionsText: String get() = "+$insertions"
    val deletionsText: String get() = "\u2212$deletions"
    val insertionShare: Float?
        get() = (insertions + deletions).takeIf { it > 0 }?.let { insertions.toFloat() / it }

    val summary: String
        get() {
            if (!hasLanding) return ""
            val parts = mutableListOf<String>()
            if (commitsText.isNotEmpty()) parts += commitsText
            parts += "$filesText $insertionsText $deletionsText"
            return parts.joinToString(" \u00B7 ")
        }

    companion object {
        fun from(reply: Map<String, Any?>): MergeStats? {
            val stats = reply["stats"] as? Map<*, *> ?: return null
            val json = reply.obj("stats")
            return MergeStats(
                base = json.str("base"),
                commit = json.str("commit"),
                commits = if (stats.containsKey("commits")) json.int("commits") else null,
                files = json.int("files"),
                insertions = json.int("insertions"),
                deletions = json.int("deletions"),
                velocity = json.maps("velocity").map {
                    MergeVelocityWindow(
                        it.str("id"), it.str("label"), it.int("count"), it.int("top"), it.double("avg"),
                        it.list("series").map { bucket -> jsonLong(bucket).toInt() },
                    )
                },
            )
        }
    }
}

data class MergeOutcome(val branch: String, val stats: MergeStats? = null) {
    fun message(fallbackBranch: String = ""): String {
        val name = branch.ifEmpty { fallbackBranch }
        val head = if (name.isEmpty()) "Merged and cleaned up." else "Merged $name and cleaned up."
        val summary = stats?.summary.orEmpty()
        return if (summary.isEmpty()) head else "$head $summary."
    }

    companion object {
        fun from(reply: Map<String, Any?>): MergeOutcome = MergeOutcome(reply.str("branch"), MergeStats.from(reply))
    }
}

data class MergeLeaderboard(val repo: String = "", val branch: String = "", val stats: MergeStats? = null) {
    val subtitle: String
        get() = listOfNotNull(
            repo.ifEmpty { null },
            branch.ifEmpty { null }?.let { "newest merge: $it" },
            stats?.base?.ifEmpty { null }?.let { "into $it" },
        ).joinToString(" · ")

    companion object {
        fun from(reply: Map<String, Any?>): MergeLeaderboard =
            MergeLeaderboard(reply.str("repo"), reply.str("branch"), MergeStats.from(reply))
    }
}

data class AgentDiffFile(val path: String, val added: Int = 0, val removed: Int = 0, val binary: Boolean = false)

data class AgentDiff(
    val sessionId: Int,
    val branch: String,
    val base: String,
    val merged: Boolean,
    val files: List<AgentDiffFile>,
    val committed: String,
    val uncommitted: String,
    val untracked: List<String>,
    val hasWorktree: Boolean,
    val truncated: Boolean,
) {
    val added: Int get() = files.sumOf { it.added }
    val removed: Int get() = files.sumOf { it.removed }
    val isEmpty: Boolean get() = committed.isEmpty() && uncommitted.isEmpty() && untracked.isEmpty()

    enum class LineKind { Header, Hunk, Added, Removed, Context }

    data class Line(val index: Int, val kind: LineKind, val text: String)

    companion object {
        fun from(json: Map<String, Any?>): AgentDiff = AgentDiff(
            sessionId = json.int("id"),
            branch = json.str("branch"),
            base = json.str("base"),
            merged = json.bool("merged"),
            files = json.maps("files").map {
                AgentDiffFile(it.str("path"), it.int("added"), it.int("removed"), it.bool("binary"))
            },
            committed = json.str("committed"),
            uncommitted = json.str("uncommitted"),
            untracked = json.strings("untracked"),
            hasWorktree = json.bool("hasWorktree"),
            truncated = json.bool("truncated"),
        )

        fun lines(patch: String, limit: Int = 4000): List<Line> {
            val parts = patch.split("\n").let { if (it.isNotEmpty() && it.last().isEmpty()) it.dropLast(1) else it }
            return parts.take(limit).mapIndexed { index, text ->
                val kind = when {
                    text.startsWith("diff --git") || text.startsWith("index ") || text.startsWith("+++") || text.startsWith("---") ->
                        LineKind.Header
                    text.startsWith("@@") -> LineKind.Hunk
                    text.startsWith("+") -> LineKind.Added
                    text.startsWith("-") -> LineKind.Removed
                    else -> LineKind.Context
                }
                Line(index, kind, text)
            }
        }
    }
}
