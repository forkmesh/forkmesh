package com.forkmesh.nativeapp.ui.theme

import android.content.ContentResolver
import android.database.ContentObserver
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.LocalTextSelectionColors
import androidx.compose.foundation.text.selection.TextSelectionColors
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.hapticfeedback.HapticFeedback
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.settings.AppTheme
import com.forkmesh.nativeapp.settings.ThemeToken
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmMessengerScope
import com.forkmesh.nativeapp.ui.kit.FmSoundCues

object FmSpace {
    val x0 = 0.dp
    val x1 = 4.dp
    val x2 = 8.dp
    val x3 = 12.dp
    val x4 = 16.dp
    val x5 = 24.dp
    val x6 = 32.dp
    val x7 = 48.dp
    val x8 = 64.dp
}

object FmType {
    val hero = 40.sp
    val display = 28.sp
    val title = 20.sp
    val heading = 17.sp
    val body = 15.sp
    val small = 13.sp
    val meta = 12.sp
    val micro = 11.sp
    val regular = FontWeight.W400
    val medium = FontWeight.W500
    val emphasis = FontWeight.W600
    const val leadingTight = 1.2f
    const val leadingSnug = 1.35f
    const val leadingRelaxed = 1.5f

    const val tabular = "tnum"

    val mono = FontFamily.Monospace
}

object FmRadius {
    val sm = 4.dp
    val md = 8.dp
    val lg = 12.dp
    val full = 999.dp
}

object FmMotion {
    const val fast = 100
    const val normal = 200

    val easeOutCubic = CubicBezierEasing(0.215f, 0.61f, 0.355f, 1f)

    val easeInCubic = CubicBezierEasing(0.55f, 0.055f, 0.675f, 0.19f)

    @Composable
    @ReadOnlyComposable
    fun effective(durationMillis: Int): Int = if (LocalFmReducedMotion.current) 0 else durationMillis
}

object FmColors {
    val bgBase = Color(0xFFF7F7F7)
    val bgRaised = Color(0xFFFFFFFF)
    val bgOverlay = Color(0xFFF0F0F0)
    val bgElevated = Color(0xFFE8E8E8)

    val darkBgBase = Color(0xFF0B0B0C)
    val darkBgRaised = Color(0xFF141414)
    val darkBgOverlay = Color(0xFF202022)
    val darkBgElevated = Color(0xFF26262A)

    val textPrimary = Color(0xFF0A0A0A)
    val textSecondary = Color(0xB80A0A0A)
    val textTertiary = Color(0x990A0A0A)
    val textDisabled = Color(0x400A0A0A)

    val darkTextPrimary = Color(0xFFF4F4F5)
    val darkTextSecondary = Color(0xB3F4F4F5)
    val darkTextTertiary = Color(0x80F4F4F5)
    val darkTextDisabled = Color(0x4DF4F4F5)

    val accent = Color(0xFF0066CC)
    val accentSubtle = Color(0x1A0066CC)
    val accentOnDark = Color(0xFF409CFF)
    val accentSubtleOnDark = Color(0x33164A8C)
    val success = Color(0xFF0F7A3B)
    val successOnDark = Color(0xFF46E07F)
    val successBtn = textPrimary
    val successBtnHover = Color(0xFF2A2E36)
    val danger = Color(0xFFC93532)
    val dangerOnDark = Color(0xFFFF6B6B)
    val warning = Color(0xFF8F5A0A)
    val warningOnDark = Color(0xFFE5C84C)
    val offline = Color(0xFFA0A7B2)
    val offlineOnLight = Color(0xFF5F6672)
    val accentEdge = Color(0xFF8250DF)
    val darkBorder = Color(0xFF252529)
    val editorGutter = Color(0xFF101012)
    val editorCurrentLine = Color(0xFF202024)
    val codeKeyword = Color(0xFFC792EA)
    val codeType = Color(0xFFFF6E73)
    val codeString = Color(0xFFC3E88D)
    val codeComment = Color(0xFF777A82)
    val codeNumber = Color(0xFFF78C6C)
    val codeFunction = Color(0xFF82AAFF)

