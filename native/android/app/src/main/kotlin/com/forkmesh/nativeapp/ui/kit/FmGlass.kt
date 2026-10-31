package com.forkmesh.nativeapp.ui.kit

import androidx.compose.ui.geometry.isSpecified
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.awaitEachGesture
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmIcon
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.Alignment
import androidx.compose.material3.Text
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.clickable
import androidx.compose.foundation.background
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.ui.layout.layout
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.BlurEffect
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ColorMatrix
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.TileMode
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.asComposeRenderEffect
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.graphics.layer.GraphicsLayer
import androidx.compose.ui.graphics.layer.drawLayer
import androidx.compose.ui.graphics.rememberGraphicsLayer
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm

@Stable
class FmBackdrop internal constructor(internal val layer: GraphicsLayer) {
    internal var origin by mutableStateOf(Offset.Zero)
}

val LocalFmBackdrop = staticCompositionLocalOf<FmBackdrop?> { null }

val LocalFmSolidGlass = staticCompositionLocalOf { false }

val LocalShellClearance = staticCompositionLocalOf { 0.dp }

@Composable
fun FmBackdropHost(modifier: Modifier = Modifier, overlay: @Composable BoxScope.() -> Unit = {}, content: @Composable BoxScope.() -> Unit) {
    val layer = rememberGraphicsLayer()
    val backdrop = remember(layer) { FmBackdrop(layer) }
    val context = LocalContext.current
    val solid = remember(context) {
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            runCatching { Settings.Secure.getInt(context.contentResolver, "high_text_contrast_enabled", 0) == 1 }.getOrDefault(false)
    }
    CompositionLocalProvider(LocalFmBackdrop provides backdrop, LocalFmSolidGlass provides solid) {
        Box(modifier.onGloballyPositioned { backdrop.origin = it.positionInRoot() }) {
            Box(
                Modifier.matchParentSize().drawWithContent {
                    layer.record { this@drawWithContent.drawContent() }
                    drawLayer(layer)
                },
            ) {
                CompositionLocalProvider(LocalFmBackdrop provides null) {
                    FmWallpaperLayer(Modifier.matchParentSize())
                    content()
                }
            }
            overlay()
        }
    }
}

object FmGlass {
    val blur: Dp = 18.dp
    const val SATURATION = 1.8f
    val edge: Dp = 0.5.dp
    val band: Dp = 18.dp
    val bend: Dp = 9.dp
    val edgeFade: Dp = 28.dp
    const val PRESS_SCALE = 1.08f

    internal const val LENS = """
        uniform shader content;
        uniform float2 size;
        uniform float radius;
        uniform float band;
        uniform float bend;

        float edgeDistance(float2 p, float2 extent, float r) {
            float2 q = abs(p) - extent + r;
            return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
        }

        half4 main(float2 xy) {
            float2 extent = size * 0.5;
            float2 p = xy - extent;
            float r = min(radius, min(extent.x, extent.y));
            float d = edgeDistance(p, extent, r);
            float t = clamp(1.0 + d / band, 0.0, 1.0);
            float dx = edgeDistance(p + float2(1.0, 0.0), extent, r) - edgeDistance(p - float2(1.0, 0.0), extent, r);
            float dy = edgeDistance(p + float2(0.0, 1.0), extent, r) - edgeDistance(p - float2(0.0, 1.0), extent, r);
            float2 n = float2(dx, dy);
            float len = length(n);
            if (len > 0.0001) n = n / len;
            return content.eval(xy - n * bend * t * t);
        }
    """

    internal fun lens(size: androidx.compose.ui.geometry.Size, radius: Float, band: Float, bend: Float, blur: Float): androidx.compose.ui.graphics.RenderEffect? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU || size.width <= 0f || size.height <= 0f) return null
        return runCatching {
            val shader = android.graphics.RuntimeShader(LENS)
            shader.setFloatUniform("size", size.width, size.height)
            shader.setFloatUniform("radius", radius)
            shader.setFloatUniform("band", band)
            shader.setFloatUniform("bend", bend)
            val blurred = android.graphics.RenderEffect.createBlurEffect(blur, blur, android.graphics.Shader.TileMode.CLAMP)
            android.graphics.RenderEffect.createChainEffect(
                android.graphics.RenderEffect.createRuntimeShaderEffect(shader, "content"),
                blurred,
            ).asComposeRenderEffect()
        }.getOrNull()
    }
}

