package com.forkmesh.nativeapp.securedrive

import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.git.withTempDir
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.io.RandomAccessFile

object SecureDriveInteropTest {
    private fun fixturesDir(): File? {
        System.getenv("FORKMESH_SECURE_DRIVE_FIXTURES")?.takeIf { it.isNotEmpty() }?.let { return File(it) }
        var dir: File? = File("").absoluteFile
        while (dir != null) {
            val candidate = File(dir, "marketplace/packages/secure-drive/tests/fixtures")
            if (candidate.isDirectory) return candidate
            dir = dir.parentFile
        }
        return null
    }

    @Suppress("UNCHECKED_CAST")
    private fun obj(value: Any?): Map<String, Any?> = value as? Map<String, Any?> ?: emptyMap()

    @Suppress("UNCHECKED_CAST")
    private fun arr(value: Any?): List<Any?> = value as? List<Any?> ?: emptyList()

    private fun int(value: Any?): Int = (value as? Number)?.toInt() ?: -1

    fun testReadsEveryFixture() {
        val dir = fixturesDir() ?: throw AssertionError("fixtures folder not found")
        val found = dir.listFiles()!!.map { it.name }.filter { it.startsWith("interop-from-") && it.endsWith(".json") }.sorted()
        expect(found.contains("interop-from-cpp.json"), "the desktop's fixture is present")
        for (name in found) readsFixture(File(dir, name))
    }

    private fun readsFixture(path: File) = withTempDir("secure-drive-fixture") { scratch ->
        val fixture = obj(Json.parse(path.readText()))
        val writer = fixture["writer"] as? String ?: "?"
        fun check(ok: Boolean, what: String) = expect(ok, "fixture from $writer: $what")
        check(fixture["format"] == "forkmesh-secure-drive-interop-v1", "format")
        val container = File(scratch, "drive.fmsd")
        File(path.parentFile, fixture["container"] as String).copyTo(container)
        container.setWritable(true, true)
        val pass = fixture["passphrase"] as String
        fun withContainer(device: Map<String, Any?>): Map<String, Any?> {
            val copy = LinkedHashMap(device)
            copy["drives"] = arr(device["drives"]).map { LinkedHashMap(obj(it)).apply { put("container", container.absolutePath) } }
            return copy
        }
        val states = mutableListOf<LocalState>()
        val drives = mutableListOf<Drive>()
        val expectedFiles = arr(fixture["files"]).map(::obj)
        for ((i, member) in arr(fixture["members"]).withIndex()) {
            val state = weakState(File(scratch, "member$i"))
            state.importDevice(withContainer(obj(member)), pass)
            check(state.drives.isNotEmpty(), "imports a member")
            val drive = Drive(state, state.drives.first().id, timing = FAST)
            drive.scan()
            check(drive.issues.isEmpty(), "the drive verifies with no problems: ${drive.issues}")
            check(drive.records.size >= int(fixture["minimumRecords"]), "every record is read")
            check(state.drives.first().label == fixture["label"], "the drive name")
            check(drive.files().size == expectedFiles.size, "the file list")
            for (want in expectedFiles) {
                val entry = drive.file(want["id"] as String)
                check(
                    entry != null && entry.name == want["name"] && entry.kind == want["kind"] && entry.versions.size == int(want["versions"]),
                    "a file's name, kind and versions (${want["name"]})",
                )
                if (entry == null) continue
                val content = drive.readBytes(entry.id)
                check(Sha256.hex(content) == want["sha256"], "a file's content decrypts to the written bytes (${entry.name})")
                check(content.size.toLong() == (want["size"] as Number).toLong(), "a file's size (${entry.name})")
            }
            for (deleted in arr(fixture["deletedFiles"])) check(drive.files().none { it.name == deleted }, "a deleted file stays deleted")
            for ((device, safety) in obj(fixture["deviceSafety"])) {
                check(drive.deviceSafetyNumber(device) == safety, "safety numbers agree")
            }
            states.add(state)
            drives.add(drive)
        }
        check(drives.size >= 2, "two members")
        val request = obj(fixture["joinRequest"])
        drives[0].scan()
        val requests = drives[0].requests
        check(
            requests.size == 1 && requests[0].mailbox == int(request["mailbox"]) &&
                requests[0].safety == request["safety"] && requests[0].name == request["name"],
            "the pending join request and its safety number",
        )
        check(SdCrypto.parseInviteCode(fixture["openInviteCode"] as String) != null, "the open invitation code parses")
        val added = drives[1].addFile("from-kotlin.txt", "written by Android".toByteArray())
        val firstId = expectedFiles.first()["id"] as String
        drives[1].saveFile(firstId, "changed by Android".toByteArray())
        drives[0].approve(int(request["mailbox"]))
        val pendingState = weakState(File(scratch, "pending"))
        pendingState.importDevice(withContainer(obj(fixture["pending"])), pass)
        check(pendingState.drives.isNotEmpty(), "imports the pending device")
        val pending = Drive(pendingState, pendingState.drives.first().id, timing = FAST)
        pending.scan()
        check(pendingState.drives.first().isMember, "the pending device joins")
        check(String(pending.readBytes(added)) == "written by Android", "it reads the added file")
        check(String(pending.readBytes(firstId)) == "changed by Android", "it reads the saved version")
        drives[0].scan()
        check(drives[0].issues.isEmpty() && pending.issues.isEmpty(), "the drive still verifies after writes from Android")
    }

