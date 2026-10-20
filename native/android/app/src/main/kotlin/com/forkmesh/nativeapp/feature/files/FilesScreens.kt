package com.forkmesh.nativeapp.feature.files

import com.forkmesh.nativeapp.ui.kit.FmSwitch
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.webkit.MimeTypeMap
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.rememberTransformableState
import androidx.compose.foundation.gestures.transformable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.code.codeLangForPath
import com.forkmesh.nativeapp.feature.code.CodeEditorState
import com.forkmesh.nativeapp.feature.code.CodeKeyBar
import com.forkmesh.nativeapp.feature.code.EditorStatusLine
import com.forkmesh.nativeapp.feature.code.FmCodeEditor
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.files.FileItem
import com.forkmesh.nativeapp.files.FileKind
import com.forkmesh.nativeapp.files.FileProblem
import com.forkmesh.nativeapp.files.FileSort
import com.forkmesh.nativeapp.files.FileSource
import com.forkmesh.nativeapp.files.FileTransfer
import com.forkmesh.nativeapp.files.FilesModel
import com.forkmesh.nativeapp.files.SavedShare
import com.forkmesh.nativeapp.files.SmbException
import com.forkmesh.nativeapp.files.SmbStatus
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.UUID
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private val store get() = FilesFeature.store

private fun problemText(problem: Throwable): String = when (problem) {
    is SmbException, is FileProblem -> problem.message ?: ""
    is SecurityException -> "Access denied."
    else -> problem.message ?: problem.toString()
}

private fun iconFor(item: FileItem): String = when (item.kind) {
    FileKind.Folder -> "folder"
    FileKind.Text -> "description"
    FileKind.Image -> "image"
    FileKind.Pdf -> "description"
    FileKind.Other -> "insert_drive_file_outlined"
}

@Composable
private fun Section(text: String) {
    Text(text, style = Fm.text.titleSmall, color = Fm.colors.textSecondary, modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x1))
}

@Composable
private fun Line(text: String, error: Boolean = false) {
    Text(
        text,
        style = Fm.text.bodyMedium,
        color = if (error) Fm.colors.danger else Fm.colors.textSecondary,
        modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
    )
}

@Composable
private fun Field(label: String, value: String, onChange: (String) -> Unit, secret: Boolean = false, hint: String = "") {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        placeholder = if (hint.isEmpty()) null else ({ Text(hint) }),
        singleLine = true,
        visualTransformation = if (secret) PasswordVisualTransformation() else VisualTransformation.None,
        keyboardOptions = KeyboardOptions(keyboardType = if (secret) KeyboardType.Password else KeyboardType.Uri, autoCorrectEnabled = false),
        modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
    )
}

