package com.forkmesh.nativeapp.feature.chat

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameMillis
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Matrix
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.chat.DirectLiveEngine
import com.forkmesh.nativeapp.chat.DirectLiveFrames
import com.forkmesh.nativeapp.chat.DirectLivePhonePose
import com.forkmesh.nativeapp.chat.DirectLivePoint
import com.forkmesh.nativeapp.chat.DirectLiveStroke
import com.forkmesh.nativeapp.chat.DirectLiveTap
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.LocalFmReducedMotion
import kotlinx.coroutines.delay
import kotlin.math.hypot
import kotlin.math.min

internal object DirectLiveGeometry {
    fun point(location: Offset, size: IntSize): DirectLivePoint {
        val width = size.width.coerceAtLeast(1).toDouble()
        return DirectLivePoint(location.x / width, (location.y - size.height / 2.0) / width)
    }

    fun location(point: DirectLivePoint, size: Size): Offset =
        Offset((point.x * size.width).toFloat(), (size.height / 2.0 + point.y * size.width).toFloat())

    fun color(rgb: Int): Color = Color(0xFF000000.toInt() or (rgb and 0xFFFFFF))

    fun colorName(rgb: Int): String = when (rgb) {
        0xFFFFFF -> "white"
        0xFF453A -> "red"
        0xFF9F0A -> "orange"
        0xFFD60A -> "yellow"
        0x32D74B -> "green"
        0x64D2FF -> "blue"
        0xBF5AF2 -> "purple"
        0x000000 -> "black"
        else -> "custom"
    }

    val PARTNER_RIM = Color(0xFFFF375F)
    val PHONE_HEIGHT = 76.dp
    val PHONE_SLOT = 50.dp
}

@Composable
private fun rememberFrameClock(running: Boolean, frameMs: Long): Long {
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(running, frameMs) {
        if (!running) return@LaunchedEffect
        while (true) {
            withFrameMillis { }
            now = System.currentTimeMillis()
            delay(frameMs)
        }
    }
    return now
}

