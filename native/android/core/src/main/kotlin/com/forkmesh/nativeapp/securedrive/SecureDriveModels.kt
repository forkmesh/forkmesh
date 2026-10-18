package com.forkmesh.nativeapp.securedrive

import com.forkmesh.nativeapp.crypto.Utf8
import com.forkmesh.nativeapp.json.Json
import java.io.File
import java.io.FileOutputStream
import java.util.TreeMap

internal object J {
    fun str(value: Any?): String = value as? String ?: ""

    @Suppress("UNCHECKED_CAST")
    fun obj(value: Any?): Map<String, Any?> = value as? Map<String, Any?> ?: emptyMap()

    @Suppress("UNCHECKED_CAST")
    fun arr(value: Any?): List<Any?> = value as? List<Any?> ?: emptyList()

    fun long(value: Any?, fallback: Long = 0L): Long = when (value) {
        is Long -> value
        is Int -> value.toLong()
        is Number -> {
            val d = value.toDouble()
            if (d.isNaN()) fallback else d.toLong()
        }
        else -> fallback
    }

    fun int(value: Any?, fallback: Int): Int = when (value) {
        is Long -> if (value in Int.MIN_VALUE..Int.MAX_VALUE) value.toInt() else fallback
        is Int -> value
        is Double -> if (value == Math.rint(value) && value >= Int.MIN_VALUE && value <= Int.MAX_VALUE) value.toInt() else fallback
        else -> fallback
    }

    fun bool(value: Any?): Boolean = value == true

    fun compact(value: Map<String, Any?>): ByteArray = SdCrypto.utf8(Json.stringify(value))

    @Suppress("UNCHECKED_CAST")
    fun parse(bytes: ByteArray): Map<String, Any?>? = try {
        Json.parse(Utf8.decode(bytes)) as? Map<String, Any?>
    } catch (_: Exception) {
        null
    }
}

class EpochKey(val id: String, val number: Int, val key: ByteArray)

class Head(val seq: Long, val hash: String)

data class Run(val start: Long, val count: Long)

class Membership {
    var id = ""
    var container = ""
    var label = ""
    var driveId = ""
    var rootDevice = ""
    var rootEd25519 = ByteArray(0)
    var epochs = mutableListOf<EpochKey>()
    var mailbox = -1
    var lastSeq = -1L
    var lastHash = ""
    var lamport = 0L
    var heads = TreeMap<String, Head>()
    var rootGeneration = -1L
    var rootHash = ""
    var removedDevices = mutableListOf<String>()
    var cachedRoot = ""
    var keyringSeen = false
    var joinSecret = ByteArray(0)
    var joinedAt = 0L

    val isMember: Boolean get() = driveId.isNotEmpty()
    val isPending: Boolean get() = driveId.isEmpty() && joinSecret.isNotEmpty()

    fun currentEpoch(): EpochKey? {
        var best: EpochKey? = null
        for (epoch in epochs) {
            val b = best
            if (b == null || epoch.number > b.number || (epoch.number == b.number && epoch.id > b.id)) best = epoch
        }
        return best
    }

    fun epoch(id: String): EpochKey? = epochs.firstOrNull { it.id == id }

    fun toJson(): Map<String, Any?> {
        val headsJson = linkedMapOf<String, Any?>()
        for ((device, head) in heads) headsJson[device] = listOf(head.seq, head.hash)
        return linkedMapOf(
            "id" to id,
            "container" to container,
            "label" to label,
            "drive" to driveId,
            "root" to rootDevice,
            "rootEd25519" to SdCrypto.toB64(rootEd25519),
            "epochs" to SecureDriveFormat.epochsJson(epochs),
            "mailbox" to mailbox.toLong(),
            "lastSeq" to lastSeq,
            "lastHash" to lastHash,
            "lamport" to lamport,
            "heads" to headsJson,
            "rootGeneration" to rootGeneration,
            "rootHash" to rootHash,
            "removed" to removedDevices.toList(),
            "cachedRoot" to cachedRoot,
            "keyringSeen" to keyringSeen,
            "joinSecret" to SdCrypto.toB64(joinSecret),
            "joinedAt" to joinedAt,
        )
    }

    fun copy(): Membership = fromJson(toJson())

