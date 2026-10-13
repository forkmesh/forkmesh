package com.forkmesh.nativeapp.admin

import com.forkmesh.nativeapp.insights.InsightsException
import com.forkmesh.nativeapp.insights.InsightsQuery
import com.forkmesh.nativeapp.insights.InsightsRequesting
import com.forkmesh.nativeapp.insights.StatsFormat
import com.forkmesh.nativeapp.insights.StatsTone
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

enum class AdminSection(val wire: String, val title: String, val icon: String) {
    Overview("overview", "Overview", "speed"),
    Users("users", "Users", "group_outlined"),
    Errors("errors", "Errors", "report_outlined"),
    Database("database", "Database", "storage"),
    Usage("usage", "Usage & jobs", "schedule"),
    Repos("repos", "Repository purge", "delete_outline"),
    ;

    companion object {
        fun of(route: String?): AdminSection = entries.firstOrNull { it.wire == route?.lowercase() } ?: Overview
    }
}

private fun Map<String, Any?>.optInt(key: String): Int? = (this[key] as? Number)?.toInt()

data class AdminOverview(
    val actor: String,
    val errors24h: Int?,
    val pendingVerifications: Int?,
    val relaysAwaitingApproval: Int?,
    val users: Int?,
    val quarantinedUsers: Int?,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>) = AdminOverview(
            actor = json.str("actor"),
            errors24h = json.optInt("errors24h"),
            pendingVerifications = json.optInt("pendingVerifications"),
            relaysAwaitingApproval = json.optInt("relaysAwaitingApproval"),
            users = json.optInt("users"),
            quarantinedUsers = json.optInt("quarantinedUsers"),
        )
    }
}

data class ErrorUser(val name: String, val count: Int)

data class ErrorGroup(
    val status: String,
    val method: String,
    val path: String,
    val message: String,
    val source: String,
    val count: Int,
    val firstSeen: Long,
    val lastSeen: Long,
    val hours: List<Int>,
    val users: List<ErrorUser>,
    val userCount: Int,
    val anonymous: Int,
) {
    val id: String get() = "$status $method $path $message"

    val sourceLabel: String
        get() = when (source) {
            "desktop" -> "Desktop"
            "javascript" -> "Browser"
            else -> "Server"
        }

    val title: String get() = message.ifEmpty { "HTTP $status" }

    val requestLine: String get() = listOf(method, path).filter { it.isNotEmpty() }.joinToString(" ")

    val affected: String
        get() {
            val parts = ArrayList<String>()
            if (userCount > 0) parts.add("$userCount user${if (userCount == 1) "" else "s"}")
            if (anonymous > 0) parts.add("$anonymous anonymous")
            return parts.joinToString(" · ")
        }

    fun agentPrompt(host: String): String {
        val lines = mutableListOf(
            "Investigate this server error group from ${host.ifEmpty { "ForkMesh" }} and fix its cause.",
            "",
            "Status: $status",
            "Request: $requestLine",
            "Source: $sourceLabel",
            "Occurrences in the last 24 hours: $count",
            "Message:",
            message,
        )
        if (userCount > 0) lines.add("Accounts affected: $userCount")
        return lines.joinToString("\n")
    }

    companion object {
        fun fromJson(json: Map<String, Any?>) = ErrorGroup(
            status = json.str("status"),
            method = json.str("method"),
            path = json.str("path"),
            message = json.str("message"),
            source = json.str("source"),
            count = json.int("count"),
            firstSeen = json.long("firstSeen"),
            lastSeen = json.long("lastSeen"),
            hours = json.list("hours").map { jsonLong(it).toInt() },
            users = json.maps("users").map { ErrorUser(it.str("name"), it.int("count")) },
            userCount = json.int("userCount"),
            anonymous = json.int("anonymous"),
        )
    }
}