@Composable
private fun Toggle(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable { onChange(!checked) }.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = Fm.text.bodyMedium, modifier = Modifier.weight(1f))
        FmSwitch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun NamePrompt(title: String, initial: String, action: String, onConfirm: (String) -> Unit, onDismiss: () -> Unit) {
    var value by remember { mutableStateOf(initial) }
    val problem = FilesModel.nameProblem(value)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                OutlinedTextField(value = value, onValueChange = { value = it }, singleLine = true, label = { Text("Name") })
                if (problem != null && value.isNotEmpty()) Text(problem, style = Fm.text.bodySmall, color = Fm.colors.danger, modifier = Modifier.padding(top = FmSpace.x1))
            }
        },
        confirmButton = { TextButton(onClick = { onDismiss(); onConfirm(value.trim()) }, enabled = problem == null) { Text(action) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun PasswordPrompt(share: SavedShare, onConfirm: (String, Boolean) -> Unit, onDismiss: () -> Unit) {
    var value by remember { mutableStateOf("") }
    var keep by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(share.title) },
        text = {
            Column {
                Text(if (share.user.isEmpty()) "Password" else "Password for ${share.user}", style = Fm.text.bodyMedium)
                OutlinedTextField(value = value, onValueChange = { value = it }, singleLine = true, visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, autoCorrectEnabled = false), modifier = Modifier.padding(top = FmSpace.x2))
                Row(Modifier.clickable { keep = !keep }.padding(top = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    Text("Remember password", style = Fm.text.bodyMedium, modifier = Modifier.weight(1f))
                    FmSwitch(checked = keep, onCheckedChange = { keep = it })
                }
            }
        },
        confirmButton = { TextButton(onClick = { onDismiss(); onConfirm(value, keep) }) { Text("Connect") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

private fun displayName(context: Context, uri: Uri): String {
    val tree = runCatching { DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri)) }.getOrNull()
    val target = tree ?: uri
    return runCatching {
        context.contentResolver.query(target, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { if (it.moveToFirst()) it.getString(0) else null }
    }.getOrNull() ?: uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':') ?: "Folder"
}

private fun mimeOf(name: String): String = MimeTypeMap.getSingleton().getMimeTypeFromExtension(FilesModel.extension(name)) ?: "application/octet-stream"

private suspend fun exportCopy(context: Context, source: FileSource, item: FileItem, progress: (Long, Long) -> Boolean): Uri = withContext(Dispatchers.IO) {
    val folder = File(store.tempDir, UUID.randomUUID().toString()).also { it.mkdirs() }
    val target = File(folder, item.name.replace('/', '_'))
    target.outputStream().use { source.readTo(item.path, it, Long.MAX_VALUE, progress) }
    FileProvider.getUriForFile(context, "${context.packageName}.composer.files", target)
}

class FilesHomeScreen : Screen() {
    override val title: String get() = "Files"

    private var message by mutableStateOf("")
    private var messageIsError by mutableStateOf(false)
    private var passwordFor by mutableStateOf<SavedShare?>(null)
    private var menuShare by mutableStateOf<SavedShare?>(null)
    private var menuFolder by mutableStateOf<SavedFolder?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val addFolder = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri == null) return@rememberLauncherForActivityResult
            val folder = store.addFolder(uri, displayName(context, uri))
            navigator.push(FolderScreen(store.folderSource(folder), ""))
        }
        passwordFor?.let { share ->
            PasswordPrompt(share, { password, remember ->
                val updated = share.copy(remember = remember)
                store.saveShare(updated, password)
                store.forget(updated)
                navigator.push(FolderScreen(store.shareSource(updated, password), "", updated))
            }, { passwordFor = null })
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator)
            LazyColumn(Modifier.fillMaxSize()) {
                if (message.isNotEmpty()) item(key = "message") { Line(message, messageIsError) }
                item(key = "device") { Section("This phone") }
                item(key = "phone") {
                    FmListRow(FilesStore.PHONE_TITLE, subtitle = "Files kept in ForkMesh on this phone", leading = { FmIcon("phone_android", tint = Fm.colors.textSecondary) },
                        onTap = { navigator.push(FolderScreen(store.phone, "")) })
                }
                items(store.folders, key = { "folder-${it.uri}" }) { folder ->
                    Box {
                        FmListRow(folder.name, subtitle = "Folder on this phone", leading = { FmIcon("folder", tint = Fm.colors.textSecondary) },
                            onTap = { navigator.push(FolderScreen(store.folderSource(folder), "")) }, onLongPress = { menuFolder = folder })
                        DropdownMenu(expanded = menuFolder == folder, onDismissRequest = { menuFolder = null }) {
                            DropdownMenuItem(text = { Text("Remove") }, onClick = {
                                menuFolder = null
                                store.removeFolder(folder)
                            })
                        }
                    }
                }
                item(key = "add-folder") {
                    FmListRow("Add folder…", subtitle = "Downloads, a memory card or another app's storage", leading = { FmIcon("add", tint = Fm.colors.accent) }, showChevron = false,
                        onTap = { addFolder.launch(null) })
                }
                item(key = "network") { Section("Network shares") }
                items(store.shares, key = { "share-${it.id}" }) { share ->
                    Box {
                        FmListRow(share.title, subtitle = share.url + if (share.guest) " · guest" else if (share.user.isNotEmpty()) " · ${share.user}" else "",
                            leading = { FmIcon("dns", tint = Fm.colors.textSecondary) },
                            onTap = {
                                if (store.needsPassword(share)) passwordFor = share else navigator.push(FolderScreen(store.shareSource(share), "", share))
                            },
                            onLongPress = { menuShare = share })
                        DropdownMenu(expanded = menuShare == share, onDismissRequest = { menuShare = null }) {
                            DropdownMenuItem(text = { Text("Edit") }, onClick = {
                                menuShare = null
                                navigator.push(AddShareScreen(share.host, share.share, share))
                            })
                            DropdownMenuItem(text = { Text("Remove") }, onClick = {
                                menuShare = null
                                store.removeShare(share)
                            })
                        }
                    }
                }
                items(store.routerShares, key = { "router-${it.host}-${it.share}" }) { share ->
                    FmListRow(share.title, subtitle = "smb://${share.host}/${share.share}/" + if (share.guests) " · guests allowed" else "",
                        leading = { FmIcon("router", tint = Fm.colors.textSecondary) },
                        onTap = { navigator.push(FilesFeature.browseShare(share.host, share.share, share.guests)) })
                }
                item(key = "add-share") {
                    FmListRow("Add network share", subtitle = "A shared folder on a router, NAS or computer (SMB)", leading = { FmIcon("add", tint = Fm.colors.accent) }, showChevron = false,
                        onTap = { navigator.push(AddShareScreen()) })
                }
                item(key = "end") { Spacer(Modifier.height(FmSpace.x6)) }
            }
        }
    }
}

