package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
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
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.chat.DiscordBridge
import com.forkmesh.nativeapp.chat.DiscordMessage
import com.forkmesh.nativeapp.chat.messageTime
import com.forkmesh.nativeapp.feature.composer.PromptComposer
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmQuietSounds
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.copyMessageText
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmColors
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private fun watchBridge(bridge: DiscordBridge) {
    bridge.startWatching {
        AppLifecycle.scope.launch(Dispatchers.IO) {
            bridge.refresh()
            for (id in bridge.watching) bridge.load(id)
        }
    }
}

class DiscordScreen(private val guild: String, private val channel: String?) : Screen() {
    private var opened = false

    override val title: String get() = "Discord"

    @Composable
    override fun Content(navigator: Navigator) {
        val revision by ChatDepth.revision.collectAsState()
        FmQuietSounds()
        val bridge = remember { ChatDepth.discord() }
        if (bridge == null) {
            FmEmptyState(
                icon = "sports_esports",
                title = "No desktop connected",
                message = "Discord goes through the Discord package on your desktop. Connect to your desktop over the network or the relay first.",
            )
            return
        }
        LaunchedEffect(bridge) {
            withContext(Dispatchers.IO) { bridge.refresh() }
            watchBridge(bridge)
            val target = channel
            if (!opened && target != null && DiscordBridge.validChannelId(target)) {
                opened = true
                navigator.push(DiscordChannelScreen(target))
            }
        }
        val guilds = remember(revision) { bridge.guilds.filter { guild.isEmpty() || it.name == guild } }
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(vertical = FmSpace.x3)) {
            item {
                val status = bridge.status
                val line = when {
                    bridge.loading -> "Loading..."
                    bridge.lastError != null -> bridge.lastError.orEmpty()
                    !status.configured -> "The Discord package on your desktop is not set up yet."
                    !status.connected -> "The desktop's Discord bot is offline."
                    status.bot.isNotEmpty() -> "Connected as ${status.bot}"
                    else -> "Connected"
                }
                Row(Modifier.padding(horizontal = FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
                    Text(line, modifier = Modifier.weight(1f), style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                    val scope = rememberCoroutineScope()
                    FmIconButton(icon = "refresh", label = "Refresh", onPressed = { scope.launch(Dispatchers.IO) { bridge.refresh() } })
                }
            }
            for (group in guilds) {
                item(key = "guild:${group.name}") {
                    Spacer(Modifier.height(FmSpace.x3))
                    Row(Modifier.padding(horizontal = FmSpace.x4)) { FmSectionHeader(title = group.name) }
                }
                items(group.channels, key = { "ch:${it.id}" }) { ch ->
                    FmListRow(
                        title = ch.display.ifEmpty { ch.name },
                        subtitle = ch.topic.ifEmpty { if (ch.canSend) null else "Read only" },
                        onTap = { navigator.push(DiscordChannelScreen(ch.id)) },
                    )
                }
            }
        }
    }
}

class DiscordChannelScreen(private val channelId: String) : Screen() {
    private var draft by mutableStateOf("")
    private var replyTo by mutableStateOf<DiscordMessage?>(null)

    override val title: String get() = ChatDepth.discord()?.channel(channelId)?.let { it.display.ifEmpty { it.name } } ?: "Discord"

