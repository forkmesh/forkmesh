package com.forkmesh.nativeapp.feature.tools

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.notes.NoteActivity
import com.forkmesh.nativeapp.notes.NoteActivityEntry
import com.forkmesh.nativeapp.notes.NoteActivityPayload
import com.forkmesh.nativeapp.notes.NoteChat
import com.forkmesh.nativeapp.notes.NoteChatMessage
import com.forkmesh.nativeapp.notes.NoteChatPayload
import com.forkmesh.nativeapp.notes.NoteHistory
import com.forkmesh.nativeapp.notes.NoteLiveTab
import com.forkmesh.nativeapp.notes.NoteVersion
import com.forkmesh.nativeapp.notes.NotesApi
import com.forkmesh.nativeapp.notes.apiErrorText
import com.forkmesh.nativeapp.notes.jsonInt
import com.forkmesh.nativeapp.notes.jsonString
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSafeInput
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun NoteChatButton(unread: Int, onClick: () -> Unit) {
    Box {
        FmIconButton("forum_outlined", NoteChat.buttonLabel(unread), onPressed = onClick, color = Fm.colors.textPrimary)
        val badge = NoteChat.badge(unread)
        if (badge.isNotEmpty()) {
            Box(
                Modifier
                    .align(Alignment.TopEnd)
                    .offset(x = (-4).dp, y = 6.dp)
                    .heightIn(min = 16.dp)
                    .clip(RoundedCornerShape(FmRadius.full))
                    .background(Fm.colors.accent)
                    .padding(horizontal = 4.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(badge, style = Fm.text.labelSmall.copy(color = Fm.colors.onAccent))
            }
        }
    }
}

internal class NoteLiveChat(
    val payload: NoteChatPayload,
    val loaded: Boolean,
    val error: String,
    val unread: Int,
    val busy: Boolean,
    val status: FmStatus,
    val send: (body: String, done: () -> Unit) -> Unit,
    val delete: (NoteChatMessage) -> Unit,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun NoteLiveSheet(
    api: NotesApi,
    noteId: String,
    currentVersion: Long,
    canRestore: Boolean,
    me: String,
    tab: NoteLiveTab,
    onTab: (NoteLiveTab) -> Unit,
    historySignal: Int,
    activitySignal: Int,
    chat: NoteLiveChat,
    onDismiss: () -> Unit,
    onRestored: (JsonObject, Long) -> Unit,
) {
    var versions by remember { mutableStateOf<List<JsonObject>?>(null) }
    var historyError by remember { mutableStateOf("") }
    var activity by remember { mutableStateOf<NoteActivityPayload?>(null) }
    var activityError by remember { mutableStateOf("") }
    var shown by remember { mutableStateOf<NoteVersion?>(null) }
    var restoring by remember { mutableStateOf<Long?>(null) }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    LaunchedEffect(noteId, historySignal) {
        if (historySignal > 0 && versions != null) delay(NoteActivity.RELOAD_MS)
        try {
            versions = withContext(Dispatchers.IO) { api.versions(noteId) }
            historyError = ""
        } catch (cancel: CancellationException) {
            throw cancel
        } catch (thrown: Exception) {
            historyError = apiErrorText(thrown, NoteHistory.UNAVAILABLE)
        }
    }
    LaunchedEffect(noteId, activitySignal, historySignal) {
        if ((activitySignal > 0 || historySignal > 0) && activity != null) delay(NoteActivity.RELOAD_MS)
        try {
            activity = withContext(Dispatchers.IO) { api.activity(noteId) }
            activityError = ""
        } catch (cancel: CancellationException) {
            throw cancel
        } catch (thrown: Exception) {
            activityError = apiErrorText(thrown, NoteActivity.UNAVAILABLE)
        }
    }
    fun view(version: Long) {
        busy = true
        scope.launch {
            try {
                shown = withContext(Dispatchers.IO) { api.version(noteId, version) }
                status.clear()
            } catch (cancel: CancellationException) {
                throw cancel
            } catch (thrown: Exception) {
                status.error(NoteVersion.error(thrown))
            }
            busy = false
        }
    }
    fun restore(version: Long) {
        busy = true
        scope.launch {
            try {
                val note = withContext(Dispatchers.IO) { api.restore(noteId, version, currentVersion) }
                shown = null
                onRestored(note, version)
            } catch (cancel: CancellationException) {
                throw cancel
            } catch (thrown: Exception) {
                status.error(apiErrorText(thrown, "That version was not restored."))
            }
            busy = false
        }
    }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding()) {
            FmPanelHeader(title = tab.label, subtitle = subtitle(tab, versions, activity, chat))
            FmSegmentTabBar(
                tabs = NoteLiveTab.entries.map { FmSegmentTab(it.label, count = if (it == NoteLiveTab.Chat && chat.unread > 0) chat.unread else null) },
                index = tab.ordinal,
                onChanged = { onTab(NoteLiveTab.entries[it]) },
            )
            when (tab) {
                NoteLiveTab.History -> {
                    FmInlineStatus(status, Modifier.padding(horizontal = 16.dp))
                    val open = shown
                    if (open != null) {
                        NoteVersionView(open, open.version == currentVersion, canRestore, busy, onBack = { shown = null }) { restoring = open.version }
                    } else {
                        NoteHistoryList(versions, historyError, currentVersion, canRestore, busy, onView = ::view) { restoring = it }
                    }
                }
                NoteLiveTab.Activity -> NoteActivityList(activity, activityError, me)
                NoteLiveTab.Chat -> NoteChatPanel(chat, me)
            }
        }
    }
    val pending = restoring
    if (pending != null) {
        FmConfirmDialog(
            title = NoteHistory.restoreTitle(pending),
            body = NoteHistory.restorePrompt(pending),
            confirmLabel = "Restore",
            onConfirm = {
                restoring = null
                restore(pending)
            },
            onDismissRequest = { restoring = null },
        )
    }
}