class AddShareScreen(host: String = "", share: String = "", private val existing: SavedShare? = null) : Screen() {
    override val title: String get() = if (existing == null) "Add network share" else "Edit network share"

    private var address by mutableStateOf(if (existing != null && existing.port != 445) "${existing.host}:${existing.port}" else host)
    private var shareName by mutableStateOf(share)
    private var user by mutableStateOf(existing?.user ?: "")
    private var password by mutableStateOf("")
    private var guest by mutableStateOf(existing?.guest ?: false)
    private var remember by mutableStateOf(existing?.remember ?: true)
    private var busy by mutableStateOf(false)
    private var message by mutableStateOf("")
    private var messageIsError by mutableStateOf(false)
    private var found by mutableStateOf<List<String>>(emptyList())

    private fun say(text: String, isError: Boolean = false) {
        message = text
        messageIsError = isError
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        fun parsed() = FilesModel.parseAddress(address)
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator, pluginMenu = false)
            if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
            LazyColumn(Modifier.fillMaxSize().imePadding()) {
                item(key = "form") {
                    Column {
                        Field("Address", address, { address = it }, hint = "192.168.1.1 or nas.local")
                        Field("Share", shareName, { shareName = it }, hint = "media")
                        Row(Modifier.padding(horizontal = FmSpace.x2)) {
                            TextButton(enabled = !busy, onClick = {
                                val target = parsed()
                                if (!target.valid) {
                                    say(target.error, true)
                                    return@TextButton
                                }
                                busy = true
                                say("Looking for shares on ${target.host}…")
                                scope.launch {
                                    try {
                                        found = withContext(Dispatchers.IO) { store.findShares(target.host, target.port, user, password, guest) }
                                        say(if (found.isEmpty()) "${target.host} lists no shared folders. Type the share name." else "Choose a share.")
                                    } catch (cancelled: CancellationException) {
                                        throw cancelled
                                    } catch (problem: Exception) {
                                        say("${problemText(problem)} Type the share name.", true)
                                    } finally {
                                        busy = false
                                    }
                                }
                            }) { Text("Find shares") }
                        }
                        found.forEach { name ->
                            Text(name, style = Fm.text.bodyMedium, color = if (name == shareName) Fm.colors.accent else Fm.colors.textPrimary,
                                modifier = Modifier.fillMaxWidth().clickable { shareName = name }.padding(horizontal = FmSpace.x5, vertical = FmSpace.x2))
                        }
                        Toggle("Sign in as guest", guest) { guest = it }
                        if (!guest) {
                            Field("User", user, { user = it }, hint = "user or DOMAIN\\user")
                            Field("Password", password, { password = it }, secret = true)
                            Toggle("Remember password", remember) { remember = it }
                        }
                        if (message.isNotEmpty()) Line(message, messageIsError)
                        Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            Button(enabled = !busy, onClick = {
                                val target = parsed()
                                if (!target.valid) {
                                    say(target.error, true)
                                    return@Button
                                }
                                val name = shareName.trim().ifEmpty { target.share }
                                if (name.isEmpty()) {
                                    say("Enter the share name, or press Find shares.", true)
                                    return@Button
                                }
                                val saved = store.newShare(target.host, name, user, guest, remember, target.port).let { if (existing != null) it.copy(id = existing.id) else it }
                                existing?.let(store::forget)
                                store.forget(saved)
                                val typed = password.ifEmpty { existing?.let(store::rememberedPassword) ?: "" }
                                val source = store.shareSource(saved, typed)
                                busy = true
                                say("Connecting to ${saved.title}…")
                                scope.launch {
                                    try {
                                        withContext(Dispatchers.IO) { source.list(target.path) }
                                        store.saveShare(saved, typed)
                                        navigator.replace(FolderScreen(source, "", saved))
                                    } catch (cancelled: CancellationException) {
                                        throw cancelled
                                    } catch (problem: Exception) {
                                        store.forget(saved)
                                        say(problemText(problem), true)
                                    } finally {
                                        busy = false
                                    }
                                }
                            }) { Text("Connect") }
                            TextButton(onClick = { navigator.pop() }) { Text("Cancel") }
                        }
                        Text(
                            "Shares are reached directly over your local network (SMB 2.1). Passwords you choose to remember are kept encrypted on this phone.",
                            style = Fm.text.bodySmall, color = Fm.colors.textTertiary, modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                        )
                    }
                }
            }
        }
    }
}

