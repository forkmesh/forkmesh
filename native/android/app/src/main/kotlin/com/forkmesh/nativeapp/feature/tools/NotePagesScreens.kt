package com.forkmesh.nativeapp.feature.tools

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.docs.DocsMarkdown
import com.forkmesh.nativeapp.feature.docs.DocsBlockView
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.notes.NoteEmoji
import com.forkmesh.nativeapp.notes.NotePage
import com.forkmesh.nativeapp.notes.NotePageDetail
import com.forkmesh.nativeapp.notes.NotePagesApi
import com.forkmesh.nativeapp.notes.NoteTimes
import com.forkmesh.nativeapp.notes.NotesLogic
import com.forkmesh.nativeapp.notes.OutlineRow
import com.forkmesh.nativeapp.notes.PageAccent
import com.forkmesh.nativeapp.notes.PageConflictException
import com.forkmesh.nativeapp.notes.PageEntry
import com.forkmesh.nativeapp.notes.PageLayout
import com.forkmesh.nativeapp.notes.PageNote
import com.forkmesh.nativeapp.notes.PageOutline
import com.forkmesh.nativeapp.notes.jsonInt
import com.forkmesh.nativeapp.notes.jsonString
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmNavBar
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmSwitch
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
import kotlin.math.roundToInt
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal fun notePagesApi(): NotePagesApi? = Services.get<ApiService>()?.let(::NotePagesApi)

@Composable
internal fun pageColor(accent: PageAccent): Color {
    val dark = Fm.colors.dark
    return when (accent) {
        PageAccent.Blue -> Fm.colors.accent
        PageAccent.Purple -> if (dark) Color(0xFFBF5AF2) else Color(0xFFAF52DE)
        PageAccent.Pink -> if (dark) Color(0xFFFF375F) else Color(0xFFFF2D55)
        PageAccent.Red -> if (dark) Color(0xFFFF453A) else Color(0xFFFF3B30)
        PageAccent.Orange -> if (dark) Color(0xFFFF9F0A) else Color(0xFFC93400)
        PageAccent.Yellow -> if (dark) Color(0xFFFFD60A) else Color(0xFFB25000)
        PageAccent.Teal -> if (dark) Color(0xFF40C8E0) else Color(0xFF00788A)
        PageAccent.Gray -> if (dark) Color(0xFF98989D) else Color(0xFF6C6C70)
    }
}

@Composable
internal fun PageTile(page: NotePage, size: androidx.compose.ui.unit.Dp = 38.dp) {
    val tint = pageColor(page.accent)
    Box(
        Modifier.size(size).clip(RoundedCornerShape(FmRadius.md)).background(tint.copy(alpha = 0.16f)),
        contentAlignment = Alignment.Center,
    ) {
        if (page.emoji.isNotEmpty()) Text(page.emoji, fontSize = (size.value * 0.5f).sp) else FmIcon("menu_book", size = size * 0.5f, tint = tint)
    }
}

@Composable
internal fun NotePagesSection(pages: List<NotePage>, busy: Boolean, onOpen: (NotePage) -> Unit, onNew: () -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 4.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Pages", style = Fm.text.titleSmall.copy(color = Fm.colors.textSecondary), modifier = Modifier.weight(1f))
            FmTextButton(onClick = onNew, enabled = !busy) {
                FmIcon("add", size = 18.dp)
                Spacer(Modifier.width(4.dp))
                Text("New page")
            }
        }
        if (pages.isEmpty()) {
            Text(
                "Gather notes into a page you can share, with an outline.",
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
            )
        }
        for (page in pages) {
            FmListRow(
                title = page.displayTitle,
                titleColor = pageColor(page.accent),
                subtitle = page.entriesLabel,
                leading = { PageTile(page) },
                onTap = { onOpen(page) },
            )
            FmRowDivider(indent = 66.dp)
        }
    }
}

