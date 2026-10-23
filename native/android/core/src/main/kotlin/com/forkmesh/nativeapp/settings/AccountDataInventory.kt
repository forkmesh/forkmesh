package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.coves.CoveJson
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

object AccountDataPaths {
    const val INVENTORY = "/api/accounts/data"
    const val STORE = "/api/accounts/data/store"

    fun inventory(refresh: Boolean): String = if (refresh) "$INVENTORY?refresh=1" else INVENTORY

    fun rows(subject: String, table: String, after: Long): String {
        var path = "/api/accounts/data/rows?subject=${encode(subject)}&table=${encode(table)}"
        if (after > 0) path += "&after=$after"
        return path
    }

    fun row(table: String, rowid: Long): String = "/api/accounts/data/rows/${encode(table)}/$rowid"

    fun storeItem(key: String): String = STORE + "/" + encode(key)

    fun encode(value: String): String = buildString {
        for (byte in value.toByteArray(Charsets.UTF_8)) {
            val code = byte.toInt() and 0xFF
            val char = code.toChar()
            if (code < 0x80 && (char.isLetterOrDigit() || char == '-' || char == '.' || char == '_')) append(char) else append("%%%02X".format(code))
        }
    }
}

data class AccountDataSubject(
    val id: String,
    val kind: String,
    val label: String,
    val role: String,
    val readable: Boolean,
    val tables: Int,
    val rows: Int,
    val nodes: List<String>,
    val repositories: Int,
) {
    val title: String get() = if (kind == "organization") "Organization $label" else label

    val subtitle: String
        get() {
            val amount = if (rows == 1) "1 item" else "$rows items"
            val places = if (tables == 1) "1 place" else "$tables places"
            val parts = mutableListOf("$amount in $places")
            when (kind) {
                "account" -> if (nodes.isNotEmpty()) parts.add("with nodes " + nodes.joinToString(", "))
                "repositories" -> parts.add(if (repositories == 1) "1 repository" else "$repositories repositories")
                "organization" -> {
                    val article = when {
                        role.isEmpty() -> "a member"
                        role == "owner" || role == "admin" -> "an $role"
                        else -> "a $role"
                    }
                    parts.add("you are $article")
                    if (!readable) parts.add("owners and admins can open the contents")
                }
            }
            return parts.joinToString(" · ")
        }

    companion object {
        fun parse(map: Map<String, Any?>) = AccountDataSubject(
            id = map.text("id"),
            kind = map.text("kind"),
            label = map.text("label"),
            role = map.text("role"),
            readable = map["readable"] as? Boolean ?: true,
            tables = map.int("tables"),
            rows = map.int("rows"),
            nodes = map.strings("nodes"),
            repositories = map.int("repositories"),
        )
    }
}

data class AccountDataGroup(
    val subject: String,
    val table: String,
    val label: String,
    val category: String,
    val rows: Int,
    val truncated: Boolean,
    val readable: Boolean,
    val withheld: Boolean,
    val deletable: Boolean,
) {
    val id: String get() = "$subject/$table"

    val countText: String get() = if (truncated) "$rows+" else "$rows"

    val note: String
        get() = when {
            withheld -> "Security credentials: counted here, never shown or downloaded."
            !readable -> "Belongs to the organization. Its owners and admins can open it."
            deletable -> "You can remove these one at a time."
            else -> ""
        }

    companion object {
        fun parse(map: Map<String, Any?>) = AccountDataGroup(
            subject = map.text("subject"),
            table = map.text("table"),
            label = map.text("label").ifEmpty { map.text("table") },
            category = map.text("category").ifEmpty { "Other" },
            rows = map.int("rows"),
            truncated = map.flag("truncated"),
            readable = map["readable"] as? Boolean ?: true,
            withheld = map.flag("withheld"),
            deletable = map.flag("deletable"),
        )
    }
}

data class AccountStoreItem(
    val key: String,
    val kind: String,
    val label: String,
    val device: String,
    val bytes: Int,
    val revision: Int,
    val updatedAt: Long,
    val readable: Boolean,
) {
    val title: String get() = label.ifEmpty { key }

    companion object {
        fun parse(map: Map<String, Any?>) = AccountStoreItem(
            key = map.text("key"),
            kind = map.text("kind"),
            label = map.text("label"),
            device = map.text("device"),
            bytes = map.int("bytes"),
            revision = map.int("revision", 1),
            updatedAt = map.long("updatedAt"),
            readable = map["readable"] as? Boolean ?: true,
        )
    }
}

