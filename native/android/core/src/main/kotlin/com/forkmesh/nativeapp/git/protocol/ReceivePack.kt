package com.forkmesh.nativeapp.git.protocol

import com.forkmesh.nativeapp.git.bytes.DartFormatException
import com.forkmesh.nativeapp.git.bytes.asciiDecode
import com.forkmesh.nativeapp.git.bytes.asciiEncode
import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.bytes.indexOfByte
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.refs.GitRefName
import com.forkmesh.nativeapp.git.security.GitResourceLimits

class GitPushRefUpdate(val refName: String, val expectedOld: GitObjectId, val next: GitObjectId) {
    init {
        GitRefName.parse(refName)
        if (next == GitReceivePack.zeroId) {
            throw GitException(GitErrorCode.invalidRef, "ref deletion is not supported by push")
        }
    }
}

class GitPushRefStatus(val refName: String, val code: GitErrorCode?, val message: String? = null) {
    val accepted: Boolean get() = code == null
}

class GitReceivePackStatus(
    val unpackOk: Boolean,
    val refs: List<GitPushRefStatus>,
    val unpackMessage: String? = null,
)

class GitReceivePackAdvertisement private constructor(
    val refs: Map<String, GitObjectId>,
    val capabilities: Set<String>,
) {
    fun supports(name: String): Boolean =
        capabilities.contains(name) || capabilities.any { token -> token.startsWith("$name=") }

    fun valueOf(name: String): String? {
        for (token in capabilities) {
            if (token.startsWith("$name=")) return token.substring(name.length + 1)
        }
        return null
    }

    companion object {
        private val capabilityToken = Regex("[A-Za-z0-9_-]+(=[\\x21-\\x7e]*)?")

        fun parse(
            packets: List<GitPktLine>,
            maxRefCount: Int = GitResourceLimits.defaultMaxRefCount,
        ): GitReceivePackAdvertisement {
            if (packets.isEmpty() || packets.last().kind != GitPktLineKind.flush) {
                throw GitException(
                    GitErrorCode.protocolUnsupported,
                    "receive-pack advertisement is not flush terminated",
                )
            }
            val refs = LinkedHashMap<String, GitObjectId>()
            val capabilities = LinkedHashSet<String>()
            var first = true
            for (packet in packets.subList(0, packets.size - 1)) {
                if (packet.kind != GitPktLineKind.data) {
                    throw GitException(
                        GitErrorCode.protocolUnsupported,
                        "receive-pack advertisement has unexpected control packet",
                    )
                }
                val payload = packet.payload
                if (first && isVersionOne(payload)) continue
                var refPart: String
                if (first) {
                    val separator = payload.indexOfByte(0)
                    if (separator == -1) {
                        throw GitException(
                            GitErrorCode.protocolUnsupported,
                            "receive-pack advertisement lacks capabilities",
                        )
                    }
                    refPart = ascii(payload, 0, separator)
                    val capabilityText = ascii(payload, separator + 1, payload.size)
                    if (!capabilityText.endsWith("\n")) {
                        throw GitException(GitErrorCode.protocolUnsupported, "receive-pack capabilities are malformed")
                    }
                    for (token in capabilityText.dartTrim().split(" ")) {
                        if (token.isEmpty()) continue
                        if (!capabilityToken.matches(token)) {
                            throw GitException(
                                GitErrorCode.protocolUnsupported,
                                "receive-pack capability token is invalid",
                            )
                        }
                        capabilities.add(token)
                    }
                    first = false
                } else {
                    if (payload.indexOfByte(0) != -1) {
                        throw GitException(
                            GitErrorCode.protocolUnsupported,
                            "receive-pack advertisement repeats capabilities",
                        )
                    }
                    refPart = ascii(payload, 0, payload.size)
                    if (!refPart.endsWith("\n")) {
                        throw GitException(GitErrorCode.protocolUnsupported, "receive-pack ref record is malformed")
                    }
                    refPart = refPart.substring(0, refPart.length - 1)
                }
                if (refPart.length < 42 || refPart[40] != ' ') {
                    throw GitException(GitErrorCode.protocolUnsupported, "receive-pack ref record is invalid")
                }
                val objectId = GitObjectId.parseSha1(refPart.substring(0, 40))
                val name = refPart.substring(41)
                if (name == "capabilities^{}") {
                    if (objectId != GitReceivePack.zeroId || refs.isNotEmpty()) {
                        throw GitException(
                            GitErrorCode.protocolUnsupported,
                            "receive-pack empty advertisement is invalid",
                        )
                    }
                    continue
                }
                if (name.endsWith("^{}")) {
                    GitRefName.parse(name.substring(0, name.length - 3))
                    continue
                }
                if (name != "HEAD") GitRefName.parse(name)
                if (refs.size >= maxRefCount) {
                    throw GitException(
                        GitErrorCode.resourceLimitExceeded,
                        "receive-pack advertisement exceeds the ref limit",
                    )
                }
                if (refs.containsKey(name)) {
                    throw GitException(GitErrorCode.protocolUnsupported, "receive-pack advertisement repeats a ref")
                }
                refs[name] = objectId
            }
            return GitReceivePackAdvertisement(refs, capabilities)
        }

        private fun isVersionOne(payload: ByteArray): Boolean = try {
            asciiDecode(payload) == "version 1\n"
        } catch (error: DartFormatException) {
            false
        }
    }
}

