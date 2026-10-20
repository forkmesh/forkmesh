package com.forkmesh.nativeapp.feature.tools

import com.forkmesh.nativeapp.tools.TelemetryHealth
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.text.TextAutoSize
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.tools.AppTelemetry
import com.forkmesh.nativeapp.tools.LogCategory
import com.forkmesh.nativeapp.tools.TelemetryChart
import com.forkmesh.nativeapp.tools.TelemetryMetric
import com.forkmesh.nativeapp.tools.TelemetryStripLayout
import com.forkmesh.nativeapp.tools.compactCount
import com.forkmesh.nativeapp.tools.formatUptime
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmType

@Composable
internal fun TelemetryStripCompactView(telemetry: AppTelemetry, onOpen: () -> Unit, modifier: Modifier = Modifier) {
    val c = Fm.colors
    telemetry.observe()
    val summary = (listOf("Diagnostics") + TelemetryMetric.entries.map { "${it.title} ${it.format(telemetry.latest(it))}" }).joinToString(", ")
    Row(
        modifier
            .clickable(onClick = onOpen)
            .clearAndSetSemantics {
                contentDescription = summary
                role = Role.Button
            }
            .padding(vertical = 3.dp),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        for (metric in TelemetryMetric.entries) {
            val series = telemetry.lastSeconds(metric, TelemetryStripLayout.SPARK_SECONDS)
            val line = when (metric.health(series)) {
                TelemetryHealth.Good -> c.success
                TelemetryHealth.Warning -> c.warning
                TelemetryHealth.Bad -> c.danger
                null -> c.textTertiary
            }
            Canvas(Modifier.width(12.dp).height(10.dp)) {
                drawTelemetryChart(series, TelemetryStripLayout.SPARK_SECONDS, metric.floor, line, Color.Transparent, strokeWidth = 1.dp.toPx())
            }
        }
    }
}

@Composable
internal fun TelemetryStripView(
    telemetry: AppTelemetry,
    onOpenMetric: (TelemetryMetric?) -> Unit,
    onOpenLogs: (LogCategory) -> Unit,
) {
    val c = Fm.colors
    telemetry.observe()
    Box(
        Modifier
            .fillMaxWidth()
            .height(TelemetryStripLayout.HEIGHT_DP.dp)
            .background(c.bgRaised)
            .drawBehind {
                val rule = 1.dp.toPx()
                drawRect(c.border, topLeft = Offset(0f, size.height - rule), size = Size(size.width, rule))
            },
    ) {
        Row(
            Modifier
                .fillMaxSize()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 12.dp, vertical = 3.dp),
        ) {
            MetricSquare(
                label = "UP",
                semanticLabel = "Uptime",
                value = formatUptime(telemetry.uptimeMs),
                series = null,
                floor = 1.0,
                onTap = { onOpenMetric(null) },
            )
            for (metric in TelemetryMetric.entries) {
                MetricSquare(
                    label = metric.label,
                    semanticLabel = metric.title,
                    value = metric.format(telemetry.latest(metric)),
                    series = telemetry.lastSeconds(metric, TelemetryStripLayout.SPARK_SECONDS),
                    floor = metric.floor,
                    onTap = { onOpenMetric(metric) },
                )
            }
            Box(
                Modifier
                    .padding(horizontal = 4.dp, vertical = 4.dp)
                    .width(1.dp)
                    .fillMaxHeight()
                    .background(c.border),
            )
            for (category in LogCategory.entries) {
                LogLight(category, telemetry.log.totalFor(category)) { onOpenLogs(category) }
            }
        }
    }
}

