package com.forkmesh.nativeapp.feature.djstudio

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.djstudio.DjDeckSide
import com.forkmesh.nativeapp.djstudio.DjJamMember
import com.forkmesh.nativeapp.djstudio.DjMotion
import com.forkmesh.nativeapp.djstudio.DjPersonStatus
import com.forkmesh.nativeapp.djstudio.DjQueueItem
import com.forkmesh.nativeapp.djstudio.DjReaction
import com.forkmesh.nativeapp.djstudio.DjReactionStyle
import com.forkmesh.nativeapp.ui.kit.FmActionMenu
import com.forkmesh.nativeapp.ui.kit.FmActionMenuButton
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.roundToInt
import kotlin.math.sin

internal fun DjStudioModel.dragQueue(item: DjQueueItem, point: Offset) {
    if (draggedId == null) {
        DjHaptics.tick(context)
        reacting = false
        draggedId = item.id
    }
    dragPoint = point
    val target = dropTarget(point)
    if (target != dropSide) {
        dropSide = target
        if (target != null) DjHaptics.tick(context)
    }
}

internal fun DjStudioModel.dropQueue(item: DjQueueItem) {
    val point = dragPoint
    val side = point?.let { dropTarget(it) }
    cancelQueueDrag()
    if (side != null) {
        DjHaptics.tick(context)
        playFromQueue(item, side)
    }
}

internal fun DjStudioModel.cancelQueueDrag() {
    draggedId = null
    dragPoint = null
    dropSide = null
}

private fun DjStudioModel.dropTarget(point: Offset): DjDeckSide? = DjDeckSide.entries.mapNotNull { side ->
    val frame = deckFrames[side] ?: return@mapNotNull null
    val slack = 28f * context.resources.displayMetrics.density
    if (point.x < frame.left - slack || point.x > frame.right + slack || point.y < frame.top - slack || point.y > frame.bottom + slack) return@mapNotNull null
    side to hypot(frame.center.x - point.x, frame.center.y - point.y)
}.minByOrNull { it.second }?.first

@Composable
internal fun DjQueueView(model: DjStudioModel, u: Float, vertical: Boolean, modifier: Modifier) {
    val items = model.shownQueue
    val content: @Composable () -> Unit = {
        if (vertical) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy((6 * u).dp)) {
                DjJamButton(model, u)
                Column(Modifier.weight(1f)) {
                    DjText("QUEUE", 10 * u, DjTheme.ink.copy(alpha = 0.85f), FontWeight.Bold)
                    model.jam?.takeIf { it.joined }?.let { DjText("${it.listeners.size} in jam", 9 * u, DjTheme.active) }
                }
                DjRoundButton((26 * u).dp, "add", "Add a video to the queue") { model.openLoader(com.forkmesh.nativeapp.djstudio.DjLoadTarget.QUEUE) }
            }
            if (model.inJam) DjPeopleStrip(model, u, Modifier.fillMaxWidth().height((40 * u).dp))
        } else {
            Column(verticalArrangement = Arrangement.spacedBy((6 * u).dp), horizontalAlignment = Alignment.CenterHorizontally) {
                DjJamButton(model, u)
                DjRoundButton((26 * u).dp, "add", "Add a video to the queue") { model.openLoader(com.forkmesh.nativeapp.djstudio.DjLoadTarget.QUEUE) }
            }
        }
    }
    val list: @Composable (Modifier) -> Unit = { inner ->
        if (items.isEmpty()) {
            Box(inner.djPress("Add a video to the queue") { model.openLoader(com.forkmesh.nativeapp.djstudio.DjLoadTarget.QUEUE) }, contentAlignment = Alignment.Center) {
                DjText(if (model.inJam) "Queue videos for the jam" else "Tap + to queue videos", 11 * u, DjTheme.muted, FontWeight.Medium)
            }
        } else if (vertical) {
            Column(inner.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy((6 * u).dp)) {
                items.forEachIndexed { index, item -> DjQueueThumb(model, item, index == 0, u, Modifier.fillMaxWidth().aspectRatio(16f / 9f)) }
            }
        } else {
            Row(inner.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy((6 * u).dp)) {
                items.forEachIndexed { index, item -> DjQueueThumb(model, item, index == 0, u, Modifier.size((72 * u).dp, (44 * u).dp)) }
            }
        }
    }
    val shape = RoundedCornerShape((6 * u).dp)
    val base = modifier.clip(shape).background(DjTheme.panel.copy(alpha = 0.85f)).border(1.dp, DjTheme.edge, shape).padding((6 * u).dp)
    if (vertical) {
        Column(base, verticalArrangement = Arrangement.spacedBy((6 * u).dp)) {
            content()
            list(Modifier.weight(1f).fillMaxWidth())
        }
    } else {
        Row(base, horizontalArrangement = Arrangement.spacedBy((8 * u).dp), verticalAlignment = Alignment.CenterVertically) {
            content()
            list(Modifier.weight(1f).fillMaxSize())
        }
    }
}

