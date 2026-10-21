package com.forkmesh.nativeapp.feature.chat

import android.graphics.BitmapFactory
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.zIndex
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.addOutline
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.LastBaseline
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.forkmesh.nativeapp.chat.ChatAttachment
import com.forkmesh.nativeapp.chat.ChatAttachmentKind
import com.forkmesh.nativeapp.chat.ChatBubble
import com.forkmesh.nativeapp.chat.ChatConversationLayout
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.chat.ReactionCatalog
import com.forkmesh.nativeapp.chat.ReactionChip
import com.forkmesh.nativeapp.chat.messageTime
import com.forkmesh.nativeapp.ui.kit.LocalFmBackdrop
import com.forkmesh.nativeapp.ui.kit.LocalFmSolidGlass
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.delay
import kotlin.math.max
import kotlin.math.min

internal class ChatBubbleColors(val dark: Boolean) {
    val sentFill: Color get() = BLUE
    val sentText: Color get() = Color.White
    val receivedFill: Color get() = if (dark) Color(0xFF26262A) else Color(0xFFE9E9EB)
    val receivedText: Color get() = if (dark) Color.White else Color.Black
    val meta: Color get() = if (dark) Color(0xFF8E8E93) else Color(0xFF8A8A8E)
    val canvas: Color get() = if (dark) Color.Black else Color.White

    companion object {
        val BLUE = Color(0xFF0A84FF)
    }
}

internal data class ChatBubbleLife(val expiresAt: Long? = null, val lifetimeMs: Long = 0, val onServer: Boolean = false)

internal class ChatBubbleShape(
    private val fromSelf: Boolean,
    private val joinsAbove: Boolean = false,
    private val joinsBelow: Boolean = false,
) : Shape {
    override fun createOutline(size: Size, layoutDirection: LayoutDirection, density: Density): Outline {
        val inset = with(density) { INSET.toPx() }
        val width = size.width
        val height = size.height
        val radius = minOf(with(density) { 18.dp.toPx() }, height / 2f, (width - inset) / 2f).coerceAtLeast(0f)
        val joined = min(with(density) { JOINED.toPx() }, radius)
        val top = if (joinsAbove) joined else radius
        val bottom = if (joinsBelow) joined else radius
        val k = 0.45f
        val path = Path().apply {
            moveTo(inset + bottom, height)
            lineTo(width - radius, height)
            cubicTo(width - radius * k, height, width, height - radius * k, width, height - radius)
            lineTo(width, radius)
            cubicTo(width, radius * k, width - radius * k, 0f, width - radius, 0f)
            lineTo(inset + top, 0f)
            cubicTo(inset + top * k, 0f, inset, top * k, inset, top)
            lineTo(inset, height - bottom)
            cubicTo(inset, height - bottom * k, inset + bottom * k, height, inset + bottom, height)
            close()
        }
        if (fromSelf) {
            val mirror = androidx.compose.ui.graphics.Matrix().apply {
                translate(width, 0f)
                scale(-1f, 1f)
            }
            path.transform(mirror)
        }
        return Outline.Generic(path)
    }

    companion object {
        val INSET = 5.dp
        val JOINED = 5.dp
    }
}

internal class ChatPhotoShape(private val mine: Boolean, private val joinsAbove: Boolean, private val joinsBelow: Boolean) : Shape {
    override fun createOutline(size: Size, layoutDirection: LayoutDirection, density: Density): Outline {
        val full = with(density) { 18.dp.toPx() }
        val near = with(density) { ChatBubbleShape.JOINED.toPx() }
        val top = if (joinsAbove) near else full
        val bottom = if (joinsBelow) near else full
        val rect = androidx.compose.ui.geometry.RoundRect(
            left = 0f,
            top = 0f,
            right = size.width,
            bottom = size.height,
            topLeftCornerRadius = CornerRadius(if (mine) full else top),
            topRightCornerRadius = CornerRadius(if (mine) top else full),
            bottomRightCornerRadius = CornerRadius(if (mine) bottom else full),
            bottomLeftCornerRadius = CornerRadius(if (mine) full else bottom),
        )
        return Outline.Rounded(rect)
    }
}

