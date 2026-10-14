package com.forkmesh.nativeapp.feature.chat

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.viewinterop.AndroidView
import android.graphics.BitmapFactory
import android.media.MediaPlayer
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.apps.AppsText
import com.forkmesh.nativeapp.chat.ChatAttachment
import com.forkmesh.nativeapp.chat.ChatAttachmentKind
import com.forkmesh.nativeapp.chat.ChatRow
import com.forkmesh.nativeapp.chat.ChatThreadSummary
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.chat.ReactionCatalog
import com.forkmesh.nativeapp.chat.ReactionChip
import com.forkmesh.nativeapp.chat.dayDividerLabel
import com.forkmesh.nativeapp.chat.messageBodySegments
import com.forkmesh.nativeapp.chat.messageTime
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmColors
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun ChatMessageRow(
    row: ChatRow,
    liveStatus: UserStatus?,
    queued: Boolean,
    onLongPress: (() -> Unit)?,
    onToggleReaction: ((String) -> Unit)?,
    onOpenThread: (() -> Unit)?,
) {
    val colors = Fm.colors
    val haptics = LocalHapticFeedback.current
    val message = row.message
    val grouped = row.continuesGroup
    Row(
        Modifier
            .fillMaxWidth()
            .combinedClickable(
                onClick = {},
                onLongClick = onLongPress?.let {
                    {
                        haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                        it()
                    }
                },
            )
            .padding(start = FmSpace.x3, end = FmSpace.x3, top = if (grouped) 1.dp else FmSpace.x3, bottom = 1.dp),
    ) {
        if (grouped) {
            Spacer(Modifier.width(28.dp + FmSpace.x2))
        } else {
            ChatSenderAvatar(message)
            Spacer(Modifier.width(FmSpace.x2))
        }
        Column(Modifier.weight(1f)) {
            if (!grouped) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        if (message.self) "${message.senderName} (you)" else message.senderName,
                        modifier = Modifier.weight(1f, fill = false),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        style = Fm.text.titleSmall.copy(color = FmColors.senderColor(message.senderName)),
                    )
                    if (row.visitor) Text(" visitor", style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.textTertiary))
                    if (message.ephemeral) {
                        Text(" ${AppsText.EPHEMERAL_NOTE}", maxLines = 1, style = Fm.text.bodySmall.copy(color = colors.textTertiary))
                    }
                    ChatSenderStatus(message, liveStatus)
                    Spacer(Modifier.width(FmSpace.x2))
                    Text(messageTime(message.timestamp), style = Fm.text.bodySmall.copy(color = colors.textTertiary))
                    val recipientsLabel = OfficeFrames.recipientsLabel(message.recipients)
                    val recipientsDescription = OfficeFrames.recipientsDescription(message.recipients)
                    if (recipientsLabel != null && recipientsDescription != null) {
                        Spacer(Modifier.width(FmSpace.x1))
                        Text(
                            recipientsLabel,
                            modifier = Modifier.semantics { contentDescription = recipientsDescription },
                            style = Fm.text.bodySmall.copy(color = colors.textTertiary),
                        )
                    }
                }
                Spacer(Modifier.height(2.dp))
            }
            if (row.text.isNotEmpty()) ChatMessageBody(row.text)
            if (message.blocks.isNotEmpty()) AppBlocksView(message)
            if (row.edited) Text("(edited)", style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.textTertiary))
            val attachment = row.attachment
            val viewOnce = row.viewOnce
            if (viewOnce != null) {
                ViewOnceBubble(viewOnce)
            } else if (attachment != null) {
                ChatAttachmentView(attachment)
            } else if (message.fileName.isNotEmpty()) {
                Row(Modifier.padding(top = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
                    FmIcon("attach_file", size = 14.dp, tint = colors.textTertiary)
                    Spacer(Modifier.width(FmSpace.x1))
                    Text(message.fileName, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(color = colors.accent))
                }
            }
            if (queued) {
                Text(
                    "Queued · goes to the next device that joins",
                    modifier = Modifier.padding(top = 2.dp),
                    style = Fm.text.bodySmall.copy(color = colors.warning),
                )
            }
            if (row.reactions.isNotEmpty() && onToggleReaction != null) ChatReactionChips(row.reactions, onToggleReaction)
            val thread = row.thread
            if (thread != null && onOpenThread != null) ChatThreadSummaryButton(thread, onOpenThread)
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ChatReactionChips(chips: List<ReactionChip>, onToggle: (String) -> Unit) {
    val colors = Fm.colors
    FlowRow(
        Modifier.padding(top = FmSpace.x1),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
    ) {
        for (chip in chips) {
            Row(
                Modifier
                    .clip(RoundedCornerShape(12.dp))
                    .background(if (chip.mine) colors.accentSubtle else colors.bgRaised)
                    .border(0.5.dp, if (chip.mine) colors.accent else colors.border, RoundedCornerShape(12.dp))
                    .clickable { onToggle(chip.value) }
                    .heightIn(min = 32.dp)
                    .padding(horizontal = FmSpace.x2, vertical = 2.dp)
                    .semantics { contentDescription = "${chip.label}, ${chip.count}. ${chip.names.joinToString(", ")}" },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(chip.emoji, style = Fm.text.bodyMedium)
                Spacer(Modifier.width(4.dp))
                Text("${chip.count}", style = Fm.text.labelSmall.copy(color = if (chip.mine) colors.accent else colors.textSecondary))
            }
        }
    }
}

@Composable
fun ChatDayDivider(timestampMs: Long) {
    val colors = Fm.colors
    val label = dayDividerLabel(timestampMs)
    Row(
        Modifier
            .fillMaxWidth()
            .padding(start = FmSpace.x3, end = FmSpace.x3, top = FmSpace.x4, bottom = FmSpace.x1)
            .semantics { contentDescription = label },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.weight(1f).height(1.dp).background(colors.border))
        Text(
            label,
            modifier = Modifier
                .padding(horizontal = FmSpace.x2)
                .border(1.dp, colors.border, RoundedCornerShape(50))
                .background(colors.bgBase, RoundedCornerShape(50))
                .padding(horizontal = FmSpace.x3, vertical = 3.dp),
            style = Fm.text.labelSmall.copy(color = colors.textSecondary),
            maxLines = 1,
        )
        Box(Modifier.weight(1f).height(1.dp).background(colors.border))
    }
}