    fun assign(other: Membership) {
        id = other.id
        container = other.container
        label = other.label
        driveId = other.driveId
        rootDevice = other.rootDevice
        rootEd25519 = other.rootEd25519
        epochs = other.epochs.toMutableList()
        mailbox = other.mailbox
        lastSeq = other.lastSeq
        lastHash = other.lastHash
        lamport = other.lamport
        heads = TreeMap(other.heads)
        rootGeneration = other.rootGeneration
        rootHash = other.rootHash
        removedDevices = other.removedDevices.toMutableList()
        cachedRoot = other.cachedRoot
        keyringSeen = other.keyringSeen
        joinSecret = other.joinSecret
        joinedAt = other.joinedAt
    }

    internal fun wipeSecrets() {
        for (epoch in epochs) SdCrypto.wipe(epoch.key)
        SdCrypto.wipe(joinSecret)
    }

    companion object {
        fun fromJson(json: Map<String, Any?>): Membership {
            val m = Membership()
            m.id = J.str(json["id"])
            m.container = J.str(json["container"])
            m.label = J.str(json["label"])
            m.driveId = J.str(json["drive"])
            m.rootDevice = J.str(json["root"])
            m.rootEd25519 = SdCrypto.fromB64(J.str(json["rootEd25519"])) ?: ByteArray(0)
            m.epochs = SecureDriveFormat.epochsFrom(J.arr(json["epochs"])).toMutableList()
            m.mailbox = J.int(json["mailbox"], -1)
            m.lastSeq = J.long(json["lastSeq"], -1)
            m.lastHash = J.str(json["lastHash"])
            m.lamport = J.long(json["lamport"])
            for ((device, value) in J.obj(json["heads"])) {
                val pair = J.arr(value)
                m.heads[device] = Head(J.long(pair.getOrNull(0), -1), J.str(pair.getOrNull(1)))
            }
            m.rootGeneration = J.long(json["rootGeneration"], -1)
            m.rootHash = J.str(json["rootHash"])
            m.cachedRoot = J.str(json["cachedRoot"])
            m.keyringSeen = J.bool(json["keyringSeen"])
            for (value in J.arr(json["removed"])) m.removedDevices.add(J.str(value))
            m.joinSecret = SdCrypto.fromB64(J.str(json["joinSecret"])) ?: ByteArray(0)
            m.joinedAt = J.long(json["joinedAt"])
            return m
        }
    }
}

class LocalState(val directory: File, private val kdfProfile: KdfProfile = KdfProfile.STANDARD) {
    var deviceId = ""
    var deviceName = ""
    var keys: DeviceKeys = DeviceKeys.EMPTY
    var publicKeys: PublicKeys = PublicKeys.EMPTY
    val drives = mutableListOf<Membership>()
    private var key: ByteArray? = null
    private var kdf: Kdf? = null

    val filePath: File get() = File(directory, "device.json")

    fun exists(): Boolean = filePath.isFile

    val isUnlocked: Boolean get() = key != null

    private val floorMemoryKiB: Int get() = minOf(Kdf.FLOOR_MEMORY_KIB, kdfProfile.memoryKiB)
    private val floorPasses: Int get() = minOf(Kdf.FLOOR_PASSES, kdfProfile.passes)

    fun create(passphrase: String, name: String) {
        if (exists()) fail("device_exists")
        if (passphrase.length < 10) fail("passphrase_too_short")
        val fresh = DeviceKeys.generate()
        val published = fresh.publicKeys()
        val freshKdf = Kdf.fresh(kdfProfile)
        val derived = SdCrypto.stretch(passphrase, freshKdf)
        keys = fresh
        publicKeys = published
        deviceId = SdCrypto.randomId()
        deviceName = name.trim().ifEmpty { "This device" }.take(64)
        drives.clear()
        kdf = freshKdf
        key = derived
        try {
            save()
        } catch (error: SecureDriveException) {
            lock()
            throw error
        }
    }

    fun unlock(passphrase: String) {
        val document = readDocument() ?: fail("device_unreadable")
        if (document["suite"] != SdCrypto.SUITE) fail("device_unreadable")
        val stored = Kdf.fromJson(document["kdf"], floorMemoryKiB, floorPasses)
        val derived = SdCrypto.stretch(passphrase, stored)
        try {
            load(derived, stored, document)
        } catch (error: SecureDriveException) {
            SdCrypto.wipe(derived)
            throw error
        }
    }

