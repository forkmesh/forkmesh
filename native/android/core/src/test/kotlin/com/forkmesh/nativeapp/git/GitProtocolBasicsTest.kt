package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class GitProtocolBasicsTest {
    private val id: GitObjectId get() = GitObjectId.parseSha1("ce013625030ba8dba906f756967f9e9ca394464a")

    private fun id(byte: Int): GitObjectId = fixtureId(byte)

    fun testPktLineEncodesDataAndDecodesDataPlusControlPktLines() {
        val bytes = GitPktLine.data(utf8("hello\n")).encode() +
            GitPktLine.flush().encode() +
            GitPktLine.delimiter().encode() +
            GitPktLine.responseEnd().encode()

        assertEq("000ahello\n", utf8Text(bytes.copyOfRange(0, 10)))
        assertEq(
            listOf(GitPktLineKind.data, GitPktLineKind.flush, GitPktLineKind.delimiter, GitPktLineKind.responseEnd),
            GitPktLine.decodeAll(bytes).map { it.kind },
        )
    }

    fun testPktLineRejectsTruncatedAndOversizedPktLinesBeforeAllocation() {
        assertFails<GitException> { GitPktLine.decodeAll(utf8("000aabc")) }
        assertFails<GitException> { GitPktLine.data(ByteArray(0xfff0) { 1 }).encode() }
    }

    fun testProtocolV2ParsesV2CapabilitiesAndBuildsAnLsRefsRequest() {
        val advertisement = GitProtocolV2Advertisement.parse(
            listOf(
                dataLine("version 2\n"),
                dataLine("agent=forkmesh-test\n"),
                dataLine("ls-refs=unborn\n"),
                dataLine("fetch=shallow wait-for-done\n"),
                GitPktLine.flush(),
            ),
        )

        expect(advertisement.supports("ls-refs"))
        expect(advertisement.supports("fetch"))
        assertEq("command=ls-refs\n", utf8Text(GitProtocolV2.lsRefsRequest().first().payload))
    }

    fun testProtocolV2ParsesTypedLsRefsRecordsAndRejectsMalformedAttributes() {
        val refs = GitProtocolV2.parseLsRefs(
            listOf(
                dataLine("${id.hex} HEAD symref-target:refs/heads/main\n"),
                dataLine("${id.hex} refs/heads/main\n"),
                GitPktLine.flush(),
            ),
        )

        assertEq(2, refs.size)
        assertEq("refs/heads/main", refs.first().symrefTarget)
        assertFails<GitException> {
            GitProtocolV2.parseLsRefs(listOf(dataLine("${id.hex} refs/heads/main unknown:value\n"), GitPktLine.flush()))
        }
    }

    fun testFetchResponseExtractsSidebandChannel1PackBytesWithoutRetainingProgress() {
        val pack = GitPackWriter.write(listOf(GitPackWriteObject(GitObjectType.blob, utf8("hello\n")))).packBytes
        val response = GitFetchResponse.parse(
            listOf(
                dataLine("packfile\n"),
                GitPktLine.data(byteArrayOf(1) + pack.copyOfRange(0, 8)),
                GitPktLine.data(byteArrayOf(1) + pack.copyOfRange(8, pack.size)),
                GitPktLine.flush(),
            ),
        )

        assertBytes(pack, response.packBytes!!)
    }

    fun testFetchResponseBlocksUntrustedFetchPacksUntilCollisionScreeningIsImplemented() {
        val pack = GitPackWriter.write(listOf(GitPackWriteObject(GitObjectType.blob, utf8("hello\n")))).packBytes

        assertGitError(GitErrorCode.hashScreeningUnavailable) {
            GitFetchResponse.validateUntrustedPack(pack, GitResourceLimits())
        }
    }

    fun testFetchResponseValidatesAnUntrustedPackOnlyThroughAClearDetector() {
        val pack = GitPackWriter.write(listOf(GitPackWriteObject(GitObjectType.blob, utf8("hello\n")))).packBytes

        GitFetchResponse.validateUntrustedPack(pack, GitResourceLimits(), detector = ClearDetector())
    }

    fun testReceivePackBuildsCompareAndSwapCommandsWithoutForce() {
        val commands = GitReceivePack.commandPackets(
            listOf(GitPushRefUpdate(refName = "refs/heads/main", expectedOld = id(1), next = id(2))),
        )

        assertEq(
            "${id(1).hex} ${id(2).hex} refs/heads/main\u0000report-status-v2 side-band-64k\n",
            utf8Text(commands.first().payload),
        )
        assertEq(GitPktLineKind.flush, commands.last().kind)
    }

    fun testReceivePackParsesPerRefRejectedStatusAsTypedNonFastForward() {
        val status = GitReceivePack.parseReportStatus(
            listOf(dataLine("unpack ok\n"), dataLine("ng refs/heads/main non-fast-forward\n"), GitPktLine.flush()),
        )

        expect(status.unpackOk)
        assertEq(GitErrorCode.nonFastForward, status.refs.single().code)
    }

    fun testSmartHttpSplitsAnUploadPackAdvertisementFromV2Capabilities() {
        val body = GitPktLine.data(utf8("# service=git-upload-pack\n")).encode() +
            GitPktLine.flush().encode() +
            GitPktLine.data(utf8("version 2\n")).encode() +
            GitPktLine.data(utf8("ls-refs\n")).encode() +
            GitPktLine.flush().encode()

        val advertisement = GitSmartHttp.parseUploadPackAdvertisement(body)

        expect(advertisement.supports("ls-refs"))
    }

    fun testSmartHttpRejectsAServiceBannerForTheWrongEndpoint() {
        val body = GitPktLine.data(utf8("# service=git-receive-pack\n")).encode() + GitPktLine.flush().encode()

        assertFails<GitException> { GitSmartHttp.parseUploadPackAdvertisement(body) }
    }
}