@Composable
private fun DjQueueThumb(model: DjStudioModel, item: DjQueueItem, next: Boolean, u: Float, modifier: Modifier) {
    var choosing by remember { mutableStateOf(false) }
    var origin by remember { mutableStateOf(Offset.Zero) }
    val current by rememberUpdatedState(item)
    val dragging = model.draggedId == item.id
    Box {
        Box(
            modifier
                .alpha(if (dragging) 0.35f else 1f)
                .clip(RoundedCornerShape((4 * u).dp))
                .border(if (next) 1.5.dp else 1.dp, if (next) DjTheme.active.copy(alpha = 0.8f) else DjTheme.edge, RoundedCornerShape((4 * u).dp))
                .onGloballyPositioned { origin = it.boundsInRoot().topLeft }
                .pointerInput(item.id) { detectTapGestures { choosing = true } }
                .pointerInput(item.id) {
                    detectDragGesturesAfterLongPress(
                        onDragStart = { at -> model.dragQueue(current, origin + at) },
                        onDragEnd = { model.dropQueue(current) },
                        onDragCancel = { model.cancelQueueDrag() },
                    ) { change, _ ->
                        change.consume()
                        model.dragQueue(current, origin + change.position)
                    }
                },
        ) {
            DjThumb(item.thumbnail, Modifier.fillMaxSize())
            if (model.inJam && item.addedBy.isNotEmpty()) {
                DjText(
                    item.addedBy, 8 * u, DjTheme.ink, FontWeight.SemiBold,
                    modifier = Modifier.align(Alignment.BottomStart).padding((2 * u).dp).clip(RoundedCornerShape((2 * u).dp)).background(Color.Black.copy(alpha = 0.65f)).padding(horizontal = (3 * u).dp),
                )
            }
        }
        FmActionMenu(choosing, { choosing = false }) {
            DjText(item.title.ifEmpty { "YouTube video" }, 13f, DjTheme.muted, modifier = Modifier.padding(horizontal = 8.dp))
            FmActionMenuButton("Load on Deck A", { choosing = false; model.playFromQueue(item, DjDeckSide.A) })
            FmActionMenuButton("Load on Deck B", { choosing = false; model.playFromQueue(item, DjDeckSide.B) })
            FmActionMenuButton("Tags and Playlists…", { choosing = false; model.editSong(item.videoId) })
            FmActionMenuButton("Remove from Queue", { choosing = false; model.removeFromQueue(item) }, destructive = true)
        }
    }
}