class FolderScreen(val source: FileSource, val path: String, private val share: SavedShare? = null) : Screen() {
    override val title: String get() = if (FilesModel.parts(path).isEmpty()) source.title else FilesModel.name(path)

    private var items by mutableStateOf<List<FileItem>?>(null)
    private var loading by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var message by mutableStateOf("")
    private var messageIsError by mutableStateOf(false)
    private var busy by mutableStateOf("")
    private var fraction by mutableFloatStateOf(-1f)
    @Volatile
    private var cancelled = false
    private var filter by mutableStateOf("")
    private var searching by mutableStateOf(false)
    private var menuFor by mutableStateOf<FileItem?>(null)
    private var renaming by mutableStateOf<FileItem?>(null)
    private var deleting by mutableStateOf<FileItem?>(null)
    private var info by mutableStateOf<FileItem?>(null)
    private var creating by mutableStateOf<Boolean?>(null)
    private var downloading by mutableStateOf<FileItem?>(null)
    private var overflow by mutableStateOf(false)
    private var sortMenu by mutableStateOf(false)

    private fun show(text: String, isError: Boolean = false) {
        message = text
        messageIsError = isError
    }

    private suspend fun load() {
        loading = true
        try {
            items = withContext(Dispatchers.IO) { source.list(path) }
            error = ""
        } catch (cancelledJob: CancellationException) {
            throw cancelledJob
        } catch (problem: Exception) {
            if (share != null && problem is SmbException && problem.status == SmbStatus.LOGON_FAILURE) store.forget(share)
            error = problemText(problem)
        } finally {
            loading = false
        }
    }

    private val progress: (Long, Long) -> Boolean = { done, total ->
        fraction = if (total > 0) (done.toFloat() / total).coerceIn(0f, 1f) else -1f
        !cancelled
    }

    private suspend fun task(label: String, done: String?, block: suspend () -> Unit) {
        if (busy.isNotEmpty()) return
        busy = label
        fraction = -1f
        cancelled = false
        try {
            block()
            if (done != null) show(done)
        } catch (cancelledJob: CancellationException) {
            throw cancelledJob
        } catch (problem: Exception) {
            show(problemText(problem), true)
        } finally {
            busy = ""
            fraction = -1f
        }
        load()
    }