@Composable
private fun MetricSquare(
    label: String,
    semanticLabel: String,
    value: String,
    series: DoubleArray?,
    floor: Double,
    onTap: () -> Unit,
) {
    val c = Fm.colors
    val style = TextStyle(
        fontFamily = FmType.ui,
        fontSize = FmType.micro,
        lineHeight = FmType.micro,
        letterSpacing = 0.sp,
        fontFeatureSettings = FmType.tabular,
    )
    val caption = buildAnnotatedString {
        withStyle(SpanStyle(color = c.textTertiary, fontWeight = FmType.medium)) { append("$label ") }
        withStyle(SpanStyle(color = c.textPrimary, fontWeight = FmType.emphasis)) { append(value) }
    }
    val autoSize = TextAutoSize.StepBased(minFontSize = 6.sp, maxFontSize = FmType.micro, stepSize = 0.5.sp)
    Box(
        Modifier
            .padding(end = 4.dp)
            .fillMaxHeight()
            .width(TelemetryStripLayout.SQUARE_WIDTH_DP.dp)
            .clip(RoundedCornerShape(FmRadius.sm))
            .background(c.bgOverlay)
            .clickable(onClick = onTap)
            .clearAndSetSemantics {
                contentDescription = "$semanticLabel $value"
                role = Role.Button
            }
            .padding(start = 4.dp, top = 3.dp, end = 4.dp, bottom = 2.dp),
    ) {
        if (series == null || series.isEmpty()) {
            BasicText(caption, Modifier.align(Alignment.Center), style = style, maxLines = 1, autoSize = autoSize)
        } else {
            Column(Modifier.fillMaxSize()) {
                BasicText(caption, style = style, maxLines = 1, autoSize = autoSize)
                Spacer(Modifier.height(2.dp))
                val line = c.accent
                val fill = c.accentSubtle
                Canvas(
                    Modifier
                        .fillMaxWidth()
                        .weight(1f),
                ) {
                    drawTelemetryChart(series, TelemetryStripLayout.SPARK_SECONDS, floor, line, fill, strokeWidth = 1.dp.toPx())
                }
            }
        }
    }
}

@Composable
private fun LogLight(category: LogCategory, count: Int, onTap: () -> Unit) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxHeight()
            .width(30.dp)
            .clip(RoundedCornerShape(FmRadius.sm))
            .clickable(onClick = onTap)
            .clearAndSetSemantics {
                contentDescription = "${category.label} logs, $count"
                role = Role.Button
            },
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            compactCount(count),
            maxLines = 1,
            style = TextStyle(
                fontFamily = FmType.ui,
                fontSize = FmType.micro,
                lineHeight = FmType.micro,
                letterSpacing = 0.sp,
                fontFeatureSettings = FmType.tabular,
                fontWeight = FmType.medium,
                color = logCategoryColor(category, count),
            ),
        )
        Spacer(Modifier.height(2.dp))
        FmIcon(category.iconName(), size = 14.dp, tint = c.textSecondary)
    }
}

internal fun DrawScope.drawTelemetryChart(
    values: DoubleArray,
    slots: Int,
    floor: Double,
    line: Color,
    fill: Color,
    strokeWidth: Float,
    grid: Color? = null,
    scrubIndex: Int? = null,
    surface: Color? = null,
) {
    val onePx = 1.dp.toPx()
    val half = 0.5.dp.toPx()
    if (grid != null) {
        drawLine(grid, Offset(0f, size.height - half), Offset(size.width, size.height - half), strokeWidth = onePx)
        drawLine(grid, Offset(0f, half), Offset(size.width, half), strokeWidth = onePx)
    }
    val n = values.size
    if (n == 0) return
    val width = size.width.toDouble()
    val height = size.height.toDouble()
    val top = TelemetryChart.scaleTop(values, floor)
    val step = TelemetryChart.step(width, n, slots)
    val inset = strokeWidth / 2.0
    fun xAt(i: Int): Float = TelemetryChart.xAt(i, n, width, step).toFloat()
    fun yAt(v: Double): Float = TelemetryChart.yAt(v, top, height, inset).toFloat()

    val stroke = Stroke(width = strokeWidth, join = StrokeJoin.Round, cap = StrokeCap.Round)
    for (run in TelemetryChart.runs(values)) {
        if (run.first == run.last) {
            drawCircle(line, radius = strokeWidth, center = Offset(xAt(run.first), yAt(values[run.first])))
            continue
        }
        val path = Path()
        val area = Path()
        path.moveTo(xAt(run.first), yAt(values[run.first]))
        area.moveTo(xAt(run.first), yAt(values[run.first]))
        for (i in run.first + 1..run.last) {
            path.lineTo(xAt(i), yAt(values[i]))
            area.lineTo(xAt(i), yAt(values[i]))
        }
        area.lineTo(xAt(run.last), size.height)
        area.lineTo(xAt(run.first), size.height)
        area.close()
        drawPath(area, fill)
        drawPath(path, line, style = stroke)
    }

    if (scrubIndex != null && scrubIndex in 0 until n) {
        val x = xAt(scrubIndex)
        drawLine(grid ?: line, Offset(x, 0f), Offset(x, size.height), strokeWidth = onePx)
        val v = values[scrubIndex]
        if (!v.isNaN()) {
            val center = Offset(x, yAt(v))
            if (surface != null) drawCircle(surface, radius = 6.dp.toPx(), center = center)
            drawCircle(line, radius = 4.dp.toPx(), center = center)
        }
    }
}