    @Composable
    override fun Content(navigator: Navigator) {
        val revision by ChatDepth.revision.collectAsState()
        FmQuietSounds()
        val messenger = LocalFmMessenger.current
        val context = LocalContext.current
        val status = rememberFmStatus()
        val scope = rememberCoroutineScope()
        val bridge = remember { ChatDepth.discord() }
        if (bridge == null) {
            FmEmptyState(icon = "sports_esports", title = "No desktop connected", message = "Connect to your desktop first.")
            return
        }
        DisposableEffect(channelId) {
            bridge.watching = bridge.watching + channelId
            watchBridge(bridge)
            onDispose { bridge.watching = bridge.watching - channelId }
        }
        LaunchedEffect(channelId) { withContext(Dispatchers.IO) { bridge.load(channelId) } }
        val messages: List<DiscordMessage> = remember(revision) { bridge.messages[channelId].orEmpty() }
        val info = remember(revision) { bridge.channel(channelId) }
        val list = rememberLazyListState()
        LaunchedEffect(messages.size) { if (messages.isNotEmpty()) list.scrollToItem(messages.lastIndex) }
        Column(Modifier.fillMaxSize().imePadding()) {
            LazyColumn(
                state = list,
                modifier = Modifier.weight(1f).fillMaxWidth().background(Fm.colors.bgBase),
                contentPadding = PaddingValues(FmSpace.x3),
            ) {
                if (messages.isEmpty()) {
                    item { Text(bridge.lastError ?: "No messages loaded yet.", style = Fm.text.bodyMedium.copy(color = Fm.colors.textTertiary)) }
                }
                items(messages, key = { it.id }) { message ->
                    DiscordMessageRow(message, onLongPress = {
                        scope.launch {
                            val copyText = message.text.ifEmpty { message.fileName }
                            if (info?.canSend != true && copyText.isEmpty()) return@launch
                            val choice = messenger.showModalBottomSheet<String> { close ->
                                Spacer(Modifier.height(FmSpace.x2))
                                Column {
                                    if (info?.canSend == true) {
                                        FmListRow(title = "Reply", leading = { FmIconTile(icon = "reply_outlined") }, onTap = { close("reply") })
                                        FmRowDivider()
                                    }
                                    if (copyText.isNotEmpty()) {
                                        FmListRow(title = "Copy text", leading = { FmIconTile(icon = "copy_outlined") }, onTap = { close("copy") })
                                    }
                                }
                                Spacer(Modifier.height(FmSpace.x2))
                            } ?: return@launch
                            when (choice) {
                                "reply" -> replyTo = message
                                "copy" -> copyMessageText(context, copyText, status)
                            }
                        }
                    })
                }
            }
            FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4))
            if (info?.canSend == true) {
                replyTo?.let { target ->
                    Row(
                        Modifier.fillMaxWidth().padding(start = FmSpace.x4, end = FmSpace.x2),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            "Replying to ${target.sender}",
                            modifier = Modifier.weight(1f),
                            style = Fm.text.labelSmall.copy(color = Fm.colors.textTertiary),
                        )
                        FmIconButton(icon = "close", label = "Cancel reply", onPressed = { replyTo = null })
                    }
                }
                PromptComposer(
                    text = draft,
                    onTextChange = { draft = it.take(DiscordBridge.MAX_SEND_CHARS) },
                    onSend = {
                        val text = draft
                        val reply = replyTo?.id.orEmpty()
                        status.clear()
                        scope.launch {
                            val prepared = withContext(Dispatchers.IO) { runCatching { bridge.prepareSend(channelId, text, reply) } }
                            val confirmation = prepared.getOrElse {
                                status.error(it.message ?: it.toString())
                                return@launch
                            }
                            val ok = messenger.showConfirmDialog(title = "Send to Discord?", body = confirmation.summary, confirmLabel = "Send")
                            if (!ok) return@launch
                            val sent = withContext(Dispatchers.IO) { runCatching { bridge.send(confirmation) } }
                            sent.onSuccess {
                                draft = ""
                                replyTo = null
                            }.onFailure { status.error(it.message ?: it.toString()) }
                        }
                    },
                    hint = "Message Discord...",
                    onAttachmentsChanged = null,
                    dictationEnabled = true,
                    submitOnEnter = true,
                    sendDescription = "Send to Discord",
                )
            } else {
                Text(
                    "The desktop's Discord bot cannot post in this channel.",
                    modifier = Modifier.fillMaxWidth().padding(FmSpace.x4),
                    style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
                )
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun DiscordMessageRow(message: DiscordMessage, onLongPress: () -> Unit) {
    val colors = Fm.colors
    val haptics = LocalHapticFeedback.current
    Column(
        Modifier
            .fillMaxWidth()
            .combinedClickable(
                onClick = {},
                onLongClick = {
                    haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                    onLongPress()
                },
            )
            .padding(vertical = FmSpace.x2),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (message.self) "${message.sender} (you)" else message.sender,
                style = Fm.text.titleSmall.copy(color = FmColors.senderColor(message.sender)),
            )
            Spacer(Modifier.width(FmSpace.x2))
            Text(messageTime(message.timestamp), style = Fm.text.bodySmall.copy(color = colors.textTertiary))
            if (message.edited) Text("  (edited)", style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.textTertiary))
        }
        if (message.text.isNotEmpty()) ChatMessageBody(message.text + if (message.truncated) "…" else "")
        if (message.fileName.isNotEmpty()) Text(message.fileName, style = Fm.text.bodyMedium.copy(color = colors.accent))
        if (message.reactions.isNotEmpty()) {
            Row(Modifier.padding(top = 2.dp)) {
                for (reaction in message.reactions) {
                    Text("${reaction.emoji} ${reaction.count}  ", style = Fm.text.bodySmall)
                }
            }
        }
    }
}
