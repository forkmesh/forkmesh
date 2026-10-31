package com.forkmesh.nativeapp.ui.kit

import android.app.Activity
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.scale
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmMotion
import com.forkmesh.nativeapp.ui.theme.ForkMeshTheme
import kotlinx.coroutines.delay

object FmOnboarding {
    val canvas = Color(0xFF050506)
    val surface = Color(0xFF141416)
    val raised = Color(0xFF29292C)
    val hairline = Color.White.copy(alpha = 0.09f)
    val secondaryText = Color(0xFF8D8D94)
    const val EXIT_MS = 160
    const val ENTER_MS = 140
    const val REVEAL_MS = 700
}

enum class FmPillKind { Primary, Dark }

@Composable
fun FmForcedDark(content: @Composable () -> Unit) {
    val view = LocalView.current
    DisposableEffect(view) {
        val window = (view.context as? Activity)?.window
        val bars = window?.let { WindowCompat.getInsetsController(it, view) }
        val before = bars?.isAppearanceLightStatusBars
        bars?.isAppearanceLightStatusBars = false
        onDispose {
            if (before != null) bars.isAppearanceLightStatusBars = before
        }
    }
    ForkMeshTheme(dark = true, content = content)
}

@Composable
fun FmPillButton(
    label: String,
    onClick: (() -> Unit)?,
    modifier: Modifier = Modifier,
    kind: FmPillKind = FmPillKind.Primary,
) {
    val haptics = FmHaptics.current
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val still = Fm.reducedMotion
    val grow by animateFloatAsState(if (pressed && !still) 1.03f else 1f, tween(140, easing = FmMotion.easeOutCubic), label = "pill-press")
    val shape = CircleShape
    val fill = when (kind) {
        FmPillKind.Primary -> Brush.verticalGradient(0f to Color.White, 0.55f to Color(0xFFF4F4F6), 1f to Color(0xFFD3D3D8))
        FmPillKind.Dark -> SolidColor(FmOnboarding.raised)
    }
    Box(
        modifier
            .fillMaxWidth()
            .scale(grow)
            .heightIn(min = 58.dp)
            .graphicsLayer { alpha = if (onClick == null) 0.55f else 1f }
            .clip(shape)
            .background(fill, shape)
            .border(1.dp, Color.White.copy(alpha = if (kind == FmPillKind.Primary) 0.6f else 0.05f), shape)
            .clickable(interactionSource = interaction, indication = null, enabled = onClick != null, role = Role.Button) {
                haptics.tap()
                onClick?.invoke()
            },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            label,
            maxLines = 1,
            style = Fm.text.titleMedium.copy(
                color = if (kind == FmPillKind.Primary) Color.Black else Color.White,
                fontWeight = FontWeight.SemiBold,
            ),
        )
    }
}

@Composable
fun FmPillTextButton(label: String, onClick: () -> Unit, modifier: Modifier = Modifier, color: Color = Color.White) {
    val haptics = FmHaptics.current
    Box(
        modifier
            .heightIn(min = 44.dp)
            .clip(RoundedCornerShape(22.dp))
            .clickable(role = Role.Button) {
                haptics.tap()
                onClick()
            }
            .padding(horizontal = 12.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, style = Fm.text.labelLarge.copy(color = color, fontWeight = FontWeight.Medium))
    }
}

