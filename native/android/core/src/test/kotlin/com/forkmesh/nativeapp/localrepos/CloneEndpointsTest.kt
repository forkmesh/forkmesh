package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.GitErrorCode
import com.forkmesh.nativeapp.git.GitException
import com.forkmesh.nativeapp.model.RepoMirror
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class CloneEndpointsTest {
    private val relay = ForkMeshRemoteCredentials(origin = "https://app.forkmesh.com", owner = "forkmesh", repo = "forkmesh")

    private fun mirror(
        node: String,
        url: String = "",
        endpoint: String = "",
        healthy: Boolean = true,
        latencyMs: Long = 0,
        behind: Boolean = false,
        online: Boolean = true,
        abuseBlocked: Boolean = false,
    ) = RepoMirror(
        owner = node,
        name = "forkmesh",
        online = online,
        node = node,
        mirrorUrl = url,
        endpoint = endpoint,
        integrity = if (healthy) "ok" else "stale",
        lastSyncMs = 1,
        cloneAvailable = true,
        endpointHealthy = healthy,
        endpointFresh = healthy,
        abuseBlocked = abuseBlocked,
        behind = behind,
        latencyMs = latencyMs,
    )

    fun testHealthyAndFastestMirrorsComeFirstAndTheRelayLast() {
        val plan = CloneEndpoints.plan(
            "forkmesh",
            "forkmesh",
            listOf(
                mirror("mirror36", url = "https://m36.test/forkmesh/forkmesh.git", healthy = false),
                mirror("mirror37", url = "https://m37.test/forkmesh/forkmesh.git", latencyMs = 90),
                mirror("mirror38", endpoint = "https://m38.test/", latencyMs = 20),
                mirror("mirror39", url = "https://m39.test/forkmesh/forkmesh.git", abuseBlocked = true),
                mirror("mirror40", url = "https://M37.test/forkmesh/forkmesh.git"),
                mirror("mirror41", url = "not a url"),
            ),
            relay,
        )
        assertEq(
            listOf(
                "https://m38.test/forkmesh/forkmesh.git",
                "https://m37.test/forkmesh/forkmesh.git",
                "https://m36.test/forkmesh/forkmesh.git",
                "https://app.forkmesh.com/forkmesh/forkmesh",
            ),
            plan.map { it.url },
        )
        expect(plan.last().isRelay)
        assertEq("the relay (app.forkmesh.com)", plan.last().label)
        assertEq("m38.test", plan.first().label)
    }

    fun testAMirrorEndpointClonesAnonymouslyByThePublicOwner() {
        val signed = ForkMeshRemoteCredentials(
            origin = "https://app.forkmesh.com",
            owner = "forkmesh",
            repo = "forkmesh",
            signer = SeededIdentity(),
            pubkeyB64url = "pub",
        )
        val endpoint = CloneEndpoint(url = "https://m38.test/forkmesh/forkmesh.git", node = "mirror38")
        val atMirror = endpoint.credentials(signed)
        assertEq("https://m38.test/forkmesh/forkmesh.git", atMirror.remoteUrl)
        assertEq("https://app.forkmesh.com/forkmesh/forkmesh", atMirror.relayUrl)
        assertEq("forkmesh", atMirror.owner)
        expect(!atMirror.canSign)
        assertEq("m38.test", atMirror.host)
        expect(CloneEndpoint(url = signed.relayUrl, isRelay = true).credentials(signed) === signed)
    }

    fun testFailoverTriesTheNextSourceAndReportsEveryAttempt() {
        val endpoints = listOf(
            CloneEndpoint(url = "https://m38.test/forkmesh/forkmesh.git", node = "mirror38"),
            CloneEndpoint(url = "https://app.forkmesh.com/forkmesh/forkmesh", isRelay = true),
        )
        val tried = ArrayList<String>()
        val result = CloneEndpoints.run("forkmesh/forkmesh", endpoints) { endpoint ->
            tried.add(endpoint.url)
            if (!endpoint.isRelay) throw GitWorkspaceException("Could not reach ForkMesh.", code = GitErrorCode.networkUnavailable)
            "ok"
        }
        assertEq("ok", result)
        assertEq(endpoints.map { it.url }, tried)

        val failure = assertFails<CloneFailure> {
            CloneEndpoints.run("forkmesh/forkmesh", endpoints) {
                throw GitWorkspaceException("Could not reach ForkMesh.", code = GitErrorCode.networkUnavailable)
            }
        }
        assertEq(
            "Could not clone forkmesh/forkmesh from any of its 2 sources.\n" +
                "• m38.test: Could not reach ForkMesh.\n" +
                "• the relay (app.forkmesh.com): Could not reach ForkMesh.",
            failure.message,
        )
    }

    fun testCancellationAndTheSizeGuardStopTheFailover() {
        val endpoints = listOf(
            CloneEndpoint(url = "https://m38.test/forkmesh/forkmesh.git"),
            CloneEndpoint(url = "https://app.forkmesh.com/forkmesh/forkmesh", isRelay = true),
        )
        var calls = 0
        assertFails<GitException> {
            CloneEndpoints.run("forkmesh/forkmesh", endpoints) {
                calls++
                throw GitException(GitErrorCode.resourceLimitExceeded, "too big")
            }
        }
        assertEq(1, calls)
    }
}
