package com.forkmesh.nativeapp.feature.securedrive

import android.graphics.BitmapFactory
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import com.forkmesh.nativeapp.securedrive.Drawing
import com.forkmesh.nativeapp.securedrive.Sheet
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

internal class SdEditorState(val name: String, val kind: String, var base: String) {
    var dirty by mutableStateOf(false)
    var serialize: () -> ByteArray = { ByteArray(0) }
}

class SecureDriveEditorScreen(
    private val driveId: String,
    private val fileId: String,
    private val version: Int,
    private val readOnly: Boolean,
) : Screen() {
    override val title: String get() = "Secure Drive"
    private var leaving: (() -> Boolean)? = null

    override fun onBack(): Boolean = leaving?.invoke() ?: false

    @Composable
    override fun Content(navigator: Navigator) {
        val service = SecureDriveFeature.service
        val scope = rememberCoroutineScope()
        SdLockedGuard(navigator)
        var opened by remember { mutableStateOf<SdOpened?>(null) }
        var error by remember { mutableStateOf<String?>(null) }
        var status by remember { mutableStateOf<String?>(null) }
        var discard by remember { mutableStateOf(false) }
        val editor = remember(opened) { opened?.let { SdEditorState(it.name, it.kind, it.blob) } }
        LaunchedEffect(fileId, version) {
            val purpose = if (readOnly || version >= 0) "view" else "edit"
            service.open(driveId, fileId, version, purpose)
                .onSuccess { opened = it }
                .onFailure { error = it.sdText() }
        }
        val dirty = editor?.dirty == true
        DisposableEffect(dirty) {
            if (dirty) service.editorOpened(true)
            onDispose { if (dirty) service.editorOpened(false) }
        }
        DisposableEffect(Unit) {
            onDispose { opened?.bytes?.fill(0) }
        }
        leaving = {
            if (editor?.dirty == true) {
                discard = true
                true
            } else {
                false
            }
        }
        val editable = !readOnly && version < 0 && editor != null && editor.kind != "image"
        fun save() {
            val state = editor ?: return
            val bytes = state.serialize()
            scope.launch {
                service.save(driveId, fileId, bytes, state.base)
                    .onSuccess { blob ->
                        state.base = blob
                        state.dirty = false
                        status = "Saved as a new encrypted version."
                    }
                    .onFailure { status = "Could not save: ${it.sdText()}" }
                bytes.fill(0)
            }
        }
        val versionText = if (version >= 0) " (version ${version + 1}, read-only)" else if (readOnly) " (read-only)" else ""
        SdScaffold(
            (opened?.name ?: "Opening…") + versionText,
            navigator,
            subtitle = status,
            onBack = { if (editor?.dirty == true) discard = true else navigator.pop() },
            actions = {
                if (editable) FmTextButton(onClick = { save() }, enabled = editor?.dirty == true, busy = false) { Text("Save") }
            },
        ) {
            val file = opened
            when {
                error != null -> Text(error.orEmpty(), Modifier.padding(FmSpace.x4), style = Fm.text.bodyMedium.copy(color = Fm.colors.danger))
                file == null || editor == null -> Text("Decrypting…", Modifier.padding(FmSpace.x4), style = Fm.text.bodyMedium)
                else -> when (file.kind) {
                    "document" -> SdDocumentEditor(file.bytes, editor, !editable)
                    "spreadsheet" -> SdSheetEditor(file.bytes, editor, !editable)
                    "drawing" -> SdDrawingEditor(file.bytes, editor, !editable)
                    "image" -> SdImageViewer(file.bytes)
                    else -> SdTextEditor(file.bytes, editor, !editable)
                }
            }
        }
        if (discard) {
            FmConfirmDialog(
                title = "Discard changes?",
                body = "Your changes to ${opened?.name.orEmpty()} are not on the drive yet.",
                confirmLabel = "Discard",
                destructive = true,
                onConfirm = {
                    discard = false
                    editor?.dirty = false
                    navigator.pop()
                },
                onDismissRequest = { discard = false },
            )
        }
    }
}

