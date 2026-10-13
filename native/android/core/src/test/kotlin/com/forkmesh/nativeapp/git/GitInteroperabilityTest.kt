package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.git.bytes.encodeLowerHex
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.nio.file.Files
import java.nio.file.Paths

class GitInteroperabilityTest {
    private val identity: GitIdentity
        get() = GitIdentity(
            name = "Ada Lovelace",
            email = "ada@example.test",
            timestampSeconds = 0,
            timezoneOffsetMinutes = 0,
        )

    private fun gitOk(root: File, vararg arguments: String) {
        SystemGit.git(root, *arguments)
    }

    fun testG1SystemGitAndForkMeshReadEachOthersLooseBlobs() {
        if (SystemGit.skipped("GitInteroperabilityTest.testG1SystemGitAndForkMeshReadEachOthersLooseBlobs")) return
        withTempDir("forkmesh-g1-interop-") { root ->
            val repository = GitRepository.init(GitRepositoryInit(root))

            val ownedId = repository.writeBlob(utf8("owned\n"))
            val catFile = SystemGit.run(listOf("-C", root.path, "cat-file", "-p", ownedId.hex))
            assertEq(0, catFile.exitCode, catFile.stderr)
            assertEq("owned\n", catFile.stdoutText)

            val oracle = SystemGit.run(listOf("-C", root.path, "hash-object", "-w", "--stdin"), stdin = utf8("oracle\n"))
            assertEq(0, oracle.exitCode, oracle.stderr)
            val oracleId = GitObjectId.parseSha1(oracle.stdoutText.trim())
            assertEq("oracle\n", utf8Text(repository.readBlob(oracleId)))
        }
    }

    fun testG2SystemGitReadsAForkMeshCommitWithCorePathAndModeVariants() {
        if (SystemGit.skipped("GitInteroperabilityTest.testG2SystemGitReadsAForkMeshCommitWithCorePathAndModeVariants")) return
        withTempDir("forkmesh-g2-owned-") { root ->
            val repository = GitRepository.init(GitRepositoryInit(root))
            val fixture = root.child("fixtures")
            fixture.mkdir()
            fixture.child("plain.txt").writeText("plain\n")
            fixture.child("binary.bin").writeBytes(byteArrayOf(0, 1, 2, 0xff.toByte()))
            fixture.child("run.sh").writeText("#!/bin/sh\n")
            expect(fixture.child("run.sh").setExecutable(true, false), "chmod 755 run.sh")
            Files.createSymbolicLink(fixture.child("link").toPath(), Paths.get("plain.txt"))
            fixture.child("café.txt").writeText("accent\n")

            repository.add(listOf(GitWorktreePath("fixtures")))
            val commit = repository.commit(
                GitCommitRequest(message = "ForkMesh G2 fixture\n", author = identity, committer = identity),
            )

            gitOk(root, "fsck", "--no-dangling")
            val tree = SystemGit.run(
                listOf("-C", root.path, "-c", "core.quotePath=false", "ls-tree", "-r", "--full-tree", commit.hex),
            )
            assertEq(0, tree.exitCode, tree.stderr)
            val output = tree.stdoutText
            expect(output.contains("100755 blob"), output)
            expect(output.contains("120000 blob"), output)
            expect(output.contains("fixtures/café.txt"), output)
        }
    }

    fun testG2ForkMeshReadsASystemGitIndexAndCleanCommittedWorktree() {
        if (SystemGit.skipped("GitInteroperabilityTest.testG2ForkMeshReadsASystemGitIndexAndCleanCommittedWorktree")) return
        withTempDir("forkmesh-g2-oracle-") { root ->
            gitOk(root, "init", "--initial-branch=main")
            gitOk(root, "config", "user.name", "Oracle")
            gitOk(root, "config", "user.email", "oracle@example.test")
            val nested = root.child("nested")
            nested.mkdir()
            nested.child("binary.bin").writeBytes(byteArrayOf(0, 0xff.toByte(), 1))
            nested.child("café.txt").writeText("accent\n")
            Files.createSymbolicLink(nested.child("link").toPath(), Paths.get("café.txt"))
            gitOk(root, "add", ".")
            gitOk(root, "commit", "-m", "oracle fixture")

            val repository = GitRepository.open(GitRepositoryOpen(root))
            val status = repository.status()

            expect(status.isClean, "status: staged=${status.staged} worktree=${status.worktree}")
            val head = repository.readHeadObjectId()
            expect(head != null)
            expect(repository.readCommit(head!!).parents.isEmpty())
        }
    }

