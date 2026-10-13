package com.forkmesh.nativeapp.feature.chat

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.Orientation
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.draggable
import androidx.compose.foundation.gestures.rememberDraggableState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.chat.ChatRow
import com.forkmesh.nativeapp.chat.DirectMessageFrames
import com.forkmesh.nativeapp.chat.MESH_CONVERSATION
import com.forkmesh.nativeapp.chat.MeshSnapshot
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.chat.RelayConnectionState
import com.forkmesh.nativeapp.chat.RelaySnapshot
import com.forkmesh.nativeapp.chat.TranscriptFollower
import com.forkmesh.nativeapp.chat.canDeleteMessage
import com.forkmesh.nativeapp.chat.continuesGroup
import com.forkmesh.nativeapp.chat.conversationTitle
import com.forkmesh.nativeapp.chat.guestComposeBlocked
import com.forkmesh.nativeapp.chat.meshBadge
import com.forkmesh.nativeapp.chat.meshEmptyMessage
import com.forkmesh.nativeapp.chat.meshMemberGroups
import com.forkmesh.nativeapp.chat.startsNewDay
import com.forkmesh.nativeapp.chat.unreadBannerLabel
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.ui.kit.FmBottomActionBar
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmQuietSounds
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.GuestComposeNotice
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt

private const val FOLLOW_SLACK_ITEMS = 2

class ChatScreen : Screen() {
    private var draft by mutableStateOf("")
    private var channelsOpen by mutableStateOf(false)
    private val status = FmStatus()
    private val follower = TranscriptFollower()

    override val title: String get() = "Chat"

    override fun onBack(): Boolean {
        if (channelsOpen) {
            channelsOpen = false
            return true
        }
        return false
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val relay by ChatFeature.relaySnapshot.collectAsState()
        val mesh by ChatFeature.meshSnapshot.collectAsState()
        val foreground by AppLifecycle.foreground.collectAsState()
        FmQuietSounds()
        val messenger = LocalFmMessenger.current
        val pendingIssue by ChatDepth.pendingIssue.collectAsState()
        LaunchedEffect(Unit) { ChatDepth.notices.collect { status.show(it) } }
        val snapshot = relay
        LaunchedEffect(pendingIssue, snapshot?.currentConversation) {
            val argument = pendingIssue ?: return@LaunchedEffect
            ChatDepth.clearIssue()
            val author = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()
            issueFromCommand(messenger, status, argument, snapshot?.currentConversation.orEmpty(), author)
        }
        val revision by ChatDepth.revision.collectAsState()
        val sessionToken = Services.get<AuthService>()?.session?.collectAsState()?.value?.sessionToken.orEmpty()
        LaunchedEffect(sessionToken) { if (sessionToken.isNotEmpty()) ChatDepth.loadAppCommands() }
        LaunchedEffect(revision) {
            val notice = ChatDepth.hub?.extras?.inviteNotice ?: return@LaunchedEffect
            ChatDepth.hub?.extras?.clearInviteNotice()
            status.show(notice)
        }
        if (snapshot == null) {
            FmEmptyState(icon = "chat_bubble_outline", title = "Chat is starting", message = "Connecting to the relay room.")
            return
        }
        val wide = LocalConfiguration.current.screenWidthDp >= 720
        val inMesh = snapshot.currentConversation == MESH_CONVERSATION
        val groups = if (inMesh) meshMemberGroups(mesh?.members.orEmpty()) else snapshot.rosterGroups
        DisposableEffect(inMesh) {
            ChatFeature.setMeshOnScreen(inMesh)
            onDispose { ChatFeature.setMeshOnScreen(false) }
        }
        LaunchedEffect(inMesh, mesh?.unread, foreground) {
            if (foreground && inMesh && (mesh?.unread ?: 0) > 0) withContext(Dispatchers.IO) { ChatFeature.mesh?.markRead() }
        }
        if (wide) {
            Row(Modifier.fillMaxSize()) {
                Box(Modifier.width(260.dp).fillMaxHeight()) {
                    ChatChannelPanel(navigator, snapshot, mesh, groups, onSelected = {})
                }
                Box(Modifier.width(FmSpace.x2).fillMaxHeight().background(Fm.colors.bgBase))
                Box(Modifier.weight(1f)) { Transcript(navigator, snapshot, mesh, revision, onOpenChannels = null) }
            }
        } else {
            Transcript(navigator, snapshot, mesh, revision, onOpenChannels = { channelsOpen = !channelsOpen }) {
                ChannelDrawer(open = channelsOpen, onClose = { channelsOpen = false }) {
                    ChatChannelPanel(navigator, snapshot, mesh, groups, onSelected = { channelsOpen = false })
                }
            }
        }
    }

