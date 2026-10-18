package com.forkmesh.nativeapp.files

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files

class FilesModelTest {
    private fun item(name: String, folder: Boolean = false, size: Long = 0, modified: Long = 0, hidden: Boolean = name.startsWith(".")) =
        FileItem(name, name, folder, size, modified, hidden)

    fun testArrangeSortsFoldersFirstAndFilters() {
        val items = listOf(item("b.txt", size = 10, modified = 3), item("A", true, modified = 1), item("c.png", size = 99, modified = 2), item(".env", size = 5))
        assertEq(listOf("A", "b.txt", "c.png"), FilesModel.arrange(items, FileSort.Name, false, "").map { it.name })
        assertEq(listOf("A", ".env", "b.txt", "c.png"), FilesModel.arrange(items, FileSort.Name, true, "").map { it.name })
        assertEq(listOf("A", "b.txt", "c.png"), FilesModel.arrange(items, FileSort.Date, false, "").map { it.name })
        assertEq(listOf("A", "c.png", "b.txt"), FilesModel.arrange(items, FileSort.Size, false, "").map { it.name })
        assertEq(listOf("c.png"), FilesModel.arrange(items, FileSort.Name, false, "PN").map { it.name })
    }

    fun testKindsAndTextSniff() {
        assertEq(FileKind.Text, FilesModel.kind("main.kt"))
        assertEq(FileKind.Image, FilesModel.kind("photo.JPG"))
        assertEq(FileKind.Pdf, FilesModel.kind("a.pdf"))
        assertEq(FileKind.Other, FilesModel.kind("movie.mkv"))
        expect(FilesModel.looksLikeText("héllo wörld".toByteArray()))
        expect(!FilesModel.looksLikeText(byteArrayOf(1, 0, 2)))
        expect(!FilesModel.looksLikeText(byteArrayOf(0xC3.toByte(), 0x28)))
        val cut = ("a".repeat(8191) + "é" + "b".repeat(10)).toByteArray()
        expect(FilesModel.looksLikeText(cut))
    }

    fun testPaths() {
        assertEq("a/b/c.txt", FilesModel.join("a/b", "c.txt"))
        assertEq("c.txt", FilesModel.join("", "c.txt"))
        assertEq("a", FilesModel.parent("a/b"))
        assertEq("", FilesModel.parent("a"))
        assertEq("b", FilesModel.name("a/b"))
        expect(FilesModel.isInside("a/b/c", "a/b"))
        expect(!FilesModel.isInside("a/bc", "a/b"))
        assertEq(listOf("media" to "", "Movies" to "Movies", "2024" to "Movies/2024"), FilesModel.crumbs("media", "Movies/2024"))
    }

    fun testNamesAndFreeNames() {
        assertEq("Enter a name.", FilesModel.nameProblem(" "))
        assertEq("Names cannot contain / \\ : * ? \" < > |.", FilesModel.nameProblem("a/b"))
        assertEq(null, FilesModel.nameProblem("ok.txt"))
        assertEq("a (2).txt", FilesModel.freeName(listOf("a.txt"), "a.txt"))
        assertEq("a (3).txt", FilesModel.freeName(listOf("a.txt", "A (2).txt"), "a.txt"))
        assertEq("Folder", FilesModel.freeName(listOf("x"), "Folder"))
        assertEq("Delete Movies and everything in it?", FilesModel.deletePrompt(item("Movies", true)))
        assertEq("Delete a.txt?", FilesModel.deletePrompt(item("a.txt")))
    }

    fun testAddressesAndUrls() {
        assertEq(SmbAddress("192.168.1.1", 445, "", ""), FilesModel.parseAddress("192.168.1.1"))
        assertEq(SmbAddress("nas.local", 4450, "media", "Movies"), FilesModel.parseAddress("smb://nas.local:4450/media/Movies"))
        assertEq(SmbAddress("router", 445, "usb", ""), FilesModel.parseAddress("\\\\router\\usb"))
        assertEq(SmbAddress("fe80::1", 445, "", ""), FilesModel.parseAddress("[fe80::1]"))
        expect(!FilesModel.parseAddress("").valid)
        expect(!FilesModel.parseAddress("nas:99999").valid)
        assertEq("smb://192.168.1.1/media/Movies/a.txt", FilesModel.smbUrl("192.168.1.1", "media", "Movies/a.txt"))
        assertEq("smb://[fe80::1]:4450/media/", FilesModel.smbUrl("fe80::1", "media", "", 4450))
    }

    fun testSavedShareRoundTrip() {
        val share = SavedShare("id1", "192.168.1.1", "media", "alice", guest = false, remember = true)
        val back = SavedShare.fromJson(Json.parseObject(Json.stringify(share.toJson())))
        assertEq(share, back)
        assertEq("media on 192.168.1.1", share.title)
        assertEq(null, SavedShare.fromJson(mapOf("host" to "x")))
    }

    fun testSizes() {
        assertEq("12 B", FilesModel.formatSize(12))
        assertEq("1.5 KB", FilesModel.formatSize(1536))
        assertEq("20 MB", FilesModel.formatSize(20L * 1024 * 1024))
    }

    fun testLocalSourceAndTransfers() {
        val root = Files.createTempDirectory("files-test").toFile()
        try {
            val local = LocalFileSource(root, "On this phone")
            local.mkdir("Docs")
            local.writeFrom("Docs/a.txt", "hello".byteInputStream(), 5)
            assertEq(listOf("a.txt"), local.list("Docs").map { it.name })
            assertFails<FileProblem> { local.list("../") }
            local.rename("Docs/a.txt", "Docs/b.txt")
            assertEq("Something with that name is already there.", assertFails<FileProblem> { local.mkdir("Docs") }.message)

            val server = FakeSmbServer().folder("Inbox")
            val smb = SmbFileSource(SmbClient("192.168.1.1", "media", SmbCredentials("alice", "secret")) { server }, "media")
            val temp = { File.createTempFile("copy", ".tmp", root) }
            FileTransfer.copy(local, "Docs", smb, "Inbox", "Docs", temp)
            assertEq("hello", String(server.files["Inbox/Docs/b.txt"]!!))
            FileTransfer.move(smb, "Inbox/Docs/b.txt", local, "", "moved.txt", temp)
            expect("Inbox/Docs/b.txt" !in server.files)
            assertEq("hello", File(root, "moved.txt").readText())
            FileTransfer.move(local, "moved.txt", local, "Docs", "c.txt", temp)
            assertEq("hello", File(root, "Docs/c.txt").readText())
            assertFails<FileProblem> { FileTransfer.copy(local, "Docs", local, "Docs/inner", "Docs", temp) }
            val out = ByteArrayOutputStream()
            local.readTo("Docs/c.txt", out)
            assertEq("hello", out.toString())
            assertEq("smb://192.168.1.1/media/Inbox", smb.location("Inbox"))
            local.delete("Docs")
            expect(!File(root, "Docs").exists())
            assertEq(emptyList<String>(), local.list("").map { it.name })
        } finally {
            root.deleteRecursively()
        }
    }
}
