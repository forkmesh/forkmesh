package com.forkmesh.nativeapp.git.crypto

import com.forkmesh.nativeapp.git.bytes.asciiEncode
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectType
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.security.GitResourceLimits

@Suppress("EnumEntryName")
enum class GitSha1ObjectOrigin { trustedLocal, untrustedTransport }

@Suppress("EnumEntryName")
enum class GitSha1CollisionDetectorStatus { clear, detected, unavailable, failed }

interface GitSha1CollisionDetector {
    fun start(): GitSha1CollisionDetectorSession
}

interface GitSha1CollisionDetectorSession {
    fun add(bytes: ByteArray)

    fun close(): GitSha1CollisionDetectorStatus

    fun dispose()
}

@Suppress("EnumEntryName")
enum class GitSha1ScreeningDecision {
    approved,
    collisionDetected,
    malformedFrame,
    resourceLimitExceeded,
    screeningUnavailable,
    cancelled,
}

class GitSha1ScreeningResult internal constructor(
    val decision: GitSha1ScreeningDecision,
    digest: ByteArray,
    val inspectedBytes: Int,
) {
    private val digestBytes: ByteArray = digest.copyOf()

    val digest: ByteArray get() = digestBytes.copyOf()

    val approved: Boolean get() = decision == GitSha1ScreeningDecision.approved

    fun toException(): GitException = when (decision) {
        GitSha1ScreeningDecision.approved -> throw IllegalStateException("approved screening result has no error")
        GitSha1ScreeningDecision.collisionDetected -> GitException(
            GitErrorCode.sha1CollisionDetected,
            "SHA-1 collision screening rejected the object",
        )
        GitSha1ScreeningDecision.malformedFrame -> GitException(
            GitErrorCode.invalidObject,
            "object frame does not match screening metadata",
        )
        GitSha1ScreeningDecision.resourceLimitExceeded -> GitException(
            GitErrorCode.resourceLimitExceeded,
            "object frame exceeds screening limit",
        )
        GitSha1ScreeningDecision.screeningUnavailable -> GitException(
            GitErrorCode.hashScreeningUnavailable,
            "untrusted SHA-1 screening is unavailable",
        )
        GitSha1ScreeningDecision.cancelled -> GitException(
            GitErrorCode.cancelled,
            "SHA-1 screening was cancelled",
        )
    }
}

class Sha1CollisionScreen(val detector: GitSha1CollisionDetector? = null) {
    val hasGeneralizedDetector: Boolean get() = detector != null

    fun start(
        objectType: GitObjectType,
        declaredBodySize: Int,
        origin: GitSha1ObjectOrigin,
        limits: GitResourceLimits,
        cancellation: GitCancellationToken? = null,
    ): GitSha1ScreeningSession = GitSha1ScreeningSession(
        objectType = objectType,
        declaredBodySize = declaredBodySize,
        origin = origin,
        limits = limits,
        cancellation = cancellation,
        detector = detector,
    )

    fun screenFrame(
        frame: ByteArray,
        objectType: GitObjectType,
        declaredBodySize: Int,
        origin: GitSha1ObjectOrigin,
        limits: GitResourceLimits,
        cancellation: GitCancellationToken? = null,
    ): GitSha1ScreeningResult {
        val session = start(
            objectType = objectType,
            declaredBodySize = declaredBodySize,
            origin = origin,
            limits = limits,
            cancellation = cancellation,
        )
        var offset = 0
        while (offset < frame.size) {
            val end = if (offset + screeningChunkBytes < frame.size) offset + screeningChunkBytes else frame.size
            session.add(frame, offset, end - offset)
            offset = end
            if (cancellation?.isCancelled == true) break
        }
        return session.close()
    }

    fun screenObject(
        type: GitObjectType,
        body: ByteArray,
        origin: GitSha1ObjectOrigin,
        limits: GitResourceLimits,
        cancellation: GitCancellationToken? = null,
    ): GitSha1ScreeningResult {
        val session = start(
            objectType = type,
            declaredBodySize = body.size,
            origin = origin,
            limits = limits,
            cancellation = cancellation,
        )
        session.add(asciiEncode("${type.name} ${body.size}\u0000"))
        var offset = 0
        while (offset < body.size) {
            if (cancellation?.isCancelled == true) break
            val end = if (offset + screeningChunkBytes < body.size) offset + screeningChunkBytes else body.size
            session.add(body, offset, end - offset)
            offset = end
        }
        return session.close()
    }

