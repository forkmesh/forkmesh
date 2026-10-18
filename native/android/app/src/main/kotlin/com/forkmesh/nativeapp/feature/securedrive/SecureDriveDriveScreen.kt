package com.forkmesh.nativeapp.feature.securedrive

import android.content.ActivityNotFoundException
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.securedrive.DeviceInfo
import com.forkmesh.nativeapp.securedrive.FileEntry
import com.forkmesh.nativeapp.securedrive.JoinRequest
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmElevatedButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPromptDialog
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class SecureDriveDriveScreen(private val driveId: String) : Screen() {
    override val title: String get() = "Secure Drive"

    @Composable
    override fun Content(navigator: Navigator) {
        val service = SecureDriveFeature.service
        val views by service.views.collectAsState()
        val view = views[driveId]
        SdLockedGuard(navigator)
        LaunchedEffect(driveId) {
            service.refresh(driveId, force = view == null)
            while (true) {
                delay(10_000)
                service.refresh(driveId, force = false)
            }
        }
        var menu by remember { mutableStateOf(false) }
        var forget by remember { mutableStateOf(false) }
        val scope = rememberCoroutineScope()
        SdScaffold(
            view?.label ?: "Secure Drive",
            navigator,
            subtitle = view?.let { if (it.pending) "Waiting for approval" else "${sdBytes(it.containerBytes)} drive file, ${sdBytes(it.usedBytes)} in use" },
            actions = {
                FmIconButton("refresh", "Check the drive now", onPressed = { scope.launch { service.refresh(driveId, force = true) } })
                Box {
                    FmIconButton("more_vert", "More drive actions", onPressed = { menu = true })
                    FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        FmDropdownMenuItem("Forget this drive on this phone", onClick = {
                            menu = false
                            forget = true
                        }, icon = "delete")
                    }
                }
            },
        ) {
            when {
                view == null -> Text("Reading and verifying the drive…", Modifier.padding(FmSpace.x4), style = Fm.text.bodyMedium)
                view.pending -> PendingView(view, navigator)
                else -> MemberView(view, navigator)
            }
        }
        if (forget) {
            FmConfirmDialog(
                title = "Forget this drive?",
                body = "This phone stops showing the drive and deletes its keys for it. The drive file and the other devices are not changed. To use the drive again, ask for a new invitation.",
                confirmLabel = "Forget",
                destructive = true,
                onConfirm = {
                    forget = false
                    scope.launch { service.forget(driveId).onSuccess { navigator.pop() } }
                },
                onDismissRequest = { forget = false },
            )
        }
    }
}

@Composable
private fun PendingView(view: SdDriveView, navigator: Navigator) {
    val service = SecureDriveFeature.service
    val scope = rememberCoroutineScope()
    var cancel by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        Text("Waiting for approval", style = Fm.text.titleMedium)
        Text(
            "On a device that is already approved, open the drive's Devices list. Approve this phone only if that screen shows the same safety number:",
            style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
        )
        SdSafetyNumber(view.joinSafety)
        if (view.joinRequestMissing) {
            SdBanner(listOf("The request is no longer on the drive: it was turned down, or someone deleted it. Cancel it and ask for a new invitation code."), danger = false)
        }
        error?.let { Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) }
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmElevatedButton(onClick = {
                scope.launch { service.refresh(view.id, force = true).onFailure { error = it.sdText() } }
            }) { Text("Check now") }
            FmTextButton(onClick = { cancel = true }) { Text("Cancel request") }
        }
    }
    if (cancel) {
        FmConfirmDialog(
            title = "Cancel the request?",
            body = "The request is removed from the drive. You can ask again with a new code.",
            confirmLabel = "Cancel request",
            destructive = true,
            onConfirm = {
                cancel = false
                scope.launch {
                    service.cancelJoin(view.id).onSuccess { navigator.pop() }.onFailure { error = it.sdText() }
                }
            },
            onDismissRequest = { cancel = false },
        )
    }
}

private enum class DriveTab(val label: String) { Files("Files"), Activity("Activity"), Devices("Devices") }