@Composable
private fun MarkdownBlocks(markdown: String, onLink: (String) -> Unit) {
    val parsed = remember(markdown) { DocsMarkdown.parse(markdown) }
    Column {
        if (parsed.blocks.isEmpty()) Text("This note is empty.", style = Fm.text.bodyMedium.copy(color = Fm.colors.textTertiary))
        for (block in parsed.blocks) DocsBlockView(block, parsed.references, onLink)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun NoteEmojiPicker(current: String, onPick: (String) -> Unit, onDismiss: () -> Unit) {
    var typed by remember { mutableStateOf("") }
    val error = NoteEmoji.error(typed)
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState())) {
            FmPanelHeader(
                title = "Emoji",
                trailing = if (current.isNotEmpty()) ({ FmTextButton(onClick = { onPick("") }, contentColor = Fm.colors.danger) { Text("Remove") } }) else null,
            )
            for (row in NoteEmoji.common.chunked(8)) {
                Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp)) {
                    for (emoji in row) {
                        Box(
                            Modifier
                                .weight(1f)
                                .aspectRatio(1f)
                                .padding(2.dp)
                                .clip(RoundedCornerShape(FmRadius.md))
                                .background(if (emoji == current) Fm.colors.accentSubtle else Color.Transparent)
                                .clickable(role = Role.Button, onClickLabel = "Use $emoji") { onPick(emoji) },
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(emoji, fontSize = 24.sp)
                        }
                    }
                }
            }
            Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                FmTextField(
                    value = typed,
                    onValueChange = { typed = it.take(64) },
                    hint = "Type or paste any emoji",
                    errorText = error,
                    modifier = Modifier.weight(1f),
                )
                FmTextButton(enabled = typed.isNotBlank() && error == null, onClick = { onPick(NoteEmoji.clean(typed)) }) { Text("Use") }
            }
        }
    }
}