@Composable
fun FmPillField(
    label: String,
    value: String,
    onValueChange: (String) -> Unit,
    hint: String,
    modifier: Modifier = Modifier,
    obscure: Boolean = false,
    keyboardOptions: KeyboardOptions = KeyboardOptions.Default,
    keyboardActions: KeyboardActions = KeyboardActions.Default,
) {
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val shape = RoundedCornerShape(20.dp)
    val style = Fm.text.bodyLarge.copy(color = Color.White)
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(
            label,
            modifier = Modifier.clearAndSetSemantics {},
            style = Fm.text.labelLarge.copy(color = Color.White, fontWeight = FontWeight.Medium),
        )
        CompositionLocalProvider(LocalFmFieldLabel provides label) {
            BasicTextField(
                value = value,
                onValueChange = onValueChange,
                modifier = Modifier
                    .fillMaxWidth()
                    .semantics { contentDescription = label },
                textStyle = style,
                singleLine = true,
                cursorBrush = SolidColor(Color.White),
                interactionSource = interaction,
                visualTransformation = if (obscure) PasswordVisualTransformation() else VisualTransformation.None,
                keyboardOptions = keyboardOptions,
                keyboardActions = keyboardActions,
                decorationBox = { inner ->
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(min = 58.dp)
                            .background(FmOnboarding.surface.copy(alpha = 0.92f), shape)
                            .border(1.dp, if (focused) Color.White.copy(alpha = 0.42f) else FmOnboarding.hairline, shape)
                            .padding(horizontal = 20.dp),
                        contentAlignment = Alignment.CenterStart,
                    ) {
                        if (value.isEmpty()) Text(hint, style = style.copy(color = Color(0xFF6E6E75)))
                        inner()
                    }
                },
            )
        }
    }
}

@Composable
fun FmRevealLine(
    text: String,
    modifier: Modifier = Modifier,
    style: TextStyle = Fm.text.displaySmall,
    color: Color = Color.White,
    textAlign: TextAlign? = null,
    delayMs: Int = 0,
    durationMs: Int = FmOnboarding.REVEAL_MS,
) {
    val still = Fm.reducedMotion
    val progress = remember { Animatable(if (still) 1f else 0f) }
    LaunchedEffect(Unit) {
        if (!still) progress.animateTo(1f, tween(durationMs, delayMs, FmMotion.easeOutCubic))
    }
    Text(
        text,
        modifier = modifier
            .graphicsLayer {
                val left = 1f - progress.value
                translationY = left * 12.dp.toPx()
                rotationZ = left * 1.8f
                transformOrigin = androidx.compose.ui.graphics.TransformOrigin(0f, 0.5f)
                compositingStrategy = CompositingStrategy.Offscreen
            }
            .drawWithContent {
                drawContent()
                val edge = size.width * 1.7f * progress.value
                drawRect(
                    Brush.horizontalGradient(
                        0f to Color.Black,
                        0.62f to Color.Black,
                        1f to Color.Transparent,
                        startX = edge - size.width * 1.7f,
                        endX = edge,
                    ),
                    blendMode = BlendMode.DstIn,
                )
            },
        textAlign = textAlign,
        style = style.copy(color = color),
    )
}

@Composable
fun FmRevealTitle(
    lines: List<String>,
    modifier: Modifier = Modifier,
    alignment: Alignment.Horizontal = Alignment.CenterHorizontally,
    style: TextStyle = Fm.text.displaySmall,
    delayMs: Int = 0,
) {
    Column(
        modifier.semantics(mergeDescendants = true) {
            heading()
            contentDescription = lines.joinToString(" ")
        },
        horizontalAlignment = alignment,
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        lines.forEachIndexed { index, line ->
            FmRevealLine(line, style = style, delayMs = delayMs + index * 160)
        }
    }
}

@Composable
fun Modifier.fmReveal(delayMs: Int = 0, rise: Dp = 18.dp, scale: Float = 0.97f, from: Float = 0f): Modifier {
    val still = Fm.reducedMotion
    val progress = remember { Animatable(if (still) 1f else 0f) }
    LaunchedEffect(Unit) {
        if (!still) progress.animateTo(1f, tween(550, delayMs, FmMotion.easeOutCubic))
    }
    return graphicsLayer {
        val shown = progress.value
        alpha = from + (1f - from) * shown
        translationY = (1f - shown) * rise.toPx()
        val k = scale + (1f - scale) * shown
        scaleX = k
        scaleY = k
    }
}

