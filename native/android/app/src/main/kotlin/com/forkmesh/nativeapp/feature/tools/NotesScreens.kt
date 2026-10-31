package com.forkmesh.nativeapp.feature.tools

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.contextmenu.builder.item
import androidx.compose.foundation.text.contextmenu.modifier.appendTextContextMenuComponents
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.OffsetMapping
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.input.TransformedText
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.docs.DocsMarkdown
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.feature.docs.DocsBlockView
import com.forkmesh.nativeapp.feature.market.rememberScreenPluginMenu
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.net.WebSocketClient
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.notes.NoteAccess
import com.forkmesh.nativeapp.notes.NoteAccessSummary
import com.forkmesh.nativeapp.notes.NoteAnchoring
import com.forkmesh.nativeapp.notes.NoteCommentText
import com.forkmesh.nativeapp.notes.NoteCommentsPayload
import com.forkmesh.nativeapp.notes.NoteConflictException
import com.forkmesh.nativeapp.notes.NoteFilter
import com.forkmesh.nativeapp.notes.NoteLink
import com.forkmesh.nativeapp.notes.NotePeer
import com.forkmesh.nativeapp.notes.NotePresence
import com.forkmesh.nativeapp.notes.NoteQuote
import com.forkmesh.nativeapp.notes.NoteReach
import com.forkmesh.nativeapp.notes.NoteRowInfo
import com.forkmesh.nativeapp.notes.NoteRows
import com.forkmesh.nativeapp.notes.NoteRoomEvent
import com.forkmesh.nativeapp.notes.NoteSaveState
import com.forkmesh.nativeapp.notes.NoteSearch
import com.forkmesh.nativeapp.notes.NoteShareSuggestion
import com.forkmesh.nativeapp.notes.NoteThread
import com.forkmesh.nativeapp.notes.NoteThreads
import com.forkmesh.nativeapp.notes.NotesApi
import com.forkmesh.nativeapp.notes.NotesLogic
import com.forkmesh.nativeapp.notes.apiErrorText
import com.forkmesh.nativeapp.notes.jsonInt
import com.forkmesh.nativeapp.notes.jsonObjects
import com.forkmesh.nativeapp.notes.jsonString
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmNavBar
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSafeInput
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import java.time.ZoneId
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

internal fun notesApi(): NotesApi? = Services.get<ApiService>()?.let(::NotesApi)

private fun notesOrigin(): String = Services.get<ApiService>()?.gitOrigin.orEmpty()

internal val noteViewsSeen = mutableStateMapOf<String, Long>()

private fun markNoteSeen(id: String, updatedAt: Long) {
    if (id.isEmpty() || updatedAt <= 0) return
    noteViewsSeen[id] = maxOf(noteViewsSeen[id] ?: 0L, updatedAt)
}

