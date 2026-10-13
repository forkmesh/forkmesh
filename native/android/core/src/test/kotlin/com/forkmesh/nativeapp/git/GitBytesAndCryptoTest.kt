package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.git.bytes.decodeLowerHex
import com.forkmesh.nativeapp.git.bytes.encodeLowerHex
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails

class GitBytesAndCryptoTest {
    fun testCheckedBytesRejectsADeclaredLengthBeyondRemainingInput() {
        val cursor = CheckedBytesReader(byteArrayOf(0, 1, 2))

        assertFails<GitException> { cursor.readBytes(4) }
    }

    fun testCheckedBytesWritesAndReadsIntegersInNetworkByteOrder() {
        val writer = CheckedBytesWriter()
        writer.writeUint32(0x5041434bL)
        val bytes = writer.takeBytes()

        assertBytes(byteArrayOf(0x50, 0x41, 0x43, 0x4b), bytes)
        assertEq(0x5041434bL, CheckedBytesReader(bytes).readUint32())
    }

    fun testCheckedBytesRejectsNoncanonicalLowercaseHexadecimal() {
        assertFails<GitException> { decodeLowerHex("CE013625") }
        assertFails<GitException> { decodeLowerHex("abc") }
        assertEq("ce013625", encodeLowerHex(decodeLowerHex("ce013625")))
    }

    fun testOwnedDigestsSha1AndSha256MatchEmptyInputFipsVectors() {
        assertEq("da39a3ee5e6b4b0d3255bfef95601890afd80709", encodeLowerHex(ForkMeshSha1().close()))
        assertEq(
            "e3b0c44298fc1c149afbf4c8996fb924" + "27ae41e4649b934ca495991b7852b855",
            encodeLowerHex(ForkMeshSha256().close()),
        )
    }

    fun testOwnedDigestsSha1MatchesFipsVectorsAcrossChunkBoundaries() {
        val digest = ForkMeshSha1()
        digest.add(utf8("a"))
        digest.add(utf8("bc"))

        assertEq("a9993e364706816aba3e25717850c26c9cd0d89d", encodeLowerHex(digest.close()))
    }

    fun testOwnedDigestsSha256MatchesFipsVectorsAcrossChunkBoundaries() {
        val digest = ForkMeshSha256()
        digest.add(utf8("a"))
        digest.add(utf8("bc"))

        assertEq("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", encodeLowerHex(digest.close()))
    }

    fun testOwnedDigestsMatchMultiBlockFipsVectors() {
        val input = utf8("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")

        assertEq("84983e441c3bd26ebaae4aa1f95129e5e54670f1", encodeLowerHex(ForkMeshSha1().also { it.add(input) }.close()))
        assertEq(
            "248d6a61d20638b8e5c026930c3e6039" + "a33ce45964ff2167f6ecedd419db06c1",
            encodeLowerHex(ForkMeshSha256().also { it.add(input) }.close()),
        )
    }

    fun testOwnedDigestsHandleOneMillionBytesAcrossBoundarySizedChunks() {
        val sha1 = ForkMeshSha1()
        val sha256 = ForkMeshSha256()
        val chunkSizes = intArrayOf(1, 55, 56, 63, 64, 65, 17)
        var written = 0
        var chunk = 0
        while (written < 1000000) {
            val candidate = chunkSizes[chunk % chunkSizes.size]
            val remaining = 1000000 - written
            val length = if (candidate < remaining) candidate else remaining
            val bytes = ByteArray(length) { 0x61 }
            sha1.add(bytes)
            sha256.add(bytes)
            written += length
            chunk += 1
        }

        assertEq("34aa973cd4c4daa4f61eeb2bdbad27316534016f", encodeLowerHex(sha1.close()))
        assertEq("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", encodeLowerHex(sha256.close()))
    }

    fun testOwnedDigestsRejectInputBeyondConfiguredLengthAccounting() {
        assertGitError(GitErrorCode.hashImplementationFailure) {
            ForkMeshSha1(maxInputBytes = 2).add(byteArrayOf(1, 2, 3))
        }
        assertGitError(GitErrorCode.hashImplementationFailure) {
            ForkMeshSha256(maxInputBytes = 2).add(byteArrayOf(1, 2, 3))
        }
    }

    fun testOwnedDigestsCollisionScreenKeepsUntrustedSha1TransportFailClosed() {
        val screen = Sha1CollisionScreen()
        val frame = GitObjectFrame.encode(GitObjectType.blob, byteArrayOf(1))

        assertEq(
            GitSha1ScreeningDecision.screeningUnavailable,
            screen.screenFrame(
                frame = frame,
                objectType = GitObjectType.blob,
                declaredBodySize = 1,
                origin = GitSha1ObjectOrigin.untrustedTransport,
                limits = GitResourceLimits(),
            ).decision,
        )
    }
}
