package com.forkmesh.nativeapp.feature.securedrive

import android.app.Application
import android.content.Intent
import android.net.Uri
import android.webkit.MimeTypeMap
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.securedrive.DeviceInfo
import com.forkmesh.nativeapp.securedrive.Drive
import com.forkmesh.nativeapp.securedrive.FileEntry
import com.forkmesh.nativeapp.securedrive.Invite
import com.forkmesh.nativeapp.securedrive.JoinRequest
import com.forkmesh.nativeapp.securedrive.LocalState
import com.forkmesh.nativeapp.securedrive.Record
import com.forkmesh.nativeapp.securedrive.SdCrypto
import com.forkmesh.nativeapp.securedrive.SecureDriveException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.RandomAccessFile

internal enum class SdPhase { Setup, Locked, Unlocked }

internal class SdDriveSummary(val id: String, val label: String, val pending: Boolean)

internal class SdActivityRow(val record: Record, val device: String, val text: String)

internal class SdDriveView(
    val id: String,
    val label: String,
    val pending: Boolean,
    val joinSafety: String,
    val joinRequestMissing: Boolean,
    val removed: Boolean,
    val files: List<FileEntry>,
    val activity: List<SdActivityRow>,
    val devices: List<DeviceInfo>,
    val deviceNames: Map<String, String>,
    val deviceSafety: Map<String, String>,
    val requests: List<JoinRequest>,
    val invites: List<Invite>,
    val issues: List<String>,
    val containerBytes: Long,
    val usedBytes: Long,
)

internal class SdOpened(val bytes: ByteArray, val blob: String, val name: String, val kind: String)

internal class SdExternal(val uri: Uri, val mime: String, val name: String)

@OptIn(ExperimentalCoroutinesApi::class)
internal class SecureDriveService(private val app: Application) {
    private val lane = Dispatchers.IO.limitedParallelism(1)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val root = File(app.filesDir, "secure-drive")
    private val openRoot = File(app.cacheDir, OPEN_FOLDER)
    private val storage = SafContainerOpener(app.contentResolver)
    private val state = LocalState(root)
    private val drives = HashMap<String, Drive>()
    private var lastActivity = System.currentTimeMillis()
    private var backgroundLock: Job? = null

    private val phaseFlow = MutableStateFlow(if (state.exists()) SdPhase.Locked else SdPhase.Setup)
    val phase: StateFlow<SdPhase> = phaseFlow.asStateFlow()
    private val summaryFlow = MutableStateFlow<List<SdDriveSummary>>(emptyList())
    val summaries: StateFlow<List<SdDriveSummary>> = summaryFlow.asStateFlow()
    private val viewFlow = MutableStateFlow<Map<String, SdDriveView>>(emptyMap())
    val views: StateFlow<Map<String, SdDriveView>> = viewFlow.asStateFlow()
    private val busyFlow = MutableStateFlow<String?>(null)
    val busy: StateFlow<String?> = busyFlow.asStateFlow()
    private val noticeFlow = MutableStateFlow<String?>(null)
    val notice: StateFlow<String?> = noticeFlow.asStateFlow()
    private val unsavedFlow = MutableStateFlow(0)
    var deviceName = ""
        private set
    var deviceSafety = ""
        private set

    init {
        scope.launch(lane) { wipeOpenCopies() }
        scope.launch {
            AppLifecycle.foreground.collect { foreground ->
                backgroundLock?.cancel()
                if (!foreground && phaseFlow.value == SdPhase.Unlocked) {
                    backgroundLock = scope.launch {
                        delay(BACKGROUND_LOCK_MS)
                        lock("Locked after a minute in the background. The keys were cleared from memory.")
                    }
                } else if (foreground) {
                    touch()
                }
            }
        }
        scope.launch {
            while (true) {
                delay(30_000)
                val idle = System.currentTimeMillis() - lastActivity
                if (phaseFlow.value == SdPhase.Unlocked && idle >= IDLE_LOCK_MS && unsavedFlow.value == 0) {
                    lock("Locked after 15 idle minutes. The keys were cleared from memory.")
                }
            }
        }
    }

