package com.forkmesh.nativeapp.git.security

import kotlin.time.Duration
import kotlin.time.Duration.Companion.minutes
import kotlin.time.Duration.Companion.seconds

class GitResourceLimits(
    val maxObjectBytes: Int = defaultMaxObjectBytes,
    val maxInflatedBytes: Int = defaultMaxInflatedBytes,
    val maxRefNameBytes: Int = defaultMaxRefNameBytes,
    val maxRefCount: Int = defaultMaxRefCount,
    val maxPktLines: Int = defaultMaxPktLines,
    val maxAdvertisementBytes: Int = defaultMaxAdvertisementBytes,
    val maxCompressedPackBytes: Int = defaultMaxCompressedPackBytes,
    val maxResponseHeaderBytes: Int = defaultMaxResponseHeaderBytes,
    val maxSidebandBytes: Int = defaultMaxSidebandBytes,
    val connectTimeout: Duration = defaultConnectTimeout,
    val idleReadTimeout: Duration = defaultIdleReadTimeout,
    val operationDeadline: Duration = defaultOperationDeadline,
    val maxNegotiationRounds: Int = defaultMaxNegotiationRounds,
    val maxCommitWalk: Int = defaultMaxCommitWalk,
) {
    init {
        validateRange(maxObjectBytes, "maxObjectBytes", hardMaxObjectBytes)
        validateRange(maxInflatedBytes, "maxInflatedBytes", hardMaxInflatedBytes)
        validateRange(maxRefNameBytes, "maxRefNameBytes", hardMaxRefNameBytes)
        validateRange(maxRefCount, "maxRefCount", hardMaxRefCount)
        validateRange(maxPktLines, "maxPktLines", hardMaxPktLines)
        validateRange(maxAdvertisementBytes, "maxAdvertisementBytes", hardMaxAdvertisementBytes)
        validateRange(maxCompressedPackBytes, "maxCompressedPackBytes", hardMaxCompressedPackBytes)
        validateRange(maxResponseHeaderBytes, "maxResponseHeaderBytes", hardMaxResponseHeaderBytes)
        validateRange(maxSidebandBytes, "maxSidebandBytes", hardMaxSidebandBytes)
        validateDuration(connectTimeout, "connectTimeout", hardConnectTimeout)
        validateDuration(idleReadTimeout, "idleReadTimeout", hardIdleReadTimeout)
        validateDuration(operationDeadline, "operationDeadline", hardOperationDeadline)
        validateRange(maxNegotiationRounds, "maxNegotiationRounds", hardMaxNegotiationRounds)
        validateRange(maxCommitWalk, "maxCommitWalk", hardMaxCommitWalk)
    }

    companion object {
        const val defaultMaxObjectBytes = 64 * 1024 * 1024
        const val hardMaxObjectBytes = 256 * 1024 * 1024
        const val defaultMaxInflatedBytes = 768 * 1024 * 1024
        const val hardMaxInflatedBytes = 1024 * 1024 * 1024
        const val defaultMaxRefNameBytes = 255
        const val hardMaxRefNameBytes = 1024
        const val defaultMaxRefCount = 25000
        const val hardMaxRefCount = 100000
        const val defaultMaxPktLines = 50000
        const val hardMaxPktLines = 200000
        const val defaultMaxAdvertisementBytes = 1024 * 1024
        const val hardMaxAdvertisementBytes = 4 * 1024 * 1024
        const val defaultMaxCompressedPackBytes = 512 * 1024 * 1024
        const val hardMaxCompressedPackBytes = 1024 * 1024 * 1024
        const val defaultMaxResponseHeaderBytes = 64 * 1024
        const val hardMaxResponseHeaderBytes = 128 * 1024
        const val defaultMaxSidebandBytes = 1024 * 1024
        const val hardMaxSidebandBytes = 4 * 1024 * 1024
        val defaultConnectTimeout: Duration = 15.seconds
        val hardConnectTimeout: Duration = 30.seconds
        val defaultIdleReadTimeout: Duration = 30.seconds
        val hardIdleReadTimeout: Duration = 60.seconds
        val defaultOperationDeadline: Duration = 5.minutes
        val hardOperationDeadline: Duration = 30.minutes
        const val defaultMaxNegotiationRounds = 8
        const val hardMaxNegotiationRounds = 32
        const val defaultMaxCommitWalk = 250000
        const val hardMaxCommitWalk = 1000000

        private fun validateRange(value: Int, name: String, hardMaximum: Int) {
            if (value <= 0 || value > hardMaximum) {
                throw IllegalArgumentException("Invalid argument ($name): must be in 1..$hardMaximum: $value")
            }
        }

        private fun validateDuration(value: Duration, name: String, hard: Duration) {
            if (value <= Duration.ZERO || value > hard) {
                throw IllegalArgumentException("Invalid argument ($name): must be positive and <= $hard: $value")
            }
        }
    }
}