@Composable
internal fun DjQueueGhost(model: DjStudioModel) {
    val id = model.draggedId ?: return
    val point = model.dragPoint ?: return
    val item = model.shownQueue.firstOrNull { it.id == id } ?: return
    val density = LocalDensity.current
    val width = with(density) { 96.dp.toPx() }
    val height = with(density) { 54.dp.toPx() }
    Column(
        Modifier.offset { IntOffset((point.x - width / 2).roundToInt(), (point.y - height - with(density) { 24.dp.toPx() }).roundToInt()) },
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        DjThumb(item.thumbnail, Modifier.size(96.dp, 54.dp).clip(RoundedCornerShape(5.dp)).border(2.dp, DjTheme.active, RoundedCornerShape(5.dp)))
        Spacer(Modifier.height(4.dp))
        DjText(model.dropSide?.let { "Load on Deck ${it.label}" } ?: "Drop on a deck", 11f, DjTheme.ink, FontWeight.SemiBold,
            modifier = Modifier.clip(CircleShape).background(Color(0xE6202226)).padding(horizontal = 8.dp, vertical = 2.dp))
    }
}

@Composable
internal fun DjJamButton(model: DjStudioModel, u: Float) {
    Box {
        DjRoundButton((26 * u).dp, "group", "Jam session", lit = model.inJam) { model.sheet = DjSheet.Jam }
        if (model.pendingInvites > 0) Box(Modifier.align(Alignment.TopEnd).size((8 * u).dp).clip(CircleShape).background(DjTheme.record))
    }
}

internal fun personColor(name: String): Color {
    val hue = (name.fold(0) { hash, c -> hash * 31 + c.code } and 0x7fffffff) % 360
    return Color.hsv(hue.toFloat(), 0.55f, 0.8f)
}

@Composable
internal fun DjPeopleStrip(model: DjStudioModel, u: Float, modifier: Modifier) {
    val jam = model.jam ?: return
    val now = rememberFrameClock(model.crowd.any { it.motion.isNotEmpty() })
    Row(modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy((6 * u).dp), verticalAlignment = Alignment.CenterVertically) {
        for (member in jam.members) DjPerson(model, member, (22 * u).dp, now)
    }
}

