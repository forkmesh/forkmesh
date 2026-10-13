package com.forkmesh.nativeapp.git.protocol

import com.forkmesh.nativeapp.git.bytes.DartFormatException
import com.forkmesh.nativeapp.git.bytes.asciiDecode
import com.forkmesh.nativeapp.git.bytes.asciiEncode
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.refs.GitRefName
import com.forkmesh.nativeapp.git.security.GitResourceLimits

class GitProtocolV2Advertisement private constructor(private val capabilities: Map<String, String?>) {
    fun supports(command: String): Boolean = capabilities.containsKey(command)

    fun valueOf(capability: String): String? = capabilities[capability]

    companion object {
        private val capabilityName = Regex("[a-z][a-z0-9-]*")

        fun parse(packets: List<GitPktLine>): GitProtocolV2Advertisement {
            if (packets.isEmpty() || packets.last().kind != GitPktLineKind.flush) {
                throw GitException(
                    GitErrorCode.protocolUnsupported,
                    "protocol v2 advertisement is not flush terminated",
                )
            }
            val lines = ArrayList<String>()
            for (packet in packets.subList(0, packets.size - 1)) {
                if (packet.kind != GitPktLineKind.data) {
                    throw GitException(
                        GitErrorCode.protocolUnsupported,
                        "protocol v2 advertisement has unexpected control packet",
                    )
                }
                lines.add(asciiLine(packet.payload))
            }
            if (lines.isEmpty() || lines.first() != "version 2") {
                throw GitException(GitErrorCode.protocolUnsupported, "protocol v2 version marker is missing")
            }
            val capabilities = LinkedHashMap<String, String?>()
            for (line in lines.drop(1)) {
                val separator = line.indexOf('=')
                val name = if (separator == -1) line else line.substring(0, separator)
                if (!capabilityName.matches(name) || capabilities.containsKey(name)) {
                    throw GitException(GitErrorCode.protocolUnsupported, "protocol v2 capability is invalid")
                }
                capabilities[name] = if (separator == -1) null else line.substring(separator + 1)
            }
            return GitProtocolV2Advertisement(capabilities)
        }
    }
}

class GitRemoteRef(
    val name: String,
    val objectId: GitObjectId,
    val peeled: GitObjectId? = null,
    val symrefTarget: String? = null,
)

object GitProtocolV2 {
    const val agent = "forkmesh-git/0.0.0"

    private val refPrefixPattern = Regex("[A-Za-z0-9/_.-]{1,255}")
    private val requestCapability = Regex("[a-z][a-z0-9-]*(=[\\x21-\\x7e]+)?")

