package com.forkmesh.nativeapp.shell

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalView
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.LocalFmReducedMotion
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import kotlinx.coroutines.Job
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.random.Random

object LaunchFlight {
    const val DURATION_MS = 1200f
    const val GHOST_MS = 950f
    const val SHRINK_MS = 320f
    const val BURST_MS = 450f
    private const val HOLD_MS = 12_000L

    class Ghost(val image: ImageBitmap, val bounds: Rect)

    class Flight(val id: Long, val count: Int, val seed: Float, val message: String?, val from: Rect?, val ghost: Ghost?) {
        val duration: Float = if (ghost == null) DURATION_MS else GHOST_MS
        var startNanos: Long = 0L
    }

    var source: Rect? = null
    var card: Rect? = null
    var capture: (suspend () -> ImageBitmap?)? = null
    private var ghost: Ghost? = null
    private var snapping: Job? = null
    var target by mutableStateOf(Rect.Zero)
    var charged by mutableStateOf<Rect?>(null)
        private set
    val flights = mutableStateListOf<Flight>()
    var floor by mutableIntStateOf(0)
        private set
    var landings by mutableIntStateOf(0)
        private set
    private var reserved = 0
    private var nextId = 0L
    private val scope = MainScope()

    fun badge(running: Int): Int = max(running, floor)

    fun charge() {
        charged = source
        ghost = null
        snapping?.cancel()
        val take = capture ?: return
        val bounds = card ?: return
        if (bounds.isEmpty) return
        snapping = scope.launch {
            ghost = runCatching { take() }.getOrNull()?.let { Ghost(it, bounds) }
        }
    }

    fun discharge() {
        charged = null
        ghost = null
        snapping?.cancel()
        snapping = null
    }

    fun launch(running: Int, message: String?, fromComposer: Boolean = true) {
        reserved = max(reserved, badge(running)) + 1
        val from = if (fromComposer) charged ?: source else null
        val shot = if (fromComposer) ghost else null
        charged = null
        ghost = null
        flights.add(Flight(nextId++, reserved, Random.nextFloat() * 2f * PI.toFloat(), message, from, shot))
    }

    internal fun land(flight: Flight) {
        floor = max(floor, flight.count)
        landings += 1
        val landing = landings
        scope.launch {
            delay(HOLD_MS)
            if (landings == landing && flights.isEmpty()) {
                floor = 0
                reserved = 0
            }
        }
    }

    internal fun remove(flight: Flight) {
        flights.removeAll { it.id == flight.id }
    }
}

@Composable
fun LaunchSparkles(modifier: Modifier = Modifier) {
    val reduced = LocalFmReducedMotion.current
    val view = LocalView.current
    val accent = Fm.colors.accent
    var origin by remember { mutableStateOf(Offset.Zero) }
    var now by remember { mutableStateOf(0L) }
    var chargeStart by remember { mutableStateOf(0L) }
    val charged = LaunchFlight.charged

    LaunchedEffect(charged, reduced) {
        chargeStart = 0L
        if (charged == null || reduced) return@LaunchedEffect
        chargeStart = withFrameNanos { it }
        while (true) {
            now = withFrameNanos { it }
        }
    }

    LaunchFlight.flights.toList().forEach { flight ->
        key(flight.id) {
            LaunchedEffect(Unit) {
                flight.message?.let { view.announceForAccessibility(it) }
                if (reduced || LaunchFlight.target == Rect.Zero) {
                    LaunchFlight.land(flight)
                    LaunchFlight.remove(flight)
                    return@LaunchedEffect
                }
                flight.startNanos = withFrameNanos { it }
                var landed = false
                while (true) {
                    val frame = withFrameNanos { it }
                    now = frame
                    val elapsed = (frame - flight.startNanos) / 1_000_000f
                    if (!landed && elapsed >= flight.duration) {
                        landed = true
                        LaunchFlight.land(flight)
                    }
                    if (elapsed >= flight.duration + LaunchFlight.BURST_MS) break
                }
                LaunchFlight.remove(flight)
            }
        }
    }

    Box(
        modifier
            .fillMaxSize()
            .onGloballyPositioned { origin = it.boundsInRoot().topLeft }
            .drawBehind {
                val frame = now
                val target = LaunchFlight.target
                if (target == Rect.Zero) return@drawBehind
                val to = target.center - origin
                val charging = LaunchFlight.charged
                if (charging != null && chargeStart != 0L) {
                    drawCharge(charging.center - origin, (frame - chargeStart) / 1_000_000f, accent)
                }
                LaunchFlight.flights.forEach { flight ->
                    if (flight.startNanos == 0L) return@forEach
                    val from = flight.from?.let { it.center - origin }
                        ?: Offset(size.width / 2f, to.y - size.height * 0.4f)
                    drawFlight(flight, (frame - flight.startNanos) / 1_000_000f, from, to, accent, origin)
                }
            },
    )
}

