package com.forkmesh.nativeapp.ui.kit

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
import androidx.compose.ui.draw.clip
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
                content = content,
            )
            overlay()
        }
    }
}

object FmGlass {
    val blur: Dp = 24.dp
    const val SATURATION = 1.8f
    val edge: Dp = 0.5.dp
}

@Composable
fun Modifier.fmGlass(shape: Shape, tint: Color? = null, shadow: Boolean = true): Modifier {
    val c = Fm.colors
    val backdrop = LocalFmBackdrop.current
    val solid = LocalFmSolidGlass.current || backdrop == null
    val density = LocalDensity.current
    val blurPx = with(density) { FmGlass.blur.toPx() }
    val sample = rememberGraphicsLayer()
    var position by remember { mutableStateOf(Offset.Zero) }
    val fill = when {
        tint != null -> tint
        solid -> c.bgRaised.copy(alpha = if (c.dark) 0.94f else 0.96f)
        else -> c.bgRaised.copy(alpha = if (c.dark) 0.55f else 0.66f)
    }
    val specular = Color.White.copy(alpha = if (c.dark) 0.07f else 0.45f)
    val edge = if (c.dark) Color.White.copy(alpha = 0.12f) else Color.Black.copy(alpha = 0.06f)
    val shadowColor = Color.Black.copy(alpha = if (c.dark) 0.45f else 0.10f)
    return this
        .onGloballyPositioned { position = it.positionInRoot() }
        .then(if (shadow) Modifier.shadow(12.dp, shape, clip = false, ambientColor = shadowColor, spotColor = shadowColor) else Modifier)
        .clip(shape)
        .drawBehind {
            if (!solid && backdrop != null) {
                sample.renderEffect = BlurEffect(blurPx, blurPx, TileMode.Clamp)
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
        }
        .border(FmGlass.edge, edge, shape)
}

@Composable
fun FmGlassSurface(shape: Shape, modifier: Modifier = Modifier, tint: Color? = null, shadow: Boolean = true, content: @Composable BoxScope.() -> Unit) {
    Box(modifier.fmGlass(shape, tint, shadow), content = content)
}
