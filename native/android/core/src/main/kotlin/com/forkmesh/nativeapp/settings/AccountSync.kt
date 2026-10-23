package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.json.Json
import java.security.MessageDigest

interface AccountSyncSource {
    val key: String
    val kind: String
    val label: String

    fun snapshot(): Map<String, Any?>?

    fun restore(value: Map<String, Any?>): Int
}

interface AccountSyncPrefs {
    fun keys(): Set<String>

    fun raw(key: String): Any?

    fun putRaw(key: String, value: Any?): Boolean
}

class MemoryAccountSyncPrefs(seed: Map<String, Any> = emptyMap()) : AccountSyncPrefs {
    private val values = LinkedHashMap<String, Any>(seed)

    @Synchronized
    override fun keys(): Set<String> = values.keys.toSet()

    @Synchronized
    override fun raw(key: String): Any? = values[key]

    @Synchronized
    override fun putRaw(key: String, value: Any?): Boolean {
        if (value == null) values.remove(key) else values[key] = value
        return true
    }
}

object AccountSyncKeys {
    const val ENABLED = "accountSync/enabled"
    const val SAVED_AT = "accountSync/savedAt"
    const val PREFIX = "accountSync/"
    const val MAX_VALUE_CHARS = 32 * 1024
    const val HOME = "android.home"
    const val RSS = "android.rss"
    const val SETTINGS = "android.settings"

    val homePrefixes = listOf("shell/", "insights/stats/layout", "insights/stats/desktop-tiles")

    val skippedPrefixes = listOf(
        "p/", "auth/", "identity/", "import/", "ownerCrypto", "owner/", "server/", "relays/", "dataFolder/", "install/",
        "accountSync/", "chat/e2ee/", "chat/history", "chat/welcomeAnnounced/", "chat/local-mesh/", "chat/people-pictures",
        "chat/lastConversation", "chat/blockedAccountsUnsent", "agent/", "agents/readBaselineMs", "agents/seenFinishedAt",
        "agents/next-task", "inbox/", "pings/", "push/", "insights/stats/history", "insights/stats/offered",
        "packages/account/", "packages/projectInstalls", "packages/versions", "packages/legacyProject",
        "packages/organization/", "nodeSync/", "node/", "localControl/", "shell/bar-recent", "shell/bar-uses",
        "shell/repoCounts/", "shell/pulls-seen", "shell/auto_send_", "code/current-repo", "codeInsight/", "diffViewed/",
        "account/recentAccounts", "vault/", "fresh/", "tour/", "nav/", "timers/", "genie/outreach_", "genie/spot_",
        "genie/parked", "appearance/instanceThemeColors", "security-scans/", "unified/",
    )

    fun skipped(key: String): Boolean = skippedPrefixes.any { key.startsWith(it) } || DataFolder.isDeviceKey(key)

    fun isHome(key: String): Boolean = homePrefixes.any { key.startsWith(it) }

    fun hash(account: String, source: String): String = PREFIX + "hash/" + account.lowercase() + "/" + source
}

class PreferenceSyncSource(
    override val key: String,
    override val kind: String,
    override val label: String,
    private val prefs: AccountSyncPrefs,
    private val includes: (String) -> Boolean,
) : AccountSyncSource {
    override fun snapshot(): Map<String, Any?>? {
        val values = LinkedHashMap<String, Any?>()
        for (name in prefs.keys()) {
            if (!includes(name)) continue
            val value = prefs.raw(name)
            if (value is String && value.length > AccountSyncKeys.MAX_VALUE_CHARS) continue
            values[name] = value
        }
        if (values.isEmpty()) return null
        val document = DataFolder.exportSettings(values)
        if ((document["values"] as? Map<*, *>).isNullOrEmpty()) return null
        return document
    }

    override fun restore(value: Map<String, Any?>): Int {
        var applied = 0
        for ((name, item) in DataFolder.decodeSettings(value)) {
            if (!includes(name)) continue
            if (prefs.raw(name) == item) continue
            if (prefs.putRaw(name, item)) applied++
        }
        return applied
    }

    companion object {
        fun home(prefs: AccountSyncPrefs) = PreferenceSyncSource(AccountSyncKeys.HOME, "layout", "Home screen and bar layout", prefs) {
            AccountSyncKeys.isHome(it) && !AccountSyncKeys.skipped(it)
        }

        fun settings(prefs: AccountSyncPrefs) = PreferenceSyncSource(AccountSyncKeys.SETTINGS, "settings", "App settings", prefs) {
            !AccountSyncKeys.isHome(it) && !AccountSyncKeys.skipped(it)
        }
    }
}

