package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq

class GitSha1ScreeningTest {
    fun testAnInjectedClearDetectorPermitsACanonicalUntrustedFrame() {
        val frame = GitObjectFrame.encode(GitObjectType.blob, byteArrayOf(1))
        val result = Sha1CollisionScreen(detector = ClearDetector()).screenFrame(
            frame = frame,
            objectType = GitObjectType.blob,
            declaredBodySize = 1,
            origin = GitSha1ObjectOrigin.untrustedTransport,
            limits = GitResourceLimits(),
        )

        assertEq(GitSha1ScreeningDecision.approved, result.decision)
    }

    fun testScreensCompleteGitFrameBytesWithTypedTrustedAndUntrustedPolicy() {
        val frame = GitObjectFrame.encode(GitObjectType.blob, utf8("hello\n"))
        val screen = Sha1CollisionScreen()

        val trusted = screen.screenFrame(
            frame = frame,
            objectType = GitObjectType.blob,
            declaredBodySize = 6,
            origin = GitSha1ObjectOrigin.trustedLocal,
            limits = GitResourceLimits(),
        )
        val untrusted = screen.screenFrame(
            frame = frame,
            objectType = GitObjectType.blob,
            declaredBodySize = 6,
            origin = GitSha1ObjectOrigin.untrustedTransport,
            limits = GitResourceLimits(),
        )

        assertEq(GitSha1ScreeningDecision.approved, trusted.decision)
        assertBytes(GitObjectId.sha1Of(frame).bytes, trusted.digest)
        assertEq(frame.size, trusted.inspectedBytes)
        assertEq(GitSha1ScreeningDecision.screeningUnavailable, untrusted.decision)
    }

    fun testReportsMalformedFramesAndCancellationAsDistinctDecisions() {
        val cancellation = GitCancellationToken()
        cancellation.cancel()
        val screen = Sha1CollisionScreen()

        val malformed = screen.screenFrame(
            frame = utf8("blob 7\u0000hello\n"),
            objectType = GitObjectType.blob,
            declaredBodySize = 7,
            origin = GitSha1ObjectOrigin.trustedLocal,
            limits = GitResourceLimits(),
        )
        val cancelled = screen.screenFrame(
            frame = GitObjectFrame.encode(GitObjectType.blob, byteArrayOf(1)),
            objectType = GitObjectType.blob,
            declaredBodySize = 1,
            origin = GitSha1ObjectOrigin.trustedLocal,
            limits = GitResourceLimits(),
            cancellation = cancellation,
        )

        assertEq(GitSha1ScreeningDecision.malformedFrame, malformed.decision)
        assertEq(GitSha1ScreeningDecision.cancelled, cancelled.decision)
    }

    fun testCancellationBetweenStreamingChunksPreventsApproval() {
        val frame = GitObjectFrame.encode(GitObjectType.blob, byteArrayOf(1, 2))
        val cancellation = GitCancellationToken()
        val session = Sha1CollisionScreen().start(
            objectType = GitObjectType.blob,
            declaredBodySize = 2,
            origin = GitSha1ObjectOrigin.trustedLocal,
            limits = GitResourceLimits(),
            cancellation = cancellation,
        )

        session.add(frame.copyOfRange(0, 4))
        cancellation.cancel()
        session.add(frame.copyOfRange(4, frame.size))

        assertEq(GitSha1ScreeningDecision.cancelled, session.close().decision)
    }

    fun testDeclaredBodiesBeyondTheObjectLimitDoNotReceiveApproval() {
        val frame = GitObjectFrame.encode(GitObjectType.blob, byteArrayOf(1, 2))

        val result = Sha1CollisionScreen().screenFrame(
            frame = frame,
            objectType = GitObjectType.blob,
            declaredBodySize = 2,
            origin = GitSha1ObjectOrigin.trustedLocal,
            limits = GitResourceLimits(maxObjectBytes = 1),
        )

        assertEq(GitSha1ScreeningDecision.resourceLimitExceeded, result.decision)
    }

    fun testPreservesTheFullFrameDecisionAcrossIrregularStreamingChunks() {
        val body = utf8("tree 0000000000000000000000000000000000000000\n\nmessage\n")
        val frame = GitObjectFrame.encode(GitObjectType.commit, body)
        val session = Sha1CollisionScreen().start(
            objectType = GitObjectType.commit,
            declaredBodySize = body.size,
            origin = GitSha1ObjectOrigin.trustedLocal,
            limits = GitResourceLimits(),
        )
        var offset = 0
        while (offset < frame.size) {
            val length = if (frame.size - offset < 5) frame.size - offset else 5
            session.add(frame.copyOfRange(offset, offset + length))
            offset += length
        }

        val result = session.close()

        assertEq(GitSha1ScreeningDecision.approved, result.decision)
        assertBytes(GitObjectId.sha1Of(frame).bytes, result.digest)
    }
}
