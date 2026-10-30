package com.forkmesh.nativeapp.feature.genie

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.genie.GenieEdge
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

enum class GenieReaction(val duration: Double) {
    Wave(1.8), Cheer(1.6), Surprise(1.3), Nod(0.9), Eureka(1.4), Giggle(1.1),
}

data class GenieCue(val reaction: GenieReaction, val at: Double)

enum class GenieMood { Idle, Listening, Thinking, Speaking }

data class GenieSpot(val x: Double, val y: Double, val surface: GenieEdge)

object GenieSpiderState {
    const val HIDDEN_KEY = "genie/hidden"
    const val SCALE_KEY = "genie/scale"
    const val SPOT_X_KEY = "genie/spot_x"
    const val SPOT_Y_KEY = "genie/spot_y"
    const val SURFACE_KEY = "genie/surface"
    const val PARKED_KEY = "genie/parked"
    const val WANDERS_KEY = "genie/wanders"
    const val LINE_SECONDS = 5.0

    private var prefs: PrefStore? = null
    private val hiddenState = MutableStateFlow(false)
    private val scaleState = MutableStateFlow(1.0)
    private val parkedState = MutableStateFlow(false)
    private val wandersState = MutableStateFlow(true)
    private val lineState = MutableStateFlow<String?>(null)
    private val cueState = MutableStateFlow<GenieCue?>(null)
    private val moodState = MutableStateFlow(GenieMood.Idle)
    val hidden: StateFlow<Boolean> = hiddenState.asStateFlow()
    val scale: StateFlow<Double> = scaleState.asStateFlow()
    val parked: StateFlow<Boolean> = parkedState.asStateFlow()
    val wanders: StateFlow<Boolean> = wandersState.asStateFlow()
    val line: StateFlow<String?> = lineState.asStateFlow()
    val cue: StateFlow<GenieCue?> = cueState.asStateFlow()
    val mood: StateFlow<GenieMood> = moodState.asStateFlow()
    private var fade: Job? = null

    val calm: Boolean get() = moodState.value == GenieMood.Idle

    fun load(store: PrefStore) {
        prefs = store
        hiddenState.value = store.getBool(HIDDEN_KEY) ?: false
        scaleState.value = (store.getDouble(SCALE_KEY) ?: 1.0).coerceIn(0.6, 2.2)
        parkedState.value = store.getBool(PARKED_KEY) ?: false
        wandersState.value = store.getBool(WANDERS_KEY) ?: true
    }

    fun spot(): GenieSpot? {
        val store = prefs ?: return null
        val x = store.getDouble(SPOT_X_KEY) ?: return null
        val y = store.getDouble(SPOT_Y_KEY) ?: return null
        val surface = GenieEdge.entries.firstOrNull { it.name.lowercase() == store.getString(SURFACE_KEY) } ?: GenieEdge.Bottom
        return GenieSpot(x, y, surface)
    }

    fun saveSpot(spot: GenieSpot) {
        prefs?.putDouble(SPOT_X_KEY, spot.x)
        prefs?.putDouble(SPOT_Y_KEY, spot.y)
        prefs?.putString(SURFACE_KEY, spot.surface.name.lowercase())
    }

    fun setHidden(value: Boolean) {
        hiddenState.value = value
        prefs?.putBool(HIDDEN_KEY, value)
        if (value) {
            lineState.value = null
            GenieOutreachCoach.dismiss()
            GenieTalkState.close()
        }
    }

    fun setScale(value: Double) {
        scaleState.value = value.coerceIn(0.6, 2.2)
        prefs?.putDouble(SCALE_KEY, scaleState.value)
    }

    fun setParked(value: Boolean) {
        parkedState.value = value
        prefs?.putBool(PARKED_KEY, value)
    }

    fun setWanders(value: Boolean) {
        wandersState.value = value
        prefs?.putBool(WANDERS_KEY, value)
    }

    fun setMood(value: GenieMood) {
        moodState.value = value
    }

    fun react(reaction: GenieReaction) {
        cueState.value = GenieCue(reaction, System.nanoTime() / 1e9)
    }

    fun say(text: String) {
        AppLifecycle.scope.launch {
            lineState.value = text
            fade?.cancel()
            fade = AppLifecycle.scope.launch {
                delay((LINE_SECONDS * 1000).toLong())
                if (lineState.value == text) lineState.value = null
            }
        }
    }

    fun reset() {
        setScale(1.0)
        setParked(false)
        prefs?.remove(SPOT_X_KEY)
        prefs?.remove(SPOT_Y_KEY)
        prefs?.remove(SURFACE_KEY)
    }
}