data class ErrorReport(
    val total: Int,
    val hours: List<Int>,
    val groupCount: Int,
    val usersHit: Int,
    val truncated: Boolean,
    val groups: List<ErrorGroup>,
) {
    fun filtered(source: String?): List<ErrorGroup> = if (source.isNullOrEmpty()) groups else groups.filter { it.source == source }

    companion object {
        fun fromJson(json: Map<String, Any?>) = ErrorReport(
            total = json.int("total"),
            hours = json.list("hours").map { jsonLong(it).toInt() },
            groupCount = json.int("groupCount"),
            usersHit = json.int("usersHit"),
            truncated = json.bool("truncated"),
            groups = json.maps("groups").map(ErrorGroup::fromJson),
        )
    }
}

data class PendingVerification(val name: String, val joinedAt: Long)

data class AdminRelay(val label: String, val baseUrl: String, val status: String, val registeredAt: Long, val approvedAt: Long)

data class DatabaseColumn(val name: String, val type: String)

data class DatabaseTable(
    val name: String,
    val rows: Int,
    val restricted: Boolean,
    val storage: String,
    val plaintextData: Boolean,
    val gaps: List<String>,
    val auditCategory: String,
    val auditNote: String,
    val columns: List<DatabaseColumn>,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>) = DatabaseTable(
            name = json.str("name"),
            rows = json.int("rows"),
            restricted = json.bool("restricted"),
            storage = json.str("storage"),
            plaintextData = json.bool("plaintextData"),
            gaps = json.strings("gaps"),
            auditCategory = json.str("auditCategory"),
            auditNote = json.str("auditNote"),
            columns = json.maps("columns").map { DatabaseColumn(it.str("name"), it.str("type")) },
        )
    }
}

data class DatabaseInventory(val sampledAt: Long, val tables: List<DatabaseTable>) {
    val totalRows: Int get() = tables.sumOf { it.rows }

    fun filtered(query: String): List<DatabaseTable> {
        val needle = query.trim().lowercase()
        val sorted = tables.sortedWith(compareByDescending<DatabaseTable> { it.rows }.thenBy { it.name })
        return if (needle.isEmpty()) sorted else sorted.filter { it.name.lowercase().contains(needle) }
    }

    companion object {
        fun fromJson(json: Map<String, Any?>) = DatabaseInventory(json.long("sampledAt"), json.maps("tables").map(DatabaseTable::fromJson))
    }
}

data class DatabaseCell(val column: String, val value: String)

data class DatabaseRow(
    val index: Int,
    val rowid: String,
    val digest: String,
    val cells: List<DatabaseCell>,
    val dataMode: String,
    val data: String,
    val walletKeys: Boolean,
) {
    val id: String get() = rowid.ifEmpty { "$index:$digest" }

    val summary: String get() = cells.take(3).joinToString(" · ") { "${it.column}: ${it.value}" }

    companion object {
        const val CELL_LIMIT = 400

        fun fromJson(index: Int, json: Map<String, Any?>, columns: List<String>): DatabaseRow {
            val values = json.obj("values")
            val cells = columns.map { column ->
                val value = values[column]
                val text = if (value != null) {
                    (if (value is String) value else Json.stringify(value)).take(CELL_LIMIT)
                } else if (column == "data" && json.containsKey("data")) {
                    "(decoded below)"
                } else {
                    "NULL"
                }
                DatabaseCell(column, text)
            }
            val data = json["data"]
            return DatabaseRow(
                index = index,
                rowid = json["rowid"]?.let { if (it is String) it else Json.stringify(it) }.orEmpty(),
                digest = json.str("digest"),
                cells = cells,
                dataMode = json.str("dataMode"),
                data = if (data != null) Json.stringify(data).take(4000) else "",
                walletKeys = json.bool("walletKeys"),
            )
        }
    }
}

data class DatabasePage(val table: String, val columns: List<String>, val rows: List<DatabaseRow>, val nextCursor: String) {
    companion object {
        fun fromJson(json: Map<String, Any?>): DatabasePage {
            val names = json.maps("columns").map { it.str("name") }
            return DatabasePage(
                table = json.str("table"),
                columns = names,
                rows = json.maps("rows").mapIndexed { index, row -> DatabaseRow.fromJson(index, row, names) },
                nextCursor = json.str("nextCursor"),
            )
        }
    }
}