@Composable
internal fun Modifier.chatBubbleGlass(shape: Shape, fill: Color, mine: Boolean, colors: ChatBubbleColors): Modifier {
    val backdrop = LocalFmBackdrop.current
    if (backdrop != null && !LocalFmSolidGlass.current) {
        return this.fmGlass(shape, tint = fill.copy(alpha = if (mine) 0.88f else 0.5f))
    }
    val dark = colors.dark
    val rim = with(LocalDensity.current) { 0.8.dp.toPx() }
    val shadowColor = Color.Black.copy(alpha = if (dark) 0.32f else 0.1f)
    return this
        .shadow(4.dp, shape, clip = false, ambientColor = shadowColor, spotColor = shadowColor)
        .drawBehind {
            val outline = shape.createOutline(size, layoutDirection, this)
            val path = Path().apply { addOutline(outline) }
            if (!mine) drawPath(path, colors.canvas.copy(alpha = 0.7f))
            drawPath(
                path,
                Brush.verticalGradient(listOf(fill.copy(alpha = if (mine) 0.78f else 0.55f), fill.copy(alpha = if (mine) 0.98f else 0.8f))),
            )
            drawPath(
                path,
                Brush.verticalGradient(0f to Color.White.copy(alpha = if (dark) 0.14f else 0.32f), 0.5f to Color.White.copy(alpha = 0f)),
            )
            drawPath(
                path,
                Brush.verticalGradient(listOf(Color.White.copy(alpha = if (dark) 0.35f else 0.7f), Color.White.copy(alpha = 0.04f))),
                style = Stroke(width = rim),
            )
        }
}

@Composable
internal fun ChatExpiryRing(expiresAt: Long, lifetimeMs: Long, tint: Color) {
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(expiresAt) {
        while (true) {
            now = System.currentTimeMillis()
            delay(60_000)
        }
    }
    val leftMs = max(0L, expiresAt - now)
    val share = if (lifetimeMs > 0) min(1f, leftMs.toFloat() / lifetimeMs.toFloat()) else 0f
    val elapsed = 1f - share
    val label = "Disappears in ${ChatConversationLayout.expiryRemaining(leftMs / 1000.0)}"
    Canvas(Modifier.size(11.dp).semantics { contentDescription = label }) {
        val inset = 1.6.dp.toPx()
        if (elapsed < 1f) {
            drawArc(
                color = tint.copy(alpha = 0.3f),
                startAngle = -90f + elapsed * 360f,
                sweepAngle = share * 360f,
                useCenter = true,
                topLeft = Offset(inset, inset),
                size = Size(size.width - inset * 2, size.height - inset * 2),
            )
        }
        val stroke = 1.2.dp.toPx()
        drawCircle(tint, radius = size.minDimension / 2f - stroke / 2f, style = Stroke(stroke))
        rotate(elapsed * 360f) {
            val handWidth = 1.3.dp.toPx()
            val handHeight = 3.6.dp.toPx()
            val top = center.y - 1.6.dp.toPx() - handHeight / 2f
            drawRoundRect(
                tint,
                topLeft = Offset(center.x - handWidth / 2f, top),
                size = Size(handWidth, handHeight),
                cornerRadius = CornerRadius(handWidth / 2f),
            )
        }
        drawCircle(tint, radius = 1.dp.toPx())
    }
}

