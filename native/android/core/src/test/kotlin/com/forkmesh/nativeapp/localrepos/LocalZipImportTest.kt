package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.GitErrorCode
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import kotlin.random.Random

class LocalZipImportTest {
    private val author = GitAuthor(name = "Alice", email = "alice@example.com")

    private val unix = (3 shl 8) or 20

    private fun mode(bits: Int): Long = bits.toLong() shl 16

    private inline fun <T> withWorkspace(temp: File, block: (GitWorkspace) -> T): T {
        val workspace = GitWorkspace(File(temp, "repo"))
        try {
            return block(workspace)
        } finally {
            workspace.dispose()
        }
    }

    fun testSkipsSymlinksDirectoriesAndMacJunk() = withScratch("fm-zip-") { temp ->
        val zip = File(temp, "a.zip")
        writeTestZip(
            zip,
            listOf(
                TestZipEntry("proj/", versionMadeBy = unix, externalAttributes = mode(0x41ed)),
                TestZipEntry("proj/a.txt", "hello", deflate = true, versionMadeBy = unix, externalAttributes = mode(0x81a4)),
                TestZipEntry("proj/link", "a.txt", versionMadeBy = unix, externalAttributes = mode(0xa1ff)),
                TestZipEntry("proj/dos-link", "a.txt", versionMadeBy = 20, externalAttributes = mode(0xa1ff)),
                TestZipEntry("__MACOSX/proj/._a.txt", "junk"),
                TestZipEntry("proj/.DS_Store", "junk"),
            ),
        )
        ZipArchive.open(zip).use { archive ->
            assertEq(
                listOf("proj/", "proj/a.txt", "proj/link", "proj/dos-link", "__MACOSX/proj/._a.txt", "proj/.DS_Store"),
                archive.files.map { it.name },
            )
            expect(!archive.files[0].isFile)
            expect(!archive.files[1].isSymbolicLink)
            expect(archive.files[2].isSymbolicLink)
            expect(!archive.files[3].isSymbolicLink)
        }
        withWorkspace(temp) { workspace ->
            val result = workspace.importFromZip(zip.path, author = author)
            assertEq(2, result.importedFiles)
            assertEq(0, result.skippedEntries)
            assertEq(listOf("a.txt", "dos-link"), workspace.listFiles().map { it.path })
            assertEq("hello", workspace.readFile("a.txt").text)
            assertEq("a.txt", workspace.readFile("dos-link").text)
        }
    }

    fun testSkipsDotGitAndUnportablePaths() = withScratch("fm-zip-") { temp ->
        val zip = File(temp, "b.zip")
        writeTestZip(
            zip,
            listOf(
                TestZipEntry(".git/config", "[core]"),
                TestZipEntry("./ok.txt", "ok"),
                TestZipEntry("a/../b.txt", "b"),
            ),
        )
        withWorkspace(temp) { workspace ->
            val result = workspace.importFromZip(zip.path, author = author)
            assertEq(1, result.importedFiles)
            assertEq(2, result.skippedEntries)
            assertEq(listOf("ok.txt"), workspace.listFiles().map { it.path })
            expect(!File(temp, "b.txt").exists())
        }
    }

    fun testADeclaredOversizedMemberIsRefusedBeforeAnyWrite() = withScratch("fm-zip-") { temp ->
        val zip = File(temp, "big.zip")
        writeTestZip(
            zip,
            listOf(TestZipEntry("huge.bin".toByteArray(), ByteArray(4), declaredSize = GitWorkspaceOps.maxImportFileBytes + 1L)),
        )
        withWorkspace(temp) { workspace ->
            val error = assertFails<GitWorkspaceException> { workspace.importFromZip(zip.path, author = author) }
            assertEq(GitErrorCode.resourceLimitExceeded, error.code)
            assertEq("This content exceeds the on-device Git limits.", error.message)
            assertEq("a file exceeds the maximum object size", error.detail)
            expect(!File(temp, "repo/.git").exists())
        }
    }

    fun testInflationStopsAtTheLimitEvenWhenTheHeaderLies() = withScratch("fm-zip-") { temp ->
        val zip = File(temp, "bomb.zip")
        writeTestZip(zip, listOf(TestZipEntry("zeros".toByteArray(), ByteArray(1_000_000), deflate = true, declaredSize = 10)))
        ZipArchive.open(zip).use { archive ->
            val entry = archive.files.single()
            assertEq(10L, entry.size)
            assertEq(1001, archive.content(entry, 1000).size)
            assertEq(1_000_000, archive.content(entry, 2_000_000).size)
        }
    }

    fun testStoredAndDeflatedMembersRoundTrip() = withScratch("fm-zip-") { temp ->
        val random = Random(42)
        val text = (0 until 2000).joinToString("\n") { "line $it ${random.nextInt()}" }
        val zip = File(temp, "round.zip")
        writeTestZip(zip, listOf(TestZipEntry("stored.txt", text), TestZipEntry("deflated.txt", text, deflate = true)))
        ZipArchive.open(zip).use { archive ->
            for (entry in archive.files) {
                assertEq(text, String(archive.content(entry, 10_000_000), Charsets.UTF_8), entry.name)
            }
        }
    }

    fun testNamesDecodeAsUtf8ElseLatin1() = withScratch("fm-zip-") { temp ->
        val zip = File(temp, "names.zip")
        writeTestZip(
            zip,
            listOf(
                TestZipEntry("ü.txt".toByteArray(Charsets.UTF_8), "u".toByteArray()),
                TestZipEntry(byteArrayOf(0xe9.toByte(), 0x2e, 0x74, 0x78, 0x74), "e".toByteArray(), utf8Flag = false),
            ),
        )
        ZipArchive.open(zip).use { archive ->
            assertEq(listOf("ü.txt", "é.txt"), archive.files.map { it.name })
        }
    }

    fun testDuplicateNamesKeepTheFirstMember() = withScratch("fm-zip-") { temp ->
        val zip = File(temp, "dup.zip")
        writeTestZip(zip, listOf(TestZipEntry("a.txt", "first"), TestZipEntry("a.txt", "second")))
        withWorkspace(temp) { workspace ->
            assertEq(1, workspace.importFromZip(zip.path, author = author).importedFiles)
            assertEq("first", workspace.readFile("a.txt").text)
        }
    }

    fun testRefusesAnythingThatIsNotAReadableZip() = withScratch("fm-zip-") { temp ->
        val junk = File(temp, "junk.zip").apply { writeBytes(Random(7).nextBytes(4096)) }
        withWorkspace(temp) { workspace ->
            assertEq(
                "The archive is not a valid zip file.",
                assertFails<GitWorkspaceException> { workspace.importFromZip(junk.path, author = author) }.message,
            )
            assertEq(
                "The selected archive could not be read.",
                assertFails<GitWorkspaceException> { workspace.importFromZip(File(temp, "missing.zip").path, author = author) }.message,
            )
        }
    }

    fun testAnArchiveWithNothingImportableLeavesNoRepository() = withScratch("fm-zip-") { temp ->
        val zip = File(temp, "empty.zip")
        writeTestZip(zip, listOf(TestZipEntry("folder/"), TestZipEntry("__MACOSX/x", "junk")))
        val store = LocalRepoStore(File(temp, "repos"))
        assertEq(
            "Nothing importable was found.",
            assertFails<GitWorkspaceException> { store.importZip(name = "empty", zipPath = zip.path, author = author) }.message,
        )
        expect(store.list().isEmpty())
        expect(!File(store.root, "empty").exists())
    }
}