data class CronSummary(
    val ticks: Int,
    val failedTicks: Int,
    val lastTickAt: Long,
    val gapMs: Long,
    val recent: List<Tick>,
    val unavailable: String,
) {
    data class Tick(val ts: Long, val ms: Int, val failures: Int, val ran: List<String>, val failed: List<String>)

    val health: StatsTone
        get() = when {
            unavailable.isNotEmpty() -> StatsTone.Warning
            lastTickAt == 0L -> StatsTone.None
            gapMs > 10 * 60 * 1000 -> StatsTone.Critical
            failedTicks > 0 -> StatsTone.Warning
            else -> StatsTone.Good
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): CronSummary {
            val summary = json.obj("summary")
            return CronSummary(
                ticks = summary.int("ticks"),
                failedTicks = summary.int("failedTicks"),
                lastTickAt = summary.long("lastTickAt"),
                gapMs = summary.long("gapMs"),
                recent = json.maps("ticks").take(30).map {
                    Tick(it.long("ts"), it.int("ms"), it.int("failures"), it.strings("ran"), it.strings("failed"))
                },
                unavailable = if (json.bool("ok") || !json.containsKey("ok")) "" else json.str("error"),
            )
        }
    }
}

data class StorageUsage(val entries: List<Entry>) {
    data class Entry(val label: String, val value: String)

    companion object {
        fun fromJson(json: Map<String, Any?>): StorageUsage {
            val storage = json.obj("storage")
            return StorageUsage(
                storage.keys.sorted().mapNotNull { key ->
                    when (val value = storage[key]) {
                        is Number -> {
                            val lower = key.lowercase()
                            val formatted = if (lower.contains("byte") || lower.endsWith("size")) {
                                StatsFormat.compactBytes(value.toLong())
                            } else {
                                StatsFormat.compactNumber(value.toDouble())
                            }
                            Entry(key, formatted)
                        }
                        is String -> Entry(key, value)
                        else -> null
                    }
                },
            )
        }
    }
}

data class AdminUser(
    val name: String,
    val email: String,
    val emailVerified: Boolean,
    val isAdmin: Boolean,
    val createdAt: Long,
    val status: String,
    val profilePrivate: Boolean,
    val referralStatus: String,
    val signupCountry: String,
    val nodes: Int,
    val quarantined: Boolean,
) {
    val badges: List<String>
        get() {
            val out = ArrayList<String>()
            if (isAdmin) out.add("Admin")
            if (quarantined) out.add("Quarantined")
            if (!emailVerified) out.add("Unverified")
            if (profilePrivate) out.add("Private")
            return out
        }

    companion object {
        fun fromJson(json: Map<String, Any?>, quarantined: Boolean = false): AdminUser {
            val created = json.long("createdAt")
            return AdminUser(
                name = json.str("name"),
                email = json.str("email"),
                emailVerified = json.bool("emailVerified"),
                isAdmin = json.bool("isAdmin"),
                createdAt = if (created in 1 until 100_000_000_000L) created * 1000 else created,
                status = json.str("accountStatus").ifEmpty { json.str("status") },
                profilePrivate = json.bool("profilePrivate"),
                referralStatus = json.str("referralStatus"),
                signupCountry = json.str("signupCountry"),
                nodes = json.list("nodes").size,
                quarantined = quarantined,
            )
        }
    }
}

data class AdminUsers(val users: List<AdminUser>, val quarantined: List<AdminUser>) {
    val hasAdminFields: Boolean get() = users.any { it.isAdmin } || quarantined.isNotEmpty()

    fun filtered(query: String, showQuarantined: Boolean): List<AdminUser> {
        val pool = if (showQuarantined) quarantined else users
        val needle = query.trim().lowercase()
        val sorted = pool.sortedWith(compareByDescending<AdminUser> { it.createdAt }.thenBy { it.name })
        if (needle.isEmpty()) return sorted
        return sorted.filter { it.name.lowercase().contains(needle) || it.email.lowercase().contains(needle) }
    }

