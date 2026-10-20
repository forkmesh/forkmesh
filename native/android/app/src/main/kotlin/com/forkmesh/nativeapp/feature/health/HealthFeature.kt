package com.forkmesh.nativeapp.feature.health

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.health.HealthApi
import com.forkmesh.nativeapp.health.HealthPrefs
import com.forkmesh.nativeapp.health.SleepSync
import com.forkmesh.nativeapp.health.SleepUsageLog
import com.forkmesh.nativeapp.health.StepsHealthSync
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.yield
import java.time.Instant

object HealthFeature {
    private const val OWNER = "existing"

    fun install(app: App) {
        NavigationFeature.register(Route.Kind.Steps, OWNER) { StepsScreen() }
        NavigationFeature.register(Route.Kind.Water, OWNER) { WaterScreen() }
        NavigationFeature.register(Route.Kind.Sleep, OWNER) { SleepScreen() }
        NavigationFeature.register(Route.Kind.SugarBreak, OWNER) { SugarBreakScreen() }
        NavigationFeature.register(Route.Kind.Stretch, OWNER) { StretchScreen() }
        NavigationFeature.register(Route.Kind.FoodGuide, OWNER) { FoodGuideScreen() }
        AppLifecycle.scope.launch {
            yield()
            stepsEnabledChanged()
            sleepEnabledChanged()
            AppLifecycle.foreground.collect { foreground ->
                sleepUsageChanged(foreground)
                StepsSensors.refresh()
                if (!foreground) {
                    WalkActivity.appPaused()
                } else {
                    WalkActivity.appResumed()
                    syncSteps(false)
                    syncSleep(false)
                }
            }
        }
    }

    fun sleepUsageChanged(foreground: Boolean) {
        val prefs = StepsSensors.prefs ?: return
        if (!HealthPrefs.bool(prefs, HealthPrefs.SLEEP)) return
        if (foreground) SleepUsageLog.begin(prefs, Instant.now()) else SleepUsageLog.end(prefs, Instant.now())
    }

    fun stepsEnabledChanged() {
        val prefs = StepsSensors.prefs
        StepsSensors.refresh()
        val on = prefs != null && HealthPrefs.bool(prefs, HealthPrefs.STEPS) &&
            StepsSensors.stepSensorAvailable && StepsSensors.permissionGranted()
        if (on) WalkActivity.start() else WalkActivity.stop()
    }

    fun sleepEnabledChanged() {
        StepsSensors.refresh()
    }

    suspend fun syncSteps(force: Boolean) {
        val api = Services.get<ApiService>() ?: return
        val prefs = StepsSensors.prefs ?: return
        val bridge = StepsSensorBridge()
        val now = Instant.now()
        withContext(Dispatchers.IO) {
            StepsHealthSync.runIfDue(
                bridge.available, { bridge.stepDays(3, now) }, prefs, HealthApi(api).steps(), now.toEpochMilli() / 1000.0, force,
            )
        }
    }

    suspend fun syncSleep(force: Boolean) {
        val api = Services.get<ApiService>() ?: return
        val prefs = StepsSensors.prefs ?: return
        val bridge = SleepSensorBridge()
        withContext(Dispatchers.IO) {
            SleepSync.runIfDue(bridge, prefs, HealthApi(api).sleep(), Instant.now(), 3, force = force)
        }
    }
}
