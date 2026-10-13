package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.GitCancellationToken
import com.forkmesh.nativeapp.git.GitErrorCode
import com.forkmesh.nativeapp.git.GitException
import com.forkmesh.nativeapp.git.GitHttpRequest
import com.forkmesh.nativeapp.git.GitHttpResponse
import com.forkmesh.nativeapp.git.GitHttpTransport
import com.forkmesh.nativeapp.git.GitResourceLimits
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.net.URI

class LocalForkMeshRemoteTest {
    private val emptyBodySha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

    private class CapturingTransport : GitHttpTransport {
        var last: GitHttpRequest? = null
        var closed = 0

        override fun send(request: GitHttpRequest, cancellation: GitCancellationToken?, limits: GitResourceLimits?): GitHttpResponse {
            last = request
            return GitHttpResponse(200, mapOf("content-type" to "application/x-git-receive-pack-result"), ByteArray(0))
        }

        override fun close() {
            closed += 1
        }
    }

    private val identity = SeededIdentity()

    fun testCanonicalIsExactlyTheSixNewlineJoinedFieldsFromTheContract() {
        val canonical = forkMeshPushCanonical(
            owner = "alice",
            repo = "demo-repo",
            method = "POST",
            ts = "1750000000000",
            body = "pack".toByteArray(),
        )
        assertEq(
            "forkmesh-git-push-v1\nalice\ndemo-repo\nPOST\n1750000000000\n${forkMeshBodyDigest("pack".toByteArray())}",
            canonical,
        )
        assertEq(5, canonical.count { it == '\n' })
        expect(!canonical.endsWith("\n"))
    }

    fun testCanonicalNormalizesOwnerAndMethodTheWayEntryPyDoes() {
        assertEq(
            "forkmesh-git-push-v1\nalice\ndemo\nGET\n17\n$emptyBodySha",
            forkMeshPushCanonical(owner = "  ALICE ", repo = " demo ", method = "get", ts = "17", body = ByteArray(0)),
        )
    }

    fun testTheAdvertisementGetSignsTheEmptyBodyDigest() {
        assertEq(emptyBodySha, forkMeshBodyDigest(ByteArray(0)))
        assertEq(
            emptyBodySha,
            forkMeshPushCanonical(owner = "alice", repo = "demo", method = "GET", ts = "1", body = ByteArray(0)).split("\n").last(),
        )
    }

    fun testNormalizesAWsRelayUrlOntoTheHttpsAppOrigin() {
        val creds = ForkMeshRemoteCredentials(origin = "ws://relay.test:8787/ws", owner = "alice", repo = "demo")
        assertEq("http://relay.test:8787", creds.origin)
        assertEq("http://relay.test:8787/alice/demo", creds.remoteUrl)
        assertEq("https://forkmesh.test", ForkMeshRemoteCredentials(origin = "wss://forkmesh.test/ws", owner = "a", repo = "b").origin)
        assertEq("https://forkmesh.test", ForkMeshRemoteCredentials(origin = "https://forkmesh.test:443/", owner = "a", repo = "b").origin)
        assertEq("", ForkMeshRemoteCredentials(origin = "", owner = "a", repo = "b").origin)
    }

    fun testCanSignNeedsBothASignerAndAPubkey() {
        expect(!ForkMeshRemoteCredentials(origin = "https://f.test", owner = "a", repo = "b").canSign)
        expect(!ForkMeshRemoteCredentials(origin = "https://f.test", owner = "a", repo = "b", signer = identity).canSign)
        val signed = ForkMeshRemoteCredentials(origin = "https://f.test", owner = "a", repo = "b", signer = identity, pubkeyB64url = "pub")
        expect(signed.canSign)
        expect(!signed.anonymous.canSign)
        assertEq(signed.origin, signed.anonymous.origin)
    }

    private fun build(inner: CapturingTransport, signed: Boolean = true) = ForkMeshSigningTransport(
        credentials = ForkMeshRemoteCredentials(
            origin = "https://forkmesh.test",
            owner = "Alice",
            repo = "demo",
            signer = if (signed) identity else null,
            pubkeyB64url = if (signed) identity.publicKeyB64url else "",
        ),
        inner = inner,
        clock = { 1750000000000 },
    )

    fun testAttachesTheHeadersAndAVerifiableSignature() {
        val inner = CapturingTransport()
        val body = "0000PACK".toByteArray()
        build(inner).send(
            GitHttpRequest(
                method = "POST",
                url = URI("https://forkmesh.test/Alice/demo/git-receive-pack"),
                headers = mapOf("content-type" to "application/x-git-receive-pack"),
                body = body,
                maxResponseBytes = 1024,
                expectedContentType = "application/x-git-receive-pack-result",
            ),
        )
        val sent = inner.last!!
        assertEq(identity.publicKeyB64url, sent.headers["x-forkmesh-pubkey"])
        assertEq("1750000000000", sent.headers["x-forkmesh-ts"])
        expect(!sent.headers["x-forkmesh-sig"].isNullOrEmpty())
        assertEq(forkMeshBodyDigest(body), sent.headers["x-forkmesh-body-sha256"])
        assertEq("application/x-git-receive-pack", sent.headers["content-type"])
        expect(sent.body.contentEquals(body))
        assertEq(1024, sent.maxResponseBytes)
        assertEq("application/x-git-receive-pack-result", sent.expectedContentType)

        val canonical = "forkmesh-git-push-v1\nalice\ndemo\nPOST\n1750000000000\n${forkMeshBodyDigest(body)}"
        val signature = sent.headers["x-forkmesh-sig"]!!
        expect(SeededIdentity.verify(canonical.toByteArray(), signature, identity.publicKeyB64url))
        expect(!SeededIdentity.verify("$canonical ".toByteArray(), signature, identity.publicKeyB64url))
    }

