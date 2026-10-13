package com.forkmesh.nativeapp.feature.glasses

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch

object GlassesFeature {
    private const val OWNER = "existing"

    fun install(app: App) {
        GlassesAlarm.ensureChannel(app)
        NavigationFeature.register(Route.Kind.GlassesAlert, OWNER) { GlassesAlertScreen() }
        AppLifecycle.scope.launch {
            combine(GlassesWatchService.state, AppLifecycle.foreground) { state, foreground -> foreground && (state.homing != null || state.tracked != null) }
                .distinctUntilChanged()
                .collect { if (it) GlassesCompass.start(app) else GlassesCompass.stop() }
        }
        if (GlassesWatchService.wantsWatching(app) && GlassesWatchService.scanPermitted(app)) {
            runCatching { GlassesWatchService.start(app) }
        }
    }
}