@Composable
fun Modifier.fmBrighten(delayMs: Int = 0): Modifier = fmReveal(delayMs, rise = 0.dp, scale = 1f, from = 0.32f)

@Composable
fun FmOnboardingProgress(count: Int, index: Int, modifier: Modifier = Modifier) {
    val duration = FmMotion.effective(450)
    Row(
        modifier.semantics(mergeDescendants = true) { contentDescription = "Step ${index + 1} of $count" },
        horizontalArrangement = Arrangement.spacedBy(5.dp),
    ) {
        repeat(count) { item ->
            val active = item == index
            val width by animateDpAsState(if (active) 34.dp else 7.dp, tween(duration, easing = FmMotion.easeOutCubic), label = "progress-width")
            val tint by animateFloatAsState(if (active) 1f else 0.3f, tween(duration), label = "progress-tint")
            Box(
                Modifier
                    .width(width)
                    .height(4.dp)
                    .background(Color.White.copy(alpha = tint), CircleShape),
            )
        }
    }
}

@Composable
fun FmOnboardingBackButton(onClick: () -> Unit, modifier: Modifier = Modifier) {
    val haptics = FmHaptics.current
    Box(
        modifier
            .size(44.dp)
            .clip(CircleShape)
            .background(Color(0xFF1A1A1D).copy(alpha = 0.72f), CircleShape)
            .border(1.dp, FmOnboarding.hairline, CircleShape)
            .clickable(role = Role.Button, onClickLabel = "Back") {
                haptics.tap()
                onClick()
            },
        contentAlignment = Alignment.Center,
    ) {
        FmIcon("arrow_back", size = 20.dp, tint = Color.White)
    }
}

@Composable
fun FmGlassIcon(size: Dp, modifier: Modifier = Modifier) {
    val outer = RoundedCornerShape(size * 0.27f)
    val innerSize = size * 0.78f
    val inner = RoundedCornerShape(innerSize * 0.24f)
    val glow = Color(0xFF8A5CFF).copy(alpha = 0.55f)
    Box(
        modifier
            .size(size)
            .clearAndSetSemantics {}
            .shadow(size * 0.2f, outer, ambientColor = glow, spotColor = glow)
            .background(Brush.linearGradient(listOf(Color.White, Color(0xFFEDE7FF), Color(0xFFC9BDF4))), outer)
            .border(1.5.dp, Color.White, outer),
        contentAlignment = Alignment.Center,
    ) {
        Image(
            painter = painterResource(R.drawable.fm_logo),
            contentDescription = null,
            contentScale = ContentScale.Fit,
            modifier = Modifier
                .size(innerSize)
                .clip(inner)
                .background(Color.Black)
                .border(1.dp, Color.Black.copy(alpha = 0.18f), inner)
                .padding(innerSize * 0.11f),
        )
    }
}

