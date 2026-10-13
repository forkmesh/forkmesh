package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.git.bytes.encodeLowerHex
import com.forkmesh.nativeapp.git.crypto.GitSha1DcDetector
import com.forkmesh.nativeapp.git.pack.crc32
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class GitPackStoreTest {
    private fun blobId(body: ByteArray): GitObjectId = GitObjectId.sha1Of(GitObjectFrame.encode(GitObjectType.blob, body))

    private class HandPack {
        var bytes = utf8("PACK") + uint32(2) + uint32(0)
        val entries = ArrayList<GitPackIndexWriteEntry>()

        fun add(id: GitObjectId, header: ByteArray, payload: ByteArray): Int {
            val offset = bytes.size
            bytes += header + zlib(payload)
            entries.add(GitPackIndexWriteEntry(id, offset = offset.toLong(), crc32 = crc32(bytes, offset, bytes.size)))
            return offset
        }

        fun finished(): Pair<ByteArray, ByteArray> {
            val pack = bytes.copyOf()
            uint32(entries.size.toLong()).copyInto(pack, 8)
            val checksum = sha1(pack)
            return (pack + checksum) to checksum
        }
    }

    private class ChainPack(val pack: ByteArray, val index: ByteArray, val ids: List<GitObjectId>, val bodies: List<ByteArray>)

    private fun sizeHeader(type: Int, size: Int): ByteArray {
        val header = ArrayList<Byte>()
        var remaining = size shr 4
        var first = (type shl 4) or (size and 0x0f)
        if (remaining > 0) first = first or 0x80
        header.add(first.toByte())
        while (remaining > 0) {
            var byte = remaining and 0x7f
            remaining = remaining shr 7
            if (remaining > 0) byte = byte or 0x80
            header.add(byte.toByte())
        }
        return header.toByteArray()
    }

    private fun install(pack: ByteArray, index: ByteArray, gitDirectory: File) {
        val directory = File(gitDirectory, "objects/pack")
        directory.mkdirs()
        val name = "pack-" + encodeLowerHex(pack.copyOfRange(pack.size - 20, pack.size))
        File(directory, "$name.pack").writeBytes(pack)
        File(directory, "$name.idx").writeBytes(index)
    }

    private fun chainPack(): ChainPack {
        val base = utf8("hello\n")
        val middle = utf8("hello\nworld\n")
        val top = utf8("hello\nworld\nagain\n")
        val ofsDelta = byteArrayOf(6, 12, 0x90.toByte(), 6, 6) + utf8("world\n")
        val refDelta = byteArrayOf(12, 18, 0x90.toByte(), 12, 6) + utf8("again\n")
        val hand = HandPack()
        val baseOffset = hand.add(blobId(base), sizeHeader(3, base.size), base)
        val distance = hand.bytes.size - baseOffset
        expect(distance < 128, "base distance fits one byte")
        hand.add(blobId(middle), sizeHeader(6, ofsDelta.size) + byteArrayOf(distance.toByte()), ofsDelta)
        hand.add(blobId(top), sizeHeader(7, refDelta.size) + blobId(middle).bytes, refDelta)
        val (pack, checksum) = hand.finished()
        return ChainPack(
            pack,
            GitPackIndexWriter.write(hand.entries, checksum),
            listOf(blobId(base), blobId(middle), blobId(top)),
            listOf(base, middle, top),
        )
    }

    fun testReadsOffsetAndReferenceDeltaChainsOnDemand() = withTempDir("forkmesh-pack-store-") { root ->
        val git = File(root, ".git")
        val chain = chainPack()
        install(chain.pack, chain.index, git)
        val parsed = GitPackReader.parse(chain.pack, GitResourceLimits())
        val store = GitPackObjectStore(git, GitResourceLimits())
        for (position in chain.ids.indices.reversed()) {
            val id = chain.ids[position]
            expect(store.contains(id), "store contains object $position")
            val frame = store.read(id)
            assertEq(GitObjectType.blob, frame.type)
            assertBytes(chain.bodies[position], frame.body)
            assertBytes(chain.bodies[position], parsed.lookup(id)!!.body)
        }
        expect(!store.contains(fixtureId(9)), "an unknown object is absent")
        assertGitError(GitErrorCode.invalidObject) { store.read(fixtureId(9)) }
        val screened = GitPackObjectStore(git, GitResourceLimits(), detector = GitSha1DcDetector())
        assertBytes(chain.bodies[2], screened.read(chain.ids[2]).body)
    }

    fun testRejectsAnIndexThatPointsAtTheWrongObject() = withTempDir("forkmesh-pack-store-") { root ->
        val git = File(root, ".git")
        val first = utf8("first\n")
        val second = utf8("second\n")
        val hand = HandPack()
        hand.add(blobId(first), sizeHeader(3, first.size), first)
        hand.add(blobId(second), sizeHeader(3, second.size), second)
        val (pack, checksum) = hand.finished()
        val swapped = listOf(
            GitPackIndexWriteEntry(blobId(first), offset = hand.entries[1].offset, crc32 = hand.entries[1].crc32),
            GitPackIndexWriteEntry(blobId(second), offset = hand.entries[0].offset, crc32 = hand.entries[0].crc32),
        )
        install(pack, GitPackIndexWriter.write(swapped, checksum), git)

        assertGitError(GitErrorCode.objectHashMismatch) { GitPackObjectStore(git, GitResourceLimits()).read(blobId(first)) }
    }

    fun testRejectsADeltaCycleAndAMismatchedIndex() = withTempDir("forkmesh-pack-store-") { root ->
        val git = File(root, "cycle/.git")
        val loop = fixtureId(7)
        val hand = HandPack()
        val delta = byteArrayOf(1, 1, 1) + utf8("x")
        hand.add(loop, sizeHeader(7, delta.size) + loop.bytes, delta)
        val (pack, checksum) = hand.finished()
        install(pack, GitPackIndexWriter.write(hand.entries, checksum), git)
        assertGitError(GitErrorCode.packCorrupt) { GitPackObjectStore(git, GitResourceLimits()).read(loop) }

        val other = File(root, "foreign/.git")
        val chain = chainPack()
        val foreign = GitPackIndexWriter.write(
            listOf(GitPackIndexWriteEntry(chain.ids[0], offset = 12, crc32 = 0)),
            ByteArray(20) { 0xaa.toByte() },
        )
        install(chain.pack, foreign, other)
        assertGitError(GitErrorCode.packChecksumMismatch) {
            GitPackObjectStore(other, GitResourceLimits()).read(chain.ids[0])
        }
    }

    fun testFindsAPackInstalledAfterAnEarlierLookup() = withTempDir("forkmesh-pack-store-") { root ->
        val git = File(root, ".git")
        val store = GitPackObjectStore(git, GitResourceLimits())
        val chain = chainPack()
        expect(!store.contains(chain.ids[0]), "nothing is installed yet")
        install(chain.pack, chain.index, git)
        expect(store.contains(chain.ids[0]), "the new pack is found")
        assertBytes(chain.bodies[1], store.read(chain.ids[1]).body)
    }

    fun testReadsEveryObjectOfADeepGitDeltaPackLikeTheFullParser() {
        if (SystemGit.skipped("GitPackStoreTest.testReadsEveryObjectOfADeepGitDeltaPackLikeTheFullParser")) return
        withTempDir("forkmesh-pack-store-git-") { source ->
            SystemGit.git(source, "init", "--quiet")
            val lines = MutableList(120) { "line $it of a file that changes a little in every commit\n" }
            for (round in 0 until 24) {
                lines[(round * 5) % lines.size] = "round $round rewrote this line\n"
                File(source, "big.txt").writeText(lines.joinToString(""))
                File(source, "small.txt").writeText("small $round\n")
                SystemGit.git(source, "add", ".")
                SystemGit.git(
                    source,
                    "-c", "user.name=Ada", "-c", "user.email=ada@example.test",
                    "commit", "--quiet", "-m", "round $round",
                )
            }
            SystemGit.git(source, "repack", "-a", "-d", "-f", "--depth=50", "--window=50", "--quiet")
            val packDirectory = File(source, ".git/objects/pack")
            val packFile = packDirectory.listFiles()!!.first { it.name.endsWith(".pack") }
            val pack = GitPackReader.parse(packFile.readBytes(), GitResourceLimits())
            val store = GitPackObjectStore(File(source, ".git"), GitResourceLimits())
            for (item in pack.objects) {
                val frame = store.read(item.objectId)
                assertEq(item.type, frame.type)
                assertBytes(item.body, frame.body)
            }
            expect(pack.objects.size > 70, "the pack holds every round's objects")
            val verified = SystemGit.git(source, "verify-pack", "-v", packFile.path.removeSuffix(".pack") + ".idx")
            expect(verified.contains("chain length = 2"), verified)
        }
    }

    fun testScreenObjectMatchesTheFrameDigestAtEverySize() {
        for (size in listOf(0, 1, 55, 56, 63, 64, 65, 1000, 100_003)) {
            val body = ByteArray(size) { (it * 31 + 7).toByte() }
            val screened = Sha1CollisionScreen(detector = GitSha1DcDetector()).screenObject(
                type = GitObjectType.blob,
                body = body,
                origin = GitSha1ObjectOrigin.untrustedTransport,
                limits = GitResourceLimits(),
            )
            assertEq(GitSha1ScreeningDecision.approved, screened.decision, "size $size")
            assertBytes(blobId(body).bytes, screened.digest)
        }
        val unscreened = Sha1CollisionScreen().screenObject(
            type = GitObjectType.blob,
            body = utf8("x\n"),
            origin = GitSha1ObjectOrigin.untrustedTransport,
            limits = GitResourceLimits(),
        )
        assertEq(GitSha1ScreeningDecision.screeningUnavailable, unscreened.decision)
    }

    fun testPackParsingReportsResolvedObjects() {
        val chain = chainPack()
        val reported = ArrayList<Pair<Int, Int>>()
        GitPackReader.parse(chain.pack, GitResourceLimits(), progress = { resolved, total -> reported.add(resolved to total) })
        assertEq(0 to 3, reported.first())
        assertEq(3 to 3, reported.last())
    }
}
