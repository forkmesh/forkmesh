package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.OwnerKeys
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.Base64

class FakeOwnerKeys(override val keyId: String) : OwnerKeys {
    override val publicBundle: Map<String, Any?> = linkedMapOf("v" to 1, "x25519" to "pub-x-$keyId", "mlkem768" to "pub-k-$keyId")

    fun seal(payload: Map<String, Any?>, vararg recipients: String): Map<String, Any?> = linkedMapOf(
        "v" to 1,
        "recipients" to recipients.toList(),
        "ciphertext" to Base64.getEncoder().encodeToString(Json.stringify(payload).toByteArray()),
    )

    override fun isRecipientOf(envelope: Map<String, Any?>): Boolean =
        (envelope["recipients"] as? List<*>)?.contains(keyId) == true

    override fun open(envelope: Map<String, Any?>): ByteArray? =
        if (isRecipientOf(envelope)) Base64.getDecoder().decode(envelope["ciphertext"] as String) else null

    @Suppress("UNCHECKED_CAST")
    override fun openJson(envelope: Map<String, Any?>): Map<String, Any?>? =
        open(envelope)?.let { Json.parse(String(it)) as? Map<String, Any?> }
}

class OwnerSealedAgentsTest {
    private fun sessionPayload(id: Int, status: String) = mapOf(
        "kind" to "forkmesh.agent-session",
        "id" to id,
        "issueNumber" to 7,
        "issueTitle" to "Stabilize signed inbox",
        "status" to status,
        "provider" to "claude-code",
        "model" to "claude-sonnet-5",
        "branchName" to "agent/issue-7",
        "numTurns" to 4,
        "costUsd" to 0.42,
        "transcript" to "private transcript tail",
    )

    private fun listing(vararg envelopes: Map<String, Any?>, extra: Map<String, Any?> = emptyMap()) = FakeTransport { request ->
        assertEq("/api/repo/alice/proj/agents/list", request.path())
        jsonResponse(
            linkedMapOf<String, Any?>(
                "ok" to true,
                "agents" to emptyList<Any>(),
                "encryptedAgents" to envelopes.mapIndexed { i, envelope -> mapOf("id" to i, "envelope" to envelope) },
                "privacyBoundary" to "owner-only-e2ee",
            ).apply { putAll(extra) },
        )
    }

    fun testSessionsSealedToThisDeviceAreOpenedAndListed() {
        val owner = FakeOwnerKeys("device-a")
        val api = ApiService(TestSettings(), listing(owner.seal(sessionPayload(42, "running"), "device-a")))
        api.ownerKeys = owner
        val page = api.agentSessionPage("alice", "proj")
        assertEq(0, page.sealedForOthers)
        assertEq(42, page.sessions.single().id)
        assertEq("Stabilize signed inbox", page.sessions.single().issueTitle)
        assertEq("Running", page.sessions.single().statusLabel)
    }

    fun testSessionsSealedToAnotherDeviceAreCountedNeverDroppedSilently() {
        val other = FakeOwnerKeys("device-b")
        val api = ApiService(TestSettings(), listing(other.seal(sessionPayload(7, "running"), "device-b")))
        api.ownerKeys = FakeOwnerKeys("device-a")
        val page = api.agentSessionPage("alice", "proj")
        assertEq(0, page.sessions.size)
        assertEq(1, page.sealedForOthers)
    }

    fun testADeviceWithNoOwnerIdentityReportsSealedRowsRatherThanNone() {
        val owner = FakeOwnerKeys("device-a")
        val api = ApiService(TestSettings(), listing(owner.seal(sessionPayload(42, "running"), "device-a")))
        val page = api.agentSessionPage("alice", "proj")
        assertEq(0, page.sessions.size)
        assertEq(1, page.sealedForOthers)
    }

    fun testLegacyRowsHeldForOwnerMigrationCountAsSealed() {
        val owner = FakeOwnerKeys("device-a")
        val transport = listing(
            owner.seal(sessionPayload(1, "done"), "device-a"),
            extra = mapOf("legacyRowsHeldForOwnerMigration" to 2, "agents" to listOf(sessionPayload(9, "queued"))),
        )
        val api = ApiService(TestSettings(), transport)
        api.ownerKeysProvider = { owner }
        val page = api.agentSessionPage("alice", "proj")
        assertEq(listOf(9, 1), page.sessions.map { it.id })
        assertEq(2, page.sealedForOthers)
    }