private fun subtitle(tab: NoteLiveTab, versions: List<JsonObject>?, activity: NoteActivityPayload?, chat: NoteLiveChat): String? = when (tab) {
    NoteLiveTab.History -> versions?.let { NoteActivity.count(it.size.toLong(), "saved version", "saved versions") }
    NoteLiveTab.Activity -> activity?.summary
    NoteLiveTab.Chat -> if (chat.loaded) NoteActivity.count(chat.payload.messages.size.toLong(), "message", "messages") else null
}

@Composable
private fun NoteHistoryList(
    versions: List<JsonObject>?,
    error: String,
    currentVersion: Long,
    canRestore: Boolean,
    busy: Boolean,
    onView: (Long) -> Unit,
    onRestore: (Long) -> Unit,
) {
    when {
        versions == null && error.isEmpty() -> Box(Modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
        versions == null -> FmEmptyState("history", "History unavailable", message = error)
        versions.isEmpty() -> Text(NoteHistory.EMPTY, Modifier.padding(16.dp), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
        else -> LazyColumn(Modifier.fillMaxWidth().height(400.dp)) {
            items(versions) { entry ->
                val version = jsonInt(entry["version"])
                FmListRow(
                    title = "Version $version",
                    subtitle = NoteHistory.subtitle(jsonInt(entry["createdAt"]), jsonString(entry["author"])),
                    showChevron = false,
                    onTap = if (busy) null else ({ onView(version) }),
                    trailing = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            FmTextButton(enabled = !busy, onClick = { onView(version) }) { Text("View") }
                            when {
                                version == currentVersion -> FmStatusBadge("Current", Fm.colors.accent)
                                canRestore -> FmTextButton(enabled = !busy, onClick = { onRestore(version) }) { Text("Restore") }
                            }
                        }
                    },
                )
                FmRowDivider(indent = 16.dp)
            }
        }
    }
}

