package com.forkmesh.nativeapp.feature.composer

import android.os.Build
import android.os.SystemClock
import android.provider.Settings
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableLongState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.composer.VoiceLaunchPhase
import com.forkmesh.nativeapp.composer.VoiceLaunchRules
import com.forkmesh.nativeapp.composer.VoiceMeter
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.LocalFmReducedMotion
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin

private const val SPOKES = 72
private const val BURST_MS = 800f
private const val SPARKS = 36
private const val GOLDEN = 2.3999632f
private val ice = Color(0xFF73F2FF)
private val rose = Color(0xFFFF8CE0)
private val gold = Color(0xFFFFD54D)

@Composable
fun rememberVoiceSolid(): Boolean {
    val context = LocalContext.current
    return remember(context) {
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            runCatching { Settings.Secure.getInt(context.contentResolver, "high_text_contrast_enabled", 0) == 1 }.getOrDefault(false)
    }
}

@Composable
fun Modifier.voiceBlur(voice: ShellVoiceLaunch): Modifier {
    val solid = rememberVoiceSolid()
    val radius by animateDpAsState(if (voice.isOpen && !voice.phase.away && !solid) 14.dp else 0.dp, tween(300), label = "voice-blur")
    return if (radius > 0.dp) blur(radius) else this
}

@Composable
fun ShellVoiceLaunchOverlay(voice: ShellVoiceLaunch) {
    if (!voice.isOpen) return
    key(voice.openedAt) { VoiceStage(voice) }
}

@Composable
private fun VoiceStage(voice: ShellVoiceLaunch) {
    val reduced = LocalFmReducedMotion.current
    val solid = rememberVoiceSolid()
    val accent = Fm.colors.accent
    val phase = voice.phase
    val away = phase.away
    val sent = phase is VoiceLaunchPhase.Sent
    val hearing = phase is VoiceLaunchPhase.Listening
    var shown by remember { mutableStateOf(false) }
    var root by remember { mutableStateOf(Offset.Zero) }
    val clock = remember { mutableLongStateOf(SystemClock.uptimeMillis()) }
    LaunchedEffect(Unit) { shown = true }
    LaunchedEffect(reduced) {
        if (reduced) return@LaunchedEffect
        while (true) {
            withFrameNanos { it }
            clock.longValue = SystemClock.uptimeMillis()
        }
    }
    val home = !shown || phase is VoiceLaunchPhase.Leaving
    val travel by animateFloatAsState(
        if (home) 0f else 1f,
        if (reduced) tween(200) else spring(dampingRatio = 0.68f, stiffness = 130f),
        label = "voice-travel",
    )
    val shrink by animateFloatAsState(if (sent && !reduced) 0.3f else 1f, spring(0.6f, Spring.StiffnessMedium), label = "voice-shrink")
    val orbAlpha by animateFloatAsState(if (away && (reduced || sent)) 0f else 1f, tween(250), label = "voice-orb-alpha")
    val dim by animateFloatAsState(if (shown && !away) 1f else 0f, tween(if (sent) 500 else 300), label = "voice-dim")
    val copy by animateFloatAsState(if (shown && !away) 1f else 0f, tween(280, delayMillis = if (shown && !away) 120 else 0), label = "voice-copy")
    val density = LocalDensity.current

    BoxWithConstraints(
        Modifier
            .fillMaxSize()
            .onGloballyPositioned { root = it.positionInRoot() },
    ) {
        val widthPx = constraints.maxWidth.toFloat()
        val heightPx = constraints.maxHeight.toFloat()
        val diameter = min(196f * density.density, widthPx * 0.5f)
        val center = Offset(widthPx / 2f, heightPx * 0.36f)
        val start = if (voice.origin.isEmpty) center else voice.origin.center - root
        val seed = if (voice.origin.width > 0f) voice.origin.width / diameter else 0.22f
        fun place(): Offset = if (reduced) center else Offset(start.x + (center.x - start.x) * travel, start.y + (center.y - start.y) * travel)
        fun grown(): Float = (if (reduced) 1f else seed + (1f - seed) * travel) * shrink
        voice.orb = {
            val half = diameter * grown() / 2f
            val at = place() + root
            Rect(at.x - half, at.y - half, at.x + half, at.y + half)
        }
        val accessibleOrb = VoiceLaunchRules.orbLabel(phase, voice.heard.isNotEmpty())

        Backdrop(voice, accent, solid, center, diameter, dim, reduced, clock) { voice.cancel() }
        if (sent && !reduced) Burst(voice.sentAt, center, diameter, accent, clock)
        Box(
            Modifier
                .size(with(density) { (diameter * 2f).toDp() })
                .graphicsLayer {
                    val at = place()
                    val factor = grown()
                    translationX = at.x - diameter
                    translationY = at.y - diameter
                    scaleX = factor
                    scaleY = factor
                    alpha = if (reduced) travel * orbAlpha else orbAlpha
                },
            contentAlignment = Alignment.Center,
        ) {
            Canvas(Modifier.fillMaxSize()) {
                val nowMs = clock.longValue
                val seconds = if (reduced) 0.0 else nowMs / 1000.0
                val level = voice.loudness
                drawCorona(voice, accent, diameter, seconds, level, hearing, nowMs, phase is VoiceLaunchPhase.Waking, reduced)
                drawCore(accent, diameter, seconds, level, reduced)
            }
            Box(
                Modifier
                    .size(with(density) { diameter.toDp() })
                    .clip(CircleShape)
                    .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { voice.sendNow() }
                    .semantics {
                        role = Role.Button
                        contentDescription = accessibleOrb
                        onClick { voice.sendNow(); true }
                    },
                contentAlignment = Alignment.Center,
            ) {
                FmIcon(VoiceLaunchRules.symbol(phase), size = with(density) { (diameter * 0.26f).toDp() }, tint = Color.White)
            }
        }
        Column(
            Modifier
                .align(Alignment.TopCenter)
                .padding(top = with(density) { (center.y + diameter * 0.8f).toDp() }, start = 24.dp, end = 24.dp)
                .widthIn(max = 520.dp)
                .graphicsLayer {
                    alpha = copy
                    translationY = (1f - copy) * 18f * density.density
                },
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Transcript(voice, hearing, reduced)
            val line = VoiceLaunchRules.hint(phase, voice.held, voice.heard.isNotEmpty())
            if (line != null) {
                Text(
                    line,
                    Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                    style = Fm.text.bodySmall.copy(color = Color.White.copy(alpha = 0.6f), fontWeight = FontWeight.Medium, textAlign = TextAlign.Center),
                )
            }
        }
        Row(
            Modifier
                .align(Alignment.BottomCenter)
                .windowInsetsPadding(WindowInsets.navigationBars)
                .padding(bottom = 40.dp)
                .graphicsLayer {
                    alpha = copy
                    translationY = (1f - copy) * 40f * density.density
                },
            horizontalArrangement = Arrangement.spacedBy(36.dp),
            verticalAlignment = Alignment.Bottom,
        ) {
            val enabled = phase !is VoiceLaunchPhase.Sending && !away
            VoiceControl("keyboard_command_key", "Commands", false, enabled) { voice.commands() }
            VoiceControl("close", "Cancel", true, enabled) { voice.cancel() }
            VoiceControl("keyboard", "Type", false, enabled) { voice.type() }
        }
    }
}