@Composable
fun FmAgentOrb(size: Dp, colors: List<Color>, modifier: Modifier = Modifier) {
    val still = Fm.reducedMotion
    val transition = rememberInfiniteTransition(label = "orb")
    val wave by transition.animateFloat(0f, 1f, infiniteRepeatable(tween(5000), RepeatMode.Reverse), label = "orb-turn")
    val turn = if (still) 0f else wave * 120f
    var blink by remember { mutableStateOf(false) }
    LaunchedEffect(still) {
        if (still) return@LaunchedEffect
        while (true) {
            delay(2800)
            blink = true
            delay(130)
            blink = false
        }
    }
    val lid by animateFloatAsState(if (blink) 0.14f else 1f, tween(90), label = "orb-blink")
    Box(modifier.size(size).clearAndSetSemantics {}, contentAlignment = Alignment.Center) {
        Canvas(Modifier.fillMaxSize()) {
            val r = size.toPx() / 2f
            val center = Offset(this.size.width / 2f, this.size.height / 2f)
            drawCircle(colors[0].copy(alpha = 0.14f), radius = r * 1.16f, center = center)
            drawContext.canvas.save()
            drawContext.transform.rotate(turn, center)
            drawCircle(Brush.sweepGradient(colors + colors[0], center), radius = r, center = center)
            drawContext.canvas.restore()
            drawCircle(
                Brush.radialGradient(
                    0f to Color.Black.copy(alpha = 0.9f),
                    0.55f to Color.Black.copy(alpha = 0.62f),
                    1f to Color.Transparent,
                    center = Offset(center.x, r * 0.84f),
                    radius = r * 1.04f,
                ),
                radius = r,
                center = center,
            )
            drawCircle(Color.White.copy(alpha = 0.26f), radius = r, center = center, style = Stroke(1.dp.toPx()))
        }
        Row(
            Modifier.offset(y = -size * 0.05f),
            horizontalArrangement = Arrangement.spacedBy(size * 0.09f),
        ) {
            repeat(2) {
                Box(
                    Modifier
                        .size(size * 0.17f, size * 0.105f)
                        .graphicsLayer { scaleY = lid }
                        .background(Color.White, CircleShape),
                )
            }
        }
    }
}

private const val MAP_COLUMNS = 80

private val mapLand: List<IntArray> = listOf(
    intArrayOf(16, 22, 29, 36, 55, 60),
    intArrayOf(12, 24, 27, 37, 52, 64),
    intArrayOf(8, 26, 28, 36, 44, 46, 50, 72),
    intArrayOf(4, 25, 30, 36, 42, 75),
    intArrayOf(3, 23, 26, 28, 31, 34, 35, 36, 41, 76),
    intArrayOf(4, 19, 24, 28, 32, 33, 40, 74),
    intArrayOf(6, 20, 23, 28, 38, 38, 40, 44, 46, 72),
    intArrayOf(9, 21, 24, 26, 37, 39, 41, 70),
    intArrayOf(10, 24, 25, 27, 40, 70, 72, 72),
    intArrayOf(12, 26, 39, 68),
    intArrayOf(12, 24, 40, 67, 71, 71),
    intArrayOf(13, 23, 38, 41, 43, 44, 45, 66, 70, 71),
    intArrayOf(13, 22, 38, 40, 44, 44, 46, 67, 68, 68, 70, 70),
    intArrayOf(14, 21, 37, 40, 42, 42, 48, 67),
    intArrayOf(15, 20, 37, 52, 54, 68),
    intArrayOf(16, 20, 36, 53, 56, 66),
    intArrayOf(17, 19, 21, 23, 36, 54, 55, 66, 67, 67),
    intArrayOf(18, 21, 24, 24, 36, 54, 56, 60, 62, 65),
    intArrayOf(19, 22, 36, 53, 57, 59, 62, 64, 68, 68),
    intArrayOf(21, 23, 36, 51, 57, 58, 63, 65, 68, 68),
    intArrayOf(23, 30, 37, 52, 58, 58, 64, 64),
    intArrayOf(24, 31, 38, 52, 64, 64, 67, 69),
    intArrayOf(24, 33, 41, 51, 63, 65, 66, 69),
    intArrayOf(24, 34, 42, 50, 64, 65, 67, 70),
    intArrayOf(24, 35, 43, 50, 70, 70, 73, 76),
    intArrayOf(25, 36, 43, 49, 66, 68, 72, 77),
    intArrayOf(26, 36, 44, 49),
    intArrayOf(26, 35, 44, 49, 51, 51, 68, 70, 72, 73),
    intArrayOf(27, 34, 44, 48, 50, 51, 66, 74),
    intArrayOf(28, 33, 45, 48, 50, 51, 65, 75),
    intArrayOf(28, 32, 45, 47, 50, 50, 65, 75),
    intArrayOf(28, 32, 45, 47, 65, 75),
    intArrayOf(28, 31, 46, 46, 66, 74),
    intArrayOf(28, 30, 68, 69, 71, 74),
    intArrayOf(27, 29, 72, 73, 78, 78),
    intArrayOf(27, 28, 73, 73, 77, 77),
    intArrayOf(27, 28),
    intArrayOf(27, 27),
    intArrayOf(27, 28),
)