private fun shareNoteText(context: Context, subject: String, text: String, type: String = "text/plain") {
    val send = Intent(Intent.ACTION_SEND).setType(type).putExtra(Intent.EXTRA_SUBJECT, subject).putExtra(Intent.EXTRA_TEXT, text)
    runCatching { context.startActivity(Intent.createChooser(send, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
}

class NotesScreen : Screen() {
    override val title: String get() = "Notes"

    private var notes by mutableStateOf<List<JsonObject>>(emptyList())
    private var loading by mutableStateOf(true)
    private var error by mutableStateOf("")
    private var query by mutableStateOf("")
    private var filter by mutableStateOf(NoteFilter.All)
    private var answer by mutableStateOf<NoteSearch?>(null)
    private var generation by mutableIntStateOf(0)
    private var menuNote by mutableStateOf<JsonObject?>(null)
    private var shareNote by mutableStateOf<JsonObject?>(null)
    private var deleteNote by mutableStateOf<JsonObject?>(null)
    private val status = FmStatus()

    private fun replaceNote(id: String, change: (JsonObject) -> JsonObject?) {
        fun apply(list: List<JsonObject>) = list.mapNotNull { if (jsonString(it["id"]) == id) change(it) else it }
        notes = apply(notes)
        answer = answer?.let { it.copy(notes = apply(it.notes)) }
    }

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
        LaunchedEffect(NotesLogic.searchQuery(query), notes) {
            val wanted = NotesLogic.searchQuery(query)
            if (wanted.isEmpty()) {
                answer = null
                return@LaunchedEffect
            }
            val api = notesApi() ?: return@LaunchedEffect
            delay(NotesLogic.SEARCH_MS)
            val found = try {
                withContext(Dispatchers.IO) { api.search(wanted) }
            } catch (cancel: CancellationException) {
                throw cancel
            } catch (_: Exception) {
                null
            }
            if (found != null && NotesLogic.searchQuery(query) == wanted) answer = NoteSearch(wanted, found)
        }
        val top = navigator.top
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        LaunchedEffect(top) { if (top === this@NotesScreen) generation++ }
        fun open(note: JsonObject) {
            val id = jsonString(note["id"])
            markNoteSeen(id, jsonInt(note["updatedAt"]))
            navigator.push(NoteEditorScreen(id))
        }
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
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
                    else -> {
                        val me = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()
                        val zone = ZoneId.systemDefault()
                        val now = System.currentTimeMillis()
                        val results = NoteRows.applySeen(NotesLogic.searchResults(notes, query, answer), noteViewsSeen)
                        val counts = NotesLogic.counts(results)
                        val shown = NotesLogic.filter(results, "", filter)
                        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 96.dp)) {
                            item {
                                FmTextField(
                                    value = query,
                                    onValueChange = { query = it },
                                    hint = "Search titles and text",
                                    leading = { FmIcon("search", size = 20.dp, tint = Fm.colors.textTertiary) },
                                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                                )
                                FmSegmentTabBar(
                                    NoteFilter.entries.map { FmSegmentTab(it.label, counts[it] ?: 0) },
                                    NoteFilter.entries.indexOf(filter),
                                    onChanged = { filter = NoteFilter.entries[it] },
                                )
                                FmInlineStatus(status, Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
                            }
                            if (shown.isEmpty()) {
                                item {
                                    Text(
                                        NotesLogic.emptyListText(query, filter, NotesLogic.searchPending(query, answer)),
                                        modifier = Modifier.fillMaxWidth().padding(24.dp),
                                        style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                                    )
                                }
                            }
                            items(shown, key = { jsonString(it["id"]) }) { note ->
                                NoteListRow(note, me, now, zone, onMenu = { menuNote = note }) { open(note) }
                                FmRowDivider()
                            }
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
        val menu = menuNote
        if (menu != null) {
            val me = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()
            NoteActionsSheet(
                NoteAccess.summary(menu, me, ZoneId.systemDefault()),
                onOpen = {
                    menuNote = null
                    open(menu)
                },
                onCopyLink = {
                    menuNote = null
                    copyToClipboard(context, NotesLogic.accessUrl(notesOrigin(), jsonString(menu["id"])))
                    status.show("Link copied")
                },
                onShare = {
                    menuNote = null
                    shareNote = menu
                },
                onDelete = {
                    menuNote = null
                    deleteNote = menu
                },
                onDismiss = { menuNote = null },
            )
        }
        val sharing = shareNote
        val api = notesApi()
        if (sharing != null && api != null) {
            val id = jsonString(sharing["id"])
            NoteShareSheet(api, id, notesOrigin(), jsonString(sharing["title"]), jsonObjects(sharing["shares"]), onChanged = { shares ->
                replaceNote(id) { it + ("shares" to shares) }
            }) {
                shareNote = null
                generation++
            }
        }
        val doomed = deleteNote
        if (doomed != null) {
            val title = jsonString(doomed["title"]).ifBlank { "Untitled note" }
            FmConfirmDialog(
                title = "Delete note?",
                body = "“$title” and its version history will be removed for everyone it's shared with. This can't be undone.",
                confirmLabel = "Delete",
                destructive = true,
                onConfirm = {
                    deleteNote = null
                    val id = jsonString(doomed["id"])
                    val notesApi = notesApi() ?: return@FmConfirmDialog
                    scope.launch {
                        try {
                            withContext(Dispatchers.IO) { notesApi.delete(id) }
                            replaceNote(id) { null }
                            status.show("Deleted “$title”")
                        } catch (cancel: CancellationException) {
                            throw cancel
                        } catch (thrown: Exception) {
                            status.error(apiErrorText(thrown, "The note was not deleted."))
                        }
                    }
                },
                onDismissRequest = { deleteNote = null },
            )
        }
    }
}

class NoteEditorScreen(private val noteId: String?, private val openComment: String? = null) : Screen() {
    override val title: String get() = "Note"

    private val api: NotesApi? = notesApi()
    private val status = FmStatus()
    private val saveLock = Mutex()
    private var note by mutableStateOf<JsonObject?>(null)
    private var loading by mutableStateOf(noteId != null)
    private var error by mutableStateOf("")
    private var titleText by mutableStateOf("")
    private var body by mutableStateOf(TextFieldValue(""))
    private var savedTitle by mutableStateOf("")
    private var savedMarkdown by mutableStateOf("")
    private var saveState by mutableStateOf(NoteSaveState.Saved)
    private var saveDetail by mutableStateOf("")
    private var saving by mutableStateOf(false)
    private var conflict by mutableStateOf<JsonObject?>(null)
    private var preview by mutableStateOf(false)
    private var leaving by mutableStateOf(false)
    private var discarded = false
    private var confirmDiscard by mutableStateOf(false)
    private var confirmDelete by mutableStateOf(false)
    private var showHistory by mutableStateOf(false)
    private var showShare by mutableStateOf(false)
    private var showLinks by mutableStateOf(false)
    private var menu by mutableStateOf(false)
    private var roomGeneration by mutableIntStateOf(0)
    private var room: WebSocketClient? = null
    private var presence: NotePresence? = null
    private var presenceLabel by mutableStateOf("")
    private var peers by mutableStateOf<List<NotePeer>>(emptyList())
    private var typedAt by mutableLongStateOf(0L)
    private var roomOpen by mutableStateOf(false)
    private var showPeople by mutableStateOf(false)
    private var comments by mutableStateOf(NoteCommentsPayload())
    private var commentsLoaded by mutableStateOf(false)
    private var commentsError by mutableStateOf("")
    private var commentsSignal by mutableIntStateOf(0)
    private var commentBusy by mutableStateOf(false)
    private val commentStatus = FmStatus()
    private var showComments by mutableStateOf(false)
    private var composing by mutableStateOf(false)
    private var commentDraft by mutableStateOf<NoteQuote?>(null)
    private var focusThread by mutableStateOf<String?>(null)
    private var pendingComment: String? = openComment
    private var highlightThread by mutableStateOf<String?>(null)
    private val editorFocus = FocusRequester()

    private val markdown: String get() = body.text
    private val id: String? get() = note?.let { jsonString(it["id"]) } ?: noteId
    private val version: Long get() = jsonInt(note?.get("version"))
    private val canEdit: Boolean get() = NotesLogic.canEdit(note)
    private val dirty: Boolean get() = titleText != savedTitle || markdown != savedMarkdown
    private val me: String get() = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()

    private fun localState(now: Long = System.currentTimeMillis()): String =
        if (canEdit) NotePresence.localState(typedAt, now) else NotePresence.VIEWING

    private fun edited(next: TextFieldValue) {
        if (next.text != body.text) {
            typedAt = System.currentTimeMillis()
            highlightThread = null
        }
        body = next
    }

    private fun selectThread(thread: NoteThread) {
        val range = thread.anchor ?: return
        if (range.last >= markdown.length) return
        preview = false
        body = body.copy(selection = TextRange(range.first, range.last + 1))
        highlightThread = thread.id
    }

    private fun startComment(quote: NoteQuote?) {
        commentDraft = quote
        composing = true
        focusThread = null
        commentStatus.clear()
        showComments = true
    }

    private fun commentCall(scope: kotlinx.coroutines.CoroutineScope, fallback: String, block: (NotesApi, String) -> NoteCommentsPayload, done: () -> Unit = {}) {
        val notes = api ?: return
        val current = id ?: return
        commentBusy = true
        scope.launch {
            try {
                comments = withContext(Dispatchers.IO) { block(notes, current) }
                commentsLoaded = true
                commentStatus.clear()
                done()
            } catch (cancel: CancellationException) {
                throw cancel
            } catch (thrown: Exception) {
                commentStatus.error(NoteCommentText.error(thrown, fallback))
            }
            commentBusy = false
        }
    }

    private fun apply(value: JsonObject, keepText: Boolean = false) {
        note = value
        savedTitle = jsonString(value["title"])
        savedMarkdown = jsonString(value["markdown"])
        if (!keepText) {
            titleText = savedTitle
            setBody(savedMarkdown)
        }
    }

    private fun setBody(text: String) {
        body = TextFieldValue(text, TextRange(text.length))
    }

    private suspend fun persist(): Boolean = saveLock.withLock {
        val notes = api ?: return@withLock false
        if (!canEdit || conflict != null || discarded) return@withLock conflict == null
        val current = id
        val title = titleText
        val text = markdown
        if (current != null && title == savedTitle && text == savedMarkdown) return@withLock true
        if (current == null && title.isBlank() && text.isBlank()) return@withLock true
        saving = true
        saveState = NoteSaveState.Saving
        val ok = try {
            val saved = withContext(Dispatchers.IO) {
                if (current == null) notes.create(title, text) else notes.save(current, version, title, text)
            }
            note = saved
            savedTitle = title
            savedMarkdown = text
            saveState = if (dirty) NoteSaveState.Dirty else NoteSaveState.Saved
            true
        } catch (clash: NoteConflictException) {
            conflict = clash.latest
            saveState = NoteSaveState.Conflict
            false
        } catch (thrown: Exception) {
            saveDetail = NotesLogic.saveError(thrown)
            saveState = NoteSaveState.Failed
            false
        }
        saving = false
        ok
    }

    private fun saveSoon() {
        AppLifecycle.scope.launch { persist() }
    }

    override fun onBack(): Boolean {
        if (!canEdit || discarded || (!dirty && !saving)) return false
        if (conflict != null) confirmDiscard = true else leaving = true
        return true
    }

    override fun onDisposed() {
        if (canEdit && dirty && conflict == null && !discarded) saveSoon()
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val notes = api ?: return
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val origin = notesOrigin()
        LaunchedEffect(Unit) {
            if (noteId == null || note != null) {
                loading = false
                return@LaunchedEffect
            }
            try {
                apply(withContext(Dispatchers.IO) { notes.get(noteId) })
            } catch (thrown: Exception) {
                error = apiErrorText(thrown, "This note could not be opened.")
            }
            loading = false
        }
        LaunchedEffect(titleText, markdown, version, conflict) {
            if (loading || error.isNotEmpty() || !canEdit || conflict != null || !dirty) return@LaunchedEffect
            if (id == null && titleText.isBlank() && markdown.isBlank()) return@LaunchedEffect
            if (!saving) saveState = NoteSaveState.Dirty
            delay(NotesLogic.AUTOSAVE_MS)
            saveSoon()
        }
        LaunchedEffect(leaving) {
            if (!leaving) return@LaunchedEffect
            val ok = AppLifecycle.scope.async { persist() }.await()
            leaving = false
            if (ok && !dirty) navigator.pop() else confirmDiscard = true
        }
        NoteRoom(notes, scope)
        val openId = id
        val loadedNote = note != null
        val editedAt = jsonInt(note?.get("updatedAt"))
        LaunchedEffect(openId, editedAt) { if (openId != null) markNoteSeen(openId, editedAt) }
        LaunchedEffect(openId, loadedNote) {
            if (noteId == null || openId == null || !loadedNote) return@LaunchedEffect
            try {
                withContext(Dispatchers.IO) { notes.recordView(openId) }
            } catch (cancel: CancellationException) {
                throw cancel
            } catch (_: Exception) {
            }
        }
        LaunchedEffect(openId, loadedNote, commentsSignal) {
            if (openId == null || !loadedNote) return@LaunchedEffect
            if (commentsSignal > 0) delay(NoteThreads.RELOAD_MS)
            try {
                comments = withContext(Dispatchers.IO) { notes.comments(openId) }
                commentsLoaded = true
                commentsError = ""
            } catch (cancel: CancellationException) {
                throw cancel
            } catch (thrown: Exception) {
                commentsError = NoteCommentText.error(thrown, "Comments are unavailable right now.")
            }
            val target = pendingComment ?: return@LaunchedEffect
            if (!commentsLoaded) return@LaunchedEffect
            pendingComment = null
            val thread = NoteThreads.build(comments.comments, markdown).find(target)
            if (thread == null) {
                status.error("That comment was deleted.")
                return@LaunchedEffect
            }
            selectThread(thread)
            composing = false
            focusThread = thread.id
            showComments = true
        }
        val threads = remember(comments, markdown) { NoteThreads.build(comments.comments, markdown) }
        LaunchedEffect(titleText, markdown) {
            val socket = room ?: return@LaunchedEffect
            val live = presence ?: return@LaunchedEffect
            if (!canEdit || !dirty) return@LaunchedEffect
            delay(NotePresence.DRAFT_MS)
            val frame = live.draftFrame(titleText, markdown, version)
            withContext(Dispatchers.IO) { socket.sendText(frame) }
        }
        val exporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/markdown")) { uri: Uri? ->
            if (uri == null) return@rememberLauncherForActivityResult
            val text = NotesLogic.exportMarkdown(titleText, markdown)
            scope.launch {
                val ok = withContext(Dispatchers.IO) {
                    runCatching { context.contentResolver.openOutputStream(uri)?.use { it.write(text.toByteArray()) } != null }.getOrDefault(false)
                }
                if (ok) status.show("Markdown exported") else status.error("The file could not be written.")
            }
        }
        fun setVisibility(visibility: String) {
            val current = id ?: return
            scope.launch {
                try {
                    apply(withContext(Dispatchers.IO) { notes.setVisibility(current, version, visibility) }, keepText = true)
                    status.show(if (visibility == "public") "Published — anyone with the link can read it" else "Note is private again")
                } catch (thrown: Exception) {
                    status.error(apiErrorText(thrown, "Visibility was not changed."))
                }
            }
        }
        fun copy(label: String, url: String) {
            copyToClipboard(context, url)
            status.show("$label copied")
        }
        val existing = id != null
        val owner = NotesLogic.role(note) == "owner"
        val public = note?.get("visibility") == "public"
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas).imePadding()) {
            val plugin = rememberScreenPluginMenu()
            FmNavBar(if (existing) (if (canEdit) "Edit note" else "Note") else "New note", onBack = { navigator.handleBack() }) {
                if (!loading && error.isEmpty()) {
                    val label = if (canEdit) NotesLogic.saveStateLabel(saveState, saveDetail) else NotesLogic.saveStateLabel(NoteSaveState.ReadOnly)
                    FmTextButton(onClick = { saveSoon() }, enabled = canEdit && dirty && !saving && conflict == null) {
                        Text(label, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.widthIn(max = 160.dp))
                    }
                }
                val opened = existing && note != null
                if (opened && error.isEmpty()) {
                    NoteCommentsButton(threads.openCount) {
                        composing = false
                        focusThread = null
                        commentStatus.clear()
                        showComments = true
                    }
                }
                if (opened || plugin != null) {
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            val current = id
                            if (opened && current != null) {
                                FmDropdownMenuItem(text = "Copy link", icon = "link", onClick = {
                                    menu = false
                                    copy("Link", NotesLogic.accessUrl(origin, current))
                                })
                                if (public) {
                                    FmDropdownMenuItem(text = "Copy public link", icon = "public", onClick = {
                                        menu = false
                                        copy("Public link", NotesLogic.publicUrl(origin, current))
                                    })
                                    FmDropdownMenuItem(text = "Open published page", icon = "open_in_new", onClick = {
                                        menu = false
                                        openExternalUrl(context, NotesLogic.publicUrl(origin, current))
                                    })
                                }
                                FmDropdownMenuItem(text = "History", onClick = { menu = false; showHistory = true })
                                if (canEdit) FmDropdownMenuItem(text = "Linked issues and PRs", onClick = { menu = false; showLinks = true })
                                FmDropdownMenuItem(text = "Export Markdown", onClick = {
                                    menu = false
                                    exporter.launch(NotesLogic.exportFileName(titleText))
                                })
                                com.forkmesh.nativeapp.feature.planning.NoteConvertItems(note, id, { titleText }, { markdown }, status, scope) { menu = false }
                                if (owner) {
                                    FmDropdownMenuItem(text = "Share", onClick = { menu = false; showShare = true })
                                    if (public) {
                                        FmDropdownMenuItem(text = "Make private", onClick = { menu = false; setVisibility("private") })
                                    } else {
                                        FmDropdownMenuItem(text = "Publish to the web", onClick = { menu = false; setVisibility("public") })
                                    }
                                    FmDropdownMenuItem(text = "Delete", onClick = { menu = false; confirmDelete = true })
                                }
                            }
                            plugin?.Items(close = { menu = false }, divided = opened)
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
                        onValueChange = {
                            val next = it.take(200)
                            if (next != titleText) typedAt = System.currentTimeMillis()
                            titleText = next
                        },
                        readOnly = !canEdit,
                        hint = "Title",
                        textStyle = Fm.text.titleLarge,
                    )
                    Text(NotesLogic.editorMeta(note), style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                    if (presenceLabel.isNotEmpty()) NotePresenceLine(presenceLabel, peers) { showPeople = true }
                    val publicUrl = id?.takeIf { public }?.let { NotesLogic.publicUrl(origin, it) }
                    if (publicUrl != null) {
                        Text(
                            publicUrl,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(color = Fm.colors.accent),
                            modifier = Modifier.clickable { copy("Public link", publicUrl) }.padding(vertical = 2.dp),
                        )
                    }
                    val links = NoteLink.list(note?.get("links"))
                    if (links.isNotEmpty()) {
                        Text(
                            "Linked: " + links.joinToString(", ") { it.label },
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(color = Fm.colors.accent),
                            modifier = Modifier.clickable { showLinks = true }.padding(vertical = 2.dp),
                        )
                    }
                    FmInlineStatus(status, Modifier.padding(top = 4.dp))
                    val clash = conflict
                    if (clash != null) {
                        ConflictBanner(
                            clash,
                            onKeepMine = {
                                conflict = null
                                note = clash
                                savedTitle = jsonString(clash["title"])
                                savedMarkdown = jsonString(clash["markdown"])
                                saveSoon()
                            },
                            onTakeTheirs = {
                                conflict = null
                                apply(clash)
                                saveState = NoteSaveState.Saved
                            },
                        )
                    }
                    Spacer(Modifier.height(8.dp))
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FmToneButton("Write", onPressed = { preview = false }, tone = if (!preview) FmTone.Accent else FmTone.Neutral, filled = !preview, height = 30.dp)
                        FmToneButton("Preview", onPressed = { preview = true }, tone = if (preview) FmTone.Accent else FmTone.Neutral, filled = preview, height = 30.dp)
                        if (existing && note != null && !preview) {
                            Spacer(Modifier.weight(1f))
                            val selected = NoteAnchoring.quote(markdown, body.selection.min, body.selection.max)
                            FmToneButton(
                                "Comment",
                                icon = "add_comment",
                                onPressed = selected?.let { quote -> { startComment(quote) } },
                                tooltip = "Select text in the note to comment on it",
                                height = 30.dp,
                            )
                        }
                    }
                    if (!preview && canEdit) {
                        FormatToolbar { kind ->
                            val edit = NotesLogic.format(markdown, body.selection.min, body.selection.max, kind)
                            edited(TextFieldValue(edit.text, TextRange(edit.start, edit.end)))
                        }
                    }
                    Spacer(Modifier.height(4.dp))
                    if (preview) {
                        MarkdownPreview(markdown, Modifier.weight(1f).fillMaxWidth()) { url -> NavigationFeature.openUri(context, url) }
                    } else {
                        MarkdownField(
                            body,
                            ::edited,
                            !canEdit,
                            Modifier.weight(1f).fillMaxWidth().focusRequester(editorFocus),
                            highlight = highlightThread?.let { threads.find(it)?.anchor },
                            onComment = if (existing && note != null) ({ NoteAnchoring.quote(markdown, body.selection.min, body.selection.max)?.let(::startComment) }) else null,
                        )
                    }
                }
            }
        }
        if (confirmDiscard) {
            FmConfirmDialog(
                title = "Discard changes?",
                body = if (conflict != null) "Someone else saved this note first. Leaving now drops your edits." else "Your edits to this note have not been saved.",
                confirmLabel = "Discard",
                onConfirm = {
                    confirmDiscard = false
                    discarded = true
                    navigator.pop()
                },
                onDismissRequest = { confirmDiscard = false },
            )
        }
        if (confirmDelete) {
            FmConfirmDialog(
                title = "Delete note?",
                body = "“${titleText.ifBlank { "Untitled note" }}” and its version history will be removed for everyone it's shared with. This can't be undone.",
                confirmLabel = "Delete",
                destructive = true,
                onConfirm = {
                    confirmDelete = false
                    val current = id ?: return@FmConfirmDialog
                    scope.launch {
                        try {
                            withContext(Dispatchers.IO) { notes.delete(current) }
                            discarded = true
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
                NoteHistorySheet(notes, current, version, canEdit, onDismiss = { showHistory = false }) { restored ->
                    showHistory = false
                    conflict = null
                    apply(restored)
                    status.show("Version restored")
                }
            }
        }
        if (showShare) {
            val current = id
            if (current != null) {
                NoteShareSheet(notes, current, origin, jsonString(note?.get("title")), jsonObjects(note?.get("shares")), onChanged = { shares ->
                    note = (note ?: emptyMap()) + ("shares" to shares)
                }) {
                    showShare = false
                    scope.launch {
                        try {
                            apply(withContext(Dispatchers.IO) { notes.get(current) }, keepText = true)
                        } catch (_: Exception) {
                        }
                    }
                }
            }
        }
        if (showPeople) {
            NotePeopleSheet(presenceLabel, peers) { showPeople = false }
        }
        if (showComments && openId != null) {
            NoteCommentsSheet(
                threads = threads,
                people = comments.people,
                canResolve = comments.canResolve,
                isOwner = NotesLogic.role(note) == "owner",
                me = me,
                loaded = commentsLoaded,
                loadError = commentsError,
                focus = focusThread,
                draft = commentDraft,
                composing = composing,
                busy = commentBusy,
                status = commentStatus,
                actions = NoteCommentActions(
                    send = { text, parentId, quote, done ->
                        commentCall(scope, "The comment was not saved.", { api, note -> api.addComment(note, text, parentId, quote) }, done)
                    },
                    edit = { commentId, text, done ->
                        commentCall(scope, "The comment was not changed.", { api, note -> api.editComment(note, commentId, text) }, done)
                    },
                    resolve = { commentId, resolved ->
                        commentCall(scope, if (resolved) "The thread was not resolved." else "The thread was not reopened.", { api, note -> api.resolveComment(note, commentId, resolved) })
                    },
                    delete = { commentId ->
                        commentCall(scope, "The comment was not deleted.", { api, note -> api.deleteComment(note, commentId) })
                    },
                    pick = { thread ->
                        showComments = false
                        selectThread(thread)
                        scope.launch {
                            delay(150)
                            runCatching { editorFocus.requestFocus() }
                        }
                    },
                ),
                onCompose = { startComment(null) },
                onCancelCompose = {
                    composing = false
                    commentDraft = null
                },
                onDismiss = {
                    showComments = false
                    composing = false
                    commentDraft = null
                    focusThread = null
                },
            )
        }
        if (showLinks) {
            val current = id
            if (current != null) {
                NoteLinksSheet(
                    notes,
                    current,
                    NoteLink.list(note?.get("links")),
                    canEdit,
                    onChanged = { links -> note = (note ?: emptyMap()) + ("links" to links.map { it.body() }) },
                    onOpen = { link -> NavigationFeature.openUri(context, NotesLogic.linkUrl(origin, link)) },
                    onDismiss = { showLinks = false },
                )
            }
        }
    }

    @Composable
    private fun NoteRoom(notes: NotesApi, scope: kotlinx.coroutines.CoroutineScope) {
        val current = id
        LaunchedEffect(typedAt, roomOpen) {
            if (!roomOpen) return@LaunchedEffect
            val socket = room ?: return@LaunchedEffect
            val live = presence ?: return@LaunchedEffect
            val now = System.currentTimeMillis()
            live.stateChangeFrame(localState(now))?.let { frame -> withContext(Dispatchers.IO) { runCatching { socket.sendText(frame) } } }
            val wait = NotePresence.msUntilViewing(typedAt, now)
            if (wait <= 0) return@LaunchedEffect
            delay(wait)
            live.stateChangeFrame(localState())?.let { frame -> withContext(Dispatchers.IO) { runCatching { socket.sendText(frame) } } }
        }
        LaunchedEffect(current, roomGeneration) {
            if (current == null) return@LaunchedEffect
            val service = Services.get<ApiService>() ?: return@LaunchedEffect
            val token = service.sessionTokenProvider?.invoke().orEmpty()
            if (token.isEmpty()) return@LaunchedEffect
            val live = NotePresence(UUID.randomUUID().toString())
            val frames = Channel<String>(Channel.UNLIMITED)
            val listener = object : WebSocketClient.Listener {
                override fun onText(socket: WebSocketClient, text: String) {
                    frames.trySend(text)
                }

                override fun onClosed(socket: WebSocketClient, code: Int, reason: String, error: Throwable?) {
                    frames.close()
                }
            }
            val options = WebSocketClient.Options().header("Authorization", "Bearer $token").header("Origin", service.gitOrigin)
            val socket = withContext(Dispatchers.IO) {
                runCatching { WebSocketClient.connect(NotePresence.roomUrl(service.gitOrigin, current), listener, options) }.getOrNull()
            }
            if (socket == null) {
                presenceLabel = ""
                return@LaunchedEffect
            }
            room = socket
            presence = live
            roomOpen = true
            if (commentsLoaded) commentsSignal++
            suspend fun send(text: String) {
                withContext(Dispatchers.IO) { socket.sendText(text) }
            }
            fun refresh() {
                val now = System.currentTimeMillis()
                peers = live.peers(now)
                presenceLabel = live.label(socket.isOpen, now)
            }
            val ticker = launch {
                while (true) {
                    send(live.presenceFrame(localState()))
                    refresh()
                    delay(NotePresence.PRESENCE_MS)
                }
            }
            var accessChanged = false
            try {
                for (text in frames) {
                    val frame = live.receive(text, System.currentTimeMillis())
                    if (frame.newcomer) send(live.presenceFrame(localState()))
                    when (val event = frame.event) {
                        is NoteRoomEvent.Draft -> if (event.baseVersion == version && !dirty && !saving) {
                            titleText = event.title
                            savedTitle = event.title
                            savedMarkdown = event.markdown
                            body = TextFieldValue(event.markdown, body.selection.let { TextRange(it.min.coerceAtMost(event.markdown.length)) })
                        }
                        is NoteRoomEvent.Updated -> if (event.version > version && !dirty && !saving) {
                            scope.launch { runCatching { apply(withContext(Dispatchers.IO) { notes.get(current) }) } }
                        }
                        NoteRoomEvent.Deleted -> {
                            discarded = true
                            status.error("This note was deleted.")
                        }
                        NoteRoomEvent.AccessChanged -> accessChanged = true
                        NoteRoomEvent.Comments -> commentsSignal++
                        is NoteRoomEvent.Left -> Unit
                        null -> Unit
                    }
                    refresh()
                }
            } finally {
                ticker.cancel()
                roomOpen = false
                room = null
                presence = null
                withContext(NonCancellable + Dispatchers.IO) { socket.cancel() }
                live.clear()
                peers = emptyList()
                presenceLabel = live.label(false, System.currentTimeMillis())
            }
            if (!discarded) {
                delay(if (accessChanged) 400L else NotePresence.PRESENCE_MS)
                runCatching { apply(withContext(Dispatchers.IO) { notes.get(current) }, keepText = true) }
                    .onFailure { thrown -> if (thrown is CancellationException) throw thrown }
                roomGeneration++
            }
        }
    }
}

@Composable
private fun NoteListRow(note: JsonObject, me: String, now: Long, zone: ZoneId, onMenu: () -> Unit, onOpen: () -> Unit) {
    val c = Fm.colors
    val info = NoteRows.info(note)
    val title = jsonString(note["title"], "Untitled note").ifEmpty { "Untitled note" }
    FmListRow(
        title = title,
        modifier = Modifier.semantics {
            customActions = listOf(CustomAccessibilityAction("Note details and actions") {
                onMenu()
                true
            })
        },
        titleContent = {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                if (info.unread) {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(c.accent).semantics { contentDescription = "Unread" })
                }
                Text(
                    title,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleSmall.copy(color = c.textPrimary, fontWeight = if (info.unread) FontWeight.Bold else FontWeight.Medium),
                    modifier = Modifier.weight(1f, fill = false),
                )
            }
        },
        subtitleContent = {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                NoteReachBadge(info.reach)
                Text(
                    NoteRows.lastEdit(note, me, now, zone),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                    modifier = Modifier.weight(1f, fill = false),
                )
                if (info.openComments > 0) {
                    val label = NoteRows.commentCountLabel(info.openComments)
                    Row(
                        Modifier.semantics(mergeDescendants = true) { contentDescription = label },
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(2.dp),
                    ) {
                        FmIcon("chat_bubble_outline", size = 13.dp, tint = c.textTertiary)
                        Text(info.openComments.toString(), style = Fm.text.bodySmall.copy(color = c.textTertiary))
                    }
                }
            }
        },
        leading = { FmIconTile(if (note["visibility"] == "public") "public" else "sticky_note_2_outlined") },
        trailing = if (info.faces.isNotEmpty()) ({ NoteShareFaces(info) }) else null,
        onTap = onOpen,
        onLongPress = onMenu,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun NoteActionsSheet(
    summary: NoteAccessSummary,
    onOpen: () -> Unit,
    onCopyLink: () -> Unit,
    onShare: () -> Unit,
    onDelete: () -> Unit,
    onDismiss: () -> Unit,
) {
    val c = Fm.colors
    FmModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.verticalScroll(rememberScrollState())) {
            FmPanelHeader(title = summary.title, subtitle = summary.role)
            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp).padding(bottom = 12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                NoteReachBadge(summary.reach)
                Text("Shared with", style = Fm.text.labelSmall.copy(color = c.textTertiary), modifier = Modifier.padding(top = 6.dp))
                for (line in summary.sharedWith) Text(line, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                if (summary.lastEdit.isNotEmpty()) {
                    Text(summary.lastEdit, style = Fm.text.bodySmall.copy(color = c.textSecondary), modifier = Modifier.padding(top = 6.dp))
                }
            }
            FmRowDivider()
            NoteActionRow("open_in_new", "Open", onClick = onOpen)
            NoteActionRow("link", "Copy link", onClick = onCopyLink)
            if (summary.isOwner) {
                NoteActionRow("share", "Share…", onClick = onShare)
                NoteActionRow("delete_outline", "Delete", destructive = true, onClick = onDelete)
            }
        }
    }
}