@Composable
fun Modifier.fmGlass(shape: Shape, tint: Color? = null, shadow: Boolean = true, interactive: Boolean = false): Modifier {
    val c = Fm.colors
    val backdrop = LocalFmBackdrop.current
    val solid = LocalFmSolidGlass.current || backdrop == null
    val density = LocalDensity.current
    val blurPx = with(density) { FmGlass.blur.toPx() }
    val bandPx = with(density) { FmGlass.band.toPx() }
    val bendPx = with(density) { FmGlass.bend.toPx() }
    val sample = rememberGraphicsLayer()
    var position by remember { mutableStateOf(Offset.Zero) }
    val fill = when {
        tint != null -> tint
        solid -> c.bgRaised.copy(alpha = if (c.dark) 0.94f else 0.96f)
        else -> c.bgRaised.copy(alpha = if (c.dark) 0.3f else 0.34f)
    }
    val specular = Color.White.copy(alpha = if (c.dark) 0.06f else 0.22f)
    val rimTop = Color.White.copy(alpha = if (c.dark) 0.28f else 0.85f)
    val rimBottom = if (c.dark) Color.White.copy(alpha = 0.06f) else Color.Black.copy(alpha = 0.06f)
    val shadowColor = Color.Black.copy(alpha = if (c.dark) 0.45f else 0.10f)
    var pressed by remember { mutableStateOf(false) }
    var touch by remember { mutableStateOf(Offset.Unspecified) }
    val swell by androidx.compose.animation.core.animateFloatAsState(if (pressed) FmGlass.PRESS_SCALE else 1f, androidx.compose.animation.core.spring(dampingRatio = 0.55f, stiffness = 600f), label = "fmGlassSwell")
    val glow by androidx.compose.animation.core.animateFloatAsState(if (pressed) 1f else 0f, androidx.compose.animation.core.tween(if (pressed) 90 else 260), label = "fmGlassGlow")
    return this
        .then(if (interactive) Modifier.graphicsLayer { scaleX = swell; scaleY = swell } else Modifier)
        .onGloballyPositioned { position = it.positionInRoot() }
        .then(if (shadow) Modifier.shadow(12.dp, shape, clip = false, ambientColor = shadowColor, spotColor = shadowColor) else Modifier)
        .clip(shape)
        .drawBehind {
            if (!solid && backdrop != null) {
                val outline = shape.createOutline(size, layoutDirection, this)
                val radius = (outline as? androidx.compose.ui.graphics.Outline.Rounded)?.roundRect?.topLeftCornerRadius?.x ?: 0f
                sample.renderEffect = FmGlass.lens(size, radius, bandPx, bendPx, blurPx) ?: BlurEffect(blurPx, blurPx, TileMode.Clamp)
                sample.colorFilter = ColorFilter.colorMatrix(ColorMatrix().apply { setToSaturation(FmGlass.SATURATION) })
                sample.record {
                    drawRect(c.bgBase)
                    val shift = backdrop.origin - position
                    translate(shift.x, shift.y) { drawLayer(backdrop.layer) }
                }
                drawLayer(sample)
            }
            drawRect(fill)
            drawRect(Brush.verticalGradient(0f to specular, 0.5f to Color.Transparent))
            if (glow > 0f) {
                val at = if (touch.isSpecified) touch else center
                drawRect(Brush.radialGradient(listOf(Color.White.copy(alpha = (if (c.dark) 0.16f else 0.3f) * glow), Color.Transparent), at, size.maxDimension * 0.9f))
            }
        }
        .border(if (solid) FmGlass.edge else 1.dp, if (solid) (if (c.dark) Color.White.copy(alpha = 0.12f) else Color.Black.copy(alpha = 0.06f)).let { SolidColor(it) } else Brush.verticalGradient(listOf(rimTop, rimBottom)), shape)
        .then(
            if (interactive) {
                Modifier.pointerInput(Unit) {
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
                        touch = down.position
                        pressed = true
                        do {
                            val event = awaitPointerEvent(PointerEventPass.Initial)
                            event.changes.firstOrNull { it.id == down.id }?.let { touch = it.position }
                        } while (event.changes.any { it.pressed })
                        pressed = false
                    }
                }
            } else {
                Modifier
            },
        )
}

@Composable
fun FmGlassSurface(shape: Shape, modifier: Modifier = Modifier, tint: Color? = null, shadow: Boolean = true, content: @Composable BoxScope.() -> Unit) {
    Box(modifier.fmGlass(shape, tint, shadow), content = content)
}

@Composable
fun FmNavBar(
    title: String,
    onBack: (() -> Unit)?,
    modifier: Modifier = Modifier,
    titleAlpha: Float = 1f,
    actions: @Composable RowScope.() -> Unit = {},
) {
    val c = Fm.colors
    Box(
        modifier
            .fillMaxWidth()
            .background(c.bgBase)
            .windowInsetsPadding(fmSafeAreaInsets(bottom = false))
            .height(52.dp)
            .padding(horizontal = FmSpace.x4),
    ) {
        Text(
            title,
            Modifier.align(Alignment.Center).padding(horizontal = 72.dp).graphicsLayer { alpha = titleAlpha }.semantics { heading() },
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.titleMedium,
        )
        if (onBack != null) {
            Box(
                Modifier
                    .align(Alignment.CenterStart)
                    .size(44.dp)
                    .fmGlass(CircleShape, interactive = true)
                    .clickable(onClick = onBack)
                    .semantics {
                        role = Role.Button
                        contentDescription = "Back"
                    },
                contentAlignment = Alignment.Center,
            ) {
                FmIcon("chevron_left", size = 30.dp, tint = c.textPrimary)
            }
        }
        FmNavActions(Modifier.align(Alignment.CenterEnd), actions)
    }
}