@Composable
internal fun DjPerson(model: DjStudioModel, member: DjJamMember, size: androidx.compose.ui.unit.Dp, now: androidx.compose.runtime.MutableLongState) {
    val status = model.status(member)
    val presence = model.crowd.firstOrNull { it.name == member.name }
    val ring = when (status) {
        DjPersonStatus.STUDIO -> DjTheme.play
        DjPersonStatus.LISTENING -> DjTheme.active
        else -> DjTheme.muted
    }
    Box(
        Modifier
            .size(size)
            .graphicsLayer {
                val motion = presence?.motion.orEmpty()
                if (motion.isNotEmpty()) {
                    val sample = DjMotion.replay(motion, (now.longValue % 1_500_000_000L) / 1.5e9)
                    translationX = (sample.x * size.toPx() * 0.12).toFloat()
                    translationY = (-sample.y * size.toPx() * 0.12).toFloat()
                    rotationZ = (sample.x * 8).toFloat()
                }
                alpha = if (status == DjPersonStatus.AWAY) 0.5f else 1f
            }
            .clip(CircleShape)
            .background(personColor(member.name))
            .border(2.dp, ring, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        DjText(member.name.take(1).uppercase(), size.value * 0.45f, Color.White, FontWeight.Bold)
        if (member.name == model.jam?.owner) {
            FmIcon("workspace_premium", size = size * 0.45f, tint = Color(0xFFFFD54F), modifier = Modifier.align(Alignment.TopEnd).offset(size * 0.2f, -size * 0.2f))
        }
    }
}

@Composable
internal fun DjReactionBar(model: DjStudioModel, modifier: Modifier) {
    Row(
        modifier.clip(RoundedCornerShape(24.dp)).background(Color(0xF0202226)).border(0.5.dp, Color.White.copy(alpha = 0.12f), RoundedCornerShape(24.dp))
            .horizontalScroll(rememberScrollState()).padding(horizontal = 10.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        for (reaction in DjReaction.ALL) {
            Box(Modifier.size(40.dp).djPress(reaction.name) { model.react(reaction) }, contentAlignment = Alignment.Center) {
                androidx.compose.material3.Text(reaction.emoji, fontSize = 24.sp)
            }
        }
    }
}

@Composable
internal fun DjReactionLayer(model: DjStudioModel, modifier: Modifier) {
    if (model.effects.isEmpty()) return
    val measurer = rememberTextMeasurer()
    var now by remember { mutableStateOf(android.os.SystemClock.uptimeMillis()) }
    LaunchedEffect(Unit) {
        while (true) withFrameNanos { now = android.os.SystemClock.uptimeMillis() }
    }
    Canvas(modifier) {
        for (effect in model.effects.toList()) {
            val t = ((now - effect.born) / 1000.0 / effect.reaction.seconds).coerceIn(0.0, 1.0)
            drawEffect(measurer, effect, t)
        }
    }
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawEffect(measurer: TextMeasurer, effect: DjEffect, t: Double) {
    val reaction = effect.reaction
    val w = size.width
    val h = size.height
    val seed = effect.id
    val fade = if (t > 0.75) (1 - t) / 0.25 else 1.0
    fun emoji(x: Float, y: Float, scale: Float, alpha: Double) {
        val result = measurer.measure(reaction.emoji, TextStyle(fontSize = (34 * scale).sp))
        drawText(result, topLeft = Offset(x - result.size.width / 2f, y - result.size.height / 2f), alpha = alpha.coerceIn(0.0, 1.0).toFloat())
    }
    val count = 14
    for (index in 0 until count) {
        val r = ((seed * 31 + index * 977) % 1000) / 1000.0
        val r2 = ((seed * 17 + index * 541) % 1000) / 1000.0
        when (reaction.style) {
            DjReactionStyle.RISE -> emoji((w * r).toFloat(), (h * (1.1 - t * (0.9 + r2 * 0.5))).toFloat(), 0.8f + r2.toFloat() * 0.6f, fade)
            DjReactionStyle.RAIN, DjReactionStyle.CONFETTI -> emoji((w * r + sin(t * 6 + index) * 20).toFloat(), (h * (-0.1 + t * (1.0 + r2 * 0.4))).toFloat(), 0.7f + r2.toFloat() * 0.5f, fade)
            DjReactionStyle.BURST, DjReactionStyle.FIREWORKS -> {
                val angle = index / count.toDouble() * 2 * PI + r
                val distance = t * (0.25 + r2 * 0.25) * w
                emoji((w / 2 + cos(angle) * distance).toFloat(), (h / 2 + sin(angle) * distance).toFloat(), 0.8f + r2.toFloat() * 0.4f, fade)
            }
            DjReactionStyle.ZOOM, DjReactionStyle.FLASH -> if (index == 0) emoji(w / 2, h / 2, (1 + t * 4).toFloat(), if (reaction.style == DjReactionStyle.FLASH) fade * (0.6 + 0.4 * sin(t * PI * 4)) else fade)
            DjReactionStyle.SPIN, DjReactionStyle.ORBIT -> {
                if (index < 8) {
                    val angle = index / 8.0 * 2 * PI + t * 2 * PI * (if (reaction.style == DjReactionStyle.SPIN) 2 else 1)
                    val radius = w * 0.25
                    emoji((w / 2 + cos(angle) * radius).toFloat(), (h / 2 + sin(angle) * radius).toFloat(), 1f, fade)
                }
            }
            DjReactionStyle.WAVE -> emoji((w * (index / count.toDouble())).toFloat(), (h * 0.5 + sin(t * 2 * PI * 2 + index * 0.6) * h * 0.15).toFloat(), 0.9f, fade)
        }
    }
    if (effect.sender.isNotEmpty()) {
        val label = measurer.measure("${effect.sender} ${reaction.emoji}", TextStyle(fontSize = 15.sp, color = Color.White, fontWeight = FontWeight.SemiBold))
        drawText(label, topLeft = Offset(w / 2 - label.size.width / 2f, 56f), alpha = fade.toFloat())
    }
}