    private fun names(): List<String> = items.orEmpty().map { it.name }

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) { if (items == null) load() }
        val upload = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
            if (uris.isEmpty()) return@rememberLauncherForActivityResult
            scope.launch {
                task("Uploading…", if (uris.size == 1) "Uploaded 1 file." else "Uploaded ${uris.size} files.") {
                    val taken = names().toMutableList()
                    for (uri in uris) {
                        val name = FilesModel.freeName(taken, displayName(context, uri).replace('/', '_'))
                        taken += name
                        busy = "Uploading $name…"
                        val length = runCatching { context.contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { if (it.moveToFirst() && !it.isNull(0)) it.getLong(0) else -1L } }.getOrNull() ?: -1L
                        withContext(Dispatchers.IO) {
                            val input = context.contentResolver.openInputStream(uri) ?: throw FileProblem("Access denied.")
                            input.use { source.writeFrom(FilesModel.join(path, name), it, length, progress) }
                        }
                    }
                }
            }
        }
        val download = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { uri ->
            val item = downloading
            downloading = null
            if (uri == null || item == null) return@rememberLauncherForActivityResult
            scope.launch {
                task("Downloading ${item.name}…", "Saved ${item.name}.") {
                    withContext(Dispatchers.IO) {
                        val out = context.contentResolver.openOutputStream(uri, "wt") ?: throw FileProblem("Access denied.")
                        out.use { source.readTo(item.path, it, Long.MAX_VALUE, progress) }
                    }
                }
            }
        }
        fun openExternally(item: FileItem, send: Boolean) {
            scope.launch {
                task("Opening ${item.name}…", null) {
                    val uri = exportCopy(context, source, item, progress)
                    val intent = if (send) Intent(Intent.ACTION_SEND).setType(mimeOf(item.name)).putExtra(Intent.EXTRA_STREAM, uri)
                    else Intent(Intent.ACTION_VIEW).setDataAndType(uri, mimeOf(item.name))
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    try {
                        context.startActivity(Intent.createChooser(intent, item.name))
                    } catch (_: ActivityNotFoundException) {
                        show("No app on this phone opens ${item.name}.", true)
                    }
                }
            }
        }
        fun open(item: FileItem) {
            when {
                item.isFolder -> navigator.push(FolderScreen(source, item.path, share))
                item.kind == FileKind.Image && item.size <= FilesModel.MAX_OPEN_BYTES -> navigator.push(ImageFileScreen(source, item))
                item.kind == FileKind.Text && item.size <= FilesModel.MAX_TEXT_BYTES -> navigator.push(TextFileScreen(source, item))
                item.kind == FileKind.Other && item.size <= FilesModel.MAX_TEXT_BYTES -> scope.launch {
                    var text = false
                    task("Opening ${item.name}…", null) {
                        val head = withContext(Dispatchers.IO) {
                            ByteArrayOutputStream().also { out -> source.readTo(item.path, out, FilesModel.MAX_TEXT_BYTES) }.toByteArray()
                        }
                        text = head.isEmpty() || FilesModel.looksLikeText(head)
                    }
                    if (text) navigator.push(TextFileScreen(source, item)) else openExternally(item, false)
                }
                else -> openExternally(item, false)
            }
        }
        fun paste() {
            val clip = store.clip ?: return
            val name = if (clip.move && clip.source === source && FilesModel.parent(clip.item.path) == path) return else FilesModel.freeName(names(), clip.item.name)
            scope.launch {
                task(if (clip.move) "Moving ${clip.item.name}…" else "Copying ${clip.item.name}…", if (clip.move) "Moved ${clip.item.name}." else "Copied ${clip.item.name}.") {
                    withContext(Dispatchers.IO) {
                        val step: (String) -> Boolean = { current ->
                            busy = if (clip.move) "Moving $current…" else "Copying $current…"
                            !cancelled
                        }
                        if (clip.move) FileTransfer.move(clip.source, clip.item.path, source, path, name, store::temp, step)
                        else FileTransfer.copy(clip.source, clip.item.path, source, path, name, store::temp, step)
                    }
                    if (clip.move) store.clip = null
                }
            }
        }
        renaming?.let { item ->
            NamePrompt("Rename", item.name, "Rename", { name ->
                scope.launch { task("Renaming…", null) { withContext(Dispatchers.IO) { source.rename(item.path, FilesModel.join(path, name)) } } }
            }, { renaming = null })
        }
        creating?.let { folder ->
            NamePrompt(if (folder) "New folder" else "New file", FilesModel.freeName(names(), if (folder) "New folder" else "New file.txt"), "Create", { name ->
                scope.launch {
                    task("Creating $name…", null) {
                        withContext(Dispatchers.IO) {
                            if (folder) source.mkdir(FilesModel.join(path, name))
                            else if (name in names()) throw FileProblem(SmbStatus.message(SmbStatus.OBJECT_NAME_COLLISION))
                            else source.writeFrom(FilesModel.join(path, name), ByteArray(0).inputStream(), 0)
                        }
                    }
                }
            }, { creating = null })
        }
        deleting?.let { item ->
            AlertDialog(
                onDismissRequest = { deleting = null },
                title = { Text(FilesModel.deletePrompt(item)) },
                text = { Text(if (share != null) "It is deleted from the network share. This cannot be undone." else "This cannot be undone.") },
                confirmButton = {
                    TextButton(onClick = {
                        deleting = null
                        scope.launch { task("Deleting ${item.name}…", "Deleted ${item.name}.") { withContext(Dispatchers.IO) { source.delete(item.path) } } }
                    }) { Text("Delete", color = Fm.colors.danger) }
                },
                dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
            )
        }
        info?.let { item ->
            val where = source.location(item.path)
            AlertDialog(
                onDismissRequest = { info = null },
                title = { Text(item.name) },
                text = {
                    Column {
                        listOf(
                            "Kind" to FilesModel.kindLabel(item),
                            "Size" to if (item.isFolder) "" else "${FilesModel.formatSize(item.size)} (${item.size} bytes)",
                            "Modified" to FilesModel.formatDate(item.modified),
                            "Where" to where,
                        ).filter { it.second.isNotEmpty() }.forEach { (label, value) ->
                            Text(label, style = Fm.text.bodySmall, color = Fm.colors.textTertiary, modifier = Modifier.padding(top = FmSpace.x2))
                            Text(value, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                        }
                    }
                },
                confirmButton = { TextButton(onClick = { info = null }) { Text("Done") } },
                dismissButton = { TextButton(onClick = { copyToClipboard(context, where); info = null }) { Text("Copy path") } },
            )
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator, pluginMenu = false) {
                Box {
                    TextButton(onClick = { sortMenu = true }) { Text(store.sort.label) }
                    DropdownMenu(expanded = sortMenu, onDismissRequest = { sortMenu = false }) {
                        FileSort.entries.forEach { sort ->
                            DropdownMenuItem(text = { Text(if (sort == store.sort) "${sort.label} ✓" else sort.label) }, onClick = {
                                sortMenu = false
                                store.updateSort(sort)
                            })
                        }
                    }
                }
                Box {
                    TextButton(onClick = { overflow = true }) { FmIcon("more_vert", tint = Fm.colors.textPrimary) }
                    DropdownMenu(expanded = overflow, onDismissRequest = { overflow = false }) {
                        DropdownMenuItem(text = { Text("New folder") }, onClick = { overflow = false; creating = true })
                        DropdownMenuItem(text = { Text("New file") }, onClick = { overflow = false; creating = false })
                        DropdownMenuItem(text = { Text("Upload") }, onClick = { overflow = false; upload.launch(arrayOf("*/*")) })
                        store.clip?.let { clip ->
                            DropdownMenuItem(text = { Text("Paste ${clip.item.name}") }, onClick = { overflow = false; paste() })
                        }
                        DropdownMenuItem(text = { Text("Filter") }, onClick = { overflow = false; searching = !searching; if (!searching) filter = "" })
                        DropdownMenuItem(text = { Text(if (store.showHidden) "Hide hidden files" else "Show hidden files") }, onClick = {
                            overflow = false
                            store.updateShowHidden(!store.showHidden)
                        })
                        DropdownMenuItem(text = { Text("Refresh") }, onClick = { overflow = false; scope.launch { load() } })
                    }
                }
            }
            Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x3, vertical = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
                val crumbs = FilesModel.crumbs(source.title, path)
                crumbs.forEachIndexed { index, (label, target) ->
                    val last = index == crumbs.lastIndex
                    Text(label, style = Fm.text.bodySmall, color = if (last) Fm.colors.textPrimary else Fm.colors.accent, maxLines = 1,
                        modifier = Modifier.clickable(enabled = !last) {
                            while (true) {
                                val top = navigator.top as? FolderScreen ?: break
                                if (top.source !== source || FilesModel.parts(top.path).size <= FilesModel.parts(target).size) break
                                if (!navigator.pop()) break
                            }
                            val top = navigator.top as? FolderScreen
                            if (top == null || top.source !== source || top.path != target) navigator.push(FolderScreen(source, target, share))
                        }.padding(vertical = FmSpace.x1))
                    if (!last) Text("  ›  ", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                }
            }
            if (searching) {
                OutlinedTextField(filter, { filter = it }, singleLine = true, placeholder = { Text("Filter this folder") },
                    modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1))
            }
            if (busy.isNotEmpty()) {
                Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
                    Text(busy, style = Fm.text.bodySmall, color = Fm.colors.textSecondary, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                    TextButton(onClick = { cancelled = true }) { Text("Cancel") }
                }
                if (fraction >= 0f) LinearProgressIndicator(progress = { fraction }, modifier = Modifier.fillMaxWidth())
                else LinearProgressIndicator(Modifier.fillMaxWidth())
            }
            if (message.isNotEmpty()) Line(message, messageIsError)
            FmPullToRefreshBox(isRefreshing = loading, onRefresh = { scope.launch { load() } }, modifier = Modifier.fillMaxSize()) {
                val shown = FilesModel.arrange(items.orEmpty(), store.sort, store.showHidden, filter)
                when {
                    error.isNotEmpty() -> FmEmptyState("error_outline", "Could not open this folder", message = error)
                    items == null -> Box(Modifier.fillMaxSize())
                    shown.isEmpty() -> FmEmptyState("folder_open_outlined", if (filter.isBlank()) FilesModel.EMPTY_FOLDER else "Nothing here matches \"${filter.trim()}\".")
                    else -> LazyColumn(Modifier.fillMaxSize()) {
                        items(shown, key = { it.path }) { item ->
                            Box {
                                FmListRow(item.name, subtitle = FilesModel.detail(item), leading = { FmIcon(iconFor(item), tint = if (item.isFolder) Fm.colors.accent else Fm.colors.textSecondary) },
                                    showChevron = item.isFolder, onTap = { open(item) }, onLongPress = { menuFor = item })
                                DropdownMenu(expanded = menuFor == item, onDismissRequest = { menuFor = null }) {
                                    @Composable
                                    fun act(label: String, action: () -> Unit) = DropdownMenuItem(text = { Text(label) }, onClick = { menuFor = null; action() })
                                    act("Open") { open(item) }
                                    act("Rename") { renaming = item }
                                    act("Copy") { store.clip = FilesClip(source, item, false); show("Copied ${item.name}. Open a folder and choose Paste.") }
                                    act("Move") { store.clip = FilesClip(source, item, true); show("Moving ${item.name}. Open a folder and choose Paste.") }
                                    if (!item.isFolder) {
                                        act("Download") { downloading = item; download.launch(item.name) }
                                        act("Share") { openExternally(item, true) }
                                    }
                                    act("Info") { info = item }
                                    DropdownMenuItem(text = { Text("Delete", color = Fm.colors.danger) }, onClick = { menuFor = null; deleting = item })
                                }
                            }
                            HorizontalDivider(color = Fm.colors.border)
                        }
                        item(key = "end") { Spacer(Modifier.height(FmSpace.x6).windowInsetsPadding(WindowInsets.navigationBars)) }
                    }
                }
            }
        }
    }
}