@Composable
internal fun ChatBubbleMeta(sentAt: Long, life: ChatBubbleLife, tint: Color) {
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
        if (life.onServer) {
            Box(Modifier.size(4.dp).background(tint, CircleShape).semantics { contentDescription = "Still on the server" })
        }
        Text(
            messageTime(sentAt),
            maxLines = 1,
            softWrap = false,
            style = TextStyle(fontSize = 11.sp, fontWeight = FontWeight.Medium, fontFeatureSettings = "tnum", color = tint),
        )
        val expiresAt = life.expiresAt
        if (expiresAt != null) ChatExpiryRing(expiresAt, life.lifetimeMs, tint)
    }
}

private enum class ReceiptPlace { TEXT, PHOTO, SHELL, BELOW }

internal class ChatBubbleActions(
    val onLongPress: (() -> Unit)? = null,
    val onDoubleTap: (() -> Unit)? = null,
    val onTapback: (() -> Unit)? = null,
    val onThread: (() -> Unit)? = null,
    val onSticker: ((String) -> Unit)? = null,
)

@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun ChatBubbleRow(
    bubble: ChatBubble,
    group: Boolean,
    colors: ChatBubbleColors,
    life: ChatBubbleLife,
    actions: ChatBubbleActions,
    viewOnce: (@Composable () -> Unit)? = null,
) {
    val row = bubble.row
    val message = row.message
    val mine = message.self
    val jumbo = bubble.jumbo
    val attachment = row.attachment
    val photo = remember(attachment?.messageId, attachment?.data?.size) {
        if (attachment?.kind == ChatAttachmentKind.IMAGE) {
            runCatching { BitmapFactory.decodeByteArray(attachment.data, 0, attachment.data.size)?.asImageBitmap() }.getOrNull()
        } else {
            null
        }
    }
    val place = when {
        viewOnce != null -> ReceiptPlace.BELOW
        row.text.isNotEmpty() -> if (jumbo) ReceiptPlace.BELOW else ReceiptPlace.TEXT
        attachment == null -> if (message.fileName.isEmpty()) ReceiptPlace.BELOW else ReceiptPlace.SHELL
        attachment.kind == ChatAttachmentKind.IMAGE -> if (photo == null) ReceiptPlace.BELOW else ReceiptPlace.PHOTO
        else -> ReceiptPlace.SHELL
    }
    val sideIndent = if (group && !mine) 46.dp else 12.dp
    Column(
        Modifier
            .fillMaxWidth()
            .padding(top = if (bubble.position.startsRun) 8.dp else 2.dp)
            .padding(horizontal = 10.dp),
        horizontalAlignment = if (mine) Alignment.End else Alignment.Start,
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        if (bubble.showsSender) {
            Text(
                message.senderName,
                modifier = Modifier.padding(start = 46.dp, top = 6.dp),
                style = TextStyle(fontSize = 12.sp, color = colors.meta),
            )
        }
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = if (mine) Arrangement.End else Arrangement.Start,
            verticalAlignment = Alignment.Bottom,
        ) {
            if (mine) {
                Spacer(Modifier.widthIn(min = 56.dp))
            } else if (group) {
                if (bubble.position.hasTail) ChatSenderAvatar(message) else Spacer(Modifier.size(28.dp, 1.dp))
                Spacer(Modifier.size(6.dp, 1.dp))
            }
            Box(
                Modifier
                    .weight(1f, fill = false)
                    .padding(top = if (row.reactions.isEmpty()) 0.dp else 14.dp),
            ) {
                Box(
                    Modifier.combinedClickable(
                        onClick = {},
                        onDoubleClick = actions.onDoubleTap,
                        onLongClick = actions.onLongPress,
                    ),
                ) {
                    BubbleContent(bubble, colors, life, place, photo, viewOnce)
                }
                if (row.stickers.isNotEmpty()) {
                    ChatStickers(
                        row.stickers,
                        onToggle = actions.onSticker,
                        modifier = Modifier
                            .align(if (mine) Alignment.BottomStart else Alignment.BottomEnd)
                            .offset(x = if (mine) (-10).dp else 10.dp, y = 10.dp)
                            .zIndex(1f),
                    )
                }
                if (row.reactions.isNotEmpty()) {
                    ChatTapbackBadge(
                        row.reactions,
                        colors,
                        onTap = actions.onTapback,
                        modifier = Modifier
                            .align(if (mine) Alignment.TopStart else Alignment.TopEnd)
                            .offset(x = if (mine) (-14).dp else 14.dp, y = (-16).dp),
                    )
                }
            }
            if (!mine) Spacer(Modifier.widthIn(min = 56.dp))
        }
        if (row.edited) {
            Text(
                "Edited",
                modifier = Modifier.padding(start = if (mine) 0.dp else sideIndent, end = if (mine) 12.dp else 0.dp),
                style = TextStyle(fontSize = 11.sp, fontWeight = FontWeight.Medium, color = colors.meta),
            )
        }
        val thread = row.thread
        val onThread = actions.onThread
        if (thread != null && onThread != null) {
            Text(
                thread.label,
                modifier = Modifier
                    .padding(start = if (mine) 0.dp else sideIndent, end = if (mine) 12.dp else 0.dp)
                    .clickable(onClick = onThread)
                    .semantics { role = Role.Button },
                style = TextStyle(fontSize = 12.sp, fontWeight = FontWeight.SemiBold, color = ChatBubbleColors.BLUE),
            )
        }
        if (place == ReceiptPlace.BELOW) {
            Box(Modifier.padding(end = 12.dp)) {
                ReceiptBadge(bubble, life, tint = colors.meta, fill = colors.canvas, accent = ChatBubbleColors.BLUE)
            }
        }
    }
}

