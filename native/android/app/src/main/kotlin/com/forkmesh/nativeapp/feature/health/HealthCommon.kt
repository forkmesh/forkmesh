package com.forkmesh.nativeapp.feature.health

import android.os.Handler
import android.os.Looper
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.material3.TimePicker
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.material3.rememberTimePickerState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.health.HealthNotice
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneOffset
import kotlin.math.max
import kotlin.math.min

internal data class HealthChartBar(val label: String, val ratio: Double, val positive: Boolean, val met: Boolean)

internal suspend fun removeHealthPackageFromAccount(id: String) {
    withContext(Dispatchers.IO) { removeHealthPackageFromAccountNow(id) }
}

internal fun removeHealthPackageFromAccountNow(id: String) {
    val store = StatusPageFeature.store ?: return
    runCatching { store.uninstall(id) }.onSuccess { state ->
        Handler(Looper.getMainLooper()).post { StatusPageFeature.adopt(state) }
    }
}

@Composable
internal fun HealthRing(ratio: Double, met: Boolean, modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    val c = Fm.colors
    val track = c.border
    val arc = if (met) c.success else c.accent
    Box(modifier.size(128.dp), contentAlignment = Alignment.Center) {
        Canvas(Modifier.fillMaxSize()) {
            val stroke = 10.dp.toPx()
            val radius = (min(size.width, size.height) - stroke) / 2
            val topLeft = Offset(center.x - radius, center.y - radius)
            val arcSize = Size(radius * 2, radius * 2)
            drawCircle(track, radius, center, style = Stroke(stroke))
            if (ratio > 0) {
                drawArc(
                    arc, -90f, (360 * ratio).toFloat().coerceAtMost(360f), false, topLeft, arcSize,
                    style = Stroke(stroke, cap = StrokeCap.Round),
                )
            }
        }
        content()
    }
}

@Composable
internal fun HealthStatsCard(stats: List<Pair<String, String>>) {
    FmCard {
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            for (row in stats.chunked(2)) {
                Row(Modifier.fillMaxWidth()) {
                    for ((label, value) in row) {
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                            Text(label.uppercase(), style = TextStyle(fontSize = 11.sp, fontWeight = FontWeight.SemiBold), color = Fm.colors.textSecondary)
                            Text(value, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                        }
                    }
                    if (row.size == 1) Spacer(Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
internal fun HealthBarsChart(bars: List<HealthChartBar>, goalRatio: Double, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val measurer = rememberTextMeasurer()
    val labelStyle = TextStyle(fontSize = 9.sp, color = c.textSecondary)
    Canvas(modifier.fillMaxWidth().height(160.dp)) {
        if (bars.isEmpty()) return@Canvas
        val plotHeight = size.height - 16.dp.toPx()
        val slot = size.width / bars.size
        val width = max(2.dp.toPx(), min(12.dp.toPx(), slot - 3.dp.toPx()))
        for ((index, bar) in bars.withIndex()) {
            val height = if (bar.positive) max(2.dp.toPx(), plotHeight * min(1.0, bar.ratio).toFloat()) else 0f
            val x = index * slot + (slot - width) / 2
            drawRoundRect(
                if (bar.met) c.success else c.accent, Offset(x, plotHeight - height), Size(width, height), CornerRadius(2.dp.toPx()),
            )
            if (bar.label.isNotEmpty()) {
                val layout = measurer.measure(bar.label, labelStyle)
                val left = (x + width / 2 - layout.size.width / 2f).coerceIn(0f, max(0f, size.width - layout.size.width))
                drawText(layout, topLeft = Offset(left, size.height - layout.size.height))
            }
        }
        if (goalRatio > 0) {
            val y = plotHeight - plotHeight * min(1.0, goalRatio).toFloat()
            drawLine(
                c.warning, Offset(0f, y), Offset(size.width, y), strokeWidth = 1.5.dp.toPx(),
                pathEffect = PathEffect.dashPathEffect(floatArrayOf(5.dp.toPx(), 4.dp.toPx())),
            )
        }
    }
}

@Composable
internal fun HealthNoticeLine(notice: HealthNotice?, onDismiss: () -> Unit) {
    FmInlineStatusLine(notice?.message, notice?.isError == true, onDismiss = onDismiss)
}

@Composable
internal fun HealthInstallPanel(title: String, message: String, button: String, notice: HealthNotice?, onDismiss: () -> Unit, onInstall: () -> Unit) {
    Column(
        Modifier.fillMaxSize().padding(FmSpace.x5),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(title, textAlign = TextAlign.Center, style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
        Spacer(Modifier.height(FmSpace.x3))
        Text(message, textAlign = TextAlign.Center, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
        Spacer(Modifier.height(FmSpace.x4))
        AccountFilledButton(button, "download", onClick = onInstall)
        Spacer(Modifier.height(FmSpace.x3))
        HealthNoticeLine(notice, onDismiss)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun HealthDayPickerDialog(day: LocalDate, onDismiss: () -> Unit, onPicked: (LocalDate) -> Unit) {
    val state = rememberDatePickerState(initialSelectedDateMillis = day.atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli())
    FmSoundOnShow()
    DatePickerDialog(
        onDismissRequest = onDismiss,
        modifier = Modifier.fmTapSounds(),
        confirmButton = {
            FmTextButton(onClick = {
                val millis = state.selectedDateMillis
                if (millis == null) {
                    onDismiss()
                } else {
                    val picked = Instant.ofEpochMilli(millis).atZone(ZoneOffset.UTC).toLocalDate()
                    val today = LocalDate.now()
                    onPicked(if (picked.isAfter(today)) today else picked)
                }
            }) { Text("OK") }
        },
        dismissButton = { FmTextButton(onClick = onDismiss) { Text("Cancel") } },
    ) { DatePicker(state) }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun HealthTimePickerDialog(minutes: Int, onDismiss: () -> Unit, onPicked: (Int) -> Unit) {
    val state = rememberTimePickerState(initialHour = minutes / 60, initialMinute = minutes % 60)
    FmAlertDialog(
        onDismissRequest = onDismiss,
        actions = {
            FmTextButton(onClick = onDismiss) { Text("Cancel") }
            FmTextButton(onClick = { onPicked(state.hour * 60 + state.minute) }) { Text("OK") }
        },
    ) { TimePicker(state) }
}