@Composable
private fun MemberView(view: SdDriveView, navigator: Navigator) {
    var tab by rememberSaveable { mutableStateOf(DriveTab.Files) }
    val c = Fm.colors
    Column(Modifier.fillMaxSize()) {
        if (view.removed) {
            SdBanner(listOf("This phone was removed from the drive. It can still read what it opened before, but nothing new."), danger = false)
        }
        SdBanner(view.issues)
        Row(Modifier.fillMaxWidth().background(c.bgRaised)) {
            for (entry in DriveTab.entries) {
                val selected = entry == tab
                Box(
                    Modifier
                        .weight(1f)
                        .clickable { tab = entry }
                        .padding(vertical = FmSpace.x3),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        entry.label,
                        style = Fm.text.labelLarge.copy(color = if (selected) c.accent else c.textSecondary),
                    )
                }
            }
        }
        Row(Modifier.fillMaxWidth()) {
            for (entry in DriveTab.entries) {
                Box(Modifier.weight(1f).height(2.dp).background(if (entry == tab) c.accent else c.border))
            }
        }
        Box(Modifier.weight(1f).fillMaxWidth()) {
            when (tab) {
                DriveTab.Files -> FilesTab(view, navigator)
                DriveTab.Activity -> ActivityTab(view)
                DriveTab.Devices -> DevicesTab(view)
            }
        }
    }
}

private sealed class FileDialog {
    data class Rename(val file: FileEntry) : FileDialog()
    data class Delete(val file: FileEntry) : FileDialog()
    data class Export(val file: FileEntry) : FileDialog()
    data class External(val file: FileEntry, val view: Boolean) : FileDialog()
}

@Composable
private fun FilesTab(view: SdDriveView, navigator: Navigator) {
    val service = SecureDriveFeature.service
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    var filter by rememberSaveable { mutableStateOf("") }
    var newMenu by remember { mutableStateOf(false) }
    var dialog by remember { mutableStateOf<FileDialog?>(null) }
    var message by remember { mutableStateOf<String?>(null) }
    var exporting by remember { mutableStateOf<FileEntry?>(null) }
    val busy by service.busy.collectAsState()
    val add = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isEmpty()) return@rememberLauncherForActivityResult
        scope.launch {
            service.addFiles(view.id, uris)
                .onSuccess { failed ->
                    message = if (failed.isEmpty()) {
                        "Encrypted ${sdPlural(uris.size, "file")} onto the drive. The originals on this phone were not changed."
                    } else {
                        "Some files were not added: ${failed.joinToString("; ")}"
                    }
                }
                .onFailure { message = it.sdText() }
        }
    }
    val export = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { uri ->
        val file = exporting
        exporting = null
        if (uri == null || file == null) return@rememberLauncherForActivityResult
        scope.launch {
            service.exportCopy(view.id, file.id, -1, uri)
                .onSuccess { message = "Saved a decrypted copy of ${file.name}. It is not encrypted." }
                .onFailure { message = it.sdText() }
        }
    }
    fun openExternal(file: FileEntry, viewIt: Boolean) {
        scope.launch {
            service.openExternal(view.id, file.id)
                .onSuccess { external ->
                    try {
                        context.startActivity(service.shareIntent(external, viewIt))
                        message = "Opened a decrypted copy of ${file.name}. The copy is deleted when you lock."
                    } catch (_: ActivityNotFoundException) {
                        message = "No app on this phone opens ${file.name}."
                    }
                }
                .onFailure { message = it.sdText() }
        }
    }
    fun open(file: FileEntry) {
        when (file.kind) {
            "document", "spreadsheet", "drawing", "text", "image" ->
                navigator.push(SecureDriveEditorScreen(view.id, file.id, -1, readOnly = file.kind == "image"))
            else -> dialog = FileDialog.External(file, true)
        }
    }
    val shown = view.files.filter { filter.isBlank() || it.name.contains(filter.trim(), ignoreCase = true) }
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Box(Modifier.weight(1f)) { FmTextField(filter, { filter = it }, hint = "Filter files") }
            Box {
                FmElevatedButton(onClick = { newMenu = true }, enabled = busy == null && !view.removed) { Text("New") }
                FmDropdownMenu(expanded = newMenu, onDismissRequest = { newMenu = false }) {
                    for ((kind, label) in listOf("document" to "Document", "spreadsheet" to "Spreadsheet", "drawing" to "Drawing")) {
                        FmDropdownMenuItem(label, onClick = {
                            newMenu = false
                            scope.launch {
                                service.newFile(view.id, kind)
                                    .onSuccess { id -> navigator.push(SecureDriveEditorScreen(view.id, id, -1, readOnly = false)) }
                                    .onFailure { message = it.sdText() }
                            }
                        }, icon = sdKindIcon(kind))
                    }
                    FmDropdownMenuItem("Add files", onClick = {
                        newMenu = false
                        add.launch(arrayOf("*/*"))
                    }, icon = "add")
                }
            }
        }
        message?.let {
            Text(it, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1), style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
        }
        if (shown.isEmpty()) {
            Text(
                if (view.files.isEmpty()) "The drive is empty. Use New to make a document, spreadsheet or drawing, or to add files." else "No file matches.",
                Modifier.padding(FmSpace.x4),
                style = Fm.text.bodyMedium.copy(color = Fm.colors.textTertiary),
            )
        }
        LazyColumn(Modifier.weight(1f)) {
            items(shown, key = { it.id }) { file ->
                FileRow(view, file, onOpen = { open(file) }, onAction = { action ->
                    when (action) {
                        "versions" -> navigator.push(SecureDriveVersionsScreen(view.id, file.id))
                        "rename" -> dialog = FileDialog.Rename(file)
                        "delete" -> dialog = FileDialog.Delete(file)
                        "export" -> dialog = FileDialog.Export(file)
                        "external" -> dialog = FileDialog.External(file, true)
                        "share" -> dialog = FileDialog.External(file, false)
                    }
                })
            }
        }
    }
    when (val open = dialog) {
        is FileDialog.Rename -> FmPromptDialog(
            title = "Rename",
            initialValue = open.file.name,
            confirmLabel = "Rename",
            helperText = "Use a name without slashes, up to 255 characters.",
            onConfirm = { name ->
                dialog = null
                scope.launch { service.rename(view.id, open.file.id, name).onFailure { message = it.sdText() } }
            },
            onDismissRequest = { dialog = null },
        )
        is FileDialog.Delete -> FmConfirmDialog(
            title = "Delete ${open.file.name}?",
            body = "Every version of ${open.file.name} is removed from the drive for every device. The activity log keeps a record that it was deleted, and by whom.",
            confirmLabel = "Delete",
            destructive = true,
            onConfirm = {
                dialog = null
                scope.launch { service.delete(view.id, open.file.id).onFailure { message = it.sdText() } }
            },
            onDismissRequest = { dialog = null },
        )
        is FileDialog.Export -> FmConfirmDialog(
            title = "Save a decrypted copy?",
            body = "The copy is not encrypted: anyone who can open the place you save it can read it, and Secure Drive cannot delete it for you. Saving it is recorded in the activity log.",
            confirmLabel = "Save a decrypted copy",
            onConfirm = {
                dialog = null
                exporting = open.file
                export.launch(open.file.name)
            },
            onDismissRequest = { dialog = null },
        )
        is FileDialog.External -> FmConfirmDialog(
            title = if (open.view) "Open in another app?" else "Share a decrypted copy?",
            body = (if (open.file.kind == "file" && open.view) "There is no built-in viewer for this kind of file. " else "") +
                "Secure Drive writes a decrypted copy to this phone for the other app and deletes it when you lock. The other app can keep its own copy.",
            confirmLabel = if (open.view) "Open" else "Share",
            onConfirm = {
                dialog = null
                openExternal(open.file, open.view)
            },
            onDismissRequest = { dialog = null },
        )
        null -> Unit
    }
}

