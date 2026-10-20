package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PackagesException

class DjJamApi(private val client: PackagesClient) {
    companion object {
        const val PACKAGE_ID = "video-mix-dj-studio"
        const val API = "/api/video-mix-dj-studio"
        private val JAM_ID = Regex("^[0-9a-f]{32}$")
        private val NAME = Regex("^[a-z0-9](?:[a-z0-9-]{0,62})$")
        private val ITEM = Regex("^[A-Za-z0-9_-]{1,64}$")

        fun isJamId(value: String): Boolean = JAM_ID.matches(value)

        fun problem(code: String): String = when (code) {
            "jam_not_found" -> "That jam has ended or you were removed from it."
            "too_many_jams" -> "You already have 20 jams. End one first."
            "queue_full" -> "The jam queue is full."
            "too_many_reactions" -> "Slow down a little: reactions are limited to eight every 12 seconds."
            "jam_not_joined" -> "Join the jam first."
            "jam_forbidden" -> "Only the host can do that."
            "owner_cannot_leave" -> "The host ends the jam instead of leaving it."
            "invalid_video" -> "That is not a YouTube video."
            "invalid_session", "unauthorized", "sign_in_required" -> "Sign in to ForkMesh to jam with other people."
            "network" -> "ForkMesh could not be reached. Check your connection."
            else -> PackagesException.sentence(code)
        }
    }

    private fun jam(id: String): String {
        if (!isJamId(id)) throw PackagesException("jam_not_found", 404)
        return "$API/jams/$id"
    }

    fun list(): List<DjJamSummary> = DjJamSummary.list(client.call("GET", "$API/jams"))

    fun open(id: String): JsonMap = client.call("GET", jam(id))

    fun create(decks: Map<String, Any?>): JsonMap = client.call("POST", "$API/jams", body = linkedMapOf("decks" to decks))

    fun join(id: String): JsonMap = client.call("POST", "${jam(id)}/join", body = emptyMap())

    fun leave(id: String, owner: Boolean): JsonMap =
        if (owner) client.call("DELETE", jam(id)) else client.call("POST", "${jam(id)}/leave", body = emptyMap())

    fun invite(id: String, names: List<String>): JsonMap =
        client.call("POST", "${jam(id)}/members", body = linkedMapOf("names" to names.map { it.trim().removePrefix("@").lowercase() }))

    fun remove(id: String, name: String): JsonMap {
        if (!NAME.matches(name)) throw PackagesException("jam_forbidden", 403)
        return client.call("DELETE", "${jam(id)}/members/$name")
    }

    fun enqueue(id: String, videoId: String, title: String, author: String): JsonMap {
        if (!DjStudio.isVideoId(videoId)) throw PackagesException("invalid_video")
        return client.call("POST", "${jam(id)}/queue", body = linkedMapOf("videoId" to videoId, "title" to title, "author" to author))
    }

    fun dequeue(id: String, item: String): JsonMap {
        if (!ITEM.matches(item)) throw PackagesException("not_found", 404)
        return client.call("DELETE", "${jam(id)}/queue/$item")
    }

    fun deck(id: String, side: DjDeckSide, state: DjJamDeck): JsonMap = client.call("PUT", "${jam(id)}/decks/${side.key}", body = state.body)

    fun presence(id: String, body: Map<String, Any?>): JsonMap = client.call("POST", "${jam(id)}/presence", body = body)

    fun react(id: String, kind: String): JsonMap = client.call("POST", "${jam(id)}/reactions", body = linkedMapOf("kind" to kind))

    fun people(query: String): JsonMap = client.call("GET", "/api/chat/direct-messages/users", listOf("query" to query))
}
