package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.api.ServerUrl
import java.net.URI

object MeetingLink {
    const val PATH = "/call"
    private val room = Regex("^[0-9a-f]{32}$")
    private val key = Regex("^[0-9a-f]{64}$")

    fun route(rawQuery: String?, rawFragment: String?): Route.Meeting? {
        val roomId = rawQuery.orEmpty().split('&', '=', '#').first().lowercase()
        val keyHex = rawFragment.orEmpty().split('&', '=').first().lowercase()
        if (!room.matches(roomId) || !key.matches(keyHex)) return null
        return Route.Meeting(roomId, keyHex)
    }

    fun route(url: String): Route.Meeting? {
        val uri = runCatching { URI(url.trim()) }.getOrNull() ?: return null
        if (uri.scheme?.lowercase() != "https" || uri.host.isNullOrEmpty()) return null
        if (uri.rawPath?.trimEnd('/') != PATH) return null
        return route(uri.rawQuery, uri.rawFragment)
    }

    fun pageUrl(serverUrl: String, meeting: Route.Meeting): String =
        ServerUrl.parse(serverUrl).httpOrigin + PATH + "?" + meeting.room + "#" + meeting.key

    fun isMeetingPage(url: String, serverUrl: String): Boolean {
        val uri = runCatching { URI(url.trim()) }.getOrNull() ?: return false
        val origin = runCatching { URI(ServerUrl.parse(serverUrl).httpOrigin) }.getOrNull() ?: return false
        return uri.scheme.equals(origin.scheme, ignoreCase = true) &&
            uri.host.equals(origin.host, ignoreCase = true) &&
            uri.port == origin.port &&
            uri.rawPath?.trimEnd('/') == PATH
    }

    fun sameOrigin(origin: String, serverUrl: String): Boolean {
        val a = runCatching { URI(origin.trim()) }.getOrNull() ?: return false
        val b = runCatching { URI(ServerUrl.parse(serverUrl).httpOrigin) }.getOrNull() ?: return false
        return a.scheme.equals(b.scheme, ignoreCase = true) && a.host.equals(b.host, ignoreCase = true) && a.port == b.port
    }
}
