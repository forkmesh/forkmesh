package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.insights.ActivityGrid
import com.forkmesh.nativeapp.insights.ActivitySummary
import com.forkmesh.nativeapp.ui.kit.FmUserActivity
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.text.NumberFormat

private val lightLevels = listOf(Color(0xFFEBEDF0), Color(0xFF9BE9A8), Color(0xFF40C463), Color(0xFF30A14E), Color(0xFF216E39))
private val darkLevels = listOf(Color(0xFF2D333B), Color(0xFF0E4429), Color(0xFF006D32), Color(0xFF26A641), Color(0xFF39D353))

private fun levelColor(level: Int, dark: Boolean): Color = (if (dark) darkLevels else lightLevels)[level.coerceIn(0, ActivityGrid.LEVELS - 1)]

@Composable
internal fun TodayActivityBlock() {
    var counts by remember { mutableStateOf(FmUserActivity.log.counts()) }
    var otherKinds by remember { mutableStateOf(FmUserActivity.log.otherKinds()) }
    val nowSlot = remember { FmUserActivity.log.nowSlot() }
    LaunchedEffect(Unit) {
        if (FmUserActivity.syncNow(force = true)) {
            counts = FmUserActivity.log.counts()
            otherKinds = FmUserActivity.log.otherKinds()
        }
    }
    TodayActivityBlock(counts, nowSlot, otherKinds)
}

@Composable
internal fun TodayActivityBlock(counts: IntArray, nowSlot: Int, otherKinds: List<String> = emptyList()) {
    val sources = if (otherKinds.isEmpty()) "" else ActivityGrid.sourcesText(FmUserActivity.KIND, otherKinds)
    val c = Fm.colors
    val summary = remember(counts) { ActivitySummary.of(counts) }
    val number = remember { NumberFormat.getIntegerInstance() }
    val spoken = if (summary.total == 0) {
        "Today's activity: nothing recorded yet."
    } else {
        "Today's activity: ${summary.total} interactions, active ${summary.activeText}, busiest hour " +
            "${ActivityGrid.slotLabel(summary.peakHour * ActivityGrid.ROWS)} with ${summary.peakHourTotal}." +
            if (sources.isEmpty()) "" else " Counted on $sources."
    }
    Column(
        Modifier.fillMaxWidth().clearAndSetSemantics { contentDescription = spoken },
        verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("TODAY'S ACTIVITY", modifier = Modifier.weight(1f), style = Fm.text.labelSmall.copy(color = c.textTertiary))
            Text(
                "${number.format(summary.total)} ${if (summary.total == 1) "interaction" else "interactions"}",
                style = Fm.text.labelSmall.copy(color = c.textSecondary),
            )
        }
        if (sources.isNotEmpty()) {
            Text("On $sources", style = Fm.text.labelSmall.copy(color = c.textTertiary))
        }
        ActivityHeatGrid(counts, nowSlot, summary.peak)
        if (summary.total > 0) {
            Text(
                "Active ${summary.activeText} · busiest hour ${ActivityGrid.slotLabel(summary.peakHour * ActivityGrid.ROWS)} " +
                    "(${number.format(summary.peakHourTotal)}) · busiest 5 min ${ActivityGrid.slotLabel(summary.peakSlot)} " +
                    "(${number.format(summary.peak)})",
                style = Fm.text.labelSmall.copy(color = c.textSecondary),
            )
        }
        ActivityLegend(c.dark)
    }
}

@Composable
private fun ActivityHeatGrid(counts: IntArray, nowSlot: Int, peak: Int) {
    val c = Fm.colors
    val measurer = rememberTextMeasurer()
    val labelStyle = TextStyle(color = c.textTertiary, fontSize = 9.sp)
    BoxWithConstraints(Modifier.fillMaxWidth()) {
        val gutter = 24.dp
        val axis = 14.dp
        val pitch = (maxWidth - gutter) / ActivityGrid.HOURS
        Canvas(Modifier.fillMaxWidth().height(pitch * ActivityGrid.ROWS + axis)) {
            val gutterPx = gutter.toPx()
            val pitchPx = pitch.toPx()
            val gap = minOf(2.dp.toPx(), pitchPx * 0.2f)
            val cell = pitchPx - gap
            val radius = CornerRadius(minOf(2.dp.toPx(), cell / 3))
            for (slot in 0 until ActivityGrid.SLOTS) {
                val value = counts.getOrElse(slot) { 0 }
                var fill = levelColor(ActivityGrid.level(value, peak), c.dark)
                if (slot > nowSlot && value <= 0) fill = fill.copy(alpha = 0.4f)
                val x = gutterPx + (slot / ActivityGrid.ROWS) * pitchPx + gap / 2
                val y = (slot % ActivityGrid.ROWS) * pitchPx + gap / 2
                drawRoundRect(fill, Offset(x, y), Size(cell, cell), radius)
            }
            if (nowSlot in 0 until ActivityGrid.SLOTS) {
                val x = gutterPx + (nowSlot / ActivityGrid.ROWS) * pitchPx + gap / 2
                val y = (nowSlot % ActivityGrid.ROWS) * pitchPx + gap / 2
                val ring = 1.5.dp.toPx()
                drawRoundRect(
                    c.textPrimary,
                    Offset(x - ring / 2, y - ring / 2),
                    Size(cell + ring, cell + ring),
                    radius,
                    style = Stroke(ring),
                )
            }
            for (row in 0 until ActivityGrid.ROWS step 3) {
                val layout = measurer.measure(":%02d".format(row * ActivityGrid.SLOT_MINUTES), labelStyle)
                val y = row * pitchPx + pitchPx / 2 - layout.size.height / 2f
                drawText(layout, topLeft = Offset(gutterPx - layout.size.width - 3.dp.toPx(), y))
            }
            for (hour in 0 until ActivityGrid.HOURS step 6) {
                val layout = measurer.measure("%02d".format(hour), labelStyle)
                val x = gutterPx + hour * pitchPx + pitchPx / 2 - layout.size.width / 2f
                drawText(layout, topLeft = Offset(x, ActivityGrid.ROWS * pitchPx + 1.dp.toPx()))
            }
        }
    }
}

@Composable
private fun ActivityLegend(dark: Boolean) {
    val c = Fm.colors
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End, verticalAlignment = Alignment.CenterVertically) {
        Text("Less", style = Fm.text.labelSmall.copy(color = c.textTertiary))
        Spacer(Modifier.width(4.dp))
        for (level in 0 until ActivityGrid.LEVELS) {
            Box(Modifier.size(10.dp).background(levelColor(level, dark), RoundedCornerShape(2.dp)))
            Spacer(Modifier.width(3.dp))
        }
        Spacer(Modifier.width(1.dp))
        Text("More", style = Fm.text.labelSmall.copy(color = c.textTertiary))
    }
}
