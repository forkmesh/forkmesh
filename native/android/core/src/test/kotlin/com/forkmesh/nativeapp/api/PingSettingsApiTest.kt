package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class PingSettingsApiTest {
    private fun api(transport: FakeTransport, token: String = "tok") =
        ApiService(TestSettings(), transport).also { if (token.isNotEmpty()) it.sessionTokenProvider = { token } }

    private val payload = mapOf(
        "ok" to true,
        "emailNotifications" to true,
        "notificationPreferences" to mapOf("mention" to true),
        "notificationDelivery" to "digest",
        "notificationPauseUntil" to 0,
        "pingMutedKinds" to listOf("repo_activity"),
        "notificationQuietHours" to mapOf("enabled" to false, "start" to 1320, "end" to 420, "days" to listOf(0, 1, 2, 3, 4, 5, 6), "timezone" to "UTC"),
        "notificationQuietUntil" to 0,
        "pingKindCatalog" to listOf(
            mapOf("key" to "conversations", "label" to "Conversations", "kinds" to listOf(mapOf("kind" to "mention", "label" to "Mentions", "email" to true))),
        ),
    )

    fun testReadIsAGetWithSettingsFlagAndBearer() {
        val transport = FakeTransport { jsonResponse(payload) }
        val settings = api(transport).pingSettings(" Alice ")
        val request = transport.requests.single()
        assertEq("GET", request.method)
        assertEq("/api/notifications", request.path())
        assertEq(mapOf("node" to "alice", "settings" to "1"), request.query())
        assertEq("Bearer tok", request.headerValue("Authorization"))
        assertEq(setOf("repo_activity"), settings.pingMutedKinds)
        assertEq("Mentions", settings.pingKindCatalog.single().kinds.single().label)
        assertEq(null, settings.statusMonitorAlerts)
    }

    fun testSaveSendsOnlyThePatchAndAdoptsTheAnswer() {
        val transport = FakeTransport { jsonResponse(payload + ("pingMutedKinds" to listOf("repo_activity", "mention"))) }
        val saved = api(transport).savePingSettings("Alice", mapOf("pingMutedKinds" to mapOf("mention" to false)))
        val request = transport.requests.single()
        assertEq("POST", request.method)
        assertEq("/api/notifications", request.path())
        assertEq("Bearer tok", request.headerValue("Authorization"))
        assertEq("application/json", request.headerValue("Content-Type"))
        assertEq(
            mapOf("node" to "alice", "pingSettings" to mapOf("pingMutedKinds" to mapOf("mention" to false))),
            request.bodyJson(),
        )
        assertEq(setOf("repo_activity", "mention"), saved.pingMutedKinds)
    }

    fun testSaveRefusalsThrowReadableErrors() {
        val transport = FakeTransport { jsonResponse(mapOf("error" to "forbidden"), 403) }
        val api = api(transport)
        val forbidden = assertFails<DartException> {
            api.savePingSettings("alice", mapOf("statusMonitorAlerts" to mapOf("website" to mapOf("pings" to false))))
        }
        assertEq("Only administrators can change platform alerts.", forbidden.message)

        var unauthorized = 0
        api.onUnauthorized = { unauthorized++ }
        transport.handler = { jsonResponse(mapOf("error" to "unauthorized"), 401) }
        val expired = assertFails<DartException> { api.savePingSettings("alice", mapOf("emailNotifications" to false)) }
        assertEq("Your session expired. Please sign in again.", expired.message)
        assertEq(1, unauthorized)

        transport.handler = { textResponse("oops", 500) }
        val broken = assertFails<DartException> { api.savePingSettings("alice", mapOf("emailNotifications" to false)) }
        assertEq("Could not save ping settings (HTTP 500).", broken.message)
    }

    fun testNoAccountRequestsNothing() {
        val transport = FakeTransport { jsonResponse(payload) }
        val api = api(transport)
        expect(assertFails<DartException> { api.pingSettings("  ") }.message!!.contains("Sign in"))
        assertFails<DartException> { api.savePingSettings("", mapOf("emailNotifications" to true)) }
        assertEq(0, transport.requests.size)
    }
}
