package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.ChatRetentionException
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace

data class ChatRetentionChoice(val ms: Long?)

suspend fun showChatRetentionSheet(messenger: FmMessenger, title: String, selectedMs: Long?, defaultMs: Long? = null): ChatRetentionChoice? =
    messenger.showModalBottomSheet<ChatRetentionChoice>(showDragHandle = true, isScrollControlled = true) { close ->
        Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
            Text(title, style = Fm.text.titleMedium)
            Spacer(Modifier.height(FmSpace.x1))
            Text(
                "Older messages disappear from your chats on every device where you are signed in.",
                style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
            )
        }
        val check: @Composable (Boolean) -> Unit = { selected ->
            if (selected) FmIcon("check", size = 18.dp, tint = Fm.colors.accent)
        }
        if (defaultMs != null) {
            FmListRow(
                title = "Use default (${ChatRetention.label(defaultMs)})",
                trailing = { check(selectedMs == null) },
                onTap = { close(ChatRetentionChoice(null)) },
                showChevron = false,
                minHeight = 48.dp,
            )
        }
        for (preset in ChatRetention.PRESETS) {
            FmListRow(
                title = preset.label,
                trailing = { check(selectedMs == preset.ms) },
                onTap = { close(ChatRetentionChoice(preset.ms)) },
                showChevron = false,
                minHeight = 48.dp,
            )
        }
        Spacer(Modifier.height(FmSpace.x3))
    }

suspend fun chatRetentionFlow(messenger: FmMessenger, status: FmStatus, conversation: String, title: String) {
    val auth = Services.get<AuthService>() ?: return
    if (auth.session.value == null || auth.isGuest.value) {
        status.error("Sign in to choose when messages disappear.")
        return
    }
    val key = ChatDepth.hub?.retentionKey(conversation) ?: conversation
    val rule = auth.chatRetention.value
    val choice = showChatRetentionSheet(
        messenger,
        "Messages in $title disappear after",
        rule.overrideMs(key),
        rule.defaultMs,
    ) ?: return
    try {
        auth.updateChatRetention(ChatRetention.chatBody(key, choice.ms))
    } catch (refused: ChatRetentionException) {
        status.error(refused.message ?: "Could not save when messages disappear.")
    }
}
