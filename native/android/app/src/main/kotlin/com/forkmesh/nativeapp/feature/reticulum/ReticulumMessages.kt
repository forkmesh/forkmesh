package com.forkmesh.nativeapp.feature.reticulum

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.DisableSelection
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.reticulum.ChatMessage
import com.forkmesh.nativeapp.reticulum.LxmfAttachment
import com.forkmesh.nativeapp.reticulum.Lxmf
import com.forkmesh.nativeapp.reticulum.MapGroup
import com.forkmesh.nativeapp.reticulum.MessageState
import com.forkmesh.nativeapp.reticulum.ReticulumFormat
import com.forkmesh.nativeapp.reticulum.ReticulumSnapshot
import com.forkmesh.nativeapp.reticulum.Rns
import com.forkmesh.nativeapp.ui.kit.FmCountPill
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

internal fun LazyListScope.messagesTab(
    snapshot: ReticulumSnapshot,
    copy: (String, String) -> Unit,
    onNew: () -> Unit,
    onDeleteConversation: () -> Unit,
    onSave: (LxmfAttachment) -> Unit,
) {
    if (state.currentPeer.isEmpty()) conversationList(snapshot, copy, onNew) else chat(snapshot, onDeleteConversation, onSave)
}

private fun LazyListScope.conversationList(snapshot: ReticulumSnapshot, copy: (String, String) -> Unit, onNew: () -> Unit) {
    item(key = "msg-head") {
        Actions { FmToneButton("New message", onPressed = onNew, icon = "add", tone = FmTone.Accent, filled = true) }
        Note("Messages use LXMF, so Sideband, MeshChat and Nomad Network users can write to you. Announce so they can find you.")
        HashRow("Your address", state.address, copy, "Messaging address")
        Section("Conversations")
        if (state.conversations.isEmpty()) Note("No conversations yet. Start one with New message, or wait for someone to write.")
    }
    items(state.conversations, key = { "conv-${it.peerHex}" }) { conversation ->
        FmListRow(
            conversation.name,
            subtitle = "${ReticulumFormat.conversationPreview(conversation)}\n${ReticulumFormat.messageMeta(conversation.last).substringBefore(" · ")}",
            leading = { GroupMarker(MapGroup.Peer) },
            trailing = if (conversation.unread > 0) ({ FmCountPill(conversation.unread, active = true) }) else null,
            onTap = { state.openConversation(conversation.peerHex) },
        )
    }
}

private fun LazyListScope.chat(snapshot: ReticulumSnapshot, onDeleteConversation: () -> Unit, onSave: (LxmfAttachment) -> Unit) {
    val peer = Rns.fromHex(state.currentPeer) ?: ByteArray(0)
    item(key = "chat-head") {
        val name = snapshot.destinations[state.currentPeer]?.displayName?.ifEmpty { null }
            ?: state.conversations.firstOrNull { it.peerHex == state.currentPeer }?.name ?: Rns.shortHash(peer)
        Row(Modifier.fillMaxWidth().padding(start = FmSpace.x2, end = FmSpace.x4, top = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
            FmIconButton("arrow_back", "Back to conversations", onPressed = { state.closeConversation() })
            Column(Modifier.weight(1f)) {
                Text(name, style = Fm.text.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                SelectionContainer {
                    Text(ReticulumFormat.peerSubtitle(peer, snapshot), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                }
            }
        }
        Actions {
            FmToneButton("Find path", onPressed = if (snapshot.onlineCount > 0) ({ state.requestPath(peer) }) else null, icon = "route")
            FmToneButton("Delete conversation", onPressed = onDeleteConversation, icon = "delete", tone = FmTone.Danger)
        }
        if (state.chat.isEmpty()) Note("No messages yet. Write the first one below.")
    }
    items(state.chat, key = { "bubble-${it.id}" }) { message -> Bubble(message, onSave) }
    item(key = "composer") { Composer(snapshot) }
}

@Composable
private fun Bubble(message: ChatMessage, onSave: (LxmfAttachment) -> Unit) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.lg)
    val fill = if (message.outgoing) colors.accent.copy(alpha = if (colors.dark) 0.22f else 0.12f) else colors.bgRaised
    Row(
        Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3, vertical = FmSpace.x1),
        horizontalArrangement = if (message.outgoing) Arrangement.End else Arrangement.Start,
    ) {
        Column(
            Modifier
                .widthIn(max = 320.dp)
                .background(fill, shape)
                .border(1.dp, colors.border, shape)
                .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            SelectionContainer {
                Column {
                    if (message.title.isNotEmpty()) Text(message.title, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold))
                    Text(message.content.take(20000), style = Fm.text.bodyMedium, color = colors.textPrimary)
                }
            }
            for (attachment in message.attachments) {
                FmToneButton(
                    "Save ${attachment.name} (${ReticulumFormat.bytes(attachment.data.size.toDouble())})",
                    onPressed = if (attachment.data.isEmpty()) null else ({ onSave(attachment) }),
                    icon = "attach_file",
                )
            }
            Text(ReticulumFormat.messageMeta(message), style = Fm.text.labelSmall, color = colors.textTertiary)
            if (message.note.isNotEmpty() && message.state != MessageState.Delivered) {
                Text(message.note, style = Fm.text.bodySmall, color = if (message.state == MessageState.Failed) colors.danger else colors.textTertiary)
            }
            if (message.outgoing && (message.state == MessageState.Failed || message.state == MessageState.Sent)) {
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    FmToneButton(if (message.state == MessageState.Failed) "Retry" else "Send again", onPressed = { state.retryMessage(message.id) }, icon = "refresh")
                    FmToneButton("Delete", onPressed = { state.deleteMessage(message.id) }, icon = "delete")
                }
            }
        }
    }
}