    companion object {
        fun fromJson(json: Map<String, Any?>) = AdminUsers(
            users = json.maps("users").map { AdminUser.fromJson(it) }.filter { it.name.isNotEmpty() },
            quarantined = json.maps("quarantinedUsers").map { AdminUser.fromJson(it, quarantined = true) }.filter { it.name.isNotEmpty() },
        )
    }
}

enum class AdminUserAction(val wire: String, val path: String, val title: String, val pastTense: String) {
    Quarantine("quarantine", "/api/accounts/admin-quarantine", "Quarantine", "Quarantined"),
    Unquarantine("unquarantine", "/api/accounts/admin-unquarantine", "Restore", "Restored"),
    Delete("delete", "/api/accounts/admin-delete", "Delete", "Deleted"),
    ;

    val needsDeviceAuth: Boolean get() = this == Delete || this == Quarantine
}

class AdminClient(private val transport: InsightsRequesting) {
    private fun get(section: String, query: List<Pair<String, String>> = emptyList()): Map<String, Any?> =
        transport.call("GET", InsightsQuery.path("/api/admin/phone/$section", query), null)

    fun overview(): AdminOverview = AdminOverview.fromJson(get("overview"))

    fun errors(): ErrorReport = ErrorReport.fromJson(get("errors"))

    fun cron(): CronSummary = CronSummary.fromJson(get("cron"))

    fun storage(): StorageUsage = StorageUsage.fromJson(get("storage"))

    fun pending(): List<PendingVerification> = get("pending").maps("pending").map { PendingVerification(it.str("name"), it.long("joinedAt")) }

    fun relays(): List<AdminRelay> = get("relays").maps("relays").map {
        AdminRelay(it.str("label"), it.str("baseUrl"), it.str("status"), it.long("registeredAt"), it.long("approvedAt"))
    }

    fun inventory(refresh: Boolean = false): DatabaseInventory =
        DatabaseInventory.fromJson(get("database", listOf("op" to "inventory", "refresh" to if (refresh) "1" else "")))

    fun rows(table: String, cursor: String = "", limit: Int = 25): DatabasePage = DatabasePage.fromJson(
        get("database", listOf("op" to "rows", "table" to table, "cursor" to cursor, "limit" to limit.coerceIn(1, 50).toString())),
    )

    fun users(): AdminUsers = AdminUsers.fromJson(transport.call("GET", "/api/accounts/users", null))

    fun perform(action: AdminUserAction, target: String) {
        val name = target.trim().lowercase()
        if (!validName(name)) throw InsightsException("invalid_user")
        transport.call("POST", action.path, mapOf("target" to name))
    }

    fun perform(action: AdminUserAction, targets: List<String>): Map<String, InsightsException> {
        val failures = LinkedHashMap<String, InsightsException>()
        for (target in targets.take(MAX_BATCH)) {
            try {
                perform(action, target)
            } catch (error: Exception) {
                failures[target] = InsightsException.from(error)
            }
        }
        return failures
    }

    fun purgeRepository(owner: String, name: String) {
        val cleanOwner = owner.trim()
        val cleanName = name.trim()
        if (!validSegment(cleanOwner) || !validSegment(cleanName)) throw InsightsException("invalid_repository")
        transport.call("DELETE", InsightsQuery.path("/api/repositories", listOf("owner" to cleanOwner, "name" to cleanName)), null)
    }

    companion object {
        const val MAX_BATCH = 25

        private fun safe(c: Char) = c in 'a'..'z' || c in 'A'..'Z' || c in '0'..'9' || c == '-' || c == '_' || c == '.'

        fun validName(name: String): Boolean = name.isNotEmpty() && name.length <= 63 && name.all(::safe)

        fun validSegment(segment: String): Boolean =
            segment.isNotEmpty() && segment.length <= 100 && segment != "." && segment != ".." && segment.all(::safe)

        fun purgeConfirmation(owner: String, name: String): String = "$owner/$name"
    }
}