@Composable
private fun NoteActionRow(icon: String, label: String, destructive: Boolean = false, onClick: () -> Unit) {
    val tint = if (destructive) Fm.colors.danger else Fm.colors.textPrimary
    FmListRow(
        title = label,
        leading = { FmIcon(icon, size = 22.dp, tint = tint) },
        titleColor = tint,
        showChevron = false,
        minHeight = 48.dp,
        onTap = onClick,
    )
}

@Composable
private fun NoteReachBadge(reach: NoteReach) {
    val c = Fm.colors
    val tint = if (reach == NoteReach.Public) c.accent else c.textSecondary
    Text(
        reach.label,
        maxLines = 1,
        style = Fm.text.labelSmall.copy(color = tint, fontWeight = FontWeight.SemiBold),
        modifier = Modifier
            .clip(RoundedCornerShape(FmRadius.full))
            .background(tint.copy(alpha = 0.12f))
            .padding(horizontal = 7.dp, vertical = 1.dp),
    )
}

@Composable
private fun NoteShareFaces(info: NoteRowInfo) {
    val c = Fm.colors
    val size = 22.dp
    val names = info.faces.joinToString(", ") { it.name } + if (info.extraFaces > 0) " and ${info.extraFaces} more" else ""
    Row(
        Modifier.padding(start = 8.dp).semantics(mergeDescendants = true) { contentDescription = "Shared with $names" },
        horizontalArrangement = Arrangement.spacedBy((-6).dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (face in info.faces) {
            val shape = if (face.isOrganization) RoundedCornerShape(5.dp) else CircleShape
            Box(Modifier.border(1.5.dp, c.bgBase, shape).padding(1.5.dp)) {
                FmAvatar(face.name, size, textStyle = Fm.text.labelSmall, corner = if (face.isOrganization) 4.dp else null)
            }
        }
        if (info.extraFaces > 0) {
            Box(
                Modifier.border(1.5.dp, c.bgBase, CircleShape).padding(1.5.dp).size(size).clip(CircleShape).background(c.bgRaised),
                contentAlignment = Alignment.Center,
            ) {
                Text(info.extraLabel, style = Fm.text.labelSmall.copy(color = c.textSecondary))
            }
        }
    }
}

@Composable
private fun ConflictBanner(latest: JsonObject, onKeepMine: () -> Unit, onTakeTheirs: () -> Unit) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(top = 8.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .border(1.dp, Fm.colors.warning, RoundedCornerShape(FmRadius.md))
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(NotesLogic.conflictText(latest), style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FmToneButton("Keep mine", onPressed = onKeepMine, tone = FmTone.Accent, filled = true)
            FmToneButton("Take theirs", onPressed = onTakeTheirs)
        }
    }
}

