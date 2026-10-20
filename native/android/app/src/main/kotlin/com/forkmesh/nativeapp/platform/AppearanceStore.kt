package com.forkmesh.nativeapp.platform

import android.content.Context
import com.forkmesh.nativeapp.settings.AccentChoice
import com.forkmesh.nativeapp.settings.AppTheme
import com.forkmesh.nativeapp.settings.Appearance
import com.forkmesh.nativeapp.settings.AppearanceKeys
import com.forkmesh.nativeapp.settings.ThemeColor
import com.forkmesh.nativeapp.settings.ThemeModeChoice
import com.forkmesh.nativeapp.settings.ThemeSurface
import com.forkmesh.nativeapp.settings.Wallpaper
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object AppearanceStore {
    private var prefs: Prefs? = null
    private val state = MutableStateFlow(Appearance())

    val appearance: StateFlow<Appearance> = state.asStateFlow()

    @Volatile
    var hapticsEnabled: Boolean = true

    fun init(context: Context) {
        val p = prefs ?: Prefs(context.applicationContext).also { prefs = it }
        state.value = read(p)
    }

    fun reload() {
        val p = prefs ?: return
        state.value = read(p)
    }

    private fun read(p: Prefs): Appearance =
        Appearance.read({ p.getString(it) }, { p.getBool(it) }) { key -> if (p.contains(key)) p.getDouble(key, Wallpaper.DEFAULT_DIM) else null }

    fun setWallpaperImage(name: String) {
        val clean = Wallpaper.cleanName(name)
        prefs?.putString(AppearanceKeys.BACKGROUND_IMAGE, clean.ifEmpty { null })
        state.value = state.value.copy(wallpaper = state.value.wallpaper.withImage(clean))
    }

    fun previewWallpaperDim(dim: Double) {
        state.value = state.value.copy(wallpaper = state.value.wallpaper.withDim(dim))
    }

    fun setWallpaperDim(dim: Double) {
        val clamped = Wallpaper.clampDim(dim)
        prefs?.putDouble(AppearanceKeys.BACKGROUND_DIM, clamped)
        state.value = state.value.copy(wallpaper = state.value.wallpaper.withDim(clamped))
    }

    fun setThemeMode(mode: ThemeModeChoice) {
        prefs?.putString(AppearanceKeys.THEME_MODE, mode.name)
        state.value = state.value.copy(themeMode = mode)
    }

    fun setAccent(accent: AccentChoice) {
        setTheme(state.value.theme.chooseAccent(accent))
    }

    fun setTheme(theme: AppTheme) {
        prefs?.let { p ->
            p.putString(AppearanceKeys.ACCENT, theme.accentName)
            p.putString(AppearanceKeys.ACCENT_CUSTOM, theme.customAccent?.let(ThemeColor::hex))
            p.putString(AppearanceKeys.SURFACE, if (theme.surface == ThemeSurface.fallback) null else theme.surface.name)
            if (theme.readable) p.putString(AppearanceKeys.READABLE_COLORS, null) else p.putBool(AppearanceKeys.READABLE_COLORS, false)
            p.putString(AppearanceKeys.THEME_COLORS, theme.colorsJson().ifEmpty { null })
        }
        state.value = state.value.copy(theme = theme)
    }
}