    val codeKeywordLight = Color(0xFF7C3AED)
    val codeTypeLight = Color(0xFFC2410C)
    val codeStringLight = Color(0xFF15803D)
    val codeCommentLight = Color(0xFF6B7280)
    val codeNumberLight = Color(0xFFB45309)
    val codeFunctionLight = Color(0xFF1D4ED8)

    val canvas = bgBase
    val surface = bgRaised
    val rail = bgRaised
    val border = Color(0xFFDADADA)
    val borderMuted = bgOverlay
    val text = textPrimary
    val textMuted = textSecondary

    val senders = listOf(
        Color(0xFFF85149),
        Color(0xFFE3B341),
        Color(0xFF3FB950),
        Color(0xFF58A6FF),
        Color(0xFFBC8CFF),
        Color(0xFFDB61A2),
        Color(0xFF39C5CF),
        Color(0xFFDB6D28),
    )

    fun senderColor(name: String): Color {
        var hash = 0
        for (unit in name) hash = (hash * 31 + unit.code) and 0x7fffffff
        return senders[hash % senders.size]
    }
}

@Immutable
data class FmPalette(
    val dark: Boolean,
    val bgBase: Color,
    val bgRaised: Color,
    val bgOverlay: Color,
    val bgElevated: Color,
    val textPrimary: Color,
    val textSecondary: Color,
    val textTertiary: Color,
    val textDisabled: Color,
    val accent: Color,
    val accentSubtle: Color,
    val success: Color,
    val danger: Color,
    val warning: Color,
    val offline: Color,
    val accentEdge: Color,
    val border: Color,
    val codeKeyword: Color,
    val codeType: Color,
    val codeString: Color,
    val codeComment: Color,
    val codeNumber: Color,
    val codeFunction: Color,
    val editorGutter: Color,
    val editorCurrentLine: Color,
    val onAccent: Color = Color.White,
) {
    val activeUnderline: Color get() = if (dark) Color.White else textPrimary

    val listSurface: Color get() = if (dark) bgBase else bgRaised

    val editorSurface: Color get() = if (dark) bgBase else bgRaised

    val editorActiveLine: Color get() = editorCurrentLine
}

private val LightBasePalette = FmPalette(
    dark = false,
    bgBase = FmColors.bgBase, bgRaised = FmColors.bgRaised, bgOverlay = FmColors.bgOverlay,
    bgElevated = FmColors.bgElevated,
    textPrimary = FmColors.textPrimary, textSecondary = FmColors.textSecondary,
    textTertiary = FmColors.textTertiary, textDisabled = FmColors.textDisabled,
    accent = FmColors.accent, accentSubtle = FmColors.accentSubtle, success = FmColors.success,
    danger = FmColors.danger, warning = FmColors.warning, offline = FmColors.offlineOnLight,
    accentEdge = FmColors.accentEdge, border = FmColors.borderMuted,
    codeKeyword = FmColors.codeKeywordLight, codeType = FmColors.codeTypeLight,
    codeString = FmColors.codeStringLight, codeComment = FmColors.codeCommentLight,
    codeNumber = FmColors.codeNumberLight, codeFunction = FmColors.codeFunctionLight,
    editorGutter = FmColors.bgOverlay, editorCurrentLine = FmColors.bgOverlay.copy(alpha = .72f),
)