    private fun readDocument(): Map<String, Any?>? {
        val file = filePath
        if (!file.isFile || file.length() > (1L shl 24)) return null
        return try {
            J.parse(file.readBytes())
        } catch (_: Exception) {
            null
        }
    }

    private fun load(derived: ByteArray, stored: Kdf, document: Map<String, Any?>) {
        val nonce = SdCrypto.fromB64(document["nonce"]) ?: fail("device_unreadable")
        val sealed = SdCrypto.fromB64(document["sealed"]) ?: fail("device_unreadable")
        val plain = SdCrypto.aeadOpen(derived, nonce, SdCrypto.label("local"), sealed) ?: fail("wrong_passphrase")
        val inner = J.parse(plain)
        SdCrypto.wipe(plain)
        inner ?: fail("device_unreadable")
        val loaded = try {
            DeviceKeys.fromJson(inner["keys"])
        } catch (_: SecureDriveException) {
            fail("device_unreadable")
        }
        val published = try {
            loaded.publicKeys()
        } catch (_: SecureDriveException) {
            fail("device_unreadable")
        }
        keys = loaded
        publicKeys = published
        deviceId = J.str(inner["device"])
        deviceName = J.str(inner["name"])
        drives.clear()
        for (value in J.arr(inner["drives"])) drives.add(Membership.fromJson(J.obj(value)))
        kdf = stored
        key = derived
    }

    fun changePassphrase(current: String, next: String) {
        val currentKey = key ?: fail("locked")
        val currentKdf = kdf ?: fail("locked")
        if (next.length < 10) fail("passphrase_too_short")
        val check = SdCrypto.stretch(current, currentKdf)
        val matches = SdCrypto.constantTimeEqual(check, currentKey)
        SdCrypto.wipe(check)
        if (!matches) fail("wrong_passphrase")
        val freshKdf = Kdf.fresh(kdfProfile)
        val derived = SdCrypto.stretch(next, freshKdf)
        kdf = freshKdf
        key = derived
        try {
            save()
        } catch (error: SecureDriveException) {
            kdf = currentKdf
            key = currentKey
            throw error
        }
    }

    fun lock() {
        SdCrypto.wipe(key)
        key = null
        keys.clear()
        keys = DeviceKeys.EMPTY
        publicKeys = PublicKeys.EMPTY
        for (m in drives) m.wipeSecrets()
        drives.clear()
        deviceId = ""
    }

    private fun innerJson(): Map<String, Any?> = linkedMapOf(
        "device" to deviceId,
        "name" to deviceName,
        "keys" to keys.toJson(),
        "drives" to drives.map { it.toJson() },
    )

    fun save() {
        val sealingKey = key ?: fail("locked")
        val currentKdf = kdf ?: fail("locked")
        val plain = J.compact(innerJson())
        val nonce = SdCrypto.random(12)
        val sealed = try {
            SdCrypto.aeadSeal(sealingKey, nonce, SdCrypto.label("local"), plain)
        } finally {
            SdCrypto.wipe(plain)
        }
        val document = linkedMapOf<String, Any?>(
            "suite" to SdCrypto.SUITE,
            "v" to 1L,
            "kdf" to currentKdf.toJson(),
            "nonce" to SdCrypto.toB64(nonce),
            "sealed" to SdCrypto.toB64(sealed),
        )
        if (!directory.isDirectory && !directory.mkdirs()) fail("device_write_failed")
        directory.setReadable(false, false)
        directory.setReadable(true, true)
        directory.setWritable(false, false)
        directory.setWritable(true, true)
        directory.setExecutable(false, false)
        directory.setExecutable(true, true)
        val temp = File(directory, "device.json.saving")
        try {
            FileOutputStream(temp).use { out ->
                out.write(SdCrypto.utf8(Json.stringify(document)))
                out.fd.sync()
            }
            temp.setReadable(false, false)
            temp.setReadable(true, true)
            temp.setWritable(false, false)
            temp.setWritable(true, true)
            if (!temp.renameTo(filePath)) fail("device_write_failed")
        } catch (error: SecureDriveException) {
            temp.delete()
            throw error
        } catch (_: Exception) {
            temp.delete()
            fail("device_write_failed")
        }
    }

    fun exportDevice(): Map<String, Any?> = innerJson()

