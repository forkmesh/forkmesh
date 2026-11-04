package com.forkmesh.nativeapp.feature.reticulum

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameMillis
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Fill
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.reticulum.DestinationRecord
import com.forkmesh.nativeapp.reticulum.MapGroup
import com.forkmesh.nativeapp.reticulum.ReticulumFormat
import com.forkmesh.nativeapp.reticulum.ReticulumNode
import com.forkmesh.nativeapp.reticulum.Rns
import com.forkmesh.nativeapp.reticulum.TrafficSample
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.sin

@Immutable
class ReticulumPalette(
    val dark: Boolean,
    val surface: Color,
    val raised: Color,
    val border: Color,
    val grid: Color,
    val text: Color,
    val muted: Color,
    val accent: Color,
    val good: Color,
    val warning: Color,
    val critical: Color,
) {
    fun series(slot: Int): Color = (if (dark) DARK_SERIES else LIGHT_SERIES)[slot.coerceIn(0, 5)]

    fun group(group: MapGroup): Color = when (group) {
        MapGroup.Peer -> series(0)
        MapGroup.Node -> series(1)
        MapGroup.Propagation -> series(2)
        MapGroup.Transport -> series(3)
        MapGroup.Other -> if (dark) Color(0xFF6E7681) else Color(0xFF8C959F)
    }

    fun tone(tone: Int): Color = when (tone) {
        1 -> good
        2 -> warning
        3 -> critical
        else -> muted
    }

    private companion object {
        val DARK_SERIES = listOf(Color(0xFF3987E5), Color(0xFFD95926), Color(0xFF199E70), Color(0xFFC98500), Color(0xFFD55181), Color(0xFF008300))
        val LIGHT_SERIES = listOf(Color(0xFF2A78D6), Color(0xFFEB6834), Color(0xFF1BAF7A), Color(0xFFEDA100), Color(0xFFE87BA4), Color(0xFF008300))
    }
}

@Composable
fun reticulumPalette(): ReticulumPalette {
    val c = Fm.colors
    return remember(c) {
        ReticulumPalette(
            dark = c.dark,
            surface = c.bgBase,
            raised = c.bgRaised,
            border = c.border,
            grid = if (c.dark) Color(0xFF262C36) else Color(0xFFD8DEE4),
            text = c.textPrimary,
            muted = c.textSecondary,
            accent = c.accent,
            good = c.success,
            warning = c.warning,
            critical = c.danger,
        )
    }
}

private fun hashFraction(hash: ByteArray, offset: Int): Double {
    var value = 0L
    for (i in 0 until 4) value = (value shl 8) or (if (offset + i < hash.size) hash[offset + i].toLong() and 0xff else 0L)
    return value / 4294967296.0
}

private fun niceCeiling(value: Double): Double {
    if (value <= 0) return 1.0
    val exponent = 10.0.pow(floor(log10(value)))
    val fraction = value / exponent
    val nice = when {
        fraction <= 1 -> 1.0
        fraction <= 2 -> 2.0
        fraction <= 5 -> 5.0
        else -> 10.0
    }
    return nice * exponent
}

fun DrawScope.groupMarker(group: MapGroup, center: Offset, radius: Float, fill: Color, ring: Color) {
    val path = Path()
    when (group) {
        MapGroup.Peer -> path.addOval(Rect(center, radius))
        MapGroup.Node -> path.addRoundRect(androidx.compose.ui.geometry.RoundRect(Rect(center.x - radius * 0.9f, center.y - radius * 0.9f, center.x + radius * 0.9f, center.y + radius * 0.9f), CornerRadius(1.5f * density)))
        MapGroup.Propagation -> {
            path.moveTo(center.x, center.y - radius * 1.2f)
            path.lineTo(center.x + radius * 1.2f, center.y)
            path.lineTo(center.x, center.y + radius * 1.2f)
            path.lineTo(center.x - radius * 1.2f, center.y)
            path.close()
        }
        MapGroup.Transport -> {
            path.moveTo(center.x, center.y - radius * 1.25f)
            path.lineTo(center.x + radius * 1.15f, center.y + radius * 0.85f)
            path.lineTo(center.x - radius * 1.15f, center.y + radius * 0.85f)
            path.close()
        }
        MapGroup.Other -> path.addOval(Rect(center, radius * 0.75f))
    }
    drawPath(path, fill, style = Fill)
    drawPath(path, ring, style = Stroke(width = 1.5f * density))
}