@Composable
private fun FileRow(view: SdDriveView, file: FileEntry, onOpen: () -> Unit, onAction: (String) -> Unit) {
    var menu by remember { mutableStateOf(false) }
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onOpen).padding(start = FmSpace.x4, end = FmSpace.x1, top = FmSpace.x2, bottom = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(sdKindIcon(file.kind), size = 22.dp, tint = c.textTertiary)
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(file.name, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyLarge)
            val who = view.deviceNames[file.modifiedBy] ?: "a device"
            val details = buildString {
                append(sdPlural(file.versions.size, "version"))
                append(" · ")
                append(sdBytes(file.current?.size ?: 0))
                append(" · ")
                append(sdTime(file.modified))
                append(" by ")
                append(who)
                if (file.conflicts > 0) append(" · saved on two devices at once ${sdPlural(file.conflicts, "time")}")
            }
            Text(details, maxLines = 2, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textTertiary))
        }
        Box {
            FmIconButton("more_vert", "File actions", onPressed = { menu = true })
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                val editable = !view.removed
                FmDropdownMenuItem("Open", onClick = { menu = false; onOpen() }, icon = "visibility")
                FmDropdownMenuItem("Versions", onClick = { menu = false; onAction("versions") }, icon = "history")
                if (editable) FmDropdownMenuItem("Rename", onClick = { menu = false; onAction("rename") }, icon = "edit")
                FmDropdownMenuItem("Open in another app", onClick = { menu = false; onAction("external") }, icon = "open_in_new")
                FmDropdownMenuItem("Share", onClick = { menu = false; onAction("share") }, icon = "share")
                FmDropdownMenuItem("Save a decrypted copy", onClick = { menu = false; onAction("export") }, icon = "download")
                if (editable) FmDropdownMenuItem("Delete", onClick = { menu = false; onAction("delete") }, icon = "delete", textColor = Fm.colors.danger)
            }
        }
    }
    HorizontalDivider(Modifier.padding(start = FmSpace.x4), color = c.border)
}

