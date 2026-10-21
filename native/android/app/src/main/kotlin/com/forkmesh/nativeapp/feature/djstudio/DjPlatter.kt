package com.forkmesh.nativeapp.feature.djstudio

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.djstudio.DjFocus
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.ui.kit.FmActionMenu
import com.forkmesh.nativeapp.ui.kit.FmActionMenuButton
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.flow.first
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.exp
import kotlin.math.min
import kotlin.math.sqrt

@Composable
internal fun DjPlatter(model: DjStudioModel, deck: DjDeck, d: Dp) {
    Box(
        Modifier.size(d).onGloballyPositioned { model.deckFrames[deck.side] = it.boundsInRoot() },
    ) {
        Canvas(Modifier.matchParentSize()) { drawPlinth() }
        DjRecord(model, deck, d)
        Canvas(Modifier.matchParentSize().graphicsLayer {
            val progress = if (deck.duration > 0) min(1.0, deck.position() / deck.duration).toFloat() else 0f
            rotationZ = if (deck.loaded) 12 + 16 * progress else 0f
            transformOrigin = androidx.compose.ui.graphics.TransformOrigin(0.885f, 0.115f)
        }) { drawTonearm() }
        val inset = { fx: Float, fy: Float, size: Dp -> Modifier.offset(d * fx - size / 2, d * fy - size / 2) }
        Box(inset(0.13f, 0.13f, d * 0.13f)) {
            DjRoundButton(d * 0.13f, "library_music", "Load Deck ${deck.side.label}") { model.openLoad(deck.side) }
        }
        Box(inset(0.12f, 0.88f, d * 0.11f).size(d * 0.11f).djPress("Return Deck ${deck.side.label} to cue and stop") { model.cueStop(deck.side) }, contentAlignment = Alignment.Center) {
            Canvas(Modifier.size(d * 0.075f)) {
                drawCircle(Color(0xFF101113))
                drawCircle(
                    Brush.radialGradient(listOf(if (deck.playing) Color(0xFFFF4040) else Color(0xFF3A1416), Color(0xFF200A0B)), radius = size.minDimension * 0.4f),
                    radius = size.minDimension / 3,
                )
                drawCircle(Color.White.copy(alpha = 0.15f), style = Stroke(1f))
            }
        }
        Box(inset(0.3f, 0.955f, d * 0.1f)) {
            DjRoundButton(d * 0.1f, "skip_next", "Load the next song on Deck ${deck.side.label}") { model.quickNext(deck.side) }
        }
        if (deck.loaded) {
            Box(inset(0.7f, 0.955f, d * 0.1f)) {
                DjRoundButton(d * 0.1f, "travel_explore", "Find songs like this one") { model.openSimilar(deck.side) }
            }
            Box(inset(0.88f, 0.88f, d * 0.13f)) { DjFileButton(model, deck, d * 0.13f) }
        }
        DjDropRing(model, deck, d)
    }
}