@Composable
private fun Composer(snapshot: ReticulumSnapshot) {
    Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
        Row(verticalAlignment = Alignment.Bottom) {
            FmTextField(
                value = state.composer,
                onValueChange = { state.composer = it },
                hint = "Write a message",
                minLines = 1,
                maxLines = 6,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Text),
                modifier = Modifier.weight(1f).heightIn(min = 44.dp),
            )
            Spacer(Modifier.width(FmSpace.x2))
            FmToneButton("Send", onPressed = if (state.composer.isNotBlank()) ({ state.sendMessage() }) else null, icon = "send", tone = FmTone.Accent, filled = true, height = 40.dp)
        }
        Text(ReticulumFormat.composerHint(state.composer, snapshot.onlineCount > 0), style = Fm.text.bodySmall, color = Fm.colors.textTertiary, modifier = Modifier.padding(top = FmSpace.x1))
    }
}

@Composable
internal fun NewMessageDialog(snapshot: ReticulumSnapshot, onDismiss: () -> Unit, onOpen: (String) -> Unit) {
    var filter by remember { mutableStateOf("") }
    var address by remember { mutableStateOf("") }
    var error by remember { mutableStateOf("") }
    val peers = remember(snapshot.destinations, filter) {
        snapshot.destinations.values
            .filter { it.aspect == Lxmf.DELIVERY_ASPECT && (filter.isBlank() || it.displayName.contains(filter.trim(), true) || it.hex.contains(filter.trim(), true)) }
            .sortedByDescending { it.lastHeard }
            .take(200)
    }
    DisableSelection {
        AlertDialog(
            onDismissRequest = onDismiss,
            title = { Text("New message") },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                    Text("Pick someone who has announced on the network, or paste an LXMF address.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                    FmTextField(value = filter, onValueChange = { filter = it }, hint = "Filter by name", modifier = Modifier.fillMaxWidth())
                    if (peers.isEmpty()) Text("Nobody has announced LXMF yet.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                    for (record in peers) {
                        Row(
                            Modifier.fillMaxWidth().clickable { onOpen(record.hex) }.padding(vertical = FmSpace.x2),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            GroupMarker(MapGroup.Peer)
                            Spacer(Modifier.width(FmSpace.x2))
                            Column(Modifier.weight(1f)) {
                                Text(record.displayName.ifEmpty { Rns.shortHash(record.hash) }, style = Fm.text.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                Text("${ReticulumFormat.hops(record.hops)} · heard ${ReticulumFormat.age(record.lastHeard, snapshot.at)}", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                            }
                            FmIcon("chevron_right", tint = Fm.colors.textTertiary)
                        }
                    }
                    FmTextField(
                        value = address,
                        onValueChange = { address = it; error = "" },
                        hint = "LXMF address, 32 hex characters",
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii, autoCorrectEnabled = false),
                        modifier = Modifier.fillMaxWidth(),
                    )
                    if (error.isNotEmpty()) Text(error, style = Fm.text.bodySmall, color = Fm.colors.danger)
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    val hash = Rns.parseHash(address)
                    when {
                        hash == null -> error = "An LXMF address is 32 hexadecimal characters."
                        hash.contentEquals(state.address) -> error = "That is your own address."
                        else -> onOpen(Rns.hex(hash))
                    }
                }) { Text("Open conversation") }
            },
            dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
        )
    }
}

