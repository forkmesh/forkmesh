package com.forkmesh.nativeapp.securedrive

import com.forkmesh.nativeapp.git.withTempDir
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.io.RandomAccessFile

internal const val PASS = "correct horse battery"
internal val FAST = LeaseTiming(0, 300, 1)

internal fun weakState(dir: File): LocalState = LocalState(dir, KdfProfile.TEST_ONLY_WEAK)

internal fun readOrNull(drive: Drive, id: String, version: Int = -1): ByteArray? = try {
    drive.readBytes(id, version)
} catch (_: SecureDriveException) {
    null
}

internal fun codeOf(block: () -> Unit): String = try {
    block()
    ""
} catch (error: SecureDriveException) {
    error.code
}

internal fun hasIssue(drive: Drive, code: String) = drive.issues.any { it.code == code }

internal fun hasAction(drive: Drive, action: String) = drive.records.any { it.action == action }

internal fun emptyContainer(file: File): String {
    file.parentFile.mkdirs()
    expect(file.createNewFile(), "container file created")
    return file.absolutePath
}

internal fun patch(file: File, offset: Long, bytes: ByteArray) {
    RandomAccessFile(file, "rw").use {
        it.seek(offset)
        it.write(bytes)
    }
}

object SecureDriveTest {
    fun testPrimitives() {
        val invite = SdCrypto.newInviteCode()
        assertEq(35, invite.code.length, "nine groups of three")
        expect(SdCrypto.parseInviteCode(invite.code)!!.contentEquals(invite.secret), "invite code round-trips")
        expect(SdCrypto.parseInviteCode(invite.code.lowercase())!!.contentEquals(invite.secret), "invite code ignores case")
        expect(
            SdCrypto.parseInviteCode(invite.code.replace("-", " "))!!.contentEquals(invite.secret),
            "spaces instead of dashes",
        )
        val typo = (if (invite.code[0] == 'A') 'B' else 'A') + invite.code.substring(1)
        expect(SdCrypto.parseInviteCode(typo) == null, "a mistyped invite code is refused")
        expect(SdCrypto.parseInviteCode(invite.code.dropLast(1)) == null, "a short code is refused")

        val alice = DeviceKeys.generate()
        val bob = DeviceKeys.generate()
        val alicePublic = alice.publicKeys()
        val sealed = SdCrypto.sealToDevice(alicePublic, "t".toByteArray(), "hello".toByteArray())
        assertEq("hello", SdCrypto.openFromDevice(alice, "t".toByteArray(), sealed)?.let { String(it) }, "the device opens it")
        expect(SdCrypto.openFromDevice(bob, "t".toByteArray(), sealed) == null, "another device cannot")
        expect(SdCrypto.openFromDevice(alice, "u".toByteArray(), sealed) == null, "a different label cannot")
        val tampered = sealed.copyOf()
        tampered[40] = (tampered[40].toInt() xor 1).toByte()
        expect(SdCrypto.openFromDevice(alice, "t".toByteArray(), tampered) == null, "a changed ciphertext is refused")
        val tamperedBody = sealed.copyOf()
        tamperedBody[tamperedBody.size - 1] = (tamperedBody.last().toInt() xor 1).toByte()
        expect(SdCrypto.openFromDevice(alice, "t".toByteArray(), tamperedBody) == null, "a changed tag is refused")

        var previous = 0L
        for (n in longArrayOf(0, 1, 4096, 4097, 100000, 1048577, 50000000)) {
            val padded = SdCrypto.paddedSize(n)
            expect(padded >= previous, "padding grows with size")
            expect(padded >= n && (n < 4096 || padded - n <= n / 8 + 1), "padding stays within an eighth for $n")
            previous = padded
        }
        assertEq(4096L, SdCrypto.paddedSize(0))
        assertEq(1L, Drive.chunkCount(0))
        assertEq(3L, Drive.chunkCount(40000))

        val safety = SdCrypto.safetyNumber("device".toByteArray(), alicePublic)
        expect(Regex("^[0-9]{5} [0-9]{5} [0-9]{5} [0-9]{5}$").matches(safety), "safety number shape")
        val epoch = SecureDriveFormat.epochIdFor(ByteArray(32))
        assertEq(16, epoch.length)
        for (name in listOf("a.HTML", "b.csv", "c.svg", "d.md", "e.JPEG", "f.raw")) {
            SecureDriveFormat.fileKindForName(name)
        }
        assertEq("document", SecureDriveFormat.fileKindForName("a.HTML"))
        assertEq("image", SecureDriveFormat.fileKindForName("e.JPEG"))
        assertEq("file", SecureDriveFormat.fileKindForName("f.raw"))
    }

