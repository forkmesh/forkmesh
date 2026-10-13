package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.model.UserStatusException
import com.forkmesh.nativeapp.model.userStatusAwayBody
import com.forkmesh.nativeapp.model.userStatusSetBody
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class UserStatusApiTest {
    private fun api(transport: FakeTransport) =
        ApiService(TestSettings(), transport).also { it.sessionTokenProvider = { "tok" } }

    private fun answer(status: Map<String, Any?>) =
        jsonResponse(mapOf("ok" to true, "nodeName" to "alice", "userStatus" to status))

    fun testEmptyPayloadReadsTheOwnStatusWithTheBearerSession() {
        val transport = FakeTransport { request ->
            assertEq("POST", request.method)
            assertEq("/api/accounts/user-status", request.path())
            assertEq(emptyMap<String, Any?>(), request.bodyJson())
            answer(mapOf("away" to true, "emoji" to "🌴", "text" to "Vacationing", "expiresAt" to 0))
        }
        val status = api(transport).updateUserStatus()
        assertEq(UserStatus(away = true, emoji = "🌴", text = "Vacationing"), status)
        assertEq("Bearer tok", transport.requests.single().headerValue("Authorization"))
        assertEq("application/json", transport.requests.single().headerValue("Content-Type"))
    }

    fun testSetAndAwayBodiesAreSentAsBuilt() {
        val transport = FakeTransport { answer(mapOf("away" to false, "emoji" to "📅", "text" to "In a meeting", "expiresAt" to 99)) }
        val api = api(transport)
        api.updateUserStatus(userStatusSetBody("📅", "In a meeting", 99))
        api.updateUserStatus(userStatusAwayBody(false))
        assertEq(
            listOf(
                mapOf("emoji" to "📅", "text" to "In a meeting", "expiresAt" to 99L),
                mapOf("away" to false),
            ),
            transport.requests.map { it.bodyJson() },
        )
    }

    fun testRefusalsCarryTheWorkerCodeAndASentence() {
        val refused = assertFails<UserStatusException> {
            api(FakeTransport { jsonResponse(mapOf("error" to "status_expiry_in_past"), 400) })
                .updateUserStatus(userStatusSetBody("🌴", "", 1))
        }
        assertEq("status_expiry_in_past", refused.code)
        assertEq("Choose a clear-after time in the future.", refused.message)

        val bare = assertFails<UserStatusException> { api(FakeTransport { textResponse("oops", 500) }).updateUserStatus() }
        assertEq("http_500", bare.code)
    }

    fun testUnauthorizedSignsTheSessionOut() {
        var unauthorized = 0
        var code = ""
        val api = api(FakeTransport { jsonResponse(mapOf("error" to "unauthorized"), 401) })
        api.onUnauthorized = { unauthorized++ }
        api.onSessionInvalid = { code = it }
        val error = assertFails<UserStatusException> { api.updateUserStatus(userStatusAwayBody(true)) }
        assertEq("unauthorized", error.code)
        expect(unauthorized == 1)
        assertEq("unauthorized", code)
    }
}
