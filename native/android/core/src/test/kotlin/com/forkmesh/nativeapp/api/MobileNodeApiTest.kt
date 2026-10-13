package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class MobileNodeApiTest {
    private fun boundPayload() = mapOf(
        "nodeName" to "alice",
        "name" to "alice",
        "email" to "alice@example.com",
        "status" to "active",
        "sessionToken" to "mobile-session",
        "sessionKind" to "mobile_node",
        "deviceKind" to "mobile_node",
        "capabilities" to listOf("browse", "comment", "submit_issue", "submit_pr", "publish_repo", "owner_sign"),
        "deviceId" to "dev-mobile-1",
        "devices" to emptyList<Any>(),
    )

    fun testDeviceRevoked401ClearsTheSessionRotatesTheKeyAndArmsTheBanner() {
        val prefs = MemoryPrefStore()
        val secrets = MemorySecretStore()
        val settings = AccountSettings.create(prefs, secrets)
        settings.setServerUrl("ws://test.local/ws")
        val identity = TestIdentity()
        secrets.write(AuthController.IDENTITY_SEED_KEY, "old-seed")
        val originalKey = identity.publicKeyB64url
        val auth = AuthController(
            settings, prefs, secrets,
            identity = { identity },
            transport = FakeTransport { jsonResponse(emptyMap<String, Any>()) },
            session = AuthSession.fromJson(boundPayload()),
        )
        val api = ApiService(settings, FakeTransport { jsonResponse(mapOf("error" to "device_revoked"), 401) })
        api.sessionTokenProvider = { auth.session?.sessionToken ?: "" }
        val codes = ArrayList<String>()
        api.onSessionInvalid = { code ->
            codes.add(code)
            auth.handleUnauthorized(code)
        }

        assertFails<DartException> { api.repositories() }

        assertEq(listOf("device_revoked"), codes)
        expect(!auth.isAuthenticated)
        expect(auth.revokedBannerPending)
        assertEq(true, prefs.getBool(AuthController.REVOKED_BANNER_KEY))
        expect(identity.publicKeyB64url != originalKey)
        assertEq(1, identity.rotations)
        assertEq(null, secrets.read(AuthController.IDENTITY_SEED_KEY))
        assertEq(null, secrets.read(AuthController.SESSION_KEY))

        auth.clearRevokedBanner()
        expect(!auth.revokedBannerPending)
    }

    fun testListDevicesGetsTheRegistryWithTheBearerTokenAndParsesRows() {
        val transport = FakeTransport {
            jsonResponse(
                mapOf(
                    "devices" to listOf(
                        mapOf("id" to "desk-1", "kind" to "desktop_node", "label" to "MacBook Pro", "capabilities" to listOf("host_repo", "mirror_repo", "publish_repo", "owner_sign"), "enabled" to true, "lastSeen" to 100, "createdAt" to 50, "current" to false),
                        mapOf("id" to "mob-1", "kind" to "mobile_node", "label" to "Pixel 9 · Android 16", "capabilities" to listOf("publish_repo", "owner_sign"), "enabled" to true, "lastSeen" to 200, "createdAt" to 150, "current" to true),
                        mapOf("id" to "mob-0", "kind" to "mobile_node", "label" to "Old phone", "capabilities" to emptyList<String>(), "enabled" to false, "lastSeen" to 10, "createdAt" to 5, "revokedAt" to 20, "current" to false),
                    ),
                ),
            )
        }
        val api = ApiService(TestSettings(), transport)
        api.sessionTokenProvider = { "tok" }
        val devices = api.listDevices()
        val request = transport.requests.single()
        assertEq("GET", request.method)
        assertEq("/api/accounts/devices", request.path())
        assertEq("Bearer tok", request.headerValue("Authorization"))
        assertEq(3, devices.size)
        expect(devices[0].isDesktopNode)
        expect(devices[0].canHost)
        expect(devices[1].current)
        assertEq(150L, devices[1].createdAt)
        assertEq("Publishes · signs as owner", devices[1].capabilitySummary)
        expect(devices[2].isRevoked)
        assertEq(20L, devices[2].revokedAt)
        expect(!devices[2].enabled)
    }

    fun testAccountDesktopsListsKeysAndMachineNames() {
        val transport = FakeTransport {
            jsonResponse(
                mapOf(
                    "ok" to true,
                    "account" to "alice",
                    "desktops" to listOf(
                        mapOf("key" to "K1", "node" to "Studio", "lastSeen" to 300),
                        mapOf("key" to "", "node" to "broken"),
                    ),
                ),
            )
        }
        val api = ApiService(TestSettings(), transport)
        api.sessionTokenProvider = { "tok" }
        val listed = api.listAccountDesktops()
        assertEq("/api/accounts/desktops", transport.requests.single().path())
        assertEq("alice", listed.account)
        assertEq(listOf(AccountDesktop("K1", "studio", 300)), listed.desktops)
        val signedOut = ApiService(TestSettings(), transport)
        signedOut.sessionTokenProvider = { "" }
        assertEq(0, signedOut.listAccountDesktops().desktops.size)
        assertEq(1, transport.requests.size)
    }
}
