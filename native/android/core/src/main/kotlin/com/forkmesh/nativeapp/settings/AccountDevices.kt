package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.TrustLists

data class RecentAccount(val name: String, val email: String, val lastUsedAt: Long) {
    val id: String get() = name.lowercase()
    val subtitle: String get() = email.ifEmpty { "@$name" }

    internal fun json(): Map<String, Any?> = linkedMapOf("name" to name, "email" to email, "lastUsedAt" to lastUsedAt)

    companion object {
        internal fun parse(map: Map<String, Any?>): RecentAccount? {
            val name = map.text("name").trim()
            if (name.isEmpty() || name.length > 63) return null
            return RecentAccount(name, map.text("email").take(254), map.long("lastUsedAt"))
        }
    }
}

class RecentAccountsStore(private val prefs: PrefStore) {
    val accounts: List<RecentAccount>
        get() {
            val raw = prefs.getString(KEY).orEmpty()
            if (raw.isEmpty()) return emptyList()
            val list = try {
                Json.parse(raw) as? List<*>
            } catch (_: Exception) {
                null
            } ?: return emptyList()
            val seen = HashSet<String>()
            @Suppress("UNCHECKED_CAST")
            return list.mapNotNull { (it as? Map<String, Any?>)?.let(RecentAccount::parse) }
                .filter { seen.add(it.id) }
                .sortedByDescending { it.lastUsedAt }
                .take(LIMIT)
        }

    fun others(current: String): List<RecentAccount> = accounts.filter { it.id != current.lowercase() }

    fun record(name: String, email: String, atMillis: Long) {
        val trimmed = name.trim()
        if (trimmed.isEmpty() || trimmed == "preview-node") return
        val list = accounts.filter { it.id != trimmed.lowercase() }.toMutableList()
        list.add(0, RecentAccount(trimmed, email, atMillis))
        save(list.take(LIMIT))
    }

    fun forget(name: String) {
        save(accounts.filter { it.id != name.lowercase() })
    }

    private fun save(list: List<RecentAccount>) {
        if (list.isEmpty()) prefs.remove(KEY) else prefs.putString(KEY, Json.stringify(list.map { it.json() }))
    }

    companion object {
        const val KEY = "account/recentAccounts"
        const val LIMIT = 8
    }
}

class DesktopNicknames(private val prefs: PrefStore) {
    val all: Map<String, String>
        get() {
            val raw = prefs.getString(KEY).orEmpty()
            val map = try {
                Json.parse(raw) as? Map<*, *>
            } catch (_: Exception) {
                null
            } ?: return emptyMap()
            val out = LinkedHashMap<String, String>()
            for ((key, value) in map) {
                val k = key?.toString().orEmpty()
                val text = value as? String ?: continue
                if (k.isNotEmpty() && text.isNotEmpty()) out[k] = text
            }
            return out
        }

    fun name(key: String, fallback: String): String {
        all[key]?.takeIf { it.isNotEmpty() }?.let { return it }
        if (fallback.isNotEmpty()) return fallback
        return if (key.length <= 12) key else "Desktop ${key.take(6)}"
    }

    fun rename(key: String, nickname: String) {
        if (key.isEmpty()) return
        val map = all.toMutableMap()
        val cleaned = nickname.trim().filter { it != '\n' && it != '\r' }.take(MAX_LENGTH)
        if (cleaned.isEmpty()) map.remove(key) else map[key] = cleaned
        save(map)
    }

    fun forget(key: String) {
        save(all - key)
    }

    fun prune(keeping: Set<String>) {
        val current = all
        val kept = current.filterKeys { it in keeping }
        if (kept.size != current.size) save(kept)
    }

    private fun save(map: Map<String, String>) {
        val trimmed = map.toSortedMap().entries.take(LIMIT).associate { it.key to it.value }
        if (trimmed.isEmpty()) prefs.remove(KEY) else prefs.putString(KEY, Json.stringify(trimmed.toSortedMap()))
    }

    companion object {
        const val KEY = "account/desktopNicknames"
        const val MAX_LENGTH = 40
        const val LIMIT = 32
    }
}

data class TrustListSummary(
    val kind: String,
    val issuer: String,
    val name: String,
    val communities: List<String>,
    val roles: List<String>,
    val version: Int,
    val entries: Int,
    val issuedAt: Long,
    val expiresAt: Long,
    val documentId: String,
) {
    companion object {
        fun summarize(raw: String): Result<TrustListSummary> {
            if (raw.toByteArray().size > 256 * 1024) return Result.failure(IllegalArgumentException("document_too_large"))
            val document = try {
                TrustLists.verifyDocument(raw)
            } catch (failure: Exception) {
                return Result.failure(failure)
            }
            val kind = when (val schema = document.text("schema")) {
                TrustLists.SCHEMA -> "Trust list"
                TrustLists.REVOCATION_SCHEMA -> "Revocation"
                TrustLists.APPEAL_SCHEMA -> "Appeal"
                else -> schema
            }
            return Result.success(
                TrustListSummary(
                    kind = kind,
                    issuer = document.text("issuer"),
                    name = document.text("name"),
                    communities = document.strings("communities"),
                    roles = document.strings("roles"),
                    version = document.int("version", document.int("fromVersion")),
                    entries = document.list("entries").size,
                    issuedAt = document.long("issuedAt"),
                    expiresAt = document.long("expiresAt"),
                    documentId = TrustLists.documentId(raw),
                ),
            )
        }
    }
}

data class DesktopStatusSummary(
    val name: String,
    val user: String,
    val version: String,
    val running: Int,
    val transport: String,
    val providers: List<String>,
    val defaultProvider: String,
) {
    val line: String
        get() {
            val parts = ArrayList<String>()
            if (version.isNotEmpty()) parts.add("ForkMesh $version")
            parts.add(if (running == 1) "1 agent running" else "$running agents running")
            if (transport == "relay") parts.add("through the relay") else if (transport == "lan") parts.add("on this network")
            return parts.joinToString(" · ")
        }

    val agentsLine: String
        get() {
            if (providers.isEmpty()) return "No agent providers set up on this desktop."
            val list = providers.joinToString(", ")
            return if (defaultProvider.isEmpty()) list else "$list. New agents use $defaultProvider."
        }

    companion object {
        fun parse(status: Map<String, Any?>, agents: Map<String, Any?> = emptyMap()): DesktopStatusSummary {
            val providers = agents.maps("providers").mapNotNull { map ->
                val label = map.text("label").ifEmpty { map.text("id") }
                if (label.isEmpty()) null else label.take(40)
            }
            val defaultId = agents.text("defaultProvider")
            val defaultLabel = agents.maps("providers").firstOrNull { it.text("id") == defaultId }?.text("label") ?: defaultId
            return DesktopStatusSummary(
                name = status.text("name").take(63),
                user = status.text("user").take(63),
                version = status.text("version").take(32),
                running = maxOf(0, status.int("running")),
                transport = status.text("transport"),
                providers = providers.take(16),
                defaultProvider = defaultLabel.take(40),
            )
        }
    }
}
