package com.forkmesh.nativeapp.feature.djstudio

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableLongState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal object DjTheme {
    val backdrop = Brush.verticalGradient(listOf(Color(0xFF3E4249), Color(0xFF2B2E33), Color(0xFF24272B)))
    val bar = Brush.verticalGradient(listOf(Color(0xFF26292D), Color(0xFF1C1E21)))
    val control = Brush.verticalGradient(listOf(Color(0xFF3A3D43), Color(0xFF27292D)))
    val panel = Color(0xFF1B1D20)
    val edge = Color.White.copy(alpha = 0.08f)
    val ink = Color.White
    val muted = Color(0xFFA3A8B0)
    val play = Color(0xFF66D93D)
    val record = Color(0xFFD0161F)
    val active = Color(0xFF4DA8FF)
    val playhead = Color(0xFFE0242C)
    val knobTop = Color(0xFFFAFAFB)
    val knobBottom = Color(0xFFBFC3C9)
    val knobLine = Color(0xFF8A8F97)
    val waveColors: List<Color> = (0 until 24).map { Color.hsv(it * 15f, 0.72f, 1f) }
}

@Composable
internal fun rememberFrameClock(running: Boolean = true): MutableLongState {
    val clock = remember { androidx.compose.runtime.mutableLongStateOf(0L) }
    LaunchedEffect(running) {
        withFrameNanos { clock.longValue = it }
        while (running) withFrameNanos { clock.longValue = it }
    }
    return clock
}

internal fun Modifier.djPress(
    label: String,
    onLongPress: (() -> Unit)? = null,
    onTap: () -> Unit,
): Modifier = composed {
    val haptics = LocalHapticFeedback.current
    var pressed by remember { mutableStateOf(false) }
    Modifier
        .scale(if (pressed) 0.96f else 1f)
        .semantics {
            contentDescription = label
            role = Role.Button
            onClick {
                onTap()
                true
            }
        }
        .pointerInput(onTap, onLongPress) {
            detectTapGestures(
                onPress = {
                    pressed = true
                    tryAwaitRelease()
                    pressed = false
                },
                onLongPress = onLongPress?.let { action ->
                    {
                        haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                        action()
                    }
                },
                onTap = {
                    haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                    onTap()
                },
            )
        }
}

@Composable
internal fun DjRoundButton(
    size: Dp,
    icon: String,
    label: String,
    lit: Boolean = false,
    onLongPress: (() -> Unit)? = null,
    onTap: () -> Unit,
) {
    Box(
        Modifier
            .size(size)
            .shadow(2.dp, CircleShape)
            .clip(CircleShape)
            .background(DjTheme.control)
            .border(if (lit) 1.5.dp else 0.75.dp, if (lit) DjTheme.active.copy(alpha = 0.9f) else Color.White.copy(alpha = 0.1f), CircleShape)
            .djPress(label, onLongPress, onTap),
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = size * 0.5f, tint = if (lit) DjTheme.active else DjTheme.muted)
    }
}

internal fun Modifier.djPanel(shape: Shape = RoundedCornerShape(5.dp)): Modifier =
    this.clip(shape).background(DjTheme.panel).border(1.dp, DjTheme.edge, shape)

internal fun Modifier.djControl(shape: Shape = RoundedCornerShape(4.dp)): Modifier =
    this.shadow(2.dp, shape).clip(shape).background(DjTheme.control).border(1.dp, Color.Black.copy(alpha = 0.6f), shape)

@Composable
internal fun DjThumb(url: String, modifier: Modifier, contentScale: ContentScale = ContentScale.Crop) {
    var image by remember(url) { mutableStateOf<ImageBitmap?>(DjImages.cached(url)) }
    LaunchedEffect(url) {
        if (image == null) image = withContext(Dispatchers.IO) { DjImages.load(url) }
    }
    Box(modifier.background(Color.White.copy(alpha = 0.08f))) {
        image?.let { Image(it, contentDescription = null, modifier = Modifier.matchParentSize(), contentScale = contentScale) }
    }
}