class TextFileScreen(private val source: FileSource, private val item: FileItem) : Screen() {
    override val title: String get() = item.name

    private val editor = CodeEditorState(codeLangForPath(item.name))
    private var original by mutableStateOf<String?>(null)
    private var error by mutableStateOf("")
    private var status by mutableStateOf("")
    private var saving by mutableStateOf(false)

    private val dirty: Boolean get() = original != null && editor.text != original

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            if (original != null) return@LaunchedEffect
            try {
                val bytes = withContext(Dispatchers.IO) { ByteArrayOutputStream().also { source.readTo(item.path, it, FilesModel.MAX_TEXT_BYTES) }.toByteArray() }
                if (bytes.isNotEmpty() && !FilesModel.looksLikeText(bytes)) {
                    error = "${item.name} is not a text file."
                    return@LaunchedEffect
                }
                val text = String(bytes, Charsets.UTF_8)
                editor.reset(text)
                original = text
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (problem: Exception) {
                error = problemText(problem)
            }
        }
        fun save() {
            if (!dirty || saving) return
            val text = editor.text
            saving = true
            status = "Saving…"
            scope.launch {
                try {
                    val bytes = text.toByteArray(Charsets.UTF_8)
                    withContext(Dispatchers.IO) { source.writeFrom(item.path, bytes.inputStream(), bytes.size.toLong()) }
                    original = text
                    status = "Saved to ${source.location(item.path)}."
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (problem: Exception) {
                    status = problemText(problem)
                } finally {
                    saving = false
                }
            }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator, pluginMenu = false) {
                TextButton(onClick = ::save, enabled = dirty && !saving) { Text("Save") }
            }
            Box(Modifier.weight(1f)) {
                when {
                    error.isNotEmpty() -> FmEmptyState("error_outline", "Could not open ${item.name}", message = error)
                    original == null -> LinearProgressIndicator(Modifier.fillMaxWidth())
                    else -> FmCodeEditor(editor)
                }
            }
            if (original != null && error.isEmpty()) {
                Column(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).windowInsetsPadding(WindowInsets.navigationBars).imePadding()) {
                    EditorStatusLine(editor.caret, editor.focused, dirty, source.title)
                    if (status.isNotEmpty()) Text(status, style = Fm.text.bodySmall, color = Fm.colors.textSecondary, modifier = Modifier.padding(horizontal = FmSpace.x3, vertical = FmSpace.x1))
                    if (editor.focused) CodeKeyBar(editor)
                }
            }
        }
    }
}

