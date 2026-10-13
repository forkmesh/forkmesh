package com.forkmesh.nativeapp.git.protocol

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

@Suppress("EnumEntryName")
enum class GitPktLineKind { data, flush, delimiter, responseEnd }

class GitPktLine private constructor(val kind: GitPktLineKind, val payload: ByteArray) {
    fun encode(): ByteArray {
        val length = when (kind) {
            GitPktLineKind.flush -> 0
            GitPktLineKind.delimiter -> 1
            GitPktLineKind.responseEnd -> 2
            GitPktLineKind.data -> payload.size + 4
        }
        if (length > 0xfff0) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "pkt-line exceeds hard length limit")
        }
        val output = ByteArray(4 + payload.size)
        for (index in 0 until 4) {
            output[index] = hexDigits[(length shr (12 - 4 * index)) and 0xf].code.toByte()
        }
        System.arraycopy(payload, 0, output, 4, payload.size)
        return output
    }

    companion object {
        private const val hexDigits = "0123456789abcdef"

        fun data(payload: ByteArray): GitPktLine = GitPktLine(GitPktLineKind.data, payload.copyOf())

        fun flush(): GitPktLine = GitPktLine(GitPktLineKind.flush, ByteArray(0))

        fun delimiter(): GitPktLine = GitPktLine(GitPktLineKind.delimiter, ByteArray(0))

        fun responseEnd(): GitPktLine = GitPktLine(GitPktLineKind.responseEnd, ByteArray(0))

        fun decodeAll(input: ByteArray, maximumPackets: Int = 50000): List<GitPktLine> {
            if (maximumPackets <= 0 || maximumPackets > 200000) {
                throw IllegalArgumentException("Invalid argument (maximumPackets): $maximumPackets")
            }
            val bytes = input
            val packets = ArrayList<GitPktLine>()
            var offset = 0
            while (offset < bytes.size) {
                if (bytes.size - offset < 4 || packets.size >= maximumPackets) {
                    throw GitException(
                        GitErrorCode.resourceLimitExceeded,
                        "pkt-line stream is truncated or exceeds packet limit",
                    )
                }
                val length = parseLength(bytes, offset)
                offset += 4
                when (length) {
                    0 -> packets.add(flush())
                    1 -> packets.add(delimiter())
                    2 -> packets.add(responseEnd())
                    3 -> throw GitException(GitErrorCode.protocolUnsupported, "pkt-line length 3 is invalid")
                    else -> {
                        if (length > 0xfff0 || length - 4 > bytes.size - offset) {
                            throw GitException(
                                GitErrorCode.protocolUnsupported,
                                "pkt-line payload is truncated or oversized",
                            )
                        }
                        packets.add(GitPktLine(GitPktLineKind.data, bytes.copyOfRange(offset, offset + length - 4)))
                        offset += length - 4
                    }
                }
            }
            return packets
        }

        private fun parseLength(bytes: ByteArray, offset: Int): Int {
            var length = 0
            for (index in 0 until 4) {
                val byte = bytes[offset + index].toInt() and 0xff
                val value = when (byte) {
                    in 0x30..0x39 -> byte - 0x30
                    in 0x61..0x66 -> byte - 0x61 + 10
                    in 0x41..0x46 -> byte - 0x41 + 10
                    else -> -1
                }
                if (value < 0) {
                    throw GitException(GitErrorCode.protocolUnsupported, "pkt-line prefix is not hexadecimal")
                }
                length = length * 16 + value
            }
            return length
        }
    }
}