@Composable
internal fun NoteEmojiButton(emoji: String, editable: Boolean, onClick: () -> Unit) {
    if (!editable && emoji.isEmpty()) return
    Box(
        Modifier
            .size(44.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .then(if (editable) Modifier.clickable(role = Role.Button, onClickLabel = if (emoji.isEmpty()) "Add an emoji" else "Change the emoji", onClick = onClick) else Modifier)
            .semantics { contentDescription = if (emoji.isEmpty()) "Add an emoji" else "Emoji $emoji" },
        contentAlignment = Alignment.Center,
    ) {
        if (emoji.isNotEmpty()) Text(emoji, fontSize = 26.sp) else FmIcon("add_reaction_outlined", size = 24.dp, tint = Fm.colors.textTertiary)
    }
}

class NotePageScreen(private val pageId: String, private val openNote: String? = null, private val customizeFirst: Boolean = false) : Screen() {
    override val title: String get() = "Page"

    private val api: NotePagesApi? = notePagesApi()
    private val status = FmStatus()
    private var detail by mutableStateOf<NotePageDetail?>(null)
    private var loading by mutableStateOf(true)
    private var error by mutableStateOf("")
    private var generation by mutableIntStateOf(0)
    private var selected by mutableStateOf<String?>(null)
    private var pendingNote: String? = openNote
    private var toggled by mutableStateOf(emptySet<String>())
    private var showCustomize by mutableStateOf(customizeFirst)
    private var showShare by mutableStateOf(false)
    private var menu by mutableStateOf(false)
    private var confirmDelete by mutableStateOf(false)
    private var deleted = false

    override fun onBack(): Boolean {
        if (selected != null && detail?.page?.layout == PageLayout.Outline) {
            selected = null
            return true
        }
        return false
    }

    private fun markViewed(note: PageNote) {
        val current = detail ?: return
        markNoteSeen(note.id, note.updatedAt)
        if (note.unread) detail = current.withNote(note.copy(unread = false))
        val notes = notesApi() ?: return
        com.forkmesh.nativeapp.platform.AppLifecycle.scope.launch(Dispatchers.IO) { runCatching { notes.recordView(note.id) } }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val pages = api ?: return
        val context = LocalContext.current
        val scope = rememberCoroutineScope()
        val origin = Services.get<ApiService>()?.gitOrigin.orEmpty()
        val top = navigator.top
        LaunchedEffect(top) { if (top === this@NotePageScreen && !deleted) generation++ }
        LaunchedEffect(generation) {
            if (generation == 0) return@LaunchedEffect
            if (detail == null) loading = true
            try {
                val loaded = withContext(Dispatchers.IO) { pages.get(pageId) }
                detail = loaded
                error = ""
                val target = pendingNote
                if (target != null) {
                    pendingNote = null
                    if (loaded.notes.containsKey(target)) selected = target else status.error(PageOutline.LOCKED)
                }
            } catch (cancel: CancellationException) {
                throw cancel
            } catch (thrown: Exception) {
                if (detail == null) error = PageOutline.error(thrown, "This page could not be opened.") else status.error(PageOutline.error(thrown, "The page could not be refreshed."))
            }
            loading = false
        }
        val current = detail
        val page = current?.page
        val picked = selected?.let { current?.notes?.get(it) }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            FmNavBar(page?.displayTitle ?: "Page", onBack = { navigator.handleBack() }) {
                if (page != null) {
                    if (page.canEdit) FmIconButton("tune", "Customize", onPressed = { showCustomize = true }, color = Fm.colors.textPrimary)
                    if (page.isOwner) FmIconButton("person_add", "Share", onPressed = { showShare = true }, color = Fm.colors.textPrimary)
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem(text = "Copy link", icon = "link", onClick = {
                                menu = false
                                copyToClipboard(context, PageOutline.accessUrl(origin, page.id, selected))
                                status.show("Link copied")
                            })
                            if (page.isOwner) FmDropdownMenuItem(text = "Delete page", icon = "delete_outline", onClick = { menu = false; confirmDelete = true })
                        }
                    }
                }
            }
            FmInlineStatus(status, Modifier.padding(horizontal = 16.dp))
            when {
                loading && current == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                current == null -> FmEmptyState("menu_book", "Page unavailable", message = error)
                current.page.layout == PageLayout.Outline && picked != null -> PageNoteView(current, picked, navigator) { selected = it.id }
                else -> PageBody(current, navigator)
            }
        }
        if (showCustomize && current != null) {
            PageCustomizeSheet(current, pages, onSaved = {
                showCustomize = false
                status.show("Page saved")
                generation++
            }, onReload = {
                showCustomize = false
                generation++
            }) { showCustomize = false }
        }
        if (showShare && page != null) {
            NoteShareSheet(
                NoteShareTarget(
                    title = page.displayTitle,
                    subtitle = "People with access can open this page in ForkMesh. Each note on it opens only for people who can open that note.",
                    linkUrl = PageOutline.accessUrl(origin, page.id),
                    includeNotesOption = true,
                    add = { type, name, role, includeNotes -> pages.addShare(page.id, type, name, role, includeNotes) },
                    remove = { type, name -> pages.removeShare(page.id, type, name) },
                    suggest = { query -> pages.shareSuggestions(page.id, query) },
                ),
                page.shares,
                onChanged = { shares -> detail = current.copy(page = page.copy(shares = shares)) },
            ) {
                showShare = false
                generation++
            }
        }
        if (confirmDelete && page != null) {
            FmConfirmDialog(
                title = "Delete page?",
                body = "“${page.displayTitle}” will be removed for everyone it's shared with. The notes on it are not deleted.",
                confirmLabel = "Delete",
                destructive = true,
                onConfirm = {
                    confirmDelete = false
                    scope.launch {
                        try {
                            withContext(Dispatchers.IO) { pages.delete(page.id) }
                            deleted = true
                            navigator.pop()
                        } catch (cancel: CancellationException) {
                            throw cancel
                        } catch (thrown: Exception) {
                            status.error(PageOutline.error(thrown, "The page was not deleted."))
                        }
                    }
                },
                onDismissRequest = { confirmDelete = false },
            )
        }
    }

    @Composable
    private fun PageHeader(page: NotePage, onLink: (String) -> Unit) {
        val tint = pageColor(page.accent)
        Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                if (page.emoji.isNotEmpty()) Text(page.emoji, fontSize = 34.sp)
                Text(page.displayTitle, style = Fm.text.headlineSmall.copy(color = tint, fontWeight = FontWeight.Bold))
            }
            if (page.description.isNotBlank()) {
                Box(
                    Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(FmRadius.md))
                        .background(tint.copy(alpha = 0.08f))
                        .border(1.dp, tint.copy(alpha = 0.24f), RoundedCornerShape(FmRadius.md))
                        .padding(12.dp),
                ) {
                    MarkdownBlocks(page.description, onLink)
                }
            }
        }
    }

    @Composable
    private fun PageBody(current: NotePageDetail, navigator: Navigator) {
        val context = LocalContext.current
        val page = current.page
        val rows = PageOutline.rows(page.outline, current.notes, page.showEmoji, toggled)
        val readable = PageOutline.readable(page.outline, current.notes)
        val continuous = page.layout == PageLayout.Continuous
        val list = rememberLazyListState()
        val scope = rememberCoroutineScope()
        val onLink: (String) -> Unit = { url -> NavigationFeature.openUri(context, url) }
        val firstNoteItem = 2 + rows.size + 1
        LaunchedEffect(continuous, selected, readable.size) {
            val target = selected ?: return@LaunchedEffect
            if (!continuous) return@LaunchedEffect
            val index = readable.indexOfFirst { it.second.id == target }
            if (index >= 0) list.animateScrollToItem(firstNoteItem + index)
        }
        LazyColumn(Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(bottom = 48.dp)) {
            item(key = "header") { PageHeader(page, onLink) }
            item(key = "outline-title") {
                Text(
                    if (continuous) "Contents" else "Outline",
                    style = Fm.text.titleSmall.copy(color = Fm.colors.textSecondary),
                    modifier = Modifier.padding(start = 16.dp, top = 8.dp, bottom = 4.dp),
                )
                if (rows.isEmpty()) {
                    Text(
                        if (page.canEdit) "Nothing on this page yet. Tap Customize to add notes, sections and links." else "Nothing on this page yet.",
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                        style = Fm.text.bodyMedium.copy(color = Fm.colors.textTertiary),
                    )
                }
            }
            for (row in rows) {
                item(key = "row:${row.entry.id}") {
                    OutlineRowView(row, pageColor(page.accent)) {
                        val note = row.note
                        when {
                            row.entry.isSection -> if (row.hasChildren) toggled = if (row.entry.id in toggled) toggled - row.entry.id else toggled + row.entry.id
                            row.entry.isLink -> NavigationFeature.openUri(context, row.entry.url)
                            note != null -> {
                                markViewed(note)
                                if (continuous) {
                                    val index = readable.indexOfFirst { it.second.id == note.id }
                                    if (index >= 0) scope.launch { list.animateScrollToItem(firstNoteItem + index) }
                                } else {
                                    selected = note.id
                                }
                            }
                        }
                    }
                }
            }
            item(key = "outline-end") { Spacer(Modifier.height(8.dp)) }
            if (continuous) {
                for ((_, note) in readable) {
                    item(key = "note:${note.id}") {
                        FmRowDivider()
                        PageNoteSection(note, navigator, onLink)
                    }
                }
            }
        }
    }

    @Composable
    private fun OutlineRowView(row: OutlineRow, tint: Color, onTap: () -> Unit) {
        val c = Fm.colors
        val indent = (row.entry.depth * 18).dp
        val tappable = !row.locked && !(row.entry.isSection && !row.hasChildren)
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 44.dp)
                .then(if (tappable) Modifier.clickable(role = Role.Button, onClick = onTap) else Modifier)
                .padding(start = 16.dp + indent, end = 16.dp, top = 6.dp, bottom = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            when {
                row.entry.isSection -> {
                    if (row.hasChildren) FmIcon(if (row.collapsed) "chevron_right" else "expand_more", size = 18.dp, tint = tint) else Spacer(Modifier.width(18.dp))
                    Text(row.title, style = Fm.text.titleSmall.copy(color = tint, fontWeight = FontWeight.SemiBold), maxLines = 2, overflow = TextOverflow.Ellipsis)
                }
                row.entry.isLink -> {
                    FmIcon("link", size = 18.dp, tint = c.textSecondary)
                    Text(row.title, style = Fm.text.bodyMedium.copy(color = c.accent), maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                    FmIcon("open_in_new", size = 16.dp, tint = c.textTertiary)
                }
                row.locked -> {
                    FmIcon("lock_outline", size = 18.dp, tint = c.textTertiary)
                    Column(Modifier.weight(1f)) {
                        Text(row.title, style = Fm.text.bodyMedium.copy(color = c.textTertiary), maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(PageOutline.LOCKED, style = Fm.text.bodySmall.copy(color = c.textTertiary))
                    }
                }
                else -> {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(if (row.unread) c.accent else Color.Transparent).semantics {
                        if (row.unread) contentDescription = "Unread"
                    })
                    if (row.emoji.isNotEmpty()) Text(row.emoji, fontSize = 16.sp)
                    Text(
                        row.title,
                        style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = if (row.unread) FontWeight.Bold else FontWeight.Normal),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f),
                    )
                    selected?.takeIf { it == row.note?.id }?.let { FmIcon("chevron_right", size = 16.dp, tint = c.textTertiary) }
                }
            }
        }
    }

    @Composable
    private fun NoteMarkdown(note: PageNote, onLink: (String) -> Unit) {
        var failed by remember(note.id) { mutableStateOf(false) }
        LaunchedEffect(note.id, note.markdown == null) {
            if (note.markdown != null) return@LaunchedEffect
            val notes = notesApi() ?: return@LaunchedEffect
            try {
                val loaded = withContext(Dispatchers.IO) { notes.get(note.id) }
                detail = detail?.withNote(note.copy(markdown = jsonString(loaded["markdown"])))
            } catch (cancel: CancellationException) {
                throw cancel
            } catch (_: Exception) {
                failed = true
            }
        }
        val markdown = note.markdown
        when {
            markdown != null -> MarkdownBlocks(markdown, onLink)
            failed -> Text("This note could not be loaded.", style = Fm.text.bodyMedium.copy(color = Fm.colors.textTertiary))
            else -> Box(Modifier.fillMaxWidth().height(80.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
        }
    }

    @Composable
    private fun NoteHeading(note: PageNote, navigator: Navigator) {
        val me = Services.get<com.forkmesh.nativeapp.feature.account.AuthService>()?.session?.value?.nodeName.orEmpty()
        val edited = NoteTimes.lastEdit(note.updatedAt, System.currentTimeMillis(), ZoneId.systemDefault())
        val by = note.lastEditedBy.takeIf { it.isNotEmpty() && !it.equals(me, ignoreCase = true) }?.let { " · by $it" }.orEmpty()
        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (note.emoji.isNotEmpty()) Text(note.emoji, fontSize = 24.sp)
                Text(note.displayTitle, style = Fm.text.titleLarge.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
            }
            if (edited.isNotEmpty()) Text("Edited $edited$by", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
            FmToneButton("Open in editor", icon = "edit", onPressed = { navigator.push(NoteEditorScreen(note.id)) }, tone = FmTone.Accent, filled = true, height = 32.dp)
        }
    }

    @Composable
    private fun PageNoteSection(note: PageNote, navigator: Navigator, onLink: (String) -> Unit) {
        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            NoteHeading(note, navigator)
            NoteMarkdown(note, onLink)
        }
    }

    @Composable
    private fun PageNoteView(current: NotePageDetail, note: PageNote, navigator: Navigator, onPick: (PageNote) -> Unit) {
        val context = LocalContext.current
        val onLink: (String) -> Unit = { url -> NavigationFeature.openUri(context, url) }
        val (previous, next) = PageOutline.neighbours(current.page.outline, current.notes, note.id)
        LaunchedEffect(note.id) { markViewed(note) }
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(
                Modifier.clip(RoundedCornerShape(FmRadius.full)).clickable(role = Role.Button) { selected = null }.padding(vertical = 4.dp, horizontal = 2.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                FmIcon("arrow_back", size = 16.dp, tint = pageColor(current.page.accent))
                Text(NoteEmoji.titled(current.page.emoji, current.page.displayTitle), style = Fm.text.bodySmall.copy(color = pageColor(current.page.accent)), maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            NoteHeading(note, navigator)
            NoteMarkdown(note, onLink)
            Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (previous != null) FmToneButton(NoteEmoji.titled(previous.emoji, previous.displayTitle), icon = "arrow_back", onPressed = { onPick(previous) })
                if (next != null) FmToneButton(NoteEmoji.titled(next.emoji, next.displayTitle), icon = "arrow_forward", onPressed = { onPick(next) })
            }
        }
    }
}

private enum class OutlineAdd { None, Note, Section, Link }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PageCustomizeSheet(
    detail: NotePageDetail,
    api: NotePagesApi,
    onSaved: (NotePage) -> Unit,
    onReload: () -> Unit,
    onDismiss: () -> Unit,
) {
    val original = detail.page
    var title by remember { mutableStateOf(original.title) }
    var emoji by remember { mutableStateOf(original.emoji) }
    var description by remember { mutableStateOf(original.description) }
    var accent by remember { mutableStateOf(original.accent) }
    var layout by remember { mutableStateOf(original.layout) }
    var showEmoji by remember { mutableStateOf(original.showEmoji) }
    var outline by remember { mutableStateOf(original.outline) }
    var known by remember { mutableStateOf(detail.notes) }
    var picking by remember { mutableStateOf(false) }
    var adding by remember { mutableStateOf(OutlineAdd.None) }
    var busy by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf<Int?>(null) }
    val status = rememberFmStatus()
    val scope = rememberCoroutineScope()
    val edited = original.copy(title = title, emoji = emoji, description = description, accent = accent, layout = layout, showEmoji = showEmoji, outline = outline)
    val changed = edited.fields() != original.fields()
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState())) {
            FmPanelHeader(title = "Customize", trailing = {
                FmTextButton(enabled = changed && !busy, onClick = {
                    busy = true
                    scope.launch {
                        try {
                            onSaved(withContext(Dispatchers.IO) { api.save(original.id, original.version, edited.fields()) })
                        } catch (cancel: CancellationException) {
                            throw cancel
                        } catch (thrown: Exception) {
                            if (thrown is PageConflictException) {
                                status.error(PageOutline.error(thrown, ""), "Reload") { onReload() }
                            } else {
                                status.error(PageOutline.error(thrown, "The page was not saved."))
                            }
                        }
                        busy = false
                    }
                }) { Text("Save") }
            })
            FmInlineStatus(status, Modifier.padding(horizontal = 16.dp))
            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    NoteEmojiButton(emoji, editable = true) { picking = true }
                    FmTextField(value = title, onValueChange = { title = it.take(PageOutline.MAX_TITLE) }, hint = "Untitled page", modifier = Modifier.weight(1f))
                }
                FmTextField(
                    value = description,
                    onValueChange = { description = it.take(PageOutline.MAX_DESCRIPTION) },
                    hint = "Description (Markdown)",
                    minLines = 2,
                    maxLines = 6,
                )
                Text("Colour", style = Fm.text.labelMedium.copy(color = Fm.colors.textSecondary))
                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    for (option in PageAccent.entries) {
                        val color = pageColor(option)
                        Box(
                            Modifier
                                .size(32.dp)
                                .clip(CircleShape)
                                .border(2.dp, if (option == accent) Fm.colors.textPrimary else Color.Transparent, CircleShape)
                                .padding(4.dp)
                                .clip(CircleShape)
                                .background(color)
                                .clickable(role = Role.RadioButton, onClickLabel = option.label) { accent = option }
                                .semantics { contentDescription = option.label + if (option == accent) ", selected" else "" },
                        )
                    }
                }
                Text("Layout", style = Fm.text.labelMedium.copy(color = Fm.colors.textSecondary))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (option in PageLayout.entries) {
                        FmToneButton(option.label, onPressed = { layout = option }, tone = if (layout == option) FmTone.Accent else FmTone.Neutral, filled = layout == option, height = 30.dp)
                    }
                }
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text("Show note emoji in the outline", style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
                    FmSwitch(checked = showEmoji, onCheckedChange = { showEmoji = it })
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Outline", style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
                    Text("${outline.size} of ${PageOutline.MAX_ENTRIES}", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                }
            }
            OutlineEditor(outline, known, onChange = { outline = it }, onRename = { renaming = it })
            Row(
                Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                val room = PageOutline.canAdd(outline)
                FmToneButton("Add note", icon = "add", onPressed = if (room) ({ adding = OutlineAdd.Note }) else null, height = 30.dp)
                FmToneButton("Add section", icon = "segment", onPressed = if (room) ({ adding = OutlineAdd.Section }) else null, height = 30.dp)
                FmToneButton("Add link", icon = "add_link", onPressed = if (room) ({ adding = OutlineAdd.Link }) else null, height = 30.dp)
            }
            when (adding) {
                OutlineAdd.Note -> AddNotePanel(outline, onAdd = { note ->
                    known = known + (note.id to note)
                    outline = PageOutline.addNote(outline, note.id)
                }) { adding = OutlineAdd.None }
                OutlineAdd.Section -> AddSectionPanel(onAdd = { outline = PageOutline.addSection(outline, it) }) { adding = OutlineAdd.None }
                OutlineAdd.Link -> AddLinkPanel(onAdd = { label, url -> outline = PageOutline.addLink(outline, label, url) }) { adding = OutlineAdd.None }
                OutlineAdd.None -> Unit
            }
            Spacer(Modifier.height(24.dp))
        }
    }
    if (picking) {
        NoteEmojiPicker(emoji, onPick = {
            emoji = it
            picking = false
        }) { picking = false }
    }
    val index = renaming
    if (index != null && index in outline.indices) {
        var label by remember(index) { mutableStateOf(outline[index].label) }
        FmConfirmDialog(
            title = "Rename",
            confirmLabel = "Rename",
            onConfirm = {
                outline = PageOutline.rename(outline, index, label)
                renaming = null
            },
            onDismissRequest = { renaming = null },
            fields = {
                FmTextField(
                    value = label,
                    onValueChange = { label = it.take(PageOutline.MAX_TITLE) },
                    hint = PageOutline.title(outline[index].copy(label = ""), known[outline[index].noteId]),
                    autofocus = true,
                )
            },
        )
    }
}

