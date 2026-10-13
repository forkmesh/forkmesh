package com.forkmesh.nativeapp.platform

import android.content.Context
import com.forkmesh.nativeapp.settings.AccentChoice
import com.forkmesh.nativeapp.settings.Appearance
import com.forkmesh.nativeapp.settings.AppearanceKeys
import com.forkmesh.nativeapp.settings.ThemeModeChoice
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
        state.value = Appearance.read { p.getString(it) }
    }

    fun reload() {
        val p = prefs ?: return
        state.value = Appearance.read { p.getString(it) }
    }

    fun setThemeMode(mode: ThemeModeChoice) {
        prefs?.putString(AppearanceKeys.THEME_MODE, mode.name)
        state.value = state.value.copy(themeMode = mode)
    }

    fun setAccent(accent: AccentChoice) {
        prefs?.putString(AppearanceKeys.ACCENT, accent.name)
        state.value = state.value.copy(accent = accent)
    }
}