    fun testWritesFixtureWhenAsked() {
        val target = System.getenv("SECURE_DRIVE_WRITE_FIXTURE")?.takeIf { it.isNotEmpty() } ?: return
        writeFixture(File(target))
    }

    private fun pattern(size: Int, seed: Int): ByteArray = ByteArray(size) { ((it * 131 + seed * 17 + (it shr 8)) and 0xff).toByte() }

    private fun deviceJson(state: LocalState, driveId: String, containerName: String): Map<String, Any?> {
        val device = LinkedHashMap(state.exportDevice())
        device["drives"] = arr(device["drives"]).map(::obj).filter { it["id"] == driveId }.map {
            LinkedHashMap(it).apply { put("container", containerName) }
        }
        return device
    }

    private fun zeroFreeSpace(drive: Drive, container: File) {
        RandomAccessFile(container, "rw").use { file ->
            val pages = file.length() / SecureDriveLayout.PAGE_BYTES
            val used = BooleanArray(pages.toInt())
            for (run in drive.dataRunsInUse()) for (i in 0 until run.count) used[(run.start + i).toInt()] = true
            val zeroPage = ByteArray(SecureDriveLayout.PAGE_BYTES.toInt())
            file.seek(SecureDriveLayout.LEASE_PAGE * SecureDriveLayout.PAGE_BYTES)
            file.write(zeroPage)
            val mailboxes = drive.mailboxesInUse()
            for (slot in 0 until SecureDriveLayout.MAILBOX_SLOTS) {
                if (mailboxes.contains(slot)) continue
                file.seek(SecureDriveLayout.mailboxOffset(slot))
                file.write(ByteArray(SecureDriveLayout.SLOT_BYTES.toInt()))
            }
            for (page in SecureDriveLayout.DATA_PAGE until pages) {
                if (used[page.toInt()]) continue
                file.seek(page * SecureDriveLayout.PAGE_BYTES)
                file.write(zeroPage)
            }
        }
    }

