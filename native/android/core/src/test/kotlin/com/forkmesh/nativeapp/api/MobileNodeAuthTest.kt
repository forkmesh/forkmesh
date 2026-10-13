package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.jsonOf
import com.forkmesh.nativeapp.model.auth.AccountDevice
import com.forkmesh.nativeapp.model.auth.AuthException
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.model.auth.androidDeviceLabel
import com.forkmesh.nativeapp.model.auth.authErrorMessage
import com.forkmesh.nativeapp.model.auth.deviceBindCanonical
import com.forkmesh.nativeapp.model.auth.joinDeviceLabel
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class MobileNodeAuthTest {
    private fun boundPayload(
        name: String = "alice",
        deviceId: String = "dev-mobile-1",
        token: String = "mobile-session",
        devices: List<Map<String, Any?>> = emptyList(),
    ): Map<String, Any?> = jsonOf(
        "nodeName" to name,
        "name" to name,
        "email" to "alice@example.com",
        "status" to "active",
        "sessionToken" to token,
        "sessionKind" to "mobile_node",
        "deviceKind" to "mobile_node",
        "deviceKeyMatched" to true,
        "desktopCapable" to false,
        "capabilities" to listOf("browse", "comment", "submit_issue", "submit_pr", "publish_repo", "owner_sign"),
        "canHost" to false,
        "canPublish" to true,
        "canOwnerSign" to true,
        "deviceId" to deviceId,
        "devices" to devices,
    )

    private fun plainPayload(name: String = "alice"): Map<String, Any?> = jsonOf(
        "nodeName" to name,
        "name" to name,
        "email" to "alice@example.com",
        "status" to "active",
        "sessionToken" to "plain-session",
        "sessionKind" to "account",
        "deviceKind" to "web_or_mobile",
        "capabilities" to listOf("browse", "comment", "submit_issue", "submit_pr"),
    )

    private class Fixture(prefSeed: Map<String, Any> = emptyMap()) {
        val prefs = MemoryPrefStore(prefSeed)
        val secrets = MemorySecretStore(mapOf(AuthController.IDENTITY_SEED_KEY to "original-seed"))
        val settings = AccountSettings.create(prefs, secrets).also { it.setServerUrl("ws://test.local/ws") }
        val identity = TestIdentity()

        fun create(
            transport: HttpTransport,
            label: () -> String = { "Mobile device" },
            ts: () -> String = { System.currentTimeMillis().toString() },
        ) = AuthController.create(settings, prefs, secrets, { identity }, transport, label, ts)
    }

    private fun verifyBind(body: Map<String, Any?>, account: String): Boolean {
        val pubkey = body["pubkey"] as String
        val ts = body["deviceTs"] as String
        val sig = body["deviceSig"] as String
        return TestIdentity.verify(deviceBindCanonical(account, pubkey, ts).toByteArray(Charsets.UTF_8), sig, pubkey)
    }

    fun testParsesDeviceIdSessionKindMobileDevicesAndIsMobileNode() {
        val session = AuthSession.fromJson(
            boundPayload(
                devices = listOf(
                    jsonOf(
                        "id" to "dev-desktop",
                        "kind" to "desktop_node",
                        "label" to "MacBook Pro",
                        "capabilities" to listOf("host_repo", "publish_repo", "owner_sign"),
                        "enabled" to true,
                        "lastSeen" to 1L,
                        "createdAt" to 2L,
                    ),
                    jsonOf(
                        "id" to "dev-mobile-1",
                        "kind" to "mobile_node",
                        "label" to "iPhone 15 Pro · iOS 19.0",
                        "capabilities" to "browse,publish_repo,owner_sign",
                        "enabled" to true,
                        "lastSeen" to 3L,
                        "createdAt" to 4L,
                        "current" to true,
                    ),
                    jsonOf(
                        "id" to "dev-mobile-old",
                        "kind" to "mobile_node",
                        "label" to "Old phone",
                        "capabilities" to listOf("publish_repo", "owner_sign"),
                        "enabled" to false,
                        "revokedAt" to 5L,
                    ),
                ),
            ),
        )

        assertEq("mobile_node", session.sessionKind)
        assertEq("dev-mobile-1", session.deviceId)
        expect(session.canOwnerSign)
        expect(session.canPublish)
        expect(!session.canHost)
        expect(session.isMobileNode)
        assertEq("dev-desktop", session.desktopDevices.single().id)
        assertEq("dev-mobile-1", session.mobileDevices.single().id)
        assertEq("iPhone 15 Pro · iOS 19.0", session.thisDevice?.label)
        assertEq(true, session.thisDevice?.current)
        assertEq(4L, session.thisDevice?.createdAt)
        assertEq("Publishes · signs as owner", session.thisDevice?.capabilitySummary)

        val revoked = session.devices.last()
        expect(revoked.isRevoked)
        assertEq(5L, revoked.revokedAt)
        assertEq("Revoked", revoked.statusLabel)
        assertEq("Mobile node", revoked.kindLabel)

        val restored = AuthSession.fromJson(session.toJson())
        assertEq("dev-mobile-1", restored.deviceId)
        assertEq("mobile_node", restored.sessionKind)
        assertEq(5L, restored.devices.last().revokedAt)
        expect(restored.devices[1].current)
        assertEq(session, restored.copy(savedAt = session.savedAt))
    }

    fun testPlainAccountSessionIsNotAMobileNode() {
        val session = AuthSession.fromJson(plainPayload())
        expect(!session.isMobileNode)
        assertEq("", session.deviceId)
        assertEq(null, session.thisDevice)
        expect(!session.toString().contains("plain-session"), "toString must not leak the token")
    }

    fun testAccountDeviceLabelsAndEnabledFlag() {
        val device = AccountDevice.fromJson(
            jsonOf("id" to "dev-mobile-1", "kind" to "mobile_node", "label" to "  ", "enabled" to 0L, "last_seen" to "9"),
        )
        expect(!device.enabled, "a numeric 0 disables the row")
        assertEq("Disabled", device.statusLabel)
        assertEq("Mobile node", device.displayName)
        assertEq("dev-mo…le-1", device.shortId)
        assertEq(9L, device.lastSeen)
        assertEq("No capabilities", device.capabilityLabel)
        assertEq("Browse only", device.capabilitySummary)
        val custom = AccountDevice.fromJson(jsonOf("id" to "short", "capabilities" to listOf("mirror_repo"), "enabled" to 1L))
        expect(custom.enabled)
        assertEq("short", custom.shortId)
        assertEq("Mirror Repo", custom.capabilityLabel)
        assertEq("Device", custom.kindLabel)
        assertEq("Browse only", custom.statusLabel)
        val restored = AccountDevice.fromJson(custom.toJson())
        assertEq(custom, restored)
    }

    fun testMessagesCanonicalAndDeviceLabels() {
        assertEq("This device was signed out from the web.", authErrorMessage("device_revoked"))
        assertEq("Could not complete the request. Please try again.", authErrorMessage("mystery"))
        assertEq("forkmesh-device-bind-v1\nalice\nPUB\n17", deviceBindCanonical("alice", "PUB", "17"))
        assertEq("Mobile device", joinDeviceLabel(" ", "", "Mobile device"))
        assertEq("Android 16", joinDeviceLabel("", "Android 16", "x"))
        assertEq(120, joinDeviceLabel("d".repeat(200), "os", "x").length)
        assertEq("Google Pixel 9 · Android 16", androidDeviceLabel("Google", "Pixel 9", "16"))
        assertEq("Samsung Galaxy S25 · Android 15", androidDeviceLabel("samsung", "Samsung Galaxy S25", "15"))
        assertEq("samsung SM-S931B · Android 15", androidDeviceLabel("samsung", "SM-S931B", "15"))
    }

    fun testNodeNameIdentifierBindsInOneCallWithAValidProof() {
        val f = Fixture()
        val transport = FakeTransport { jsonResponse(boundPayload()) }
        val auth = f.create(transport, { "iPhone 15 Pro · iOS 19.0" }, { "1760000000000" })

        val session = auth.login(identifier = " Alice ", password = "password123", totp = " 123456 ")

        assertEq(listOf("/api/accounts/login"), transport.requests.map { it.path() })
        assertEq("http://test.local/api/accounts/login", transport.requests.single().url)
        assertEq("POST", transport.requests.single().method)
        val body = transport.requests.single().bodyJson()
        assertEq("alice", body["identifier"])
        assertEq("alice", body["email"])
        assertEq("password123", body["password"])
        assertEq("123456", body["totp"])
        assertEq(f.identity.publicKeyB64url, body["pubkey"])
        assertEq("1760000000000", body["deviceTs"])
        assertEq("mobile_node", body["deviceKind"])
        assertEq("iPhone 15 Pro · iOS 19.0", body["deviceLabel"])
        expect(body["deviceSig"] is String)
        expect(!(body["deviceSig"] as String).contains('='))
        expect(verifyBind(body, "alice"), "device bind signature must verify")
        assertEq(
            listOf("identifier", "email", "password", "totp", "pubkey", "deviceTs", "deviceSig", "deviceKind", "deviceLabel"),
            body.keys.toList(),
        )

        expect(session.isMobileNode)
        assertEq("dev-mobile-1", session.deviceId)
        assertEq("mobile-session", auth.session?.sessionToken)
        assertEq("alice", f.settings.displayName)
    }

    fun testEmailIdentifierLogsInFirstThenBindsWithTheAccountName() {
        val f = Fixture()
        val transport = FakeTransport { request ->
            val body = request.bodyJson()
            when {
                request.path() == "/api/accounts/logout" -> jsonResponse(mapOf("ok" to true))
                body["pubkey"] == null -> jsonResponse(plainPayload(name = "alice"))
                else -> jsonResponse(boundPayload(name = "alice", token = "bound-session"))
            }
        }
        val auth = f.create(transport, { "Pixel 9 · Android 16" }, { "1760000000001" })

        val session = auth.login(identifier = "Alice@Example.com", password = "password123")
        val deadline = System.currentTimeMillis() + 2_000
        while (transport.requests.none { it.path() == "/api/accounts/logout" } && System.currentTimeMillis() < deadline) {
            Thread.sleep(10)
        }

        val logins = transport.requests.filter { it.path() == "/api/accounts/login" }.map { it.bodyJson() }
        assertEq(2, logins.size)
        assertEq("alice@example.com", logins[0]["identifier"])
        expect(!logins[0].containsKey("pubkey"))
        expect(!logins[0].containsKey("deviceKind"))
        assertEq("alice", logins[1]["identifier"])
        assertEq(f.identity.publicKeyB64url, logins[1]["pubkey"])
        assertEq("mobile_node", logins[1]["deviceKind"])
        assertEq("Pixel 9 · Android 16", logins[1]["deviceLabel"])
        expect(verifyBind(logins[1], "alice"))
        val logouts = transport.requests.filter { it.path() == "/api/accounts/logout" }
        assertEq("plain-session", logouts.single().bodyJson()["sessionToken"])

        assertEq("bound-session", session.sessionToken)
        expect(session.isMobileNode)
    }

    fun testDeviceKeyConflictRotatesTheSeedAndRetriesTheBindOnce() {
        val f = Fixture()
        val originalKey = f.identity.publicKeyB64url
        val pubkeys = ArrayList<String>()
        val transport = FakeTransport { request ->
            pubkeys.add(request.bodyJson()["pubkey"].toString())
            if (pubkeys.size == 1) {
                jsonResponse(mapOf("error" to "device_key_conflict"), 409)
            } else {
                jsonResponse(boundPayload())
            }
        }
        val auth = f.create(transport)

        val session = auth.login(identifier = "alice", password = "pw")

        assertEq(2, pubkeys.size)
        assertEq(originalKey, pubkeys.first())
        expect(pubkeys.last() != originalKey)
        assertEq(f.identity.publicKeyB64url, pubkeys.last())
        assertEq(1, f.identity.rotations)
        expect(session.isMobileNode)
        assertEq(null, f.secrets.read(AuthController.IDENTITY_SEED_KEY))
    }

    fun testASecondDeviceKeyConflictSurfacesAReadableError() {
        val f = Fixture()
        var attempts = 0
        val transport = FakeTransport {
            attempts++
            jsonResponse(mapOf("error" to "device_key_conflict"), 409)
        }
        val auth = f.create(transport)

        val error = assertFails<AuthException> { auth.login(identifier = "alice", password = "pw") }
        assertEq("device_key_conflict", error.code)
        expect(error.message.contains("another account"), error.message)
        assertEq(2, attempts)
        expect(!auth.isAuthenticated)
    }

    fun testDeviceProofRequiredAndInvalidDeviceKindAreReadable() {
        val f = Fixture()
        var code = "device_proof_required"
        val auth = f.create(FakeTransport { jsonResponse(mapOf("error" to code), 401) })

        val proof = assertFails<AuthException> { auth.login(identifier = "alice", password = "pw") }
        expect(proof.message.contains("device clock"), proof.message)
        code = "invalid_device_kind"
        val kind = assertFails<AuthException> { auth.login(identifier = "alice", password = "pw") }
        expect(kind.message.contains("mobile node"), kind.message)
    }

    fun testANonJsonErrorMapsToAnHttpCode() {
        val f = Fixture()
        val auth = f.create(FakeTransport { HttpClient.Response(502, emptyMap(), "bad gateway".toByteArray()) })
        val error = assertFails<AuthException> { auth.login(identifier = "alice", password = "pw") }
        assertEq("http_502", error.code)
        assertEq("Could not complete the request. Please try again.", error.message)
    }

    fun testLoginWithoutAnIdentityFailsBeforeAnyRequest() {
        val f = Fixture()
        val transport = FakeTransport { jsonResponse(boundPayload()) }
        val auth = AuthController.create(f.settings, f.prefs, f.secrets, { null }, transport)
        val error = assertFails<AuthException> { auth.login(identifier = "alice", password = "pw") }
        assertEq("identity_unavailable", error.code)
        expect(transport.requests.isEmpty())
    }

    fun testAnInvalidSessionOnADeviceBoundSessionIsTreatedAsRevocation() {
        val f = Fixture()
        f.secrets.write(AuthController.SESSION_KEY, "{}")
        val originalKey = f.identity.publicKeyB64url
        val auth = AuthController(
            f.settings, f.prefs, f.secrets, { f.identity },
            FakeTransport { jsonResponse(emptyMap<String, Any?>()) },
            AuthSession.fromJson(boundPayload()),
        )
        var notified = 0
        auth.addListener { notified++ }

        auth.handleUnauthorized(code = "invalid_session")

        expect(!auth.isAuthenticated)
        expect(auth.revokedBannerPending)
        assertEq(true, f.prefs.getBool(AuthController.REVOKED_BANNER_KEY))
        expect(f.identity.publicKeyB64url != originalKey)
        assertEq(null, f.secrets.read(AuthController.IDENTITY_SEED_KEY))
        assertEq(null, f.secrets.read(AuthController.SESSION_KEY))
        expect(notified >= 2, "session cleared and banner armed both notify")

        auth.clearRevokedBanner()
        expect(!auth.revokedBannerPending)
    }

    fun testDeviceRevokedCodeRotatesEvenWithoutADeviceId() {
        val f = Fixture()
        val auth = AuthController(
            f.settings, f.prefs, f.secrets, { f.identity },
            FakeTransport { jsonResponse(emptyMap<String, Any?>()) },
            AuthSession.fromJson(plainPayload()),
        )
        auth.handleUnauthorized(code = "device_revoked")
        expect(auth.revokedBannerPending)
        assertEq(1, f.identity.rotations)
    }

    fun testAPlainExpiredSessionOnlyClearsLocalAuth() {
        val f = Fixture()
        val originalKey = f.identity.publicKeyB64url
        val auth = AuthController(
            f.settings, f.prefs, f.secrets, { f.identity },
            FakeTransport { jsonResponse(emptyMap<String, Any?>()) },
            AuthSession.fromJson(plainPayload()),
        )

        auth.handleUnauthorized(code = "invalid_session")

        expect(!auth.isAuthenticated)
        expect(!auth.revokedBannerPending)
        assertEq(originalKey, f.identity.publicKeyB64url)
        assertEq("original-seed", f.secrets.read(AuthController.IDENTITY_SEED_KEY))
    }

    fun testASuccessfulLoginConsumesAPendingBanner() {
        val f = Fixture(mapOf(AuthController.REVOKED_BANNER_KEY to true))
        val auth = f.create(FakeTransport { jsonResponse(boundPayload()) })
        expect(auth.revokedBannerPending)
        auth.login(identifier = "alice", password = "pw")
        expect(!auth.revokedBannerPending)
    }

    fun testSessionPersistsAndPreviewSessionsAreDroppedOnLoad() {
        val f = Fixture()
        f.create(FakeTransport { jsonResponse(boundPayload()) }).login(identifier = "alice", password = "pw")
        val restored = f.create(FakeTransport { jsonResponse(emptyMap<String, Any?>()) })
        assertEq("mobile-session", restored.session?.sessionToken)

        restored.logout()
        val preview = f.create(FakeTransport { jsonResponse(emptyMap<String, Any?>()) })
        expect(preview.session == null)
        preview.authenticatePreview()
        assertEq("preview", preview.session?.sessionKind)
        assertEq(f.identity.publicKeyB64url, preview.session?.pubkey)
        expect(f.secrets.read(AuthController.SESSION_KEY) != null)
        val reloaded = f.create(FakeTransport { jsonResponse(emptyMap<String, Any?>()) })
        expect(reloaded.session == null, "a preview session is not restored")
        assertEq(null, f.secrets.read(AuthController.SESSION_KEY))
    }

    fun testLegacyPrefsSessionMigratesIntoTheSecretStore() {
        val f = Fixture()
        f.prefs.putString(AuthController.SESSION_KEY, com.forkmesh.nativeapp.json.Json.stringify(boundPayload()))
        val auth = f.create(FakeTransport { jsonResponse(emptyMap<String, Any?>()) })
        assertEq("alice", auth.session?.nodeName)
        expect(!f.prefs.contains(AuthController.SESSION_KEY))
    }

    fun testLogoutClearsLocallyEvenWhenTheWorkerIsDown() {
        val f = Fixture()
        val auth = AuthController(
            f.settings, f.prefs, f.secrets, { f.identity },
            FakeTransport { throw java.io.IOException("offline") },
            AuthSession.fromJson(boundPayload()),
        )
        auth.logout()
        expect(!auth.isAuthenticated)

        val transport = FakeTransport { jsonResponse(mapOf("ok" to true)) }
        val online = AuthController(f.settings, f.prefs, f.secrets, { f.identity }, transport, AuthSession.fromJson(boundPayload()))
        online.logout()
        assertEq("/api/accounts/logout", transport.requests.single().path())
        assertEq(mapOf("sessionToken" to "mobile-session"), transport.requests.single().bodyJson())
    }

    fun testPreviewNeverReplacesARealSession() {
        val f = Fixture()
        val auth = AuthController(
            f.settings, f.prefs, f.secrets, { f.identity },
            FakeTransport { jsonResponse(emptyMap<String, Any?>()) },
            AuthSession.fromJson(boundPayload()),
        )
        auth.authenticatePreview()
        assertEq("mobile_node", auth.session?.sessionKind)
    }

    fun testDeleteAccountValidatesThenPostsTheProfileDeletion() {
        val f = Fixture()
        val transport = FakeTransport { jsonResponse(mapOf("ok" to true)) }
        val guest = AuthController(f.settings, f.prefs, f.secrets, { f.identity }, transport)
        assertEq("not_authenticated", assertFails<AuthException> { guest.deleteAccount("pw") }.code)

        val auth = AuthController(f.settings, f.prefs, f.secrets, { f.identity }, transport, AuthSession.fromJson(boundPayload()))
        assertEq("password_required", assertFails<AuthException> { auth.deleteAccount("  ") }.code)
        expect(transport.requests.isEmpty())

        auth.deleteAccount(" secret ")
        val request = transport.requests.single()
        assertEq("/api/accounts/profile", request.path())
        assertEq(
            jsonOf(
                "nodeName" to "alice",
                "email" to "alice@example.com",
                "sessionToken" to "mobile-session",
                "password" to "secret",
                "deleteAccount" to true,
            ),
            request.bodyJson(),
        )
        expect(!auth.isAuthenticated)
    }

    fun testCheckNodeName() {
        val f = Fixture()
        val transport = FakeTransport { request ->
            when (request.path()) {
                "/api/accounts/taken" -> jsonResponse(mapOf("exists" to true, "available" to false, "name" to "taken"))
                "/api/accounts/free" -> jsonResponse(mapOf("exists" to false, "available" to true))
                else -> throw java.io.IOException("offline")
            }
        }
        val auth = AuthController(f.settings, f.prefs, f.secrets, { f.identity }, transport)

        val invalid = auth.checkNodeName("9bad")
        expect(!invalid.ok && !invalid.available)
        expect(transport.requests.isEmpty())

        val taken = auth.checkNodeName(" Taken ")
        expect(taken.ok && taken.exists && !taken.available)
        assertEq("That name is already taken.", taken.message)
        assertEq("GET", transport.requests.last().method)

        val free = auth.checkNodeName("free")
        expect(free.ok && free.available)
        assertEq("free", free.name)
        assertEq("“free” is available.", free.message)

        val offline = auth.checkNodeName("offline")
        expect(!offline.ok && offline.available)
        assertEq("Could not check availability. You can still try creating the account.", offline.message)
        assertEq("http://test.local/signup", auth.signupUrl)
    }
}