    private fun createPack(root: File): ByteArray {
        val result = SystemGit.run(
            listOf(
                "-C",
                root.path,
                "pack-objects",
                "--stdout",
                "--revs",
                "--window=10",
                "--depth=10",
                "--no-reuse-delta",
            ),
            stdin = utf8("HEAD\n"),
        )
        assertEq(0, result.exitCode, result.stderr)
        return result.stdout
    }

    fun testG3ReadsEveryObjectFromASystemGitGeneratedPack() {
        if (SystemGit.skipped("GitInteroperabilityTest.testG3ReadsEveryObjectFromASystemGitGeneratedPack")) return
        withTempDir("forkmesh-g3-pack-") { root ->
            gitOk(root, "init", "--initial-branch=main")
            gitOk(root, "config", "user.name", "Oracle")
            gitOk(root, "config", "user.email", "oracle@example.test")
            val file = root.child("data.txt")
            file.writeText(List(2048) { "base content" }.joinToString("\n"))
            gitOk(root, "add", "data.txt")
            gitOk(root, "commit", "-m", "base")
            file.writeText((List(2047) { "base content" } + "changed").joinToString("\n"))
            gitOk(root, "add", "data.txt")
            gitOk(root, "commit", "-m", "changed")

            val pack = GitPackReader.parse(createPack(root), GitResourceLimits())

            expect(pack.objects.size > 4, "pack has ${pack.objects.size} objects")
            for (packObject in pack.objects) {
                val type = SystemGit.run(listOf("-C", root.path, "cat-file", "-t", packObject.objectId.hex))
                assertEq(0, type.exitCode, type.stderr)
                assertEq(packObject.type.name, type.stdoutText.trim())
                val body = SystemGit.run(
                    listOf("-C", root.path, "cat-file", packObject.type.name, packObject.objectId.hex),
                )
                assertEq(0, body.exitCode, body.stderr)
                assertBytes(packObject.body, body.stdout)
            }
        }
    }

    fun testG3ParsesASystemGitGeneratedVersion2PackIndex() {
        if (SystemGit.skipped("GitInteroperabilityTest.testG3ParsesASystemGitGeneratedVersion2PackIndex")) return
        withTempDir("forkmesh-g3-index-") { root ->
            gitOk(root, "init", "--initial-branch=main")
            gitOk(root, "config", "user.name", "Oracle")
            gitOk(root, "config", "user.email", "oracle@example.test")
            root.child("file.txt").writeText("content\n")
            gitOk(root, "add", ".")
            gitOk(root, "commit", "-m", "fixture")
            gitOk(root, "repack", "-ad")
            val packDirectory = root.child(".git").child("objects").child("pack")
            val files = packDirectory.listFiles()!!.filter { it.path.endsWith(".idx") }
            assertEq(1, files.size)

            val index = GitPackIndexV2.parse(files.single().readBytes(), GitResourceLimits())
            val head = SystemGit.git(root, "rev-parse", "HEAD")

            expect(index.lookup(GitObjectId.parseSha1(head)) != null, "index lists HEAD")
        }
    }

    fun testG3OpensAPackedSystemGitRepositoryThroughTheRepositoryApi() {
        if (SystemGit.skipped("GitInteroperabilityTest.testG3OpensAPackedSystemGitRepositoryThroughTheRepositoryApi")) return
        withTempDir("forkmesh-g3-repository-") { root ->
            gitOk(root, "init", "--initial-branch=main")
            gitOk(root, "config", "user.name", "Oracle")
            gitOk(root, "config", "user.email", "oracle@example.test")
            root.child("packed.txt").writeText("packed\n")
            gitOk(root, "add", ".")
            gitOk(root, "commit", "-m", "packed fixture")
            val blob = SystemGit.git(root, "rev-parse", "HEAD:packed.txt")
            gitOk(root, "repack", "-ad")
            val repository = GitRepository.open(GitRepositoryOpen(root))

            assertEq("packed\n", utf8Text(repository.readBlob(GitObjectId.parseSha1(blob))))
        }
    }