@Composable
private fun FormatToolbar(onFormat: (String) -> Unit) {
    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(top = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        FmIconButton("title", "Heading", onPressed = { onFormat("heading") }, color = Fm.colors.textSecondary)
        FmIconButton("Bold", onPressed = { onFormat("bold") }, color = Fm.colors.textSecondary) {
            Text("B", style = Fm.text.titleMedium.copy(fontWeight = FontWeight.Bold))
        }
        FmIconButton("Italic", onPressed = { onFormat("italic") }, color = Fm.colors.textSecondary) {
            Text("I", style = Fm.text.titleMedium.copy(fontStyle = FontStyle.Italic))
        }
        FmIconButton("format_list_bulleted", "List", onPressed = { onFormat("list") }, color = Fm.colors.textSecondary)
        FmIconButton("checklist", "Task", onPressed = { onFormat("task") }, color = Fm.colors.textSecondary)
        FmIconButton("link", "Link", onPressed = { onFormat("link") }, color = Fm.colors.textSecondary)
        FmIconButton("code", "Code", onPressed = { onFormat("code") }, color = Fm.colors.textSecondary)
    }
}

@Composable
private fun MarkdownField(value: TextFieldValue, onChange: (TextFieldValue) -> Unit, readOnly: Boolean, modifier: Modifier, highlight: IntRange? = null, onComment: (() -> Unit)? = null) {
    val c = Fm.colors
    val style = Fm.mono(size = Fm.text.bodyMedium.fontSize).copy(color = c.textPrimary)
    val mark = c.accent.copy(alpha = 0.22f)
    val transformation = remember(highlight, mark) {
        if (highlight == null) {
            VisualTransformation.None
        } else {
            VisualTransformation { text ->
                val from = highlight.first.coerceIn(0, text.length)
                val to = (highlight.last + 1).coerceIn(from, text.length)
                val marked = AnnotatedString.Builder(text).apply { if (to > from) addStyle(SpanStyle(background = mark), from, to) }.toAnnotatedString()
                TransformedText(marked, OffsetMapping.Identity)
            }
        }
    }
    BasicTextField(
        value = value,
        onValueChange = FmSafeInput.text(onChange),
        readOnly = readOnly,
        visualTransformation = transformation,
        textStyle = style,
        cursorBrush = SolidColor(c.accent),
        modifier = modifier
            .appendTextContextMenuComponents {
                if (onComment != null && !value.selection.collapsed) {
                    separator()
                    item(key = "note-comment", label = "Comment") {
                        close()
                        onComment()
                    }
                }
            }
            .clip(RoundedCornerShape(FmRadius.md))
            .border(1.dp, c.border, RoundedCornerShape(FmRadius.md))
            .padding(12.dp),
        decorationBox = { inner ->
            Box {
                if (value.text.isEmpty()) Text("Write in markdown…", style = style.copy(color = c.textTertiary))
                inner()
            }
        },
    )
}

