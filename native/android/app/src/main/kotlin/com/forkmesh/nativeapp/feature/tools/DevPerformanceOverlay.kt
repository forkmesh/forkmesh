package com.forkmesh.nativeapp.feature.tools

import android.app.Activity
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.tools.DevPerformanceText
import com.forkmesh.nativeapp.tools.PerformanceEvent
import com.forkmesh.nativeapp.tools.PerformanceMonitorService
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.ForkMeshTheme

object DevPerformanceOverlay {
    private const val TAG = "fm-dev-performance-overlay"

    fun attach(activity: Activity, monitor: PerformanceMonitorService) {
        if (!monitor.enabled) return
        val root = activity.findViewById<ViewGroup>(android.R.id.content) ?: return
        if (root.findViewWithTag<ComposeView>(TAG) != null) return
        val view = ComposeView(activity).apply {
            tag = TAG
            setContent { ForkMeshTheme(messenger = false) { Hud(monitor) } }
        }
        root.addView(view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    }
}

@Composable
private fun Hud(monitor: PerformanceMonitorService) {
    monitor.observe()
    val event = monitor.latestEvent
    var showLog by remember { mutableStateOf(false) }
    var position by remember { mutableStateOf<IntOffset?>(null) }
    var chipSize by remember { mutableStateOf(IntSize.Zero) }
    val density = LocalDensity.current
    val insets = WindowInsets.safeDrawing.asPaddingValues()
    if (event == null && !showLog) return
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val layerHeight = maxHeight
        val bounds = with(density) { IntSize(maxWidth.roundToPx(), maxHeight.roundToPx()) }
        val marginPx = with(density) { FmSpace.x2.roundToPx() }
        val topPx = with(density) { insets.calculateTopPadding().roundToPx() }
        val bottomPx = with(density) { insets.calculateBottomPadding().roundToPx() }
        fun clamp(p: IntOffset): IntOffset {
            val maxX = maxOf(marginPx, bounds.width - chipSize.width - marginPx)
            val minY = topPx + marginPx
            val maxY = maxOf(minY, bounds.height - chipSize.height - bottomPx - marginPx)
            return IntOffset(p.x.coerceIn(marginPx, maxX), p.y.coerceIn(minY, maxY))
        }
        if (event != null) {
            val at = position?.let(::clamp) ?: IntOffset(
                bounds.width - chipSize.width - with(density) { FmSpace.x3.roundToPx() },
                topPx + marginPx,
            )
            Row(
                Modifier
                    .offset { at }
                    .onSizeChanged { chipSize = it }
                    .pointerInput(bounds) {
                        detectDragGestures { change, amount ->
                            change.consume()
                            val from = position ?: at
                            position = clamp(IntOffset(from.x + amount.x.toInt(), from.y + amount.y.toInt()))
                        }
                    }
                    .shadow(6.dp, RoundedCornerShape(FmRadius.full))
                    .background(Fm.colors.warning, RoundedCornerShape(FmRadius.full))
                    .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { showLog = true }
                    .semantics { contentDescription = "Open performance monitor" }
                    .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("speed_rounded", size = 16.dp, tint = Color.White)
                Spacer(Modifier.width(FmSpace.x1))
                Text(DevPerformanceText.durationLabel(event), color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.W800)
            }
        }
        if (showLog) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.38f))
                    .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { showLog = false }
                    .padding(FmSpace.x4),
                contentAlignment = Alignment.Center,
            ) {
                PerformanceDialog(monitor, maxHeight = layerHeight * 0.78f) { showLog = false }
            }
        }
    }
}

@Composable
private fun PerformanceDialog(monitor: PerformanceMonitorService, maxHeight: androidx.compose.ui.unit.Dp, onClose: () -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val events = monitor.events
    Column(
        Modifier
            .widthIn(max = 640.dp)
            .heightIn(max = maxHeight)
            .fillMaxWidth()
            .shadow(28.dp, RoundedCornerShape(FmRadius.lg))
            .background(c.bgRaised, RoundedCornerShape(FmRadius.lg))
            .border(1.dp, c.border, RoundedCornerShape(FmRadius.lg))
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
    ) {
        Row(Modifier.padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x3, bottom = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
            Text("Performance monitor", modifier = Modifier.weight(1f), fontSize = 20.sp, fontWeight = FontWeight.W700, color = c.textPrimary)
            FmTextButton(onClick = { monitor.clear() }) {
                FmIcon("delete_outline", size = 18.dp, tint = c.accent)
                Spacer(Modifier.width(FmSpace.x1))
                Text("Clear")
            }
        }
        if (events.isEmpty()) {
            FmEmptyState("speed_rounded", "No slow events", message = DevPerformanceText.emptyMessage(monitor))
        } else {
            LazyColumn(Modifier.weight(1f, fill = false).padding(horizontal = FmSpace.x4)) {
                items(events) { EventCard(it) }
            }
        }
        monitor.stallLogPath?.let { path ->
            SelectionContainer(Modifier.padding(horizontal = FmSpace.x4)) {
                Text(DevPerformanceText.loggedTo(path), fontSize = 11.sp, color = c.textSecondary)
            }
        }
        Row(Modifier.padding(FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
            FmTextButton(onClick = { copyToClipboard(context, DevPerformanceText.formatLog(monitor.events)) }) {
                FmIcon("copy_rounded", size = 18.dp, tint = c.accent)
                Spacer(Modifier.width(FmSpace.x1))
                Text("Copy")
            }
            Spacer(Modifier.weight(1f))
            AccountFilledButton("Close", "close_rounded", onClick = onClose)
        }
    }
}

@Composable
private fun EventCard(event: PerformanceEvent) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxWidth()
            .padding(bottom = FmSpace.x2)
            .background(c.bgBase, RoundedCornerShape(FmRadius.md))
            .border(1.dp, c.border, RoundedCornerShape(FmRadius.md))
            .padding(FmSpace.x3),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(event.name, modifier = Modifier.weight(1f), style = Fm.text.labelMedium.copy(color = c.textPrimary))
            Text(DevPerformanceText.durationLabel(event), style = Fm.text.labelMedium.copy(color = if (event.isStall) c.warning else c.textSecondary))
        }
        Text(DevPerformanceText.cardSubtitle(event), style = Fm.text.bodySmall.copy(color = c.textTertiary))
        val details = DevPerformanceText.cardDetails(event)
        if (details.isNotEmpty()) {
            Spacer(Modifier.padding(top = FmSpace.x1))
            SelectionContainer { Text(details, style = Fm.mono(color = c.textSecondary)) }
        }
    }
}