@Composable
private fun ActivityTab(view: SdDriveView) {
    val service = SecureDriveFeature.service
    val scope = rememberCoroutineScope()
    var filter by rememberSaveable { mutableStateOf("") }
    var message by remember { mutableStateOf<String?>(null) }
    val export = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/csv")) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            service.activityCsv(view.id, uri)
                .onSuccess { message = "Exported the activity log. The exported file is not encrypted." }
                .onFailure { message = it.sdText() }
        }
    }
    val needle = filter.trim()
    val rows = view.activity.filter {
        needle.isEmpty() || it.text.contains(needle, ignoreCase = true) || it.device.contains(needle, ignoreCase = true)
    }
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Box(Modifier.weight(1f)) { FmTextField(filter, { filter = it }, hint = "Filter activity") }
            FmTextButton(onClick = { export.launch("${view.label} activity.csv") }, enabled = !view.removed) { Text("Export as CSV") }
        }
        message?.let {
            Text(it, Modifier.padding(horizontal = FmSpace.x4), style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
        }
        val devices = view.devices.count { !it.removed }
        Text(
            "${sdPlural(view.activity.size, "signed activity record")} from ${sdPlural(devices, "approved device")}" +
                if (view.issues.isEmpty()) ", none missing or altered." else ". Some problems were found; see the top of the page.",
            Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
            style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
        )
        LazyColumn(Modifier.weight(1f)) {
            items(rows, key = { it.record.hash }) { row ->
                Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                    Text(row.text, style = Fm.text.bodyMedium)
                    Text("${row.device} · ${sdTime(row.record.time)}", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                }
                HorizontalDivider(Modifier.padding(start = FmSpace.x4), color = Fm.colors.border)
            }
        }
    }
}

private sealed class DeviceDialog {
    data class Approve(val request: JoinRequest) : DeviceDialog()
    data class Deny(val request: JoinRequest) : DeviceDialog()
    data class Remove(val device: DeviceInfo, val name: String) : DeviceDialog()
    data class Code(val code: String) : DeviceDialog()
    data object CancelInvites : DeviceDialog()
}