@Composable
private fun MarkdownPreview(markdown: String, modifier: Modifier, onLink: (String) -> Unit) {
    val parsed = remember(markdown) { DocsMarkdown.parse(markdown) }
    Column(modifier.verticalScroll(rememberScrollState())) {
        if (parsed.blocks.isEmpty()) {
            Text("Nothing to preview yet.", style = Fm.text.bodyMedium.copy(color = Fm.colors.textTertiary))
        }
        for (block in parsed.blocks) DocsBlockView(block, parsed.references, onLink)
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
                    val author = jsonString(entry["author"])
                    FmListRow(
                        title = "Version $version",
                        subtitle = listOf(relativeTime(jsonInt(entry["createdAt"])), author).filter { it.isNotEmpty() }.joinToString(" · "),
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
private fun NoteShareSheet(
    api: NotesApi,
    noteId: String,
    origin: String,
    noteTitle: String,
    initial: List<JsonObject>,
    onChanged: (List<JsonObject>) -> Unit,
    onDismiss: () -> Unit,
) {
    var shares by remember { mutableStateOf(initial) }
    var name by remember { mutableStateOf("") }
    var type by remember { mutableStateOf("user") }
    var role by remember { mutableStateOf("viewer") }
    var busy by remember { mutableStateOf(false) }
    var suggestions by remember { mutableStateOf<List<NoteShareSuggestion>>(emptyList()) }
    var picked by remember { mutableStateOf("") }
    var roleMenu by remember { mutableStateOf("") }
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    val context = LocalContext.current
    LaunchedEffect(name) {
        val query = NotesLogic.suggestionQuery(name)
        if (query.isEmpty() || query == picked) {
            suggestions = emptyList()
            return@LaunchedEffect
        }
        delay(NotesLogic.SUGGEST_MS)
        val found = try {
            withContext(Dispatchers.IO) { api.shareSuggestions(noteId, query) }
        } catch (cancel: CancellationException) {
            throw cancel
        } catch (_: Exception) {
            emptyList()
        }
        if (NotesLogic.suggestionQuery(name) == query) suggestions = found
    }
    fun send(block: () -> List<JsonObject>, notice: String, clear: Boolean) {
        busy = true
        scope.launch {
            try {
                shares = withContext(Dispatchers.IO) { block() }
                onChanged(shares)
                if (clear) {
                    name = ""
                    picked = ""
                    suggestions = emptyList()
                }
                status.show(notice)
            } catch (thrown: Exception) {
                status.error(NotesLogic.shareError(thrown, type))
            }
            busy = false
        }
    }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState())) {
            FmPanelHeader(title = "Share", subtitle = "People with access can open this note in ForkMesh.")
            if (shares.isEmpty()) {
                Text("Not shared with anyone yet.", modifier = Modifier.padding(16.dp), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
            }
            for (share in shares) {
                val shareType = jsonString(share["type"])
                val shareName = jsonString(share["name"])
                val shareRole = jsonString(share["role"], "viewer")
                val key = "$shareType:$shareName"
                FmListRow(
                    title = shareName,
                    subtitle = if (shareType == "organization") "Organization" else "User",
                    showChevron = false,
                    leading = { FmIconTile(if (shareType == "organization") "groups_outlined" else "person_outline") },
                    trailing = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Box {
                                FmTextButton(enabled = !busy, onClick = { roleMenu = key }) { Text(NotesLogic.roleLabel(shareRole)) }
                                FmDropdownMenu(expanded = roleMenu == key, onDismissRequest = { roleMenu = "" }) {
                                    for (option in listOf("editor", "viewer")) {
                                        FmDropdownMenuItem(text = NotesLogic.roleLabel(option), onClick = {
                                            roleMenu = ""
                                            if (option != shareRole) {
                                                send({ api.addShare(noteId, shareType, shareName, option) }, NotesLogic.roleNotice(shareName, option), false)
                                            }
                                        })
                                    }
                                }
                            }
                            FmIconButton("close", "Stop sharing with $shareName", onPressed = if (busy) null else ({
                                send({ api.removeShare(noteId, shareType, shareName) }, "Removed $shareName", false)
                            }))
                        }
                    },
                )
            }
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmToneButton("User", onPressed = { type = "user" }, tone = if (type == "user") FmTone.Accent else FmTone.Neutral, filled = type == "user")
                    FmToneButton("Org", onPressed = { type = "organization" }, tone = if (type == "organization") FmTone.Accent else FmTone.Neutral, filled = type == "organization")
                    Spacer(Modifier.width(8.dp))
                    FmToneButton("Can view", onPressed = { role = "viewer" }, tone = if (role == "viewer") FmTone.Accent else FmTone.Neutral, filled = role == "viewer")
                    FmToneButton("Can edit", onPressed = { role = "editor" }, tone = if (role == "editor") FmTone.Accent else FmTone.Neutral, filled = role == "editor")
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(value = name, onValueChange = { name = it }, hint = if (type == "user") "Username" else "Organization", modifier = Modifier.weight(1f))
                    FmTextButton(enabled = !busy && NotesLogic.shareName(name).isNotEmpty(), onClick = {
                        val chosen = NotesLogic.shareName(name)
                        send({ api.addShare(noteId, type, chosen, role) }, NotesLogic.sharedNotice(chosen), true)
                    }) { Text("Share") }
                }
                if (suggestions.isNotEmpty()) {
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(max = 280.dp)
                            .clip(RoundedCornerShape(FmRadius.md))
                            .border(1.dp, Fm.colors.border, RoundedCornerShape(FmRadius.md))
                            .background(Fm.colors.bgRaised)
                            .verticalScroll(rememberScrollState()),
                    ) {
                        for (item in suggestions) {
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .clickable {
                                        picked = item.name
                                        name = item.name
                                        type = item.type
                                        suggestions = emptyList()
                                    }
                                    .padding(horizontal = 12.dp, vertical = 8.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(10.dp),
                            ) {
                                FmAvatar(item.name, 24.dp, textStyle = Fm.text.labelSmall)
                                Text(item.name, style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
                                Text(item.typeLabel, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                            }
                        }
                    }
                }
                FmInlineStatus(status)
                if (shares.isNotEmpty()) {
                    val url = NotesLogic.accessUrl(origin, noteId)
                    Text("Link for people with access", style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary), modifier = Modifier.padding(top = 8.dp))
                    Text(url, maxLines = 2, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FmToneButton("Copy link", icon = "content_copy", onPressed = {
                            copyToClipboard(context, url)
                            status.show("Link copied")
                        })
                        FmToneButton("Send", icon = "share", onPressed = { shareNoteText(context, noteTitle.ifBlank { "Untitled note" }, url) })
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun NoteLinksSheet(
    api: NotesApi,
    noteId: String,
    initial: List<NoteLink>,
    canEdit: Boolean,
    onChanged: (List<NoteLink>) -> Unit,
    onOpen: (NoteLink) -> Unit,
    onDismiss: () -> Unit,
) {
    var links by remember { mutableStateOf(initial) }
    var input by remember { mutableStateOf("") }
    var kind by remember { mutableStateOf("issue") }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    LaunchedEffect(noteId) {
        try {
            links = withContext(Dispatchers.IO) { api.links(noteId) }
            onChanged(links)
        } catch (cancel: CancellationException) {
            throw cancel
        } catch (_: Exception) {
        }
    }
    fun send(block: () -> List<NoteLink>, notice: String, clear: Boolean) {
        busy = true
        scope.launch {
            try {
                links = withContext(Dispatchers.IO) { block() }
                onChanged(links)
                if (clear) input = ""
                status.show(notice)
            } catch (thrown: Exception) {
                status.error(NotesLogic.linkError(thrown))
            }
            busy = false
        }
    }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState())) {
            FmPanelHeader(title = "Linked issues and PRs")
            if (links.isEmpty()) {
                Text("Nothing linked yet.", modifier = Modifier.padding(16.dp), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
            }
            for (link in links) {
                FmListRow(
                    title = "${link.owner}/${link.repo}#${link.number}",
                    subtitle = link.kindLabel,
                    leading = { FmIconTile(if (link.kind == "pull") "call_merge" else if (link.kind == "discussion") "forum_outlined" else "adjust") },
                    showChevron = false,
                    onTap = { onOpen(link) },
                    trailing = if (canEdit) ({
                        FmIconButton("close", "Unlink ${link.label}", onPressed = if (busy) null else ({
                            send({ api.removeLink(noteId, link) }, "Unlinked ${link.label}", false)
                        }))
                    }) else null,
                )
            }
            if (canEdit) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        for ((value, label) in listOf("issue" to "Issue", "pull" to "PR", "discussion" to "Discussion")) {
                            FmToneButton(label, onPressed = { kind = value }, tone = if (kind == value) FmTone.Accent else FmTone.Neutral, filled = kind == value)
                        }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        FmTextField(value = input, onValueChange = { input = it }, hint = "Paste a URL or owner/repo#123", modifier = Modifier.weight(1f))
                        FmTextButton(enabled = !busy && input.isNotBlank(), onClick = {
                            val link = NoteLink.parse(input, kind)
                            if (link == null) {
                                status.error("Paste an issue or pull request URL, or type owner/repo#123.")
                            } else {
                                send({ api.addLink(noteId, link) }, "Linked ${link.label}", true)
                            }
                        }) { Text("Link") }
                    }
                }
            }
            FmInlineStatus(status, Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
        }
    }
}
