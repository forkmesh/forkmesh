package com.forkmesh.nativeapp.feature.chat

import com.forkmesh.nativeapp.ui.kit.confirmAction
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.ui.kit.FmElevatedButton
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class OfficeChannelSettingsScreen(private val roomId: String) : Screen() {
    override val title: String get() = "Channel settings"

    @Composable
    override fun Content(navigator: Navigator) {
        val messenger = LocalFmMessenger.current
        val scope = rememberCoroutineScope()
        val service = ChatDepth.office
        val room = remember { service?.rooms?.firstOrNull { it.id == roomId } }
        if (service == null || room == null) {
            FmEmptyState(icon = "apartment", title = "Channel not found", message = "That channel is gone.")
            return
        }
        var name by remember { mutableStateOf(room.name) }
        var topic by remember { mutableStateOf(room.topic) }
        var isPrivate by remember { mutableStateOf(room.isPrivate) }
        var members by remember { mutableStateOf<List<String>>(emptyList()) }
        var newMember by remember { mutableStateOf("") }
        var working by remember { mutableStateOf(false) }
        var error by remember { mutableStateOf<String?>(null) }
        var notice by remember { mutableStateOf<String?>(null) }
        suspend fun loadMembers() {
            if (!room.isPrivate) return
            members = withContext(Dispatchers.IO) { runCatching { service.members(room) }.getOrDefault(room.members) }
        }
        LaunchedEffect(roomId) { loadMembers() }
        fun run(block: () -> Unit, after: suspend () -> Unit = {}) {
            working = true
            error = null
            notice = null
            scope.launch {
                val result = withContext(Dispatchers.IO) { runCatching(block) }
                working = false
                result.onSuccess { after() }.onFailure { error = it.message ?: it.toString() }
            }
        }
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4)) {
            FmSectionHeader(title = "Name")
            FmTextField(value = name, onValueChange = { name = it }, hint = "channel-name", keyboardOptions = KeyboardOptions(autoCorrectEnabled = false), modifier = Modifier.fillMaxWidth())
            Spacer(Modifier.height(FmSpace.x4))
            FmSectionHeader(title = "Topic")
            FmTextField(
                value = topic,
                onValueChange = { topic = it.take(OfficeFrames.MAX_TOPIC_CHARS) },
                hint = "What this channel is for",
                helperText = "${topic.length}/${OfficeFrames.MAX_TOPIC_CHARS}",
                minLines = 2,
                maxLines = 4,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(FmSpace.x4))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Private", modifier = Modifier.weight(1f), style = Fm.text.bodyLarge)
                FmSwitch(checked = isPrivate, onCheckedChange = { isPrivate = it })
            }
            Spacer(Modifier.height(FmSpace.x3))
            FmElevatedButton(
                onClick = {
                    run({
                        service.update(
                            room,
                            name = name.takeIf { it != room.name },
                            topic = topic.takeIf { it != room.topic },
                            isPrivate = isPrivate.takeIf { it != room.isPrivate },
                        )
                    }) { notice = "Channel saved" }
                },
                enabled = !working,
            ) { Text("Save") }
            FmInlineStatusLine(notice, isError = false, modifier = Modifier.padding(top = FmSpace.x2), onDismiss = { notice = null })
            if (room.isPrivate) {
                Spacer(Modifier.height(FmSpace.x5))
                FmSectionHeader(title = "Members")
                for (member in members) {
                    FmListRow(
                        title = member,
                        showChevron = false,
                        trailing = {
                            FmIconButton(icon = "close", label = "Remove $member", onPressed = {
                                run({ service.removeMember(room, member) }) { loadMembers() }
                            })
                        },
                    )
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(value = newMember, onValueChange = { newMember = it }, hint = "username", keyboardOptions = KeyboardOptions(autoCorrectEnabled = false), modifier = Modifier.weight(1f))
                    FmTextButton(onClick = {
                        val username = newMember.trim()
                        if (username.isEmpty()) return@FmTextButton
                        run({ service.addMember(room, username) }) {
                            newMember = ""
                            loadMembers()
                        }
                    }, enabled = !working) { Text("Add") }
                }
            }
            error?.let {
                Spacer(Modifier.height(FmSpace.x3))
                Text(it, style = Fm.text.bodySmall.copy(color = Fm.colors.danger))
            }
            Spacer(Modifier.height(FmSpace.x6))
            FmTextButton(
                onClick = {
                    scope.launch {
                        val ok = messenger.confirmAction("Delete #${room.name}?", "Delete for everyone", "The channel and its history go away for every member.")
                        if (!ok) return@launch
                        run({ service.delete(room) }) { navigator.pop() }
                    }
                },
                contentColor = Fm.colors.danger,
                enabled = !working,
            ) { Text("Delete channel") }
        }
    }
}