    @Composable
    private fun ChannelDrawer(open: Boolean, onClose: () -> Unit, content: @Composable () -> Unit) {
        val duration = 250
        var dragX by remember { mutableFloatStateOf(0f) }
        val closeDistance = with(LocalDensity.current) { 80.dp.toPx() }
        val focus = LocalFocusManager.current
        LaunchedEffect(open) {
            if (open) {
                dragX = 0f
                focus.clearFocus()
            }
        }
        Box(Modifier.fillMaxSize()) {
            AnimatedVisibility(visible = open, enter = fadeIn(tween(duration)), exit = fadeOut(tween(duration))) {
                Box(
                    Modifier
                        .fillMaxSize()
                        .background(Color.Black.copy(alpha = 0.45f))
                        .pointerInput(Unit) { detectTapGestures { onClose() } },
                )
            }
            AnimatedVisibility(
                visible = open,
                enter = slideInHorizontally(tween(duration)) { -it },
                exit = slideOutHorizontally(tween(duration)) { -it },
            ) {
                Box(
                    Modifier
                        .fillMaxSize()
                        .offset { IntOffset(dragX.roundToInt(), 0) }
                        .shadow(18.dp)
                        .background(Fm.colors.bgRaised)
                        .draggable(
                            orientation = Orientation.Horizontal,
                            state = rememberDraggableState { delta -> dragX = (dragX + delta).coerceAtMost(0f) },
                            onDragStopped = { velocity ->
                                if (dragX < -closeDistance || velocity < -1200f) onClose() else dragX = 0f
                            },
                        ),
                ) {
                    content()
                }
            }
        }
    }

