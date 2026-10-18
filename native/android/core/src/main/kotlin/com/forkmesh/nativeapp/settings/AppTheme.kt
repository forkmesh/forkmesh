package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.json.Json
import java.util.TreeMap
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.random.Random

enum class ThemeTokenGroup(val label: String) {
    surfaces("Surfaces"),
    text("Text"),
    accent("Accent"),
    status("Status");

    val tokens: List<ThemeToken> get() = ThemeToken.entries.filter { it.group == this }
}

enum class ThemeToken(val label: String, val group: ThemeTokenGroup) {
    bgBase("Background", ThemeTokenGroup.surfaces),
    bgRaised("Cards and bars", ThemeTokenGroup.surfaces),
    bgOverlay("Sheets and fields", ThemeTokenGroup.surfaces),
    bgElevated("Raised controls", ThemeTokenGroup.surfaces),
    border("Borders", ThemeTokenGroup.surfaces),
    textPrimary("Text", ThemeTokenGroup.text),
    textSecondary("Secondary text", ThemeTokenGroup.text),
    textTertiary("Hints and captions", ThemeTokenGroup.text),
    accent("Accent", ThemeTokenGroup.accent),
    onAccent("Text on accent", ThemeTokenGroup.accent),
    success("Success", ThemeTokenGroup.status),
    danger("Danger", ThemeTokenGroup.status),
    warning("Warning", ThemeTokenGroup.status);

    companion object {
        fun fromName(name: String?): ThemeToken? = entries.firstOrNull { it.name == name }
    }
}

enum class ThemeSurface(val label: String, private val light: LongArray, private val dark: LongArray) {
    forkmesh(
        "ForkMesh",
        longArrayOf(0xF7F7F7, 0xFFFFFF, 0xF0F0F0, 0xE8E8E8, 0xF0F0F0, 0x0A0A0A),
        longArrayOf(0x0B0B0C, 0x141414, 0x202022, 0x26262A, 0x252529, 0xF4F4F5),
    ),
    slate(
        "Slate",
        longArrayOf(0xF6F8FA, 0xFFFFFF, 0xEFF2F5, 0xE6EAEF, 0xE1E4E8, 0x1F2328),
        longArrayOf(0x010409, 0x0D1117, 0x161B22, 0x21262D, 0x30363D, 0xF0F6FC),
    ),
    zinc(
        "Zinc",
        longArrayOf(0xFAFAFA, 0xFFFFFF, 0xF4F4F5, 0xE4E4E7, 0xE4E4E7, 0x18181B),
        longArrayOf(0x09090B, 0x18181B, 0x27272A, 0x3F3F46, 0x27272A, 0xF4F4F5),
    ),
    stone(
        "Stone",
        longArrayOf(0xFAFAF9, 0xFFFFFF, 0xF5F5F4, 0xE7E5E4, 0xE7E5E4, 0x1C1917),
        longArrayOf(0x0C0A09, 0x1C1917, 0x292524, 0x44403C, 0x292524, 0xF5F5F4),
    ),
    pure(
        "Pure",
        longArrayOf(0xFFFFFF, 0xFFFFFF, 0xF2F2F2, 0xE8E8E8, 0xE5E5E5, 0x000000),
        longArrayOf(0x000000, 0x0A0A0A, 0x161616, 0x1F1F1F, 0x1F1F1F, 0xFFFFFF),
    );

    fun colors(dark: Boolean): Map<ThemeToken, Long> {
        val values = if (dark) this.dark else light
        return SURFACE_TOKENS.indices.associate { SURFACE_TOKENS[it] to values[it] }
    }

    companion object {
        val fallback: ThemeSurface = forkmesh

        private val SURFACE_TOKENS = listOf(
            ThemeToken.bgBase, ThemeToken.bgRaised, ThemeToken.bgOverlay, ThemeToken.bgElevated, ThemeToken.border, ThemeToken.textPrimary,
        )

        fun fromName(name: String?): ThemeSurface = entries.firstOrNull { it.name == name } ?: fallback
    }
}

data class Hsl(val hue: Double, val saturation: Double, val lightness: Double)

object ThemeColor {
    const val MINIMUM_CONTRAST: Double = 4.5

    fun hex(rgb: Long): String = "#%06x".format(rgb and 0xFFFFFF)

