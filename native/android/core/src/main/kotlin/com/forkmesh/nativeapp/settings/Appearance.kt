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
    purple("Purple", 0xFF6161F2, 0x1A6161F2, 0xFF6C63FF, 0x332F2A74),
    teal("Teal", 0xFF0D9488, 0x1A0D9488, 0xFF2DD4BF, 0x33115E59);

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
