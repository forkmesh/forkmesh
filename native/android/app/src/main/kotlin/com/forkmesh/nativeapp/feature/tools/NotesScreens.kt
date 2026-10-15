package com.forkmesh.nativeapp.feature.tools

import com.forkmesh.nativeapp.feature.market.rememberScreenPluginMenu
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.notes.NotesApi
import com.forkmesh.nativeapp.notes.NotesLogic
import com.forkmesh.nativeapp.notes.apiErrorText
import com.forkmesh.nativeapp.notes.jsonInt
import com.forkmesh.nativeapp.notes.jsonObjects
import com.forkmesh.nativeapp.notes.jsonString
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal fun notesApi(): NotesApi? = Services.get<ApiService>()?.let(::NotesApi)

class NotesScreen : Screen() {
    override val title: String get() = "Notes"

    private var notes by mutableStateOf<List<JsonObject>>(emptyList())
    private var loading by mutableStateOf(true)
    private var error by mutableStateOf("")
    private var query by mutableStateOf("")
    private var generation by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        LaunchedEffect(generation) {
            val api = notesApi() ?: return@LaunchedEffect
            loading = true
            error = ""
            try {
                notes = withContext(Dispatchers.IO) { api.list() }
            } catch (thrown: Exception) {
                error = apiErrorText(thrown, "Notes are unavailable right now.")
            }
            loading = false
        }
        val top = navigator.top
        LaunchedEffect(top) { if (top === this@NotesScreen) generation++ }
        Box(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar("Notes", navigator) {
                    FmIconButton("refresh", "Refresh", onPressed = { generation++ }, color = Fm.colors.textPrimary)
                }
                when {
                    loading && notes.isEmpty() -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    error.isNotEmpty() && notes.isEmpty() -> FmEmptyState("sticky_note_2_outlined", "Notes are unavailable", message = error)
                    notes.isEmpty() -> FmEmptyState(
                        "sticky_note_2_outlined",
                        "No notes yet",
                        message = "Notes are private markdown documents that follow your account across web, desktop, and mobile.",
                    )
                    else -> LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 96.dp)) {
                        item {
                            FmTextField(
                                value = query,
                                onValueChange = { query = it },
                                hint = "Search notes",
                                leading = { FmIcon("search", size = 20.dp, tint = Fm.colors.textTertiary) },
                                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                            )
                        }
                        items(NotesLogic.filter(notes, query), key = { jsonString(it["id"]) }) { note ->
                            FmListRow(
                                title = jsonString(note["title"], "Untitled note"),
                                subtitle = NotesLogic.rowSubtitle(note),
                                leading = { FmIconTile(if (note["visibility"] == "public") "public" else "sticky_note_2_outlined") },
                                onTap = { navigator.push(NoteEditorScreen(jsonString(note["id"]))) },
                            )
                            FmRowDivider()
                        }
                    }
                }
            }
            FmExtendedFloatingActionButton(
                "New note",
                onClick = { navigator.push(NoteEditorScreen(null)) },
                modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
                icon = "add",
            )
        }
    }
}

class NoteEditorScreen(private val noteId: String?) : Screen() {
    override val title: String get() = "Note"

    private var note by mutableStateOf<JsonObject?>(null)
    private var loading by mutableStateOf(noteId != null)
    private var saving by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var titleText by mutableStateOf("")
    private var markdown by mutableStateOf("")
    private var savedTitle = ""
    private var savedMarkdown = ""
    private var confirmDiscard by mutableStateOf(false)
    private var confirmDelete by mutableStateOf(false)
    private var showHistory by mutableStateOf(false)
    private var showShare by mutableStateOf(false)
    private var menu by mutableStateOf(false)
    private var navigatorRef: Navigator? = null

    private val id: String? get() = note?.let { jsonString(it["id"]) } ?: noteId
    private val canEdit: Boolean get() = NotesLogic.canEdit(note)
    private val dirty: Boolean get() = titleText != savedTitle || markdown != savedMarkdown

    private fun apply(value: JsonObject, keepText: Boolean = false) {
        note = value
        savedTitle = jsonString(value["title"])
        savedMarkdown = jsonString(value["markdown"])
        if (!keepText) {
            titleText = savedTitle
            markdown = savedMarkdown
        }
    }

