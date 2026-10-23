package com.forkmesh.nativeapp.feature.agents

import android.os.Handler
import android.os.Looper
import android.provider.Settings
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.agents.AgentChipFacts
import com.forkmesh.nativeapp.agents.AgentEagleFeed
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentLineChips
import com.forkmesh.nativeapp.agents.AgentReadState
import com.forkmesh.nativeapp.agentwatch.ControlWatchFeeds
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.math.max
import kotlin.math.min
import kotlin.math.cos
import kotlin.math.PI
import com.forkmesh.nativeapp.agents.AgentLineStatus
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.PathMeasure
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.animation.core.tween
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.LinearEasing

internal sealed class AgentEagleSource {
    abstract val moving: Boolean

    data class Control(val row: UnifiedAgentRow) : AgentEagleSource() {
        override val moving: Boolean get() = row.isActive
    }

    data class Desktop(val repoName: String, val sessionId: Int, override val moving: Boolean) : AgentEagleSource()

    data class Launched(val key: String, val session: AgentLaunchSession) : AgentEagleSource() {
        override val moving: Boolean get() = session.isActive
    }
}

@Stable
internal class AgentEagleState {
    private val feed = AgentEagleFeed()
    private val lines = mutableStateMapOf<String, String>()
    private var rowsVersion by mutableIntStateOf(0)
    private val main = Handler(Looper.getMainLooper())

    init {
        feed.onChange = { main.post { sync() } }
    }

    fun follow(rows: List<UnifiedAgentRow>) {
        val control = AgentsFeature.control
        feed.follow(rows, { ControlWatchFeeds.forControl(control) }) { row -> control.transcript(row, 0).text }
        rowsVersion++
        sync()
    }

    fun stop() {
        feed.stop()
        lines.clear()
        rowsVersion++
    }

    private fun sync() {
        val now = feed.lines
        for ((id, text) in now) if (lines[id] != text) lines[id] = text
        lines.keys.retainAll(now.keys)
    }

    private fun rowId(source: AgentEagleSource): String? = when (source) {
        is AgentEagleSource.Control -> source.row.id
        is AgentEagleSource.Desktop -> {
            rowsVersion
            feed.desktopRowId(source.repoName, source.sessionId)
        }
        is AgentEagleSource.Launched -> null
    }

    fun text(source: AgentEagleSource): String = when (source) {
        is AgentEagleSource.Launched -> feed.launchedDialog(source.key, source.session)
        else -> rowId(source)?.let { lines[it] } ?: ""
    }

    fun recall(source: AgentEagleSource) {
        val id = rowId(source) ?: return
        feed.recall(id, source.moving, (source as? AgentEagleSource.Control)?.row)
    }

    companion object {
        fun watchKey(rows: List<UnifiedAgentRow>): String = "eagle:" + rows.joinToString(",") { if (it.isActive) "+" + it.id else it.id }
    }
}

internal val LocalAgentEagle = staticCompositionLocalOf<AgentEagleState?> { null }

@Composable
internal fun AgentEagleRow(
    title: String,
    facts: AgentChipFacts,
    onTap: () -> Unit,
    unread: Boolean?,
    eagle: AgentEagleSource?,
    trailing: (@Composable () -> Unit)?,
) {
    val c = Fm.colors
    val rule = c.border
    Row(
        Modifier
            .fillMaxWidth()
            .drawBehind {
                val stroke = 0.5.dp.toPx()
                drawLine(rule, Offset(0f, 0f), Offset(size.width, 0f), stroke)
                drawLine(rule, Offset(0f, size.height), Offset(size.width, size.height), stroke)
            }
            .clickable(onClick = onTap)
            .semantics {
                if (unread == true) stateDescription = AgentReadState.UNREAD_LABEL
                contentDescription = listOf(title, AgentLineChips.eagleSummary(facts)).filter { it.isNotEmpty() }.joinToString(", ")
            }
            .padding(horizontal = 7.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                title,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis),
            )
            Box(Modifier.fillMaxWidth().height(12.dp)) { AgentEagleLine(eagle) }
        }
        trailing?.invoke()
        AgentEagleTile(facts, unread == true)
    }
}

@Composable
private fun AgentEagleLine(source: AgentEagleSource?) {
    val state = LocalAgentEagle.current
    if (state == null || source == null) {
        AgentEagleStrip("", moving = false)
        return
    }
    LaunchedEffect(source, source.moving) { state.recall(source) }
    AgentEagleStrip(state.text(source), moving = source.moving)
}

private object EagleTile {
    val SIDE = 32.dp
    val RING = 2.dp
    val RADIUS = 8.dp
    const val TASKS_SHOWN = 3
    const val BLINK_MS = 1400
}

