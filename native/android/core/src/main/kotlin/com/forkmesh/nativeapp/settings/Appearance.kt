package com.forkmesh.nativeapp.settings

object AppearanceKeys {
    const val THEME_MODE = "appearance/themeMode"
    const val ACCENT = "appearance/accent"
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
    blue("Blue", 0xFF007AFF, 0x1A007AFF, 0xFF0A84FF, 0x33164A8C),
    cyan("Cyan", 0xFF0E7490, 0x1A0E7490, 0xFF22D3EE, 0x330E7490),
    teal("Teal", 0xFF0D9488, 0x1A0D9488, 0xFF2DD4BF, 0x33115E59),
    green("Green", 0xFF15803D, 0x1A15803D, 0xFF4ADE80, 0x3315803D),
    lime("Lime", 0xFF4D7C0F, 0x1A4D7C0F, 0xFFA3E635, 0x334D7C0F),
    amber("Amber", 0xFFB45309, 0x1AB45309, 0xFFFBBF24, 0x33B45309),
    orange("Orange", 0xFFC2410C, 0x1AC2410C, 0xFFFB923C, 0x33C2410C),
    red("Red", 0xFFDC2626, 0x1ADC2626, 0xFFF87171, 0x33B91C1C),
    pink("Pink", 0xFFDB2777, 0x1ADB2777, 0xFFF472B6, 0x33BE185D),
    violet("Violet", 0xFF9333EA, 0x1A9333EA, 0xFFC084FC, 0x337E22CE),
    purple("Purple", 0xFF6161F2, 0x1A6161F2, 0xFF6C63FF, 0x332F2A74),
    graphite("Graphite", 0xFF52525B, 0x1A52525B, 0xFFA1A1AA, 0x3352525B);

    fun accent(dark: Boolean): Long = if (dark) onDark else light

    fun accentSubtle(dark: Boolean): Long = if (dark) subtleOnDark else subtle

    companion object {
        val fallback: AccentChoice = blue

        fun fromName(name: String?): AccentChoice = entries.firstOrNull { it.name == name } ?: fallback
    }
}

data class Appearance(val themeMode: ThemeModeChoice = ThemeModeChoice.system, val accent: AccentChoice = AccentChoice.fallback) {
    fun isDark(systemDark: Boolean): Boolean = themeMode.isDark(systemDark)

    companion object {
        fun read(get: (String) -> String?): Appearance =
            Appearance(ThemeModeChoice.fromName(get(AppearanceKeys.THEME_MODE)), AccentChoice.fromName(get(AppearanceKeys.ACCENT)))
    }
}
