package com.forkmesh.nativeapp.feature.tools

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
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
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.people.openProfile
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.notes.NoteComment
import com.forkmesh.nativeapp.notes.NoteCommentText
import com.forkmesh.nativeapp.notes.NoteMentions
import com.forkmesh.nativeapp.notes.NotePeer
import com.forkmesh.nativeapp.notes.NotePresence
import com.forkmesh.nativeapp.notes.NoteQuote
import com.forkmesh.nativeapp.notes.NoteThread
import com.forkmesh.nativeapp.notes.NoteThreadList
import com.forkmesh.nativeapp.notes.NoteThreads
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSafeInput
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius

internal class NoteCommentActions(
    val send: (body: String, parentId: String?, quote: NoteQuote?, done: () -> Unit) -> Unit,
    val edit: (commentId: String, body: String, done: () -> Unit) -> Unit,
    val resolve: (commentId: String, resolved: Boolean) -> Unit,
    val delete: (commentId: String) -> Unit,
    val pick: (NoteThread) -> Unit,
)

@Composable
internal fun NotePresenceLine(label: String, peers: List<NotePeer>, onOpen: () -> Unit) {
    val now = System.currentTimeMillis()
    val faces = NotePresence.faces(peers)
    Row(
        Modifier
            .padding(top = 2.dp)
            .clip(RoundedCornerShape(FmRadius.full))
            .clickable(enabled = peers.isNotEmpty(), role = Role.Button, onClickLabel = "Show who is here", onClick = onOpen)
            .padding(vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        for (peer in faces.shown) NotePeerFace(peer, 20.dp, now)
        if (faces.extra > 0) {
            Box(
                Modifier.size(20.dp).clip(CircleShape).background(Fm.colors.bgRaised).border(1.dp, Fm.colors.border, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Text(faces.extraLabel, style = Fm.text.labelSmall.copy(color = Fm.colors.textSecondary))
            }
        }
        Text(label, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary), maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun NotePeerFace(peer: NotePeer, size: Dp, now: Long, modifier: Modifier = Modifier) {
    val editing = peer.isEditing(now)
    Box(modifier.semantics { contentDescription = peer.line(now) }) {
        FmAvatar(peer.name, size, textStyle = Fm.text.labelSmall)
        if (editing) {
            Box(
                Modifier
                    .align(Alignment.BottomEnd)
                    .offset(x = size * 0.18f, y = size * 0.18f)
                    .size(size * 0.62f)
                    .clip(CircleShape)
                    .background(Fm.colors.accent)
                    .border(1.dp, Fm.colors.bgBase, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                FmIcon("edit", size = size * 0.4f, tint = Fm.colors.onAccent)
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun NotePeopleSheet(label: String, peers: List<NotePeer>, onDismiss: () -> Unit) {
    val now = System.currentTimeMillis()
    FmModalBottomSheet(onDismissRequest = onDismiss) {
        FmPanelHeader(title = "Here now", subtitle = label)
        if (peers.isEmpty()) {
            Text("Nobody else has this note open.", modifier = Modifier.padding(16.dp), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
        }
        Column(Modifier.heightIn(max = 420.dp).verticalScroll(rememberScrollState())) {
            for (peer in peers) {
                FmListRow(
                    title = peer.name,
                    subtitle = if (peer.isEditing(now)) "Editing" else "Viewing",
                    leading = { NotePeerFace(peer, 32.dp, now, Modifier.openProfile(peer.name)) },
                    showChevron = false,
                )
                FmRowDivider(indent = 16.dp)
            }
        }
    }
}

@Composable
internal fun NoteCommentsButton(openCount: Int, onClick: () -> Unit) {
    Box {
        FmIconButton("chat_bubble_outline", NoteThreads.buttonLabel(openCount), onPressed = onClick, color = Fm.colors.textPrimary)
        if (openCount > 0) {
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
                Text(if (openCount > 99) "99+" else openCount.toString(), style = Fm.text.labelSmall.copy(color = Fm.colors.onAccent))
            }
        }
    }
}

private fun threadKeys(threads: NoteThreadList, composing: Boolean, showResolved: Boolean): List<String> = buildList {
    if (composing) add("composer")
    threads.open.forEach { add("t:${it.id}") }
    if (threads.resolved.isNotEmpty()) add("resolved")
    if (showResolved) threads.resolved.forEach { add("t:${it.id}") }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun NoteCommentsSheet(
    threads: NoteThreadList,
    people: List<String>,
    canResolve: Boolean,
    isOwner: Boolean,
    me: String,
    loaded: Boolean,
    loadError: String,
    focus: String?,
    draft: NoteQuote?,
    composing: Boolean,
    busy: Boolean,
    status: FmStatus,
    actions: NoteCommentActions,
    onCompose: () -> Unit,
    onCancelCompose: () -> Unit,
    onDismiss: () -> Unit,
) {
    var showResolved by remember { mutableStateOf(false) }
    var editing by remember { mutableStateOf<String?>(null) }
    var deleting by remember { mutableStateOf<NoteComment?>(null) }
    val list = rememberLazyListState()
    LaunchedEffect(focus, loaded) {
        val target = focus ?: return@LaunchedEffect
        if (threads.resolved.any { it.id == target }) showResolved = true
        val index = threadKeys(threads, composing, showResolved || threads.resolved.any { it.id == target }).indexOf("t:$target")
        if (index >= 0) list.animateScrollToItem(index)
    }
    LaunchedEffect(composing) { if (composing) list.scrollToItem(0) }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding()) {
            FmPanelHeader(
                title = "Comments",
                subtitle = if (threads.openCount == 1) "1 open thread" else "${threads.openCount} open threads",
                trailing = {
                    if (!composing) FmIconButton("add_comment", "Comment on the whole note", onPressed = onCompose, color = Fm.colors.accent)
                },
            )
            FmInlineStatus(status, Modifier.padding(horizontal = 16.dp))
            when {
                !loaded && loadError.isEmpty() -> Box(Modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                !loaded -> Text(loadError, modifier = Modifier.padding(16.dp), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
                else -> LazyColumn(Modifier.fillMaxWidth().weight(1f, fill = false), state = list, contentPadding = PaddingValues(bottom = 24.dp)) {
                    if (composing) {
                        item(key = "composer") {
                            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text("New comment", style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary))
                                if (draft != null) {
                                    NoteQuoteBlock(draft.quote, detached = false, onClick = null)
                                } else {
                                    Text("On the whole note", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                                }
                                NoteCommentInput(
                                    people = people,
                                    me = me,
                                    hint = "Add a comment. Type @ to mention someone.",
                                    action = "Comment",
                                    busy = busy,
                                    autofocus = true,
                                    onCancel = onCancelCompose,
                                ) { body, done -> actions.send(body, null, draft) { done(); onCancelCompose() } }
                            }
                            FmRowDivider()
                        }
                    }
                    if (threads.all.isEmpty() && !composing) {
                        item(key = "empty") {
                            Text(
                                "No comments yet. Select text in the note and choose Comment, or comment on the whole note.",
                                modifier = Modifier.padding(16.dp),
                                style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                            )
                        }
                    }
                    for (thread in threads.open) {
                        item(key = "t:${thread.id}") {
                            NoteThreadCard(thread, thread.id == focus, me, canResolve, isOwner, people, busy, editing, { editing = it }, { deleting = it }, actions)
                        }
                    }
                    if (threads.resolved.isNotEmpty()) {
                        item(key = "resolved") {
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .clickable(role = Role.Button) { showResolved = !showResolved }
                                    .padding(horizontal = 16.dp, vertical = 12.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(6.dp),
                            ) {
                                FmIcon(if (showResolved) "expand_more" else "chevron_right", size = 20.dp, tint = Fm.colors.textSecondary)
                                Text(threads.resolvedLabel, style = Fm.text.titleSmall.copy(color = Fm.colors.textSecondary))
                            }
                        }
                    }
                    if (showResolved) {
                        for (thread in threads.resolved) {
                            item(key = "t:${thread.id}") {
                                NoteThreadCard(thread, thread.id == focus, me, canResolve, isOwner, people, busy, editing, { editing = it }, { deleting = it }, actions)
                            }
                        }
                    }
                }
            }
        }
    }
    val doomed = deleting
    if (doomed != null) {
        FmConfirmDialog(
            title = NoteCommentText.deleteTitle(doomed),
            body = NoteCommentText.deletePrompt(doomed, if (doomed.isThread) threads.find(doomed.id)?.replies?.size ?: 0 else 0, me),
            confirmLabel = "Delete",
            destructive = true,
            onConfirm = {
                deleting = null
                actions.delete(doomed.id)
            },
            onDismissRequest = { deleting = null },
        )
    }
}

@Composable
private fun NoteThreadCard(
    thread: NoteThread,
    focused: Boolean,
    me: String,
    canResolve: Boolean,
    isOwner: Boolean,
    people: List<String>,
    busy: Boolean,
    editing: String?,
    onEditing: (String?) -> Unit,
    onDelete: (NoteComment) -> Unit,
    actions: NoteCommentActions,
) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 6.dp)
            .clip(shape)
            .background(if (focused) c.accentSubtle else c.bgRaised)
            .border(1.dp, if (focused) c.accent else c.border, shape)
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        val label = NoteThreads.quoteLabel(thread)
        if (thread.comment.isInline) {
            NoteQuoteBlock(thread.comment.quote, thread.isDetached, onClick = if (thread.anchor != null) ({ actions.pick(thread) }) else null)
        }
        if (label.isNotEmpty()) {
            Text(label, style = Fm.text.labelSmall.copy(color = if (thread.isDetached) c.warning else c.textTertiary))
        }
        NoteCommentEntry(thread.comment, me, isOwner, people, busy, editing == thread.comment.id, onEditing, onDelete, actions)
        for (reply in thread.replies) {
            Row {
                Spacer(Modifier.width(14.dp))
                Box(Modifier.weight(1f)) {
                    NoteCommentEntry(reply, me, isOwner, people, busy, editing == reply.id, onEditing, onDelete, actions)
                }
            }
        }
        if (thread.isResolved) {
            val by = thread.comment.resolvedBy.ifEmpty { "someone" }
            Text("Resolved by $by · ${relativeTime(thread.comment.resolvedAt)}", style = Fm.text.bodySmall.copy(color = c.textTertiary))
        } else {
            NoteCommentInput(people = people, me = me, hint = "Reply", action = "Reply", busy = busy) { body, done ->
                actions.send(body, thread.id, null, done)
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.horizontalScroll(rememberScrollState())) {
            if (thread.canResolve(canResolve, me)) {
                FmToneButton(
                    if (thread.isResolved) "Reopen" else "Resolve",
                    icon = if (thread.isResolved) "undo" else "check_circle_outline",
                    onPressed = if (busy) null else ({ actions.resolve(thread.id, !thread.isResolved) }),
                    height = 30.dp,
                )
            }
            if (thread.anchor != null) {
                FmToneButton("Show in note", icon = "format_quote", onPressed = { actions.pick(thread) }, height = 30.dp)
            }
        }
    }
}

@Composable
private fun NoteCommentEntry(
    comment: NoteComment,
    me: String,
    isOwner: Boolean,
    people: List<String>,
    busy: Boolean,
    isEditing: Boolean,
    onEditing: (String?) -> Unit,
    onDelete: (NoteComment) -> Unit,
    actions: NoteCommentActions,
) {
    val c = Fm.colors
    val author = comment.author.ifEmpty { "Someone" }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FmAvatar(author, 22.dp, Modifier.openProfile(author), textStyle = Fm.text.labelSmall)
            Text(author, style = Fm.text.titleSmall.copy(color = c.textPrimary), maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(comment.meta(), style = Fm.text.bodySmall.copy(color = c.textTertiary), maxLines = 1, modifier = Modifier.weight(1f))
            if (!isEditing) {
                if (comment.canEdit(me)) {
                    FmIconButton("edit", "Edit comment", onPressed = if (busy) null else ({ onEditing(comment.id) }), color = c.textSecondary, iconSize = 18.dp)
                }
                if (comment.canDelete(me, isOwner)) {
                    FmIconButton("delete_outline", "Delete comment", onPressed = if (busy) null else ({ onDelete(comment) }), color = c.textSecondary, iconSize = 18.dp)
                }
            }
        }
        if (isEditing) {
            NoteCommentInput(
                people = people,
                me = me,
                hint = "Edit your comment",
                action = "Save",
                busy = busy,
                initial = comment.body,
                autofocus = true,
                onCancel = { onEditing(null) },
            ) { body, done -> actions.edit(comment.id, body) { done(); onEditing(null) } }
        } else {
            Text(mentionText(comment.body, comment.mentions, c.accent), style = Fm.text.bodyMedium.copy(color = c.textPrimary))
        }
    }
}

private fun mentionText(body: String, mentions: List<String>, color: Color): AnnotatedString = buildAnnotatedString {
    append(body)
    for (range in NoteMentions.ranges(body, mentions)) {
        addStyle(SpanStyle(color = color, fontWeight = FontWeight.SemiBold), range.first, range.last + 1)
    }
}

@Composable
private fun NoteQuoteBlock(quote: String, detached: Boolean, onClick: (() -> Unit)?) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .height(IntrinsicSize.Min)
            .then(if (onClick != null) Modifier.clickable(role = Role.Button, onClickLabel = "Show in note", onClick = onClick) else Modifier),
    ) {
        Box(Modifier.width(3.dp).fillMaxHeight().clip(RoundedCornerShape(2.dp)).background(if (detached) c.textTertiary else c.accent))
        Spacer(Modifier.width(8.dp))
        Text(
            quote,
            maxLines = 3,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.bodySmall.copy(
                color = if (detached) c.textTertiary else c.textSecondary,
                textDecoration = if (detached) TextDecoration.LineThrough else null,
            ),
        )
    }
}

@Composable
private fun NoteCommentInput(
    people: List<String>,
    me: String,
    hint: String,
    action: String,
    busy: Boolean,
    initial: String = "",
    autofocus: Boolean = false,
    onCancel: (() -> Unit)? = null,
    onSend: (String, () -> Unit) -> Unit,
) {
    val c = Fm.colors
    var value by remember { mutableStateOf(TextFieldValue(initial, TextRange(initial.length))) }
    val focus = remember { FocusRequester() }
    if (autofocus) LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    val mention = if (value.selection.collapsed) NoteMentions.active(value.text, value.selection.end) else null
    val suggestions = mention?.let { NoteMentions.suggestions(people, it.query, me) }.orEmpty()
    val style = Fm.text.bodyMedium.copy(color = c.textPrimary)
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        BasicTextField(
            value = value,
            onValueChange = FmSafeInput.typing { next: TextFieldValue -> if (next.text.length <= NoteCommentText.MAX_BODY) value = next },
            textStyle = style,
            cursorBrush = SolidColor(c.accent),
            minLines = 1,
            maxLines = 6,
            modifier = Modifier
                .fillMaxWidth()
                .focusRequester(focus)
                .clip(RoundedCornerShape(FmRadius.md))
                .background(c.bgBase)
                .border(1.dp, c.border, RoundedCornerShape(FmRadius.md))
                .padding(horizontal = 10.dp, vertical = 8.dp),
            decorationBox = { inner ->
                Box {
                    if (value.text.isEmpty()) Text(hint, style = style.copy(color = c.textTertiary))
                    inner()
                }
            },
        )
        if (mention != null && suggestions.isNotEmpty()) {
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                for (name in suggestions) {
                    Row(
                        Modifier
                            .clip(RoundedCornerShape(FmRadius.full))
                            .background(c.accentSubtle)
                            .clickable(role = Role.Button, onClickLabel = "Mention $name") {
                                val edit = NoteMentions.insert(value.text, mention, name)
                                value = TextFieldValue(edit.text, TextRange(edit.start, edit.end))
                            }
                            .padding(horizontal = 8.dp, vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        FmAvatar(name, 18.dp, textStyle = Fm.text.labelSmall)
                        Text("@$name", style = Fm.text.bodySmall.copy(color = c.textPrimary))
                    }
                }
            }
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End), verticalAlignment = Alignment.CenterVertically) {
            if (onCancel != null) FmTextButton(onClick = onCancel, minHeight = 30.dp) { Text("Cancel") }
            FmToneButton(
                action,
                onPressed = if (!busy && NoteCommentText.canSend(value.text)) ({ onSend(value.text) { value = TextFieldValue("") } }) else null,
                tone = FmTone.Accent,
                filled = true,
                height = 30.dp,
            )
        }
    }
}