    fun importDevice(device: Map<String, Any?>, passphrase: String) {
        if (passphrase.length < 10) fail("passphrase_too_short")
        val loaded = DeviceKeys.fromJson(device["keys"])
        val published = loaded.publicKeys()
        val freshKdf = Kdf.fresh(kdfProfile)
        val derived = SdCrypto.stretch(passphrase, freshKdf)
        keys = loaded
        publicKeys = published
        deviceId = J.str(device["device"])
        deviceName = J.str(device["name"])
        drives.clear()
        for (value in J.arr(device["drives"])) drives.add(Membership.fromJson(J.obj(value)))
        kdf = freshKdf
        key = derived
        save()
    }

    fun membership(id: String): Membership? = drives.firstOrNull { it.id == id }

    fun forget(id: String) {
        val index = drives.indexOfFirst { it.id == id }
        if (index < 0) return
        drives[index].wipeSecrets()
        drives.removeAt(index)
    }
}

class DeviceInfo(
    val id: String,
    val name: String,
    val keys: PublicKeys?,
    val addedBy: String,
    val addedAt: Long,
    val mailbox: Int,
    val root: Boolean = false,
) {
    var removed = false
    var removedBy = ""
    var removedAt = 0L
    var removedSeq = -1L
}

class FileVersion(
    var blob: String,
    var key: ByteArray,
    var pages: List<Run>,
    val size: Long,
    val sha256: String,
    val time: Long = 0,
    val device: String = "",
)

class FileEntry(val id: String, var name: String, val kind: String) {
    val versions = mutableListOf<FileVersion>()
    var created = 0L
    var modified = 0L
    var modifiedBy = ""
    var createdBy = ""
    var deleted = false
    var conflicts = 0
    var opens = 0
    var lastOpened = 0L
    var lastOpenedBy = ""

    val current: FileVersion? get() = versions.lastOrNull()
}

class Record(
    val device: String,
    val seq: Long,
    val lamport: Long,
    val time: Long,
    val action: String,
    val data: Map<String, Any?>,
    val prev: String,
    val hash: String,
    val witness: Map<String, Any?>,
    val stored: Map<String, Any?>,
)

data class Issue(val code: String, val device: String, val detail: String)

class Invite(
    val id: String,
    val requestKey: ByteArray,
    val approveKey: ByteArray,
    val mailbox: Int,
    val created: Long,
    val expires: Long,
    val createdBy: String,
) {
    var used = false
    var revoked = false

    fun open(now: Long): Boolean = !used && !revoked && expires > now
}

class JoinRequest(
    val mailbox: Int,
    val inviteId: String,
    val device: String,
    val name: String,
    val keys: PublicKeys,
    val time: Long,
    val safety: String,
)

class LeaseTiming(val settleMs: Long = 600, val staleMs: Long = 120000, val pollMs: Long = 250)

object SecureDriveFormat {
    fun epochIdFor(key: ByteArray): String = SdCrypto.hex(SdCrypto.hkdf(key, "key-id", 8))

    fun epochsJson(epochs: List<EpochKey>): List<Any?> = epochs.map {
        linkedMapOf("id" to it.id, "n" to it.number.toLong(), "key" to SdCrypto.toB64(it.key))
    }

    fun epochsFrom(array: List<Any?>): List<EpochKey> {
        val out = mutableListOf<EpochKey>()
        for (value in array) {
            val json = J.obj(value)
            val key = SdCrypto.fromB64(J.str(json["key"])) ?: continue
            if (key.size != SdCrypto.KEY_BYTES) continue
            val id = epochIdFor(key)
            if (id != J.str(json["id"])) continue
            out.add(EpochKey(id, J.int(json["n"], 0), key))
        }
        return out
    }

    fun mailboxForInvite(secret: ByteArray): Int {
        val bytes = SdCrypto.hkdf(secret, "mailbox", 2)
        return (((bytes[0].toInt() and 0xff) shl 8) or (bytes[1].toInt() and 0xff)) % SecureDriveLayout.MAILBOX_SLOTS
    }

    fun runsFromJson(array: List<Any?>): List<Run> {
        val runs = mutableListOf<Run>()
        for (value in array) {
            val pair = J.arr(value)
            val start = J.long(pair.getOrNull(0), -1)
            val count = J.long(pair.getOrNull(1), -1)
            if (start < SecureDriveLayout.DATA_PAGE || count <= 0) return emptyList()
            runs.add(Run(start, count))
        }
        return runs
    }

    fun runsToJson(runs: List<Run>): List<Any?> = runs.map { listOf(it.start, it.count) }

