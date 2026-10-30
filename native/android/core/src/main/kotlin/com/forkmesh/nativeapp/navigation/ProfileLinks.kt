package com.forkmesh.nativeapp.navigation

object ProfileLinks {
    const val PREFIX = "forkmesh://profile/@"
    private val reserved = setOf("forkbot", "here", "channel", "everyone")
    private val accountPattern = Regex("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")

    fun account(raw: String): String? {
        val name = raw.trim().removePrefix("@").lowercase()
        if (name in reserved || !accountPattern.matches(name)) return null
        return name
    }

    fun link(raw: String): String? = account(raw)?.let { PREFIX + it }

    fun isProfileLink(url: String): Boolean = url.startsWith(PREFIX)
}