private val sparkleGold = Color(0xFFFFD54D)
private val sparklePink = Color(0xFFFF73BF)

private fun eased(t: Float): Float = if (t < 0.5f) 4f * t * t * t else 1f - (-2f * t + 2f).pow(3) / 2f

private fun curve(from: Offset, to: Offset, t: Float, unit: Float): Offset {
    val top = min(from.y, to.y)
    val lift = min(360f * unit, max(160f * unit, (top - 120f * unit) / 0.75f))
    val lean = if (abs(to.x - from.x) < 60f * unit) (if (to.x < from.x) 60f else -60f) * unit else 0f
    val rise = Offset(from.x + lean, top - lift)
    val fall = Offset(to.x - lean, top - lift)
    val a = (1 - t) * (1 - t) * (1 - t)
    val b = 3 * (1 - t) * (1 - t) * t
    val c = 3 * (1 - t) * t * t
    val d = t * t * t
    return Offset(a * from.x + b * rise.x + c * fall.x + d * to.x, a * from.y + b * rise.y + c * fall.y + d * to.y)
}

private fun star(center: Offset, radius: Float, spin: Float): Path {
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

private fun DrawScope.drawCharge(center: Offset, elapsed: Float, accent: Color) {
    val colors = listOf(accent, sparkleGold, sparklePink, Color.White)
    val unit = density
    val seconds = elapsed / 1000f
    val grow = min(1f, elapsed / 220f)
    val wave = min(1f, elapsed / 500f)
    drawCircle(accent.copy(alpha = 0.5f * (1f - wave)), radius = (18f + 26f * wave) * unit, center = center, style = Stroke(width = 2f * unit))
    for (index in 0 until 6) {
        val angle = seconds * 5f + index * (2f * PI.toFloat() / 6f)
        val orbit = 22f * unit * grow
        val at = Offset(center.x + orbit * cos(angle), center.y + orbit * sin(angle))
        drawPath(star(at, 3f * unit * grow, angle), colors[index % colors.size].copy(alpha = 0.85f))
    }
    val pulse = (1f + 0.2f * sin(seconds * 30f)) * grow
    drawCircle(accent.copy(alpha = 0.22f), radius = 14f * unit * grow, center = center)
    drawPath(star(center, 11f * unit * pulse, seconds * 4f), sparkleGold)
    drawPath(star(center, 5f * unit * pulse, seconds * 4f), Color.White)
}

private class GhostPlace(val rect: Rect, val radius: Float, val travel: Float)

private fun ghostPlace(ghost: LaunchFlight.Ghost, elapsed: Float, origin: Offset, to: Offset, unit: Float): GhostPlace {
    val bounds = ghost.bounds
    val width = max(bounds.width, 1f)
    val small = min(0.3f, 120f * unit / width)
    val end = 14f * unit / width
    val from = bounds.center - origin
    var scale = 1f
    var center = from
    var travel = 0f
    if (elapsed < LaunchFlight.SHRINK_MS) {
        val u = elapsed / LaunchFlight.SHRINK_MS
        val eased = 1f - (1f - u) * (1f - u) * (1f - u)
        scale = 1f + (small - 1f) * eased
    } else {
        travel = min(1f, (elapsed - LaunchFlight.SHRINK_MS) / (LaunchFlight.GHOST_MS - LaunchFlight.SHRINK_MS))
        val u = travel * travel
        val bend = Offset(from.x + (to.x - from.x) * 0.85f, from.y)
        center = Offset(
            (1 - u) * (1 - u) * from.x + 2 * (1 - u) * u * bend.x + u * u * to.x,
            (1 - u) * (1 - u) * from.y + 2 * (1 - u) * u * bend.y + u * u * to.y,
        )
        scale = small + (end - small) * u
    }
    val size = Size(bounds.width * scale, bounds.height * scale)
    val rect = Rect(Offset(center.x - size.width / 2f, center.y - size.height / 2f), size)
    val radius = min(size.height / 2f, 28f * unit * scale + (size.height / 2f - 28f * unit * scale) * travel)
    return GhostPlace(rect, radius, travel)
}

private fun DrawScope.drawGhost(flight: LaunchFlight.Flight, ghost: LaunchFlight.Ghost, elapsed: Float, origin: Offset, to: Offset, accent: Color) {
    val colors = listOf(accent, sparkleGold, sparklePink, Color.White)
    val unit = density
    val seconds = elapsed / 1000f
    val place = ghostPlace(ghost, elapsed, origin, to, unit)
    if (place.travel > 0f) {
        for (step in 12 downTo 1) {
            val lag = elapsed - step * 30f
            if (lag <= LaunchFlight.SHRINK_MS) continue
            val p = ghostPlace(ghost, lag, origin, to, unit).rect.center
            val fade = 1f - step / 13f
            val wobble = sin(flight.seed + step * 1.7f + seconds * 18f)
            val at = Offset(p.x + wobble * 6f * unit * (1 - fade), p.y + cos(flight.seed + step) * 5f * unit * (1 - fade))
            drawPath(star(at, (2f + 4f * fade) * unit, seconds * 6f + step), colors[step % colors.size].copy(alpha = fade * 0.85f))
        }
    }
    val glow = (6f + 10f * place.travel) * unit
    val halo = place.rect.inflate(glow / 2f)
    drawPath(
        Path().apply { addRoundRect(RoundRect(halo, CornerRadius(place.radius + glow / 2f))) },
        accent.copy(alpha = (0.18f + 0.2f * place.travel) * min(1f, place.travel * 4f)),
    )
    drawImage(
        ghost.image,
        dstOffset = IntOffset(place.rect.left.toInt(), place.rect.top.toInt()),
        dstSize = IntSize(max(1, place.rect.width.toInt()), max(1, place.rect.height.toInt())),
        alpha = 1f - 0.5f * max(0f, (place.travel - 0.6f) / 0.4f),
    )
}

private fun DrawScope.drawFlight(flight: LaunchFlight.Flight, elapsed: Float, from: Offset, to: Offset, accent: Color, origin: Offset) {
    val colors = listOf(accent, sparkleGold, sparklePink, Color.White)
    val unit = density
    val seconds = elapsed / 1000f
    val raw = elapsed / flight.duration
    val ghost = flight.ghost
    if (ghost != null) {
        if (raw < 1f) {
            drawGhost(flight, ghost, elapsed, origin, to, accent)
            return
        }
    } else if (flight.from != null && elapsed < 400f) {
        val puff = elapsed / 400f
        val spread = (22f + 30f * puff) * unit
        for (index in 0 until 6) {
            val angle = flight.seed + index * (2f * PI.toFloat() / 6f)
            val at = Offset(from.x + spread * cos(angle), from.y + spread * sin(angle))
            drawPath(star(at, (3f * (1f - puff) + 0.5f) * unit, angle + puff * 4f), colors[index % colors.size].copy(alpha = 0.85f * (1f - puff)))
        }
        drawCircle(accent.copy(alpha = 0.4f * (1f - puff)), radius = spread, center = from, style = Stroke(width = 2f * unit))
    }
    if (raw < 1f) {
        for (step in 16 downTo 1) {
            val lag = raw - step * 0.035f
            if (lag <= 0f) continue
            val p = curve(from, to, eased(lag), unit)
            val fade = 1f - step / 17f
            val wobble = sin(flight.seed + step * 1.7f + seconds * 18f)
            val jitter = Offset(p.x + wobble * 6f * unit * (1 - fade), p.y + cos(flight.seed + step) * 5f * unit * (1 - fade))
            drawPath(star(jitter, (2f + 5f * fade) * unit, seconds * 6f + step), colors[step % colors.size].copy(alpha = fade * 0.9f))
        }
        val head = curve(from, to, eased(raw), unit)
        val pulse = 1f + 0.2f * sin(seconds * 30f)
        drawCircle(accent.copy(alpha = 0.22f), radius = 14f * unit, center = head)
        drawPath(star(head, 11f * unit * pulse, seconds * 4f), sparkleGold)
        drawPath(star(head, 5f * unit * pulse, seconds * 4f), Color.White)
        return
    }
    val burst = min(1f, (elapsed - flight.duration) / LaunchFlight.BURST_MS)
    val ring = (10f + 34f * burst) * unit
    for (index in 0 until 10) {
        val angle = flight.seed + index * (2f * PI.toFloat() / 10f)
        val at = Offset(to.x + ring * cos(angle), to.y + ring * sin(angle))
        drawPath(star(at, (5f * (1 - burst) + 1f) * unit, angle + burst * 3f), colors[index % colors.size].copy(alpha = 1f - burst))
    }
    drawCircle(accent.copy(alpha = 0.35f * (1f - burst)), radius = ring, center = to, style = Stroke(width = 2f * unit))
}
