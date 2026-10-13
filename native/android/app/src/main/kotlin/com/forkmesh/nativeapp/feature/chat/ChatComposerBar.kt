package com.forkmesh.nativeapp.feature.chat

import android.content.ClipboardManager
import android.content.Context
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.chat.ChatComposerTools
import com.forkmesh.nativeapp.chat.ChatLimits
import com.forkmesh.nativeapp.chat.MESH_CONVERSATION
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.composer.PromptPaste
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.composer.ComposerIconButton
import com.forkmesh.nativeapp.feature.composer.PhotoIntake
import com.forkmesh.nativeapp.feature.composer.PromptComposer
import com.forkmesh.nativeapp.ui.kit.ComposeIdentityBar
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.composeIdentityName
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun ChatComposerBar(
    conversation: String,
    text: String,
    onTextChange: (String) -> Unit,
    connected: Boolean,
    onSend: () -> Unit,
    hint: String = "Message...",
    allowFiles: Boolean = true,
    appCommands: Boolean = true,
) {
    val context = LocalContext.current
    val messenger = LocalFmMessenger.current
    val status = rememberFmStatus()
    val scope = rememberCoroutineScope()
    val session = Services.get<AuthService>()?.session?.collectAsState()?.value
    val relayRoom = conversation != MESH_CONVERSATION
    val canAttach = allowFiles && relayRoom && !OfficeFrames.isOffice(conversation) && ChatDepth.hub != null
    val file = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        status.clear()
        scope.launch {
            val picked = withContext(Dispatchers.IO) {
                runCatching {
                    val resolver = context.contentResolver
                    val name = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                        if (cursor.moveToFirst()) cursor.getString(0) else null
                    } ?: uri.lastPathSegment ?: "file"
                    val mime = resolver.getType(uri) ?: ""
                    val bytes = resolver.openInputStream(uri)?.use { input ->
                        val out = java.io.ByteArrayOutputStream()
                        val chunk = ByteArray(64 * 1024)
                        while (out.size() <= ChatLimits.MAX_SEND_FILE_BYTES) {
                            val read = input.read(chunk)
                            if (read < 0) break
                            out.write(chunk, 0, read)
                        }
                        out.toByteArray()
                    } ?: ByteArray(0)
                    Triple(name, mime, bytes)
                }.getOrNull()
            }
            if (picked == null) {
                status.error("The file could not be read.")
                return@launch
            }
            withContext(Dispatchers.IO) { ChatDepth.hub?.sendAttachment(conversation, picked.first, picked.second, picked.third) }
        }
    }
    fun sendPasted(paste: PromptPaste) {
        scope.launch {
            for (image in paste.images) {
                val photo = withContext(Dispatchers.Default) { runCatching { PhotoIntake.prepare(image.data, image.name) }.getOrNull() }
                if (photo == null) {
                    status.error("That image could not be sent.")
                    continue
                }
                withContext(Dispatchers.IO) { ChatDepth.hub?.sendAttachment(conversation, photo.name, photo.mediaType, photo.bytes) }
            }
        }
    }
    val mentionToken = remember(text) { ChatComposerTools.mentionToken(text) }
    val mentions = remember(text, conversation) {
        val token = mentionToken ?: return@remember emptyList()
        val chat = ChatDepth.hub ?: return@remember emptyList()
        ChatComposerTools.mentionCandidates(chat.mentionNames(conversation), session?.nodeName ?: "", token.query)
    }
    val revision by ChatDepth.revision.collectAsState()
    val slash = remember(text, conversation, revision) {
        val builtIn = ChatComposerTools.slashCommands(text, ChatDepth.hub?.office?.room(conversation)?.canManage == true)
        val apps = if (relayRoom && appCommands) ChatDepth.apps?.suggestions(text).orEmpty() else emptyList()
        builtIn + apps.filter { app -> builtIn.none { it.command == app.command } }
    }
    Column {
        FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3, vertical = FmSpace.x1))
        if (mentions.isNotEmpty() || slash.isNotEmpty()) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
                    .padding(horizontal = FmSpace.x3, vertical = FmSpace.x1),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
            ) {
                for (name in mentions) {
                    Suggestion("@$name") { onTextChange(ChatComposerTools.insertMention(text, name)) }
                }
                for (command in slash) {
                    Suggestion("${command.usage} · ${command.summary}") { onTextChange(command.command + " ") }
                }
            }
        }
        PromptComposer(
            text = text,
            onTextChange = onTextChange,
            onSend = if (connected) onSend else null,
            hint = hint,
            onAttachmentsChanged = null,
            onPasteImages = if (canAttach) ::sendPasted else null,
            header = if (session != null) {
                {
                    ComposeIdentityBar(
                        name = composeIdentityName(session.nodeName, session.email),
                        avatarPng = session.avatarPng,
                        padding = PaddingValues(start = 4.dp, bottom = 4.dp),
                    )
                }
            } else {
                null
            },
            extraControls = {
                if (!connected) {
                    ComposerIconButton(icon = "sync", description = "Reconnect to chat", onClick = { ChatFeature.reconnect() })
                }
                if (canAttach) {
                    ComposerIconButton(icon = "attach_file", description = "Send a file", onClick = { file.launch("*/*") })
                }
                if (relayRoom) {
                    var menu by remember { mutableStateOf(false) }
                    Box {
                        ComposerIconButton(icon = "add_reaction_outlined", description = "Emoji, link and ForkBot", onClick = { menu = true })
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem("Emoji", icon = "emoji_emotions_outlined", onClick = {
                                menu = false
                                scope.launch {
                                    val emoji = pickEmoji(messenger) ?: return@launch
                                    onTextChange(text + emoji)
                                }
                            })
                            FmDropdownMenuItem("Insert link", icon = "link", onClick = {
                                menu = false
                                val clipboard = (context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager)
                                    ?.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(context)?.toString().orEmpty()
                                onTextChange(ChatComposerTools.linkInsertion(text, text.length, text.length, clipboard).text)
                            })
                            if (ChatDepth.hub?.canAskForkbot(conversation) == true) {
                                FmDropdownMenuItem("Ask ForkBot", icon = "auto_awesome", onClick = {
                                    menu = false
                                    onTextChange("@forkbot $text")
                                })
                            }
                        }
                    }
                }
            },
            submitOnEnter = true,
            sendDescription = if (connected) "Send message" else "Reconnect to chat",
        )
    }
}

@Composable
private fun Suggestion(label: String, onTap: () -> Unit) {
    Text(
        label,
        modifier = Modifier
            .background(Fm.colors.bgRaised, RoundedCornerShape(16.dp))
            .clickable(onClick = onTap)
            .heightIn(min = 36.dp)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        style = Fm.text.labelMedium.copy(color = Fm.colors.accent),
    )
}
