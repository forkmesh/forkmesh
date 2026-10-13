package com.forkmesh.nativeapp.git.protocol

import com.forkmesh.nativeapp.git.bytes.DartFormatException
import com.forkmesh.nativeapp.git.bytes.asciiDecode
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitResourceLimits

class GitSmartHttpService private constructor(val name: String) {
    val advertisementContentType: String get() = "application/x-$name-advertisement"
    val requestContentType: String get() = "application/x-$name-request"
    val resultContentType: String get() = "application/x-$name-result"

    override fun toString(): String = name

    companion object {
        val uploadPack = GitSmartHttpService("git-upload-pack")
        val receivePack = GitSmartHttpService("git-receive-pack")
    }
}

object GitSmartHttp {
    fun parseUploadPackAdvertisement(
        body: ByteArray,
        maxPackets: Int = GitResourceLimits.defaultMaxPktLines,
    ): GitProtocolV2Advertisement {
        val packets = GitPktLine.decodeAll(body, maximumPackets = maxPackets)
        if (packets.size >= 2 &&
            packets.first().kind == GitPktLineKind.data &&
            matchesService(packets.first().payload, GitSmartHttpService.uploadPack.name) &&
            packets[1].kind == GitPktLineKind.flush
        ) {
            return GitProtocolV2Advertisement.parse(packets.subList(2, packets.size))
        }
        return GitProtocolV2Advertisement.parse(packets)
    }

    fun parseReceivePackAdvertisement(
        body: ByteArray,
        maxPackets: Int = GitResourceLimits.defaultMaxPktLines,
        maxRefCount: Int = GitResourceLimits.defaultMaxRefCount,
    ): GitReceivePackAdvertisement {
        val packets = bannerPackets(body, GitSmartHttpService.receivePack, maxPackets)
        return GitReceivePackAdvertisement.parse(packets, maxRefCount = maxRefCount)
    }

    private fun bannerPackets(body: ByteArray, service: GitSmartHttpService, maxPackets: Int): List<GitPktLine> {
        val packets = GitPktLine.decodeAll(body, maximumPackets = maxPackets)
        if (packets.size < 3 ||
            packets.first().kind != GitPktLineKind.data ||
            !matchesService(packets.first().payload, service.name) ||
            packets[1].kind != GitPktLineKind.flush
        ) {
            throw GitException(GitErrorCode.protocolUnsupported, "${service.name} advertisement banner is invalid")
        }
        return packets.subList(2, packets.size)
    }

    private fun matchesService(payload: ByteArray, service: String): Boolean = try {
        asciiDecode(payload) == "# service=$service\n"
    } catch (error: DartFormatException) {
        false
    }
}
