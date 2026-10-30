package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.market.PackagesTransport
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails

class DjJamApiTest {
    private val jam = "0123456789abcdef0123456789abcdef"

    fun testRequestsUseTheWorkerRoutes() {
        val sent = mutableListOf<String>()
        val api = DjJamApi(PackagesClient(PackagesTransport { method, path, query, body ->
            sent.add("$method $path${if (query.isEmpty()) "" else "?" + query.joinToString("&") { "${it.first}=${it.second}" }} ${body ?: ""}")
            ApiJsonResult(200, mapOf("ok" to true, "jams" to listOf(mapOf("id" to jam, "myStatus" to "owner"))))
        }))
        assertEq(listOf(DjJamSummary(jam, "", "", "owner", 0)), api.list())
        api.open(jam)
        api.join(jam)
        api.leave(jam, owner = false)
        api.leave(jam, owner = true)
        api.invite(jam, listOf(" @Bob"))
        api.remove(jam, "bob")
        api.enqueue(jam, "dQw4w9WgXcQ", "Song", "Band")
        api.dequeue(jam, "q1")
        api.deck(jam, DjDeckSide.B, DjJamDeck("dQw4w9WgXcQ", playing = true, position = 1.5))
        api.react(jam, "fire")
        api.people("bo")
        assertEq(
            listOf(
                "GET /api/video-mix-dj-studio/jams ",
                "GET /api/video-mix-dj-studio/jams/$jam ",
                "POST /api/video-mix-dj-studio/jams/$jam/join {}",
                "POST /api/video-mix-dj-studio/jams/$jam/leave {}",
                "DELETE /api/video-mix-dj-studio/jams/$jam ",
                "POST /api/video-mix-dj-studio/jams/$jam/members {names=[bob]}",
                "DELETE /api/video-mix-dj-studio/jams/$jam/members/bob ",
                "POST /api/video-mix-dj-studio/jams/$jam/queue {videoId=dQw4w9WgXcQ, title=Song, author=Band}",
                "DELETE /api/video-mix-dj-studio/jams/$jam/queue/q1 ",
                "PUT /api/video-mix-dj-studio/jams/$jam/decks/b {videoId=dQw4w9WgXcQ, title=, author=, playing=true, position=1.5, rate=1.0}",
                "POST /api/video-mix-dj-studio/jams/$jam/reactions {kind=fire}",
                "GET /api/chat/direct-messages/users?query=bo ",
            ),
            sent,
        )
    }

    fun testBadIdsAndErrorsBecomeSentences() {
        val api = DjJamApi(PackagesClient(PackagesTransport { _, _, _, _ -> ApiJsonResult(409, mapOf("ok" to false, "error" to "queue_full")) }))
        assertEq("jam_not_found", assertFails<PackagesException> { api.open("../x") }.code)
        assertEq("invalid_video", assertFails<PackagesException> { api.enqueue(jam, "bad", "", "") }.code)
        assertEq("queue_full", assertFails<PackagesException> { api.enqueue(jam, "dQw4w9WgXcQ", "", "") }.code)
        assertEq("The jam queue is full.", DjJamApi.problem("queue_full"))
    }
}
