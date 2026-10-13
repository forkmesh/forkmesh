package com.forkmesh.nativeapp.feature.glasses

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.glasses.GlassesHoming
import com.forkmesh.nativeapp.glasses.GlassesHomingReading
import com.forkmesh.nativeapp.glasses.GlassesRadarBlip
import com.forkmesh.nativeapp.glasses.GlassesRadarLayout
import com.forkmesh.nativeapp.glasses.NearbyDevice
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

@Composable
internal fun closenessColor(reading: GlassesHomingReading?): Color = when {
    reading == null || !reading.live -> Fm.colors.textTertiary
    reading.closeness >= 0.7 -> Fm.colors.danger
    reading.closeness >= 0.45 -> Fm.colors.warning
    else -> Fm.colors.accent
}

@Composable
internal fun GlassesArrow(angle: Double?, color: Color, size: Dp, modifier: Modifier = Modifier) {
    var shown by remember { mutableDoubleStateOf(angle ?: 0.0) }
    LaunchedEffect(angle) {
        if (angle != null) shown = GlassesHoming.unwrap(shown, angle)
    }
    val rotation by animateFloatAsState(shown.toFloat(), tween(250), label = "glasses-arrow")
    if (angle == null) {
        val pulse by rememberInfiniteTransition(label = "glasses-search").animateFloat(
            0.35f, 1f, infiniteRepeatable(tween(900), RepeatMode.Reverse), label = "glasses-search-alpha",
        )
        Canvas(modifier.size(size).alpha(pulse)) {
            val radius = this.size.minDimension / 2
            drawCircle(color, radius * 0.82f, style = Stroke(width = radius * 0.12f))
            drawCircle(color, radius * 0.18f)
        }
        return
    }
    Canvas(modifier.size(size).rotate(rotation)) {
        val w = this.size.width
        val h = this.size.height
        val arrow = Path().apply {
            moveTo(w / 2, h * 0.04f)
            lineTo(w * 0.86f, h * 0.92f)
            lineTo(w / 2, h * 0.72f)
            lineTo(w * 0.14f, h * 0.92f)
            close()
        }
        drawPath(arrow, color)
    }
}

@Composable
internal fun GlassesClosenessBar(closeness: Double, color: Color, modifier: Modifier = Modifier, height: Dp = 8.dp) {
    val fill by animateFloatAsState(closeness.toFloat().coerceIn(0f, 1f), tween(300), label = "glasses-closeness")
    Canvas(modifier.height(height).clip(RoundedCornerShape(FmRadius.full))) {
        drawRect(color.copy(alpha = 0.18f))
        if (fill > 0f) {
            drawRoundRect(
                color,
                size = this.size.copy(width = this.size.width * fill),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(this.size.height / 2),
            )
        }
    }
}

@Composable
internal fun GlassesHomingCard(reading: GlassesHomingReading) {
    val heading by GlassesCompass.heading.collectAsState()
    val color = closenessColor(reading)
    val arrow = if (reading.live) reading.arrow(heading) else null
    FmCard(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(104.dp).background(color.copy(alpha = 0.12f), CircleShape)
                    .semantics { contentDescription = arrow?.let { "${reading.label} likely ${GlassesHoming.clock(it)}" } ?: "Direction not known yet" },
                contentAlignment = Alignment.Center,
            ) {
                GlassesArrow(arrow, color, 72.dp)
            }
            Spacer(Modifier.width(16.dp))
            Column(Modifier.weight(1f)) {
                Text(reading.label, style = Fm.text.titleMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
                Text(reading.summary, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    GlassesClosenessBar(if (reading.live) reading.closeness else 0.0, color, Modifier.weight(1f), 10.dp)
                    Spacer(Modifier.width(8.dp))
                    Text(
                        if (reading.live) "${(reading.closeness * 100).roundToInt()}%" else "–",
                        style = Fm.text.labelMedium,
                        color = color,
                    )
                }
                Spacer(Modifier.height(8.dp))
                Text(reading.hint, style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            }
        }
    }
}