private val DarkBasePalette = FmPalette(
    dark = true,
    bgBase = FmColors.darkBgBase, bgRaised = FmColors.darkBgRaised, bgOverlay = FmColors.darkBgOverlay,
    bgElevated = FmColors.darkBgElevated,
    textPrimary = FmColors.darkTextPrimary, textSecondary = FmColors.darkTextSecondary,
    textTertiary = FmColors.darkTextTertiary, textDisabled = FmColors.darkTextDisabled,
    accent = FmColors.accentOnDark, accentSubtle = FmColors.accentSubtleOnDark, success = FmColors.successOnDark,
    danger = FmColors.dangerOnDark, warning = FmColors.warningOnDark, offline = FmColors.offline,
    accentEdge = FmColors.accentEdge, border = FmColors.darkBorder,
    codeKeyword = FmColors.codeKeyword, codeType = FmColors.codeType, codeString = FmColors.codeString,
    codeComment = FmColors.codeComment, codeNumber = FmColors.codeNumber, codeFunction = FmColors.codeFunction,
    editorGutter = FmColors.editorGutter, editorCurrentLine = FmColors.editorCurrentLine,
)

fun fmPaletteFor(theme: AppTheme, dark: Boolean): FmPalette {
    val resolved = theme.resolve(dark)
    fun color(token: ThemeToken) = Color(resolved[token])
    return (if (dark) DarkBasePalette else LightBasePalette).copy(
        bgBase = color(ThemeToken.bgBase),
        bgRaised = color(ThemeToken.bgRaised),
        bgOverlay = color(ThemeToken.bgOverlay),
        bgElevated = color(ThemeToken.bgElevated),
        textPrimary = color(ThemeToken.textPrimary),
        textSecondary = color(ThemeToken.textSecondary),
        textTertiary = color(ThemeToken.textTertiary),
        textDisabled = Color(resolved.textDisabled),
        accent = color(ThemeToken.accent),
        accentSubtle = Color(resolved.accentSubtle),
        onAccent = color(ThemeToken.onAccent),
        success = color(ThemeToken.success),
        danger = color(ThemeToken.danger),
        warning = color(ThemeToken.warning),
        border = color(ThemeToken.border),
    )
}

val LightPalette = fmPaletteFor(AppTheme.standard, dark = false)

val DarkPalette = fmPaletteFor(AppTheme.standard, dark = true)

val LocalFmAppTheme = staticCompositionLocalOf { AppTheme.standard }

val LocalFmPalette = staticCompositionLocalOf { LightPalette }

val LocalFmTextTheme = staticCompositionLocalOf {
    fmTypography(LightPalette.textPrimary, LightPalette.textSecondary, LightPalette.textTertiary)
}

val LocalFmReducedMotion = staticCompositionLocalOf { false }

object Fm {
    val colors: FmPalette
        @Composable
        @ReadOnlyComposable
        get() = LocalFmPalette.current

    val text: Typography
        @Composable
        @ReadOnlyComposable
        get() = LocalFmTextTheme.current

    val dark: Boolean
        @Composable
        @ReadOnlyComposable
        get() = LocalFmPalette.current.dark

    val reducedMotion: Boolean
        @Composable
        @ReadOnlyComposable
        get() = LocalFmReducedMotion.current

    @Composable
    @ReadOnlyComposable
    fun mono(
        size: TextUnit = FmType.meta,
        weight: FontWeight = FmType.regular,
        color: Color? = null,
        height: Float = FmType.leadingSnug,
    ): TextStyle = MaterialDefaults.bodySmall.copy(
        fontFamily = FmType.mono,
        fontSize = size,
        fontWeight = weight,
        lineHeight = size * height,
        letterSpacing = 0.sp,
        color = color ?: LocalFmPalette.current.textPrimary,
    )
}

@Stable
class FmHaptics internal constructor(private val feedback: HapticFeedback, private val reducedMotion: Boolean) {
    fun selection(cue: SoundCue = SoundCue.SELECT) = perform(HapticFeedbackType.SegmentTick, cue)

    fun tap(cue: SoundCue = SoundCue.CLICK) = perform(HapticFeedbackType.VirtualKey, cue)

