package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PushBinderTest {
    private class RecordingApi : ApiService(TestSettings(), FakeTransport { error("no network") }) {
        val registered = ArrayList<Map<String, String>>()
        val unregistered = ArrayList<String>()
        var testPushes = 0
        var relayConfigured: Boolean? = true
        var testResult = PushTestResult(
            statusCode = 200, ok = true, configured = true, devices = 1, delivered = 1,
            results = listOf(PushTestDelivery(environment = "development", delivered = true, status = 200, reason = "")),
        )

        override fun registerPushDevice(deviceToken: String, environment: String): PushRegistration {
            registered.add(mapOf("token" to deviceToken, "environment" to environment))
            return PushRegistration(ok = true, statusCode = 200, relayConfigured = relayConfigured)
        }

        override fun unregisterPushDevice(deviceToken: String): Boolean {
            unregistered.add(deviceToken)
            return true
        }

        override fun sendTestPush(): PushTestResult {
            testPushes++
            return testResult
        }
    }

    private class ScriptedPlatform : PushPlatform {
        val calls = ArrayList<String>()
        var permission = "notDetermined"
        var granted = true
        var cachedToken = ""

        override fun permissionStatus(): String {
            calls.add("permissionStatus")
            return permission
        }

        override fun requestPermission(): Boolean {
            calls.add("requestPermission")
            return granted
        }

        override fun environment(): String {
            calls.add("environment")
            return "development"
        }

        override fun getToken(): String {
            calls.add("getToken")
            return cachedToken
        }
    }

    private class Fixture {
        val api = RecordingApi()
        val platform = ScriptedPlatform()

        fun build() = PushBinder(api, platform, supportedOverride = true)
    }

    fun testAGrantedPhoneRegistersItsTokenAgainstTheAccount() = with(Fixture()) {
        platform.cachedToken = "a".repeat(64)
        build().start(accountKey = "alice")
        expect("requestPermission" in platform.calls)
        assertEq(mapOf("token" to "a".repeat(64), "environment" to "development"), api.registered.single())
    }

    fun testADeniedPhoneNeverRegistersAndNeverRePrompts() = with(Fixture()) {
        platform.permission = "denied"
        val push = build()
        push.start(accountKey = "alice")
        assertEq(listOf("permissionStatus"), platform.calls)
        assertEq(0, api.registered.size)
        assertEq("denied", push.permission)
        assertEq("", push.token)
    }

    fun testDecliningTheSystemPromptRegistersNothing() = with(Fixture()) {
        platform.granted = false
        val push = build()
        push.start(accountKey = "alice")
        assertEq(0, api.registered.size)
        assertEq("denied", push.permission)
    }

    fun testARotatedTokenReRegistersOnItsOwn() = with(Fixture()) {
        platform.cachedToken = "a".repeat(64)
        val push = build()
        push.accountKeyProvider = { "alice" }
        push.start(accountKey = "alice")
        assertEq(1, api.registered.size)
        push.onToken("b".repeat(64), "production")
        assertEq(2, api.registered.size)
        assertEq(mapOf("token" to "b".repeat(64), "environment" to "production"), api.registered.last())
    }

    fun testAnUnchangedTokenIsNotRePostedOnEveryRebind() = with(Fixture()) {
        platform.cachedToken = "a".repeat(64)
        val push = build()
        push.accountKeyProvider = { "alice" }
        push.start(accountKey = "alice")
        push.rebind(accountKey = "alice")
        assertEq(1, api.registered.size)
        push.rebind(accountKey = "bob")
        assertEq(2, api.registered.size)
    }

    fun testSigningOutDropsTheRegistrationAndTheRelayState() = with(Fixture()) {
        platform.cachedToken = "a".repeat(64)
        val push = build()
        push.start(accountKey = "alice")
        expect(push.registered)
        push.stop()
        assertEq(listOf("a".repeat(64)), api.unregistered)
        expect(!push.registered)
        assertEq(null, push.relayConfigured)
    }

    fun testTappingAPushAsksTheAppToRefresh() = with(Fixture()) {
        var opened = 0
        val push = build()
        push.onOpened = { opened++ }
        push.start(accountKey = "alice")
        push.onNotificationOpened()
        assertEq(1, opened)
    }

    fun testARegistrationErrorIsSurfacedNotThrown() = with(Fixture()) {
        val push = build()
        push.start(accountKey = "alice")
        push.onError("no valid aps-environment entitlement")
        expect(push.lastError.contains("aps-environment"))

        val throwing = PushBinder(
            api,
            object : PushPlatform {
                override fun permissionStatus(): String = throw IllegalStateException("bridge missing")

                override fun requestPermission(): Boolean? = null

                override fun environment(): String? = null

                override fun getToken(): String? = null
            },
        )
        throwing.start(accountKey = "alice")
        assertEq("bridge missing", throwing.lastError)
    }

    fun testUnsupportedPlatformsStayInert() = with(Fixture()) {
        val push = PushBinder(api, platform, supportedOverride = false)
        push.start(accountKey = "alice")
        push.rebind(accountKey = "alice")
        assertEq(0, platform.calls.size)
        assertEq(0, api.registered.size)

        val android = PushBinder(api, null)
        expect(!android.supported)
        android.start(accountKey = "alice")
        android.enable()
        assertEq(0, api.registered.size)
        assertEq("unknown", android.permission)
    }

    fun testRegistrationRecordsWhereTheChainStands() = with(Fixture()) {
        platform.cachedToken = "a".repeat(64)
        val push = build()
        var notifications = 0
        push.addListener { notifications++ }
        push.start(accountKey = "alice")
        assertEq("authorized", push.permission)
        assertEq("development", push.environment)
        expect(push.registered)
        assertEq(true, push.relayConfigured)
        assertEq("", push.lastError)
        expect(notifications > 0)
    }

    fun testARelayWithoutCredentialsOrPredatingTheFlagIsReported() = with(Fixture()) {
        platform.cachedToken = "a".repeat(64)
        api.relayConfigured = false
        val without = build()
        without.start(accountKey = "alice")
        expect(without.registered)
        assertEq(false, without.relayConfigured)

        api.relayConfigured = null
        val older = build()
        older.start(accountKey = "bob")
        expect(older.registered)
        assertEq(null, older.relayConfigured)
    }

    fun testARefusedRegistrationExplainsItself() = with(Fixture()) {
        platform.cachedToken = "a".repeat(64)
        val refusing = object : ApiService(TestSettings(), FakeTransport { error("no network") }) {
            var status = 403

            override fun registerPushDevice(deviceToken: String, environment: String) = PushRegistration(ok = false, statusCode = status)
        }
        val push = PushBinder(refusing, platform, supportedOverride = true)
        push.start(accountKey = "alice")
        expect(!push.registered)
        assertEq("The relay refused the registration (HTTP 403).", push.lastError)
        refusing.status = 0
        push.rebind(accountKey = "bob")
        assertEq("The relay could not be reached to register this phone.", push.lastError)
    }

    fun testATestPushGoesThroughTheRelayAndUpdatesTheRelayState() = with(Fixture()) {
        platform.cachedToken = "a".repeat(64)
        api.relayConfigured = null
        val push = build()
        push.start(accountKey = "alice")
        assertEq(null, push.relayConfigured)
        val result = push.sendTest()
        assertEq(1, api.testPushes)
        expect(result.summary.contains("Sent to 1 of 1 device"))
        expect(result.summary.contains("APNs sandbox"))
        assertEq(true, push.relayConfigured)
        assertEq(2, run { PushBinder(api, null).sendTest(); api.testPushes })
    }

    fun testSummaryNamesTheBrokenLink() = with(Fixture()) {
        expect(PushTestResult.unreachable().summary.contains("Could not reach the relay"))
        expect(PushTestResult.fromJson(404, emptyMap()).summary.contains("does not support push tests yet"))
        expect(PushTestResult.fromJson(429, mapOf("error" to "too_many_requests")).summary.contains("Try again in a few seconds"))
        assertEq("The session has expired. Sign in again.", PushTestResult.fromJson(401, emptyMap()).summary)
        expect(PushTestResult.fromJson(500, emptyMap()).summary.contains("answered HTTP 500"))
        expect(PushTestResult.fromJson(200, mapOf("error" to "device_lookup_failed")).summary.contains("could not read this account's devices"))
        expect(
            PushTestResult.fromJson(200, mapOf("ok" to true, "configured" to false, "devices" to 1, "delivered" to 0, "results" to emptyList<Any>()))
                .summary.contains("no APNs credentials"),
        )
        expect(
            PushTestResult.fromJson(502, mapOf("ok" to false, "configured" to true, "error" to "apns_provider_key"))
                .summary.contains("APNs key is invalid"),
        )
        expect(
            PushTestResult.fromJson(200, mapOf("ok" to true, "configured" to true, "devices" to 0, "delivered" to 0, "results" to emptyList<Any>()))
                .summary.contains("not registered for pushes yet"),
        )
        val rejected = PushTestResult.fromJson(
            200,
            mapOf(
                "ok" to true, "configured" to true, "devices" to 1, "delivered" to 0,
                "results" to listOf(mapOf("environment" to "production", "delivered" to false, "status" to 400, "reason" to "BadDeviceToken")),
            ),
        ).summary
        expect(rejected.contains("Apple rejected the push (BadDeviceToken)"))
        expect(rejected.contains("other APNs environment"))
        expect(
            PushTestResult.fromJson(
                200,
                mapOf(
                    "ok" to true, "configured" to true, "devices" to 1, "delivered" to 0,
                    "results" to listOf(mapOf("environment" to "development", "delivered" to false, "status" to 0, "reason" to "")),
                ),
            ).summary.contains("could not reach Apple's gateway"),
        )
        assertEq(
            "Apple rejected the push (HTTP 410).",
            PushTestResult.fromJson(
                200,
                mapOf("devices" to 2, "results" to listOf(mapOf("delivered" to false, "status" to 410))),
            ).summary,
        )
        val production = PushTestResult.fromJson(
            200,
            mapOf("devices" to 2, "delivered" to 2, "results" to listOf(mapOf("environment" to "production", "delivered" to true, "status" to 200))),
        ).summary
        assertEq("Sent to 2 of 2 devices. A ForkMesh banner should arrive within a few seconds.", production)
    }
}
