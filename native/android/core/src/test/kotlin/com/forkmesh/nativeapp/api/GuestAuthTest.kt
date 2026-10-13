package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.expect

class GuestAuthTest {
    private fun session(): AuthSession = AuthSession.fromJson(
        mapOf(
            "nodeName" to "demo-node",
            "email" to "demo@example.com",
            "status" to "active",
            "emailVerified" to true,
            "capabilities" to listOf("submit_issue", "submit_pr"),
        ),
    )

    private class Fixture {
        val prefs = MemoryPrefStore()
        val secrets = MemorySecretStore()
        val settings = AccountSettings.create(prefs, secrets)
        val identity = TestIdentity()

        fun auth(session: AuthSession? = null) =
            AuthController(settings, prefs, secrets, { identity }, FakeTransport { jsonResponse(emptyMap<String, Any?>()) }, session)
    }

    fun testGuestModeIsPersistedUntilTheUserLeavesIt() {
        val f = Fixture()
        val auth = f.auth()
        expect(!auth.isGuest)
        expect(!auth.hasAppAccess)

        auth.enterGuestMode()
        expect(auth.isGuest)
        expect(auth.hasAppAccess)
        expect(f.auth().isGuest, "guest mode survives a new controller")

        auth.exitGuestMode()
        expect(!auth.isGuest)
        expect(!auth.hasAppAccess)
    }

    fun testSignedInUserIsNeverGuestAndSigningInClearsIt() {
        val f = Fixture()
        val signedIn = f.auth(session())
        signedIn.enterGuestMode()
        expect(!signedIn.isGuest)

        val auth = f.auth()
        auth.enterGuestMode()
        auth.authenticatePreview()
        expect(!auth.isGuest)
        expect(!f.prefs.contains(AuthController.GUEST_MODE_KEY))
    }

    fun testGuestModeChangesNotifyListeners() {
        val f = Fixture()
        val auth = f.auth()
        var calls = 0
        auth.addListener { calls++ }
        auth.enterGuestMode()
        auth.exitGuestMode()
        expect(calls == 2, "expected two notifications, got $calls")
    }
}