@Composable
private fun FmWorldDots(modifier: Modifier = Modifier) {
    Canvas(modifier.graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }) {
        val step = size.width / MAP_COLUMNS
        val rowStep = step * 1.3f
        val radius = step * 0.31f
        val dot = Color.White.copy(alpha = 0.3f)
        mapLand.forEachIndexed { row, spans ->
            val shift = if (row % 2 == 0) 0f else step / 2f
            var index = 0
            while (index + 1 < spans.size) {
                for (column in spans[index]..spans[index + 1]) {
                    drawCircle(dot, radius, Offset(column * step + shift + radius, row * rowStep + radius))
                }
                index += 2
            }
        }
        drawRect(
            Brush.verticalGradient(
                0.1f to Color.Transparent,
                0.3f to Color.Black,
                0.56f to Color.Black,
                0.72f to Color.Transparent,
            ),
            blendMode = BlendMode.DstIn,
        )
    }
}

@Composable
fun FmAuroraBackground(modifier: Modifier = Modifier) {
    val still = Fm.reducedMotion
    val transition = rememberInfiniteTransition(label = "aurora")
    val wave by transition.animateFloat(0.72f, 1f, infiniteRepeatable(tween(3600), RepeatMode.Reverse), label = "aurora-dots")
    BoxWithConstraints(
        modifier
            .fillMaxSize()
            .clearAndSetSemantics {}
            .drawBehind {
                val w = size.width
                val h = size.height
                drawRect(
                    Brush.verticalGradient(
                        0f to Color(0xFF1F24E8),
                        0.1f to Color(0xFF4A2CF0),
                        0.2f to Color(0xFF9A3DE6),
                        0.27f to Color(0xFFD768C8),
                        0.33f to Color(0xFFF2A38C),
                        0.4f to Color(0xFF9A3A97),
                        0.47f to Color(0xFF3B124F),
                        0.55f to Color(0xFF0B0410),
                        0.62f to Color.Black,
                    ),
                )
                drawRect(
                    Brush.radialGradient(
                        listOf(Color(0xFF1422D8).copy(alpha = 0.85f), Color.Transparent),
                        center = Offset(w * 0.92f, h * 0.04f),
                        radius = w * 0.8f,
                    ),
                )
                drawRect(
                    Brush.radialGradient(
                        listOf(Color(0xFF1422D8).copy(alpha = 0.7f), Color.Transparent),
                        center = Offset(w * 0.06f, h * 0.02f),
                        radius = w * 0.65f,
                    ),
                )
                drawRect(
                    Brush.radialGradient(
                        listOf(Color.Black.copy(alpha = 0.62f), Color.Transparent),
                        center = Offset(w * 1.05f, h * 0.34f),
                        radius = w * 0.6f,
                    ),
                )
            },
    ) {
        val top = maxHeight * 0.055f
        Column(Modifier.fillMaxWidth()) {
            Spacer(Modifier.height(top))
            FmWorldDots(
                Modifier
                    .fillMaxWidth()
                    .aspectRatio(1f / 0.82f)
                    .graphicsLayer { alpha = if (still) 1f else wave },
            )
        }
    }
}

@Composable
fun FmOnboardingCanvas(modifier: Modifier = Modifier) {
    Box(
        modifier
            .fillMaxSize()
            .drawBehind {
                drawRect(FmOnboarding.canvas)
                drawRect(
                    Brush.radialGradient(
                        listOf(Color.White.copy(alpha = 0.035f), Color.Transparent),
                        center = Offset(size.width / 2f, size.height),
                        radius = size.width,
                    ),
                )
            },
    )
}
