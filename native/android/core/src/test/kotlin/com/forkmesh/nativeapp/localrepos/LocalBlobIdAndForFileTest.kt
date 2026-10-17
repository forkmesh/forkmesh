package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class LocalBlobIdAndForFileTest {
    private fun diff(path: String, before: String, after: String): PatchBuildResult {
        val beforeBytes = before.toByteArray(Charsets.UTF_8)
        return PatchBuilder.forFile(path, beforeBytes, after, gitBlobId(beforeBytes), gitBlobId(after.toByteArray(Charsets.UTF_8)))
    }

    private fun repoWith(temp: File, name: String, files: Map<String, String>): File {
        val dir = File(temp, name).apply { mkdirs() }
        HostGit.run(dir, "init", "-q")
        for ((path, text) in files) File(dir, path).writeText(text)
        HostGit.run(dir, "add", "-A")
        HostGit.run(dir, "-c", "user.email=a@b.c", "-c", "user.name=A", "commit", "-q", "-m", "base")
        return dir
    }

    fun testBlobIdMatchesGitHashObject() {
        assertEq("ce013625030ba8dba906f756967f9e9ca394464a", gitBlobId("hello\n".toByteArray()).hex)
        assertEq("e69de29bb2d1d6434b8b29ae775ad8c2e48c5391", gitBlobId(ByteArray(0)).hex)
        assertEq("de980441c3ab03a8c07dda1ad27b8a11f39deb1e", gitBlobId("a\nb\nc\n".toByteArray()).hex)
        assertEq("69db55d99f68896760e56c209fbd5823dae98e66", gitBlobId("no trailing newline".toByteArray()).hex)
        assertEq(
            gitBlobId(byteArrayOf(0xc3.toByte(), 0xa9.toByte(), 0xe2.toByte(), 0x82.toByte(), 0xac.toByte())).hex,
            gitBlobId("é€".toByteArray(Charsets.UTF_8)).hex,
        )
    }

    fun testServedBlobIdAgreement() {
        val id = gitBlobId("hello\n".toByteArray())
        expect(servedBlobIdAgrees(id.hex, id))
        expect(servedBlobIdAgrees(id.hex.uppercase(), id))
        expect(servedBlobIdAgrees("  ${id.hex}  ", id))
        expect(servedBlobIdAgrees("", id))
        expect(servedBlobIdAgrees("not-a-sha", id))
        expect(servedBlobIdAgrees("abc123", id))
        expect(!servedBlobIdAgrees("e69de29bb2d1d6434b8b29ae775ad8c2e48c5391", id))
    }

    fun testForFileAppliesWithRealGit() = withScratch("fm-forfile-") { temp ->
        if (!HostGit.available) return@withScratch
        val before = "one\ntwo\nthree\n"
        val after = "one\nTWO\nthree\nfour\n"
        val result = diff("README.md", before, after)
        assertEq(listOf("README.md"), result.changedPaths)
        val dir = repoWith(temp, "apply", mapOf("README.md" to before))
        File(dir, "change.patch").writeText(result.patch)
        assertEq(0, HostGit.run(dir, "apply", "change.patch").first)
        assertEq(after, File(dir, "README.md").readText())
    }

    fun testForFileKeepsTheExecutableBit() = withScratch("fm-forfile-") { temp ->
        if (!HostGit.available) return@withScratch
        val before = "#!/bin/sh\necho hi\n"
        val result = diff("run.sh", before, "#!/bin/sh\necho bye\n")
        expect(!result.patch.contains("100644"))
        expect(!result.patch.contains("old mode"))
        val dir = repoWith(temp, "exec", mapOf("run.sh" to before))
        File(dir, "run.sh").setExecutable(true)
        HostGit.run(dir, "add", "-A")
        HostGit.run(dir, "-c", "user.email=a@b.c", "-c", "user.name=A", "commit", "-q", "-m", "exec")
        File(dir, "change.patch").writeText(result.patch)
        val applied = HostGit.run(dir, "apply", "--index", "--3way", "change.patch")
        assertEq(0, applied.first, applied.second)
        expect(HostGit.run(dir, "ls-files", "-s", "run.sh").second.trim().startsWith("100755"))
    }

    fun testForFileEdgeCases() = withScratch("fm-forfile-") { temp ->
        val same = diff("a.txt", "same\n", "same\n")
        expect(same.isEmpty)
        expect(same.changedPaths.isEmpty())
        val newline = diff("a.txt", "x", "x\n")
        expect(!newline.isEmpty)
        expect(newline.patch.contains("\\ No newline at end of file"))
        if (HostGit.available) {
            val dir = repoWith(temp, "newline", mapOf("a.txt" to "x"))
            File(dir, "change.patch").writeText(newline.patch)
            assertEq(0, HostGit.run(dir, "apply", "--check", "change.patch").first)
        }
        val binaryBytes = byteArrayOf(0x89.toByte(), 0x50, 0x00, 0x01)
        val binary = PatchBuilder.forFile("logo.png", binaryBytes, "text", gitBlobId(binaryBytes), gitBlobId("text".toByteArray()))
        expect(binary.hasBinaryChanges)
        assertEq(listOf("logo.png"), binary.binaryPaths)
        assertEq("", binary.patch)
        val latin = byteArrayOf(0x68, 0xff.toByte(), 0x0a)
        val refused = PatchBuilder.forFile("latin.txt", latin, "hi\n", gitBlobId(latin), gitBlobId("hi\n".toByteArray()))
        assertEq(listOf("latin.txt"), refused.binaryPaths)
        assertEq("", refused.patch)
    }

    fun testStoreRootFollowsTheDocumentsLayout() {
        val docs = File("/data/user/0/com.forkmesh.forkmesh/app_documents")
        assertEq(File(docs, "forkmesh/repos"), LocalRepoStore.rootIn(docs))
        assertEq(File(docs, "projects/app.next.forkmesh.com_8443/forkmesh/repos"), LocalRepoStore.rootIn(docs, "App.Next.ForkMesh.com:8443"))
        assertEq("___1_", LocalRepoStore.projectSlug("[::1]"))
    }
}