@Composable
fun ChatThreadSummaryButton(summary: ChatThreadSummary, onOpen: () -> Unit) {
    val colors = Fm.colors
    Row(
        Modifier
            .padding(top = FmSpace.x1)
            .clip(RoundedCornerShape(FmRadius.md))
            .clickable(onClick = onOpen)
            .heightIn(min = 36.dp)
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1)
            .semantics { contentDescription = "${summary.label}, latest from ${summary.latestReplier}" },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (face in summary.faces) {
            FmAvatar(name = face.ifEmpty { "?" }, size = 18.dp, textStyle = Fm.text.labelSmall)
            Spacer(Modifier.width(2.dp))
        }
        Spacer(Modifier.width(FmSpace.x1))
        Text(summary.label, style = Fm.text.labelMedium.copy(color = colors.accent))
        Spacer(Modifier.width(FmSpace.x2))
        Text("Last reply ${messageTime(summary.latestAt)}", style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.textTertiary))
    }
}

@Composable
fun ChatAttachmentView(attachment: ChatAttachment) {
    val colors = Fm.colors
    val context = LocalContext.current
    val status = rememberFmStatus()
    val scope = rememberCoroutineScope()
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument(attachment.mime.ifEmpty { "application/octet-stream" })) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        status.clear()
        scope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching { context.contentResolver.openOutputStream(uri)?.use { it.write(attachment.data) } != null }.getOrDefault(false)
            }
            if (ok) status.show("Saved ${attachment.fileName}") else status.error("The file could not be saved.")
        }
    }
    Column {
        when (attachment.kind) {
            ChatAttachmentKind.IMAGE -> {
                val bitmap = remember(attachment.messageId) {
                    runCatching { BitmapFactory.decodeByteArray(attachment.data, 0, attachment.data.size)?.asImageBitmap() }.getOrNull()
                }
                var full by remember { mutableStateOf(false) }
                if (bitmap != null) {
                    Image(
                        bitmap = bitmap,
                        contentDescription = "Image ${attachment.fileName}",
                        contentScale = ContentScale.Fit,
                        modifier = Modifier
                            .padding(top = FmSpace.x1)
                            .widthIn(max = 260.dp)
                            .heightIn(max = 260.dp)
                            .clip(RoundedCornerShape(FmRadius.md))
                            .clickable { full = true },
                    )
                    if (full) {
                        FmSoundOnShow()
                        Dialog(onDismissRequest = { full = false }, properties = DialogProperties(usePlatformDefaultWidth = false)) {
                            Box(Modifier.fmTapSounds().fillMaxSize().background(colors.bgBase).clickable { full = false }, contentAlignment = Alignment.Center) {
                                Image(bitmap = bitmap, contentDescription = attachment.fileName, contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize())
                                Row(Modifier.align(Alignment.TopEnd).padding(FmSpace.x3)) {
                                    FmIconButton(icon = "download", label = "Save image", onPressed = { save.launch(attachment.fileName) })
                                    FmIconButton(icon = "close", label = "Close", onPressed = { full = false })
                                }
                            }
                        }
                    }
                } else {
                    AttachmentFileRow(attachment, "image_outlined") { save.launch(attachment.fileName) }
                }
            }
            ChatAttachmentKind.AUDIO -> AudioAttachmentRow(attachment) { save.launch(attachment.fileName) }
            ChatAttachmentKind.VIDEO -> VideoAttachmentView(attachment) { save.launch(attachment.fileName) }
            ChatAttachmentKind.FILE -> AttachmentFileRow(attachment, "attach_file") { save.launch(attachment.fileName) }
        }
        FmInlineStatus(status, Modifier.widthIn(max = 260.dp).padding(top = FmSpace.x1))
    }
}