@Composable
private fun OutlineEditor(outline: List<PageEntry>, known: Map<String, PageNote>, onChange: (List<PageEntry>) -> Unit, onRename: (Int) -> Unit) {
    val rowHeight = 52.dp
    val rowPx = with(LocalDensity.current) { rowHeight.toPx() }
    var dragging by remember { mutableIntStateOf(-1) }
    var dragOffset by remember { mutableFloatStateOf(0f) }
    var menuFor by remember { mutableIntStateOf(-1) }
    if (outline.isEmpty()) {
        Text(
            "Add notes, sections and links below. Drag the handle to reorder.",
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
        )
    }
    Column(Modifier.fillMaxWidth()) {
        for ((index, entry) in outline.withIndex()) {
            val note = known[entry.noteId]
            val title = PageOutline.title(entry, if (entry.isNote) note else null)
            val moving = dragging == index
            val actions = buildList {
                if (index > 0) add(CustomAccessibilityAction("Move up") { onChange(PageOutline.move(outline, index, index - 1)); true })
                if (index < outline.lastIndex) add(CustomAccessibilityAction("Move down") { onChange(PageOutline.move(outline, index, index + 1)); true })
                add(CustomAccessibilityAction("Indent") { onChange(PageOutline.indent(outline, index)); true })
                add(CustomAccessibilityAction("Outdent") { onChange(PageOutline.outdent(outline, index)); true })
            }
            Row(
                Modifier
                    .fillMaxWidth()
                    .height(rowHeight)
                    .zIndex(if (moving) 1f else 0f)
                    .offset { IntOffset(0, if (moving) dragOffset.roundToInt() else 0) }
                    .background(if (moving) Fm.colors.bgRaised else Color.Transparent)
                    .semantics { customActions = actions }
                    .padding(end = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(
                    Modifier
                        .size(44.dp)
                        .pointerInput(outline) {
                            detectDragGesturesAfterLongPress(
                                onDragStart = {
                                    dragging = index
                                    dragOffset = 0f
                                },
                                onDragEnd = {
                                    val target = (index + (dragOffset / rowPx).roundToInt()).coerceIn(0, outline.lastIndex)
                                    dragging = -1
                                    dragOffset = 0f
                                    onChange(PageOutline.move(outline, index, target))
                                },
                                onDragCancel = {
                                    dragging = -1
                                    dragOffset = 0f
                                },
                            ) { change, amount ->
                                change.consume()
                                dragOffset += amount.y
                            }
                        }
                        .semantics { contentDescription = "Reorder $title. Long press and drag." },
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon("reorder", size = 20.dp, tint = Fm.colors.textTertiary)
                }
                Spacer(Modifier.width((entry.depth * 16).dp))
                FmIcon(
                    when {
                        entry.isSection -> "segment"
                        entry.isLink -> "link"
                        note == null -> "lock_outline"
                        else -> "notes"
                    },
                    size = 18.dp,
                    tint = Fm.colors.textSecondary,
                )
                Spacer(Modifier.width(8.dp))
                val emoji = if (entry.isNote) note?.emoji.orEmpty() else ""
                Text(
                    NoteEmoji.titled(emoji, title),
                    style = Fm.text.bodyMedium.copy(
                        color = Fm.colors.textPrimary,
                        fontWeight = if (entry.isSection) FontWeight.SemiBold else FontWeight.Normal,
                    ),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                Box {
                    FmIconButton("more_vert", "Edit $title", onPressed = { menuFor = index }, color = Fm.colors.textSecondary)
                    FmDropdownMenu(expanded = menuFor == index, onDismissRequest = { menuFor = -1 }) {
                        fun act(change: () -> List<PageEntry>) {
                            menuFor = -1
                            onChange(change())
                        }
                        FmDropdownMenuItem(text = "Rename", icon = "edit", onClick = { menuFor = -1; onRename(index) })
                        FmDropdownMenuItem(text = "Indent", icon = "arrow_forward", onClick = { act { PageOutline.indent(outline, index) } })
                        FmDropdownMenuItem(text = "Outdent", icon = "arrow_back", onClick = { act { PageOutline.outdent(outline, index) } })
                        if (index > 0) FmDropdownMenuItem(text = "Move up", icon = "arrow_upward", onClick = { act { PageOutline.move(outline, index, index - 1) } })
                        if (index < outline.lastIndex) FmDropdownMenuItem(text = "Move down", icon = "arrow_downward", onClick = { act { PageOutline.move(outline, index, index + 1) } })
                        FmDropdownMenuItem(text = "Remove", icon = "delete_outline", onClick = { act { PageOutline.remove(outline, index) } })
                    }
                }
            }
        }
    }
}

@Composable
private fun AddNotePanel(outline: List<PageEntry>, onAdd: (PageNote) -> Unit, onClose: () -> Unit) {
    var query by remember { mutableStateOf("") }
    var all by remember { mutableStateOf<List<JsonObject>?>(null) }
    var failed by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        val notes = notesApi() ?: return@LaunchedEffect
        try {
            all = withContext(Dispatchers.IO) { notes.list() }
        } catch (cancel: CancellationException) {
            throw cancel
        } catch (_: Exception) {
            failed = true
        }
    }
    val onPage = outline.filter { it.isNote }.map { it.noteId }.toSet()
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmTextField(value = query, onValueChange = { query = it }, hint = "Search your notes", autofocus = true, modifier = Modifier.weight(1f))
            FmTextButton(onClick = onClose) { Text("Done") }
        }
        val list = all
        when {
            failed -> Text("Your notes could not be loaded.", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
            list == null -> FmCircularProgressIndicator()
            else -> {
                val found = NotesLogic.filter(list, query).take(20)
                if (found.isEmpty()) Text("No notes match.", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                for (item in found) {
                    val id = jsonString(item["id"])
                    val added = id in onPage
                    val note = PageNote(id, jsonString(item["title"]), NoteEmoji.of(item), jsonInt(item["updatedAt"]), jsonString(item["lastEditedBy"]), false, jsonString(item["role"], "owner"))
                    FmListRow(
                        title = NoteEmoji.titled(note.emoji, note.displayTitle),
                        subtitle = if (added) "On this page" else null,
                        showChevron = false,
                        minHeight = 44.dp,
                        padding = PaddingValues(vertical = 4.dp),
                        trailing = { FmIcon(if (added) "check" else "add", size = 20.dp, tint = if (added) Fm.colors.textTertiary else Fm.colors.accent) },
                        onTap = if (added || !PageOutline.canAdd(outline)) null else ({ onAdd(note) }),
                    )
                }
            }
        }
    }
}

@Composable
private fun AddSectionPanel(onAdd: (String) -> Unit, onClose: () -> Unit) {
    var label by remember { mutableStateOf("") }
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
        FmTextField(value = label, onValueChange = { label = it.take(PageOutline.MAX_TITLE) }, hint = "Section name", autofocus = true, modifier = Modifier.weight(1f))
        FmTextButton(enabled = label.isNotBlank(), onClick = {
            onAdd(label)
            onClose()
        }) { Text("Add") }
    }
}

@Composable
private fun AddLinkPanel(onAdd: (String, String) -> Unit, onClose: () -> Unit) {
    var label by remember { mutableStateOf("") }
    var url by remember { mutableStateOf("https://") }
    val valid = PageOutline.isLink(url)
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        FmTextField(value = label, onValueChange = { label = it.take(PageOutline.MAX_TITLE) }, hint = "Label", autofocus = true)
        FmTextField(
            value = url,
            onValueChange = { url = it.trim() },
            hint = "https://",
            errorText = if (url.length > 8 && !valid) "Links must start with https://" else null,
        )
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
            FmTextButton(onClick = onClose) { Text("Cancel") }
            FmTextButton(enabled = valid, onClick = {
                onAdd(label, url)
                onClose()
            }) { Text("Add") }
        }
    }
}