    fun testG4SystemGitVerifiesAndReadsAForkMeshGeneratedNonDeltaPack() {
        if (SystemGit.skipped("GitInteroperabilityTest.testG4SystemGitVerifiesAndReadsAForkMeshGeneratedNonDeltaPack")) return
        withTempDir("forkmesh-g4-pack-") { root ->
            val initialized = SystemGit.run(listOf("init", "--initial-branch=main", root.path))
            assertEq(0, initialized.exitCode, initialized.stderr)
            val written = GitPackWriter.write(
                listOf(
                    GitPackWriteObject(GitObjectType.blob, utf8("owned\n")),
                    GitPackWriteObject(GitObjectType.blob, byteArrayOf(0, 1, 0xff.toByte())),
                ),
            )
            val packDirectory = root.child(".git").child("objects").child("pack")
            val base = packDirectory.child("pack-forkmesh").path
            File("$base.pack").writeBytes(written.packBytes)
            File("$base.idx").writeBytes(written.indexBytes)

            val verified = SystemGit.run(listOf("verify-pack", "-v", "$base.idx"))
            assertEq(0, verified.exitCode, verified.stderr)
            val first = GitObjectId.sha1Of(GitObjectFrame.encode(GitObjectType.blob, utf8("owned\n")))
            val body = SystemGit.run(listOf("-C", root.path, "cat-file", "-p", first.hex))

            assertEq(0, body.exitCode, body.stderr)
            assertEq("owned\n", body.stdoutText)
        }
    }

    private fun oracleObjectId(type: GitObjectType, body: ByteArray, directory: File): String {
        val result = SystemGit.run(listOf("hash-object", "-t", type.name, "--stdin"), stdin = body, directory = directory)
        assertEq(0, result.exitCode, result.stderr)
        return result.stdoutText.trim()
    }

    private fun ownedFrameId(frame: ByteArray): String {
        val digest = ForkMeshSha1()
        val chunkSizes = intArrayOf(1, 55, 56, 63, 64, 65, 7, 3)
        var offset = 0
        var chunk = 0
        while (offset < frame.size) {
            val requested = chunkSizes[chunk % chunkSizes.size]
            val length = if (requested < frame.size - offset) requested else frame.size - offset
            digest.add(frame.copyOfRange(offset, offset + length))
            offset += length
            chunk += 1
        }
        return encodeLowerHex(digest.close())
    }

    fun testG45GitObjectFramesMatchSystemGitAcrossTypesAndIrregularChunks() {
        if (SystemGit.skipped("GitInteroperabilityTest.testG45GitObjectFramesMatchSystemGitAcrossTypesAndIrregularChunks")) return
        withTempDir("forkmesh-g4-5-digest-") { scratch ->
            val emptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
            val frames = listOf(
                GitObjectType.blob to utf8("blob body\n"),
                GitObjectType.tree to ByteArray(0),
                GitObjectType.commit to utf8(
                    "tree $emptyTree\n" +
                        "author Oracle <oracle@example.test> 0 +0000\n" +
                        "committer Oracle <oracle@example.test> 0 +0000\n" +
                        "\n" +
                        "commit body\n",
                ),
                GitObjectType.tag to utf8(
                    "object $emptyTree\n" +
                        "type tree\n" +
                        "tag v1\n" +
                        "tagger Oracle <oracle@example.test> 0 +0000\n" +
                        "\n" +
                        "tag body\n",
                ),
            )

            for ((type, body) in frames) {
                val frame = GitObjectFrame.encode(type, body)
                val expected = oracleObjectId(type, body, scratch)

                assertEq(expected, GitObjectId.sha1Of(frame).hex)
                assertEq(expected, ownedFrameId(frame))
            }
        }
    }
}
