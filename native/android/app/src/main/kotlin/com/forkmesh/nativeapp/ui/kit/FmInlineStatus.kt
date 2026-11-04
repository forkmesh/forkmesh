package com.forkmesh.nativeapp.ui.kit

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon

@Stable
class FmStatus {
    var message by mutableStateOf<String?>(null)
        private set
    var isError by mutableStateOf(false)
        private set
    var actionLabel by mutableStateOf<String?>(null)
        private set
    private var onAction: (() -> Unit)? = null

    fun show(message: String, actionLabel: String? = null, onAction: (() -> Unit)? = null) =
        set(message, false, actionLabel, onAction)

    fun error(message: String, actionLabel: String? = null, onAction: (() -> Unit)? = null) =
        set(message, true, actionLabel, onAction)

    fun report(message: String, isError: Boolean) = set(message, isError, null, null)

    fun clear() {
        message = null
        isError = false
        actionLabel = null
        onAction = null
    }

    fun runAction() {
        val action = onAction
        clear()
        action?.invoke()
    }

    private fun set(message: String, isError: Boolean, actionLabel: String?, onAction: (() -> Unit)?) {
        this.message = message
        this.isError = isError
        this.actionLabel = if (onAction != null) actionLabel else null
        this.onAction = onAction
    }
}

@Composable
fun rememberFmStatus(): FmStatus = remember { FmStatus() }

@Composable
fun FmInlineStatus(status: FmStatus, modifier: Modifier = Modifier, dismissible: Boolean = true) {
    val message = status.message ?: return
    FmInlineStatusLine(
        message = message,
        isError = status.isError,
        modifier = modifier,
        actionLabel = status.actionLabel,
        onAction = status::runAction,
        onDismiss = if (dismissible) status::clear else null,
    )
}

@Composable
fun FmInlineStatusLine(
    message: String?,
    isError: Boolean,
    modifier: Modifier = Modifier,
    actionLabel: String? = null,
    onAction: (() -> Unit)? = null,
    onDismiss: (() -> Unit)? = null,
) {
    if (message.isNullOrEmpty()) return
    val c = Fm.colors
    val context = LocalContext.current
    val haptics = FmHaptics.current
    Row(
        modifier
            .fillMaxWidth()
            .clickable(onClickLabel = "Open in composer") {
                haptics.tap()
                FmMessageHandoff.send(context, message)
                onDismiss?.invoke()
            }
            .semantics { liveRegion = if (isError) LiveRegionMode.Assertive else LiveRegionMode.Polite },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            message,
            modifier = Modifier.weight(1f),
            style = Fm.text.bodySmall.copy(color = if (isError) c.danger else c.textSecondary),
        )
        if (actionLabel != null && onAction != null) {
            FmTextButton(onClick = onAction) { Text(actionLabel) }
        }
        if (onDismiss != null) {
            FmCountdownClose(
                key = message,
                millis = if (isError) FmNoticeTiming.ERROR_MILLIS else FmNoticeTiming.INFO_MILLIS,
                tint = if (isError) c.danger else c.textTertiary,
                onDismiss = onDismiss,
            )
        }
    }
}

object FmNoticeTiming {
    const val INFO_MILLIS = 6_000
    const val ERROR_MILLIS = 10_000
}

@Composable
fun FmCountdownClose(key: String, millis: Int, tint: Color, onDismiss: () -> Unit) {
    val left = remember(key) { Animatable(1f) }
    val dismiss by rememberUpdatedState(onDismiss)
    LaunchedEffect(key) {
        left.animateTo(0f, tween(durationMillis = millis, easing = LinearEasing))
        dismiss()
    }
    val track = Fm.colors.border
    FmIconButton(label = "Dismiss", onPressed = onDismiss, color = Fm.colors.textTertiary) {
        Box(Modifier.size(22.dp), contentAlignment = Alignment.Center) {
            Canvas(Modifier.size(22.dp)) {
                val stroke = 1.5.dp.toPx()
                val inset = stroke / 2f
                val ring = Size(size.width - stroke, size.height - stroke)
                drawArc(track, 0f, 360f, false, Offset(inset, inset), ring, style = Stroke(stroke))
                drawArc(tint, -90f, 360f * left.value, false, Offset(inset, inset), ring, style = Stroke(stroke, cap = StrokeCap.Round))
            }
            FmIcon("close", size = 10.dp, tint = Fm.colors.textTertiary)
        }
    }
}
