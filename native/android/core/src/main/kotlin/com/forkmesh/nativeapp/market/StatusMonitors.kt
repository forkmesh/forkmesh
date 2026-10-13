package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import java.net.URI
import java.util.concurrent.CopyOnWriteArrayList

data class StatusMonitor(val id: String, val name: String, val url: String) {
    fun toJson(): Map<String, Any?> = linkedMapOf("id" to id, "name" to name, "url" to url)

    companion object {
        fun fromJson(json: JsonMap): StatusMonitor? {
            val id = json.str("id")
            if (!PackagesText.isHexId(id)) return null
            val url = json.str("url")
            if (StatusMonitorDraft.urlIssue(url) != null) return null
            return StatusMonitor(id, PackagesText.bounded(json.str("name"), StatusMonitorDraft.NAME_LIMIT).ifEmpty { url }, url)
        }
    }
}

data class StatusMonitorList(val installed: Boolean = true, val max: Int = DEFAULT_MAX, val monitors: List<StatusMonitor> = emptyList()) {
    val full: Boolean get() = monitors.size >= max

    fun toJson(): Map<String, Any?> = linkedMapOf("installed" to installed, "max" to max, "monitors" to monitors.map(StatusMonitor::toJson))

    companion object {
        const val DEFAULT_MAX = 20

        fun fromJson(json: JsonMap, previous: StatusMonitorList? = null): StatusMonitorList = StatusMonitorList(
            json.optBool("installed") ?: previous?.installed ?: true,
            json.int("max", previous?.max ?: DEFAULT_MAX).coerceIn(0, 200),
            json.maps("monitors").take(200).mapNotNull(StatusMonitor::fromJson).distinctBy { it.id },
        )
    }
}

object StatusMonitorDraft {
    const val NAME_LIMIT = 40
    const val URL_LIMIT = 300

    fun nameIssue(name: String): String? = if (name.trim().length !in 1..NAME_LIMIT) "invalid_name" else null

    fun urlIssue(url: String): String? {
        val text = url.trim()
        if (text.isEmpty() || text.length > URL_LIMIT) return "invalid_url"
        val uri = runCatching { URI(text) }.getOrNull() ?: return "invalid_url"
        if (uri.scheme?.lowercase() !in setOf("http", "https")) return "invalid_url"
        if (uri.host.isNullOrEmpty() || !uri.rawUserInfo.isNullOrEmpty()) return "invalid_url"
        return null
    }

    fun issue(name: String, url: String): String? = nameIssue(name) ?: urlIssue(url)
}

class StatusMonitorsStore(private val client: PackagesClient?, private val prefs: PrefStore?) {
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var list: StatusMonitorList = load()
        private set

    val monitors: List<StatusMonitor> get() = list.monitors

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    fun refresh(): StatusMonitorList = adopt(StatusMonitorList.fromJson(requireClient().call("GET", PATH), list))

    fun add(name: String, url: String): StatusMonitorList {
        StatusMonitorDraft.issue(name, url)?.let { throw PackagesException(it, 400) }
        val body = linkedMapOf<String, Any?>("name" to name.trim(), "url" to url.trim())
        return adopt(StatusMonitorList.fromJson(requireClient().call("POST", "$PATH/monitors", body = body), list))
    }

    fun update(id: String, name: String, url: String): StatusMonitorList {
        StatusMonitorDraft.issue(name, url)?.let { throw PackagesException(it, 400) }
        val body = linkedMapOf<String, Any?>("name" to name.trim(), "url" to url.trim())
        return adopt(StatusMonitorList.fromJson(requireClient().call("PUT", "$PATH/monitors/${checked(id)}", body = body), list))
    }

    fun remove(id: String): StatusMonitorList =
        adopt(StatusMonitorList.fromJson(requireClient().call("DELETE", "$PATH/monitors/${checked(id)}", body = emptyMap()), list))

    fun clear() {
        adopt(StatusMonitorList(installed = false, max = list.max))
    }

    private fun adopt(next: StatusMonitorList): StatusMonitorList {
        val changed = next != list
        list = next
        prefs?.putString(CACHE_KEY, Json.stringify(next.toJson()))
        if (changed) for (listener in listeners) listener()
        return next
    }

    private fun requireClient(): PackagesClient = client ?: throw PackagesException("offline")

    private fun checked(id: String): String {
        if (!PackagesText.isHexId(id)) throw PackagesException("monitor_not_found", 404)
        return id
    }

    private fun load(): StatusMonitorList {
        val text = prefs?.getString(CACHE_KEY) ?: return StatusMonitorList()
        return StatusMonitorList.fromJson(Json.parseObject(text))
    }

    companion object {
        const val PATH = "/api/status-monitors"
        const val CACHE_KEY = "packages/status-page/monitors"
    }
}
