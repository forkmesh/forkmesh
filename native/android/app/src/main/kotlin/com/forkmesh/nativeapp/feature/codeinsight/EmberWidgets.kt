package com.forkmesh.nativeapp.feature.codeinsight

import android.graphics.Paint
import android.text.TextPaint
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.codeinsight.Ember
import com.forkmesh.nativeapp.codeinsight.EmberColoring
import com.forkmesh.nativeapp.codeinsight.EmberPanel
import com.forkmesh.nativeapp.codeinsight.EmberRow
import com.forkmesh.nativeapp.codeinsight.InsightTrendPoint
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

@Composable
internal fun EmberRowItem(row: EmberRow, selected: Boolean, onTap: () -> Unit, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val spoken = buildString {
        if (row.rank > 0) append("Rank ${row.rank}. ")
        append(row.title)
        if (row.detail.isNotEmpty()) append(", ${row.detail}")
        append(". ${row.value}")
        for (note in row.notes) append(". $note")
    }
    Column(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .background(if (selected) c.accentSubtle else Color.Transparent)
            .clickable {
                haptics.selection()
                onTap()
            }
            .clearAndSetSemantics { contentDescription = spoken }
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x2),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (row.rank > 0) {
                Box(
                    Modifier
                        .size(20.dp)
                        .background(c.textPrimary, CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Text("${row.rank}", style = Fm.text.labelSmall.copy(color = c.bgBase, letterSpacing = 0.sp))
                }
                Spacer(Modifier.width(FmSpace.x2))
            }
            Box(
                Modifier
                    .size(10.dp)
                    .background(row.color.compose(), CircleShape),
            )
            Spacer(Modifier.width(FmSpace.x2))
            Column(Modifier.weight(1f)) {
                Text(row.title, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(fontWeight = FmType.medium))
                if (row.detail.isNotEmpty()) {
                    Text(row.detail, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall)
                }
            }
            Spacer(Modifier.width(FmSpace.x2))
            Text(row.value, style = Fm.text.bodySmall.copy(fontFeatureSettings = FmType.tabular))
        }
        Spacer(Modifier.height(FmSpace.x1))
        EmberBar(row.fraction, row.color.compose())
        for (note in row.notes) {
            Text(
                note,
                modifier = Modifier.padding(top = 2.dp),
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodySmall.copy(color = c.textTertiary),
            )
        }
    }
}

@Composable
internal fun EmberBar(fraction: Double, color: Color, modifier: Modifier = Modifier) {
    val track = Fm.colors.border
    Canvas(
        modifier
            .fillMaxWidth()
            .height(4.dp),
    ) {
        val r = androidx.compose.ui.geometry.CornerRadius(size.height / 2)
        drawRoundRect(track, cornerRadius = r)
        val w = size.width * fraction.toFloat().coerceIn(0f, 1f)
        if (w > 0f) drawRoundRect(color, size = Size(maxOf(w, size.height), size.height), cornerRadius = r)
    }
}

@Composable
internal fun EmberHealthRing(health: Double, modifier: Modifier = Modifier, diameter: Dp = 84.dp) {
    val c = Fm.colors
    val color = Ember.healthColor(health).compose()
    val track = if (c.dark) Color(255, 255, 255, 28) else Color(31, 35, 40, 26)
    Box(
        modifier
            .size(diameter)
            .semantics { contentDescription = "Code health ${EmberPanel.healthText(health)} of 10" },
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(diameter)) {
            val stroke = 8.dp.toPx()
            val inset = 7.dp.toPx()
            val box = Size(size.width - 2 * inset, size.height - 2 * inset)
            drawArc(track, 0f, 360f, false, Offset(inset, inset), box, style = Stroke(stroke))
            drawArc(color, -90f, (EmberPanel.healthShare(health) * 360).toFloat(), false, Offset(inset, inset), box, style = Stroke(stroke, cap = StrokeCap.Round))
        }
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(EmberPanel.healthText(health), style = Fm.text.titleLarge.copy(fontWeight = FontWeight.Bold))
            Text("of 10", style = Fm.text.labelSmall.copy(color = c.textTertiary, letterSpacing = 0.sp))
        }
    }
}