    fun commit(cue: SoundCue = SoundCue.SUCCESS) = perform(HapticFeedbackType.KeyboardTap, cue)

    fun failure(cue: SoundCue = SoundCue.ERROR) = perform(HapticFeedbackType.ContextClick, cue)

    fun ratchet(crisp: Boolean, caught: Boolean) {
        if (reducedMotion || !com.forkmesh.nativeapp.platform.AppearanceStore.hapticsEnabled) return
        feedback.performHapticFeedback(
            when {
                caught -> HapticFeedbackType.ContextClick
                crisp -> HapticFeedbackType.SegmentTick
                else -> HapticFeedbackType.SegmentFrequentTick
            },
        )
    }

    private fun perform(type: HapticFeedbackType, cue: SoundCue) {
        FmSoundCues.play(cue)
        if (!reducedMotion && com.forkmesh.nativeapp.platform.AppearanceStore.hapticsEnabled) feedback.performHapticFeedback(type)
    }

    companion object {
        val current: FmHaptics
            @Composable
            get() {
                val feedback = LocalHapticFeedback.current
                val reduced = LocalFmReducedMotion.current
                return remember(feedback, reduced) { FmHaptics(feedback, reduced) }
            }
    }
}

private val MaterialDefaults = Typography()

private fun fmTypography(primary: Color, muted: Color, tertiary: Color): Typography {
    fun TextStyle.scaled(
        size: TextUnit,
        weight: FontWeight = FmType.regular,
        height: Float = FmType.leadingSnug,
        tracking: TextUnit = 0.sp,
        color: Color = primary,
    ) = copy(fontSize = size, fontWeight = weight, lineHeight = size * height, letterSpacing = tracking, color = color)

    val m = MaterialDefaults
    return Typography(
        displayLarge = m.displayLarge.scaled(FmType.hero, FmType.emphasis, 1.02f, (-1).sp),
        displayMedium = m.displayMedium.copy(color = primary),
        displaySmall = m.displaySmall.scaled(FmType.display, FmType.emphasis, FmType.leadingTight, (-0.5).sp),
        headlineLarge = m.headlineLarge.copy(color = primary),
        headlineMedium = m.headlineMedium.copy(color = primary),
        headlineSmall = m.headlineSmall.copy(color = primary),
        titleLarge = m.titleLarge.scaled(FmType.title, FmType.emphasis, FmType.leadingTight, (-0.3).sp),
        titleMedium = m.titleMedium.scaled(FmType.heading, FmType.emphasis, FmType.leadingTight, (-0.2).sp),
        titleSmall = m.titleSmall.scaled(FmType.body, FmType.emphasis, FmType.leadingTight),
        bodyLarge = m.bodyLarge.scaled(FmType.body, height = FmType.leadingRelaxed),
        bodyMedium = m.bodyMedium.scaled(FmType.small),
        bodySmall = m.bodySmall.scaled(FmType.meta, color = muted),
        labelLarge = m.labelLarge.scaled(FmType.body, FmType.emphasis),
        labelMedium = m.labelMedium.scaled(FmType.small, FmType.emphasis),
        labelSmall = m.labelSmall.scaled(FmType.micro, FmType.emphasis, 1f, 0.5.sp, tertiary),
    )
}

val FmMaterialTypography = fmTypography(Color.Unspecified, Color.Unspecified, Color.Unspecified)

val FmShapes = Shapes(
    extraSmall = RoundedCornerShape(FmRadius.lg),
    small = RoundedCornerShape(FmRadius.md),
    medium = RoundedCornerShape(FmRadius.md),
    large = RoundedCornerShape(14.dp),
    extraLarge = RoundedCornerShape(14.dp),
)