    fun testLocalStatePassphrase() = withTempDir("secure-drive-state") { root ->
        val state = weakState(File(root, "me"))
        assertEq("passphrase_too_short", codeOf { weakState(File(root, "x")).create("short", "") })
        state.create(PASS, "Pocket phone")
        expect(state.isUnlocked && state.exists(), "keys created and saved")
        val id = state.deviceId
        state.lock()
        expect(!state.isUnlocked && state.drives.isEmpty(), "locking forgets every key")
        assertEq("wrong_passphrase", codeOf { state.unlock("wrong passphrase!") })
        state.unlock(PASS)
        assertEq(id, state.deviceId)
        assertEq("Pocket phone", state.deviceName)
        state.changePassphrase(PASS, "another long passphrase")
        state.lock()
        state.unlock("another long passphrase")
        expect(state.isUnlocked, "the new passphrase unlocks")
        assertEq("unsafe_kdf_parameters", codeOf { LocalState(File(root, "me")).unlock("another long passphrase") })
        val text = File(root, "me/device.json").readText()
        expect(!text.contains("Pocket phone"), "the key file holds nothing readable")
    }

    fun testDriveFlow() = withTempDir("secure-drive-flow") { root ->
        val folder = File(root, "router-usb")
        val containerFile = File(folder, "Shared.fmsd")
        val container = emptyContainer(containerFile)

        val alice = weakState(File(root, "alice"))
        alice.create(PASS, "Office laptop")
        val aliceId = Drive.create(alice, container, "Team share", timing = FAST)
        val aliceDrive = Drive(alice, aliceId, timing = FAST)
        aliceDrive.scan()
        expect(hasAction(aliceDrive, "drive_created") && hasAction(aliceDrive, "drive_opened"), "creation and connection are logged")
        assertEq("folder_already_connected", codeOf { Drive.create(alice, container, "", timing = FAST) })
        val other = weakState(File(root, "other"))
        other.create(PASS, "Other")
        assertEq("container_exists", codeOf { Drive.create(other, container, "", timing = FAST) })
        val emptySize = containerFile.length()
        assertEq(73L * SecureDriveLayout.PAGE_BYTES, emptySize)

        val letter = "<html><body>Quarterly secret plan</body></html>".toByteArray()
        val letterId = aliceDrive.addFile("plan.html", letter)
        expect(readOrNull(aliceDrive, letterId)!!.contentEquals(letter), "alice reads it back")
        assertEq(emptySize, containerFile.length(), "a small file does not change the drive's size")
        val big = ByteArray(3 * 1024 * 1024 + 7) { ((it * 31) and 0xff).toByte() }
        val bigId = aliceDrive.addFile("photo.raw", big)
        expect(readOrNull(aliceDrive, bigId)!!.contentEquals(big), "a multi-page file round-trips")
        expect(containerFile.length() > emptySize && containerFile.length() % SecureDriveLayout.PAGE_BYTES == 0L, "grows in whole pages")
        val emptyId = aliceDrive.addFile("empty.txt", ByteArray(0))
        assertEq(0, readOrNull(aliceDrive, emptyId)?.size, "an empty file reads back empty")
        assertEq(3, aliceDrive.files().size)
        val planEntry = aliceDrive.file(letterId)!!
        expect(planEntry.kind == "document" && planEntry.opens == 1, "typed and its open counted")
        onlyOneOpaqueFile(folder, containerFile, listOf("Quarterly", "plan.html", "Team share", "Office laptop", "photo.raw"))

        val bob = weakState(File(root, "bob"))
        bob.create(PASS, "Front desk")
        val invite = aliceDrive.createInvite()
        val join = Drive.requestJoin(bob, container, invite.code)
        val bobDrive = Drive(bob, join.id, timing = FAST)
        bobDrive.scan()
        expect(bob.membership(join.id)!!.isPending, "bob waits for approval")
        expect(bobDrive.files().isEmpty(), "bob sees nothing before approval")
        assertEq(join.safety, bobDrive.joinSafetyNumber())
        assertEq(1, folder.listFiles()!!.size, "a join request adds no file")

        aliceDrive.scan()
        assertEq(1, aliceDrive.requests.size, "alice sees bob's request")
        assertEq(join.safety, aliceDrive.requests[0].safety, "both screens show the same safety number")
        assertEq("Front desk", aliceDrive.requests[0].name)
        aliceDrive.approve(aliceDrive.requests[0].mailbox)

        bobDrive.scan()
        expect(bob.membership(join.id)!!.isMember, "bob becomes a member")
        assertEq(3, bobDrive.files().size)
        assertEq("Team share", bob.membership(join.id)!!.label)
        expect(readOrNull(bobDrive, letterId)!!.contentEquals(letter), "bob opens alice's document")
        assertEq(2, bobDrive.devices().size)
        val edited = "<html><body>Revised plan</body></html>".toByteArray()
        bobDrive.saveFile(letterId, edited)
        aliceDrive.scan()
        expect(readOrNull(aliceDrive, letterId)!!.contentEquals(edited), "alice sees bob's version")
        assertEq(2, aliceDrive.file(letterId)!!.versions.size)
        expect(readOrNull(aliceDrive, letterId, 0)!!.contentEquals(letter), "the earlier version still opens")
        assertEq(emptyList<Issue>(), aliceDrive.issues, "an honest drive reports no problems")
        expect(aliceDrive.records.any { it.device == bob.deviceId && it.action == "file_opened" }, "alice's log shows bob opening")
        expect(aliceDrive.activityCsv().contains("Front desk"), "the activity export names devices")

        val mallory = weakState(File(root, "mallory"))
        mallory.create(PASS, "Stranger")
        var strangerCode = SdCrypto.newInviteCode()
        while (aliceDrive.mailboxesInUse().contains(SecureDriveFormat.mailboxForInvite(strangerCode.secret))) {
            strangerCode = SdCrypto.newInviteCode()
        }
        Drive.requestJoin(mallory, container, strangerCode.code)
        aliceDrive.scan()
        expect(aliceDrive.requests.isEmpty(), "a request without a valid code is invisible")
        assertEq("folder_already_connected", codeOf { Drive.requestJoin(mallory, container, invite.code) })

        bobDrive.scan()
        val bobStarted = bobDrive.file(letterId)!!.current!!.blob
        aliceDrive.saveFile(letterId, "<p>alice</p>".toByteArray())
        bobDrive.saveFile(letterId, "<p>bob</p>".toByteArray(), bobStarted)
        aliceDrive.scan()
        assertEq(1, aliceDrive.file(letterId)!!.conflicts, "a save made without seeing the other is a conflict")

        concurrentWriters(alice, aliceId, bob, join.id)
        aliceDrive.scan()
        assertEq(16, aliceDrive.records.count { it.action == "log_exported" }, "the lease keeps every record")
        assertEq(emptyList<Issue>(), aliceDrive.issues)

        patch(containerFile, SecureDriveLayout.mailboxOffset(bob.membership(join.id)!!.mailbox), SdCrypto.random(SecureDriveLayout.SLOT_BYTES.toInt()))
        bobDrive.scan()
        expect(hasIssue(bobDrive, "keyring_restored") && !bobDrive.removedFromDrive, "an overwritten key slot does not lock a member out")
        expect(!hasIssue(bobDrive, "keyring_missing"), "the overwritten slot is not taken as a removal")
        bobDrive.note("log_exported", emptyMap())
        bobDrive.scan()
        expect(!hasIssue(bobDrive, "keyring_restored"), "the member writes its key slot back")
        aliceDrive.scan()
        assertEq(emptyList<Issue>(), aliceDrive.issues, "the repair leaves the drive clean")

        val tamperId = aliceDrive.addFile("ledger.csv", "a,b\n1,2\n".toByteArray())
        val page = aliceDrive.file(tamperId)!!.current!!.pages.first().start
        val byte = containerFile.readBytes()[(page * SecureDriveLayout.PAGE_BYTES + 20).toInt()]
        patch(containerFile, page * SecureDriveLayout.PAGE_BYTES + 20, byteArrayOf((byte.toInt() xor 0x40).toByte()))
        assertEq("content_damaged", codeOf { aliceDrive.readBytes(tamperId) }, "a changed ciphertext byte is refused")
        expect(hasAction(aliceDrive, "integrity_alert"), "the refusal is logged")
        expect(hasIssue(aliceDrive, "content_damaged"), "and reported")

        val beforeBob = containerFile.readBytes()
        bobDrive.scan()
        bobDrive.note("log_exported", emptyMap())
        aliceDrive.scan()
        containerFile.writeBytes(beforeBob)
        aliceDrive.scan()
        expect(hasIssue(aliceDrive, "index_restored") || hasIssue(aliceDrive, "rolled_back"), "an old copy is detected")
        bobDrive.scan()
        expect(hasIssue(bobDrive, "own_records_missing") || hasIssue(bobDrive, "index_restored"), "bob notices too")

        patch(containerFile, SecureDriveLayout.ROOT_A_PAGE * SecureDriveLayout.PAGE_BYTES, SdCrypto.random((SecureDriveLayout.ROOT_PAGES * SecureDriveLayout.PAGE_BYTES).toInt()))
        patch(containerFile, SecureDriveLayout.ROOT_B_PAGE * SecureDriveLayout.PAGE_BYTES, SdCrypto.random((SecureDriveLayout.ROOT_PAGES * SecureDriveLayout.PAGE_BYTES).toInt()))
        aliceDrive.scan()
        expect(hasIssue(aliceDrive, "index_restored") && aliceDrive.files().size >= 3, "a destroyed index is rebuilt from the cached copy")
        aliceDrive.note("log_exported", emptyMap())
        bobDrive.scan()
        expect(bobDrive.files().size >= 3, "the other device reads the repaired drive")

        alice.lock()
        alice.unlock(PASS)
        assertEq(1, alice.drives.size)
        val aliceAgain = Drive(alice, aliceId, timing = FAST)
        aliceAgain.scan()

        val carol = weakState(File(root, "carol"))
        carol.create(PASS, "Spare tablet")
        val carolInvite = aliceAgain.createInvite()
        val carolJoin = Drive.requestJoin(carol, container, carolInvite.code)
        aliceAgain.scan()
        assertEq(1, aliceAgain.requests.size)
        aliceAgain.approve(aliceAgain.requests[0].mailbox)
        val carolDrive = Drive(carol, carolJoin.id, timing = FAST)
        carolDrive.scan()
        expect(readOrNull(carolDrive, bigId)!!.contentEquals(big), "carol reads the large file")
        val oldPages = aliceAgain.file(bigId)!!.current!!.pages
        val oldFirst = pageBytes(containerFile, oldPages.first().start)

        aliceAgain.scan()
        val issuesBefore = aliceAgain.issues.size
        var progressCalls = 0
        aliceAgain.removeDevice(carol.deviceId) { _, _ -> progressCalls++ }
        expect(progressCalls > 0, "re-encryption reports progress")
        expect(!pageBytes(containerFile, oldPages.first().start).contentEquals(oldFirst), "content under the old key is overwritten")
        aliceAgain.scan()
        expect(readOrNull(aliceAgain, bigId)!!.contentEquals(big), "alice still reads it")
        bobDrive.scan()
        expect(readOrNull(bobDrive, bigId)!!.contentEquals(big), "bob picks up the new key")
        assertEq(2, bob.membership(join.id)!!.epochs.size, "bob holds both keys")
        val carolScan = codeOf { carolDrive.scan() }
        expect(carolScan.isNotEmpty() || (carolDrive.removedFromDrive && hasIssue(carolDrive, "keyring_missing")), "carol finds no index under a key she holds")
        expect(carolScan.isNotEmpty() || readOrNull(carolDrive, bigId) == null, "carol cannot open anything stored after her removal")
        expect(codeOf { carolDrive.note("drive_opened", emptyMap()) }.isNotEmpty(), "carol can no longer write records")
        aliceAgain.scan()
        assertEq(issuesBefore, aliceAgain.issues.size, "carol's attempts leave the drive intact")
        expect(aliceAgain.devices().first { it.id == carol.deviceId }.removed, "the roster marks carol removed")

        aliceAgain.renameFile(letterId, "plan-v2.html")
        assertEq("plan-v2.html", aliceAgain.file(letterId)!!.name)
        aliceAgain.restoreVersion(letterId, 0)
        expect(readOrNull(aliceAgain, letterId)!!.contentEquals(letter), "an older version can be restored")
        val letterPages = aliceAgain.file(letterId)!!.current!!.pages
        val letterBefore = pageBytes(containerFile, letterPages.first().start)
        aliceAgain.deleteFile(letterId)
        expect(aliceAgain.file(letterId) == null, "delete")
        expect(!pageBytes(containerFile, letterPages.first().start).contentEquals(letterBefore), "a deleted file's pages are overwritten")
        onlyOneOpaqueFile(folder, containerFile, listOf("Quarterly", "Revised", "plan-v2", "Spare tablet", "Front desk"))

        val invites = aliceAgain.openInvites()
        expect(invites.isEmpty(), "used invitations are closed")
        val spare = aliceAgain.createInvite()
        assertEq(1, aliceAgain.openInvites().size)
        aliceAgain.revokeInvite(spare.inviteId)
        expect(aliceAgain.openInvites().isEmpty(), "a cancelled invitation is closed")
        val dave = weakState(File(root, "dave"))
        dave.create(PASS, "Dave phone")
        val daveInvite = aliceAgain.createInvite()
        val daveJoin = Drive.requestJoin(dave, container, daveInvite.code)
        aliceAgain.scan()
        aliceAgain.deny(aliceAgain.requests[0].mailbox)
        val daveDrive = Drive(dave, daveJoin.id, timing = FAST)
        daveDrive.scan()
        expect(daveDrive.joinRequestMissing, "a turned-down request is gone from the drive")
        daveDrive.cancelJoin()
        expect(dave.drives.isEmpty(), "cancelling forgets the pending drive")
        expect(aliceAgain.activityCsv().lines().first() == "time,device,activity,record,hash", "csv header")
    }

