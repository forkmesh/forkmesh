package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.chat.CallDiagnosis
import com.forkmesh.nativeapp.chat.CallDiagnostics
import com.forkmesh.nativeapp.chat.CallLevel
import com.forkmesh.nativeapp.chat.CallLossMark
import com.forkmesh.nativeapp.chat.CallQuality
import com.forkmesh.nativeapp.chat.CallQualityGrade
import com.forkmesh.nativeapp.chat.CallQualityHistory
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.ui.theme.FmColors
import kotlin.math.max

private object CallQualityColors {
    val you = FmColors.accentOnDark
    val them = FmColors.offline
    val line = FmColors.darkTextPrimary
    val grid = Color.White.copy(alpha = .3f)
    val idle = Color.White.copy(alpha = .25f)
    val good = FmColors.successOnDark
    val fair = FmColors.warningOnDark
    val poor = FmColors.dangerOnDark
    val glass = Color(0xFF0B0B0C).copy(alpha = .55f)
    val edge = Color.White.copy(alpha = .12f)
    val text = FmColors.darkTextPrimary
    val muted = FmColors.darkTextSecondary

    fun grade(grade: CallQualityGrade): Color = when (grade) {
        CallQualityGrade.GOOD -> good
        CallQualityGrade.FAIR -> fair
        CallQualityGrade.POOR -> poor
        CallQualityGrade.UNKNOWN -> idle
    }

    fun loss(mark: CallLossMark): Color? = when (mark) {
        CallLossMark.NONE -> null
        CallLossMark.FAIR -> fair
        CallLossMark.POOR -> poor
    }
}

private val pillShape = RoundedCornerShape(13.dp)
private val cardShape = RoundedCornerShape(14.dp)

@Composable
internal fun CallQualityWidget(
    history: CallQualityHistory,
    video: Boolean,
    modifier: Modifier = Modifier,
    peerLabel: String = "Them",
    sending: Boolean = true,
    finishOnLeave: Boolean = true,
) {
    if (history.latest == null && history.levels.isEmpty()) return
    val prefs = remember { Services.get<PrefStore>() }
    var open by remember { mutableStateOf(prefs?.getBool(CallQuality.OPEN_KEY) == true) }
    val latest = history.latest
    val live = history.live
    val summary = CallQuality.accessibility(history)
    val diagnosis = remember(history.samples, live, video, sending) {
        if (live) CallDiagnostics.diagnose(history.samples, video = video, sending = sending) else CallDiagnosis.EMPTY
    }
    LaunchedEffect(diagnosis) { if (live) CallDiagnosticsBook.refreshed(diagnosis) }
    if (finishOnLeave) DisposableEffect(Unit) { onDispose { CallDiagnosticsBook.finish() } }
    Column(modifier, horizontalAlignment = Alignment.Start) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
            Row(
                Modifier
                    .height(26.dp)
                    .clip(pillShape)
                    .background(CallQualityColors.glass)
                    .border(0.5.dp, CallQualityColors.edge, pillShape)
                    .clickable(role = Role.Button, onClickLabel = if (open) "Hide live charts" else "Show live charts") {
                        open = !open
                        prefs?.putBool(CallQuality.OPEN_KEY, open)
                    }
                    .semantics(mergeDescendants = true) {
                        contentDescription = summary
                        stateDescription = if (open) "Expanded" else "Collapsed"
                    }
                    .padding(horizontal = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (live) {
                    val bars = latest?.bars ?: 0
                    val color = CallQualityColors.grade(latest?.grade ?: CallQualityGrade.UNKNOWN)
                    Canvas(Modifier.size(width = 14.dp, height = 12.dp)) { signalBars(bars, color) }
                }
                Canvas(Modifier.size(width = 40.dp, height = 16.dp)) { voiceWave(history.levels, CallQuality.PILL_SAMPLES) }
                val rtt = CallQuality.rttText(latest)
                if (rtt.isNotEmpty()) {
                    Text(rtt, color = CallQualityColors.text, fontSize = 11.sp, fontWeight = FontWeight.Medium, maxLines = 1)
                }
            }
            if (live) CallDiagnosticsButton(diagnosis)
        }
        if (open) {
            Spacer(Modifier.height(6.dp))
            QualityCard(history, video, peerLabel)
        }
    }
}