    fun touch() {
        lastActivity = System.currentTimeMillis()
    }

    fun editorOpened(dirty: Boolean) {
        unsavedFlow.value = (unsavedFlow.value + if (dirty) 1 else -1).coerceAtLeast(0)
    }

    fun clearNotice() {
        noticeFlow.value = null
    }

    private suspend fun <T> run(work: String?, block: () -> T): Result<T> {
        touch()
        if (work != null) busyFlow.value = work
        return try {
            Result.success(withContext(lane) { block() })
        } catch (error: SecureDriveException) {
            Result.failure(error)
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            Result.failure(SecureDriveException("unexpected"))
        } finally {
            if (work != null) busyFlow.value = null
            touch()
        }
    }

    private fun requireUnlocked() {
        if (!state.isUnlocked) throw SecureDriveException("locked")
    }

    private fun drive(id: String): Drive {
        requireUnlocked()
        state.membership(id) ?: throw SecureDriveException("unknown_drive")
        return drives.getOrPut(id) { Drive(state, id, storage) }
    }

    private fun publishSummaries() {
        summaryFlow.value = state.drives.map { SdDriveSummary(it.id, it.label, it.isPending) }
        deviceName = state.deviceName
        deviceSafety = if (state.isUnlocked) SdCrypto.safetyNumber("device".toByteArray(), state.publicKeys) else ""
    }

    private fun publish(drive: Drive) {
        val m = drive.membership ?: return
        val names = drive.devices().associate { it.id to drive.deviceName(it.id) }
        val activity = drive.records.sortedByDescending { it.time }.map {
            SdActivityRow(it, drive.deviceName(it.device), drive.describe(it))
        }
        val view = SdDriveView(
            id = drive.id,
            label = m.label,
            pending = m.isPending,
            joinSafety = drive.joinSafetyNumber(),
            joinRequestMissing = drive.joinRequestMissing,
            removed = drive.removedFromDrive,
            files = drive.files(),
            activity = activity,
            devices = drive.devices(),
            deviceNames = names,
            deviceSafety = drive.devices().associate { it.id to drive.deviceSafetyNumber(it.id) },
            requests = drive.requests,
            invites = drive.openInvites(),
            issues = drive.issues.map { drive.issueText(it) },
            containerBytes = drive.containerBytes,
            usedBytes = if (m.isMember) drive.usedBytes() else 0L,
        )
        viewFlow.value = viewFlow.value + (drive.id to view)
        publishSummaries()
    }

    private fun scanAndPublish(drive: Drive) {
        try {
            drive.scan()
        } finally {
            if (drive.scanned) publish(drive)
        }
    }

    suspend fun setUp(name: String, passphrase: String, again: String): Result<Unit> {
        if (passphrase != again) return Result.failure(SecureDriveException("passphrases_differ"))
        return run("Creating this phone's keys…") {
            state.create(passphrase, name)
            publishSummaries()
            phaseFlow.value = SdPhase.Unlocked
        }
    }

    suspend fun unlock(passphrase: String): Result<Unit> = run("Unlocking…") {
        if (!state.isUnlocked) state.unlock(passphrase)
        noticeFlow.value = null
        publishSummaries()
        phaseFlow.value = SdPhase.Unlocked
    }

    fun lock(message: String = "Locked. The keys were cleared from memory.") {
        if (phaseFlow.value != SdPhase.Unlocked) return
        phaseFlow.value = SdPhase.Locked
        noticeFlow.value = message
        viewFlow.value = emptyMap()
        summaryFlow.value = emptyList()
        unsavedFlow.value = 0
        scope.launch(lane) {
            drives.clear()
            state.lock()
            wipeOpenCopies()
        }
    }

    suspend fun changePassphrase(current: String, next: String, again: String): Result<Unit> {
        if (next != again) return Result.failure(SecureDriveException("passphrases_differ"))
        return run("Changing the passphrase…") { state.changePassphrase(current, next) }
    }