internal object FmNavActionsRoom {
    const val MIN_ROOM = 184
    const val RESERVED = 52

    fun maxWidth(barWidth: Int, minRoom: Int, reserved: Int): Int =
        if (barWidth == androidx.compose.ui.unit.Constraints.Infinity) barWidth else maxOf(minRoom, barWidth - reserved).coerceAtMost(barWidth)
}

@Composable
private fun FmNavActions(modifier: Modifier, actions: @Composable RowScope.() -> Unit) {
    val scroll = androidx.compose.foundation.rememberScrollState(Int.MAX_VALUE)
    Layout(
        content = { Row(Modifier.horizontalScroll(scroll), verticalAlignment = Alignment.CenterVertically, content = actions) },
        modifier = modifier,
    ) { measurables, constraints ->
        val room = FmNavActionsRoom.maxWidth(constraints.maxWidth, FmNavActionsRoom.MIN_ROOM.dp.roundToPx(), FmNavActionsRoom.RESERVED.dp.roundToPx())
        val row = measurables.first().measure(constraints.copy(minWidth = 0, minHeight = 0, maxWidth = room))
        if (row.width == 0) {
            layout(0, 0) {}
        } else {
            layout(row.width, row.height) { row.place(0, 0) }
        }
    }
}

@Composable
fun Modifier.fmScrollEdge(): Modifier {
    val c = Fm.colors
    var scrolled by remember { mutableStateOf(false) }
    val fade by androidx.compose.animation.core.animateFloatAsState(if (scrolled) 1f else 0f, androidx.compose.animation.core.tween(160), label = "fmScrollEdge")
    val connection = remember {
        object : androidx.compose.ui.input.nestedscroll.NestedScrollConnection {
            override fun onPostScroll(
                consumed: Offset,
                available: Offset,
                source: androidx.compose.ui.input.nestedscroll.NestedScrollSource,
            ): Offset {
                if (consumed.y < 0f) scrolled = true
                if (available.y > 0f) scrolled = false
                return Offset.Zero
            }
        }
    }
    return this
        .nestedScroll(connection)
        .drawWithContent {
            drawContent()
            if (fade > 0f) {
                val depth = FmGlass.edgeFade.toPx()
                drawRect(
                    Brush.verticalGradient(0f to c.bgBase.copy(alpha = 0.94f * fade), 0.55f to c.bgBase.copy(alpha = 0.5f * fade), 1f to Color.Transparent, endY = depth),
                    size = androidx.compose.ui.geometry.Size(size.width, depth),
                )
            }
        }
}


@Stable
class FmLargeTitleState {
    var height by mutableFloatStateOf(0f)
    var offset by mutableFloatStateOf(0f)
    val collapsed: Float get() = if (height <= 0f) 0f else (-offset / height).coerceIn(0f, 1f)
    val barTitleAlpha: Float get() = ((collapsed - 0.7f) / 0.3f).coerceIn(0f, 1f)

    val connection = object : NestedScrollConnection {
        override fun onPreScroll(available: Offset, source: NestedScrollSource): Offset {
            if (available.y >= 0f || height <= 0f) return Offset.Zero
            val next = (offset + available.y).coerceAtLeast(-height)
            val used = next - offset
            offset = next
            return Offset(0f, used)
        }

        override fun onPostScroll(consumed: Offset, available: Offset, source: NestedScrollSource): Offset {
            if (available.y <= 0f) return Offset.Zero
            val next = (offset + available.y).coerceAtMost(0f)
            val used = next - offset
            offset = next
            return Offset(0f, used)
        }
    }
}

@Composable
fun rememberFmLargeTitle(): FmLargeTitleState = remember { FmLargeTitleState() }

@Composable
fun FmLargeTitleBox(title: String, state: FmLargeTitleState, modifier: Modifier = Modifier, content: @Composable BoxScope.() -> Unit) {
    val c = Fm.colors
    androidx.compose.foundation.layout.Column(modifier.nestedScroll(state.connection)) {
        Box(
            Modifier
                .fillMaxWidth()
                .clipToBounds()
                .layout { measurable, constraints ->
                    val placeable = measurable.measure(constraints)
                    state.height = placeable.height.toFloat()
                    val shown = (placeable.height + state.offset).toInt().coerceAtLeast(0)
                    layout(placeable.width, shown) { placeable.place(0, state.offset.toInt()) }
                },
        ) {
            Text(
                title,
                Modifier
                    .padding(start = FmSpace.x4, end = FmSpace.x4, top = 2.dp, bottom = 8.dp)
                    .graphicsLayer { alpha = 1f - state.collapsed }
                    .semantics { heading() },
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.headlineLarge.copy(fontSize = 34.sp, lineHeight = 41.sp, fontWeight = FontWeight.Bold, color = c.textPrimary),
            )
        }
        Box(Modifier.weight(1f).fillMaxWidth().fmScrollEdge(), content = content)
    }
}
