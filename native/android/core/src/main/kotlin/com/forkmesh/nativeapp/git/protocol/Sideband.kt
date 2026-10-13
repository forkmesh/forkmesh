package com.forkmesh.nativeapp.git.protocol

import com.forkmesh.nativeapp.git.bytes.GitBytesBuilder
import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

class GitSidebandResult internal constructor(
    val data: ByteArray,
    val progress: ByteArray,
)

object GitSideband {
    fun demux(packets: Iterable<GitPktLine>, maxProgressBytes: Int): GitSidebandResult {
        val data = GitBytesBuilder()
        val progress = GitBytesBuilder()
        for (packet in packets) {
            if (packet.kind != GitPktLineKind.data || packet.payload.isEmpty()) {
                throw GitException(GitErrorCode.protocolUnsupported, "sideband packet is invalid")
            }
            val payload = packet.payload
            when (payload[0].toInt()) {
                1 -> data.add(payload, 1, payload.size - 1)
                2 -> {
                    val remaining = maxProgressBytes - progress.length
                    if (remaining > 0) {
                        val chunkLength = payload.size - 1
                        progress.add(payload, 1, if (chunkLength > remaining) remaining else chunkLength)
                    }
                }
                3 -> throw GitException(
                    GitErrorCode.remoteRejected,
                    "remote reported an error: ${redactRemoteText(payload.copyOfRange(1, payload.size))}",
                )
                else -> throw GitException(GitErrorCode.protocolUnsupported, "sideband channel is invalid")
            }
        }
        return GitSidebandResult(data.takeBytes(), progress.takeBytes())
    }
}

fun redactRemoteText(bytes: ByteArray, maxLength: Int = 200): String {
    val buffer = StringBuilder()
    for (byte in bytes) {
        val value = byte.toInt() and 0xff
        if (buffer.length >= maxLength) {
            buffer.append("...")
            break
        }
        if (value in 0x20 until 0x7f) {
            buffer.append(value.toChar())
        } else if (value == 0x0a || value == 0x09) {
            buffer.append(' ')
        }
    }
    return buffer.toString().dartTrim()
}