@Composable
internal fun DirectLiveInkLayer(conversation: String, modifier: Modifier = Modifier) {
    val engine = DirectLiveFeature.engine ?: return
    val revision by DirectLiveFeature.revision.collectAsState()
    val strokes = remember(revision, conversation) { engine.strokes(conversation) }
    val taps = remember(revision, conversation) { engine.taps(conversation) }
    val busy = strokes.isNotEmpty() || taps.isNotEmpty()
    val now = rememberFrameClock(busy, 33)
    if (!busy) return
    Canvas(modifier.fillMaxSize()) {
        for (stroke in strokes) drawStroke(stroke, now)
        for (tap in taps) drawRipple(tap, now)
    }
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawStroke(stroke: DirectLiveStroke, now: Long) {
    val opacity = stroke.opacity(now).toFloat()
    val first = stroke.points.firstOrNull() ?: return
    if (opacity <= 0f) return
    val color = DirectLiveGeometry.color(stroke.color)
    val width = stroke.width.toFloat().dp.toPx()
    if (stroke.points.size == 1) {
        drawCircle(color.copy(alpha = opacity), radius = width / 2f, center = DirectLiveGeometry.location(first, size))
        return
    }
    val path = Path()
    var previous = DirectLiveGeometry.location(first, size)
    path.moveTo(previous.x, previous.y)
    for (point in stroke.points.drop(1)) {
        val next = DirectLiveGeometry.location(point, size)
        path.quadraticTo(previous.x, previous.y, (previous.x + next.x) / 2f, (previous.y + next.y) / 2f)
        previous = next
    }
    path.lineTo(previous.x, previous.y)
    drawPath(path, color.copy(alpha = opacity * 0.35f), style = Stroke(width * 2.6f, cap = StrokeCap.Round, join = StrokeJoin.Round))
    drawPath(path, color.copy(alpha = opacity), style = Stroke(width, cap = StrokeCap.Round, join = StrokeJoin.Round))
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawRipple(tap: DirectLiveTap, now: Long) {
    val t = (now - tap.at).toFloat() / DirectLiveFrames.TAP_LIFE_MS.toFloat()
    if (t < 0f || t >= 1f) return
    val center = DirectLiveGeometry.location(tap.point, size)
    val tint = if (tap.isSelf) Color.White else DirectLiveGeometry.PARTNER_RIM
    for (ring in 0 until 3) {
        val local = t - ring * 0.14f
        if (local <= 0f) continue
        val radius = (14f + local * 120f).dp.toPx()
        drawCircle(tint.copy(alpha = ((1 - local) * 0.8f).coerceIn(0f, 1f)), radius, center, style = Stroke((3f - ring * 0.7f).dp.toPx()))
    }
    drawCircle(tint.copy(alpha = 1 - t), (16f * (1 - t)).dp.toPx(), center)
}

@Composable
internal fun DirectLiveTouchSurface(conversation: String, onDone: () -> Unit) {
    val engine = DirectLiveFeature.engine ?: return
    val context = LocalContext.current
    val travel = with(LocalDensity.current) { 8.dp.toPx() }
    var color by remember { mutableIntStateOf(DirectLiveFrames.COLORS[1]) }
    var picking by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxSize()) {
        Box(
            Modifier
                .fillMaxSize()
                .semantics {
                    contentDescription = "Touch canvas. Tap to make their phone tap back. Drag to draw on the background."
                    customActions = listOf(
                        CustomAccessibilityAction("Send a tap") {
                            engine.tap(conversation, DirectLivePoint(0.5, 0.0))
                            DirectLiveHaptics.sent(context)
                            true
                        },
                    )
                }
                .pointerInput(conversation, color) {
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        picking = false
                        val start = down.position
                        var stroke = ""
                        var last = start
                        while (true) {
                            val event = awaitPointerEvent()
                            val change = event.changes.firstOrNull { it.id == down.id } ?: break
                            last = change.position
                            if (!change.pressed) break
                            if (stroke.isEmpty() && hypot(last.x - start.x, last.y - start.y) >= travel) {
                                stroke = engine.beginStroke(conversation, DirectLiveGeometry.point(start, size), color, 5.0)
                            }
                            if (stroke.isNotEmpty()) engine.extendStroke(conversation, stroke, DirectLiveGeometry.point(last, size))
                            change.consume()
                        }
                        if (stroke.isEmpty()) {
                            engine.tap(conversation, DirectLiveGeometry.point(last, size))
                            DirectLiveHaptics.sent(context)
                        } else {
                            engine.extendStroke(conversation, stroke, DirectLiveGeometry.point(last, size))
                            engine.endStroke(conversation, stroke)
                        }
                    }
                },
        )
        Column(
            Modifier.fillMaxWidth().padding(top = FmSpace.x2, start = FmSpace.x3, end = FmSpace.x3),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                LiveToolbar(
                    color = color,
                    picking = picking,
                    onPick = { picking = !picking },
                    onClear = { engine.clearInk(conversation) },
                    onDone = onDone,
                )
                AnimatedVisibility(picking, enter = fadeIn(tween(180)), exit = fadeOut(tween(180))) {
                    LiveColorGrid(color) {
                        color = it
                        picking = false
                    }
                }
            }
            if (!picking) {
                Text(
                    "Tap to buzz. Draw on the background.",
                    modifier = Modifier
                        .fmGlass(RoundedCornerShape(50))
                        .padding(horizontal = 14.dp, vertical = 8.dp),
                    style = TextStyle(fontSize = 13.sp, fontWeight = FontWeight.Medium, color = Fm.colors.textPrimary),
                )
            }
        }
    }
}

@Composable
private fun LiveSwatch(rgb: Int, selected: Boolean) {
    Box(
        Modifier
            .size(30.dp)
            .border(2.dp, if (selected) Fm.colors.textPrimary else Color.Transparent, CircleShape)
            .padding(3.dp)
            .background(DirectLiveGeometry.color(rgb), CircleShape)
            .border(0.5.dp, Color.Gray.copy(alpha = 0.5f), CircleShape),
    )
}