    fun testRemovalLeavesNoIndexUnderTheOldKey() = withTempDir("secure-drive-removal") { root ->
        val container = emptyContainer(File(root, "share/Drive.fmsd"))
        val alice = weakState(File(root, "alice"))
        alice.create(PASS, "Laptop")
        val aliceId = Drive.create(alice, container, "Empty share", timing = FAST)
        val aliceDrive = Drive(alice, aliceId, timing = FAST)
        aliceDrive.scan()
        val carol = weakState(File(root, "carol"))
        carol.create(PASS, "Tablet")
        val join = Drive.requestJoin(carol, container, aliceDrive.createInvite().code)
        aliceDrive.scan()
        aliceDrive.approve(aliceDrive.requests[0].mailbox)
        val carolDrive = Drive(carol, join.id, timing = FAST)
        carolDrive.scan()
        aliceDrive.scan()
        expect(aliceDrive.files().isEmpty(), "no files, so removal is a single commit")
        aliceDrive.removeDevice(carol.deviceId)
        val code = codeOf { carolDrive.scan() }
        expect(code.isNotEmpty() || (carolDrive.removedFromDrive && hasIssue(carolDrive, "keyring_missing")), "carol is out: $code ${carolDrive.issues}")
        expect(!hasIssue(carolDrive, "keyring_restored"), "no index under the old key is left to trust")
        expect(codeOf { carolDrive.note("drive_opened", emptyMap()) }.isNotEmpty(), "carol can no longer write")
        aliceDrive.scan()
        assertEq(emptyList<Issue>(), aliceDrive.issues)
    }

