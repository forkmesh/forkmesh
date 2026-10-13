package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ActivityTodayApiTest {
    fun testApiPostsToTheActivityRoute() {
        val transport = FakeTransport { request ->
            assertEq("POST", request.method)
            assertEq("/api/accounts/activity-today", request.path())
            jsonResponse(mapOf("ok" to true, "nodeName" to "alice", "others" to List(288) { 0 }))
        }
        val api = ApiService(TestSettings(), transport).also { it.sessionTokenProvider = { "tok" } }
        val result = api.syncActivityToday(mapOf("day" to "2026-10-06", "device" to "device-0001"))
        expect(result.ok)
        assertEq("Bearer tok", transport.requests.single().headerValue("Authorization"))
        assertEq(mapOf("day" to "2026-10-06", "device" to "device-0001"), transport.requests.single().bodyJson())
    }
}
