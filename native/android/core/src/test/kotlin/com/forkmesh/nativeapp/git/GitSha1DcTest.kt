package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.git.crypto.ForkMeshSha1
import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetectorStatus
import com.forkmesh.nativeapp.git.crypto.GitSha1DcDetector
import com.forkmesh.nativeapp.git.crypto.Sha1Dc
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import java.util.Base64
import java.util.Random

class GitSha1DcTest {
    private val shatteredPrefix: ByteArray = Base64.getDecoder().decode(
        "JVBERi0xLjMKJeLjz9MKCgoxIDAgb2JqCjw8L1dpZHRoIDIgMCBSL0hlaWdodCAzIDAgUi9UeXBlIDQgMCBSL1N1YnR5cGUgNSAwIFIvRmlsdGVyIDYgMCBSL0NvbG9yU3BhY2UgNyAwIFIvTGVuZ3RoIDggMCBSL0JpdHNQZXJDb21wb25lbnQgOD4+CnN0cmVhbQr/2P/+ACRTSEEtMSBpcyBkZWFkISEhISGFL+wJIzl1nDmxocY8TJfh//4Bc0bckWa2fhGPApq2IbJWD/nKZ8yox/hbqEx5AwwrPeIY+G2zqQkB1d9FwU8m/t+z3DjpasIv571yjw5FvOBG0jxXD+sUE5i7VS71oKgr4zH+pIA3uLXXHw4zLt+TrDUA603cDezBqGR5DHgsdiFWYN0wl5HQa9CvP5jNpLxGKbEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP/+APwAAAAAAAAAAP/gABBKRklGAAEBAQBIAEgAAP/bAEMAAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQ==",
    )

    private fun screen(vararg chunks: ByteArray): GitSha1CollisionDetectorStatus {
        val session = GitSha1DcDetector().start()
        for (chunk in chunks) session.add(chunk)
        return session.close()
    }

    fun testTheShatteredCollisionIsDetected() {
        assertEq(640, shatteredPrefix.size)
        assertEq(GitSha1CollisionDetectorStatus.detected, screen(shatteredPrefix))
        assertEq(
            GitSha1CollisionDetectorStatus.detected,
            screen(shatteredPrefix.copyOfRange(0, 100), shatteredPrefix.copyOfRange(100, 640)),
        )
    }

    fun testOrdinaryObjectsAreClear() {
        assertEq(GitSha1CollisionDetectorStatus.clear, screen("blob 6\u0000hello\n".toByteArray()))
        assertEq(GitSha1CollisionDetectorStatus.clear, screen())
        val tampered = shatteredPrefix.copyOf()
        tampered[0] = (tampered[0].toInt() xor 1).toByte()
        assertEq(GitSha1CollisionDetectorStatus.clear, screen(tampered))
    }

    fun testDigestsMatchPlainSha1WhenNoCollisionIsFound() {
        val random = Random(7)
        for (size in listOf(0, 1, 55, 56, 63, 64, 65, 127, 128, 1000, 4096 + 13)) {
            val data = ByteArray(size).also { random.nextBytes(it) }
            val dc = Sha1Dc()
            var offset = 0
            while (offset < size) {
                val step = minOf(size - offset, 1 + random.nextInt(97))
                dc.update(data, offset, step)
                offset += step
            }
            val out = ByteArray(20)
            assertEq(false, dc.finish(out))
            val plain = ForkMeshSha1()
            plain.add(data, 0, data.size)
            assertBytes(plain.close(), out)
        }
    }

    fun testTheShatteredDigestIsTheUnsafeSha1WhenSafeHashIsOff() {
        val dc = Sha1Dc()
        dc.update(shatteredPrefix)
        val out = ByteArray(20)
        assertEq(true, dc.finish(out))
        val plain = ForkMeshSha1()
        plain.add(shatteredPrefix, 0, shatteredPrefix.size)
        assertBytes(plain.close(), out)
    }

    fun testAnUntrustedDeltaPackParsesWithTheDetector() {
        val base = utf8("hello\n")
        val baseId = GitObjectId.sha1Of(GitObjectFrame.encode(GitObjectType.blob, base))
        val delta = byteArrayOf(6, 12, 0x90.toByte(), 6, 6) + utf8("world\n")
        val bytes = withSha1Trailer(
            utf8("PACK") + uint32(2) + uint32(2) +
                byteArrayOf(0x36) + zlib(base) +
                byteArrayOf((0x70 or delta.size).toByte()) + baseId.bytes + zlib(delta),
        )
        val pack = GitPackReader.parse(
            bytes,
            GitResourceLimits(),
            origin = GitSha1ObjectOrigin.untrustedTransport,
            detector = GitSha1DcDetector(),
        )
        assertEq(2, pack.objects.size)
        assertGitError(GitErrorCode.hashScreeningUnavailable) {
            GitPackReader.parse(bytes, GitResourceLimits(), origin = GitSha1ObjectOrigin.untrustedTransport)
        }
    }

    fun testAClosedSessionRefusesMoreInput() {
        val session = GitSha1DcDetector().start()
        assertEq(GitSha1CollisionDetectorStatus.clear, session.close())
        assertFails<IllegalStateException> { session.add(byteArrayOf(1)) }
        assertEq(GitSha1CollisionDetectorStatus.failed, session.close())
        session.dispose()
    }
}