@Composable
internal fun SdTextEditor(bytes: ByteArray, editor: SdEditorState, readOnly: Boolean) {
    var text by remember { mutableStateOf(String(bytes, Charsets.UTF_8)) }
    editor.serialize = { text.toByteArray(Charsets.UTF_8) }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).imePadding().padding(FmSpace.x3)) {
        FmTextField(
            value = text,
            onValueChange = {
                text = it
                editor.dirty = true
            },
            readOnly = readOnly,
            minLines = 20,
            maxLines = Int.MAX_VALUE,
            textStyle = Fm.text.bodyMedium.copy(fontFamily = FmType.mono),
        )
    }
}

@Composable
internal fun SdImageViewer(bytes: ByteArray) {
    val bitmap = remember(bytes) { BitmapFactory.decodeByteArray(bytes, 0, bytes.size) }
    Box(Modifier.fillMaxSize().padding(FmSpace.x2), contentAlignment = Alignment.Center) {
        if (bitmap == null) {
            Text("Could not show this picture. Open it in another app instead.", style = Fm.text.bodyMedium)
        } else {
            Image(bitmap.asImageBitmap(), contentDescription = null, modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Fit)
        }
    }
}

class SecureDriveVersionsScreen(private val driveId: String, private val fileId: String) : Screen() {
    override val title: String get() = "Versions"

    @Composable
    override fun Content(navigator: Navigator) {
        val service = SecureDriveFeature.service
        val views by service.views.collectAsState()
        val view = views[driveId]
        val file = view?.files?.firstOrNull { it.id == fileId }
        val scope = rememberCoroutineScope()
        var restore by remember { mutableStateOf<Int?>(null) }
        var message by remember { mutableStateOf<String?>(null) }
        SdLockedGuard(navigator)
        SdScaffold(if (file != null) "Versions of ${file.name}" else "Versions", navigator) {
            if (file == null) {
                Text("That file is no longer on the drive.", Modifier.padding(FmSpace.x4), style = Fm.text.bodyMedium)
                return@SdScaffold
            }
            Column(Modifier.fillMaxSize()) {
                Text(
                    "Every save is kept as its own encrypted version until the file is deleted.",
                    Modifier.padding(FmSpace.x4),
                    style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
                )
                message?.let { Text(it, Modifier.padding(horizontal = FmSpace.x4), style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary)) }
                LazyColumn(Modifier.weight(1f)) {
                    items(file.versions.indices.reversed().toList()) { index ->
                        val v = file.versions[index]
                        val current = index == file.versions.size - 1
                        Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                            Text("Version ${index + 1}${if (current) " (current)" else ""}", style = Fm.text.bodyLarge)
                            Text(
                                "${sdBytes(v.size)} · ${sdTime(v.time)} · ${view.deviceNames[v.device] ?: "a device"}",
                                style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
                            )
                            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                FmTextButton(onClick = {
                                    navigator.push(SecureDriveEditorScreen(driveId, fileId, index, readOnly = true))
                                }) { Text("Open read-only") }
                                if (!current && !view.removed) {
                                    FmTextButton(onClick = { restore = index }) { Text("Make this the current version") }
                                }
                            }
                        }
                        HorizontalDivider(Modifier.padding(start = FmSpace.x4), color = Fm.colors.border)
                    }
                }
            }
        }
        restore?.let { index ->
            FmConfirmDialog(
                title = "Make version ${index + 1} the current version?",
                body = "The newer versions are still kept.",
                confirmLabel = "Restore",
                onConfirm = {
                    restore = null
                    scope.launch {
                        service.restore(driveId, fileId, index)
                            .onSuccess { message = "Restored. The newer versions are still kept." }
                            .onFailure { message = it.sdText() }
                    }
                },
                onDismissRequest = { restore = null },
            )
        }
    }
}

internal fun sdParseDrawing(bytes: ByteArray): Drawing? = if (bytes.isEmpty()) Drawing() else Drawing.fromSvg(bytes)

internal fun sdParseSheet(bytes: ByteArray): Sheet = Sheet.fromCsv(bytes)