@Composable
private fun AgentEagleTile(facts: AgentChipFacts, unread: Boolean) {
    val c = Fm.colors
    val share = AgentLineChips.churnShare(facts.linesAdded, facts.linesRemoved)
    val added = c.success
    val removed = c.danger
    val hairline = c.border
    Box(Modifier.size(EagleTile.SIDE)) {
        Canvas(Modifier.size(EagleTile.SIDE)) {
            val radius = EagleTile.RADIUS.toPx()
            drawRoundRect(c.bgOverlay, cornerRadius = CornerRadius(radius))
            if (share == null) {
                val stroke = 0.5.dp.toPx()
                drawRoundRect(
                    hairline,
                    topLeft = Offset(stroke / 2, stroke / 2),
                    size = Size(size.width - stroke, size.height - stroke),
                    cornerRadius = CornerRadius(radius - stroke / 2),
                    style = Stroke(stroke),
                )
            } else {
                val width = EagleTile.RING.toPx()
                val inset = width / 2
                val path = Path().apply {
                    addRoundRect(RoundRect(inset, inset, size.width - inset, size.height - inset, CornerRadius(radius - inset)))
                }
                val measure = PathMeasure().apply { setPath(path, false) }
                val length = measure.length
                val split = length * share
                if (split > 0f) {
                    val green = Path()
                    measure.getSegment(0f, split, green, true)
                    drawPath(green, added, style = Stroke(width))
                }
                if (split < length) {
                    val red = Path()
                    measure.getSegment(split, length, red, true)
                    drawPath(red, removed, style = Stroke(width))
                }
            }
        }
        AgentTilePill(facts, Modifier.align(Alignment.TopEnd).padding(3.dp))
        if (facts.backgroundTasks > 0) {
            AgentTileTasks(facts.backgroundTasks, facts.status == AgentLineStatus.Running || facts.status == AgentLineStatus.Waiting, Modifier.align(Alignment.BottomStart).padding(4.dp))
        }
        if (unread) {
            Box(
                Modifier
                    .align(Alignment.TopStart)
                    .offset(x = (-2).dp, y = (-2).dp)
                    .size(8.dp)
                    .background(c.accent, RoundedCornerShape(50)),
            )
        }
    }
}

