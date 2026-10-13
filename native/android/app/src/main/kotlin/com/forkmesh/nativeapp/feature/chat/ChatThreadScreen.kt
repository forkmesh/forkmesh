package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import com.forkmesh.nativeapp.chat.ChatRow
import com.forkmesh.nativeapp.chat.RelayConnectionState
import com.forkmesh.nativeapp.chat.guestComposeBlocked
import com.forkmesh.nativeapp.chat.startsNewDay
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.ui.kit.FmBottomActionBar
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmQuietSounds
import com.forkmesh.nativeapp.ui.kit.GuestComposeNotice
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class ChatThreadScreen(private val channel: String, private val rootId: String) : Screen() {
    private var draft by mutableStateOf("")

    override val title: String get() = "Thread in $channel"

    @Composable
    override fun Content(navigator: Navigator) {
        val relay by ChatFeature.relaySnapshot.collectAsState()
        val revision by ChatDepth.revision.collectAsState()
        FmQuietSounds()
        val context = LocalContext.current
        val messenger = LocalFmMessenger.current
        val status = rememberFmStatus()
        val scope = rememberCoroutineScope()
        val hub = ChatDepth.hub
        val root = remember(relay, revision) { hub?.rootMessage(rootId, channel) }
        val rows: List<ChatRow> = remember(relay, revision) { hub?.threadRows(rootId).orEmpty() }
        val rootRow = remember(root, revision) { hub?.rows(channel)?.firstOrNull { it.message.id == rootId } }
        val list = rememberLazyListState()
        LaunchedEffect(rows.size) { if (rows.isNotEmpty()) list.animateScrollToItem(rows.size) }
        if (hub == null) {
            FmEmptyState(icon = "forum_outlined", title = "Chat is starting", message = "Connecting to the relay room.")
            return
        }
        Column(Modifier.fillMaxSize().imePadding()) {
            LazyColumn(
                state = list,
                modifier = Modifier.weight(1f).fillMaxWidth().background(Fm.colors.bgBase),
                contentPadding = PaddingValues(horizontal = FmSpace.x2, vertical = FmSpace.x3),
            ) {
                item(key = "root") {
                    if (rootRow != null) {
                        ChatMessageRow(
                            row = rootRow.copy(continuesGroup = false, thread = null),
                            liveStatus = null,
                            queued = false,
                            onLongPress = { scope.launch { messageActionsFlow(messenger, context, navigator, rootRow, inThread = true, status) } },
                            onToggleReaction = { value -> scope.launch(Dispatchers.IO) { hub.toggleReaction(rootRow.message, value) } },
                            onOpenThread = null,
                        )
                    } else {
                        Text(
                            "The first message of this thread is not on this phone.",
                            modifier = Modifier.padding(FmSpace.x4),
                            style = Fm.text.bodyMedium.copy(color = Fm.colors.textTertiary),
                        )
                    }
                    Spacer(Modifier.height(FmSpace.x3))
                    Text(
                        if (rows.size == 1) "1 reply" else "${rows.size} replies",
                        modifier = Modifier.padding(horizontal = FmSpace.x3),
                        style = Fm.text.labelMedium.copy(color = Fm.colors.textTertiary),
                    )
                }
                itemsIndexed(rows, key = { _, r -> r.message.id }) { index, row ->
                    Column {
                        if (startsNewDay(rows.getOrNull(index - 1)?.message ?: rootRow?.message, row.message)) {
                            ChatDayDivider(row.message.timestamp)
                        }
                        ChatMessageRow(
                            row = row,
                            liveStatus = null,
                            queued = false,
                            onLongPress = { scope.launch { messageActionsFlow(messenger, context, navigator, row, inThread = true, status) } },
                            onToggleReaction = { value -> scope.launch(Dispatchers.IO) { hub.toggleReaction(row.message, value) } },
                            onOpenThread = null,
                        )
                    }
                }
            }
            FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4))
            val auth = Services.get<AuthService>()
            val guest = auth?.isGuest?.collectAsState()?.value == true
            val blocked = guestComposeBlocked(guest, channel)
            val requireAccount = AccountFeature.rememberRequireAccount()
            if (blocked != null) {
                FmBottomActionBar(padding = PaddingValues(FmSpace.x3)) {
                    GuestComposeNotice(action = blocked, onLogIn = { scope.launch { requireAccount(blocked) } })
                }
                return
            }
            ChatComposerBar(
                conversation = channel,
                text = draft,
                onTextChange = { draft = it },
                connected = relay?.state == RelayConnectionState.CONNECTED,
                onSend = {
                    val text = draft
                    scope.launch {
                        val sent = withContext(Dispatchers.IO) { hub.replyInThread(channel, rootId, text) }
                        if (sent) draft = ""
                    }
                },
                hint = "Reply in thread...",
                appCommands = false,
                allowFiles = false,
            )
        }
    }
}
