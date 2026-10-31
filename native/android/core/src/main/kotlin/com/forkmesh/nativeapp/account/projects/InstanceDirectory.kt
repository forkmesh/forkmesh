package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient

data class InstanceDirectoryEntry(val id: String, val name: String, val logoUrl: String = "", val online: Boolean = true) {
    val initial: String get() = name.ifEmpty { id }.take(1).uppercase()
}

class InstanceDirectory(
    private val transport: HttpTransport = HttpTransport.DEFAULT,
    private val origin: String = "https://${ProjectDefaults.PROD_HOST}",
    private val timeoutMs: Int = 8000,
) {
    fun list(): List<InstanceDirectoryEntry> {
        val response = runCatching {
            transport.send(HttpClient.Request.get("$origin/api/start/instances").header("Accept", "application/json").timeouts(timeoutMs, timeoutMs))
        }.getOrNull() ?: return listOf(flagship)
        if (response.status != 200) return listOf(flagship)
        val body = runCatching { Json.parse(response.text()) }.getOrNull() as? Map<*, *> ?: return listOf(flagship)
        return entries(body["instances"] as? List<*> ?: emptyList<Any?>())
    }

    fun entries(rows: List<*>): List<InstanceDirectoryEntry> {
        val seen = hashSetOf(flagship.id)
        val found = arrayListOf(flagship)
        for (raw in rows) {
            val row = raw as? Map<*, *> ?: continue
            if (row["listed"] != true) continue
            val url = (row["url"] as? String).orEmpty().trim()
            if (!url.lowercase().startsWith("https://")) continue
            val id = Project.normalizeOrigin(url) ?: continue
            if (!seen.add(id)) continue
            val logo = (row["logo"] as? String).orEmpty()
            val name = (row["name"] as? String).orEmpty().trim().take(40)
            found += InstanceDirectoryEntry(
                id = id,
                name = name.ifEmpty { id },
                logoUrl = if (LOGO_PATH.matches(logo)) origin + logo else "",
                online = row["status"] == "online",
            )
        }
        return found
    }

    companion object {
        val flagship = InstanceDirectoryEntry(ProjectDefaults.PROD_HOST, "ForkMesh")
        private val LOGO_PATH = Regex("^/api/start/instances/[0-9a-f]{32}/logo\\?v=\\d+$")

        fun logoCandidates(result: ProbeResult?, instances: List<InstanceDirectoryEntry>): List<String> {
            val project = result?.takeIf { it.ok }?.project ?: return emptyList()
            val listed = instances.firstOrNull { it.id == project.id }?.logoUrl.orEmpty()
            val favicons = if (result.branded) emptyList() else listOf("/favicon.ico", "/favicon/favicon.ico").map { project.httpOrigin + it }
            return (listOf(result.logoUrl, listed) + favicons).filter { it.isNotEmpty() }.distinct()
        }
    }
}