@Composable
private fun AttachmentFileRow(attachment: ChatAttachment, icon: String, onSave: () -> Unit) {
    val colors = Fm.colors
    Row(
        Modifier
            .padding(top = FmSpace.x1)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(colors.bgRaised)
            .clickable(onClick = onSave)
            .heightIn(min = 44.dp)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 16.dp, tint = colors.textTertiary)
        Spacer(Modifier.width(FmSpace.x2))
        Column(Modifier.weight(1f, fill = false)) {
            Text(attachment.fileName, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(color = colors.accent))
            Text("${attachment.data.size / 1024 + 1} KB · tap to save", style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.textTertiary))
        }
    }
}

private fun chatMediaFile(context: android.content.Context, attachment: ChatAttachment): File? = runCatching {
    val dir = File(context.cacheDir, "chat-media").apply { mkdirs() }
    val file = File(dir, "${attachment.messageId.hashCode()}-${attachment.fileName.replace('/', '_')}")
    if (!file.exists()) file.writeBytes(attachment.data)
    file
}.getOrNull()

@Composable
private fun VideoAttachmentView(attachment: ChatAttachment, onSave: () -> Unit) {
    val colors = Fm.colors
    val context = LocalContext.current
    var file by remember(attachment.messageId) { mutableStateOf<File?>(null) }
    var failed by remember(attachment.messageId) { mutableStateOf(false) }
    val playing = file
    if (playing == null) {
        Row(
            Modifier
                .padding(top = FmSpace.x1)
                .clip(RoundedCornerShape(FmRadius.md))
                .background(colors.bgRaised)
                .padding(horizontal = FmSpace.x1),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIconButton(
                icon = "play_arrow",
                label = "Play ${attachment.fileName}",
                onPressed = {
                    failed = false
                    file = chatMediaFile(context, attachment).also { if (it == null) failed = true }
                },
            )
            Column(Modifier.weight(1f, fill = false)) {
                Text(attachment.fileName, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(color = colors.accent))
                if (failed) Text("This video could not be played.", style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.danger))
            }
            FmIconButton(icon = "download", label = "Save ${attachment.fileName}", onPressed = onSave)
        }
        return
    }
    Box(
        Modifier
            .padding(top = FmSpace.x1)
            .width(260.dp)
            .height(180.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(Color.Black),
    ) {
        AndroidView(
            factory = { ctx ->
                android.widget.VideoView(ctx).apply {
                    val controller = android.widget.MediaController(ctx)
                    controller.setAnchorView(this)
                    setMediaController(controller)
                    setOnErrorListener { _, _, _ ->
                        failed = true
                        file = null
                        true
                    }
                    setVideoPath(playing.absolutePath)
                    setOnPreparedListener { it.isLooping = false; start() }
                }
            },
            onRelease = { it.stopPlayback() },
            modifier = Modifier.fillMaxSize().semantics { contentDescription = "Video ${attachment.fileName}" },
        )
        Row(Modifier.align(Alignment.TopEnd)) {
            FmIconButton(icon = "download", label = "Save ${attachment.fileName}", onPressed = onSave)
            FmIconButton(icon = "close", label = "Close video", onPressed = { file = null })
        }
    }
}

@Composable
private fun AudioAttachmentRow(attachment: ChatAttachment, onSave: () -> Unit) {
    val colors = Fm.colors
    val context = LocalContext.current
    var player by remember { mutableStateOf<MediaPlayer?>(null) }
    DisposableEffect(attachment.messageId) {
        onDispose {
            player?.release()
            player = null
        }
    }
    Row(
        Modifier
            .padding(top = FmSpace.x1)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(colors.bgRaised)
            .padding(horizontal = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIconButton(
            icon = if (player == null) "play_arrow" else "stop",
            label = if (player == null) "Play ${attachment.fileName}" else "Stop",
            onPressed = {
                val current = player
                if (current != null) {
                    current.release()
                    player = null
                } else {
                    val dir = File(context.cacheDir, "chat-media").apply { mkdirs() }
                    val file = File(dir, "${attachment.messageId.hashCode()}-${attachment.fileName.replace('/', '_')}")
                    runCatching {
                        if (!file.exists()) file.writeBytes(attachment.data)
                        val next = MediaPlayer()
                        next.setDataSource(file.absolutePath)
                        next.setOnCompletionListener {
                            it.release()
                            player = null
                        }
                        next.prepare()
                        next.start()
                        player = next
                    }
                }
            },
        )
        Text(attachment.fileName, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false), style = Fm.text.bodyMedium)
        FmIconButton(icon = "download", label = "Save ${attachment.fileName}", onPressed = onSave)
    }
}

