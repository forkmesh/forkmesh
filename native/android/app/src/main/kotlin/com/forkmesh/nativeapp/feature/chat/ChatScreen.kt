package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.layout.ExperimentalLayoutApi
import com.forkmesh.nativeapp.chat.peerPresence
import com.forkmesh.nativeapp.chat.peerOnline
import androidx.compose.foundation.layout.isImeVisible
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.ui.unit.IntSize
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.sp
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.shape.RoundedCornerShape
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.fmGlass
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
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
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
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.chat.ChatConversationLayout
import com.forkmesh.nativeapp.chat.ChatRow
import com.forkmesh.nativeapp.chat.DirectMessageFrames
import com.forkmesh.nativeapp.chat.ChatHub
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
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.spring
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.onSizeChanged
import com.forkmesh.nativeapp.chat.ChatMessageInfo
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private const val FOLLOW_SLACK_ITEMS = 2

class ChatScreen : Screen() {
    private var draft by mutableStateOf("")
    private var channelsOpen by mutableStateOf(false)
    private val status = FmStatus()
    private val follower = TranscriptFollower()
    private var infoId by mutableStateOf<String?>(null)
    private var infoOpen by mutableStateOf(false)
    private var infoSettle by mutableIntStateOf(0)
    private val infoReveal = Animatable(0f)

    private fun settleInfo(open: Boolean) {
        infoOpen = open
        infoSettle++
    }

    private val sheets = ArrayList<Job>()

    private fun CoroutineScope.sheet(block: suspend CoroutineScope.() -> Unit) {
        val job = launch(block = block)
        synchronized(sheets) { sheets.add(job) }
        job.invokeOnCompletion { synchronized(sheets) { sheets.remove(job) } }
    }

    private fun revealConversation() {
        channelsOpen = false
        if (infoId != null) settleInfo(false)
        val open = synchronized(sheets) { sheets.toList() }
        for (job in open) job.cancel()
    }

    private fun openInfo(id: String) {
        infoId = id
        settleInfo(true)
    }

    override val title: String get() = "Chat"