    fun parseHex(text: String): Long? {
        val digits = text.trim().removePrefix("#")
        if (digits.length != 6 || !digits.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }) return null
        return digits.toLongOrNull(16)
    }

    fun luminance(rgb: Long): Double {
        fun channel(shift: Int): Double {
            val value = ((rgb shr shift) and 0xFF).toDouble() / 255
            return if (value <= 0.03928) value / 12.92 else ((value + 0.055) / 1.055).pow(2.4)
        }
        return 0.2126 * channel(16) + 0.7152 * channel(8) + 0.0722 * channel(0)
    }

    fun contrast(a: Long, b: Long): Double {
        val first = luminance(a)
        val second = luminance(b)
        return (max(first, second) + 0.05) / (min(first, second) + 0.05)
    }

    fun mix(a: Long, b: Long, amount: Double): Long {
        fun part(shift: Int): Long {
            val from = ((a shr shift) and 0xFF).toDouble()
            val to = ((b shr shift) and 0xFF).toDouble()
            return clampByte(from + (to - from) * amount) shl shift
        }
        return part(16) or part(8) or part(0)
    }

    fun hsl(rgb: Long): Hsl {
        val red = ((rgb shr 16) and 0xFF).toDouble() / 255
        val green = ((rgb shr 8) and 0xFF).toDouble() / 255
        val blue = (rgb and 0xFF).toDouble() / 255
        val high = max(red, max(green, blue))
        val low = min(red, min(green, blue))
        val lightness = (high + low) / 2
        if (high <= low) return Hsl(0.0, 0.0, lightness)
        val delta = high - low
        val saturation = if (lightness > 0.5) delta / (2 - high - low) else delta / (high + low)
        var hue = when {
            high == red -> (green - blue) / delta + (if (green < blue) 6 else 0)
            high == green -> (blue - red) / delta + 2
            else -> (red - green) / delta + 4
        }
        hue *= 60
        return Hsl(hue, saturation, lightness)
    }

    fun fromHsl(hue: Double, saturation: Double, lightness: Double): Long {
        val chroma = saturation * min(lightness, 1 - lightness)
        fun part(n: Double): Long {
            val k = (n + hue / 30) % 12
            val value = lightness - chroma * max(-1.0, min(min(k - 3, 9 - k), 1.0))
            return clampByte(value * 255)
        }
        return (part(0.0) shl 16) or (part(8.0) shl 8) or part(4.0)
    }

    fun fromHsl(hsl: Hsl): Long = fromHsl(hsl.hue, hsl.saturation, hsl.lightness)

    fun passes(rgb: Long, surfaces: List<Long>, minimum: Double = MINIMUM_CONTRAST): Boolean =
        surfaces.all { contrast(rgb, it) >= minimum }

    fun readable(rgb: Long, surfaces: List<Long>, minimum: Double = MINIMUM_CONTRAST): Long {
        val first = surfaces.firstOrNull()
        if (first == null || passes(rgb, surfaces, minimum)) return rgb and 0xFFFFFF
        val darken = luminance(first) > 0.18
        val start = hsl(rgb)
        var low = if (darken) 0.0 else start.lightness
        var high = if (darken) start.lightness else 1.0
        var best: Long = if (darken) 0x000000 else 0xFFFFFF
        repeat(24) {
            val middle = (low + high) / 2
            val candidate = fromHsl(start.hue, start.saturation, middle)
            if (passes(candidate, surfaces, minimum)) {
                best = candidate
                if (darken) low = middle else high = middle
            } else {
                if (darken) high = middle else low = middle
            }
        }
        return best
    }

    fun label(fill: Long, preferred: Long): Long {
        if (contrast(fill, preferred) >= MINIMUM_CONTRAST) return preferred
        val white = 0xFFFFFFL
        val black = 0x0A0A0AL
        return if (contrast(fill, white) >= contrast(fill, black)) white else black
    }

    private fun clampByte(value: Double): Long = min(255L, max(0L, Math.round(value)))
}

data class ResolvedTheme(
    val dark: Boolean,
    val colors: Map<ThemeToken, Long>,
    val textDisabled: Long,
    val accentSubtle: Long,
) {
    operator fun get(token: ThemeToken): Long = colors[token] ?: 0xFF000000

    fun rgb(token: ThemeToken): Long = get(token) and 0xFFFFFF
}

data class ThemeEdits(val light: Map<ThemeToken, Long>, val dark: Map<ThemeToken, Long>)