@Composable
private fun QualityCard(history: CallQualityHistory, video: Boolean, peerLabel: String) {
    val latest = history.latest
    val samples = history.samples
    Column(
        Modifier
            .widthIn(max = 340.dp)
            .clip(cardShape)
            .background(CallQualityColors.glass)
            .border(0.5.dp, CallQualityColors.edge, cardShape)
            .padding(horizontal = 10.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text(CallQuality.heading(history), color = CallQualityColors.text, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        QualityRow("Voice", CallQuality.voiceText(history, peerLabel), "Voice levels over the last five seconds") {
            voiceWave(history.levels, CallQuality.LEVEL_SAMPLES)
        }
        if (!history.live) return@Column
        val network = CallQuality.describeNetwork(latest)
        QualityRow("Network", network, "Round-trip time over the last minute; ticks mark packet loss. $network") {
            sparkline(
                samples.map { it.rttMs },
                floor = 100.0,
                marks = samples.map { CallQualityColors.loss(CallQuality.lossMark(it.lossPct)) },
            )
        }
        if (!video) return@Column
        val incoming = CallQuality.videoInText(latest)
        QualityRow("Video in", incoming, "Incoming bitrate over the last minute. $incoming") {
            sparkline(samples.map { it.recvKbps }, floor = 200.0)
        }
        val outgoing = CallQuality.videoOutText(latest)
        QualityRow("Video out", outgoing, "Outgoing bitrate over the last minute. $outgoing") {
            sparkline(samples.map { it.sendKbps }, floor = 200.0)
        }
    }
}

@Composable
private fun QualityRow(label: String, value: String, description: String, draw: DrawScope.() -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = CallQualityColors.muted, fontSize = 11.sp, maxLines = 1, modifier = Modifier.width(58.dp))
        Canvas(Modifier.size(width = 120.dp, height = 22.dp).semantics { contentDescription = description }, onDraw = draw)
        Spacer(Modifier.width(8.dp))
        Text(value, color = CallQualityColors.text, fontSize = 11.sp, lineHeight = 14.sp, maxLines = 2, modifier = Modifier.weight(1f, fill = false))
    }
}

private fun DrawScope.signalBars(bars: Int, color: Color) {
    val gap = 1.5.dp.toPx()
    val width = (size.width - gap * 3) / 4
    for (i in 0 until 4) {
        val height = size.height * (i + 1) / 4
        drawRect(
            if (i < bars) color else CallQualityColors.idle,
            topLeft = Offset(i * (width + gap), size.height - height),
            size = Size(width, height),
        )
    }
}

private fun DrawScope.voiceWave(levels: List<CallLevel>, count: Int) {
    val px = 1.dp.toPx()
    val middle = size.height / 2
    val slot = size.width / count
    val bar = max(px, slot - px)
    val recent = levels.takeLast(count)
    val offset = count - recent.size
    drawRect(CallQualityColors.grid, topLeft = Offset(0f, middle - px / 2), size = Size(size.width, px))
    recent.forEachIndexed { i, entry ->
        val x = (offset + i) * slot
        val up = max(px / 2, entry.mic.toFloat() * (middle - px))
        drawRect(CallQualityColors.you, topLeft = Offset(x, middle - px / 2 - up), size = Size(bar, up))
        val peer = entry.peer ?: return@forEachIndexed
        val down = max(px / 2, peer.toFloat() * (middle - px))
        drawRect(CallQualityColors.them, topLeft = Offset(x, middle + px / 2), size = Size(bar, down))
    }
}

private fun DrawScope.sparkline(values: List<Double?>, floor: Double, marks: List<Color?> = emptyList()) {
    val px = 1.dp.toPx()
    val count = CallQuality.STATS_SAMPLES
    val slot = size.width / (count - 1)
    val offset = count - values.size
    drawRect(CallQualityColors.grid, topLeft = Offset(0f, size.height - px), size = Size(size.width, px))
    marks.forEachIndexed { i, mark ->
        if (mark != null) drawRect(mark, topLeft = Offset((offset + i) * slot - px / 2, 0f), size = Size(px, size.height - px))
    }
    val known = values.filterNotNull()
    if (known.isEmpty()) return
    val top = (max(floor, known.max()) * 1.15).takeIf { it > 0 } ?: 1.0
    val path = Path()
    var drawing = false
    values.forEachIndexed { i, value ->
        if (value == null) {
            drawing = false
            return@forEachIndexed
        }
        val x = (offset + i) * slot
        val y = size.height - 2 * px - (value / top).toFloat() * (size.height - 4 * px)
        if (drawing) path.lineTo(x, y) else path.moveTo(x, y)
        drawing = true
    }
    drawPath(
        path,
        CallQualityColors.line,
        style = Stroke(width = 1.5.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round),
    )
}
