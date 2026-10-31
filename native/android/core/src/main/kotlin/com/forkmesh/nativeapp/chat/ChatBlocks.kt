package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText

class ChatBlocks(private val api: ChatAccountCalling, private val prefs: PrefStore? = null) {
    companion object {
        const val PATH = "/api/chat/blocks"
        const val MAX_ACCOUNTS = 500
        const val LIST_KEY = "chat/blockedAccounts"
        const val DIRTY_KEY = "chat/blockedAccountsUnsent"

        fun normalized(account: String): String = account.trim().lowercase().removePrefix("@").take(ChatLimits.MAX_DISPLAY_NAME_CHARS * 4)
    }

    private val lock = Any()
    private val list = LinkedHashSet<String>()
    private var dirty = false

    @Volatile
    var onChange: (() -> Unit)? = null

    init {
        val stored = prefs?.getString(LIST_KEY)
        if (!stored.isNullOrEmpty()) {
            runCatching { (Json.parse(stored) as? List<*>).orEmpty().map(::jsonText).map(::normalized).filter { it.isNotEmpty() } }
                .getOrNull()?.let { list.addAll(it.take(MAX_ACCOUNTS)) }
        }
        dirty = prefs?.getBool(DIRTY_KEY) == true
    }

    val accounts: List<String> get() = synchronized(lock) { list.toList() }

    fun isBlocked(account: String): Boolean {
        val name = normalized(account)
        return name.isNotEmpty() && synchronized(lock) { name in list }
    }

    fun set(account: String, blocked: Boolean): Boolean {
        val name = normalized(account)
        if (name.isEmpty()) return false
        val changed = synchronized(lock) {
            val moved = if (blocked) list.add(name) else list.remove(name)
            if (moved) {
                while (list.size > MAX_ACCOUNTS) list.remove(list.first())
                dirty = true
                save()
            }
            moved
        }
        if (!changed) return false
        onChange?.invoke()
        push()
        return true
    }

    fun push(): Boolean {
        val body = synchronized(lock) { linkedMapOf<String, Any?>("accounts" to list.toList()) }
        val ok = runCatching { api.accountApiCall("PUT", PATH, body) }.isSuccess
        if (ok) synchronized(lock) {
            if (body["accounts"] == list.toList()) {
                dirty = false
                save()
            }
        }
        return ok
    }

    fun signedIn(): Boolean {
        if (synchronized(lock) { dirty }) return push()
        val answer = runCatching { api.accountApiCall("GET", PATH, null) }.getOrNull() ?: return false
        val fetched = (answer["accounts"] as? List<*>).orEmpty().map { normalized(jsonText(it)) }.filter { it.isNotEmpty() }.distinct().take(MAX_ACCOUNTS)
        val changed = synchronized(lock) {
            if (fetched == list.toList()) return@synchronized false
            list.clear()
            list.addAll(fetched)
            save()
            true
        }
        if (changed) onChange?.invoke()
        return true
    }

    fun clear() {
        synchronized(lock) {
            list.clear()
            dirty = false
            save()
        }
        onChange?.invoke()
    }

    private fun save() {
        val store = prefs ?: return
        store.putString(LIST_KEY, Json.stringify(list.toList()))
        store.putBool(DIRTY_KEY, dirty)
    }
}