data class AppTheme(
    val surface: ThemeSurface = ThemeSurface.fallback,
    val accent: AccentChoice = AccentChoice.fallback,
    val customAccent: Long? = null,
    val readable: Boolean = true,
    val lightColors: Map<ThemeToken, Long> = emptyMap(),
    val darkColors: Map<ThemeToken, Long> = emptyMap(),
) {
    val isStandard: Boolean get() = this == standard

    val accentName: String get() = if (customAccent == null) accent.name else CUSTOM_ACCENT_NAME

    fun edits(dark: Boolean): Map<ThemeToken, Long> = if (dark) darkColors else lightColors

    fun setColor(rgb: Long?, token: ThemeToken, dark: Boolean): AppTheme {
        val updated = edits(dark).toMutableMap()
        if (rgb == null) updated.remove(token) else updated[token] = rgb and 0xFFFFFF
        return if (dark) copy(darkColors = updated) else copy(lightColors = updated)
    }

    fun chooseAccent(accent: AccentChoice): AppTheme = copy(accent = accent, customAccent = null).clearEdits(ThemeTokenGroup.accent)

    fun chooseCustomAccent(rgb: Long): AppTheme = copy(customAccent = rgb and 0xFFFFFF).clearEdits(ThemeTokenGroup.accent)

    fun chooseSurface(surface: ThemeSurface): AppTheme =
        copy(surface = surface).clearEdits(ThemeTokenGroup.surfaces).clearEdits(ThemeTokenGroup.text)

    fun clearEdits(group: ThemeTokenGroup): AppTheme =
        copy(lightColors = lightColors.filterKeys { it.group != group }, darkColors = darkColors.filterKeys { it.group != group })

    fun resolve(dark: Boolean): ResolvedTheme {
        val edits = edits(dark)
        val base = surface.colors(dark).toMutableMap()
        base.putAll(edits.filterKeys { it.group == ThemeTokenGroup.surfaces || it == ThemeToken.textPrimary })
        val surfaces = listOf(base[ThemeToken.bgBase] ?: 0L, base[ThemeToken.bgRaised] ?: 0L, base[ThemeToken.bgOverlay] ?: 0L)
        val fix: (Long) -> Long = { rgb -> if (readable) ThemeColor.readable(rgb, surfaces) else rgb }
        val text = fix(base[ThemeToken.textPrimary] ?: 0L)
        val accentSeed = edits[ThemeToken.accent] ?: customAccent ?: ((if (dark) accent.onDark else accent.light) and 0xFFFFFF)
        val accentColor = fix(accentSeed)
        val statusSeeds = if (dark) longArrayOf(0x46E07F, 0xFF6B6B, 0xE5C84C) else longArrayOf(0x0F7A3B, 0xC93532, 0x8F5A0A)
        val preferredLabel = edits[ThemeToken.onAccent] ?: if (dark) (base[ThemeToken.bgBase] ?: 0L) else 0xFFFFFFL
        val onAccent = if (readable) ThemeColor.label(accentColor, preferredLabel) else preferredLabel
        val colors = LinkedHashMap<ThemeToken, Long>()
        for (token in ThemeTokenGroup.surfaces.tokens) colors[token] = opaque(base[token] ?: 0L)
        colors[ThemeToken.textPrimary] = opaque(text)
        colors[ThemeToken.textSecondary] = edits[ThemeToken.textSecondary]?.let(::opaque) ?: alpha(if (dark) 0xB3 else 0xB8, text)
        colors[ThemeToken.textTertiary] = edits[ThemeToken.textTertiary]?.let(::opaque) ?: alpha(if (dark) 0x80 else 0x99, text)
        colors[ThemeToken.accent] = opaque(accentColor)
        colors[ThemeToken.onAccent] = opaque(onAccent)
        colors[ThemeToken.success] = opaque(fix(edits[ThemeToken.success] ?: statusSeeds[0]))
        colors[ThemeToken.danger] = opaque(fix(edits[ThemeToken.danger] ?: statusSeeds[1]))
        colors[ThemeToken.warning] = opaque(fix(edits[ThemeToken.warning] ?: statusSeeds[2]))
        val subtle = when {
            !dark -> alpha(0x1A, accentColor)
            edits[ThemeToken.accent] == null && customAccent == null -> alpha(0x33, accent.subtleOnDark)
            else -> alpha(0x33, ThemeColor.mix(accentColor, 0x000000, 0.45))
        }
        return ResolvedTheme(dark, colors, alpha(if (dark) 0x4D else 0x40, text), subtle)
    }

    fun colorsJson(): String {
        if (lightColors.isEmpty() && darkColors.isEmpty()) return ""
        fun encode(colors: Map<ThemeToken, Long>): Map<String, String> =
            TreeMap<String, String>().apply { for ((token, rgb) in colors) put(token.name, ThemeColor.hex(rgb)) }
        val document = TreeMap<String, Map<String, String>>()
        if (darkColors.isNotEmpty()) document["dark"] = encode(darkColors)
        if (lightColors.isNotEmpty()) document["light"] = encode(lightColors)
        return Json.stringify(document)
    }

    companion object {
        const val CUSTOM_ACCENT_NAME: String = "custom"

        val standard: AppTheme = AppTheme()

        fun shuffled(random: Random = Random.Default): AppTheme {
            val hue = random.nextInt(360).toDouble()
            val surface = ThemeSurface.entries.random(random)
            return AppTheme(surface = surface, customAccent = ThemeColor.fromHsl(hue, 0.75, 0.5))
        }

        fun parseColors(json: String?): ThemeEdits {
            val document = json?.let { runCatching { Json.parse(it) }.getOrNull() } as? Map<*, *> ?: return ThemeEdits(emptyMap(), emptyMap())
            fun decode(value: Any?): Map<ThemeToken, Long> {
                val entries = value as? Map<*, *> ?: return emptyMap()
                val colors = LinkedHashMap<ThemeToken, Long>()
                for ((name, raw) in entries) {
                    val token = ThemeToken.fromName(name as? String) ?: continue
                    val rgb = (raw as? String)?.let(ThemeColor::parseHex) ?: continue
                    colors[token] = rgb
                }
                return colors
            }
            return ThemeEdits(decode(document["light"]), decode(document["dark"]))
        }

        private fun opaque(rgb: Long): Long = 0xFF000000 or (rgb and 0xFFFFFF)

        private fun alpha(value: Long, rgb: Long): Long = (value shl 24) or (rgb and 0xFFFFFF)

        private fun alpha(value: Int, rgb: Long): Long = alpha(value.toLong(), rgb)
    }
}
