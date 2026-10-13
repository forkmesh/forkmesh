package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class GitPackReaderTest {
    private fun singleBlobPack(body: ByteArray): ByteArray {
        val objectHeader = 0x30 or body.size
        return withSha1Trailer(utf8("PACK") + uint32(2) + uint32(1) + byteArrayOf(objectHeader.toByte()) + zlib(body))
    }

    private fun refDeltaPack(): ByteArray {
        val base = utf8("hello\n")
        val baseId = GitObjectId.sha1Of(GitObjectFrame.encode(GitObjectType.blob, base))
        val delta = byteArrayOf(6, 12, 0x90.toByte(), 6, 6) + utf8("world\n")
        return withSha1Trailer(
            utf8("PACK") + uint32(2) + uint32(2) +
                byteArrayOf(0x36) + zlib(base) +
                byteArrayOf((0x70 or delta.size).toByte()) + baseId.bytes + zlib(delta),
        )
    }

    private fun ofsDeltaPack(): ByteArray {
        val base = utf8("hello\n")
        val delta = byteArrayOf(6, 12, 0x90.toByte(), 6, 6) + utf8("world\n")
        val prefix = utf8("PACK") + uint32(2) + uint32(2) + byteArrayOf(0x36) + zlib(base)
        val baseDistance = prefix.size - 12
        expect(baseDistance < 128, "base distance fits one byte")
        return withSha1Trailer(
            prefix + byteArrayOf((0x60 or delta.size).toByte(), baseDistance.toByte()) + zlib(delta),
        )
    }

    fun testReadsAChecksummedVersion2PackWithABlobObject() {
        val pack = GitPackReader.parse(singleBlobPack(utf8("hello\n")), GitResourceLimits())

        assertEq(1, pack.objects.size)
        assertEq(GitObjectType.blob, pack.objects.single().type)
        assertEq("hello\n", utf8Text(pack.objects.single().body))
        assertEq("ce013625030ba8dba906f756967f9e9ca394464a", pack.objects.single().objectId.hex)
    }

    fun testRejectsAPackWhoseTrailerChecksumHasChanged() {
        val bytes = singleBlobPack(utf8("hello\n"))
        bytes[bytes.size - 1] = (bytes[bytes.size - 1].toInt() xor 1).toByte()

        assertFails<GitException> { GitPackReader.parse(bytes, GitResourceLimits()) }
    }

    fun testResolvesReferenceAndOffsetDeltasWithCheckedCopyRanges() {
        for (bytes in listOf(refDeltaPack(), ofsDeltaPack())) {
            val pack = GitPackReader.parse(bytes, GitResourceLimits())

            assertEq(2, pack.objects.size)
            assertEq("hello\nworld\n", utf8Text(pack.objects.last().body))
            assertEq(GitObjectType.blob, pack.objects.last().type)
        }
    }

    fun testFailsClosedAfterReconstructingAnUntrustedPackObjectFrame() {
        assertGitError(GitErrorCode.hashScreeningUnavailable) {
            GitPackReader.parse(refDeltaPack(), GitResourceLimits(), origin = GitSha1ObjectOrigin.untrustedTransport)
        }
    }

    fun testScreensEachReconstructedUntrustedDeltaFrameWithTheDetector() {
        val pack = GitPackReader.parse(
            refDeltaPack(),
            GitResourceLimits(),
            origin = GitSha1ObjectOrigin.untrustedTransport,
            detector = ClearDetector(),
        )

        assertEq(2, pack.objects.size)
    }
}