@Composable
private fun DjRecord(model: DjStudioModel, deck: DjDeck, d: Dp) {
    var turn by remember { mutableStateOf(0.0) }
    LaunchedEffect(deck) {
        var last = 0L
        var speed = 0.0
        var lastPosition = deck.position()
        while (true) {
            if (speed == 0.0 && !deck.playing && !deck.scratching) {
                androidx.compose.runtime.snapshotFlow { deck.playing || deck.scratching }.first { it }
                last = 0L
                lastPosition = deck.position()
            }
            withFrameNanos { now ->
                val dt = if (last == 0L) 0.0 else min(0.1, (now - last) / 1e9)
                last = now
                val position = deck.position()
                if (deck.scratching) {
                    turn += position - lastPosition
                    speed = 0.0
                } else {
                    val goal = if (deck.playing) deck.effectiveRate else 0.0
                    val tau = if (goal > speed) 0.09 else 0.18
                    if (dt > 0) speed += (goal - speed) * (1 - exp(-dt / tau))
                    if (abs(speed) < 0.0005 && goal == 0.0) speed = 0.0
                    turn += speed * dt
                }
                lastPosition = position
            }
        }
    }
    var lastAngle by remember { mutableStateOf<Double?>(null) }
    val gain = DjStudio.gain(deck.side, model.crossfader)
    Box(
        Modifier
            .size(d)
            .semantics {
                contentDescription = "Deck ${deck.side.label} turntable"
                stateDescription = if (deck.loaded) if (deck.playing) "Playing" else "Paused" else "Empty"
                if (!deck.loaded) onClick {
                    model.openLoad(deck.side)
                    true
                }
            }
            .pointerInput(deck.side) {
                awaitEachGesture {
                    val down = awaitFirstDown()
                    val centre = Offset(size.width / 2f, size.height / 2f)
                    val radius = size.width / 2f
                    if ((down.position - centre).getDistance() > radius * 0.86f) return@awaitEachGesture
                    var pressed = false
                    if (deck.loaded) {
                        pressed = true
                        model.touchBegan(deck.side)
                    }
                    lastAngle = null
                    var moved = Offset.Zero
                    while (true) {
                        val event = awaitPointerEvent()
                        val change = event.changes.firstOrNull { it.id == down.id } ?: break
                        if (!change.pressed) break
                        moved = change.position - down.position
                        val dx = (change.position.x - centre.x).toDouble()
                        val dy = (change.position.y - centre.y).toDouble()
                        val angle = atan2(dy, dx)
                        val previous = lastAngle
                        if (previous == null) {
                            val threshold = maxOf(DjStudio.SCRATCH_THRESHOLD * density, size.width * 0.05)
                            if (deck.loaded && DjStudio.isScratch(moved.x.toDouble(), moved.y.toDouble(), threshold)) {
                                lastAngle = angle
                                model.scratchBegan(deck.side)
                            }
                        } else {
                            lastAngle = angle
                            change.consume()
                            if (sqrt(dx * dx + dy * dy) > size.width * 0.07) {
                                var delta = angle - previous
                                if (delta > PI) delta -= 2 * PI else if (delta < -PI) delta += 2 * PI
                                model.scratchMoved(deck.side, delta)
                            }
                        }
                    }
                    if (lastAngle != null) {
                        lastAngle = null
                        model.scratchEnded(deck.side)
                    } else if (pressed) {
                        model.touchEnded(deck.side)
                    } else if (!deck.loaded && abs(moved.x) + abs(moved.y) < 10 * density) {
                        model.openLoad(deck.side)
                    }
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(d * 0.82f)) { drawVinyl() }
        Box(Modifier.size(d).graphicsLayer { rotationZ = DjStudio.turnDegrees(turn).toFloat() }, contentAlignment = Alignment.Center) {
            deck.artwork?.let {
                Image(
                    it, contentDescription = null, contentScale = ContentScale.Crop,
                    modifier = Modifier.size(d * 0.82f).clip(CircleShape).graphicsLayer { alpha = (0.4 + 0.6 * gain).toFloat() },
                )
            }
            Canvas(Modifier.size(d)) { drawMarks() }
        }
    }
}

@Composable
private fun DjDropRing(model: DjStudioModel, deck: DjDeck, d: Dp) {
    val dragging = model.draggedId != null
    val target = model.dropSide == deck.side
    if (!dragging) return
    Canvas(Modifier.size(d)) {
        val radius = size.minDimension * 0.46f
        if (target) drawCircle(DjTheme.active.copy(alpha = 0.18f), radius)
        drawCircle(
            DjTheme.active.copy(alpha = if (target) 1f else 0.35f), radius,
            style = Stroke(if (target) 4.dp.toPx() else 2.dp.toPx(), pathEffect = if (target) null else androidx.compose.ui.graphics.PathEffect.dashPathEffect(floatArrayOf(18f, 15f))),
        )
    }
}

@Composable
private fun DjFileButton(model: DjStudioModel, deck: DjDeck, size: Dp) {
    var menu by remember { mutableStateOf(false) }
    val crate = model.crate
    val id = deck.videoId
    val filed = crate.track(id)?.tags?.isNotEmpty() == true || crate.playlists.any { id in it.songs }
    Box {
        DjRoundButton(size, "playlist_add", "File this song into a playlist or tag", lit = filed) { menu = true }
        FmActionMenu(menu, { menu = false }, offset = DpOffset((-200).dp, 0.dp)) {
            val focus = crate.focus
            val inFocus = focus.isFiled && crate.songs(focus).any { it.id == id }
            crate.playlists.forEach { list ->
                FmActionMenuButton((if (id in list.songs) "✓ " else "") + list.name, { menu = false; model.move(deck.side, DjFocus.Playlist(list.id)) })
            }
            crate.tagCounts.forEach { tag ->
                FmActionMenuButton((if (crate.track(id)?.tags?.contains(tag.name) == true) "✓ " else "") + "#${tag.name}", { menu = false; model.move(deck.side, DjFocus.Tag(tag.name)) })
            }
            FmActionMenuButton("New Playlist…", { menu = false; model.prompt = DjPrompt.NewPlaylist(listOf(id)) })
            FmActionMenuButton("New Tag…", { menu = false; model.prompt = DjPrompt.NewTag(id) })
            if (inFocus) FmActionMenuButton("Take Out of ${crate.label(focus)}", { menu = false; model.move(deck.side, DjFocus.Everything) }, destructive = true)
            FmActionMenuButton(if (crate.shuffles(id)) "Don't Shuffle" else "Shuffle Again", { menu = false; model.toggleShuffle(id) })
            FmActionMenuButton("Tags and Playlists…", { menu = false; model.editSong(id) })
            FmActionMenuButton("Find Similar…", { menu = false; model.openSimilar(deck.side) })
            FmActionMenuButton("Delete from Library", { menu = false; model.deleteSong(deck.side) }, destructive = true)
        }
    }
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawPlinth() {
    val w = size.width
    val face = Brush.verticalGradient(listOf(Color(0xFF33363C), Color(0xFF1F2125)))
    fun circle(brush: Brush, fx: Float, fy: Float, fd: Float) = drawCircle(brush, w * fd / 2, Offset(w * fx, w * fy))
    drawCircle(Color.Black.copy(alpha = 0.35f), w * 0.48f, Offset(w / 2, w / 2 + w * 0.012f))
    circle(face, 0.5f, 0.5f, 0.96f)
    circle(face, 0.13f, 0.13f, 0.2f)
    drawCircle(Color(0xFF17181B), w * 0.11f, Offset(w * 0.885f, w * 0.115f))
    circle(face, 0.12f, 0.88f, 0.12f)
    circle(face, 0.3f, 0.955f, 0.13f)
    circle(face, 0.7f, 0.955f, 0.13f)
    drawCircle(Color.Black.copy(alpha = 0.5f), w * 0.48f, style = Stroke(1f))
    drawCircle(Brush.verticalGradient(listOf(Color(0xFF4A4D53), Color(0xFF2A2C30))), w * 0.45f)
    drawCircle(Color.Black.copy(alpha = 0.55f), w * 0.415f, style = Stroke(1.5f))
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawVinyl() {
    val w = size.width
    drawCircle(Color(0xFF111214))
    var radius = w * 0.17f
    while (radius < w * 0.49f) {
        drawCircle(Color.White.copy(alpha = 0.045f), radius, style = Stroke(0.6f))
        radius += maxOf(2f, w * 0.012f)
    }
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawMarks() {
    val d = size.width
    val centre = Offset(d / 2, d / 2)
    val radius = d * 0.435f
    val dot = maxOf(1.2f, d * 0.007f)
    for (index in 0 until 110) {
        val angle = index / 110.0 * 2 * PI
        drawCircle(Color.White.copy(alpha = 0.55f), dot / 2, Offset(centre.x + (kotlin.math.cos(angle) * radius).toFloat(), centre.y + (kotlin.math.sin(angle) * radius).toFloat()))
    }
    drawCircle(Color.White.copy(alpha = 0.24f), d * 0.135f, centre)
    drawCircle(Color.White.copy(alpha = 0.35f), d * 0.009f, Offset(centre.x + d * 0.09f, centre.y - d * 0.03f))
    drawCircle(Brush.radialGradient(listOf(Color(0xFFD9DCE0), Color(0xFF6D7178)), centre, d * 0.014f), d * 0.013f, centre)
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawTonearm() {
    val w = size.width
    val pivot = Offset(w * 0.885f, w * 0.115f)
    val elbow = Offset(w * 0.885f, w * 0.72f)
    val head = Offset(w * 0.865f, w * 0.8f)
    val silver = Brush.horizontalGradient(listOf(Color(0xFF8C9097), Color(0xFFF4F5F7), Color(0xFF9A9EA5)), pivot.x - w * 0.02f, pivot.x + w * 0.02f)
    val ring = w * 0.075f
    drawCircle(Color(0xFF2B2D32), ring, pivot)
    drawCircle(Color.White.copy(alpha = 0.25f), ring, pivot, style = Stroke(1f))
    drawRoundRect(silver, Offset(pivot.x - w * 0.032f, pivot.y - w * 0.11f), Size(w * 0.064f, w * 0.085f), androidx.compose.ui.geometry.CornerRadius(w * 0.012f))
    val arm = Path().apply {
        moveTo(pivot.x, pivot.y)
        lineTo(elbow.x, elbow.y)
        lineTo(head.x, head.y)
    }
    drawPath(arm, Color.Black.copy(alpha = 0.4f), style = Stroke(w * 0.02f, cap = StrokeCap.Round), alpha = 0.5f)
    drawPath(arm, silver, style = Stroke(w * 0.02f, cap = StrokeCap.Round))
    drawLine(silver, Offset(head.x + w * 0.012f, head.y - w * 0.015f), Offset(head.x - w * 0.022f, head.y + w * 0.04f), w * 0.04f, StrokeCap.Round)
    drawRect(Color(0xFF1A1B1E), Offset(head.x - w * 0.03f, head.y + w * 0.035f), Size(w * 0.022f, w * 0.022f))
    drawCircle(silver, w * 0.03f, pivot)
}