@Composable
private fun ReceiptBadge(bubble: ChatBubble, life: ChatBubbleLife, tint: Color, fill: Color, accent: Color) {
    val message = bubble.row.message
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
        ChatBubbleMeta(message.timestamp, life, tint)
        val label = OfficeFrames.recipientsLabel(message.recipients)
        val description = OfficeFrames.recipientsDescription(message.recipients)
        if (label != null && description != null) {
            Text(
                label,
                modifier = Modifier.semantics { contentDescription = description },
                style = TextStyle(fontSize = 11.sp, fontWeight = FontWeight.Medium, color = tint),
            )
        }
        val receipt = bubble.receipt
        if (receipt != null) ChatReceiptMark(receipt, mutedColor = tint, accentColor = accent, backgroundColor = fill)
    }
}

@Composable
private fun InBubbleReceipt(bubble: ChatBubble, life: ChatBubbleLife, colors: ChatBubbleColors) {
    val mine = bubble.row.message.self
    Box(Modifier.padding(bottom = 1.dp)) {
        ReceiptBadge(
            bubble,
            life,
            tint = if (mine) colors.sentText.copy(alpha = 0.78f) else colors.meta.copy(alpha = 0.9f),
            fill = if (mine) colors.sentFill else colors.receivedFill,
            accent = if (mine) colors.sentText else ChatBubbleColors.BLUE,
        )
    }
}