    private fun concurrentWriters(alice: LocalState, aliceId: String, bob: LocalState, bobId: String) {
        val timing = LeaseTiming(15, 3000, 2)
        val aliceWriter = Drive(alice, aliceId, timing = timing)
        val bobWriter = Drive(bob, bobId, timing = timing)
        aliceWriter.scan()
        bobWriter.scan()
        var aliceFailure = ""
        val thread = Thread {
            for (i in 0 until 8) {
                val code = codeOf { aliceWriter.note("log_exported", emptyMap()) }
                if (code.isNotEmpty()) aliceFailure = code
            }
        }
        thread.start()
        var bobFailure = ""
        for (i in 0 until 8) {
            val code = codeOf { bobWriter.note("log_exported", emptyMap()) }
            if (code.isNotEmpty()) bobFailure = code
        }
        thread.join()
        assertEq("", aliceFailure, "alice's concurrent writes")
        assertEq("", bobFailure, "bob's concurrent writes")
    }

    private fun pageBytes(file: File, page: Long): ByteArray = RandomAccessFile(file, "r").use {
        val out = ByteArray(SecureDriveLayout.PAGE_BYTES.toInt())
        it.seek(page * SecureDriveLayout.PAGE_BYTES)
        it.readFully(out)
        out
    }

    private fun onlyOneOpaqueFile(folder: File, container: File, needles: List<String>) {
        val entries = folder.listFiles()!!
        expect(entries.size == 1 && entries[0].absolutePath == container.absolutePath, "the share holds one file")
        val bytes = container.readBytes()
        assertEq(0L, bytes.size.toLong() % SecureDriveLayout.PAGE_BYTES)
        val text = String(bytes, Charsets.ISO_8859_1)
        for (needle in needles) expect(!text.contains(needle), "the file does not contain $needle")
    }
}