    @Composable
    private fun Transcript(
        navigator: Navigator,
        relay: RelaySnapshot,
        mesh: MeshSnapshot?,
        revision: Int,
        onOpenChannels: (() -> Unit)?,
        drawer: @Composable () -> Unit = {},
    ) {
        val colors = Fm.colors
        val context = LocalContext.current
        val messenger = LocalFmMessenger.current
        val conversation = relay.currentConversation
        val inMesh = conversation == MESH_CONVERSATION
        val inOffice = OfficeFrames.isOffice(conversation)
        val rows: List<ChatRow> = remember(conversation, relay, mesh?.messages, revision) {
            if (inMesh) {
                val list = mesh?.messages.orEmpty()
                val hub = ChatDepth.hub
                val shown = if (hub == null) list else list.filter { !hub.extras.isHidden(it.id) }
                shown.mapIndexed { index, message -> ChatRow(message, continuesGroup(shown.getOrNull(index - 1), message), deletable = false) }
            } else {
                ChatDepth.hub?.rows(conversation)
                    ?: relay.messages(conversation).let { list ->
                        list.mapIndexed { index, message -> ChatRow(message, continuesGroup(list.getOrNull(index - 1), message), deletable = canDeleteMessage(message)) }
                    }
            }
        }
        val messages = rows.map { it.message }
        val list = rememberLazyListState()
        val scope = rememberCoroutineScope()
        val focus = LocalFocusManager.current
        val statuses = remember(relay.roster) { relay.roster.associate { it.id to it.userStatus } }
        var tick by remember { mutableLongStateOf(0L) }
        LaunchedEffect(conversation) {
            while (true) {
                delay(2_000)
                tick++
            }
        }
        val typing = remember(conversation, revision, tick) { typingLabelFor(conversation) }
        LaunchedEffect(conversation, messages.lastOrNull()?.id) {
            val action = follower.onMessages(conversation, messages) {
                val info = list.layoutInfo
                val last = info.visibleItemsInfo.lastOrNull()?.index ?: 0
                info.totalItemsCount == 0 || last >= info.totalItemsCount - 1 - FOLLOW_SLACK_ITEMS
            }
            when (action) {
                TranscriptFollower.Action.JUMP_TO_END -> if (messages.isNotEmpty()) list.scrollToItem(messages.lastIndex)
                TranscriptFollower.Action.ANIMATE_TO_END -> if (messages.isNotEmpty()) list.animateScrollToItem(messages.lastIndex)
                TranscriptFollower.Action.NONE -> {}
            }
        }
        val title = if (inOffice) "#" + conversation.removePrefix(OfficeFrames.PREFIX) else conversationTitle(conversation, relay.roster)
        val topic = remember(conversation, revision) { ChatDepth.office?.room(conversation)?.topic.orEmpty() }
        val privateRoom = remember(conversation, revision) { ChatDepth.hub?.isPrivateRoom(conversation) ?: conversation.startsWith("@") }
        val otherRoomsUnread = relay.totalUnread > 0 || (!inMesh && mesh?.hasUnread == true)
        Column(Modifier.fillMaxSize().imePadding()) {
            Box(Modifier.background(colors.bgRaised)) {
                FmPanelHeader(
                    title = title,
                    subtitle = ChatDepth.hub?.serverCopiesLine(conversation)?.ifEmpty { null },
                    leading = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            if (onOpenChannels != null) {
                                Box {
                                    FmIconButton(icon = "menu", label = "Channels", onPressed = onOpenChannels)
                                    if (otherRoomsUnread) {
                                        Box(Modifier.align(Alignment.TopEnd).padding(8.dp).size(8.dp).background(colors.danger, CircleShape))
                                    }
                                }
                                Spacer(Modifier.width(FmSpace.x1))
                            }
                            FmIcon(
                                when {
                                    inMesh -> "lan_outlined"
                                    title.startsWith("@") -> "alternate_email"
                                    privateRoom -> "lock_outline"
                                    inOffice -> "apartment"
                                    else -> "tag"
                                },
                                size = 18.dp,
                                tint = colors.textTertiary,
                            )
                        }
                    },
                    trailing = {
                        val label = if (inMesh) {
                            meshBadge(mesh?.networkActive == true, mesh?.bluetoothActive == true, mesh?.peerCount ?: 0)
                        } else {
                            ChatDepth.hub?.badge(conversation) ?: "encrypted"
                        }
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            DirectCallButtons(conversation)
                            FmIconButton(
                                icon = "timer_outlined",
                                label = "Messages disappear after",
                                onPressed = { scope.launch { chatRetentionFlow(messenger, status, conversation, title) } },
                            )
                            val showSeal = {
                                val summary = ChatDepth.hub?.sealSummary(conversation).orEmpty()
                                if (summary.isNotEmpty()) {
                                    scope.launch {
                                        messenger.showConfirmDialog(
                                            title = if (privateRoom) "End-to-end encryption" else "Public channel",
                                            body = summary,
                                            hideCancel = true,
                                        )
                                    }
                                }
                            }
                            if (!inMesh && DirectMessageFrames.isDirect(conversation)) {
                                FmIconButton(icon = "lock_outline", label = label, onPressed = showSeal, color = colors.success)
                            } else {
                                Box(Modifier.clickable(enabled = !inMesh) { showSeal() }) {
                                    FmStatusBadge(label = label, color = colors.success)
                                }
                            }
                        }
                    },
                )
            }
            Box(Modifier.weight(1f).fillMaxWidth()) {
                Column(Modifier.fillMaxSize()) {
                    if (topic.isNotEmpty()) {
                        Text(
                            topic,
                            modifier = Modifier.fillMaxWidth().background(colors.bgRaised).padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(color = colors.textTertiary),
                        )
                    }
                    if (inMesh) MeshDebugBar()
                    if (!inMesh && relay.totalUnread > 0) {
                        UnreadBanner(relay.totalUnread) {
                            scope.launch {
                                withContext(Dispatchers.IO) { ChatDepth.hub?.markAllRead() ?: ChatFeature.relay?.markAllRead() }
                                if (messages.isNotEmpty()) list.animateScrollToItem(messages.lastIndex)
                            }
                        }
                    }
                    Box(
                        Modifier
                            .weight(1f)
                            .fillMaxWidth()
                            .background(colors.bgBase)
                            .clickable(indication = null, interactionSource = null) { focus.clearFocus() },
                    ) {
                        if (rows.isEmpty()) {
                            FmEmptyState(
                                icon = if (inMesh) "lan_outlined" else "chat_bubble_outline",
                                title = "No messages yet",
                                message = if (inMesh) {
                                    meshEmptyMessage(mesh?.networkActive == true, mesh?.peerCount ?: 0)
                                } else {
                                    "Encrypted relay messages will appear here."
                                },
                            )
                        } else {
                            LazyColumn(
                                state = list,
                                modifier = Modifier.fillMaxSize(),
                                contentPadding = PaddingValues(horizontal = FmSpace.x2, vertical = FmSpace.x3),
                            ) {
                                itemsIndexed(rows, key = { _, r -> r.message.id }) { index, row ->
                                    val message = row.message
                                    Column {
                                        if (startsNewDay(rows.getOrNull(index - 1)?.message, message)) {
                                            ChatDayDivider(message.timestamp)
                                        }
                                        ChatMessageRow(
                                            row = row,
                                            liveStatus = if (inMesh) {
                                                if (message.self) statuses[message.senderId] else null
                                            } else {
                                                statuses[message.senderId]
                                            },
                                            queued = inMesh && message.self && mesh?.queued?.contains(message.id) == true,
                                            onLongPress = if (message.ephemeral) null else ({ scope.launch { messageActionsFlow(messenger, context, navigator, row, inThread = false, status) } }),
                                            onToggleReaction = if (inMesh || inOffice) null else ({ value ->
                                                scope.launch(Dispatchers.IO) { ChatDepth.hub?.toggleReaction(message, value) }
                                            }),
                                            onOpenThread = { navigator.push(ChatThreadScreen(conversation, message.id)) },
                                        )
                                    }
                                }
                            }
                        }
                    }
                    FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4))
                    ChatTypingLine(typing)
                    Composer(relay, inMesh)
                }
                drawer()
            }
        }
    }

    @Composable
    private fun Composer(relay: RelaySnapshot, inMesh: Boolean) {
        val auth = Services.get<AuthService>()
        val guest = auth?.isGuest?.collectAsState()?.value == true
        val conversation = relay.currentConversation
        val blocked = guestComposeBlocked(guest, conversation)
        val requireAccount = AccountFeature.rememberRequireAccount()
        val scope = rememberCoroutineScope()
        if (blocked != null) {
            FmBottomActionBar(padding = PaddingValues(FmSpace.x3)) {
                GuestComposeNotice(action = blocked, onLogIn = { scope.launch { requireAccount(blocked) } })
            }
            return
        }
        if (ChatDepth.hub?.isReadOnly(conversation) == true) {
            FmBottomActionBar(padding = PaddingValues(FmSpace.x3)) {
                Text(
                    "Only ForkMesh posts in this channel.",
                    modifier = Modifier.fillMaxWidth(),
                    style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                )
            }
            return
        }
        val connected = inMesh || OfficeFrames.isOffice(conversation) || relay.state == RelayConnectionState.CONNECTED
        ChatComposerBar(
            conversation = conversation,
            text = draft,
            onTextChange = { next ->
                draft = next
                scope.launch(Dispatchers.IO) { ChatDepth.hub?.composerChanged(next) }
            },
            connected = connected,
            onSend = {
                val text = draft
                status.clear()
                scope.launch {
                    when (val result = ChatFeature.sendChatText(text)) {
                        ChatSendResult.Sent -> draft = ""
                        ChatSendResult.Empty -> {}
                        is ChatSendResult.NeedsAccount -> requireAccount(result.action)
                        is ChatSendResult.Failed -> if (result.message.isNotEmpty()) status.error(result.message)
                    }
                }
            },
        )
    }

    @Composable
    private fun MeshDebugBar() {
        val colors = Fm.colors
        val context = LocalContext.current
        val copied = rememberFmStatus()
        var expanded by remember { mutableStateOf(false) }
        var tick by remember { mutableLongStateOf(0L) }
        LaunchedEffect(Unit) {
            while (true) {
                delay(1000)
                tick++
            }
        }
        val text by produceState("", tick, expanded) {
            value = withContext(Dispatchers.IO) {
                val room = ChatFeature.mesh
                when {
                    room == null -> "mesh not running"
                    expanded -> room.diagnosticsDetail()
                    else -> room.diagnosticsLine()
                }
            }
        }
        Row(
            Modifier
                .fillMaxWidth()
                .background(colors.bgRaised)
                .border(width = 0.5.dp, color = colors.border)
                .clickable { expanded = !expanded }
                .padding(start = FmSpace.x3, top = FmSpace.x1, end = FmSpace.x1, bottom = FmSpace.x1),
        ) {
            Text(
                text,
                modifier = Modifier.weight(1f),
                maxLines = if (expanded) 16 else 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodySmall.copy(fontFamily = FontFamily.Monospace, fontSize = FmType.micro, color = colors.textTertiary),
            )
            FmIconButton(
                icon = "copy",
                label = "Copy mesh diagnostics",
                iconSize = 14.dp,
                color = colors.textTertiary,
                onPressed = {
                    val detail = ChatFeature.mesh?.diagnosticsDetail() ?: text
                    if (copyToClipboard(context, detail)) copied.show("Mesh diagnostics copied") else copied.error("Could not reach the clipboard.")
                },
            )
        }
        FmInlineStatus(copied, Modifier.background(colors.bgRaised).padding(horizontal = FmSpace.x3))
    }

    @Composable
    private fun UnreadBanner(count: Int, onTap: () -> Unit) {
        val colors = Fm.colors
        Row(
            Modifier
                .fillMaxWidth()
                .background(colors.accentSubtle)
                .clickable(onClick = onTap)
                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon("mark_chat_unread_outlined", size = 16.dp, tint = colors.accent)
            Spacer(Modifier.width(FmSpace.x2))
            Text(unreadBannerLabel(count), modifier = Modifier.weight(1f), style = Fm.text.labelMedium.copy(color = colors.accent))
            FmIcon("keyboard_arrow_up_rounded", size = 22.dp, tint = colors.accent, contentDescription = "Mark as read")
        }
    }
}

private fun copyToClipboard(context: Context, text: String): Boolean {
    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager ?: return false
    clipboard.setPrimaryClip(ClipData.newPlainText("ForkMesh", text))
    return true
}
