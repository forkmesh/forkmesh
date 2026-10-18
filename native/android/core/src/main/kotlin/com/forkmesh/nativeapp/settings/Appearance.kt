package com.forkmesh.nativeapp.settings

object AppearanceKeys {
    const val THEME_MODE = "appearance/themeMode"
    const val ACCENT = "appearance/accent"
    const val ACCENT_CUSTOM = "appearance/accentCustom"
    const val SURFACE = "appearance/surface"
    const val READABLE_COLORS = "appearance/readableColors"
    const val THEME_COLORS = "appearance/themeColors"
}

enum class ThemeModeChoice(val label: String) {
    system("System"),
    light("Light"),
    dark("Dark");

    fun isDark(systemDark: Boolean): Boolean = when (this) {
        system -> systemDark
        light -> false
        dark -> true
    }

    companion object {
        fun fromName(name: String?): ThemeModeChoice = entries.firstOrNull { it.name == name } ?: system
    }
}

enum class AccentChoice(
    val label: String,
    val light: Long,
    val subtle: Long,
    val onDark: Long,
    val subtleOnDark: Long,
) {
    blue("Blue", 0xFF0066CC, 0x1A0066CC, 0xFF409CFF, 0x33164A8C),
    cyan("Cyan", 0xFF0E7490, 0x1A0E7490, 0xFF22D3EE, 0x330E7490),
    teal("Teal", 0xFF0F766E, 0x1A0F766E, 0xFF2DD4BF, 0x33115E59),
    green("Green", 0xFF147C3B, 0x1A147C3B, 0xFF4ADE80, 0x3315803D),
    lime("Lime", 0xFF4B780F, 0x1A4B780F, 0xFFA3E635, 0x334D7C0F),
    amber("Amber", 0xFFAF5109, 0x1AAF5109, 0xFFFBBF24, 0x33B45309),
    orange("Orange", 0xFFC0400C, 0x1AC0400C, 0xFFFB923C, 0x33C2410C),
    red("Red", 0xFFD22222, 0x1AD22222, 0xFFF87171, 0x33B91C1C),
    pink("Pink", 0xFFCB226D, 0x1ACB226D, 0xFFF472B6, 0x33BE185D),
    violet("Violet", 0xFF9333EA, 0x1A9333EA, 0xFFC084FC, 0x337E22CE),
    purple("Purple", 0xFF5856D6, 0x1A5856D6, 0xFF8B85FF, 0x332F2A74),
    graphite("Graphite", 0xFF52525B, 0x1A52525B, 0xFFA1A1AA, 0x3352525B);

    fun accent(dark: Boolean): Long = if (dark) onDark else light

    fun accentSubtle(dark: Boolean): Long = if (dark) subtleOnDark else subtle

    companion object {
        val fallback: AccentChoice = blue

        fun fromName(name: String?): AccentChoice = entries.firstOrNull { it.name == name } ?: fallback
    }
}

data class Appearance(val themeMode: ThemeModeChoice = ThemeModeChoice.system, val theme: AppTheme = AppTheme.standard) {
    val accent: AccentChoice get() = theme.accent

    fun isDark(systemDark: Boolean): Boolean = themeMode.isDark(systemDark)

    companion object {
        fun read(get: (String) -> String?): Appearance = read(get) { get(it)?.toBooleanStrictOrNull() }

        fun read(get: (String) -> String?, getBool: (String) -> Boolean?): Appearance =
            Appearance(ThemeModeChoice.fromName(get(AppearanceKeys.THEME_MODE)), readTheme(get, getBool))

        fun readTheme(get: (String) -> String?, getBool: (String) -> Boolean?): AppTheme {
            val accentName = get(AppearanceKeys.ACCENT)
            val edits = AppTheme.parseColors(get(AppearanceKeys.THEME_COLORS))
            val custom = if (accentName == AppTheme.CUSTOM_ACCENT_NAME) get(AppearanceKeys.ACCENT_CUSTOM)?.let(ThemeColor::parseHex) else null
            return AppTheme(
                surface = ThemeSurface.fromName(get(AppearanceKeys.SURFACE)),
                accent = AccentChoice.fromName(accentName),
                customAccent = custom,
                readable = getBool(AppearanceKeys.READABLE_COLORS) ?: true,
                lightColors = edits.light,
                darkColors = edits.dark,
            )
        }
    }
}
