package com.forkmesh.nativeapp.ui.kit

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.gestures.ScrollableState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmMotion
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

@Composable
fun FmSkeleton(
    modifier: Modifier = Modifier,
    width: Dp? = null,
    height: Dp = 12.dp,
    radius: Dp = FmRadius.sm,
) {
    val colors = Fm.colors
    val base = colors.bgOverlay
    val highlight = colors.bgElevated
    val progress: State<Float> = if (Fm.reducedMotion) {
        remember { mutableFloatStateOf(0f) }
    } else {
        rememberInfiniteTransition(label = "FmSkeleton").animateFloat(
            initialValue = 0f,
            targetValue = 1f,
            animationSpec = infiniteRepeatable(tween(1100, easing = LinearEasing), RepeatMode.Reverse),
            label = "shimmer",
        )
    }
    Box(
        modifier
            .then(if (width != null) Modifier.width(width) else Modifier.fillMaxWidth())
            .height(height)
            .clearAndSetSemantics {}
            .drawBehind {
                drawRoundRect(lerpSrgb(base, highlight, progress.value), cornerRadius = CornerRadius(radius.toPx()))
            },
    )
}

private fun lerpSrgb(a: Color, b: Color, t: Float) = Color(
    red = a.red + (b.red - a.red) * t,
    green = a.green + (b.green - a.green) * t,
    blue = a.blue + (b.blue - a.blue) * t,
    alpha = a.alpha + (b.alpha - a.alpha) * t,
)

private val titleWidths = listOf(148.dp, 196.dp, 122.dp, 172.dp)
private val subtitleWidths = listOf(96.dp, 132.dp, 110.dp, 84.dp)

@Composable
fun FmSkeletonRow(
    modifier: Modifier = Modifier,
    index: Int = 0,
    hasLeading: Boolean = true,
    minHeight: Dp = 60.dp,
) {
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = minHeight)
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (hasLeading) {
            FmSkeleton(width = 38.dp, height = 38.dp, radius = FmRadius.md)
            Spacer(Modifier.width(FmSpace.x3))
        }
        Column {
            FmSkeleton(width = titleWidths[index % titleWidths.size], height = 13.dp)
            Spacer(Modifier.height(FmSpace.x2))
            FmSkeleton(width = subtitleWidths[index % subtitleWidths.size], height = 10.dp)
        }
    }
}

@Composable
fun FmSkeletonList(
    modifier: Modifier = Modifier,
    rows: Int = 5,
    hasLeading: Boolean = true,
    label: String = "Loading",
) {
    Layout(
        content = {
            Column {
                for (i in 0 until rows) {
                    if (i > 0) FmRowDivider()
                    FmSkeletonRow(index = i, hasLeading = hasLeading)
                }
            }
        },
        modifier = modifier
            .semantics {
                contentDescription = label
                liveRegion = LiveRegionMode.Polite
            }
            .clipToBounds(),
    ) { measurables, constraints ->
        val placeable = measurables.first().measure(constraints.copy(minHeight = 0, maxHeight = Constraints.Infinity))
        val width = placeable.width.coerceIn(constraints.minWidth, constraints.maxWidth)
        val height = placeable.height.coerceIn(constraints.minHeight, constraints.maxHeight)
        layout(width, height) { placeable.place(0, 0) }
    }
}

@Composable
fun FmEdgeFadeScroll(
    state: ScrollableState,
    modifier: Modifier = Modifier,
    fadeWidth: Dp = 32.dp,
    content: @Composable () -> Unit,
) {
    Box(
        modifier
            .graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }
            .drawWithContent {
                drawContent()
                val atStart = !state.canScrollBackward
                val atEnd = !state.canScrollForward
                if (atStart && atEnd) return@drawWithContent
                val fade = (fadeWidth.toPx() / size.width).coerceIn(0f, .5f)
                drawRect(
                    Brush.horizontalGradient(
                        0f to Color.Transparent,
                        (if (atStart) 0f else fade) to Color.Black,
                        (if (atEnd) 1f else 1f - fade) to Color.Black,
                        1f to Color.Transparent,
                    ),
                    blendMode = BlendMode.DstIn,
                )
            },
    ) {
        content()
    }
}

@Composable
fun FmEdgeFadeScroll(
    modifier: Modifier = Modifier,
    fadeWidth: Dp = 32.dp,
    builder: @Composable (ScrollState) -> Unit,
) {
    val state = rememberScrollState()
    FmEdgeFadeScroll(state = state, modifier = modifier, fadeWidth = fadeWidth) { builder(state) }
}

private val SwitchOutEasing = CubicBezierEasing(0.325f, 0.81f, 0.45f, 0.945f)

@Composable
fun <T> FmSwap(
    stateKey: T,
    modifier: Modifier = Modifier,
    durationMillis: Int? = null,
    content: @Composable (T) -> Unit,
) {
    val duration = FmMotion.effective(durationMillis ?: FmMotion.normal)
    AnimatedContent(
        targetState = stateKey,
        modifier = modifier,
        transitionSpec = {
            (
                fadeIn(tween(duration, easing = FmMotion.easeOutCubic)) togetherWith
                    fadeOut(tween(duration, easing = SwitchOutEasing))
                ).using(null)
        },
        contentAlignment = Alignment.TopCenter,
        label = "FmSwap",
    ) { key ->
        content(key)
    }
}

@Composable
fun FmSliverFadeIn(
    stateKey: Any?,
    modifier: Modifier = Modifier,
    durationMillis: Int? = null,
    content: @Composable () -> Unit,
) {
    val duration = FmMotion.effective(durationMillis ?: FmMotion.normal)
    val opacity = remember { Animatable(1f) }
    val fadingKey = remember { mutableStateOf(stateKey) }
    LaunchedEffect(stateKey) {
        if (fadingKey.value == stateKey) return@LaunchedEffect
        opacity.snapTo(0f)
        fadingKey.value = stateKey
        opacity.animateTo(1f, tween(duration, easing = FmMotion.easeOutCubic))
    }
    Box(
        modifier.graphicsLayer {
            alpha = if (fadingKey.value != stateKey) 0f else opacity.value
        },
    ) {
        content()
    }
}