data class AccountDataSection(val category: String, val groups: List<AccountDataGroup>)

data class AccountDataInventory(
    val subjects: List<AccountDataSubject> = emptyList(),
    val groups: List<AccountDataGroup> = emptyList(),
    val store: List<AccountStoreItem> = emptyList(),
    val generatedAt: Long = 0,
) {
    val totalRows: Int get() = groups.sumOf { it.rows } + store.size

    fun groups(subject: String): List<AccountDataSection> {
        val buckets = LinkedHashMap<String, MutableList<AccountDataGroup>>()
        for (group in groups) if (group.subject == subject) buckets.getOrPut(group.category) { ArrayList() }.add(group)
        val order = buckets.keys.sortedWith { left, right ->
            when {
                left == right -> 0
                left == "Account" -> -1
                right == "Account" -> 1
                left == "Other" -> 1
                right == "Other" -> -1
                else -> left.compareTo(right)
            }
        }
        return order.map { category -> AccountDataSection(category, buckets[category].orEmpty().sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.label })) }
    }

    fun removing(group: String, removed: Int): AccountDataInventory = copy(
        groups = groups.mapNotNull { item ->
            if (item.id != group) return@mapNotNull item
            val left = maxOf(0, item.rows - removed)
            if (left > 0 || item.truncated) item.copy(rows = left) else null
        },
    )

    fun removingStore(key: String?): AccountDataInventory = copy(store = if (key == null) emptyList() else store.filter { it.key != key })

    companion object {
        fun parse(map: Map<String, Any?>) = AccountDataInventory(
            subjects = map.maps("subjects").map(AccountDataSubject::parse),
            groups = map.maps("groups").map(AccountDataGroup::parse),
            store = map.maps("store").map(AccountStoreItem::parse),
            generatedAt = map.long("generatedAt"),
        )
    }
}

data class AccountDataField(val name: String, val value: Any?) {
    val label: String
        get() {
            val words = name.replace('_', ' ')
            return words.take(1).uppercase() + words.drop(1)
        }

    val isStructured: Boolean get() = value is Map<*, *> || value is List<*>

    val text: String
        get() = when (val raw = value) {
            null -> "empty"
            is Boolean -> if (raw) "yes" else "no"
            is Long, is Int -> {
                val number = (raw as Number).toLong()
                if (looksLikeTime(name) && number > 1_000_000_000_000 && number < 10_000_000_000_000) date(number) else number.toString()
            }
            is Number -> {
                val number = raw.toDouble()
                if (number == Math.floor(number) && !number.isInfinite() && kotlin.math.abs(number) < 1e15) {
                    val whole = number.toLong()
                    if (looksLikeTime(name) && whole > 1_000_000_000_000 && whole < 10_000_000_000_000) date(whole) else whole.toString()
                } else {
                    number.toString()
                }
            }
            is String -> raw.ifEmpty { "empty" }
            else -> CoveJson.pretty(raw, indent = "  ")
        }

    companion object {
        private val DATE = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm 'UTC'").withZone(ZoneOffset.UTC)

        fun looksLikeTime(name: String): Boolean =
            name.endsWith("_at") || name.endsWith("At") || name == "ts" || name.endsWith("_ts") || name == "last_seen" || name.endsWith("_ms")

        fun date(millis: Long): String = DATE.format(Instant.ofEpochMilli(millis))
    }
}

data class AccountDataRow(val id: String, val rowid: Long, val deletable: Boolean, val fields: List<AccountDataField>) {
    val title: String
        get() {
            for (name in TITLE_FIELDS) {
                val text = fields.firstOrNull { it.name == name }?.value as? String ?: continue
                if (text.isNotEmpty() && !text.startsWith("(")) return text
            }
            for (field in fields) {
                val inner = field.value as? Map<*, *> ?: continue
                for (name in TITLE_FIELDS) {
                    val text = inner[name] as? String ?: continue
                    if (text.isNotEmpty() && !text.startsWith("(")) return text
                }
            }
            fields.firstOrNull { AccountDataField.looksLikeTime(it.name) && ((it.value as? Number)?.toLong() ?: 0) > 0 }?.let { return it.text }
            return if (rowid > 0) "Item $rowid" else "Item"
        }

    val subtitle: String
        get() {
            val heading = title
            return fields.filter { !it.isStructured && it.text != "empty" && !it.text.startsWith("(") && it.text != heading }
                .take(2).joinToString(" · ") { "${it.label}: ${it.text}" }
        }