    private companion object {
        const val screeningChunkBytes = 64 * 1024
    }
}

class GitSha1ScreeningSession internal constructor(
    val objectType: GitObjectType,
    val declaredBodySize: Int,
    val origin: GitSha1ObjectOrigin,
    val limits: GitResourceLimits,
    val cancellation: GitCancellationToken?,
    val detector: GitSha1CollisionDetector?,
) {
    private val digest = ForkMeshSha1()

    private val framePrefix = ByteArray(framePrefixBytes)
    private var detectorSession: GitSha1CollisionDetectorSession? = null
    private var inspectedBytes = 0
    private var limitExceeded = false
    private var detectorFailed = false
    private var closed = false

    init {
        try {
            detectorSession = detector?.start()
        } catch (error: Throwable) {
            detectorFailed = true
        }
    }

    fun add(bytes: ByteArray) = add(bytes, 0, bytes.size)

    fun add(bytes: ByteArray, offset: Int, length: Int) {
        check(!closed) { "screening session is closed" }
        if (cancellation?.isCancelled == true) return
        val maximumFrameBytes = limits.maxObjectBytes.toLong() + 128
        if (length > maximumFrameBytes - inspectedBytes) {
            limitExceeded = true
            return
        }
        digest.add(bytes, offset, length)
        if (inspectedBytes < framePrefixBytes) {
            val take = minOf(framePrefixBytes - inspectedBytes, length)
            System.arraycopy(bytes, offset, framePrefix, inspectedBytes, take)
        }
        val session = detectorSession
        if (session != null) {
            try {
                session.add(bytes.copyOfRange(offset, offset + length))
            } catch (error: Throwable) {
                detectorFailed = true
            }
        }
        inspectedBytes += length
    }

    fun close(): GitSha1ScreeningResult {
        check(!closed) { "screening session is closed" }
        closed = true
        try {
            if (cancellation?.isCancelled == true) {
                return GitSha1ScreeningResult(GitSha1ScreeningDecision.cancelled, ByteArray(0), inspectedBytes)
            }
            if (limitExceeded || declaredBodySize > limits.maxObjectBytes) {
                return GitSha1ScreeningResult(
                    GitSha1ScreeningDecision.resourceLimitExceeded,
                    ByteArray(0),
                    inspectedBytes,
                )
            }
            val digestBytes = digest.close()
            val header = asciiEncode("${objectType.name} $declaredBodySize\u0000")
            if (declaredBodySize < 0 ||
                inspectedBytes.toLong() != header.size.toLong() + declaredBodySize ||
                !sameBytes(header)
            ) {
                return GitSha1ScreeningResult(GitSha1ScreeningDecision.malformedFrame, digestBytes, inspectedBytes)
            }
            val detectorStatus = finishDetector()
            if (detectorStatus == GitSha1CollisionDetectorStatus.detected) {
                return GitSha1ScreeningResult(GitSha1ScreeningDecision.collisionDetected, digestBytes, inspectedBytes)
            }
            if (origin == GitSha1ObjectOrigin.untrustedTransport &&
                detectorStatus != GitSha1CollisionDetectorStatus.clear
            ) {
                return GitSha1ScreeningResult(
                    GitSha1ScreeningDecision.screeningUnavailable,
                    digestBytes,
                    inspectedBytes,
                )
            }
            return GitSha1ScreeningResult(GitSha1ScreeningDecision.approved, digestBytes, inspectedBytes)
        } finally {
            try {
                detectorSession?.dispose()
            } catch (error: Throwable) {
            }
        }
    }

    private fun finishDetector(): GitSha1CollisionDetectorStatus {
        if (detectorFailed) return GitSha1CollisionDetectorStatus.failed
        val session = detectorSession ?: return GitSha1CollisionDetectorStatus.unavailable
        return try {
            session.close()
        } catch (error: Throwable) {
            GitSha1CollisionDetectorStatus.failed
        }
    }

    private fun sameBytes(header: ByteArray): Boolean {
        if (inspectedBytes < header.size || header.size > framePrefixBytes) return false
        var different = 0
        for (index in header.indices) {
            different = different or (framePrefix[index].toInt() xor header[index].toInt())
        }
        return different == 0
    }

    private companion object {
        const val framePrefixBytes = 64
    }
}