class ImageFileScreen(private val source: FileSource, private val item: FileItem) : Screen() {
    override val title: String get() = item.name

    private var bitmap by mutableStateOf<android.graphics.Bitmap?>(null)
    private var error by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        LaunchedEffect(Unit) {
            if (bitmap != null) return@LaunchedEffect
            try {
                bitmap = withContext(Dispatchers.IO) {
                    val bytes = ByteArrayOutputStream().also { source.readTo(item.path, it, FilesModel.MAX_OPEN_BYTES) }.toByteArray()
                    BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                } ?: run {
                    error = "This picture cannot be shown here."
                    null
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (problem: Exception) {
                error = problemText(problem)
            }
        }
        var scale by remember { mutableFloatStateOf(1f) }
        var offsetX by remember { mutableFloatStateOf(0f) }
        var offsetY by remember { mutableFloatStateOf(0f) }
        val transform = rememberTransformableState { _, zoom, pan, _ ->
            scale = (scale * zoom).coerceIn(1f, 8f)
            offsetX = if (scale == 1f) 0f else offsetX + pan.x
            offsetY = if (scale == 1f) 0f else offsetY + pan.y
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator, pluginMenu = false)
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                val shown = bitmap
                when {
                    error.isNotEmpty() -> FmEmptyState("image", "Could not open ${item.name}", message = error)
                    shown == null -> LinearProgressIndicator(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4))
                    else -> Image(
                        shown.asImageBitmap(),
                        contentDescription = item.name,
                        contentScale = ContentScale.Fit,
                        modifier = Modifier.fillMaxSize().transformable(transform).graphicsLayer(scaleX = scale, scaleY = scale, translationX = offsetX, translationY = offsetY),
                    )
                }
            }
        }
    }
}
