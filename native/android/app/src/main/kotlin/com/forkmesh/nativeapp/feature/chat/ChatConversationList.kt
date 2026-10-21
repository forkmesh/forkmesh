package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.chat.ChatConversationItem
import com.forkmesh.nativeapp.chat.ChatMessageInfo
import com.forkmesh.nativeapp.chat.ChatRow
import com.forkmesh.nativeapp.model.ChatReceiptTimes
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

internal fun LazyListScope.conversationItems(
    items: List<ChatConversationItem>,
    group: Boolean,
    colors: ChatBubbleColors,
    typing: String,
    lastOwn: String?,
    revision: Int,
    infoOpen: Boolean,
    canReact: Boolean,
    pullInfo: (String, Float) -> Unit,
    releaseInfo: (Float, Float) -> Unit,
    onLongPress: (ChatRow) -> Unit,
    onTapback: (String) -> Unit,
    onThread: (String) -> Unit,
    onInfo: (String) -> Unit,
) {
    items(items, key = { it.id }) { item ->
        when (item) {
            is ChatConversationItem.Time -> ChatTimeLine(item.at, colors)
            is ChatConversationItem.Notice -> ChatBubbleSystemLine(item.row.message.text, colors)
            is ChatConversationItem.Message -> {
                val bubble = item.bubble
                val row = bubble.row
                val message = row.message
                val hub = ChatDepth.hub
                val life = remember(message.id, bubble.receipt, revision) {
                    ChatBubbleLife(
                        expiresAt = hub?.expiresAt(message),
                        lifetimeMs = hub?.retentionMs(message.conversation) ?: 0L,
                        onServer = hub?.isOnServer(message, bubble.receipt) == true,
                    )
                }
                val swipeable = !message.ephemeral && row.viewOnce == null
                val pressable = !message.ephemeral && row.viewOnce == null
                val tapback = if (canReact && pressable) ({ onTapback(message.id) }) else null
                Column(
                    Modifier.sideSwipe(
                        ChatSwipeHeading.LEFT,
                        enabled = swipeable && !infoOpen,
                        moved = { dx -> pullInfo(message.id, dx) },
                        ended = releaseInfo,
                    ),
                ) {
                    val viewOnce = row.viewOnce
                    ChatBubbleRow(
                        bubble = bubble,
                        group = group,
                        colors = colors,
                        life = life,
                        actions = ChatBubbleActions(
                            onLongPress = if (pressable) ({ onLongPress(row) }) else null,
                            onDoubleTap = tapback,
                            onTapback = tapback,
                            onThread = { onThread(message.id) },
                            onSticker = if (pressable) ({ slug ->
                                AppLifecycle.scope.launch(Dispatchers.IO) { hub?.toggleSticker(message, slug) }
                            }) else null,
                        ),
                        viewOnce = if (viewOnce != null) ({ ViewOnceBubble(viewOnce) }) else null,
                    )
                    if (message.id == lastOwn) {
                        val receiptText = remember(row.receipt, revision) {
                            val times = hub?.receiptTimes(message) ?: ChatReceiptTimes()
                            ChatMessageInfo.receiptStatus(message, row.receipt, times, System.currentTimeMillis())
                        }
                        if (receiptText != null) ChatReceiptLine(receiptText) { onInfo(message.id) }
                    }
                }
            }
        }
    }
    if (typing.isNotEmpty()) {
        item(key = "typing") { ChatTypingBubble(typing, colors) }
    }
}
