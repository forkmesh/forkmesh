package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.ChatRetentionException
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class ChatRetentionApiTest {
    private fun api(transport: FakeTransport) =
        ApiService(TestSettings(), transport).also { it.sessionTokenProvider = { "tok" } }

    private fun answer(retention: Map<String, Any?>) =
        jsonResponse(mapOf("ok" to true, "nodeName" to "alice", "chatRetention" to retention))

    fun testEmptyPayloadReadsThePreferencesWithTheBearerSession() {
        val transport = FakeTransport { request ->
            assertEq("POST", request.method)
            assertEq("/api/accounts/chat-retention", request.path())
            assertEq(emptyMap<String, Any?>(), request.bodyJson())
            answer(mapOf("defaultMs" to 86_400_000L, "chats" to mapOf("#team" to 3_600_000L), "updatedAt" to 5L))
        }
        val retention = api(transport).updateChatRetention()
        assertEq(ChatRetention(86_400_000L, mapOf("#team" to 3_600_000L), 5L), retention)
        assertEq("Bearer tok", transport.requests.single().headerValue("Authorization"))
        assertEq("application/json", transport.requests.single().headerValue("Content-Type"))
    }

    fun testDefaultAndPerChatBodiesAreSentAsBuiltIncludingNullClears() {
        val transport = FakeTransport { answer(mapOf("defaultMs" to 604_800_000L, "chats" to emptyMap<String, Any?>(), "updatedAt" to 0L)) }
        val api = api(transport)
        api.updateChatRetention(ChatRetention.defaultBody(28_800_000L))
        api.updateChatRetention(ChatRetention.defaultBody(null))
        api.updateChatRetention(ChatRetention.chatBody("channel:abc", 3_600_000L))
        api.updateChatRetention(ChatRetention.chatBody("@peer", null))
        assertEq(
            listOf(
                mapOf("defaultMs" to 28_800_000L),
                mapOf("defaultMs" to null),
                mapOf("chat" to "channel:abc", "retentionMs" to 3_600_000L),
                mapOf("chat" to "@peer", "retentionMs" to null),
            ),
            transport.requests.map { it.bodyJson() },
        )
    }

    fun testRefusalsCarryTheServerCodeAndASentence() {
        val refused = assertFails<ChatRetentionException> {
            api(FakeTransport { jsonResponse(mapOf("error" to "invalid_chat_retention"), 400) })
                .updateChatRetention(ChatRetention.chatBody("", 1))
        }
        assertEq("invalid_chat_retention", refused.code)
        assertEq("That disappearing-message time is not valid.", refused.message)
        val bare = assertFails<ChatRetentionException> { api(FakeTransport { textResponse("oops", 500) }).updateChatRetention() }
        assertEq("http_500", bare.code)
    }

    fun testUnauthorizedSignsTheSessionOut() {
        var unauthorized = 0
        val api = api(FakeTransport { jsonResponse(mapOf("error" to "unauthorized"), 401) })
        api.onUnauthorized = { unauthorized++ }
        val error = assertFails<ChatRetentionException> { api.updateChatRetention() }
        assertEq("unauthorized", error.code)
        expect(unauthorized == 1)
    }

    fun testLoginPayloadCarriesAndPersistsTheRetention() {
        val session = AuthSession.fromJson(
            mapOf("nodeName" to "alice", "chatRetention" to mapOf("defaultMs" to 3_600_000L, "chats" to emptyMap<String, Any?>())),
        )
        assertEq(3_600_000L, session.chatRetention?.defaultMs)
        assertEq(session.chatRetention, AuthSession.fromJson(session.toJson()).chatRetention)
        assertEq(null, AuthSession.fromJson(mapOf("nodeName" to "bob")).chatRetention)
        expect(!AuthSession.fromJson(mapOf("nodeName" to "bob")).toJson().containsKey("chatRetention"))
    }
}