@Composable
private fun DevicesTab(view: SdDriveView) {
    val service = SecureDriveFeature.service
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    var dialog by remember { mutableStateOf<DeviceDialog?>(null) }
    var message by remember { mutableStateOf<String?>(null) }
    val busy by service.busy.collectAsState()
    val c = Fm.colors
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        message?.let { Text(it, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
        if (view.requests.isNotEmpty()) {
            Text("Asking to join", style = Fm.text.titleSmall)
            for (request in view.requests) {
                Column(Modifier.fillMaxWidth().background(c.bgRaised).padding(FmSpace.x3), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                    Text(request.name.ifEmpty { "A device" }, style = Fm.text.bodyLarge)
                    Text("Safety number", style = Fm.text.bodySmall.copy(color = c.textTertiary))
                    SdSafetyNumber(request.safety)
                    Text(sdTime(request.time), style = Fm.text.bodySmall.copy(color = c.textTertiary))
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        FmElevatedButton(onClick = { dialog = DeviceDialog.Approve(request) }, enabled = busy == null && !view.removed) { Text("Approve") }
                        FmTextButton(onClick = { dialog = DeviceDialog.Deny(request) }, enabled = busy == null && !view.removed) { Text("Turn down") }
                    }
                }
            }
            SdSpacer()
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Approved devices", Modifier.weight(1f), style = Fm.text.titleSmall)
            FmElevatedButton(onClick = {
                scope.launch {
                    service.invite(view.id).onSuccess { dialog = DeviceDialog.Code(it) }.onFailure { message = it.sdText() }
                }
            }, enabled = busy == null && !view.removed) { Text("Invite a device") }
        }
        for (device in view.devices.sortedWith(compareBy({ it.removed }, { it.addedAt }))) {
            val name = view.deviceNames[device.id] ?: device.name
            Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                FmIcon("smartphone", size = 20.dp, tint = c.textTertiary)
                Spacer(Modifier.width(FmSpace.x3))
                Column(Modifier.weight(1f)) {
                    Text(name, style = Fm.text.bodyLarge.copy(color = if (device.removed) c.textTertiary else c.textPrimary))
                    val details = when {
                        device.removed -> "Removed ${sdTime(device.removedAt)} by ${view.deviceNames[device.removedBy] ?: "a device"}"
                        device.root -> "Created the drive ${sdTime(device.addedAt)}"
                        else -> "Approved ${sdTime(device.addedAt)} by ${view.deviceNames[device.addedBy] ?: "a device"}"
                    }
                    Text(details, style = Fm.text.bodySmall.copy(color = c.textTertiary))
                    view.deviceSafety[device.id]?.takeIf { it.isNotEmpty() && !device.removed }?.let {
                        Text("Safety number $it", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                    }
                }
                if (!device.removed && !name.endsWith("(this device)") && !view.removed) {
                    FmTextButton(onClick = { dialog = DeviceDialog.Remove(device, name) }, enabled = busy == null, contentColor = c.danger) { Text("Remove") }
                }
            }
            HorizontalDivider(color = c.border)
        }
        if (view.invites.isNotEmpty()) {
            SdSpacer()
            Text(
                "${sdPlural(view.invites.size, "invitation code")} still open. Codes work once and expire 24 hours after they are made.",
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
            FmTextButton(onClick = { dialog = DeviceDialog.CancelInvites }, enabled = busy == null && !view.removed) { Text("Cancel open invitations") }
        }
        SdSpacer()
        Text("This phone's safety number", style = Fm.text.titleSmall)
        SdSafetyNumber(service.deviceSafety)
        Text(
            "Shown only on approved devices; the drive file itself never holds it in the clear.",
            style = Fm.text.bodySmall.copy(color = c.textTertiary),
        )
    }
    when (val open = dialog) {
        is DeviceDialog.Approve -> FmConfirmDialog(
            title = "Approve ${open.request.name.ifEmpty { "this device" }}?",
            body = "Approve only if the device asking to join shows exactly this safety number:\n\n${open.request.safety}\n\nIt then gets the drive key and can read, change and delete everything on the drive.",
            confirmLabel = "Approve",
            onConfirm = {
                dialog = null
                scope.launch {
                    service.approve(view.id, open.request.mailbox)
                        .onSuccess { message = "Approved ${open.request.name}. It picks up the drive key the next time it checks the drive." }
                        .onFailure { message = it.sdText() }
                }
            },
            onDismissRequest = { dialog = null },
        )
        is DeviceDialog.Deny -> FmConfirmDialog(
            title = "Turn down ${open.request.name.ifEmpty { "this device" }}?",
            body = "The request is removed from the drive and its invitation code stops working.",
            confirmLabel = "Turn down",
            destructive = true,
            onConfirm = {
                dialog = null
                scope.launch {
                    service.deny(view.id, open.request.mailbox)
                        .onSuccess { message = "Turned down. Its invitation code no longer works." }
                        .onFailure { message = it.sdText() }
                }
            },
            onDismissRequest = { dialog = null },
        )
        is DeviceDialog.Remove -> FmConfirmDialog(
            title = "Remove ${open.name}?",
            body = "This replaces the drive key and re-encrypts every file on the drive, which can take a while for a large drive. ${open.name} keeps what it already opened or saved, but cannot open anything stored after this. Also change the share's password so it can no longer write to the drive file.",
            confirmLabel = "Remove",
            destructive = true,
            onConfirm = {
                dialog = null
                scope.launch {
                    service.removeDevice(view.id, open.device.id, open.name)
                        .onSuccess { message = "Removed ${open.name}. Everything on the drive is now under a key it never had." }
                        .onFailure { message = it.sdText() }
                }
            },
            onDismissRequest = { dialog = null },
        )
        is DeviceDialog.Code -> FmConfirmDialog(
            title = "Invitation code",
            body = "On the other device: Secure Drive, Join a drive, pick this drive's file and type this code. It works once and expires in 24 hours.\n\n${open.code}\n\nThen compare the safety numbers before you approve it here.",
            confirmLabel = "Copy",
            onConfirm = {
                sdCopy(context, open.code)
                message = "Copied the invitation code."
                dialog = null
            },
            onDismissRequest = { dialog = null },
        )
        DeviceDialog.CancelInvites -> FmConfirmDialog(
            title = "Cancel open invitations?",
            body = "Codes that were not used yet stop working.",
            confirmLabel = "Cancel invitations",
            destructive = true,
            onConfirm = {
                dialog = null
                scope.launch {
                    service.cancelInvites(view.id)
                        .onSuccess { message = "Cancelled. Those codes no longer work." }
                        .onFailure { message = it.sdText() }
                }
            },
            onDismissRequest = { dialog = null },
        )
        null -> Unit
    }
}