    fun fileKindForName(name: String): String {
        val base = name.substringAfterLast('/')
        val suffix = if (base.contains('.')) base.substringAfterLast('.').lowercase() else ""
        return when (suffix) {
            "html", "htm" -> "document"
            "csv" -> "spreadsheet"
            "svg" -> "drawing"
            "txt", "md", "json", "log" -> "text"
            "png", "jpg", "jpeg", "gif", "bmp", "webp" -> "image"
            else -> "file"
        }
    }

    private fun quoted(text: String): String = "“$text”"

    fun describeRecord(record: Record, deviceName: (String) -> String): String {
        val x = record.data
        val name = quoted(J.str(x["name"]))
        return when (record.action) {
            "drive_created" -> "Created the drive ${quoted(J.str(x["label"]))}"
            "drive_opened" -> "Connected to the drive"
            "device_added" -> "Approved device $name"
            "device_removed" -> "Removed device $name and replaced the drive key"
            "invite_created" -> "Created an invitation code"
            "invite_revoked" -> "Cancelled an invitation code"
            "invite_used" -> "Used an invitation code to add $name"
            "join_denied" -> "Turned down a request to join from $name"
            "file_created" -> "Added $name"
            "file_saved" -> "Saved a new version of $name"
            "file_restored" -> "Restored version ${J.int(x["version"], 0) + 1} of $name"
            "file_renamed" -> "Renamed ${quoted(J.str(x["from"]))} to $name"
            "file_deleted" -> "Deleted $name"
            "file_rekeyed" -> "Re-encrypted $name under the new drive key"
            "file_opened" -> when (J.str(x["purpose"])) {
                "edit" -> "Opened $name to edit"
                "export" -> "Saved a decrypted copy of $name outside the drive"
                "external" -> "Opened $name in another app"
                else -> "Opened $name"
            }
            "drive_rekeyed" -> "Replaced the drive key"
            "log_exported" -> "Exported the activity log"
            "integrity_alert" -> "Reported a problem: " +
                issueText(Issue(J.str(x["code"]), J.str(x["device"]), J.str(x["detail"])), deviceName)
            else -> record.action
        }
    }

    fun issueText(issue: Issue, deviceName: ((String) -> String)?): String {
        val device = deviceName?.invoke(issue.device) ?: issue.device
        return when (issue.code) {
            "records_missing" -> "Activity records from $device are missing from the drive (${issue.detail}). Someone with access to the drive may have deleted them."
            "log_forked" -> "$device has two different activity records with the same number. The log was altered or a device's keys were copied."
            "chain_broken" -> "The activity records from $device do not link up (${issue.detail})."
            "bad_signature" -> "A record claiming to come from $device has an invalid signature and was ignored."
            "unknown_device" -> "Records signed by a device that was never approved were ignored."
            "after_removal" -> "$device wrote records after it was removed; they were ignored."
            "own_records_missing" -> "Activity records this device wrote are missing from the drive."
            "rolled_back" -> if (issue.device.isEmpty()) {
                "The drive's encrypted index went back to an older version (${issue.detail}). The drive file may have been restored from an old copy."
            } else {
                "The drive shows older activity from $device than this device saw before (${issue.detail}). The drive may have been restored from an old copy."
            }
            "untrusted_root" -> "The drive's encrypted index was written by a device that is not an approved member ($device) and was not used."
            "keyring_missing" -> "This device's key slot on the drive is gone: it was probably removed from the drive, or the drive file was damaged. It can no longer make changes."
            "keyring_restored" -> "This device's key slot on the drive was overwritten. The drive still opens with its keys, so it writes the slot again with its next change."
            "index_restored" -> "The drive's encrypted index was older than the one this device last saw, or was damaged. This device used its own verified copy; the next change saves it back to the drive."
            "drive_truncated" -> "The drive file is shorter than its index says. Part of it was cut off or lost."
            "missing_content" -> "The encrypted content of ${quoted(issue.detail)} is missing from the drive."
            "content_damaged" -> "The encrypted content of ${quoted(issue.detail)} was changed or damaged and was not opened."
            "untrusted_keyring" -> "A key file that no member device signed was ignored."
            "unreadable_objects" -> "${issue.detail} encrypted parts of the drive could not be opened with this device's keys."
            "misplaced_object" -> "An object from a different drive was found in this folder and ignored."
            else -> issue.code
        }
    }
}
