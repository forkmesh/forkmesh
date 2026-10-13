package com.forkmesh.nativeapp.git.protocol

import com.forkmesh.nativeapp.git.bytes.DartFormatException
import com.forkmesh.nativeapp.git.bytes.asciiDecode
import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.crypto.GitSha1ObjectOrigin
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.pack.GitPackReader
import com.forkmesh.nativeapp.git.security.GitResourceLimits

class GitFetchResponse private constructor(
    val acknowledged: List<GitObjectId>,
    val nak: Boolean,
    val ready: Boolean,
    val packBytes: ByteArray?,
    val progress: ByteArray,
    val shallow: List<GitObjectId> = emptyList(),
    val unshallow: List<GitObjectId> = emptyList(),
) {
    val hasPack: Boolean get() = packBytes != null

    companion object {
        fun parse(
            packets: List<GitPktLine>,
            maxProgressBytes: Int = GitResourceLimits.defaultMaxSidebandBytes,
        ): GitFetchResponse {
            if (packets.size < 2 || packets.last().kind != GitPktLineKind.flush) {
                throw GitException(GitErrorCode.protocolUnsupported, "fetch response framing is invalid")
            }
            var index = 0
            val acknowledged = ArrayList<GitObjectId>()
            var nak = false
            var ready = false
            var packBytes: ByteArray? = null
            var progress = ByteArray(0)
            val shallow = ArrayList<GitObjectId>()
            val unshallow = ArrayList<GitObjectId>()
            while (index < packets.size) {
                val header = packets[index]
                if (header.kind != GitPktLineKind.data) {
                    throw GitException(GitErrorCode.protocolUnsupported, "fetch response section header is invalid")
                }
                val section = sectionName(header.payload)
                index += 1
                val end = sectionEnd(packets, index)
                val body = packets.subList(index, end)
                when (section) {
                    "acknowledgments" -> {
                        for (packet in body) {
                            val line = asciiLine(packet)
                            if (line == "NAK") {
                                nak = true
                            } else if (line == "ready") {
                                ready = true
                            } else if (line.startsWith("ACK ")) {
                                acknowledged.add(GitObjectId.parseSha1(line.substring(4)))
                            } else {
                                throw GitException(GitErrorCode.protocolUnsupported, "fetch acknowledgment is invalid")
                            }
                        }
                        if (nak && acknowledged.isNotEmpty()) {
                            throw GitException(GitErrorCode.protocolUnsupported, "fetch response mixes ACK and NAK")
                        }
                    }
                    "shallow-info" -> {
                        for (packet in body) {
                            val line = shallowLine(packet)
                            if (line.startsWith("shallow ")) {
                                shallow.add(GitObjectId.parseSha1(line.substring(8)))
                            } else if (line.startsWith("unshallow ")) {
                                unshallow.add(GitObjectId.parseSha1(line.substring(10)))
                            } else {
                                throw GitException(GitErrorCode.protocolUnsupported, "fetch shallow-info line is invalid")
                            }
                        }
                    }
                    "packfile" -> {
                        if (packBytes != null) {
                            throw GitException(
                                GitErrorCode.protocolUnsupported,
                                "fetch response has two packfile sections",
                            )
                        }
                        val demuxed = GitSideband.demux(body, maxProgressBytes = maxProgressBytes)
                        packBytes = demuxed.data
                        progress = demuxed.progress
                    }
                    else -> throw GitException(GitErrorCode.protocolUnsupported, "fetch response section is unsupported")
                }
                val terminator = packets[end].kind
                index = end + 1
                if (terminator == GitPktLineKind.flush) {
                    if (index != packets.size) {
                        throw GitException(GitErrorCode.protocolUnsupported, "fetch response has data after flush")
                    }
                    break
                }
                if (section == "packfile") {
                    throw GitException(GitErrorCode.protocolUnsupported, "packfile section must be flush terminated")
                }
            }
            if (packBytes == null && acknowledged.isEmpty() && !nak && !ready) {
                throw GitException(
                    GitErrorCode.protocolUnsupported,
                    "fetch response carries neither acknowledgments nor a pack",
                )
            }
            if (packBytes != null && !ready && acknowledged.isEmpty() && !nak) {
                ready = true
            }
            return GitFetchResponse(acknowledged, nak, ready, packBytes, progress, shallow, unshallow)
        }

        fun validateUntrustedPack(
            bytes: ByteArray,
            limits: GitResourceLimits,
            detector: GitSha1CollisionDetector? = null,
        ) {
            GitPackReader.parse(bytes, limits, origin = GitSha1ObjectOrigin.untrustedTransport, detector = detector)
        }

        private fun sectionEnd(packets: List<GitPktLine>, start: Int): Int {
            for (index in start until packets.size) {
                val kind = packets[index].kind
                if (kind == GitPktLineKind.flush || kind == GitPktLineKind.delimiter) return index
                if (kind != GitPktLineKind.data) {
                    throw GitException(GitErrorCode.protocolUnsupported, "fetch response has unexpected control packet")
                }
            }
            throw GitException(GitErrorCode.protocolUnsupported, "fetch response section is not terminated")
        }

        private fun sectionName(payload: ByteArray): String {
            val text = try {
                asciiDecode(payload)
            } catch (error: DartFormatException) {
                throw GitException(GitErrorCode.protocolUnsupported, "fetch response section header is not ASCII")
            }
            if (!text.endsWith("\n") || text.length < 2) {
                throw GitException(GitErrorCode.protocolUnsupported, "fetch response section header is malformed")
            }
            return text.substring(0, text.length - 1)
        }

        private fun shallowLine(packet: GitPktLine): String {
            val text = try {
                asciiDecode(packet.payload)
            } catch (error: DartFormatException) {
                throw GitException(GitErrorCode.protocolUnsupported, "fetch shallow-info is not ASCII")
            }
            val line = if (text.endsWith("\n")) text.substring(0, text.length - 1) else text
            if (line.isEmpty()) {
                throw GitException(GitErrorCode.protocolUnsupported, "fetch shallow-info is malformed")
            }
            return line
        }

        private fun asciiLine(packet: GitPktLine): String {
            val text = try {
                asciiDecode(packet.payload)
            } catch (error: DartFormatException) {
                throw GitException(GitErrorCode.protocolUnsupported, "fetch acknowledgment is not ASCII")
            }
            if (!text.endsWith("\n") || text.length < 2) {
                throw GitException(GitErrorCode.protocolUnsupported, "fetch acknowledgment is malformed")
            }
            return text.substring(0, text.length - 1)
        }
    }
}