class AccountSyncService(
    private val client: AccountEndpointCalling,
    private val prefs: AccountSyncPrefs,
    sources: List<AccountSyncSource> = emptyList(),
    private val device: String = "",
    private val clock: () -> Long = System::currentTimeMillis,
    private val account: () -> String,
) {
    private val registered = ArrayList(sources)

    @Volatile var busy = false
        private set
    @Volatile var error = ""
        private set
    @Volatile var lastSavedAt: Long = (prefs.raw(AccountSyncKeys.SAVED_AT) as? Number)?.toLong() ?: 0L
        private set
    @Volatile var lastSaved = 0
        private set
    @Volatile var lastRestored = 0
        private set

    val sources: List<AccountSyncSource> get() = synchronized(registered) { registered.toList() }

    var enabled: Boolean
        get() = prefs.raw(AccountSyncKeys.ENABLED) == true
        set(value) {
            if (value == enabled) return
            prefs.putRaw(AccountSyncKeys.ENABLED, if (value) true else null)
            if (!value) forgetHashes()
        }

    fun register(source: AccountSyncSource) {
        synchronized(registered) {
            registered.removeAll { it.key == source.key }
            registered.add(source)
        }
    }

    @Synchronized
    fun saveIfChanged(): Int {
        if (!enabled || busy || account().isEmpty()) return 0
        return save(force = false)
    }

    @Synchronized
    fun saveNow(): Int {
        if (busy || account().isEmpty()) return 0
        return save(force = true)
    }

    private fun save(force: Boolean): Int {
        busy = true
        try {
            val owner = account()
            var saved = 0
            var failure = ""
            for (source in sources) {
                val value = source.snapshot() ?: continue
                val digest = digest(value)
                val hashKey = AccountSyncKeys.hash(owner, source.key)
                if (!force && prefs.raw(hashKey) == digest) continue
                val body = linkedMapOf<String, Any?>("kind" to source.kind, "label" to source.label, "value" to value)
                if (device.isNotEmpty()) body["device"] = device
                try {
                    client.accountCall("PUT", AccountDataPaths.storeItem(source.key), body)
                    prefs.putRaw(hashKey, digest)
                    saved++
                } catch (problem: AccountEndpointException) {
                    if (problem.status == 404 || problem.status == 405) {
                        failure = "This server cannot save your data yet."
                        break
                    }
                    failure = AccountErrorText.text(problem, "Could not save your data to ForkMesh.")
                } catch (problem: Exception) {
                    failure = AccountErrorText.text(problem, "Could not save your data to ForkMesh.")
                }
            }
            error = failure
            lastSaved = saved
            if (saved > 0 || (force && failure.isEmpty())) {
                lastSavedAt = clock()
                prefs.putRaw(AccountSyncKeys.SAVED_AT, lastSavedAt)
            }
            return saved
        } finally {
            busy = false
        }
    }

    @Synchronized
    fun restore(): Int {
        if (busy || account().isEmpty()) return 0
        busy = true
        try {
            val owner = account()
            var applied = 0
            var found = 0
            var failure = ""
            for (source in sources) {
                try {
                    val map = client.accountCall("GET", AccountDataPaths.storeItem(source.key), null)
                    @Suppress("UNCHECKED_CAST")
                    val value = map.obj("item")["value"] as? Map<String, Any?> ?: continue
                    found++
                    applied += source.restore(value)
                    source.snapshot()?.let { prefs.putRaw(AccountSyncKeys.hash(owner, source.key), digest(it)) }
                } catch (problem: AccountEndpointException) {
                    if (problem.status == 404) {
                        if (problem.code != "not_found") {
                            failure = "This server cannot restore saved data yet."
                            break
                        }
                    } else {
                        failure = AccountErrorText.text(problem, "Could not restore your saved data.")
                    }
                } catch (problem: Exception) {
                    failure = AccountErrorText.text(problem, "Could not restore your saved data.")
                }
            }
            if (failure.isEmpty() && found == 0) failure = "Nothing is saved on ForkMesh yet."
            error = failure
            lastRestored = applied
            return applied
        } finally {
            busy = false
        }
    }

    fun forgetHashes() {
        for (key in prefs.keys()) if (key.startsWith(AccountSyncKeys.PREFIX + "hash/")) prefs.putRaw(key, null)
    }

    companion object {
        fun digest(value: Any?): String =
            MessageDigest.getInstance("SHA-256").digest(Json.stringify(value).toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it.toInt() and 0xFF) }
    }
}
