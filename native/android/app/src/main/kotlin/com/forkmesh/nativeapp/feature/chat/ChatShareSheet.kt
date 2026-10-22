package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.chat.ChatHubSend
import com.forkmesh.nativeapp.chat.ChatShareKind
import com.forkmesh.nativeapp.chat.ChatShareTarget
import com.forkmesh.nativeapp.chat.ChatShareTargets
import com.forkmesh.nativeapp.chat.guestComposeBlocked
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

object ChatShare {
    suspend fun targets(): List<ChatShareTarget> = withContext(Dispatchers.IO) {
        val hub = ChatDepth.hub ?: return@withContext emptyList()
        runCatching {
            val channels = hub.channels
            ChatShareTargets.build(
                channels,
                hub.officeChannels,
                hub.directConversations,
                hub.relay.rosterGroups(),
            )
        }.getOrDefault(emptyList())
    }

    suspend fun searchUsers(query: String, known: List<ChatShareTarget>): List<ChatShareTarget> = withContext(Dispatchers.IO) {
        val direct = ChatDepth.hub?.direct ?: return@withContext emptyList()
        if (!ChatShareTargets.searchable(query)) return@withContext emptyList()
        ChatShareTargets.users(runCatching { direct.searchUsers(query.trim().trimStart('@')) }.getOrDefault(emptyList()), known)
    }

    suspend fun send(target: ChatShareTarget, text: String): ChatSendResult {
        val body = text.trim()
        if (body.isEmpty()) return ChatSendResult.Empty
        val guest = Services.get<AuthService>()?.isGuest?.value == true
        guestComposeBlocked(guest, target.guestProbe)?.let { return ChatSendResult.NeedsAccount(it) }
        return withContext(Dispatchers.IO) {
            val hub = ChatDepth.hub ?: return@withContext ChatSendResult.Failed("Chat is not available.")
            val conversation = if (target.kind == ChatShareKind.USER || target.kind == ChatShareKind.PERSON) {
                val direct = hub.direct ?: return@withContext ChatSendResult.Failed("Direct messages are not available.")
                try {
                    direct.start(target.user).conversation
                } catch (error: Exception) {
                    return@withContext ChatSendResult.Failed(error.message ?: "The conversation could not be started.")
                }
            } else {
                target.conversation
            }
            when (val result = hub.sendText(conversation, body)) {
                ChatHubSend.Sent -> ChatSendResult.Sent
                ChatHubSend.Empty -> ChatSendResult.Empty
                is ChatHubSend.Issue -> {
                    ChatDepth.requestIssue(result.argument)
                    ChatSendResult.Sent
                }
                is ChatHubSend.Failed -> ChatSendResult.Failed(result.message.ifEmpty { "The message could not be sent." })
            }
        }
    }
}

private fun shareIcon(target: ChatShareTarget): String = when (target.kind) {
    ChatShareKind.CHANNEL -> if (target.private) "lock_outline" else "tag"
    ChatShareKind.OFFICE -> "apartment"
    else -> "person_outline"
}

suspend fun pickChatShareTarget(messenger: FmMessenger, title: String = "Send to"): ChatShareTarget? {
    val targets = ChatShare.targets()
    return messenger.showModalBottomSheet<ChatShareTarget>(isScrollControlled = true) { close ->
        var query by remember { mutableStateOf("") }
        var found by remember { mutableStateOf(emptyList<ChatShareTarget>()) }
        var searching by remember { mutableStateOf(false) }
        LaunchedEffect(query) {
            found = emptyList()
            if (!ChatShareTargets.searchable(query)) {
                searching = false
                return@LaunchedEffect
            }
            searching = true
            delay(300)
            found = ChatShare.searchUsers(query, targets)
            searching = false
        }
        val shown = remember(query) { ChatShareTargets.filter(targets, query) }
        val sections = listOf(
            "Channels" to shown.filter { it.kind == ChatShareKind.CHANNEL || it.kind == ChatShareKind.OFFICE },
            "Direct messages" to shown.filter { it.kind == ChatShareKind.DIRECT },
            "Online now" to shown.filter { it.kind == ChatShareKind.PERSON },
            "People" to found,
        ).filter { it.second.isNotEmpty() }
        Text(title, modifier = Modifier.padding(FmSpace.x4), style = Fm.text.titleMedium)
        FmTextField(
            value = query,
            onValueChange = { query = it },
            hint = "Search channels and people",
            leading = { FmIcon("search", size = 18.dp) },
            trailing = if (searching) ({ FmCircularProgressIndicator(Modifier.padding(end = FmSpace.x2).size(18.dp), strokeWidth = 2.dp) }) else null,
            modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4),
        )
        Spacer(Modifier.height(FmSpace.x2))
        if (sections.isEmpty() && !searching) {
            Text(
                when {
                    targets.isEmpty() && query.isBlank() -> "Chat isn't connected yet. Search for a person to send them a direct message."
                    else -> "No channel or person matches."
                },
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
                style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
            )
        }
        LazyColumn(Modifier.weight(1f, fill = false)) {
            for ((header, rows) in sections) {
                item(key = "header:$header") {
                    FmSectionHeader(title = header, modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x3, bottom = FmSpace.x1))
                }
                for (target in rows) {
                    item(key = "${target.kind}:${target.conversation}:${target.user}") {
                        FmListRow(
                            title = target.title,
                            leading = { FmIcon(shareIcon(target), size = 20.dp, tint = Fm.colors.textTertiary) },
                            showChevron = false,
                            minHeight = 48.dp,
                            onTap = { close(target) },
                        )
                    }
                }
            }
        }
        Spacer(Modifier.height(FmSpace.x2))
    }
}

suspend fun composeChatShare(messenger: FmMessenger, target: ChatShareTarget, text: String): String? = messenger.showDialog<String> { done ->
    var message by remember { mutableStateOf(text) }
    FmAlertDialog(
        onDismissRequest = { done(null) },
        title = "Send to ${target.title}",
        actions = {
            FmDialogActions(
                onCancel = { done(null) },
                onConfirm = if (message.isBlank()) null else ({ done(message.trim()) }),
                confirmLabel = "Send",
            )
        },
    ) {
        FmTextField(
            value = message,
            onValueChange = { message = it },
            minLines = 3,
            maxLines = 8,
            autofocus = true,
            modifier = Modifier.fillMaxWidth(),
        )
    }
}