@Composable
private fun BubbleContent(
    bubble: ChatBubble,
    colors: ChatBubbleColors,
    life: ChatBubbleLife,
    place: ReceiptPlace,
    photo: androidx.compose.ui.graphics.ImageBitmap?,
    viewOnce: (@Composable () -> Unit)?,
) {
    val row = bubble.row
    val message = row.message
    val mine = message.self
    val attachment = row.attachment
    val textColor = if (mine) colors.sentText else colors.receivedText
    Column(horizontalAlignment = if (mine) Alignment.End else Alignment.Start, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        if (viewOnce != null) {
            viewOnce()
        } else if (attachment != null && attachment.kind == ChatAttachmentKind.IMAGE && photo != null) {
            BubblePhoto(bubble, attachment, photo, place == ReceiptPlace.PHOTO, life)
        } else if (attachment != null || message.fileName.isNotEmpty()) {
            BubbleShell(bubble, colors) {
                WithReceipt(place == ReceiptPlace.SHELL, bubble, life, colors) {
                    if (attachment != null) {
                        ChatAttachmentView(attachment)
                    } else {
                        Text(message.fileName, style = TextStyle(fontSize = 15.sp, color = textColor))
                    }
                }
            }
        }
        if (row.text.isNotEmpty()) {
            if (bubble.jumbo) {
                Text(
                    row.text,
                    modifier = Modifier.padding(horizontal = 6.dp).semantics { contentDescription = row.text },
                    style = TextStyle(fontSize = 52.sp),
                )
            } else {
                BubbleShell(bubble, colors) {
                    BubbleTextWithMeta(row.text, textColor, inline = place == ReceiptPlace.TEXT) {
                        InBubbleReceipt(bubble, life, colors)
                    }
                }
            }
        }
        if (message.blocks.isNotEmpty()) {
            BubbleShell(bubble, colors) { AppBlocksView(message) }
        }
    }
}

@Composable
private fun BubbleShell(bubble: ChatBubble, colors: ChatBubbleColors, content: @Composable () -> Unit) {
    val mine = bubble.row.message.self
    val shape = remember(mine, bubble.joinsAbove, bubble.joinsBelow) { ChatBubbleShape(mine, bubble.joinsAbove, bubble.joinsBelow) }
    Box(
        Modifier
            .chatBubbleGlass(shape, if (mine) colors.sentFill else colors.receivedFill, mine, colors)
            .padding(top = 8.dp, bottom = 8.dp, start = if (mine) 13.dp else 18.dp, end = if (mine) 18.dp else 13.dp),
    ) {
        content()
    }
}

@Composable
private fun WithReceipt(
    shown: Boolean,
    bubble: ChatBubble,
    life: ChatBubbleLife,
    colors: ChatBubbleColors,
    content: @Composable () -> Unit,
) {
    if (!shown) {
        content()
        return
    }
    Layout(content = {
        content()
        InBubbleReceipt(bubble, life, colors)
    }) { measurables, constraints ->
        val loose = constraints.copy(minWidth = 0, minHeight = 0)
        val inner = measurables[0].measure(loose)
        val meta = measurables[1].measure(loose)
        val gap = 8.dp.roundToPx()
        if (inner.width + gap + meta.width <= constraints.maxWidth) {
            val height = max(inner.height, meta.height)
            layout(inner.width + gap + meta.width, height) {
                inner.place(0, height - inner.height)
                meta.place(inner.width + gap, height - meta.height)
            }
        } else {
            val width = max(inner.width, meta.width)
            val spacing = 2.dp.roundToPx()
            layout(width, inner.height + spacing + meta.height) {
                inner.place(0, 0)
                meta.place(width - meta.width, inner.height + spacing)
            }
        }
    }
}