@Composable
private fun Backdrop(
    voice: ShellVoiceLaunch,
    accent: Color,
    solid: Boolean,
    center: Offset,
    diameter: Float,
    dim: Float,
    reduced: Boolean,
    clock: MutableLongState,
    onTap: () -> Unit,
) {
    Canvas(
        Modifier
            .fillMaxSize()
            .alpha(dim)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onTap),
    ) {
        drawRect(Color.Black.copy(alpha = if (solid) 0.92f else 0.5f))
        val level = voice.loudness
        val breathe = if (reduced) 0f else sin(clock.longValue / 1000.0 * 1.6).toFloat() * 0.04f
        drawRect(
            Brush.radialGradient(
                colors = listOf(accent.copy(alpha = 0.55f + level * 0.35f), accent.copy(alpha = 0.12f), Color.Transparent),
                center = center,
                radius = diameter * (2.1f + breathe + level * 0.9f),
            ),
        )
    }
}

@Composable
private fun Transcript(voice: ShellVoiceLaunch, hearing: Boolean, reduced: Boolean) {
    val style = Fm.text.titleLarge.copy(fontWeight = FontWeight.SemiBold, textAlign = TextAlign.Center, fontSize = 22.sp, lineHeight = 28.sp)
    val live = Modifier.semantics { liveRegion = LiveRegionMode.Polite }
    val missed = voice.missed
    val pulse = if (hearing && !reduced) {
        rememberInfiniteTransition(label = "voice-shimmer")
            .animateFloat(0.4f, 0.8f, infiniteRepeatable(tween(900), RepeatMode.Reverse), label = "voice-pulse").value
    } else {
        0.55f
    }
    when {
        missed != null -> Text(missed, live, style = style.copy(color = Color.White.copy(alpha = 0.9f), fontSize = 20.sp))
        voice.words.isEmpty() -> Text(VoiceLaunchRules.placeholder(voice.phase), live, style = style.copy(color = Color.White.copy(alpha = pulse)))
        else -> Text(voice.words, live, style = style.copy(color = Color.White), maxLines = 7, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun VoiceControl(icon: String, label: String, prominent: Boolean, enabled: Boolean, onTap: () -> Unit) {
    val circle = if (prominent) 64.dp else 52.dp
    Column(
        Modifier
            .alpha(if (enabled) 1f else 0.5f)
            .clickable(enabled = enabled, interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onTap)
            .semantics {
                contentDescription = label
                role = Role.Button
            },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Box(
            Modifier
                .size(circle)
                .clip(CircleShape)
                .background(Color.White.copy(alpha = if (prominent) 0.2f else 0.12f))
                .border(1.dp, Color.White.copy(alpha = 0.18f), CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon(icon, size = if (prominent) 26.dp else 22.dp, tint = Color.White)
        }
        Text(label, style = Fm.text.labelMedium.copy(color = Color.White.copy(alpha = 0.75f)))
    }
}

private fun DrawScope.drawCore(accent: Color, diameter: Float, seconds: Double, level: Float, reduced: Boolean) {
    val c = Offset(size.width / 2f, size.height / 2f)
    val swell = if (reduced) 1f else 1f + level * 0.12f + sin(seconds * 2.1).toFloat() * 0.015f
    val r = diameter / 2f * swell
    val halo = r * (1.5f + level * 0.4f)
    drawCircle(
        Brush.radialGradient(listOf(accent.copy(alpha = 0.6f + level * 0.4f), Color.Transparent), center = c, radius = halo),
        radius = halo,
        center = c,
    )
    drawCircle(
        Brush.verticalGradient(listOf(accent.copy(alpha = 0.95f), accent, accent.copy(alpha = 0.7f)), startY = c.y - r, endY = c.y + r),
        radius = r,
        center = c,
    )
    val aurora = Brush.sweepGradient(listOf(ice.copy(alpha = 0f), ice, rose.copy(alpha = 0f), rose, ice.copy(alpha = 0f)), c)
    rotate(if (reduced) 0f else (seconds * 0.5 * 180.0 / PI).toFloat(), c) {
        drawCircle(aurora, radius = r, center = c, alpha = 0.3f + level * 0.2f, blendMode = BlendMode.Screen)
    }
    drawCircle(
        Brush.radialGradient(listOf(Color.White.copy(alpha = 0.4f), Color.Transparent), center = Offset(c.x, c.y - r * 0.4f), radius = r * 0.9f),
        radius = r,
        center = c,
    )
    if (reduced) return
    for (index in 0 until 14) {
        val n = index.toFloat()
        val orbit = r * (0.3f + ((n * 0.618f + 0.31f) % 1f) * 0.5f)
        val angle = n * GOLDEN + (seconds * 0.12 * (0.4f + (n * 0.7548776f) % 1f)).toFloat()
        val twinkle = sin(seconds * 2.2 + n * 1.37).toFloat() * 0.5f + 0.5f
        val at = Offset(c.x + orbit * cos(angle), c.y + orbit * sin(angle))
        val tip = (1.5f + (n * 0.5698f) % 1f * 1.6f) * density * (1f + level)
        drawPath(starPath(at, tip, 0f), Color.White.copy(alpha = 0.35f + twinkle * 0.6f))
    }
}

private fun DrawScope.drawCorona(
    voice: ShellVoiceLaunch,
    accent: Color,
    diameter: Float,
    seconds: Double,
    level: Float,
    hearing: Boolean,
    nowMs: Long,
    waking: Boolean,
    reduced: Boolean,
) {
    val c = Offset(size.width / 2f, size.height / 2f)
    val radius = diameter / 2f
    val wash = Brush.sweepGradient(listOf(accent, ice, rose, gold, accent), c)
    val fade = if (waking) 0.35f else 1f
    val spin = (seconds * 0.35 * 180.0 / PI).toFloat()
    rotate(spin, c) {
        if (!reduced) {
            for (ring in 0 until 3) {
                val cycle = 2.4 - level * 1.1
                val age = ((seconds / cycle + ring / 3.0) % 1.0).toFloat()
                val reach = radius * (1.02f + age * (0.75f + level * 0.5f))
                drawCircle(
                    wash,
                    radius = reach,
                    center = c,
                    alpha = (1f - age) * (if (hearing) 0.55f else 0.25f) * fade,
                    style = Stroke((1.5f + (1f - age) * 2f) * density),
                )
            }
        }
        drawSpokes(wash, c, radius, voice.levels, seconds, hearing, reduced, fade)
    }
    val since = voice.heardAt
    if (hearing && !voice.held && since != 0L) {
        val fill = VoiceLaunchRules.quietFill(nowMs - since, level >= VoiceMeter.VOICE)
        if (fill > 0f) drawQuietArc(c, radius * 0.98f, fill, 0f)
    }
    if (voice.phase is VoiceLaunchPhase.Sending) {
        drawQuietArc(c, radius * 0.98f, 0.28f, if (reduced) 0f else (seconds * 6 * 180.0 / PI).toFloat())
    }
}

private fun DrawScope.drawSpokes(
    wash: Brush,
    c: Offset,
    radius: Float,
    levels: FloatArray,
    seconds: Double,
    hearing: Boolean,
    reduced: Boolean,
    fade: Float,
) {
    val inner = radius * 1.08f
    for (spoke in 0 until SPOKES) {
        val turn = spoke.toFloat() / SPOKES
        val mirrored = if (turn < 0.5f) turn * 2f else (1f - turn) * 2f
        val index = (mirrored * (levels.size - 1)).toInt().coerceIn(0, levels.size - 1)
        val heard = levels[levels.size - 1 - index]
        val ripple = if (reduced) 0f else sin(seconds * 3 + turn * PI * 8).toFloat() * 0.5f + 0.5f
        val length = radius * (0.05f + (if (hearing) heard * 0.42f else 0f) + ripple * 0.04f)
        val angle = turn * 2f * PI.toFloat() - PI.toFloat() / 2f
        drawLine(
            wash,
            Offset(c.x + cos(angle) * inner, c.y + sin(angle) * inner),
            Offset(c.x + cos(angle) * (inner + length), c.y + sin(angle) * (inner + length)),
            strokeWidth = 2.4f * density,
            cap = StrokeCap.Round,
            alpha = (0.35f + heard * 0.65f) * fade,
            blendMode = BlendMode.Plus,
        )
    }
}

private fun DrawScope.drawQuietArc(center: Offset, radius: Float, upTo: Float, spinDegrees: Float) {
    drawArc(
        Color.White.copy(alpha = 0.9f),
        startAngle = spinDegrees - 90f,
        sweepAngle = 360f * upTo,
        useCenter = false,
        topLeft = Offset(center.x - radius, center.y - radius),
        size = Size(radius * 2f, radius * 2f),
        style = Stroke(3.5f * density, cap = StrokeCap.Round),
    )
}

@Composable
private fun Burst(sentAt: Long, center: Offset, diameter: Float, accent: Color, clock: MutableLongState) {
    Canvas(Modifier.fillMaxSize()) {
        val age = (clock.longValue - sentAt) / BURST_MS
        if (age < 0f || age >= 1f) return@Canvas
        val ease = 1f - (1f - age).pow(3)
        val radius = diameter / 2f
        val wave = radius * (0.6f + ease * 2.2f)
        drawCircle(Color.White, radius = wave, center = center, alpha = 1f - age, style = Stroke((6f * (1f - age) + 1f) * density))
        val flash = radius * (0.8f + ease * 0.6f)
        drawCircle(
            Brush.radialGradient(listOf(Color.White, accent.copy(alpha = 0.6f), accent.copy(alpha = 0f)), center = center, radius = flash),
            radius = flash,
            center = center,
            alpha = (0.7f - age * 1.6f).coerceIn(0f, 1f),
        )
        val palette = listOf(Color.White, accent, gold, rose)
        for (spark in 0 until SPARKS) {
            val n = spark.toFloat()
            val angle = n * GOLDEN
            val speed = 0.9f + (n * 0.618f % 1f) * 1.4f
            val reach = radius * (0.4f + ease * 2.4f * speed)
            val at = Offset(center.x + cos(angle) * reach, center.y + sin(angle) * reach)
            val tip = (2 + spark % 4) * (1f - age * 0.7f) * density * 1.6f
            drawPath(starPath(at, tip, 0f), palette[spark % palette.size].copy(alpha = 1f - age))
        }
    }
}

private fun starPath(center: Offset, radius: Float, spin: Float): Path {
    val path = Path()
    val inner = radius * 0.32f
    for (index in 0 until 8) {
        val angle = spin + index * PI.toFloat() / 4f
        val r = if (index % 2 == 0) radius else inner
        val x = center.x + r * cos(angle)
        val y = center.y + r * sin(angle)
        if (index == 0) path.moveTo(x, y) else path.lineTo(x, y)
    }
    path.close()
    return path
}