    private fun writeFixture(directory: File) = withTempDir("secure-drive-writer") { scratch ->
        directory.mkdirs()
        val name = "interop-from-kotlin"
        val containerName = "$name.fmsd"
        val containerFile = File(directory, containerName)
        if (containerFile.exists()) expect(containerFile.delete(), "replaced the previous fixture")
        val container = emptyContainer(containerFile)
        val alice = weakState(File(scratch, "alice"))
        val bob = weakState(File(scratch, "bob"))
        val carol = weakState(File(scratch, "carol"))
        alice.create(PASS, "Office laptop")
        bob.create(PASS, "Front desk phone")
        carol.create(PASS, "Spare tablet")
        val aliceId = Drive.create(alice, container, "Interop share", timing = FAST)
        val a = Drive(alice, aliceId, timing = FAST)
        a.inviteLifetimeMs = 100L * 365 * 24 * 60 * 60 * 1000
        a.scan()
        val notes = a.addFile("Notes.html", "<html><body><h1>Notes</h1><p>First draft</p></body></html>".toByteArray())
        val budget = a.addFile("Budget.csv", "Item,Cost\r\nPaper,4.5\r\nInk,19.99\r\nTotal,\"=SUM(B2:B3)\"\r\n".toByteArray())
        val drawing = Drawing(strokes = mutableListOf(Stroke("#0969da", 6.0, mutableListOf(Point(100.0, 100.0), Point(500.0, 100.0), Point(500.0, 400.0)))))
        val sketch = a.addFile("Sketch.svg", drawing.toSvg())
        a.addFile("empty.txt", ByteArray(0))
        a.addFile("photo.bin", pattern(40000, 3))
        val old = a.addFile("old.txt", "to be deleted".toByteArray())
        a.renameFile(budget, "Budget 2027.csv")
        a.deleteFile(old)

        val invite = a.createInvite()
        val bobJoin = Drive.requestJoin(bob, container, invite.code)
        a.scan()
        a.approve(a.requests[0].mailbox)
        val b = Drive(bob, bobJoin.id, timing = FAST)
        b.scan()
        b.saveFile(notes, "<html><body><h1>Notes</h1><p>Second draft from the phone</p></body></html>".toByteArray())
        b.readBytes(sketch)

        a.scan()
        val carolInvite = a.createInvite()
        val carolJoin = Drive.requestJoin(carol, container, carolInvite.code)
        a.scan()
        val openCode = a.createInvite().code
        a.scan()
        b.scan()
        expect(a.issues.isEmpty() && b.issues.isEmpty() && a.requests.size == 1, "fixture drive is clean")
        zeroFreeSpace(a, containerFile)
        a.scan()
        b.scan()
        expect(a.issues.isEmpty() && b.issues.isEmpty(), "zeroing free space kept the fixture intact")

        val files = a.files().map { entry ->
            val v = entry.current!!
            linkedMapOf<String, Any?>(
                "id" to entry.id,
                "name" to entry.name,
                "kind" to entry.kind,
                "versions" to entry.versions.size.toLong(),
                "size" to v.size,
                "sha256" to v.sha256,
            )
        }
        val safety = linkedMapOf<String, Any?>()
        for (device in a.devices()) safety[device.id] = a.deviceSafetyNumber(device.id)
        val request = a.requests[0]
        val fixture = linkedMapOf<String, Any?>(
            "format" to "forkmesh-secure-drive-interop-v1",
            "writer" to "kotlin",
            "container" to containerName,
            "passphrase" to PASS,
            "drive" to alice.membership(aliceId)!!.driveId,
            "label" to "Interop share",
            "members" to listOf(deviceJson(alice, aliceId, containerName), deviceJson(bob, bobJoin.id, containerName)),
            "pending" to deviceJson(carol, carolJoin.id, containerName),
            "joinRequest" to linkedMapOf("mailbox" to request.mailbox.toLong(), "name" to request.name, "safety" to request.safety),
            "openInviteCode" to openCode,
            "files" to files,
            "deletedFiles" to listOf("old.txt"),
            "minimumRecords" to a.records.size.toLong(),
            "deviceSafety" to safety,
        )
        File(directory, "$name.json").writeText(pretty(fixture, "") + "\n")
        println("wrote ${File(directory, "$name.json")}")
    }

    private fun pretty(value: Any?, indent: String): String {
        val inner = "$indent    "
        return when (value) {
            is Map<*, *> -> if (value.isEmpty()) "{}" else value.entries.joinToString(",\n", "{\n", "\n$indent}") {
                inner + Json.stringify(it.key.toString()) + ": " + pretty(it.value, inner)
            }
            is List<*> -> if (value.isEmpty()) "[]" else value.joinToString(",\n", "[\n", "\n$indent]") { inner + pretty(it, inner) }
            else -> Json.stringify(value)
        }
    }

    fun testFixtureWriterRoundTrip() = withTempDir("secure-drive-own-fixture") { dir ->
        writeFixture(dir)
        val json = File(dir, "interop-from-kotlin.json")
        expect(json.isFile && File(dir, "interop-from-kotlin.fmsd").isFile, "the writer makes both files")
        readsFixture(json)
        assertEq("kotlin", obj(Json.parse(json.readText()))["writer"])
    }
}