    fun testSignsTheEmptyBodyDigestForTheAdvertisementGet() {
        val inner = CapturingTransport()
        build(inner).send(
            GitHttpRequest(
                method = "GET",
                url = URI("https://forkmesh.test/Alice/demo/info/refs?service=git-receive-pack"),
                maxResponseBytes = 1024,
                expectedContentType = "application/x-git-receive-pack-advertisement",
            ),
        )
        val signature = inner.last!!.headers["x-forkmesh-sig"]!!
        expect(
            SeededIdentity.verify(
                "forkmesh-git-push-v1\nalice\ndemo\nGET\n1750000000000\n$emptyBodySha".toByteArray(),
                signature,
                identity.publicKeyB64url,
            ),
        )
    }

    fun testAnAnonymousReadCarriesNoForkMeshHeaders() {
        val inner = CapturingTransport()
        build(inner, signed = false).send(
            GitHttpRequest(
                method = "GET",
                url = URI("https://forkmesh.test/Alice/demo/info/refs"),
                maxResponseBytes = 1024,
                expectedContentType = "application/x-git-upload-pack-advertisement",
            ),
        )
        expect(inner.last!!.headers.keys.none { it.startsWith("x-forkmesh") })
    }

    fun testCloseClosesTheInnerTransport() {
        val inner = CapturingTransport()
        build(inner).close()
        assertEq(1, inner.closed)
    }

    private fun push(code: GitErrorCode, message: String = "") = describeRemoteFailure(GitException(code, message), push = true)

    fun testRecoversTheWorkerStatusFromTheEngineMessage() {
        assertEq(413, forkMeshHttpStatus("remote returned HTTP 413"))
        assertEq(null, forkMeshHttpStatus("nothing to see"))
    }

    fun test413IsPushTooLargeAndNamesThe8MbCap() {
        val failure = push(GitErrorCode.remoteRejected, "remote returned HTTP 413")
        assertEq("push_too_large", failure.code)
        expect(failure.message.contains("8 MB"))
    }

    fun test401And403ArePushUnauthorized() {
        assertEq("push_unauthorized", push(GitErrorCode.remoteRejected, "remote returned HTTP 401").code)
        assertEq("push_unauthorized", push(GitErrorCode.remoteRejected, "remote returned HTTP 403").code)
        assertEq("push_unauthorized", push(GitErrorCode.authenticationFailed).code)
    }

    fun test503IsPushUnavailableOnAPushOfflineOnARead() {
        val pushed = push(GitErrorCode.remoteRejected, "remote returned HTTP 503")
        assertEq("push_unavailable", pushed.code)
        expect(pushed.message.contains("accept this push"))
        val read = describeRemoteFailure(GitException(GitErrorCode.remoteRejected, "remote returned HTTP 503"))
        assertEq("push_unavailable", read.code)
        expect(read.message.contains("offline"))
    }

    fun testNonFastForwardTellsTheUserToFetchAndRetry() {
        val failure = push(GitErrorCode.nonFastForward)
        assertEq("non_fast_forward", failure.code)
        assertEq("Someone else pushed first — fetch and retry.", failure.message)
    }

    fun testResourceLimitsDifferBetweenPushAndClone() {
        expect(push(GitErrorCode.resourceLimitExceeded).message.contains("8 MB"))
        expect(describeRemoteFailure(GitException(GitErrorCode.resourceLimitExceeded, "")).message.contains("too large to clone"))
    }

    fun testNotFoundNetworkTlsCancelledKeepStableCodes() {
        assertEq("not_found", push(GitErrorCode.remoteRejected, "remote returned HTTP 404").code)
        assertEq("network_unavailable", push(GitErrorCode.networkUnavailable).code)
        assertEq("tls_rejected", push(GitErrorCode.tlsRejected).code)
        assertEq("cancelled", push(GitErrorCode.cancelled).code)
        assertEq("protocol_unsupported", push(GitErrorCode.protocolUnsupported).code)
    }

    fun testANonGitErrorIsPassedThroughAsRemoteRejected() {
        val failure = describeRemoteFailure(IllegalStateException("boom"))
        assertEq("remote_rejected", failure.code)
        expect(failure.message.contains("boom"))
    }

    fun testUnscreenedObjectsFailClosedWithTheEngineMessage() {
        val failure = describeRemoteFailure(
            GitException(GitErrorCode.hashScreeningUnavailable, "untrusted SHA-1 screening is unavailable"),
        )
        assertEq("remote_rejected", failure.code)
        assertEq("untrusted SHA-1 screening is unavailable", failure.message)
    }
}