@Composable
private fun NoteVersionView(version: NoteVersion, current: Boolean, canRestore: Boolean, busy: Boolean, onBack: () -> Unit, onRestore: () -> Unit) {
    val context = LocalContext.current
    Column(Modifier.fillMaxWidth().height(440.dp)) {
        Row(Modifier.fillMaxWidth().padding(end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            FmIconButton("arrow_back", "Back to all versions", onPressed = onBack, color = Fm.colors.textPrimary)
            Column(Modifier.weight(1f)) {
                Text(NoteHistory.viewTitle(version.version, current), style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary))
                Text(NoteHistory.subtitle(version.createdAt, version.author), style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
            }
            if (!current && canRestore) {
                FmTextButton(enabled = !busy, onClick = onRestore) { Text("Restore") }
            }
        }
        Text(
            listOf(version.emoji, version.title.ifBlank { "Untitled note" }).filter { it.isNotEmpty() }.joinToString(" "),
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            style = Fm.text.titleLarge.copy(color = Fm.colors.textPrimary),
        )
        MarkdownPreview(version.markdown, Modifier.weight(1f).fillMaxWidth().padding(horizontal = 16.dp)) { url -> NavigationFeature.openUri(context, url) }
    }
}

@Composable
private fun NoteActivityList(activity: NoteActivityPayload?, error: String, me: String) {
    when {
        activity == null && error.isEmpty() -> Box(Modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
        activity == null -> FmEmptyState("history", "Activity unavailable", message = error)
        activity.entries.isEmpty() -> Text(NoteActivity.EMPTY, Modifier.padding(16.dp), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
        else -> LazyColumn(Modifier.fillMaxWidth().height(400.dp)) {
            items(activity.entries) { entry -> NoteActivityRow(entry, me) }
        }
    }
}

@Composable
private fun NoteActivityRow(entry: NoteActivityEntry, me: String) {
    val line = entry.line(me)
    val time = if (entry.at > 0) relativeTime(entry.at) else ""
    FmListRow(
        title = line,
        titleContent = { Text(line, style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary), maxLines = 3) },
        subtitle = time,
        leading = { FmIconTile(entry.icon, size = 32.dp, iconSize = 17.dp) },
        showChevron = false,
        minHeight = 52.dp,
        modifier = Modifier.semantics { contentDescription = listOf(line, time).filter { it.isNotEmpty() }.joinToString(", ") },
    )
}

@Composable
private fun NoteChatPanel(chat: NoteLiveChat, me: String) {
    var draft by remember { mutableStateOf("") }
    var deleting by remember { mutableStateOf<NoteChatMessage?>(null) }
    val list = rememberLazyListState()
    val messages = chat.payload.messages
    LaunchedEffect(messages.size, chat.loaded) {
        if (messages.isNotEmpty()) list.scrollToItem(messages.size - 1)
    }
    val c = Fm.colors
    FmInlineStatus(chat.status, Modifier.padding(horizontal = 16.dp))
    when {
        !chat.loaded && chat.error.isEmpty() -> Box(Modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
        !chat.loaded -> Text(chat.error, Modifier.padding(16.dp), style = Fm.text.bodyMedium.copy(color = c.textSecondary))
        messages.isEmpty() -> Text(NoteChat.EMPTY, Modifier.fillMaxWidth().height(120.dp).padding(16.dp), style = Fm.text.bodyMedium.copy(color = c.textSecondary))
        else -> LazyColumn(Modifier.fillMaxWidth().height(340.dp), state = list, contentPadding = PaddingValues(vertical = 8.dp)) {
            items(messages, key = { it.id }) { message ->
                NoteChatRow(message, me, chat.payload.canModerate, chat.busy) { deleting = message }
            }
        }
    }
    Row(
        Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 8.dp, bottom = 16.dp),
        verticalAlignment = Alignment.Bottom,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        val style = Fm.text.bodyMedium.copy(color = c.textPrimary)
        BasicTextField(
            value = draft,
            onValueChange = FmSafeInput.typing { next: String -> if (next.length <= NoteChat.MAX_BODY) draft = next },
            textStyle = style,
            cursorBrush = SolidColor(c.accent),
            minLines = 1,
            maxLines = 5,
            enabled = chat.loaded,
            modifier = Modifier
                .weight(1f)
                .clip(RoundedCornerShape(FmRadius.md))
                .background(c.bgBase)
                .border(1.dp, c.border, RoundedCornerShape(FmRadius.md))
                .padding(horizontal = 10.dp, vertical = 10.dp),
            decorationBox = { inner ->
                Box {
                    if (draft.isEmpty()) Text("Message everyone in this note", style = style.copy(color = c.textTertiary))
                    inner()
                }
            },
        )
        FmIconButton(
            "send",
            "Send",
            onPressed = if (chat.loaded && !chat.busy && NoteChat.canSend(draft)) ({ chat.send(draft) { draft = "" } }) else null,
            color = c.accent,
        )
    }
    val pending = deleting
    if (pending != null) {
        FmConfirmDialog(
            title = NoteChat.deleteTitle(),
            body = NoteChat.deletePrompt(pending, me),
            confirmLabel = "Delete",
            destructive = true,
            onConfirm = {
                deleting = null
                chat.delete(pending)
            },
            onDismissRequest = { deleting = null },
        )
    }
}

@Composable
private fun NoteChatRow(message: NoteChatMessage, me: String, canModerate: Boolean, busy: Boolean, onDelete: () -> Unit) {
    val c = Fm.colors
    Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 4.dp, top = 6.dp, bottom = 6.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        FmAvatar(message.author.ifEmpty { "?" }, 28.dp, textStyle = Fm.text.labelSmall)
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(message.authorLabel(me), style = Fm.text.labelMedium.copy(color = c.textPrimary))
                Text(relativeTime(message.createdAt), style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
            Text(message.body, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
        }
        if (message.canDelete(me, canModerate)) {
            FmIconButton("delete_outline", "Delete message", onPressed = if (busy) null else onDelete, color = c.textTertiary, iconSize = 20.dp)
        } else {
            Spacer(Modifier.width(4.dp))
        }
    }
}
