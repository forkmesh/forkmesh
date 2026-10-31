package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.api.DartUri
import com.forkmesh.nativeapp.api.ServerUrl

data class RemoteFork(
    val url: String,
    val origin: String,
    val host: String,
    val owner: String,
    val name: String,
    val label: String = "",
) {
    val displayName: String get() = "$owner/$name on $host"

    val pullsUrl: String? get() = if (validOrigin(origin)) "$origin/api/repo/${DartUri.encodeComponent(owner)}/${DartUri.encodeComponent(name)}/pulls" else null

    companion object {
        private val ORIGIN = Regex("^https://[a-z0-9.-]+(?::\\d{1,5})?$")
        private val NAME = Regex("^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$")

        fun validOrigin(origin: String): Boolean = ORIGIN.matches(origin)

        fun validName(value: String): Boolean = NAME.matches(value)

        fun fromJson(j: Map<*, *>): RemoteFork? {
            fun text(key: String): String = (j[key] as? String).orEmpty()
            val origin = text("origin").lowercase()
            val owner = text("owner")
            val name = text("name")
            if (!validOrigin(origin) || !validName(owner) || !validName(name)) return null
            return RemoteFork(
                url = text("url").ifEmpty { "$origin/$owner/$name" },
                origin = origin,
                host = text("host").ifEmpty { origin.removePrefix("https://") },
                owner = owner,
                name = name,
                label = text("label"),
            )
        }

        fun list(data: Any?): List<RemoteFork> =
            ((data as? Map<*, *>)?.get("forks") as? List<*>).orEmpty().mapNotNull { (it as? Map<*, *>)?.let(::fromJson) }

        fun signedHead(host: String, owner: String, name: String, branch: String): String = "$host/$owner/$name:$branch"

        fun instanceHost(serverUrl: String): String {
            val server = ServerUrl.parse(serverUrl)
            val port = server.port
            return if (port == null || port == 443) server.host else "${server.host}:$port"
        }
    }
}