private fun fmColorScheme(p: FmPalette): ColorScheme {
    val base = if (p.dark) darkColorScheme() else lightColorScheme()
    val onDefault = if (p.dark) Color.Black else Color.White
    val overlayLayer = if (p.dark) p.bgOverlay else p.bgRaised
    return base.copy(
        primary = p.textPrimary,
        onPrimary = if (p.dark) p.bgBase else Color.White,
        primaryContainer = p.accent,
        onPrimaryContainer = p.onAccent,
        inversePrimary = FmColors.accentOnDark,
        secondary = p.accent,
        onSecondary = Color.Black,
        secondaryContainer = p.bgOverlay,
        onSecondaryContainer = p.textPrimary,
        tertiary = p.accent,
        onTertiary = Color.Black,
        tertiaryContainer = p.accent,
        onTertiaryContainer = Color.Black,
        background = p.bgBase,
        onBackground = p.textPrimary,
        surface = p.bgRaised,
        onSurface = p.textPrimary,
        surfaceVariant = p.bgRaised,
        onSurfaceVariant = p.textPrimary,
        surfaceTint = Color.Transparent,
        inverseSurface = if (p.dark) p.bgElevated else p.textPrimary,
        inverseOnSurface = if (p.dark) p.textPrimary else Color.White,
        error = p.danger,
        onError = onDefault,
        errorContainer = p.danger,
        onErrorContainer = onDefault,
        outline = p.border,
        outlineVariant = p.border,
        scrim = Color.Black,
        surfaceBright = p.bgRaised,
        surfaceDim = p.bgRaised,
        surfaceContainerLowest = p.bgRaised,
        surfaceContainerLow = overlayLayer,
        surfaceContainer = overlayLayer,
        surfaceContainerHigh = overlayLayer,
        surfaceContainerHighest = p.bgRaised,
    )
}

@Composable
private fun rememberReducedMotion(): Boolean {
    val resolver = LocalContext.current.contentResolver
    var reduced by remember(resolver) { mutableStateOf(readReducedMotion(resolver)) }
    DisposableEffect(resolver) {
        val observer = object : ContentObserver(Handler(Looper.getMainLooper())) {
            override fun onChange(selfChange: Boolean) {
                reduced = readReducedMotion(resolver)
            }
        }
        resolver.registerContentObserver(
            Settings.Global.getUriFor(Settings.Global.TRANSITION_ANIMATION_SCALE),
            false,
            observer,
        )
        onDispose { resolver.unregisterContentObserver(observer) }
    }
    return reduced
}

private fun readReducedMotion(resolver: ContentResolver): Boolean =
    Settings.Global.getString(resolver, Settings.Global.TRANSITION_ANIMATION_SCALE)?.toFloatOrNull() == 0f

@Composable
fun ForkMeshTheme(
    dark: Boolean = isSystemInDarkTheme(),
    theme: AppTheme = LocalFmAppTheme.current,
    messenger: Boolean = true,
    content: @Composable () -> Unit,
) {
    val palette = remember(dark, theme) { fmPaletteFor(theme, dark) }
    val reducedMotion = rememberReducedMotion()
    val textTheme = remember(palette) { fmTypography(palette.textPrimary, palette.textSecondary, palette.textTertiary) }
    val scheme = remember(palette) { fmColorScheme(palette) }
    val selection = remember(palette) {
        TextSelectionColors(handleColor = palette.accent, backgroundColor = palette.accent.copy(alpha = .28f))
    }
    CompositionLocalProvider(
        LocalFmAppTheme provides theme,
        LocalFmPalette provides palette,
        LocalFmTextTheme provides textTheme,
        LocalFmReducedMotion provides reducedMotion,
    ) {
        MaterialTheme(colorScheme = scheme, typography = FmMaterialTypography, shapes = FmShapes) {
            CompositionLocalProvider(
                LocalContentColor provides palette.textPrimary,
                LocalTextStyle provides FmMaterialTypography.bodyMedium,
                LocalTextSelectionColors provides selection,
            ) {
                if (messenger) FmMessengerScope(content) else content()
            }
        }
    }
}
