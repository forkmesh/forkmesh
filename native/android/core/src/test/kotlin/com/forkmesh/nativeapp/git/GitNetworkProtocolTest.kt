package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class GitNetworkProtocolTest {
    private fun id(byte: Int): GitObjectId = fixtureId(byte)

    fun testRequestsLsRefsRequestCarriesCapabilitiesPrefixesPeelSymrefs() {
        val request = GitProtocolV2.lsRefsRequest(refPrefixes = listOf("HEAD", "refs/heads/"))
        val lines = request.filter { it.kind == GitPktLineKind.data }.map { utf8Text(it.payload) }

        assertEq(
            listOf(
                "command=ls-refs\n",
                "agent=${GitProtocolV2.agent}\n",
                "peel\n",
                "symrefs\n",
                "ref-prefix HEAD\n",
                "ref-prefix refs/heads/\n",
            ),
            lines,
        )
        assertEq(GitPktLineKind.delimiter, request[2].kind)
        assertEq(GitPktLineKind.flush, request.last().kind)
    }

    fun testRequestsFetchRequestNeverAsksForThinPacksAndCanSendDone() {
        val request = GitProtocolV2.fetchRequest(wants = listOf(id(1)), haves = listOf(id(2)), done = true)
        val lines = request.filter { it.kind == GitPktLineKind.data }.map { utf8Text(it.payload) }

        expect(lines.contains("ofs-delta\n"))
        expect(lines.contains("no-progress\n"))
        expect(lines.contains("want ${id(1).hex}\n"))
        expect(lines.contains("have ${id(2).hex}\n"))
        expect(lines.contains("done\n"))
        expect(lines.none { it.contains("thin-pack") })
    }

    fun testRequestsFetchRequestRequiresAtLeastOneWant() {
        assertFails<GitException> { GitProtocolV2.fetchRequest(wants = emptyList(), done = true) }
    }

    fun testFetchResponseParsesAnAcknowledgmentsOnlyResponse() {
        val response = GitFetchResponse.parse(GitPktLine.decodeAll(acknowledgmentsOnlyBody(listOf("ACK ${id(3).hex}"))))

        expect(!response.hasPack)
        assertEq(listOf(id(3)), response.acknowledged)
        expect(!response.nak)
    }

    fun testFetchResponseParsesAcknowledgmentsFollowedByAPackfileSection() {
        val pack = GitPackWriter.write(listOf(GitPackWriteObject(GitObjectType.blob, utf8("x\n")))).packBytes
        val response = GitFetchResponse.parse(
            GitPktLine.decodeAll(packfileBody(pack, acknowledgments = listOf("ACK ${id(4).hex}", "ready"))),
        )

        expect(response.ready)
        assertEq(listOf(id(4)), response.acknowledged)
        assertBytes(pack, response.packBytes!!)
    }

    fun testFetchResponseRejectsAResponseMixingAckAndNak() {
        assertFails<GitException> {
            GitFetchResponse.parse(GitPktLine.decodeAll(acknowledgmentsOnlyBody(listOf("ACK ${id(4).hex}", "NAK"))))
        }
    }

    fun testFetchResponseTurnsASidebandErrorChannelIntoRemoteRejected() {
        assertGitError(GitErrorCode.remoteRejected) {
            GitFetchResponse.parse(
                listOf(
                    dataLine("packfile\n"),
                    GitPktLine.data(byteArrayOf(3) + utf8("server exploded")),
                    GitPktLine.flush(),
                ),
            )
        }
    }

    fun testFetchResponseRejectsUnsupportedSectionsSuchAsShallowInfo() {
        assertFails<GitException> {
            GitFetchResponse.parse(
                listOf(dataLine("shallow-info\n"), dataLine("shallow ${id(1).hex}\n"), GitPktLine.flush()),
            )
        }
    }

    fun testReceivePackAdvertisementParsesRefsPeeledEntriesAndCapabilityTokens() {
        val advertisement = GitSmartHttp.parseReceivePackAdvertisement(
            receivePackAdvertisementBody(linkedMapOf("refs/heads/main" to id(1), "refs/tags/v1" to id(2))),
        )

        assertEq(2, advertisement.refs.size)
        assertEq(id(1), advertisement.refs["refs/heads/main"])
        expect(advertisement.supports("report-status-v2"))
        expect(advertisement.supports("delete-refs"))
        assertEq("sha1", advertisement.valueOf("object-format"))
    }

    fun testReceivePackAdvertisementParsesAnEmptyRepositoryAdvertisement() {
        val advertisement = GitSmartHttp.parseReceivePackAdvertisement(receivePackAdvertisementBody(emptyMap()))

        expect(advertisement.refs.isEmpty())
        expect(advertisement.supports("report-status"))
    }

    fun testReceivePackAdvertisementSelectsReportStatusV2AndSidebandNeverDeleteRefs() {
        val advertisement = GitSmartHttp.parseReceivePackAdvertisement(
            receivePackAdvertisementBody(mapOf("refs/heads/main" to id(1))),
        )
        val capabilities = GitReceivePack.selectCapabilities(advertisement)

        expect(capabilities.contains("report-status-v2"))
        expect(capabilities.contains("side-band-64k"))
        expect(capabilities.contains("atomic"))
        expect(!capabilities.contains("delete-refs"))
    }

    fun testReceivePackAdvertisementRequiresReportStatusSupport() {
        val advertisement = GitSmartHttp.parseReceivePackAdvertisement(
            receivePackAdvertisementBody(mapOf("refs/heads/main" to id(1)), capabilities = "side-band-64k agent=git/test"),
        )

        assertGitError(GitErrorCode.protocolUnsupported) { GitReceivePack.selectCapabilities(advertisement) }
    }

    fun testReceivePackReportParsesASidebandReportStatusV2ResponseWithOptions() {
        val status = GitReceivePack.parseResponse(
            sidebandReportBody(
                listOf(
                    "unpack ok",
                    "ok refs/heads/main",
                    "option old-oid ${id(1).hex}",
                    "option new-oid ${id(2).hex}",
                    "ng refs/heads/dev non-fast-forward",
                ),
            ),
            sideband = true,
        )

        expect(status.unpackOk)
        assertEq(2, status.refs.size)
        expect(status.refs.first().accepted)
        assertEq(GitErrorCode.nonFastForward, status.refs.last().code)
    }

    fun testReceivePackReportReportsUnpackFailureWithARedactedMessage() {
        val status = GitReceivePack.parseResponse(
            sidebandReportBody(listOf("unpack index-pack abnormal exit")),
            sideband = true,
        )

        expect(!status.unpackOk)
        assertEq("index-pack abnormal exit", status.unpackMessage)
    }

    fun testPushUpdateARefDeletionIsNotExpressible() {
        assertGitError(GitErrorCode.invalidRef) {
            GitPushRefUpdate(refName = "refs/heads/main", expectedOld = id(1), next = GitReceivePack.zeroId)
        }
        assertFails<GitException> { GitPushRefspec.parse(":refs/heads/main") }
        assertFails<GitException> { GitPushRefspec.parse("+refs/heads/main:refs/heads/main") }
    }
}