    suspend fun createDrive(uri: Uri, label: String): Result<String> = run("Creating the drive…") {
        requireUnlocked()
        storage.keepAccess(uri)
        val container = uri.toString()
        if (state.drives.any { it.container == container }) throw SecureDriveException("folder_already_connected")
        storage.check(container)
        val name = label.trim().ifEmpty { storage.displayName(uri).substringBeforeLast('.').ifEmpty { "Secure Drive" } }
        val id = Drive.create(state, container, name, storage, discard = { storage.discard(container) })
        scanAndPublish(drive(id))
        id
    }

    suspend fun joinDrive(uri: Uri, code: String): Result<String> = run("Asking to join…") {
        requireUnlocked()
        storage.keepAccess(uri)
        val container = uri.toString()
        storage.check(container)
        val label = storage.displayName(uri).substringBeforeLast('.').ifEmpty { "Secure Drive" }
        val started = Drive.requestJoin(state, container, code, label, storage)
        scanAndPublish(drive(started.id))
        started.id
    }

    suspend fun cancelJoin(id: String): Result<Unit> = run("Cancelling the request…") {
        drive(id).cancelJoin()
        drives.remove(id)
        viewFlow.value = viewFlow.value - id
        publishSummaries()
    }

    suspend fun forget(id: String): Result<Unit> = run(null) {
        requireUnlocked()
        drives.remove(id)
        state.forget(id)
        state.save()
        viewFlow.value = viewFlow.value - id
        publishSummaries()
    }

    suspend fun refresh(id: String, force: Boolean): Result<Unit> = run(if (force) "Reading and verifying the drive…" else null) {
        val drive = drive(id)
        if (force || !drive.scanned || drive.changedSinceScan()) scanAndPublish(drive)
    }

    suspend fun addFiles(id: String, uris: List<Uri>): Result<List<String>> = run("Encrypting ${uris.size} file${if (uris.size == 1) "" else "s"} onto the drive…") {
        val drive = drive(id)
        val failed = mutableListOf<String>()
        for (uri in uris) {
            val name = storage.displayName(uri).ifEmpty { "file" }
            try {
                drive.addFile(name, storage.source(uri))
            } catch (error: SecureDriveException) {
                failed += "$name (${sdMessage(error.code)})"
            }
        }
        publish(drive)
        failed
    }

    suspend fun newFile(id: String, kind: String): Result<String> = run("Creating…") {
        val drive = drive(id)
        val (base, extension, initial) = when (kind) {
            "spreadsheet" -> Triple("Untitled spreadsheet", "csv", ByteArray(0))
            "drawing" -> Triple("Untitled drawing", "svg", com.forkmesh.nativeapp.securedrive.Drawing().toSvg())
            else -> Triple("Untitled document", "html", "<html><body></body></html>".toByteArray())
        }
        val taken = drive.files().map { it.name.lowercase() }.toSet()
        var name = "$base.$extension"
        var n = 2
        while (name.lowercase() in taken) name = "$base ${n++}.$extension"
        val fileId = drive.addFile(name, initial, kind)
        publish(drive)
        fileId
    }

    suspend fun open(id: String, fileId: String, version: Int, purpose: String): Result<SdOpened> = run("Decrypting…") {
        val drive = drive(id)
        val entry = drive.file(fileId) ?: throw SecureDriveException("unknown_file")
        val index = if (version < 0) entry.versions.size - 1 else version
        val blob = entry.versions.getOrNull(index)?.blob ?: throw SecureDriveException("unknown_version")
        val size = entry.versions[index].size
        if (size > MAX_IN_MEMORY) throw SecureDriveException("too_large")
        try {
            val bytes = drive.readBytes(fileId, index, purpose)
            SdOpened(bytes, blob, entry.name, entry.kind)
        } finally {
            publish(drive)
        }
    }

    suspend fun save(id: String, fileId: String, bytes: ByteArray, base: String): Result<String> = run("Saving…") {
        val drive = drive(id)
        drive.saveFile(fileId, bytes, base)
        publish(drive)
        drive.file(fileId)?.current?.blob.orEmpty()
    }

    suspend fun rename(id: String, fileId: String, name: String): Result<Unit> = run("Renaming…") {
        val drive = drive(id)
        drive.renameFile(fileId, name)
        publish(drive)
    }