    fun testAnEncryptedTranscriptIsOpenedOrFlaggedWhenItIsNotOurs() {
        val owner = FakeOwnerKeys("device-a")
        val mine = owner.seal(mapOf("status" to "success", "transcript" to "the agent edited three files"), "device-a")
        val transport = FakeTransport {
            jsonResponse(mapOf("ok" to true, "encrypted" to true, "envelope" to mine, "serverCanDecrypt" to false))
        }
        val api = ApiService(TestSettings(), transport)
        api.ownerKeys = owner
        val opened = api.agentTranscript("alice", "proj", 42)
        assertEq("the agent edited three files", opened.transcript)
        assertEq("success", opened.status)
        expect(!opened.sealedForOtherDevice)

        api.ownerKeys = FakeOwnerKeys("stranger")
        val sealed = api.agentTranscript("alice", "proj", 42)
        expect(sealed.sealedForOtherDevice)
        assertEq("", sealed.transcript)

        api.ownerKeys = null
        expect(api.agentTranscript("alice", "proj", 42).sealedForOtherDevice)
    }

    fun testEnrollmentPostsThePublicBundleOnlyAndVerifiesTheReply() {
        val owner = FakeOwnerKeys("key-id-a")
        val transport = FakeTransport { request ->
            assertEq("/api/security/owner-keys", request.path())
            jsonResponse(mapOf("ok" to true, "keyId" to owner.keyId, "custody" to "owner-device-only", "privateKeysStored" to false), 201)
        }
        val api = ApiService(TestSettings(), transport)
        assertEq(owner.keyId, api.registerOwnerKey(owner))
        val body = transport.requests.single().bodyJson()
        @Suppress("UNCHECKED_CAST")
        val bundle = body["publicBundle"] as Map<String, Any?>
        assertEq(setOf("v", "x25519", "mlkem768"), bundle.keys)
        val encoded = Json.stringify(body).lowercase()
        for (suspicious in listOf("priv", "secret", "seed")) expect(!encoded.contains(suspicious), suspicious)
    }

    fun testAnEnrollmentTheRelayDescribesDifferentlyIsRefused() {
        val owner = FakeOwnerKeys("key-id-a")
        val wrongKey = ApiService(
            TestSettings(),
            FakeTransport { jsonResponse(mapOf("ok" to true, "keyId" to "some-other-device-key-id-value", "privateKeysStored" to false), 201) },
        )
        assertEq("", wrongKey.registerOwnerKey(owner))
        val custodial = ApiService(
            TestSettings(),
            FakeTransport { jsonResponse(mapOf("ok" to true, "keyId" to owner.keyId, "privateKeysStored" to true), 201) },
        )
        assertEq("", custodial.registerOwnerKey(owner))
        assertEq("", ApiService(TestSettings(), FakeTransport { textResponse("", 500) }).registerOwnerKey(owner))
    }

    fun testOwnerEnrollmentFollowsTheSession() {
        val owner = FakeOwnerKeys("key-id-a")
        var ok = true
        val transport = FakeTransport {
            if (ok) jsonResponse(mapOf("ok" to true, "keyId" to owner.keyId), 201) else textResponse("", 503)
        }
        val api = ApiService(TestSettings(), transport)
        var token = ""
        api.sessionTokenProvider = { token }

        expect(!api.syncOwnerEnrollment())
        api.ownerKeysProvider = { owner }
        expect(!api.syncOwnerEnrollment())
        assertEq(0, transport.requests.size)

        token = "session-1"
        expect(api.syncOwnerEnrollment())
        expect(api.syncOwnerEnrollment())
        assertEq(1, transport.requests.size)
        assertEq("Bearer session-1", transport.requests.single().headerValue("Authorization"))

        token = "session-2"
        ok = false
        expect(!api.syncOwnerEnrollment())
        ok = true
        expect(api.syncOwnerEnrollment())
        assertEq(3, transport.requests.size)

        token = ""
        expect(!api.syncOwnerEnrollment())
        token = "session-2"
        expect(api.syncOwnerEnrollment())
        assertEq(4, transport.requests.size)
    }
}