@Composable
fun ChatMessageBody(text: String) {
    val colors = Fm.colors
    val parts = messageBodySegments(text)
    if (parts.size == 1 && !parts.first().code) {
        ChatLinkText(parts.first().text, style = Fm.text.bodyLarge)
        return
    }
    Column {
        for (part in parts) {
            if (part.code) {
                Box(
                    Modifier
                        .padding(bottom = FmSpace.x1)
                        .fillMaxWidth()
                        .background(colors.bgOverlay, RoundedCornerShape(FmRadius.md))
                        .horizontalScroll(rememberScrollState())
                        .padding(FmSpace.x2),
                ) {
                    Text(
                        part.text,
                        softWrap = false,
                        style = Fm.text.bodyMedium.copy(fontFamily = FontFamily.Monospace, lineHeight = Fm.text.bodyMedium.fontSize * FmType.leadingSnug),
                    )
                }
            } else {
                ChatLinkText(part.text, style = Fm.text.bodyLarge, modifier = Modifier.padding(bottom = FmSpace.x1))
            }
        }
    }
}

@Composable
fun ChatSenderStatus(message: ChatMessage, liveStatus: UserStatus?) {
    val status by produceState(liveStatus, message.senderName, liveStatus) {
        if (liveStatus != null) {
            value = liveStatus
            return@produceState
        }
        val api = Services.get<ApiService>() ?: return@produceState
        if (message.senderName.isBlank() || message.senderId == "forkbot" || message.senderId.startsWith("app:")) return@produceState
        value = withContext(Dispatchers.IO) { runCatching { api.accountUserStatus(message.senderName) }.getOrNull() }
    }
    val emoji = status?.displayEmoji(System.currentTimeMillis()).orEmpty()
    if (emoji.isNotEmpty()) Text(" $emoji", style = Fm.text.bodySmall)
}

@Composable
fun ChatSenderAvatar(message: ChatMessage) {
    val ownAvatar = Services.get<AuthService>()?.session?.collectAsState()?.value?.avatarPng.orEmpty()
    val avatar by produceState(if (message.self) ownAvatar else "", message.senderName, message.self, ownAvatar) {
        if (message.self && ownAvatar.isNotEmpty()) {
            value = ownAvatar
            return@produceState
        }
        val api = Services.get<ApiService>() ?: return@produceState
        if (message.senderName.isBlank() || message.senderId == "forkbot") return@produceState
        value = withContext(Dispatchers.IO) { runCatching { api.accountAvatarPng(message.senderName) }.getOrDefault("") }
    }
    FmAvatar(name = message.senderName.ifEmpty { "?" }, size = 28.dp, avatarPng = avatar, textStyle = Fm.text.bodySmall)
}

@Composable
fun ChatQuickReactions(onPick: (String) -> Unit, onMore: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (value in ReactionCatalog.quick) {
            Box(
                Modifier
                    .size(44.dp)
                    .clip(RoundedCornerShape(22.dp))
                    .background(Fm.colors.bgRaised)
                    .clickable { onPick(value) }
                    .semantics { contentDescription = ReactionCatalog.label(value) },
                contentAlignment = Alignment.Center,
            ) {
                Text(ReactionCatalog.emoji(value), style = Fm.text.titleMedium)
            }
        }
        FmIconButton(icon = "add_reaction_outlined", label = "More reactions", onPressed = onMore)
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ChatEmojiGrid(emojis: List<String>, labels: List<String>? = null, onPick: (Int) -> Unit) {
    FlowRow(
        Modifier.fillMaxWidth().padding(FmSpace.x3),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
    ) {
        emojis.forEachIndexed { index, emoji ->
            Box(
                Modifier
                    .size(44.dp)
                    .clip(RoundedCornerShape(FmRadius.md))
                    .clickable { onPick(index) }
                    .semantics { contentDescription = labels?.getOrNull(index) ?: emoji },
                contentAlignment = Alignment.Center,
            ) {
                Text(emoji, style = Fm.text.titleMedium)
            }
        }
    }
}