    suspend fun delete(id: String, fileId: String): Result<Unit> = run("Deleting…") {
        val drive = drive(id)
        drive.deleteFile(fileId)
        publish(drive)
    }

    suspend fun restore(id: String, fileId: String, version: Int): Result<Unit> = run("Restoring version ${version + 1}…") {
        val drive = drive(id)
        drive.restoreVersion(fileId, version)
        publish(drive)
    }

    suspend fun exportCopy(id: String, fileId: String, version: Int, target: Uri): Result<Unit> = run("Decrypting…") {
        val drive = drive(id)
        val bytes = try {
            drive.readBytes(fileId, version, "export")
        } finally {
            publish(drive)
        }
        try {
            app.contentResolver.openOutputStream(target, "wt")?.use { it.write(bytes) } ?: throw SecureDriveException("write_failed")
        } catch (error: SecureDriveException) {
            throw error
        } catch (_: Exception) {
            throw SecureDriveException("write_failed")
        } finally {
            bytes.fill(0)
        }
    }

    suspend fun openExternal(id: String, fileId: String): Result<SdExternal> = run("Decrypting for another app…") {
        val drive = drive(id)
        val entry = drive.file(fileId) ?: throw SecureDriveException("unknown_file")
        val folder = File(openRoot, SdCrypto.randomId())
        if (!folder.mkdirs()) throw SecureDriveException("write_failed")
        val safeName = entry.name.replace('/', '_').replace('\\', '_')
        val target = File(folder, safeName)
        try {
            target.outputStream().use { out -> drive.readFile(fileId, -1, out, "external") }
        } catch (error: SecureDriveException) {
            shred(target)
            folder.delete()
            throw error
        } finally {
            publish(drive)
        }
        val uri = FileProvider.getUriForFile(app, "${app.packageName}.composer.files", target)
        val extension = safeName.substringAfterLast('.', "").lowercase()
        val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension) ?: "application/octet-stream"
        SdExternal(uri, mime, entry.name)
    }

    suspend fun activityCsv(id: String, target: Uri): Result<Unit> = run("Exporting activity…") {
        val drive = drive(id)
        drive.note("log_exported", emptyMap())
        val csv = drive.activityCsv()
        publish(drive)
        try {
            app.contentResolver.openOutputStream(target, "wt")?.use { it.write(csv.toByteArray()) } ?: throw SecureDriveException("write_failed")
        } catch (error: SecureDriveException) {
            throw error
        } catch (_: Exception) {
            throw SecureDriveException("write_failed")
        }
    }

    suspend fun invite(id: String): Result<String> = run("Creating an invitation…") {
        val drive = drive(id)
        val invite = drive.createInvite()
        publish(drive)
        invite.code
    }

    suspend fun cancelInvites(id: String): Result<Unit> = run("Cancelling invitations…") {
        val drive = drive(id)
        for (invite in drive.openInvites()) drive.revokeInvite(invite.id)
        publish(drive)
    }

    suspend fun approve(id: String, mailbox: Int): Result<Unit> = run("Sealing the drive key…") {
        val drive = drive(id)
        drive.approve(mailbox)
        publish(drive)
    }

    suspend fun deny(id: String, mailbox: Int): Result<Unit> = run("Turning the request down…") {
        val drive = drive(id)
        drive.deny(mailbox)
        publish(drive)
    }

    suspend fun removeDevice(id: String, deviceId: String, name: String): Result<Unit> = run("Removing $name and replacing the drive key…") {
        val drive = drive(id)
        try {
            drive.removeDevice(deviceId) { done, total ->
                busyFlow.value = "Re-encrypting under the new key: $done of $total…"
            }
        } finally {
            publish(drive)
        }
    }

    fun shareIntent(external: SdExternal, view: Boolean): Intent {
        val intent = if (view) {
            Intent(Intent.ACTION_VIEW).setDataAndType(external.uri, external.mime)
        } else {
            Intent(Intent.ACTION_SEND).setType(external.mime).putExtra(Intent.EXTRA_STREAM, external.uri)
        }
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        return Intent.createChooser(intent, external.name)
    }

    private fun shred(file: File) {
        if (!file.isFile) return
        try {
            RandomAccessFile(file, "rw").use { access ->
                val zeros = ByteArray(64 * 1024)
                var left = access.length()
                access.seek(0)
                while (left > 0) {
                    val n = minOf(left, zeros.size.toLong()).toInt()
                    access.write(zeros, 0, n)
                    left -= n
                }
                access.fd.sync()
            }
        } catch (_: Exception) {
        }
        file.delete()
    }

    private fun wipeOpenCopies() {
        val rootPath = openRoot.canonicalFile
        if (rootPath.parentFile?.canonicalPath != app.cacheDir.canonicalPath) return
        val folders = rootPath.listFiles() ?: return
        for (folder in folders) {
            if (folder.canonicalFile.parentFile?.canonicalPath != rootPath.canonicalPath) continue
            if (folder.isFile) {
                shred(folder)
                continue
            }
            folder.listFiles()?.forEach { if (it.isFile) shred(it) }
            folder.delete()
        }
    }

    companion object {
        const val OPEN_FOLDER = "secure-drive-open"
        private const val BACKGROUND_LOCK_MS = 60_000L
        private const val IDLE_LOCK_MS = 15 * 60_000L
        private const val MAX_IN_MEMORY = 64L * 1024 * 1024
    }
}