@Composable
private fun LiveToolbar(color: Int, picking: Boolean, onPick: () -> Unit, onClear: () -> Unit, onDone: () -> Unit) {
    val ink = Fm.colors.textPrimary
    val haptics = com.forkmesh.nativeapp.ui.theme.FmHaptics.current
    Row(
        Modifier
            .fmGlass(RoundedCornerShape(50))
            .padding(horizontal = 6.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(
            Modifier
                .height(44.dp)
                .clickable {
                    haptics.selection()
                    onPick()
                }
                .padding(horizontal = 8.dp)
                .semantics {
                    role = Role.Button
                    contentDescription = "Ink colour"
                    stateDescription = DirectLiveGeometry.colorName(color)
                },
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            LiveSwatch(color, selected = false)
            FmIcon("keyboard_arrow_down", size = 18.dp, tint = ink, modifier = Modifier.rotate(if (picking) 180f else 0f))
        }
        LiveToolButton("delete_outline", "Clear drawing") {
            haptics.tap()
            onClear()
        }
        LiveToolButton("check", "Done", onDone)
    }
}

@Composable
private fun LiveToolButton(icon: String, label: String, onClick: () -> Unit) {
    Box(
        Modifier
            .size(44.dp)
            .clickable(onClick = onClick)
            .semantics {
                role = Role.Button
                contentDescription = label
            },
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = 20.dp, tint = Fm.colors.textPrimary)
    }
}