object GitReceivePack {
    val zeroId: GitObjectId = GitObjectId.parseSha1("0".repeat(40))

    const val agent = "forkmesh-git/0.0.0"

    private val requestCapability = Regex("[a-z][a-z0-9-]*(=[\\x21-\\x7e]+)?")

    fun selectCapabilities(advertisement: GitReceivePackAdvertisement): List<String> {
        val objectFormat = advertisement.valueOf("object-format")
        if (objectFormat != null && objectFormat != "sha1") {
            throw GitException(GitErrorCode.unsupportedRepositoryFormat, "remote object format is unsupported")
        }
        val selected = ArrayList<String>()
        if (advertisement.supports("report-status-v2")) {
            selected.add("report-status-v2")
        } else if (advertisement.supports("report-status")) {
            selected.add("report-status")
        } else {
            throw GitException(GitErrorCode.protocolUnsupported, "remote does not support report-status")
        }
        if (advertisement.supports("side-band-64k")) selected.add("side-band-64k")
        if (advertisement.supports("quiet")) selected.add("quiet")
        if (advertisement.supports("atomic")) selected.add("atomic")
        if (objectFormat != null) selected.add("object-format=sha1")
        selected.add("agent=$agent")
        return selected
    }

    fun commandPackets(
        updates: Iterable<GitPushRefUpdate>,
        capabilities: Iterable<String> = listOf("report-status-v2", "side-band-64k"),
    ): List<GitPktLine> {
        val values = updates.toList()
        if (values.isEmpty() || values.size > 100000) {
            throw GitException(GitErrorCode.invalidRef, "push must contain a bounded non-empty set of updates")
        }
        val capabilityTokens = capabilities.toList()
        for (token in capabilityTokens) {
            if (!requestCapability.matches(token) || token == "delete-refs") {
                throw GitException(GitErrorCode.protocolUnsupported, "push capability is invalid")
            }
        }
        val names = HashSet<String>()
        val packets = ArrayList<GitPktLine>()
        for ((index, update) in values.withIndex()) {
            if (!names.add(update.refName)) {
                throw GitException(GitErrorCode.invalidRef, "duplicate push ref")
            }
            val suffix = if (index == 0) "\u0000${capabilityTokens.joinToString(" ")}" else ""
            packets.add(
                GitPktLine.data(
                    asciiEncode("${update.expectedOld.hex} ${update.next.hex} ${update.refName}$suffix\n"),
                ),
            )
        }
        packets.add(GitPktLine.flush())
        return packets
    }