@Composable
internal fun EmberLegend(coloring: EmberColoring, onInfo: () -> Unit, modifier: Modifier = Modifier) {
    val c = Fm.colors
    Row(
        modifier
            .clip(RoundedCornerShape(FmRadius.full))
            .background(if (c.dark) Color(22, 27, 34, 225) else Color(255, 255, 255, 235))
            .padding(end = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(36.dp)
                .clip(CircleShape)
                .clickable(onClick = onInfo)
                .semantics { contentDescription = EmberPanel.INFO_TITLE },
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("info_outline", size = 18.dp, tint = c.textSecondary)
        }
        val labelStyle = Fm.text.bodySmall.copy(color = c.textSecondary)
        if (coloring.scale) {
            Text(coloring.scaleLowLabel, style = labelStyle)
            Spacer(Modifier.width(6.dp))
            val low = coloring.scaleLow?.compose() ?: Color.Gray
            val high = coloring.scaleHigh?.compose() ?: Color.Gray
            val stops = if (coloring.tab == com.forkmesh.nativeapp.codeinsight.EmberTab.Hotspots) {
                arrayOf(0f to low, 0.5f to Ember.hotspotOrange.compose(), 1f to high)
            } else {
                arrayOf(0f to low, 1f to high)
            }
            Box(
                Modifier
                    .width(48.dp)
                    .height(8.dp)
                    .clip(RoundedCornerShape(4.dp))
                    .background(Brush.horizontalGradient(*stops)),
            )
            Spacer(Modifier.width(6.dp))
            Text(coloring.scaleHighLabel, style = labelStyle)
            Spacer(Modifier.width(FmSpace.x3))
        }
        coloring.legend.forEachIndexed { i, entry ->
            if (i > 0 || coloring.scale) Spacer(Modifier.width(FmSpace.x3))
            val color = entry.color?.compose()
            Canvas(Modifier.size(12.dp)) {
                val center = Offset(size.width / 2, size.height / 2)
                when {
                    entry.ring && color != null -> drawCircle(color.copy(alpha = 0.8f), size.minDimension / 2.4f, center, style = Stroke(3.dp.toPx()))
                    entry.ring -> drawCircle(
                        c.textSecondary, size.minDimension / 2.4f, center,
                        style = Stroke(1.2.dp.toPx(), pathEffect = androidx.compose.ui.graphics.PathEffect.dashPathEffect(floatArrayOf(2.dp.toPx(), 2.dp.toPx()))),
                    )
                    color != null -> drawCircle(color, size.minDimension / 2.6f, center)
                }
            }
            Spacer(Modifier.width(4.dp))
            Text(entry.label, style = labelStyle, maxLines = 1)
        }
    }
}