    override fun onBack(): Boolean {
        if (dirty && canEdit) {
            confirmDiscard = true
            return true
        }
        return false
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        navigatorRef = navigator
        val scope = rememberCoroutineScope()
        val status = rememberFmStatus()
        val api = notesApi() ?: return
        LaunchedEffect(Unit) {
            if (noteId == null) return@LaunchedEffect
            try {
                apply(withContext(Dispatchers.IO) { api.get(noteId) })
            } catch (thrown: Exception) {
                error = apiErrorText(thrown, "This note could not be opened.")
            }
            loading = false
        }
        fun save() {
            if (saving || !canEdit) return
            saving = true
            scope.launch {
                try {
                    val current = id
                    val saved = withContext(Dispatchers.IO) {
                        if (current == null) api.create(titleText, markdown) else api.save(current, jsonInt(note?.get("version")), titleText, markdown)
                    }
                    apply(saved)
                    status.show("Note saved")
                } catch (thrown: Exception) {
                    status.error(NotesLogic.saveError(thrown))
                }
                saving = false
            }
        }
        fun setVisibility(visibility: String) {
            val current = id ?: return
            scope.launch {
                try {
                    apply(withContext(Dispatchers.IO) { api.setVisibility(current, jsonInt(note?.get("version")), visibility) }, keepText = true)
                    status.show(if (visibility == "public") "Note published" else "Note is private again")
                } catch (thrown: Exception) {
                    status.error(apiErrorText(thrown, "Visibility was not changed."))
                }
            }
        }
        val existing = id != null
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase).imePadding()) {
            val plugin = rememberScreenPluginMenu()
            ToolsAppBar(if (existing) (if (canEdit) "Edit note" else "Note") else "New note", navigator, pluginMenu = false) {
                if (canEdit && !loading && error.isEmpty()) {
                    FmTextButton(onClick = { save() }, enabled = !saving && (!existing || dirty)) { Text(if (saving) "Saving…" else "Save") }
                }
                val owned = existing && note != null
                if (owned || plugin != null) {
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            if (owned) {
                                FmDropdownMenuItem(text = "History", onClick = { menu = false; showHistory = true })
                                com.forkmesh.nativeapp.feature.planning.NoteConvertItems(note, id, { titleText }, { markdown }, status, scope) { menu = false }
                                if (NotesLogic.role(note) == "owner") {
                                    FmDropdownMenuItem(text = "Share", onClick = { menu = false; showShare = true })
                                    if (note?.get("visibility") == "public") {
                                        FmDropdownMenuItem(text = "Make private", onClick = { menu = false; setVisibility("private") })
                                    } else {
                                        FmDropdownMenuItem(text = "Publish", onClick = { menu = false; setVisibility("public") })
                                    }
                                    FmDropdownMenuItem(text = "Delete", onClick = { menu = false; confirmDelete = true })
                                }
                            }
                            plugin?.Items(close = { menu = false }, divided = owned)
                        }
                    }
                    plugin?.Host()
                }
            }
            when {
                loading -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                error.isNotEmpty() -> FmEmptyState("sticky_note_2_outlined", "Note unavailable", message = error)
                else -> Column(Modifier.fillMaxSize().padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp)) {
                    FmTextField(
                        value = titleText,
                        onValueChange = { titleText = it.take(200) },
                        readOnly = !canEdit,
                        hint = "Title",
                        textStyle = Fm.text.titleLarge,
                    )
                    Text(NotesLogic.editorMeta(note), style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                    FmInlineStatus(status, Modifier.padding(top = 4.dp))
                    Spacer(Modifier.height(8.dp))
                    FmTextField(
                        value = markdown,
                        onValueChange = { markdown = it },
                        readOnly = !canEdit,
                        hint = "Write in markdown…",
                        minLines = 12,
                        maxLines = Int.MAX_VALUE,
                        textStyle = Fm.mono(size = Fm.text.bodyMedium.fontSize),
                        modifier = Modifier.weight(1f).fillMaxWidth(),
                    )
                }
            }
        }
        if (confirmDiscard) {
            FmConfirmDialog(
                title = "Discard changes?",
                body = "Your edits to this note have not been saved.",
                confirmLabel = "Discard",
                onConfirm = {
                    confirmDiscard = false
                    savedTitle = titleText
                    savedMarkdown = markdown
                    navigator.pop()
                },
                onDismissRequest = { confirmDiscard = false },
            )
        }
        if (confirmDelete) {
            FmConfirmDialog(
                title = "Delete note?",
                body = "This removes the note and its history for everyone it is shared with.",
                confirmLabel = "Delete",
                destructive = true,
                onConfirm = {
                    confirmDelete = false
                    val current = id ?: return@FmConfirmDialog
                    scope.launch {
                        try {
                            withContext(Dispatchers.IO) { api.delete(current) }
                            savedTitle = titleText
                            savedMarkdown = markdown
                            navigator.pop()
                        } catch (thrown: Exception) {
                            status.error(apiErrorText(thrown, "The note was not deleted."))
                        }
                    }
                },
                onDismissRequest = { confirmDelete = false },
            )
        }
        if (showHistory) {
            val current = id
            if (current != null) {
                NoteHistorySheet(api, current, jsonInt(note?.get("version")), canEdit, onDismiss = { showHistory = false }) { restored ->
                    showHistory = false
                    apply(restored)
                    status.show("Version restored")
                }
            }
        }
        if (showShare) {
            val current = id
            if (current != null) {
                NoteShareSheet(api, current, jsonObjects(note?.get("shares"))) {
                    showShare = false
                    scope.launch {
                        try {
                            apply(withContext(Dispatchers.IO) { api.get(current) }, keepText = true)
                        } catch (_: Exception) {
                        }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun NoteHistorySheet(api: NotesApi, noteId: String, currentVersion: Long, canRestore: Boolean, onDismiss: () -> Unit, onRestored: (JsonObject) -> Unit) {
    var versions by remember { mutableStateOf<List<JsonObject>?>(null) }
    var error by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    LaunchedEffect(noteId) {
        try {
            versions = withContext(Dispatchers.IO) { api.versions(noteId) }
        } catch (thrown: Exception) {
            error = apiErrorText(thrown, "History is unavailable.")
        }
    }
    FmModalBottomSheet(onDismissRequest = onDismiss) {
        FmPanelHeader(title = "History")
        FmInlineStatus(status, Modifier.padding(horizontal = 16.dp))
        val list = versions
        when {
            list == null && error.isEmpty() -> Box(Modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
            list == null -> FmEmptyState("history", "History unavailable", message = error)
            else -> LazyColumn(Modifier.fillMaxWidth().height(360.dp)) {
                items(list) { entry ->
                    val version = jsonInt(entry["version"])
                    FmListRow(
                        title = "Version $version",
                        subtitle = relativeTime(jsonInt(entry["createdAt"])),
                        showChevron = false,
                        trailing = when {
                            version == currentVersion -> ({ FmStatusBadge("Current", Fm.colors.accent) })
                            canRestore -> ({
                                FmTextButton(enabled = !busy, onClick = {
                                    busy = true
                                    scope.launch {
                                        try {
                                            onRestored(withContext(Dispatchers.IO) { api.restore(noteId, version, currentVersion) })
                                        } catch (thrown: Exception) {
                                            busy = false
                                            status.error(apiErrorText(thrown, "That version was not restored."))
                                        }
                                    }
                                }) { Text("Restore") }
                            })
                            else -> null
                        },
                    )
                    FmRowDivider(indent = 16.dp)
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun NoteShareSheet(api: NotesApi, noteId: String, initial: List<JsonObject>, onDismiss: () -> Unit) {
    var shares by remember { mutableStateOf(initial) }
    var name by remember { mutableStateOf("") }
    var type by remember { mutableStateOf("user") }
    var role by remember { mutableStateOf("viewer") }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    fun send(block: () -> List<JsonObject>, clear: Boolean) {
        busy = true
        scope.launch {
            try {
                shares = withContext(Dispatchers.IO) { block() }
                if (clear) name = ""
                status.clear()
            } catch (thrown: Exception) {
                status.error(NotesLogic.shareError(thrown, type))
            }
            busy = false
        }
    }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding()) {
            FmPanelHeader(title = "Share", subtitle = "Viewers can read; editors can change the text.")
            if (shares.isEmpty()) {
                Text("Not shared with anyone yet.", modifier = Modifier.padding(16.dp), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
            }
            for (share in shares) {
                val shareType = jsonString(share["type"])
                val shareName = jsonString(share["name"])
                FmListRow(
                    title = shareName,
                    subtitle = "${if (shareType == "organization") "Organization" else "User"} · ${if (share["role"] == "editor") "Editor" else "Viewer"}",
                    showChevron = false,
                    leading = { FmIconTile(if (shareType == "organization") "groups_outlined" else "person_outline") },
                    trailing = {
                        FmIconButton("close", "Stop sharing", onPressed = if (busy) null else ({ send({ api.removeShare(noteId, shareType, shareName) }, false) }))
                    },
                )
            }
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmToneButton("User", onPressed = { type = "user" }, tone = if (type == "user") FmTone.Accent else FmTone.Neutral, filled = type == "user")
                    FmToneButton("Org", onPressed = { type = "organization" }, tone = if (type == "organization") FmTone.Accent else FmTone.Neutral, filled = type == "organization")
                    Spacer(Modifier.width(8.dp))
                    FmToneButton("Viewer", onPressed = { role = "viewer" }, tone = if (role == "viewer") FmTone.Accent else FmTone.Neutral, filled = role == "viewer")
                    FmToneButton("Editor", onPressed = { role = "editor" }, tone = if (role == "editor") FmTone.Accent else FmTone.Neutral, filled = role == "editor")
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(value = name, onValueChange = { name = it }, hint = if (type == "user") "Username" else "Organization", modifier = Modifier.weight(1f))
                    FmTextButton(enabled = !busy && NotesLogic.shareName(name).isNotEmpty(), onClick = {
                        send({ api.addShare(noteId, type, name, role) }, true)
                    }) { Text("Share") }
                }
                FmInlineStatus(status)
            }
        }
    }
}