    fun parseResponse(
        body: ByteArray,
        sideband: Boolean,
        maxPackets: Int = GitResourceLimits.defaultMaxPktLines,
        maxProgressBytes: Int = GitResourceLimits.defaultMaxSidebandBytes,
    ): GitReceivePackStatus {
        val packets = GitPktLine.decodeAll(body, maximumPackets = maxPackets)
        if (!sideband) return parseReportStatus(packets)
        if (packets.isEmpty() || packets.last().kind != GitPktLineKind.flush) {
            throw GitException(
                GitErrorCode.protocolUnsupported,
                "receive-pack sideband response is not flush terminated",
            )
        }
        val demuxed = GitSideband.demux(packets.subList(0, packets.size - 1), maxProgressBytes = maxProgressBytes)
        return parseReportStatus(GitPktLine.decodeAll(demuxed.data, maximumPackets = maxPackets))
    }

    fun parseReportStatus(packets: List<GitPktLine>): GitReceivePackStatus {
        if (packets.size < 2 || packets.last().kind != GitPktLineKind.flush) {
            throw GitException(GitErrorCode.protocolUnsupported, "receive-pack status framing is invalid")
        }
        val lines = ArrayList<String>()
        for (packet in packets.subList(0, packets.size - 1)) {
            if (packet.kind != GitPktLineKind.data) {
                throw GitException(
                    GitErrorCode.protocolUnsupported,
                    "receive-pack status has unexpected control packet",
                )
            }
            lines.add(line(packet.payload))
        }
        val unpackOk = lines.first() == "unpack ok"
        if (!unpackOk && !lines.first().startsWith("unpack ")) {
            throw GitException(GitErrorCode.protocolUnsupported, "receive-pack unpack status is invalid")
        }
        val unpackMessage = if (unpackOk) null else redactRemoteText(asciiEncode(lines.first().substring(7)))
        val refs = ArrayList<GitPushRefStatus>()
        val seen = HashSet<String>()
        for (line in lines.drop(1)) {
            if (line.startsWith("option ")) {
                if (refs.isEmpty() || !refs.last().accepted) {
                    throw GitException(
                        GitErrorCode.protocolUnsupported,
                        "receive-pack option line has no preceding ok status",
                    )
                }
                continue
            }
            if (line.startsWith("ok ")) {
                val ref = line.substring(3)
                GitRefName.parse(ref)
                if (!seen.add(ref)) {
                    throw GitException(GitErrorCode.protocolUnsupported, "receive-pack status repeats a ref")
                }
                refs.add(GitPushRefStatus(ref, null))
                continue
            }
            if (!line.startsWith("ng ")) {
                throw GitException(GitErrorCode.protocolUnsupported, "receive-pack ref status is invalid")
            }
            val separator = line.indexOf(' ', 3)
            if (separator == -1 || separator == line.length - 1) {
                throw GitException(GitErrorCode.protocolUnsupported, "receive-pack rejected ref status is invalid")
            }
            val ref = line.substring(3, separator)
            val message = redactRemoteText(asciiEncode(line.substring(separator + 1)))
            GitRefName.parse(ref)
            if (!seen.add(ref)) {
                throw GitException(GitErrorCode.protocolUnsupported, "receive-pack status repeats a ref")
            }
            val lowered = message.lowercase()
            refs.add(
                GitPushRefStatus(
                    ref,
                    if (lowered.contains("non-fast-forward") || lowered.contains("fetch first")) {
                        GitErrorCode.nonFastForward
                    } else {
                        GitErrorCode.remoteRejected
                    },
                    message = message,
                ),
            )
        }
        return GitReceivePackStatus(unpackOk, refs, unpackMessage = unpackMessage)
    }
}

private fun ascii(bytes: ByteArray, start: Int, end: Int): String = try {
    asciiDecode(bytes, start, end)
} catch (error: DartFormatException) {
    throw GitException(GitErrorCode.protocolUnsupported, "receive-pack advertisement is not ASCII")
}

private fun line(bytes: ByteArray): String {
    val value = try {
        asciiDecode(bytes)
    } catch (error: DartFormatException) {
        throw GitException(GitErrorCode.protocolUnsupported, "receive-pack status is not ASCII")
    }
    if (!value.endsWith("\n") || value.contains("\r") || value.length == 1) {
        throw GitException(GitErrorCode.protocolUnsupported, "receive-pack status is malformed")
    }
    return value.substring(0, value.length - 1)
}
