package com.forkmesh.nativeapp.feature.djstudio

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.djstudio.DjMotionSample
import com.forkmesh.nativeapp.djstudio.DjReaction
import com.forkmesh.nativeapp.djstudio.DjSaved
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import kotlin.math.sqrt

object DjStudioHub {
    private const val OWNER = "S045"
    private var model: DjStudioModel? = null
    private var relaunched = false
    var shown by mutableStateOf<DjStudioModel?>(null)
        private set

    fun install() {
        NavigationFeature.register(Route.Kind.VideoMixDjStudio, OWNER) { DjStudioScreen() }
    }

    fun current(): DjStudioModel? = model

    fun studio(context: Context = App.instance): DjStudioModel {
        model?.let { return it }
        val app = context.applicationContext
        val engine = DjEngine()
        val made = DjStudioModel(app, engine, DjResolver(app))
        model = made
        shown = made
        return made
    }

    fun relaunch() {
        if (relaunched) return
        relaunched = true
        val prefs = Services.get<PrefStore>() ?: return
        if (!PackageGateFeature.isInstalled(DjStudioModel.PACKAGE)) return
        if (!DjSaved.decode(prefs.getString(DjStudio.STORAGE_KEY)).playing) return
        studio().resumePlayback()
    }
}

class DjEffect(val reaction: DjReaction, val sender: String) {
    val id: Long = SystemClock.elapsedRealtime() * 1000 + (counter++ % 1000)
    val born: Long = SystemClock.uptimeMillis()

    private companion object {
        var counter = 0L
    }
}

internal object DjHaptics {
    fun tick(context: Context) {
        val vibrator = if (Build.VERSION.SDK_INT >= 31) {
            context.getSystemService(VibratorManager::class.java)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Vibrator::class.java)
        } ?: return
        runCatching { vibrator.vibrate(VibrationEffect.createPredefined(VibrationEffect.EFFECT_TICK)) }
    }
}

internal class DjMotionFeed(context: Context) : SensorEventListener {
    private val sensors = context.getSystemService(SensorManager::class.java)
    private val linear = sensors?.getDefaultSensor(Sensor.TYPE_LINEAR_ACCELERATION) ?: sensors?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
    private val samples = ArrayList<DjMotionSample>()
    private var sharing = false
    private var shaking = false
    private var registered = false
    private val peaks = ArrayDeque<Long>()
    private var lastShake = 0L
    var onShake: (() -> Unit)? = null

    fun start() {
        sharing = true
        sync()
    }

    fun stop() {
        sharing = false
        synchronized(samples) { samples.clear() }
        sync()
    }

    fun listenForShakes(on: Boolean) {
        shaking = on
        sync()
    }

    fun drain(): List<DjMotionSample> = synchronized(samples) {
        val copy = samples.toList()
        samples.clear()
        copy
    }

    private fun sync() {
        val want = sharing || shaking
        if (want == registered || linear == null || sensors == null) return
        registered = want
        if (want) sensors.registerListener(this, linear, 33_000) else sensors.unregisterListener(this)
    }

    override fun onSensorChanged(event: SensorEvent) {
        val gravity = if (event.sensor.type == Sensor.TYPE_ACCELEROMETER) SensorManager.GRAVITY_EARTH else 0f
        val x = event.values[0] / SensorManager.GRAVITY_EARTH
        val y = event.values[1] / SensorManager.GRAVITY_EARTH
        if (sharing) synchronized(samples) {
            samples.add(DjMotionSample(x.toDouble(), y.toDouble()))
            if (samples.size > 240) samples.removeAt(0)
        }
        if (!shaking) return
        val strength = sqrt(event.values[0] * event.values[0] + event.values[1] * event.values[1] + event.values[2] * event.values[2]) - gravity
        val now = SystemClock.elapsedRealtime()
        if (strength > 14f) {
            peaks.addLast(now)
            while (peaks.isNotEmpty() && now - peaks.first() > 900) peaks.removeFirst()
            if (peaks.size >= 3 && now - lastShake > 2000) {
                lastShake = now
                peaks.clear()
                onShake?.invoke()
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
}
