package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AvatarRefreshTest {
    private fun session(kind: String = "account"): AuthSession = AuthSession.fromJson(
        mapOf(
            "nodeName" to "demo node",
            "email" to "demo@example.com",
            "status" to "active",
            "avatarPng" to "old",
            "avatarUpdatedAt" to 5L,
            "sessionKind" to kind,
            "sessionToken" to "tok",
        ),
    )

    fun testAdoptsANewerPictureAndKeepsItAcrossRestarts() {
        val prefs = MemoryPrefStore()
        val secrets = MemorySecretStore()
        val settings = AccountSettings.create(prefs, secrets)
        val transport = FakeTransport { jsonResponse(mapOf("exists" to true, "avatarPng" to "new", "avatarUpdatedAt" to 9L)) }
        val auth = AuthController(settings, prefs, secrets, { TestIdentity() }, transport, session())
        var notified = 0
        auth.addListener { notified++ }
        expect(auth.refreshAvatar())
        expect(transport.requests.single().url.endsWith("/api/accounts/demo%20node"))
        assertEq("new", auth.session?.avatarPng)
        assertEq(9L, auth.session?.avatarUpdatedAt)
        assertEq("tok", auth.session?.sessionToken)
        assertEq(1, notified)
        assertEq("new", AuthController.readSession(secrets, "")?.avatarPng)
        expect(!auth.refreshAvatar())
    }

    fun testLeavesTheSessionAloneWhenNothingChanged() {
        val prefs = MemoryPrefStore()
        val secrets = MemorySecretStore()
        val settings = AccountSettings.create(prefs, secrets)
        val missing = FakeTransport { jsonResponse(mapOf("exists" to false)) }
        expect(!AuthController(settings, prefs, secrets, { TestIdentity() }, missing, session()).refreshAvatar())
        val silent = FakeTransport { jsonResponse(mapOf("exists" to true)) }
        expect(!AuthController(settings, prefs, secrets, { TestIdentity() }, silent, session()).refreshAvatar())
        val preview = FakeTransport { jsonResponse(mapOf("avatarPng" to "new")) }
        expect(!AuthController(settings, prefs, secrets, { TestIdentity() }, preview, session("preview")).refreshAvatar())
        assertEq(0, preview.requests.size)
    }
}
