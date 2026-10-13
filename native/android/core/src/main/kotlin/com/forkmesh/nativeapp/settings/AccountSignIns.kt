package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.format.relativeTime

data class AccountSignIn(
    val id: String,
    val deviceLabel: String,
    val ipAddress: String = "",
    val createdAt: Long = 0,
    val lastSeenAt: Long = 0,
    val expiresAt: Long = 0,
    val current: Boolean = false,
) {
    val title: String get() = if (current) "$deviceLabel (this app)" else deviceLabel

    val icon: String
        get() {
            val label = deviceLabel.lowercase()
            return when {
                "desktop" in label -> "desktop_mac_outlined"
                "mobile" in label -> "phone_iphone"
                "browser" in label -> "language"
                else -> "devices_other"
            }
        }

    fun meta(nowMillis: Long = System.currentTimeMillis()): String = listOfNotNull(
        when {
            lastSeenAt > 0 -> "Active ${relativeTime(lastSeenAt, nowMillis)}"
            createdAt > 0 -> "Signed in ${relativeTime(createdAt, nowMillis)}"
            else -> null
        },
        ipAddress.ifEmpty { null },
    ).joinToString(" · ")

    companion object {
        private fun long(value: Any?): Long = when (value) {
            is Number -> value.toLong()
            is String -> value.trim().toLongOrNull() ?: value.trim().toDoubleOrNull()?.toLong() ?: 0L
            else -> 0L
        }

        private fun text(value: Any?): String = value?.toString()?.trim() ?: ""

        fun fromJson(json: Map<*, *>): AccountSignIn = AccountSignIn(
            id = text(json["id"]),
            deviceLabel = text(json["deviceLabel"]).ifEmpty { "Unknown device" },
            ipAddress = text(json["ipAddress"]),
            createdAt = long(json["createdAt"]),
            lastSeenAt = long(json["lastSeenAt"]),
            expiresAt = long(json["expiresAt"]),
            current = json["current"] == true,
        )

        fun parse(data: Any?): List<AccountSignIn> {
            val list = (data as? Map<*, *>)?.get("sessions") as? List<*> ?: return emptyList()
            return list.mapNotNull { (it as? Map<*, *>)?.let(::fromJson) }.filter { it.id.isNotEmpty() }
        }
    }
}

object AccountSignIns {
    const val PATH = "/api/accounts/sessions"
    const val OTHERS = "others"
    const val TITLE = "Where you're signed in"
    const val BLURB = "Browsers, desktop apps and phones logged in to your account. Log out any you don't recognise or no longer use."
    const val LOAD_FAILED = "Could not load your sessions. Tap refresh to try again."
    const val REVOKE_FAILED = "Could not log that session out. Try again."
    const val SIGNED_OUT_HERE = "This device was signed out too."

    fun headline(sessions: List<AccountSignIn>?): String {
        if (sessions == null) return "Signed-in sessions"
        return "${sessions.size} active session${if (sessions.size == 1) "" else "s"}"
    }

    fun emptyText(loading: Boolean, loaded: Boolean): String =
        if (loading || !loaded) "Loading sessions…" else "No active sessions found."

    fun others(sessions: List<AccountSignIn>): Int = sessions.count { !it.current }

    fun revokePath(id: String): String = "$PATH/${java.net.URLEncoder.encode(id, "UTF-8").replace("+", "%20")}"

    fun currentRevoked(data: Any?): Boolean = (data as? Map<*, *>)?.get("currentRevoked") == true

    fun confirmTitle(session: AccountSignIn): String = if (session.current) "Log out of this app?" else "Log out this session?"

    fun confirmBody(session: AccountSignIn): String = if (session.current) {
        "This ends the session this app is using. You will need to sign in again here."
    } else {
        "${session.deviceLabel} will be signed out and must sign in again to use your account."
    }

    fun confirmOthersBody(count: Int): String =
        "Every other browser, desktop app and phone signed in to your account ($count) will be signed out. This app stays signed in."

    fun revokedText(target: String): String = if (target == OTHERS) "Logged out of every other session." else "Session logged out."

    fun remaining(sessions: List<AccountSignIn>, target: String): List<AccountSignIn> =
        if (target == OTHERS) sessions.filter { it.current } else sessions.filter { it.id != target }
}