    companion object {
        val TITLE_FIELDS = listOf("label", "title", "name", "device_label", "username", "kind", "package_id", "repo", "account_name", "key_type", "fingerprint")

        fun parse(map: Map<String, Any?>, index: Int): AccountDataRow {
            val rowid = map.long("rowid")
            val raw = map.obj("fields")
            return AccountDataRow(
                id = if (rowid > 0) rowid.toString() else "i$index",
                rowid = rowid,
                deletable = map.flag("deletable") && rowid > 0,
                fields = raw.keys.map { AccountDataField(it, raw[it]) },
            )
        }
    }
}

data class AccountDataPage(val rows: List<AccountDataRow> = emptyList(), val next: Long = 0, val readable: Boolean = true, val reason: String = "") {
    fun appending(more: AccountDataPage) = AccountDataPage(rows + more.rows, more.next, more.readable, more.reason)

    fun removing(rowid: Long) = copy(rows = rows.filter { it.rowid != rowid })

    val reasonText: String
        get() = when (reason) {
            "organization_managers_only" -> "This belongs to the organization. Ask one of its owners or admins for a copy."
            "security_credential" -> "These are security credentials, such as keys and sign-in secrets. ForkMesh counts them here but never shows or downloads them."
            else -> ""
        }

    companion object {
        fun parse(map: Map<String, Any?>, offset: Int = 0): AccountDataPage = AccountDataPage(
            rows = map.maps("rows").mapIndexed { index, row -> AccountDataRow.parse(row, offset + index) },
            next = map.long("next"),
            readable = map["readable"] as? Boolean ?: true,
            reason = map.text("reason"),
        )
    }
}

class AccountDataBrowser(private val client: AccountEndpointCalling) {
    @Volatile var inventory: AccountDataInventory? = null
        private set
    @Volatile var pages: Map<String, AccountDataPage> = emptyMap()
        private set
    @Volatile var loading = false
        private set
    @Volatile var loadingGroup = ""
        private set
    @Volatile var error = ""
        private set
    @Volatile var rowsError = ""
        private set

    fun load(refresh: Boolean = false) {
        loading = true
        try {
            inventory = AccountDataInventory.parse(client.accountCall("GET", AccountDataPaths.inventory(refresh), null))
            if (refresh) pages = emptyMap()
            error = ""
        } catch (failure: AccountEndpointException) {
            error = if (failure.status == 404 || failure.status == 405) "This server cannot list your data yet." else AccountErrorText.text(failure, "Could not load your data.")
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not load your data.")
        } finally {
            loading = false
        }
    }

    fun page(group: AccountDataGroup): AccountDataPage? = pages[group.id]

    fun loadRows(group: AccountDataGroup, more: Boolean = false) {
        val current = pages[group.id]
        if (more && (current?.next ?: 0L) == 0L) return
        val after = if (more) current?.next ?: 0L else 0L
        loadingGroup = group.id
        try {
            val map = client.accountCall("GET", AccountDataPaths.rows(group.subject, group.table, after), null)
            val page = AccountDataPage.parse(map, if (more) current?.rows?.size ?: 0 else 0)
            pages = pages + (group.id to if (more) (current ?: AccountDataPage()).appending(page) else page)
            rowsError = ""
        } catch (failure: Exception) {
            rowsError = AccountErrorText.text(failure, "Could not load these items.")
        } finally {
            loadingGroup = ""
        }
    }

    fun delete(row: AccountDataRow, group: AccountDataGroup): Boolean {
        if (!row.deletable) return false
        return try {
            client.accountCall("DELETE", AccountDataPaths.row(group.table, row.rowid), null)
            pages[group.id]?.let { pages = pages + (group.id to it.removing(row.rowid)) }
            inventory = inventory?.removing(group.id, 1)
            rowsError = ""
            true
        } catch (failure: Exception) {
            rowsError = AccountErrorText.text(failure, "Could not remove that item.")
            false
        }
    }

    fun deleteStore(key: String?): Boolean = try {
        client.accountCall("DELETE", key?.let(AccountDataPaths::storeItem) ?: AccountDataPaths.STORE, null)
        inventory = inventory?.removingStore(key)
        error = ""
        true
    } catch (failure: Exception) {
        error = AccountErrorText.text(failure, "Could not remove the saved data.")
        false
    }
}
