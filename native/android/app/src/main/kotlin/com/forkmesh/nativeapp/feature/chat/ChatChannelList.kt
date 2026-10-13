package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.chat.ChannelGroups
import com.forkmesh.nativeapp.chat.ChatVisitorPresence
import com.forkmesh.nativeapp.chat.MESH_CONVERSATION
import com.forkmesh.nativeapp.chat.MeshSnapshot
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.chat.RelaySnapshot
import com.forkmesh.nativeapp.chat.platformIconName
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.settings.showUserStatusSheet
import com.forkmesh.nativeapp.model.MemberGroup
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun ChatChannelPanel(navigator: Navigator, relay: RelaySnapshot, mesh: MeshSnapshot?, groups: List<MemberGroup>, onSelected: () -> Unit) {
    val revision by ChatDepth.revision.collectAsState()
    val scope = rememberCoroutineScope()
    val messenger = LocalFmMessenger.current
    val status = rememberFmStatus()
    val hub = ChatDepth.hub
    val store = ChatDepth.groups
    var groupsVersion by remember { mutableStateOf(0) }
    val select: (String) -> Unit = { conversation ->
        scope.launch(Dispatchers.IO) {
            val chat = ChatDepth.hub
            if (chat != null) chat.select(conversation) else ChatFeature.relay?.switchConversation(conversation)
        }
        onSelected()
    }
    val channels = remember(relay.channels, revision) { hub?.channels ?: relay.channels }
    val offices = remember(revision) { hub?.officeChannels.orEmpty() }
    val rows = remember(channels, revision, groupsVersion) {
        ChannelGroups.visibleRows(ChannelGroups.layout(store?.state ?: ChannelGroups.State(), channels))
    }
    val allRows = remember(channels, revision, groupsVersion) { ChannelGroups.layout(store?.state ?: ChannelGroups.State(), channels) }
    fun updateGroups(change: (ChannelGroups.State) -> ChannelGroups.State?) {
        if (store?.update(change) == true) groupsVersion++
    }
    LazyColumn(
        Modifier.fillMaxSize().background(Fm.colors.bgRaised),
        contentPadding = PaddingValues(FmSpace.x3),
    ) {
        item {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.weight(1f)) { FmSectionHeader(title = "Channels") }
                var menu by remember { mutableStateOf(false) }
                Box {
                    FmIconButton(icon = "add", label = "New channel or group", onPressed = { menu = true })
                    FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        FmDropdownMenuItem("New channel", icon = "tag", onClick = {
                            menu = false
                            scope.launch {
                                val created = newChannelFlow(messenger, hub) ?: return@launch
                                select(created)
                            }
                        })
                        FmDropdownMenuItem("New group", icon = "folder_outlined", onClick = {
                            menu = false
                            scope.launch {
                                val name = messenger.showPromptDialog(title = "New group", hint = "Group name", confirmLabel = "Create") ?: return@launch
                                updateGroups { ChannelGroups.createGroup(it, name) }
                            }
                        })
                    }
                }
            }
        }
        item { Spacer(Modifier.height(FmSpace.x2)) }
        if (status.message != null) item(key = "status") { FmInlineStatus(status, Modifier.padding(bottom = FmSpace.x2)) }
        for (row in rows) {
            if (row.header) {
                item(key = "group:${row.group}") {
                    var menu by remember { mutableStateOf(false) }
                    Box {
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .combinedClickable(
                                    onClick = { updateGroups { ChannelGroups.setCollapsed(it, row.group, !row.collapsed, row.automatic) } },
                                    onLongClick = if (row.automatic) null else ({ menu = true }),
                                )
                                .heightIn(min = 40.dp)
                                .padding(horizontal = FmSpace.x2),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            FmIcon(if (row.collapsed) "chevron_right" else "expand_more", size = 16.dp, tint = Fm.colors.textTertiary)
                            Spacer(Modifier.width(FmSpace.x2))
                            Text(row.group, style = Fm.text.labelMedium.copy(color = Fm.colors.textTertiary))
                        }
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem("Rename group", icon = "edit_outlined", onClick = {
                                menu = false
                                scope.launch {
                                    val name = messenger.showPromptDialog(title = "Rename group", initialValue = row.group, confirmLabel = "Rename") ?: return@launch
                                    val members = ChannelGroups.renderedMembers(allRows, row.group)
                                    updateGroups { ChannelGroups.renameGroup(it, row.group, name, members) }
                                }
                            })
                            FmDropdownMenuItem("Delete group", icon = "delete_outline", textColor = Fm.colors.danger, onClick = {
                                menu = false
                                val members = ChannelGroups.renderedMembers(allRows, row.group)
                                updateGroups { ChannelGroups.removeGroup(it, row.group, members) }
                            })
                        }
                    }
                }
            } else {
                val channel = row.channel
                item(key = "channel:$channel") {
                    var menu by remember { mutableStateOf(false) }
                    val private = hub?.extras?.isPrivate(channel) == true
                    Box {
                        ChannelTile(
                            title = channel,
                            icon = if (private) "lock_outline" else "tag",
                            selected = relay.currentConversation == channel,
                            unread = relay.hasUnread(channel),
                            onTap = { select(channel) },
                            onLongPress = { menu = true },
                        )
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem("Move to group", icon = "folder_outlined", onClick = {
                                menu = false
                                scope.launch {
                                    val names = ChannelGroups.renderedGroupNames(allRows).filter { !ChannelGroups.renderedGroupIsAutomatic(allRows, it) }
                                    val picked = messenger.showModalBottomSheet<String> { close ->
                                        Spacer(Modifier.height(FmSpace.x2))
                                        FmListRow(title = "No group", onTap = { close("") }, showChevron = false)
                                        for (name in names) FmListRow(title = name, onTap = { close(name) }, showChevron = false)
                                        Spacer(Modifier.height(FmSpace.x2))
                                    } ?: return@launch
                                    val members = ChannelGroups.renderedMembers(allRows, picked)
                                    updateGroups { ChannelGroups.assign(it, channel, picked, -1, members, false) }
                                }
                            })
                            FmDropdownMenuItem("Messages disappear after…", icon = "timer_outlined", onClick = {
                                menu = false
                                scope.launch { chatRetentionFlow(messenger, status, channel, channel) }
                            })
                            if (private) {
                                FmDropdownMenuItem("Invite people", icon = "person_add_alt_1_outlined", onClick = {
                                    menu = false
                                    scope.launch { inviteFlow(messenger, channel) }
                                })
                            }
                            if (channel != "#general") {
                                FmDropdownMenuItem("Leave channel", icon = "logout", textColor = Fm.colors.danger, onClick = {
                                    menu = false
                                    scope.launch {
                                        val ok = messenger.showConfirmDialog(
                                            title = "Leave $channel?",
                                            body = "It disappears from this phone. Messages others send there stop arriving here.",
                                            confirmLabel = "Leave channel",
                                            destructive = true,
                                        )
                                        if (!ok) return@launch
                                        withContext(Dispatchers.IO) {
                                            val chat = ChatDepth.hub ?: return@withContext
                                            if (chat.currentConversation == channel) chat.select("#general")
                                            chat.extras.removeChannel(channel)
                                        }
                                    }
                                })
                            }
                        }
                    }
                }
            }
        }
        item(key = "channel:mesh") {
            ChannelTile(MESH_CONVERSATION, "lan_outlined", relay.currentConversation == MESH_CONVERSATION, mesh?.hasUnread == true, { select(MESH_CONVERSATION) })
        }
        if (offices.isNotEmpty()) {
            item { Spacer(Modifier.height(FmSpace.x4)) }
            item { FmSectionHeader(title = "Office") }
            item { Spacer(Modifier.height(FmSpace.x2)) }
            for (conversation in offices) {
                item(key = "office:$conversation") {
                    val room = ChatDepth.office?.room(conversation)
                    var menu by remember { mutableStateOf(false) }
                    Box {
                        ChannelTile(
                            title = conversation.removePrefix(OfficeFrames.PREFIX).let { "#$it" },
                            icon = if (room?.isPrivate == true) "lock_outline" else "apartment",
                            selected = relay.currentConversation == conversation,
                            unread = relay.hasUnread(conversation),
                            onTap = { select(conversation) },
                            onLongPress = { menu = true },
                        )
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem("Messages disappear after…", icon = "timer_outlined", onClick = {
                                menu = false
                                scope.launch { chatRetentionFlow(messenger, status, conversation, "#" + conversation.removePrefix(OfficeFrames.PREFIX)) }
                            })
                            if (room?.canManage == true) {
                                FmDropdownMenuItem("Channel settings", icon = "settings_outlined", onClick = {
                                    menu = false
                                    navigator.push(OfficeChannelSettingsScreen(room.id))
                                })
                            }
                        }
                    }
                }
            }
        }
        item { Spacer(Modifier.height(FmSpace.x2)) }
        item(key = "discord") {
            ChannelTile("Discord (through your desktop)", "sports_esports", false, false, {
                onSelected()
                navigator.push(DiscordScreen("", null))
            })
        }
        item { Spacer(Modifier.height(FmSpace.x4)) }
        item { FmSectionHeader(title = "People") }
        item { Spacer(Modifier.height(FmSpace.x2)) }
        for (group in groups) {
            item(key = "person:${group.id}") {
                MemberGroupTile(group, relay.currentConversation == "@${group.id}") {
                    if (!group.self) {
                        select("@${group.id}")
                    } else {
                        val auth = Services.get<AuthService>()
                        if (auth != null) scope.launch { showUserStatusSheet(messenger, auth) }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ChannelTile(title: String, icon: String, selected: Boolean, unread: Boolean, onTap: () -> Unit, onLongPress: (() -> Unit)? = null) {
    val colors = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .padding(bottom = FmSpace.x1)
            .background(if (selected) colors.accentSubtle else colors.bgRaised, RoundedCornerShape(FmRadius.md))
            .combinedClickable(onClick = onTap, onLongClick = onLongPress)
            .heightIn(min = 44.dp)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 17.dp, tint = if (selected) colors.accent else colors.textTertiary)
        Spacer(Modifier.width(FmSpace.x3))
        Text(
            title,
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.bodyMedium.copy(
                color = if (selected) colors.textPrimary else colors.textSecondary,
                fontSize = FmType.small,
                fontWeight = if (selected) FmType.emphasis else FmType.regular,
            ),
        )
        if (unread) {
            Spacer(Modifier.width(FmSpace.x2))
            Box(Modifier.size(8.dp).background(colors.danger, CircleShape))
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun MemberGroupTile(group: MemberGroup, selected: Boolean, onTap: () -> Unit) {
    val colors = Fm.colors
    val now = System.currentTimeMillis()
    val status = group.members.firstNotNullOfOrNull { it.userStatus }?.visible(now) ?: UserStatus()
    val away = group.online && status.away
    val visitor = !group.self && ChatVisitorPresence.isTransientVisitor(name = group.name)
    Row(
        Modifier
            .fillMaxWidth()
            .padding(bottom = FmSpace.x1)
            .background(if (selected) colors.accentSubtle else colors.bgRaised, RoundedCornerShape(FmRadius.md))
            .combinedClickable(onClick = onTap)
            .heightIn(min = 44.dp)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
    ) {
        Box(
            Modifier
                .padding(top = 4.dp)
                .size(8.dp)
                .background(
                    when {
                        away -> colors.warning
                        group.online -> colors.success
                        else -> colors.offline
                    },
                    CircleShape,
                ),
        )
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    if (group.self) "${group.name} (you)" else group.name,
                    modifier = Modifier.weight(1f, fill = false),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodyMedium.copy(
                        color = if (selected) colors.textPrimary else colors.textSecondary,
                        fontSize = FmType.small,
                        fontWeight = if (selected) FmType.emphasis else FmType.regular,
                    ),
                )
                if (status.emoji.isNotEmpty()) Text(" ${status.emoji}", style = Fm.text.bodySmall)
            }
            if (visitor) Text("Visitor", style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.textTertiary))
            if (away) Text("Away", style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.warning))
            if (group.disambiguator.isNotEmpty()) {
                Text(
                    "id ${group.disambiguator}",
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.textTertiary),
                )
            }
            Spacer(Modifier.height(FmSpace.x1))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                for (node in group.members) {
                    FmIcon(platformIconName(node.platform), size = 13.dp, tint = colors.textTertiary, contentDescription = node.platform.ifEmpty { "node" })
                }
            }
        }
    }
}