    override fun onBack(): Boolean {
        if (infoId != null) {
            settleInfo(false)
            return true
        }
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
        val visible by AppLifecycle.visible.collectAsState()
        FmQuietSounds()
        val messenger = LocalFmMessenger.current
        val pendingIssue by ChatDepth.pendingIssue.collectAsState()
        LaunchedEffect(Unit) { withContext(Dispatchers.IO) { ChatDepth.restoreLastConversation() } }
        LaunchedEffect(Unit) { ChatDepth.notices.collect { status.show(it) } }
        val snapshot = relay
        LaunchedEffect(pendingIssue, snapshot?.currentConversation) {
            val argument = pendingIssue ?: return@LaunchedEffect
            ChatDepth.clearIssue()
            val author = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()
            issueFromCommand(messenger, status, argument, snapshot?.currentConversation.orEmpty(), author)
        }
        val revision by ChatDepth.revision.collectAsState()
        val reveal by ChatDepth.revealRequests.collectAsState()
        var revealSeen by remember { mutableIntStateOf(reveal) }
        LaunchedEffect(reveal) {
            if (reveal != revealSeen) {
                revealSeen = reveal
                revealConversation()
            }
        }
        val sessionToken = Services.get<AuthService>()?.session?.collectAsState()?.value?.sessionToken.orEmpty()
        LaunchedEffect(sessionToken) { if (sessionToken.isNotEmpty()) ChatDepth.loadAppCommands() }
        val lockOn by ChatPrivacy.lockEnabled.collectAsState()
        val locked by ChatPrivacy.chatLocked.collectAsState()
        if (lockOn && locked) {
            ChatLockGate {}
            return
        }
        if (snapshot == null) {
            FmEmptyState(icon = "chat_bubble_outline", title = "Chat is starting", message = "Connecting to the relay room.")
            return
        }
        val focusedConversation = if (visible && !channelsOpen && DirectMessageFrames.isDirect(snapshot.currentConversation)) {
            snapshot.currentConversation
        } else {
            ""
        }
        LaunchedEffect(focusedConversation) { withContext(Dispatchers.IO) { ChatDepth.direct?.screenFocus(focusedConversation) } }
        val liveConversation = if (visible && DirectMessageFrames.isDirect(snapshot.currentConversation)) snapshot.currentConversation else ""
        DisposableEffect(liveConversation) {
            if (liveConversation.isNotEmpty()) DirectLiveFeature.startSharing(liveConversation)
            onDispose { if (liveConversation.isNotEmpty()) DirectLiveFeature.stopSharing(liveConversation) }
        }
        DisposableEffect(Unit) {
            onDispose { AppLifecycle.scope.launch(Dispatchers.IO) { ChatDepth.direct?.screenFocus("") } }
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
        val focus = LocalFocusManager.current
        LaunchedEffect(open) {
            if (open) focus.clearFocus()
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
                        .shadow(18.dp)
                        .background(Fm.colors.bgRaised),
                ) {
                    content()
                }
            }
        }
    }

    @OptIn(ExperimentalLayoutApi::class)
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
        val inDirect = DirectMessageFrames.isDirect(conversation)
        val route by produceState("", conversation, revision) {
            value = if (!inDirect) "" else withContext(Dispatchers.IO) { ChatDepth.direct?.routeLabel(conversation).orEmpty() }
        }
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
        val bubbles = remember(conversation, revision, inMesh) {
            !inMesh && (ChatDepth.hub?.isConversation(conversation) ?: conversation.startsWith("@"))
        }
        val group = remember(conversation, revision) { ChatDepth.hub?.isGroupConversation(conversation) == true }
        val items = remember(rows, bubbles, group) { if (bubbles) ChatConversationLayout.items(rows, group) else emptyList() }
        val bubbleColors = ChatBubbleColors(Fm.dark)
        val liveRevision by DirectLiveFeature.revision.collectAsState()
        val liveEngine = DirectLiveFeature.engine
        val liveHere = inDirect && liveEngine != null
        var drawing by remember(conversation) { mutableStateOf(false) }
        var transcriptSize by remember { mutableStateOf(IntSize.Zero) }
        val imeVisible = WindowInsets.isImeVisible
        var tapbackId by remember(conversation) { mutableStateOf<String?>(null) }
        val list = rememberLazyListState()
        val scope = rememberCoroutineScope()
        val focus = LocalFocusManager.current
        val haptics = FmHaptics.current
        var pageWidth by remember { mutableIntStateOf(0) }
        val width = pageWidth.toFloat().coerceAtLeast(1f)
        LaunchedEffect(conversation) {
            infoId = null
            infoOpen = false
            infoReveal.snapTo(0f)
        }
        LaunchedEffect(infoSettle) {
            if (infoId == null) return@LaunchedEffect
            infoReveal.animateTo(if (infoOpen) 1f else 0f, spring(dampingRatio = 0.9f, stiffness = 340f))
            if (!infoOpen) infoId = null
        }
        val lastOwn = remember(rows) { if (inDirect) ChatMessageInfo.lastOwnId(rows) else null }
        val pullInfo: (String, Float) -> Unit = { id, dx ->
            if (infoId != id) {
                infoId = id
                infoOpen = false
            }
            focus.clearFocus()
            scope.launch { infoReveal.snapTo((-dx / width).coerceIn(0f, 1f)) }
        }
        val releaseInfo: (Float, Float) -> Unit = { dx, fling ->
            if (infoId != null && !infoOpen) {
                val open = ChatMessageInfo.opens(dx, fling, width)
                if (open) haptics.selection()
                settleInfo(open)
            }
        }
        val statuses = remember(relay.roster) { relay.roster.associate { it.id to it.userStatus } }
        var tick by remember { mutableLongStateOf(0L) }
        LaunchedEffect(conversation) {
            while (true) {
                delay(2_000)
                tick++
            }
        }
        val typing = remember(conversation, revision, tick) { typingLabelFor(conversation) }
        val endIndex = if (bubbles) items.lastIndex + (if (typing.isNotEmpty()) 1 else 0) else messages.lastIndex
        LaunchedEffect(conversation, messages.lastOrNull()?.id) {
            val action = follower.onMessages(conversation, messages) {
                val info = list.layoutInfo
                val last = info.visibleItemsInfo.lastOrNull()?.index ?: 0
                info.totalItemsCount == 0 || last >= info.totalItemsCount - 1 - FOLLOW_SLACK_ITEMS
            }
            when (action) {
                TranscriptFollower.Action.JUMP_TO_END -> if (endIndex >= 0) list.scrollToItem(endIndex)
                TranscriptFollower.Action.ANIMATE_TO_END -> if (endIndex >= 0) list.animateScrollToItem(endIndex)
                TranscriptFollower.Action.NONE -> {}
            }
        }
        val title = if (inOffice) "#" + conversation.removePrefix(OfficeFrames.PREFIX) else conversationTitle(conversation, relay.roster)
        val topic = remember(conversation, revision) { ChatDepth.office?.room(conversation)?.topic.orEmpty() }
        val privateRoom = remember(conversation, revision) { ChatDepth.hub?.isPrivateRoom(conversation) ?: conversation.startsWith("@") }
        val otherRoomsUnread = relay.totalUnread > 0 || (!inMesh && mesh?.hasUnread == true)
        Box(Modifier.fillMaxSize().onSizeChanged { pageWidth = it.width }) {
        Box(Modifier.fillMaxSize().imePadding().graphicsLayer { translationX = -infoReveal.value * 0.3f * width }) {
            var chromeTop by remember { mutableIntStateOf(0) }
            var chromeBottom by remember { mutableIntStateOf(0) }
            val density = androidx.compose.ui.platform.LocalDensity.current
            val topInset = with(density) { chromeTop.toDp() }
            val bottomInset = with(density) { chromeBottom.toDp() }
            val canvas = if (bubbles) bubbleColors.canvas else colors.bgBase
            val badgeLabel = if (inMesh) {
                meshBadge(mesh?.networkActive == true, mesh?.bluetoothActive == true, mesh?.peerCount ?: 0)
            } else {
                ChatDepth.hub?.badge(conversation) ?: "encrypted"
            }
            val showSeal = {
                val summary = ChatDepth.hub?.sealSummary(conversation).orEmpty()
                if (summary.isNotEmpty()) {
                    scope.sheet {
                        messenger.showConfirmDialog(
                            title = if (privateRoom) "End-to-end encryption" else "Public channel",
                            body = summary,
                            hideCancel = true,
                        )
                    }
                }
            }
            val oneToOne = bubbles && !group
            val peerOnline = remember(conversation, relay, revision, liveRevision, typing) {
                oneToOne && ChatDepth.hub?.peerOnline(conversation) == true
            }
            val presenceLabel = remember(conversation, relay, revision, liveRevision, typing) {
                if (oneToOne) ChatDepth.hub?.peerPresence(conversation)?.label.orEmpty() else ""
            }
            val detail = if (oneToOne) {
                listOfNotNull(
                    if (peerOnline) null else presenceLabel.ifEmpty { null },
                    route.ifEmpty { null },
                    ChatDepth.hub?.serverCopiesLine(conversation)?.ifEmpty { null },
                ).joinToString(" \u00B7 ")
            } else listOfNotNull(
                route.ifEmpty { null },
                if (!inMesh && !inDirect && relay.onlineCount > 0) "${relay.onlineCount} online" else null,
                badgeLabel,
                ChatDepth.hub?.serverCopiesLine(conversation)?.ifEmpty { null },
            ).joinToString(" \u00B7 ")
            com.forkmesh.nativeapp.ui.kit.FmBackdropHost(Modifier.fillMaxSize(), overlay = {
            Column(
                Modifier
                    .align(Alignment.TopCenter)
                    .fillMaxWidth()
                    .background(androidx.compose.ui.graphics.Brush.verticalGradient(0f to canvas, 0.6f to canvas.copy(alpha = 0.7f), 1f to canvas.copy(alpha = 0f)))
                    .onSizeChanged { chromeTop = it.height },
            ) {
            Row(
                Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (onOpenChannels != null) {
                    ChatGlassCircle("chevron_left", "Channels", onOpenChannels, dot = otherRoomsUnread)
                }
                var roomMenu by remember { mutableStateOf(false) }
                Box(Modifier.weight(1f)) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(min = 48.dp)
                        .fmGlass(RoundedCornerShape(24.dp), interactive = true)
                        .clickable { roomMenu = true }
                        .padding(horizontal = FmSpace.x4),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FmDropdownMenu(expanded = roomMenu, onDismissRequest = { roomMenu = false }) {
                        if (!inMesh) {
                            FmDropdownMenuItem(if (privateRoom) "Encryption" else "About this channel", icon = "lock_outline", onClick = {
                                roomMenu = false
                                showSeal()
                            })
                        }
                        if (inDirect) {
                            FmDropdownMenuItem("Safety number", icon = "verified_user_outlined", onClick = {
                                roomMenu = false
                                navigator.push(SafetyNumberScreen(conversation))
                            })
                        }
                        FmDropdownMenuItem("Messages disappear after", icon = "timer_outlined", onClick = {
                            roomMenu = false
                            scope.sheet { chatRetentionFlow(messenger, status, conversation, title) }
                        })
                    }
                    if (liveHere) Spacer(Modifier.size(DirectLiveGeometry.PHONE_SLOT, 38.dp)) else FmIcon(
                        when {
                            inMesh -> "lan_outlined"
                            title.startsWith("@") -> "alternate_email"
                            privateRoom -> "lock_outline"
                            inOffice -> "apartment"
                            else -> "tag"
                        },
                        size = 20.dp,
                        tint = colors.textPrimary,
                    )
                    Spacer(Modifier.width(FmSpace.x2))
                    Column(Modifier.weight(1f)) {
                        Text(
                            title.removePrefix("#"),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.titleSmall.copy(fontSize = 16.sp, fontWeight = FontWeight.Bold, color = colors.textPrimary),
                        )
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(5.dp),
                            modifier = Modifier.semantics(mergeDescendants = true) {
                                if (peerOnline) contentDescription = listOf("Online", detail).filter { it.isNotEmpty() }.joinToString(", ")
                            },
                        ) {
                            if (peerOnline) DirectLiveOnlineLight(colors.success)
                            if (detail.isNotEmpty()) {
                                Text(detail, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = colors.textSecondary))
                            }
                        }
                    }
                    DirectCallButtons(conversation)
                    GroupCallButtons(conversation)
                }
                if (liveHere) {
                    DirectLivePartnerPhone(
                        conversation,
                        title.removePrefix("@"),
                        Fm.dark,
                        Modifier.align(Alignment.CenterStart).padding(start = FmSpace.x4),
                    )
                }
                }
                if (liveHere) {
                    DirectLiveTouchButton(drawing) {
                        focus.clearFocus()
                        haptics.selection()
                        drawing = !drawing
                    }
                }
                if (!bubbles && onOpenChannels != null) ChatGlassCircle("edit_outlined", "New message", onOpenChannels)
                if (!bubbles && com.forkmesh.nativeapp.feature.market.PackageGateFeature.surfaces.collectAsState().value.agents) {
                    com.forkmesh.nativeapp.feature.composer.PromptButton("Chat", onClick = {
                        com.forkmesh.nativeapp.feature.navigation.NavigationFeature.shell()?.openPrompt()
                    })
                }
                val account by com.forkmesh.nativeapp.feature.account.AccountFeature.shellState.collectAsState()
                val avatarPng = com.forkmesh.nativeapp.core.Services.get<com.forkmesh.nativeapp.feature.account.AuthService>()?.session?.collectAsState()?.value?.avatarPng.orEmpty()
                if (!bubbles) Box(
                    Modifier
                        .height(48.dp)
                        .fmGlass(RoundedCornerShape(24.dp), interactive = true)
                        .clickable { com.forkmesh.nativeapp.feature.navigation.NavigationFeature.shell()?.openHub() }
                        .semantics { contentDescription = "You: projects, tools and settings" }
                        .padding(horizontal = FmSpace.x2 + 5.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    FmAvatar(account.label, 30.dp, avatarPng = avatarPng, textStyle = Fm.text.labelMedium)
                }
            }
                    if (topic.isNotEmpty()) {
                        Text(
                            topic,
                            modifier = Modifier.fillMaxWidth().background(colors.bgRaised).padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(color = colors.textTertiary),
                        )
                    }
                    if (inMesh) {
                        Text(
                            ChatHub.MESH_SEAL_SUMMARY,
                            modifier = Modifier.fillMaxWidth().background(colors.bgRaised).padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(color = colors.textTertiary),
                        )
                        MeshDebugBar()
                    }
                    if (inDirect) PendingDeviceBanner(conversation, revision)
                    if (!inMesh && relay.totalUnread > 0) {
                        UnreadBanner(relay.totalUnread) {
                            scope.launch {
                                withContext(Dispatchers.IO) { ChatDepth.hub?.markAllRead() ?: ChatFeature.relay?.markAllRead() }
                                if (endIndex >= 0) list.animateScrollToItem(endIndex)
                            }
                        }
                    }
            }
            Column(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .onSizeChanged { chromeBottom = it.height },
            ) {
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4))
                if (!bubbles) ChatTypingLine(typing)
                Composer(relay, inMesh)
            }
            }) {
                    Box(
                        Modifier
                            .fillMaxSize()
                            .background(canvas)
                            .onSizeChanged { transcriptSize = it }
                            .then(
                                if (liveHere && liveEngine != null) {
                                    Modifier.pointerInput(conversation, imeVisible) {
                                        detectTapGestures { offset ->
                                            if (imeVisible) {
                                                focus.clearFocus()
                                            } else {
                                                liveEngine.tap(conversation, DirectLiveGeometry.point(offset, size))
                                                DirectLiveHaptics.sent(context)
                                            }
                                        }
                                    }
                                } else {
                                    Modifier.clickable(indication = null, interactionSource = null) { focus.clearFocus() }
                                },
                            ),
                    ) {
                        if (liveHere) DirectLiveInkLayer(conversation)
                        if (rows.isEmpty()) {
                            Box(Modifier.fillMaxSize().padding(top = topInset, bottom = bottomInset)) { FmEmptyState(
                                icon = if (inMesh) "lan_outlined" else "chat_bubble_outline",
                                title = "No messages yet",
                                message = if (inMesh) {
                                    meshEmptyMessage(mesh?.networkActive == true, mesh?.peerCount ?: 0)
                                } else {
                                    "Encrypted relay messages will appear here."
                                },
                            ) }
                        } else {
                            LazyColumn(
                                state = list,
                                modifier = Modifier.fillMaxSize(),
                                contentPadding = if (bubbles) PaddingValues(top = topInset + FmSpace.x2, bottom = bottomInset + 6.dp) else PaddingValues(start = FmSpace.x2, end = FmSpace.x2, top = topInset + FmSpace.x3, bottom = bottomInset + FmSpace.x3),
                            ) {
                                if (bubbles) {
                                    conversationItems(
                                        items = items,
                                        group = group,
                                        colors = bubbleColors,
                                        typing = typing,
                                        lastOwn = lastOwn,
                                        revision = revision,
                                        infoOpen = infoOpen,
                                        canReact = !inOffice,
                                        pullInfo = pullInfo,
                                        releaseInfo = releaseInfo,
                                        onLongPress = { row -> scope.sheet { messageActionsFlow(messenger, context, navigator, row, inThread = false, status) } },
                                        onTapback = { id ->
                                            haptics.selection()
                                            tapbackId = id
                                        },
                                        onThread = { id -> navigator.push(ChatThreadScreen(conversation, id)) },
                                        onInfo = { id -> openInfo(id) },
                                    )
                                    return@LazyColumn
                                }
                                itemsIndexed(rows, key = { _, r -> r.message.id }) { index, row ->
                                    val message = row.message
                                    if (message.senderId == DirectMessageFrames.SYSTEM_SENDER && message.ephemeral) {
                                        ChatSystemLine(message.text)
                                        return@itemsIndexed
                                    }
                                    val swipeable = !inMesh && !message.ephemeral && row.viewOnce == null
                                    Column(
                                        Modifier.sideSwipe(
                                            ChatSwipeHeading.LEFT,
                                            enabled = swipeable && !infoOpen,
                                            moved = { dx -> pullInfo(message.id, dx) },
                                            ended = releaseInfo,
                                        ),
                                    ) {
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
                                            onLongPress = if (message.ephemeral || row.viewOnce != null) null else ({ scope.sheet { messageActionsFlow(messenger, context, navigator, row, inThread = false, status) } }),
                                            onToggleReaction = if (inMesh || inOffice) null else ({ value ->
                                                scope.launch(Dispatchers.IO) { ChatDepth.hub?.toggleReaction(message, value) }
                                            }),
                                            onOpenThread = { navigator.push(ChatThreadScreen(conversation, message.id)) },
                                            onToggleSticker = if (inMesh || inOffice) null else ({ slug ->
                                                scope.launch(Dispatchers.IO) { ChatDepth.hub?.toggleSticker(message, slug) }
                                            }),
                                        )
                                        ChatRowLinkPreviews(row)
                                        if (message.id == lastOwn) {
                                            val receiptText = remember(row.receipt, revision) {
                                                val times = ChatDepth.hub?.receiptTimes(message) ?: com.forkmesh.nativeapp.model.ChatReceiptTimes()
                                                ChatMessageInfo.receiptStatus(message, row.receipt, times, System.currentTimeMillis())
                                            }
                                            if (receiptText != null) ChatReceiptLine(receiptText) { openInfo(message.id) }
                                        }
                                    }
                                }
                            }
                        }
                        if (liveHere && drawing) DirectLiveTouchSurface(conversation) { drawing = false }
                    }
                drawer()
            }
        }
            val tapbackRow = tapbackId?.let { id -> rows.firstOrNull { it.message.id == id } }
            if (tapbackRow != null) {
                ChatTapbackOverlay(
                    tapbackRow.reactions,
                    onPick = { value ->
                        haptics.tap()
                        tapbackId = null
                        scope.launch(Dispatchers.IO) { ChatDepth.hub?.toggleReaction(tapbackRow.message, value) }
                    },
                    onDismiss = { tapbackId = null },
                )
            }
            val shownId = infoId
            val infoRow = if (shownId == null) null else rows.firstOrNull { it.message.id == shownId }
            if (infoRow != null) {
                val hub = ChatDepth.hub
                val info = remember(infoRow, revision) {
                    val message = infoRow.message
                    ChatMessageInfo.page(
                        infoRow,
                        hub?.receiptTimes(message) ?: com.forkmesh.nativeapp.model.ChatReceiptTimes(),
                        expiresAt = hub?.expiresAt(message),
                        onServer = hub?.isOnServer(message, infoRow.receipt) == true,
                        nowMs = System.currentTimeMillis(),
                    )
                }
                ChatMessageInfoPageView(
                    infoRow,
                    info,
                    onClose = { settleInfo(false) },
                    modifier = Modifier
                        .graphicsLayer {
                            translationX = (1f - infoReveal.value) * width
                            shadowElevation = 12.dp.toPx() * infoReveal.value
                        }
                        .sideSwipe(
                            ChatSwipeHeading.RIGHT,
                            enabled = infoOpen,
                            moved = { dx -> scope.launch { infoReveal.snapTo(1f - dx.coerceAtLeast(0f) / width) } },
                            ended = { dx, fling -> settleInfo(!ChatMessageInfo.closes(dx, fling, width)) },
                        ),
                )
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
                style = Fm.text.bodySmall.copy(fontFamily = FmType.mono, fontSize = FmType.micro, color = colors.textTertiary),
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

@Composable
private fun ChatGlassCircle(icon: String, label: String, onClick: () -> Unit, dot: Boolean = false) {
    Box(
        Modifier
            .size(48.dp)
            .fmGlass(CircleShape, interactive = true)
            .clickable(onClick = onClick)
            .semantics {
                role = Role.Button
                contentDescription = label
            },
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = 22.dp, tint = Fm.colors.textPrimary)
        if (dot) Box(Modifier.align(Alignment.TopEnd).padding(9.dp).size(9.dp).background(Fm.colors.danger, CircleShape))
    }
}