@Composable
fun GroupMarker(group: MapGroup, modifier: Modifier = Modifier) {
    val palette = reticulumPalette()
    Canvas(modifier.size(12.dp)) {
        groupMarker(group, center, 4.dp.toPx(), palette.group(group), palette.surface)
    }
}

@Composable
fun StatTile(label: String, value: String, detail: String, tone: Int = 0, spark: List<Double> = emptyList(), modifier: Modifier = Modifier) {
    val palette = reticulumPalette()
    val shape = RoundedCornerShape(FmRadius.lg)
    Box(
        modifier
            .width(168.dp)
            .height(92.dp)
            .background(palette.raised, shape)
            .border(1.dp, palette.border, shape)
            .semantics { contentDescription = "$label: $value, $detail" }
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
    ) {
        if (spark.size > 1) {
            Canvas(Modifier.align(Alignment.CenterEnd).padding(top = 6.dp).width(68.dp).height(30.dp)) {
                val maximum = max(1e-9, spark.max())
                val line = Path()
                spark.forEachIndexed { index, sample ->
                    val point = Offset(size.width * index / (spark.size - 1), size.height - (size.height * sample / maximum).toFloat())
                    if (index == 0) line.moveTo(point.x, point.y) else line.lineTo(point.x, point.y)
                }
                val area = Path().apply {
                    addPath(line)
                    lineTo(size.width, size.height)
                    lineTo(0f, size.height)
                    close()
                }
                drawPath(area, palette.series(0).copy(alpha = 0.16f))
                drawPath(line, palette.series(0), style = Stroke(width = 1.5.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
            }
        }
        Column(Modifier.fillMaxWidth()) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (tone != 0) {
                    Box(Modifier.size(8.dp).background(palette.tone(tone), CircleShape))
                    Spacer(Modifier.width(6.dp))
                }
                Text(label, style = Fm.text.labelSmall.copy(color = palette.muted), maxLines = 1)
            }
            Spacer(Modifier.height(4.dp))
            Text(
                value,
                style = Fm.text.titleMedium.copy(color = palette.text, fontWeight = FmType.emphasis, fontSize = 19.sp, fontFeatureSettings = FmType.tabular),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = if (spark.size > 1) Modifier.fillMaxWidth(0.55f) else Modifier,
            )
            Spacer(Modifier.height(2.dp))
            Text(detail, style = Fm.text.labelSmall.copy(color = palette.muted), maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
fun TrafficChart(samples: List<TrafficSample>, title: String = "Traffic, last 3 minutes", modifier: Modifier = Modifier) {
    val palette = reticulumPalette()
    val measurer = rememberTextMeasurer()
    var hover by remember { mutableStateOf(-1) }
    val rxTotal = samples.sumOf { it.rx }
    val txTotal = samples.sumOf { it.tx }
    val description = "Received ${ReticulumFormat.bytes(rxTotal.toDouble())} and sent ${ReticulumFormat.bytes(txTotal.toDouble())} in the last ${samples.size} seconds"
    val small = TextStyle(fontSize = 11.sp, color = palette.muted)
    Canvas(
        modifier
            .fillMaxWidth()
            .height(200.dp)
            .semantics { contentDescription = "$title. $description" }
            .pointerInput(samples.size) {
                fun pick(x: Float) {
                    val left = 58.dp.toPx()
                    val width = size.width - left - 12.dp.toPx()
                    if (samples.isEmpty() || width <= 0) {
                        hover = -1
                        return
                    }
                    val slots = ReticulumNode.HISTORY_SECONDS
                    val offset = slots - samples.size
                    val slot = (x - left) / width * (slots - 1)
                    hover = (slot.roundToInt() - offset).coerceIn(0, samples.size - 1)
                }
                detectDragGestures(
                    onDragStart = { pick(it.x) },
                    onDragEnd = { hover = -1 },
                    onDragCancel = { hover = -1 },
                ) { change, _ -> pick(change.position.x) }
            }
            .pointerInput(samples.size) {
                detectTapGestures(onPress = { offset ->
                    val left = 58.dp.toPx()
                    val width = size.width - left - 12.dp.toPx()
                    if (samples.isNotEmpty() && width > 0) {
                        val slots = ReticulumNode.HISTORY_SECONDS
                        hover = (((offset.x - left) / width * (slots - 1)).roundToInt() - (slots - samples.size)).coerceIn(0, samples.size - 1)
                    }
                    tryAwaitRelease()
                    hover = -1
                })
            },
    ) {
        val plot = Rect(58.dp.toPx(), 32.dp.toPx(), size.width - 12.dp.toPx(), size.height - 24.dp.toPx())
        val slots = ReticulumNode.HISTORY_SECONDS
        var maximum = 0.0
        for (sample in samples) maximum = max(maximum, max(sample.rx.toDouble(), sample.tx.toDouble()))
        val top = niceCeiling(max(maximum * 1.1, 512.0))

        drawText(measurer, title, Offset(0f, 4.dp.toPx()), TextStyle(fontSize = 13.sp, fontWeight = FontWeight.SemiBold, color = palette.text))
        var legendX = size.width
        for ((label, color) in listOf("Sent" to palette.series(1), "Received" to palette.series(0))) {
            val layout = measurer.measure(label, small)
            legendX -= layout.size.width
            drawText(layout, topLeft = Offset(legendX, 6.dp.toPx()))
            legendX -= 18.dp.toPx()
            val y = 6.dp.toPx() + layout.size.height / 2f
            drawLine(color, Offset(legendX, y), Offset(legendX + 12.dp.toPx(), y), strokeWidth = 2.dp.toPx(), cap = StrokeCap.Round)
            legendX -= 14.dp.toPx()
        }

        for (step in 0..4) {
            val value = top * step / 4
            val y = plot.bottom - plot.height * step / 4
            drawLine(if (step == 0) palette.border else palette.grid, Offset(plot.left, y), Offset(plot.right, y), strokeWidth = 1.dp.toPx())
            val layout = measurer.measure(if (step == 0) "0" else ReticulumFormat.rate(value), small)
            drawText(layout, topLeft = Offset(plot.left - 6.dp.toPx() - layout.size.width, y - layout.size.height / 2f))
        }
        val marks = listOf("3 min ago", "2 min", "1 min", "now")
        marks.forEachIndexed { index, mark ->
            val layout = measurer.measure(mark, small)
            val x = plot.left + plot.width * index / 3
            val left = when (index) {
                0 -> x
                3 -> x - layout.size.width
                else -> x - layout.size.width / 2f
            }
            drawText(layout, topLeft = Offset(left, plot.bottom + 4.dp.toPx()))
        }

        val offset = slots - samples.size
        fun point(index: Int, value: Double) = Offset(
            plot.left + plot.width * (offset + index) / (slots - 1).toFloat(),
            plot.bottom - (plot.height * value / top).toFloat(),
        )
        if (samples.size >= 2) {
            for (series in 1 downTo 0) {
                val line = Path()
                samples.forEachIndexed { index, sample ->
                    val p = point(index, if (series == 0) sample.rx.toDouble() else sample.tx.toDouble())
                    if (index == 0) line.moveTo(p.x, p.y) else line.lineTo(p.x, p.y)
                }
                val area = Path().apply {
                    addPath(line)
                    val end = point(samples.size - 1, 0.0)
                    val start = point(0, 0.0)
                    lineTo(end.x, end.y)
                    lineTo(start.x, start.y)
                    close()
                }
                drawPath(area, palette.series(series).copy(alpha = 0.12f))
                drawPath(line, palette.series(series), style = Stroke(width = 2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
            }
        } else {
            val layout = measurer.measure(
                "Traffic shows here a few seconds after an interface comes online",
                small,
                constraints = androidx.compose.ui.unit.Constraints(maxWidth = plot.width.toInt().coerceAtLeast(1)),
            )
            drawText(layout, topLeft = Offset(plot.center.x - layout.size.width / 2f, plot.center.y - layout.size.height / 2f))
        }

        val index = hover
        if (index in samples.indices) {
            val sample = samples[index]
            val rx = point(index, sample.rx.toDouble())
            val tx = point(index, sample.tx.toDouble())
            drawLine(palette.muted, Offset(rx.x, plot.top), Offset(rx.x, plot.bottom), strokeWidth = 1.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f, 6f)))
            for ((p, color) in listOf(rx to palette.series(0), tx to palette.series(1))) {
                drawCircle(palette.surface, 6.dp.toPx(), p)
                drawCircle(color, 4.5.dp.toPx(), p)
            }
            val secondsAgo = samples.size - 1 - index
            val lines = listOf(
                if (secondsAgo == 0) "Now" else "$secondsAgo s ago",
                "Received ${ReticulumFormat.rate(sample.rx.toDouble())}",
                "Sent ${ReticulumFormat.rate(sample.tx.toDouble())}",
                ReticulumFormat.plural(sample.announces, "announce"),
            )
            val layouts = lines.mapIndexed { i, line -> measurer.measure(line, small.copy(color = if (i == 0) palette.muted else palette.text)) }
            val boxWidth = layouts.maxOf { it.size.width } + 20.dp.toPx()
            val lineHeight = layouts.first().size.height.toFloat()
            val boxHeight = lineHeight * lines.size + 12.dp.toPx()
            var boxX = rx.x + 12.dp.toPx()
            if (boxX + boxWidth > size.width - 4.dp.toPx()) boxX = rx.x - 12.dp.toPx() - boxWidth
            val box = Rect(Offset(boxX, plot.top + 4.dp.toPx()), Size(boxWidth, boxHeight))
            drawRoundRect(palette.raised, box.topLeft, box.size, CornerRadius(6.dp.toPx()))
            drawRoundRect(palette.border, box.topLeft, box.size, CornerRadius(6.dp.toPx()), style = Stroke(1.dp.toPx()))
            layouts.forEachIndexed { i, layout -> drawText(layout, topLeft = Offset(box.left + 10.dp.toPx(), box.top + 6.dp.toPx() + i * lineHeight)) }
        }
    }
}

class MapSpoke(val id: String, val name: String, val online: Boolean)

private class MapLayout(
    val positions: Map<String, Offset>,
    val sectors: List<Triple<String, Double, Double>>,
    val center: Offset,
    val maxRing: Int,
    val radii: List<Float>,
)

private fun ringRadius(hops: Int, size: IntSize, maxRing: Int, density: Float): Float {
    val outer = max(40f * density, min(size.width, size.height) / 2f - 44f * density)
    val inner = outer * 0.24f
    val ring = hops.coerceIn(1, maxRing)
    if (maxRing <= 1) return outer
    return inner + (outer - inner) * (ring - 1) / (maxRing - 1).toFloat()
}

private fun layoutMap(size: IntSize, density: Float, spokes: List<MapSpoke>, destinations: List<DestinationRecord>): MapLayout {
    val maxRing = min(max(2, destinations.maxOfOrNull { it.hops } ?: 0), 8)
    val center = Offset(size.width / 2f, size.height / 2f - 6f * density)
    val radii = (1..maxRing).map { ringRadius(it, size, maxRing, density) }
    val known = spokes.map { it.id }.toSet()
    val counts = HashMap<String, Int>()
    for (record in destinations) {
        val id = if (record.interfaceId in known) record.interfaceId else ""
        counts[id] = (counts[id] ?: 0) + 1
    }
    val order = spokes.map { it.id }.toMutableList()
    if (counts.containsKey("")) order.add("")
    if (order.isEmpty() || size.width == 0) return MapLayout(emptyMap(), emptyList(), center, maxRing, radii)
    val total = order.sumOf { max(1, counts[it] ?: 0) + 2 }.toDouble()
    var angle = -PI / 2
    val sectors = ArrayList<Triple<String, Double, Double>>()
    val sectorById = HashMap<String, Pair<Double, Double>>()
    for (id in order) {
        val span = 2 * PI * (max(1, counts[id] ?: 0) + 2) / total
        sectors.add(Triple(id, angle, span))
        sectorById[id] = angle to span
        angle += span
    }
    val ringGap = if (maxRing > 1) radii[1] - radii[0] else 30f * density
    val positions = HashMap<String, Offset>(destinations.size * 2)
    for (record in destinations) {
        val id = if (record.interfaceId in known) record.interfaceId else ""
        val (start, span) = sectorById[id] ?: continue
        val margin = min(0.12, span * 0.15)
        val theta = start + margin + (span - 2 * margin) * hashFraction(record.hash, 0)
        val jitter = ((hashFraction(record.hash, 4) - 0.5) * ringGap * 0.5).toFloat()
        val radius = ringRadius(record.hops, size, maxRing, density) + jitter
        positions[record.hex] = center + Offset((cos(theta) * radius).toFloat(), (sin(theta) * radius).toFloat())
    }
    return MapLayout(positions, sectors, center, maxRing, radii)
}

private fun hashAt(layout: MapLayout, point: Offset, density: Float): String {
    var best = ""
    var bestDistance = (18f * density) * (18f * density)
    for ((hash, position) in layout.positions) {
        val dx = position.x - point.x
        val dy = position.y - point.y
        val distance = dx * dx + dy * dy
        if (distance < bestDistance) {
            bestDistance = distance
            best = hash
        }
    }
    return best
}

@Composable
fun NetworkMap(
    spokes: List<MapSpoke>,
    destinations: List<DestinationRecord>,
    selfOnline: Boolean,
    selected: String,
    filter: String,
    pulses: Map<String, Long>,
    nowMs: Long,
    onSelect: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val palette = reticulumPalette()
    val measurer = rememberTextMeasurer()
    var size by remember { mutableStateOf(IntSize.Zero) }
    val density = androidx.compose.ui.platform.LocalDensity.current.density
    val layout = remember(size, spokes, destinations) { layoutMap(size, density, spokes, destinations) }
    var frame by remember { mutableLongStateOf(System.currentTimeMillis()) }
    val reducedMotion = com.forkmesh.nativeapp.ui.theme.LocalFmReducedMotion.current
    val animating = !reducedMotion && pulses.values.any { System.currentTimeMillis() - it < PULSE_MS }
    LaunchedEffect(animating, pulses.size) {
        while (animating && pulses.values.any { System.currentTimeMillis() - it < PULSE_MS }) {
            withFrameMillis { frame = System.currentTimeMillis() }
        }
        frame = System.currentTimeMillis()
    }
    Canvas(
        modifier
            .onSizeChanged { size = it }
            .semantics { contentDescription = "Reticulum network map: ${destinations.size} destinations on ${spokes.size} interfaces" }
            .pointerInput(layout) {
                detectTapGestures { point ->
                    val hash = hashAt(layout, point, density)
                    if (hash.isNotEmpty()) onSelect(hash)
                }
            },
    ) {
        val clock = max(frame, nowMs)
        val middle = layout.center
        val tiny = TextStyle(fontSize = 10.sp, color = palette.muted)
        for (ring in 1..layout.maxRing) {
            val radius = layout.radii[ring - 1]
            drawCircle(palette.grid, radius, middle, style = Stroke(1.dp.toPx()))
            val label = when {
                ring == layout.maxRing && layout.maxRing == 8 -> "8+ hops"
                ring == 1 -> "1 hop"
                else -> "$ring hops"
            }
            val text = measurer.measure(label, tiny)
            drawText(text, topLeft = Offset(middle.x + 4.dp.toPx(), middle.y - radius - text.size.height))
        }
        val outer = layout.radii.last()
        val spokeById = spokes.associateBy { it.id }
        for ((id, start, span) in layout.sectors) {
            val mid = start + span / 2
            if (layout.sectors.size > 1) {
                val direction = Offset(cos(start).toFloat(), sin(start).toFloat())
                drawLine(palette.grid, middle + direction * 14.dp.toPx(), middle + direction * (outer + 8.dp.toPx()), strokeWidth = 1.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(8f, 6f)))
            }
            val spoke = spokeById[id]
            val online = id.isNotEmpty() && spoke?.online == true
            val direction = Offset(cos(mid).toFloat(), sin(mid).toFloat())
            drawLine(
                if (online) palette.accent else palette.border,
                middle + direction * 14.dp.toPx(),
                middle + direction * (layout.radii.first() * 0.92f),
                strokeWidth = (if (online) 2.5f else 1.5f) * density,
                cap = StrokeCap.Round,
            )
            val name = if (id.isEmpty()) "Earlier interfaces" else spoke?.name.orEmpty()
            val text = measurer.measure(
                name,
                TextStyle(fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = if (online) palette.text else palette.muted),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                constraints = androidx.compose.ui.unit.Constraints(maxWidth = (120 * density).toInt()),
            )
            val textWidth = text.size.width + 12.dp.toPx()
            val anchor = middle + direction * (outer + 18.dp.toPx())
            var x = anchor.x - textWidth / 2
            if (direction.x > 0.35f) x = anchor.x - 6.dp.toPx() else if (direction.x < -0.35f) x = anchor.x - textWidth + 6.dp.toPx()
            x = x.coerceIn(2f, max(2f, size.width - textWidth - 2f))
            val y = (anchor.y - text.size.height / 2f).coerceIn(2f, max(2f, size.height - text.size.height - 22.dp.toPx()))
            drawCircle(if (online) palette.good else palette.muted, 3.dp.toPx(), Offset(x + 4.dp.toPx(), y + text.size.height / 2f))
            drawText(text, topLeft = Offset(x + 10.dp.toPx(), y))
        }

        val selectedPoint = layout.positions[selected]
        if (selectedPoint != null) {
            drawLine(palette.accent, middle, selectedPoint, strokeWidth = 1.5.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(8f, 6f)))
        }
        val needle = filter.trim()
        for (record in destinations) {
            val point = layout.positions[record.hex] ?: continue
            val group = MapGroup.of(record.kind)
            val age = (clock - record.lastHeard) / 1000
            var alpha = if (age < 600) 1f else if (age < 3600) 0.75f else 0.45f
            if (needle.isNotEmpty() && !(record.displayName.contains(needle, true) || record.label.contains(needle, true) || record.hex.contains(needle, true))) alpha = 0.12f
            val emphasised = record.hex == selected
            groupMarker(group, point, (if (emphasised) 6.5f else 4.5f) * density, palette.group(group).copy(alpha = alpha), palette.surface)
            if (emphasised) drawCircle(palette.accent, 10.dp.toPx(), point, style = Stroke(2.dp.toPx()))
            val pulse = if (reducedMotion) null else pulses[record.hex]
            if (pulse != null) {
                val progress = (clock - pulse).toFloat() / PULSE_MS
                if (progress in 0f..1f) {
                    drawCircle(palette.group(group).copy(alpha = 0.8f * (1 - progress)), (5 + 22 * progress) * density, point, style = Stroke(2.dp.toPx()))
                }
            }
        }
        if (selectedPoint != null) {
            destinations.firstOrNull { it.hex == selected }?.let { record ->
                val label = record.displayName.ifEmpty { Rns.prettyHash(record.hash).take(12) + "…>" }
                val text = measurer.measure(label, TextStyle(fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = palette.text))
                val boxWidth = text.size.width + 14.dp.toPx()
                val boxHeight = text.size.height + 8.dp.toPx()
                var left = selectedPoint.x + 12.dp.toPx()
                if (left + boxWidth > size.width - 2f) left = selectedPoint.x - 12.dp.toPx() - boxWidth
                val topLeft = Offset(left, selectedPoint.y - boxHeight / 2)
                drawRoundRect(palette.raised, topLeft, Size(boxWidth, boxHeight), CornerRadius(5.dp.toPx()))
                drawRoundRect(palette.border, topLeft, Size(boxWidth, boxHeight), CornerRadius(5.dp.toPx()), style = Stroke(1.dp.toPx()))
                drawText(text, topLeft = topLeft + Offset(7.dp.toPx(), 4.dp.toPx()))
            }
        }

        drawCircle(palette.surface, 12.5.dp.toPx(), middle)
        drawCircle(if (selfOnline) palette.accent else palette.muted, 11.dp.toPx(), middle)
        drawCircle(palette.surface, 4.dp.toPx(), middle)
        val me = measurer.measure("This phone", TextStyle(fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = palette.text))
        drawText(me, topLeft = Offset(middle.x - me.size.width / 2f, middle.y + 14.dp.toPx()))

        val groupCounts = destinations.groupingBy { MapGroup.of(it.kind) }.eachCount()
        var legendY = size.height - 16.dp.toPx()
        var legendX = 6.dp.toPx()
        for (group in MapGroup.entries) {
            val text = measurer.measure("${group.label} ${groupCounts[group] ?: 0}", tiny)
            if (legendX + text.size.width + 24.dp.toPx() > size.width) {
                legendX = 6.dp.toPx()
                legendY -= 16.dp.toPx()
            }
            groupMarker(group, Offset(legendX + 5.dp.toPx(), legendY + text.size.height / 2f), 4.dp.toPx(), palette.group(group), palette.surface)
            drawText(text, topLeft = Offset(legendX + 14.dp.toPx(), legendY))
            legendX += text.size.width + 26.dp.toPx()
        }

        if (destinations.isEmpty()) {
            val text = measurer.measure(
                if (spokes.isEmpty()) "Add an interface to join a Reticulum network" else "Destinations appear here as their announces arrive",
                TextStyle(fontSize = 13.sp, color = palette.muted, textAlign = androidx.compose.ui.text.style.TextAlign.Center),
                constraints = androidx.compose.ui.unit.Constraints(maxWidth = (min(size.width.toFloat(), 300 * density)).toInt().coerceAtLeast(1)),
            )
            drawText(text, topLeft = Offset(middle.x - text.size.width / 2f, middle.y + 36.dp.toPx()))
        }
    }
}

private const val PULSE_MS = 1600L