    fun lsRefsRequest(
        refPrefixes: Iterable<String> = emptyList(),
        capabilities: Iterable<String> = listOf("agent=$agent"),
    ): List<GitPktLine> {
        val prefixes = refPrefixes.toList()
        if (prefixes.size > 64) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "too many ls-refs prefixes")
        }
        for (prefix in prefixes) {
            if (!refPrefixPattern.matches(prefix) || prefix.contains("..")) {
                throw GitException(GitErrorCode.invalidRef, "ls-refs prefix is invalid")
            }
        }
        val packets = ArrayList<GitPktLine>()
        packets.add(line("command=ls-refs\n"))
        for (capability in capabilities) packets.add(line("${capability(capability)}\n"))
        packets.add(GitPktLine.delimiter())
        packets.add(line("peel\n"))
        packets.add(line("symrefs\n"))
        for (prefix in prefixes) packets.add(line("ref-prefix $prefix\n"))
        packets.add(GitPktLine.flush())
        return packets
    }

    fun fetchRequest(
        wants: Iterable<GitObjectId>,
        haves: Iterable<GitObjectId> = emptyList(),
        done: Boolean,
        ofsDelta: Boolean = true,
        noProgress: Boolean = true,
        capabilities: Iterable<String> = listOf("agent=$agent"),
        shallow: Iterable<GitObjectId> = emptyList(),
        deepen: Int? = null,
        deepenRelative: Boolean = false,
        deepenSince: Long? = null,
    ): List<GitPktLine> {
        val wantList = wants.toList()
        val haveList = haves.toList()
        if (wantList.isEmpty()) {
            throw GitException(GitErrorCode.protocolUnsupported, "fetch requires at least one want")
        }
        if (wantList.size > GitResourceLimits.hardMaxRefCount || haveList.size > GitResourceLimits.hardMaxRefCount) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "fetch request exceeds the want or have limit")
        }
        val packets = ArrayList<GitPktLine>()
        packets.add(line("command=fetch\n"))
        for (capability in capabilities) packets.add(line("${capability(capability)}\n"))
        packets.add(GitPktLine.delimiter())
        if (ofsDelta) packets.add(line("ofs-delta\n"))
        if (noProgress) packets.add(line("no-progress\n"))
        for (want in wantList) packets.add(line("want ${want.hex}\n"))
        for (id in shallow) packets.add(line("shallow ${id.hex}\n"))
        if (deepen != null) {
            if (deepen < 1) throw GitException(GitErrorCode.protocolUnsupported, "fetch deepen must be positive")
            packets.add(line("deepen $deepen\n"))
            if (deepenRelative) packets.add(line("deepen-relative\n"))
        }
        if (deepenSince != null) {
            if (deepen != null || deepenSince < 0) {
                throw GitException(GitErrorCode.protocolUnsupported, "fetch deepen-since is invalid")
            }
            packets.add(line("deepen-since $deepenSince\n"))
        }
        for (have in haveList) packets.add(line("have ${have.hex}\n"))
        if (done) packets.add(line("done\n"))
        packets.add(GitPktLine.flush())
        return packets
    }

    private fun line(text: String): GitPktLine = GitPktLine.data(asciiEncode(text))

    private fun capability(value: String): String {
        if (!requestCapability.matches(value)) {
            throw GitException(GitErrorCode.protocolUnsupported, "protocol v2 request capability is invalid")
        }
        return value
    }

    fun parseLsRefs(
        packets: List<GitPktLine>,
        maxRefCount: Int = GitResourceLimits.defaultMaxRefCount,
    ): List<GitRemoteRef> {
        if (packets.isEmpty() || packets.last().kind != GitPktLineKind.flush) {
            throw GitException(GitErrorCode.protocolUnsupported, "ls-refs response is not flush terminated")
        }
        val refs = ArrayList<GitRemoteRef>()
        val names = HashSet<String>()
        for (packet in packets.subList(0, packets.size - 1)) {
            if (packet.kind != GitPktLineKind.data) {
                throw GitException(GitErrorCode.protocolUnsupported, "ls-refs response has unexpected control packet")
            }
            if (refs.size >= maxRefCount) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "ls-refs response exceeds the ref limit")
            }
            val parts = asciiLine(packet.payload).split(" ")
            if (parts.size < 2 || parts.any { it.isEmpty() }) {
                throw GitException(GitErrorCode.protocolUnsupported, "ls-refs record is invalid")
            }
            val name = parts[1]
            if (name != "HEAD") GitRefName.parse(name)
            if (!names.add(name)) {
                throw GitException(GitErrorCode.protocolUnsupported, "ls-refs response repeats a ref")
            }
            var peeled: GitObjectId? = null
            var symrefTarget: String? = null
            for (attribute in parts.drop(2)) {
                if (attribute.startsWith("peeled:") && peeled == null) {
                    peeled = GitObjectId.parseSha1(attribute.substring(7))
                } else if (attribute.startsWith("symref-target:") && symrefTarget == null) {
                    symrefTarget = attribute.substring(14)
                    GitRefName.branch(symrefTarget)
                } else {
                    throw GitException(GitErrorCode.protocolUnsupported, "ls-refs attribute is unsupported")
                }
            }
            refs.add(
                GitRemoteRef(
                    name = name,
                    objectId = GitObjectId.parseSha1(parts.first()),
                    peeled = peeled,
                    symrefTarget = symrefTarget,
                ),
            )
        }
        return refs
    }
}

private fun asciiLine(bytes: ByteArray): String {
    val line = try {
        asciiDecode(bytes)
    } catch (error: DartFormatException) {
        throw GitException(GitErrorCode.protocolUnsupported, "protocol field is not ASCII")
    }
    if (!line.endsWith("\n") || line.contains("\r") || line.length == 1) {
        throw GitException(GitErrorCode.protocolUnsupported, "protocol field is not newline terminated")
    }
    return line.substring(0, line.length - 1)
}