@Composable
private fun BubbleTextWithMeta(text: String, color: Color, inline: Boolean, meta: @Composable () -> Unit) {
    val style = TextStyle(fontSize = 17.sp, lineHeight = 22.sp, color = color)
    if (!inline) {
        ChatLinkText(text, style = style, linkColor = color)
        return
    }
    val layoutRef = remember { arrayOfNulls<TextLayoutResult>(1) }
    Layout(content = {
        ChatLinkText(text, style = style, linkColor = color, onTextLayout = { layoutRef[0] = it })
        meta()
    }) { measurables, constraints ->
        val loose = constraints.copy(minWidth = 0, minHeight = 0)
        val textPlaceable = measurables[0].measure(loose)
        val metaPlaceable = measurables[1].measure(loose)
        val result = layoutRef[0]
        val gap = 8.dp.roundToPx()
        val lastLine = (result?.lineCount ?: 1) - 1
        val lastRight = result?.getLineRight(lastLine)?.let { kotlin.math.ceil(it).toInt() } ?: textPlaceable.width
        val metaBaseline = metaPlaceable[LastBaseline]
        if (result != null && lastRight + gap + metaPlaceable.width <= constraints.maxWidth) {
            val width = max(textPlaceable.width, lastRight + gap + metaPlaceable.width)
            val textBaseline = result.getLineBaseline(lastLine).toInt()
            val metaTop = if (metaBaseline > 0) textBaseline - metaBaseline else result.getLineBottom(lastLine).toInt() - metaPlaceable.height
            val shift = if (metaTop < 0) -metaTop else 0
            val height = max(textPlaceable.height + shift, metaTop + shift + metaPlaceable.height)
            layout(width, height) {
                textPlaceable.place(0, shift)
                metaPlaceable.place(width - metaPlaceable.width, metaTop + shift)
            }
        } else {
            val width = max(textPlaceable.width, metaPlaceable.width)
            val spacing = 2.dp.roundToPx()
            layout(width, textPlaceable.height + spacing + metaPlaceable.height) {
                textPlaceable.place(0, 0)
                metaPlaceable.place(width - metaPlaceable.width, textPlaceable.height + spacing)
            }
        }
    }
}

@Composable
private fun BubblePhoto(
    bubble: ChatBubble,
    attachment: ChatAttachment,
    photo: androidx.compose.ui.graphics.ImageBitmap,
    showsReceipt: Boolean,
    life: ChatBubbleLife,
) {
    val mine = bubble.row.message.self
    val shape = remember(mine, bubble.joinsAbove, bubble.joinsBelow) { ChatPhotoShape(mine, bubble.joinsAbove, bubble.joinsBelow) }
    val (width, height) = photoSize(photo.width, photo.height)
    var full by remember { mutableStateOf(false) }
    Box(
        Modifier
            .size(width, height)
            .clip(shape)
            .border(0.5.dp, Color.Gray.copy(alpha = 0.16f), shape)
            .clickable { full = true }
            .semantics {
                role = Role.Button
                contentDescription = "Photo"
            },
    ) {
        Image(photo, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
        if (showsReceipt) {
            Box(
                Modifier
                    .align(Alignment.BottomEnd)
                    .padding(8.dp)
                    .background(Color.Black.copy(alpha = 0.45f), RoundedCornerShape(50))
                    .padding(horizontal = 6.dp, vertical = 3.dp),
            ) {
                ReceiptBadge(bubble, life, tint = Color.White, fill = Color(0xFF262626), accent = Color.White)
            }
        }
    }
    if (full) {
        Dialog(onDismissRequest = { full = false }, properties = DialogProperties(usePlatformDefaultWidth = false)) {
            Box(Modifier.fillMaxSize().background(Color.Black).clickable { full = false }, contentAlignment = Alignment.Center) {
                Image(photo, contentDescription = attachment.fileName, contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize())
            }
        }
    }
}

private fun photoSize(pixelWidth: Int, pixelHeight: Int): Pair<Dp, Dp> {
    val maxSide = 240f
    val width = max(1, pixelWidth).toFloat()
    val height = max(1, pixelHeight).toFloat()
    val scale = min(maxSide / width, maxSide / height)
    return max(120f, width * scale).dp to max(90f, height * scale).dp
}

@Composable
internal fun ChatTapbackBadge(chips: List<ReactionChip>, colors: ChatBubbleColors, onTap: (() -> Unit)?, modifier: Modifier = Modifier) {
    val mine = chips.any { it.mine }
    val total = chips.sumOf { it.count }
    val label = chips.joinToString("; ") { "${it.label} ${it.count}: ${it.names.joinToString(", ")}" }
    Row(
        modifier
            .heightIn(min = 28.dp)
            .widthIn(min = 30.dp)
            .border(2.dp, colors.canvas, RoundedCornerShape(50))
            .background(if (mine) ChatBubbleColors.BLUE else colors.receivedFill, RoundedCornerShape(50))
            .then(if (onTap != null) Modifier.clickable(onClick = onTap) else Modifier)
            .padding(horizontal = 7.dp)
            .semantics { contentDescription = label },
        horizontalArrangement = Arrangement.spacedBy(1.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (chip in chips.take(3)) Text(chip.emoji, style = TextStyle(fontSize = 14.sp))
        if (total > 1) {
            Text(
                "$total",
                modifier = Modifier.padding(start = 2.dp),
                style = TextStyle(fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = if (mine) Color.White else colors.receivedText),
            )
        }
    }
}

@Composable
internal fun ChatTapbackOverlay(chips: List<ReactionChip>, onPick: (String) -> Unit, onDismiss: () -> Unit) {
    val chosen = chips.filter { it.mine }.map { it.value }.toSet()
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.25f))
            .clickable(indication = null, interactionSource = null, onClick = onDismiss),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            Row(
                Modifier
                    .fmGlass(RoundedCornerShape(50))
                    .padding(horizontal = 6.dp, vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                for (value in ReactionCatalog.quick) {
                    Box(
                        Modifier
                            .size(42.dp)
                            .background(if (value in chosen) ChatBubbleColors.BLUE.copy(alpha = 0.85f) else Color.Transparent, CircleShape)
                            .clickable { onPick(value) }
                            .semantics {
                                role = Role.Button
                                contentDescription = ReactionCatalog.label(value)
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(ReactionCatalog.emoji(value), style = TextStyle(fontSize = 26.sp))
                    }
                }
            }
            if (chips.isNotEmpty()) {
                Column(
                    Modifier
                        .background(Color.Black.copy(alpha = 0.55f), RoundedCornerShape(14.dp))
                        .padding(FmSpace.x3),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    for (chip in chips) {
                        Text("${chip.emoji}  ${chip.names.joinToString(", ")}", maxLines = 2, style = TextStyle(fontSize = 14.sp, color = Color.White))
                    }
                }
            }
        }
    }
}