@Composable
internal fun NearbyDevicesSection(devices: List<NearbyDevice>, listening: Boolean, onPick: (NearbyDevice) -> Unit) {
    Column(Modifier.padding(start = 12.dp, end = 12.dp, top = 10.dp, bottom = 4.dp)) {
        FmSectionHeader("Devices", icon = "bluetooth", count = devices.size)
        if (devices.isEmpty()) {
            Text(
                if (listening) "Listening…" else "Open ForkMesh to scan.",
                style = Fm.text.bodySmall,
                color = Fm.colors.textTertiary,
                modifier = Modifier.padding(top = 4.dp),
            )
        }
        Column(Modifier.padding(top = 6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            devices.chunked(3).forEach { row ->
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    row.forEach { device -> DeviceTile(device, Modifier.weight(1f)) { onPick(device) } }
                    repeat(3 - row.size) { Spacer(Modifier.weight(1f)) }
                }
            }
        }
    }
}

@Composable
private fun DeviceTile(device: NearbyDevice, modifier: Modifier = Modifier, onClick: () -> Unit) {
    val signal = when {
        device.rssi >= -60 -> Fm.colors.danger
        device.rssi >= -75 -> Fm.colors.warning
        else -> Fm.colors.accent
    }
    Column(
        modifier
            .clip(RoundedCornerShape(FmRadius.lg))
            .background(signal.copy(alpha = if (Fm.colors.dark) 0.13f else 0.08f))
            .border(1.dp, signal.copy(alpha = 0.42f), RoundedCornerShape(FmRadius.lg))
            .clickable(onClick = onClick, onClickLabel = "Show how far it is and which way")
            .padding(horizontal = 7.dp, vertical = 6.dp)
            .semantics {
                role = Role.Button
                contentDescription = "${device.kind.label}, ${device.label}, ${device.address}, ${device.distance}, ${device.rssi} dBm"
            },
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon(device.kind.icon, size = 12.dp, tint = signal)
            Spacer(Modifier.width(4.dp))
            Text(
                device.kind.label,
                fontSize = 9.sp,
                color = Fm.colors.textPrimary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
        }
        Text(
            device.label,
            fontSize = 9.sp,
            fontWeight = FontWeight.SemiBold,
            color = Fm.colors.textPrimary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(top = 3.dp),
        )
        Text(
            device.address,
            fontSize = 8.sp,
            color = Fm.colors.textTertiary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Row(Modifier.padding(top = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(
                device.distance,
                fontSize = 8.sp,
                fontWeight = FontWeight.Bold,
                color = signal,
                maxLines = 1,
                modifier = Modifier.background(signal.copy(alpha = 0.16f), RoundedCornerShape(FmRadius.full))
                    .padding(horizontal = 5.dp, vertical = 1.dp),
            )
            Spacer(Modifier.weight(1f))
            Text("${device.rssi} dBm", fontSize = 8.sp, color = signal, maxLines = 1)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun NearbyDeviceSheet(device: NearbyDevice, tracked: GlassesHomingReading?, watching: Boolean, onDismiss: () -> Unit) {
    FmModalBottomSheet(onDismissRequest = onDismiss, showDragHandle = true) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(52.dp)
                    .background(Fm.colors.bgRaised, RoundedCornerShape(FmRadius.lg))
                    .border(1.dp, Fm.colors.border, RoundedCornerShape(FmRadius.lg)),
                contentAlignment = Alignment.Center,
            ) {
                FmIcon(device.kind.icon, size = 28.dp, tint = Fm.colors.textPrimary)
            }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(device.label, style = Fm.text.titleMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
                Text(
                    listOfNotNull(device.kind.label, device.maker).joinToString(" · "),
                    style = Fm.text.bodySmall,
                    color = Fm.colors.textSecondary,
                )
            }
        }
        if (tracked != null) {
            GlassesHomingCard(tracked)
        } else {
            Text(
                if (watching) "Listening for it…" else "Start watching to follow it.",
                style = Fm.text.bodyMedium,
                color = Fm.colors.textSecondary,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
        }
        Text(
            "Distance is a guess from signal strength. Walls, bodies and the device's own power all change it.",
            style = Fm.text.bodySmall,
            color = Fm.colors.textTertiary,
            modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 24.dp),
        )
    }
}

@Composable
internal fun GlassesRadar(blips: List<GlassesRadarBlip>, color: Color, dot: Color, alert: Color, size: Dp = 34.dp) {
    val sweep by rememberInfiniteTransition(label = "glasses-radar").animateFloat(
        0f, 360f, infiniteRepeatable(tween(2500, easing = LinearEasing)), label = "glasses-radar-sweep",
    )
    Canvas(Modifier.size(size)) {
        val radius = this.size.minDimension / 2
        val line = 1.dp.toPx()
        drawCircle(color.copy(alpha = 0.08f), radius)
        drawCircle(color.copy(alpha = 0.45f), radius - line / 2, style = Stroke(width = line))
        drawCircle(color.copy(alpha = 0.25f), radius * 0.5f, style = Stroke(width = line / 2))
        withTransform({ rotate(sweep) }) {
            drawCircle(
                Brush.sweepGradient(0f to color.copy(alpha = 0f), 0.7f to color.copy(alpha = 0f), 1f to color.copy(alpha = 0.55f)),
                radius,
            )
        }
        blips.forEach { blip ->
            val turn = Math.toRadians(blip.angle)
            val reach = radius * blip.distance.toFloat()
            val at = Offset(radius + reach * sin(turn).toFloat(), radius - reach * cos(turn).toFloat())
            drawCircle(if (blip.glasses) alert else dot, (if (blip.glasses) 3.dp else 1.5.dp).toPx(), at)
        }
        drawCircle(color, 1.5.dp.toPx())
    }
}

@Composable
internal fun GlassesRadarChip() {
    val state by GlassesWatchService.state.collectAsState()
    if (!state.watching) return
    val heading by GlassesCompass.heading.collectAsState()
    val glasses = state.nearby.size
    val count = glasses + state.others.size
    val alarmed = state.ringing || glasses > 0
    val blips = GlassesRadarLayout.blips(state.others, state.nearby, state.homing, heading)
    val label = if (glasses > 0) {
        "Glasses Alert: ${if (glasses == 1) "smart glasses" else "$glasses smart glasses"} nearby, $count devices heard"
    } else {
        "Glasses Alert scanning, ${if (count == 1) "1 device" else "$count devices"} heard"
    }
    Box(Modifier.padding(start = FmSpace.x2)) {
        Box(
            Modifier.size(44.dp)
                .clip(CircleShape)
                .background(Fm.colors.bgBase)
                .border(1.dp, Fm.colors.border, CircleShape)
                .clickable(onClickLabel = "Open Glasses Alert") { NavigationFeature.open(Route.GlassesAlert) }
                .semantics {
                    role = Role.Button
                    contentDescription = label
                },
            contentAlignment = Alignment.Center,
        ) {
            GlassesRadar(blips, if (alarmed) Fm.colors.danger else Fm.colors.success, Fm.colors.textPrimary.copy(alpha = 0.75f), Fm.colors.danger)
        }
        Text(
            "$count",
            fontSize = 10.sp,
            fontWeight = FontWeight.Bold,
            color = Color.White,
            maxLines = 1,
            modifier = Modifier.align(Alignment.TopEnd)
                .offset(x = 6.dp, y = (-4).dp)
                .background(if (alarmed) Fm.colors.danger else Fm.colors.accent, RoundedCornerShape(FmRadius.full))
                .padding(horizontal = 4.dp)
                .clearAndSetSemantics {},
        )
    }
}