@Composable
private fun AgentTilePill(facts: AgentChipFacts, modifier: Modifier) {
    val c = Fm.colors
    val tint = when (facts.status) {
        AgentLineStatus.Running -> c.accent
        AgentLineStatus.Waiting -> c.warning
        AgentLineStatus.Success -> c.success
        AgentLineStatus.Merged -> Color(0xFFA371F7)
        AgentLineStatus.Failed -> c.danger
        AgentLineStatus.Queued, AgentLineStatus.Stopped, AgentLineStatus.Other -> c.textTertiary
    }
    val files = facts.filesChanged >= 0
    Row(
        modifier
            .height(10.dp)
            .background(Color.Black.copy(alpha = 0.45f), RoundedCornerShape(50))
            .padding(horizontal = if (files) 3.dp else 2.5.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        if (files) {
            Text(
                AgentLineChips.compactCount(facts.filesChanged),
                maxLines = 1,
                softWrap = false,
                style = TextStyle(fontSize = 7.sp, lineHeight = 8.sp, fontWeight = FontWeight.Bold, color = Color.White, fontFeatureSettings = "tnum"),
            )
        }
        Box(Modifier.size(5.dp).background(tint, RoundedCornerShape(50)))
    }
}

@Composable
private fun AgentTileTasks(count: Int, active: Boolean, modifier: Modifier) {
    val blinking = active && !Fm.reducedMotion
    val glow = if (blinking) {
        val phase by rememberInfiniteTransition(label = "eagle-tasks").animateFloat(
            initialValue = 0f,
            targetValue = 1f,
            animationSpec = infiniteRepeatable(tween(EagleTile.BLINK_MS, easing = LinearEasing), RepeatMode.Restart),
            label = "eagle-tasks-phase",
        )
        0.6f + 0.4f * cos(phase * 2 * PI.toFloat())
    } else {
        1f
    }
    Row(
        modifier
            .height(7.dp)
            .graphicsLayer { alpha = glow }
            .background(Color.Black.copy(alpha = 0.45f), RoundedCornerShape(50))
            .padding(horizontal = 2.5.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(1.5.dp),
    ) {
        repeat(min(count, EagleTile.TASKS_SHOWN)) {
            Box(Modifier.size(3.5.dp).background(Color.White, RoundedCornerShape(50)))
        }
        if (count > EagleTile.TASKS_SHOWN) {
            Text(
                "+${count - EagleTile.TASKS_SHOWN}",
                maxLines = 1,
                softWrap = false,
                style = TextStyle(fontSize = 6.5.sp, lineHeight = 7.sp, fontWeight = FontWeight.Bold, color = Color.White, fontFeatureSettings = "tnum"),
            )
        }
    }
}

private object EagleStrip {
    const val SPEED_DP = 80f
    const val GAP_DP = 5f
    const val HOLD_NANOS = 500_000_000L
    const val MAX_CARS = 60
}

@Composable
internal fun AgentEagleStrip(dialog: String, moving: Boolean, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val context = LocalContext.current
    val density = LocalDensity.current
    val reduceMotion = remember {
        Settings.Global.getFloat(context.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
    }
    val measurer = rememberTextMeasurer()
    val style = TextStyle(fontSize = 10.sp, fontWeight = FontWeight.Medium, color = c.textPrimary)
    val layouts = remember(style) { HashMap<String, TextLayoutResult>() }
    val speed = with(density) { EagleStrip.SPEED_DP.dp.toPx() }
    val gap = with(density) { EagleStrip.GAP_DP.dp.toPx() }
    var train by remember { mutableStateOf(emptyList<String>()) }
    var cycleStart by remember { mutableLongStateOf(Long.MAX_VALUE) }
    var frame by remember { mutableLongStateOf(0L) }
    var lane by remember { mutableFloatStateOf(0f) }

    fun layout(text: String): TextLayoutResult = layouts.getOrPut(text) {
        measurer.measure(text, style, overflow = TextOverflow.Clip, softWrap = false, maxLines = 1)
    }

    fun width(text: String): Float = layout(text).size.width.toFloat()

    fun scrolls(): Boolean = moving && !reduceMotion && train.isNotEmpty()

    fun distance(): Float = (train.firstOrNull()?.let(::width) ?: 0f) + gap

    fun elapsedSeconds(at: Long): Float = if (cycleStart == Long.MAX_VALUE) 0f else (at - cycleStart) / 1e9f

    fun offset(at: Long): Float = if (!scrolls()) 0f else -min(max(elapsedSeconds(at), 0f) * speed, distance())

    fun cars(): List<String> {
        if (!scrolls()) return train.take(1)
        val out = train.toMutableList()
        val last = train.last()
        if (width(last) <= 0f) return out
        var span = train.sumOf { (width(it) + gap).toDouble() }.toFloat()
        val need = distance() + lane
        while (span < need && out.size < EagleStrip.MAX_CARS) {
            out += last
            span += width(last) + gap
        }
        return out
    }

    fun prune() {
        layouts.keys.retainAll(train.toSet())
    }

    fun offer(next: String) {
        val now = System.nanoTime()
        if (next.isEmpty()) {
            train = emptyList()
            cycleStart = Long.MAX_VALUE
            prune()
            return
        }
        if (next == train.lastOrNull()) return
        if (train.isEmpty() || !scrolls()) {
            train = listOf(next)
            cycleStart = now + EagleStrip.HOLD_NANOS
            prune()
            return
        }
        val current = cars()
        var x = offset(now)
        var cut = current.size
        for ((index, text) in current.withIndex()) {
            if (index > 0 && x > lane) {
                cut = index
                break
            }
            x += width(text) + gap
        }
        train = current.take(cut) + next
        prune()
    }

    fun wrap(at: Long) {
        val over = max(elapsedSeconds(at) - distance() / speed, 0f)
        var rest = cars().drop(1)
        if (rest.isEmpty()) rest = train.take(1)
        val next = rest.firstOrNull() ?: return
        val trimmed = rest.toMutableList()
        while (trimmed.size > 1 && trimmed[trimmed.size - 1] == trimmed[trimmed.size - 2]) trimmed.removeAt(trimmed.size - 1)
        val lap = max((width(next) + gap) / speed, 0.1f)
        train = trimmed
        prune()
        cycleStart = at - ((over % lap) * 1e9f).toLong()
    }

    LaunchedEffect(dialog) { offer(dialog) }
    LaunchedEffect(moving) {
        if (!moving) train.lastOrNull()?.let { train = listOf(it) }
        prune()
        cycleStart = System.nanoTime() + EagleStrip.HOLD_NANOS
    }
    val scrolling = scrolls()
    LaunchedEffect(scrolling) {
        if (!scrolling) return@LaunchedEffect
        while (true) {
            withFrameNanos { at ->
                frame = at
                if (elapsedSeconds(at) * speed >= distance()) wrap(at)
            }
        }
    }

    Canvas(
        modifier
            .fillMaxWidth()
            .height(12.dp)
            .onSizeChanged { lane = it.width.toFloat() }
            .clipToBounds()
            .graphicsLayer(compositingStrategy = CompositingStrategy.Offscreen)
            .semantics { if (dialog.isNotEmpty()) contentDescription = dialog },
    ) {
        if (train.isEmpty()) return@Canvas
        var x = offset(frame)
        for (car in cars()) {
            val text = layout(car)
            drawText(text, topLeft = Offset(x, (size.height - text.size.height) / 2f))
            x += text.size.width + gap
            if (x > size.width) break
        }
        drawRect(
            Brush.horizontalGradient(
                0f to Color.Black.copy(alpha = 0.35f),
                0.02f to Color.Black,
                0.95f to Color.Black,
                1f to Color.Transparent,
            ),
            blendMode = BlendMode.DstIn,
        )
    }
}