@Composable
private fun LiveColorGrid(selected: Int, onPick: (Int) -> Unit) {
    val haptics = com.forkmesh.nativeapp.ui.theme.FmHaptics.current
    Column(
        Modifier
            .fmGlass(RoundedCornerShape(22.dp))
            .padding(6.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        for (row in DirectLiveFrames.COLORS.chunked(4)) {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                for (rgb in row) {
                    Box(
                        Modifier
                            .size(44.dp)
                            .clickable {
                                haptics.selection()
                                onPick(rgb)
                            }
                            .semantics {
                                role = Role.Button
                                contentDescription = "Ink colour ${DirectLiveGeometry.colorName(rgb)}"
                                this.selected = rgb == selected
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        LiveSwatch(rgb, rgb == selected)
                    }
                }
            }
        }
    }
}

@Composable
internal fun DirectLiveTouchButton(drawing: Boolean, onToggle: () -> Unit) {
    val colors = Fm.colors
    Box(
        Modifier
            .size(44.dp)
            .fmGlass(CircleShape, interactive = true)
            .background(if (drawing) colors.accent else Color.Transparent, CircleShape)
            .clickable(onClick = onToggle)
            .semantics {
                role = Role.Button
                contentDescription = if (drawing) "Stop drawing" else "Draw"
            },
        contentAlignment = Alignment.Center,
    ) {
        FmIcon("draw", size = 18.dp, tint = if (drawing) Color.White else colors.textPrimary)
    }
}

@Composable
internal fun DirectLiveOnlineLight(color: Color) {
    val reduced = LocalFmReducedMotion.current
    val alpha = if (reduced) {
        1f
    } else {
        val blink = rememberInfiniteTransition(label = "online")
        val value by blink.animateFloat(1f, 0.2f, infiniteRepeatable(tween(700), RepeatMode.Reverse), label = "online-alpha")
        value
    }
    Box(
        Modifier
            .size(7.dp)
            .graphicsLayer { this.alpha = alpha }
            .shadow((3f * alpha).dp, CircleShape, ambientColor = color, spotColor = color)
            .background(color, CircleShape),
    )
}

@Composable
internal fun DirectLivePartnerPhone(conversation: String, name: String, dark: Boolean, modifier: Modifier = Modifier) {
    val engine = DirectLiveFeature.engine ?: return
    val context = LocalContext.current
    val revision by DirectLiveFeature.revision.collectAsState()
    val sharing = remember(revision, conversation) { engine.partnerMotion(conversation) != null }
    val now = rememberFrameClock(sharing, 16)
    val pose = if (sharing) engine.motionTrack(conversation)?.let { DirectLivePhonePose.live(it, now) } else null
    val avatar = remember(name) { Services.get<ApiService>()?.knownAccountAvatarPng(name).orEmpty() }
    Box(
        modifier
            .size(DirectLiveGeometry.PHONE_SLOT, 44.dp)
            .clickable(indication = null, interactionSource = null) {}
            .semantics {
                contentDescription = "$name's phone"
                stateDescription = if (sharing) "Showing its direction" else "Not sharing its direction"
                customActions = listOf(
                    CustomAccessibilityAction("Send a tap") {
                        engine.tap(conversation, DirectLivePoint(0.5, 0.0))
                        DirectLiveHaptics.sent(context)
                        true
                    },
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        DirectLivePhoneFigure(pose, dark, name, avatar, Modifier.requiredSize(DirectLiveGeometry.PHONE_HEIGHT))
    }
}

@Composable
internal fun DirectLivePhoneFigure(pose: DirectLivePhonePose?, dark: Boolean, name: String, avatarPng: String, modifier: Modifier = Modifier) {
    val shown = pose ?: DirectLivePhonePose.still
    val height = DirectLiveGeometry.PHONE_HEIGHT
    val unit = height / DirectLivePhonePose.HEIGHT.toFloat()
    val ink = if (dark) Color.White else Color.Black.copy(alpha = 0.75f)
    val rim = if (pose == null) ink.copy(alpha = 0.45f) else DirectLiveGeometry.PARTNER_RIM.copy(alpha = min(1f, 0.75f + shown.jolt.toFloat() * 0.25f))
    val screen = if (dark) Color(0xFF141414) else Color(0xFFF7F7F7)
    val outline = remember(shown.roll, shown.pitch) { shown.outline() }
    val map = shown.faceMap
    Box(
        modifier.graphicsLayer {
            val lift = shown.lift.toFloat()
            scaleX = lift
            scaleY = lift
            translationX = (shown.offset.x * unit.toPx()).toFloat()
            translationY = (shown.offset.y * unit.toPx()).toFloat()
        },
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.fillMaxSize()) {
            val px = unit.toPx()
            val path = Path()
            outline.forEachIndexed { index, point ->
                val x = center.x + point.x.toFloat() * px
                val y = center.y + point.y.toFloat() * px
                if (index == 0) path.moveTo(x, y) else path.lineTo(x, y)
            }
            path.close()
            drawPath(path, rim)
        }
        if (shown.faceShowing) {
            val faceWidth = unit * DirectLivePhonePose.WIDTH.toFloat()
            val faceHeight = unit * DirectLivePhonePose.HEIGHT.toFloat()
            val bezel = unit * DirectLivePhonePose.BEZEL.toFloat()
            val corner = unit * DirectLivePhonePose.CORNER.toFloat()
            Box(
                Modifier
                    .size(faceWidth, faceHeight)
                    .drawWithContent {
                        val w = size.width
                        val h = size.height
                        val px = unit.toPx()
                        val a = map.a.toFloat()
                        val b = map.b.toFloat()
                        val c = map.c.toFloat()
                        val d = map.d.toFloat()
                        val matrix = Matrix()
                        matrix.values[Matrix.ScaleX] = a
                        matrix.values[Matrix.SkewY] = b
                        matrix.values[Matrix.SkewX] = c
                        matrix.values[Matrix.ScaleY] = d
                        matrix.values[Matrix.TranslateX] = map.tx.toFloat() * px + w / 2f - a * w / 2f - c * h / 2f
                        matrix.values[Matrix.TranslateY] = map.ty.toFloat() * px + h / 2f - b * w / 2f - d * h / 2f
                        drawContext.transform.transform(matrix)
                        drawContent()
                    },
                contentAlignment = Alignment.Center,
            ) {
                Box(Modifier.fillMaxSize().padding(bezel).background(screen, RoundedCornerShape(corner - bezel)))
                Box(
                    Modifier
                        .align(Alignment.TopCenter)
                        .padding(top = unit * 3f)
                        .size(unit * 6f, unit * 1.4f)
                        .background(rim, RoundedCornerShape(50)),
                )
                FmAvatar(name.ifEmpty { "?" }, unit * 15f, avatarPng = avatarPng, textStyle = Fm.text.labelSmall)
            }
        }
    }
}