@Composable
internal fun ChatBubbleSystemLine(text: String, colors: ChatBubbleColors, bold: Boolean = false) {
    Text(
        text,
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = FmSpace.x6)
            .padding(top = if (bold) 14.dp else 2.dp, bottom = 2.dp),
        textAlign = TextAlign.Center,
        style = TextStyle(fontSize = 11.sp, fontWeight = if (bold) FontWeight.SemiBold else FontWeight.Normal, color = colors.meta),
    )
}

@Composable
internal fun ChatTimeLine(at: Long, colors: ChatBubbleColors) {
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(at) {
        while (true) {
            delay(60_000)
            now = System.currentTimeMillis()
        }
    }
    ChatBubbleSystemLine(ChatConversationLayout.timeLabel(at, now), colors, bold = true)
}

@Composable
internal fun ChatTypingBubble(label: String, colors: ChatBubbleColors) {
    var step by remember { mutableLongStateOf(0L) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(350)
            step++
        }
    }
    val shape = remember { ChatBubbleShape(fromSelf = false) }
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 10.dp)
            .padding(top = 8.dp)
            .semantics { contentDescription = label },
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalAlignment = Alignment.Bottom,
    ) {
        Row(
            Modifier
                .chatBubbleGlass(shape, colors.receivedFill, mine = false, colors = colors)
                .padding(top = 12.dp, bottom = 12.dp, start = 18.dp, end = 14.dp),
            horizontalArrangement = Arrangement.spacedBy(5.dp),
        ) {
            for (index in 0 until 3) {
                Box(Modifier.size(8.dp).background(colors.meta.copy(alpha = if (index.toLong() == step % 3) 1f else 0.45f), CircleShape))
            }
        }
        Text(label, maxLines = 1, modifier = Modifier.padding(bottom = 6.dp), style = TextStyle(fontSize = 12.sp, color = colors.meta))
    }
}