@Composable
internal fun EmberTrendChart(points: List<InsightTrendPoint>, modifier: Modifier = Modifier) {
    val c = Fm.colors
    var hover by remember(points) { mutableIntStateOf(-1) }
    val series = listOf(if (c.dark) Color(0xFF8B949E) else Color(0xFF57606A), Color(0xFFF0883E))
    val titles = listOf("Lines of code", "Complexity")
    val shown = if (hover in points.indices) hover else points.lastIndex
    val density = LocalDensity.current
    val axisPaint = remember { TextPaint(Paint.ANTI_ALIAS_FLAG) }
    val axisWidth = 40.dp
    Column(modifier, verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        for (band in 0..1) {
            val values = points.map { if (band == 0) it.codeLines else it.complexity }
            val niceMax = EmberPanel.niceMax(values.maxOrNull()?.toDouble() ?: 1.0)
            Row(Modifier.fillMaxWidth().padding(start = axisWidth), verticalAlignment = Alignment.CenterVertically) {
                Text(titles[band], style = Fm.text.labelMedium, modifier = Modifier.weight(1f))
                Text(Ember.number(values[shown]), style = Fm.text.bodySmall.copy(fontFeatureSettings = FmType.tabular))
            }
            Canvas(
                Modifier
                    .fillMaxWidth()
                    .height(110.dp)
                    .semantics { contentDescription = "${titles[band]} chart, ${points.size} versions, now ${values.last()}. Tap to inspect a version." }
                    .pointerInput(points) {
                        val left = axisWidth.toPx()
                        detectTapGestures { offset ->
                            hover = EmberPanel.nearestTrendPoint(points, ((offset.x - left) / (size.width - left - 8.dp.toPx())).toDouble())
                        }
                    }
                    .pointerInput(points) {
                        val left = axisWidth.toPx()
                        detectHorizontalDragGestures { change, _ ->
                            hover = EmberPanel.nearestTrendPoint(points, ((change.position.x - left) / (size.width - left - 8.dp.toPx())).toDouble())
                        }
                    },
            ) {
                val left = axisWidth.toPx()
                val right = size.width - 8.dp.toPx()
                val top = 6.dp.toPx()
                val bottom = size.height - 4.dp.toPx()
                fun x(i: Int) = if (points.size < 2) (left + right) / 2 else left + EmberPanel.trendX(points, i).toFloat() * (right - left)
                fun y(v: Int) = bottom - (v / niceMax).toFloat() * (bottom - top)
                val grid = if (c.dark) Color(255, 255, 255, 22) else Color(31, 35, 40, 24)
                axisPaint.textSize = with(density) { 10.sp.toPx() }
                axisPaint.color = c.textTertiary.toArgb()
                axisPaint.textAlign = Paint.Align.RIGHT
                for (g in 0..2) {
                    val gy = bottom - g / 2f * (bottom - top)
                    drawLine(grid, Offset(left, gy), Offset(right, gy), 1.dp.toPx())
                    drawIntoCanvas { it.nativeCanvas.drawText(EmberPanel.compact(niceMax * g / 2), left - 6.dp.toPx(), gy + 3.dp.toPx(), axisPaint) }
                }
                val line = Path()
                val area = Path()
                values.forEachIndexed { i, v ->
                    val p = Offset(x(i), y(v))
                    if (i == 0) {
                        line.moveTo(p.x, p.y)
                        area.moveTo(p.x, bottom)
                        area.lineTo(p.x, p.y)
                    } else {
                        line.lineTo(p.x, p.y)
                        area.lineTo(p.x, p.y)
                    }
                }
                area.lineTo(x(values.lastIndex), bottom)
                area.close()
                drawPath(area, Brush.verticalGradient(listOf(series[band].copy(alpha = 70f / 255f), series[band].copy(alpha = 0f)), top, bottom))
                drawPath(line, series[band], style = Stroke(2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
                if (hover in points.indices) {
                    val hx = x(hover)
                    drawLine(c.textTertiary, Offset(hx, top), Offset(hx, bottom), 1.dp.toPx(), pathEffect = androidx.compose.ui.graphics.PathEffect.dashPathEffect(floatArrayOf(4.dp.toPx(), 4.dp.toPx())))
                }
                val m = Offset(x(shown), y(values[shown]))
                drawCircle(c.bgRaised, 6.5.dp.toPx(), m)
                drawCircle(series[band], 4.5.dp.toPx(), m)
            }
        }
        Row(Modifier.fillMaxWidth().padding(start = axisWidth, end = 8.dp)) {
            Text(EmberPanel.monthText(points.first().time), style = Fm.text.bodySmall, modifier = Modifier.weight(1f))
            Text(EmberPanel.monthText(points.last().time), style = Fm.text.bodySmall, textAlign = TextAlign.End)
        }
        Text(
            EmberPanel.trendInfo(points[shown]),
            modifier = Modifier.padding(start = axisWidth, top = FmSpace.x1),
            style = Fm.text.bodySmall.copy(color = if (hover >= 0) c.textPrimary else c.textSecondary),
        )
    }
}

@Composable
internal fun EmberBandStrip(modifier: Modifier = Modifier) {
    Canvas(
        modifier
            .fillMaxWidth()
            .height(4.dp),
    ) {
        val unit = size.width / 9f
        drawRect(Ember.unhealthyColor.compose(), Offset.Zero, Size(unit * 3, size.height))
        drawRect(Ember.problematicColor.compose(), Offset(unit * 3, 0f), Size(unit * 4, size.height))
        drawRect(Ember.healthyColor.compose(), Offset(unit * 7, 0f), Size(unit * 2, size.height))
    }
}