internal fun sdMessage(code: String): String = when (code) {
    "wrong_passphrase" -> "That passphrase is not right."
    "passphrase_too_short" -> "Use at least 10 characters; a few unrelated words work well."
    "passphrases_differ" -> "The two passphrases do not match."
    "invalid_invite_code" -> "That invitation code is not valid. Check it for typos."
    "container_exists" -> "That file already has something in it. Pick a new name, or use Join a drive to open an existing drive."
    "container_unavailable", "folder_unavailable" -> "The drive file cannot be reached or is read-only. Check that the router's drive is plugged in and shared, and that this phone can write to it."
    "not_seekable" -> "This storage app cannot open the file for reading and writing in place, which Secure Drive needs. Open the share through a storage or SMB app that allows it."
    "not_a_drive" -> "That file is not a Secure Drive."
    "folder_already_connected" -> "This phone is already connected to that drive."
    "drive_busy" -> "Another device has been writing to the drive for a long time. Try again in a minute."
    "drive_changed" -> "Another device changed the drive at the same moment. Try again."
    "drive_full" -> "The drive has no room left. Free space on the router's USB drive, or delete files."
    "drive_index_full" -> "The drive's index is full. Delete old files or versions."
    "drive_write_failed" -> "Could not write to the drive. Check that it is still connected and has free space."
    "drive_unreadable" -> "This phone cannot read the drive's index with its keys. It may have been removed from the drive, or the file was damaged."
    "device_removed" -> "This phone was removed from the drive and can no longer change it."
    "content_damaged" -> "The file's encrypted content was changed or damaged, so it was not opened. This is recorded in the activity log."
    "content_missing", "missing_content" -> "The file's encrypted content is missing from the drive."
    "invite_expired" -> "That invitation expired or was cancelled. Create a new one."
    "unknown_request" -> "That request is no longer on the drive."
    "invalid_name" -> "Use a name without slashes, up to 255 characters."
    "device_unreadable" -> "This phone's Secure Drive key file cannot be read."
    "device_write_failed" -> "Could not save this phone's key file."
    "cannot_remove_self" -> "To take this phone off the drive, use Forget this drive."
    "read_failed" -> "Could not read that file on this phone."
    "write_failed" -> "Could not write the decrypted copy."
    "too_large" -> "This file is too large to open in memory here. Open it in another app instead."
    "locked" -> "Unlock Secure Drive first."
    "unknown_file" -> "That file is no longer on the drive."
    "unsafe_kdf_parameters" -> "This phone's Secure Drive key file uses settings that are too weak and was not opened."
    else -> "Something went wrong ($code)."
}

internal fun Throwable.sdText(): String = sdMessage((this as? SecureDriveException)?.code ?: "unexpected")
