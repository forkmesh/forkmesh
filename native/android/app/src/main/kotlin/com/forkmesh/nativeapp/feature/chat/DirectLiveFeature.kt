package com.forkmesh.nativeapp.feature.chat

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import com.forkmesh.nativeapp.chat.ChatHub
import com.forkmesh.nativeapp.chat.DirectLiveEngine
import com.forkmesh.nativeapp.chat.DirectLiveMotionSample
import com.forkmesh.nativeapp.chat.DirectLivePoint
import com.forkmesh.nativeapp.chat.DirectMessageService
import com.forkmesh.nativeapp.platform.AppearanceStore
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlin.math.sqrt

object DirectLiveFeature {
    private val revisionState = MutableStateFlow(0)
    val revision: StateFlow<Int> = revisionState.asStateFlow()

    @Volatile
    var engine: DirectLiveEngine? = null
        private set

    @Volatile
    private var feed: DirectLiveMotionFeed? = null

    private val bump: () -> Unit = { revisionState.value = revisionState.value + 1 }

    fun install(context: Context, hub: ChatHub, direct: DirectMessageService): DirectLiveEngine {
        val app = context.applicationContext
        val live = direct.makeLiveEngine()
        live.onTap = { _, _ -> DirectLiveHaptics.tapped(app) }
        live.onStroke = { _, _ -> DirectLiveHaptics.drawn(app) }
        live.addListener(bump)
        hub.live = live
        engine = live
        feed = DirectLiveMotionFeed(app)
        return live
    }

    fun uninstall(hub: ChatHub?) {
        val live = engine ?: return
        feed?.stop()
        live.removeListener(bump)
        live.clear()
        if (hub?.live === live) hub.live = null
        engine = null
        feed = null
        bump()
    }

    val motionAvailable: Boolean get() = feed?.available == true

    fun startSharing(conversation: String) {
        val live = engine ?: return
        val motion = feed ?: return
        if (!motion.available || conversation.isEmpty()) return
        live.setSharingMotion(conversation, true)
        motion.start(live)
    }

    fun stopSharing(conversation: String) {
        val live = engine ?: return
        live.setSharingMotion(conversation, false)
        if (live.sharingMotion.isEmpty()) feed?.stop()
    }

    fun stopAll() {
        engine?.stopSharingMotion()
        feed?.stop()
    }
}

object DirectLiveHaptics {
    private val main = Handler(Looper.getMainLooper())

    private fun vibrator(context: Context): Vibrator? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }

    private fun pulse(context: Context, millis: Long, strength: Float, primitive: Int) {
        val device = vibrator(context) ?: return
        if (!device.hasVibrator()) return
        runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && device.areAllPrimitivesSupported(primitive)) {
                device.vibrate(VibrationEffect.startComposition().addPrimitive(primitive, strength).compose())
            } else {
                val amplitude = if (device.hasAmplitudeControl()) (strength * 255).toInt().coerceIn(1, 255) else VibrationEffect.DEFAULT_AMPLITUDE
                device.vibrate(VibrationEffect.createOneShot(millis, amplitude))
            }
        }
    }

    private val thud: Int get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) VibrationEffect.Composition.PRIMITIVE_THUD else 0
    private val click: Int get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) VibrationEffect.Composition.PRIMITIVE_CLICK else 0
    private val tick: Int get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) VibrationEffect.Composition.PRIMITIVE_TICK else 0

    fun tapped(context: Context) {
        main.post {
            pulse(context, 40, 1f, if (supports(context, thud)) thud else click)
            main.postDelayed({ pulse(context, 20, 0.55f, click) }, 90)
        }
    }

    fun drawn(context: Context) {
        if (!AppearanceStore.hapticsEnabled) return
        main.post { pulse(context, 12, 0.6f, tick) }
    }

    fun sent(context: Context) {
        main.post { pulse(context, 14, 0.45f, tick) }
    }

    private fun supports(context: Context, primitive: Int): Boolean =
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && vibrator(context)?.areAllPrimitivesSupported(primitive) == true
}

class DirectLiveMotionFeed(private val context: Context) : SensorEventListener {
    private val manager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
    private val gravitySensor = manager?.getDefaultSensor(Sensor.TYPE_GRAVITY)
    private val linearSensor = manager?.getDefaultSensor(Sensor.TYPE_LINEAR_ACCELERATION)
    private val rawSensor = manager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)

    @Volatile
    private var engine: DirectLiveEngine? = null
    private var running = false
    private var fused = false
    private var gravity: FloatArray? = null
    private var push = FloatArray(3)
    private var resting: DoubleArray? = null
    private var lastTap = 0L

    val available: Boolean get() = (gravitySensor != null && linearSensor != null) || rawSensor != null

    @Synchronized
    fun start(engine: DirectLiveEngine) {
        this.engine = engine
        if (running) return
        val sensors = manager ?: return
        fused = gravitySensor != null && linearSensor != null
        running = if (fused) {
            sensors.registerListener(this, gravitySensor, PERIOD_US) and sensors.registerListener(this, linearSensor, PERIOD_US)
        } else if (rawSensor != null) {
            resting = null
            sensors.registerListener(this, rawSensor, PERIOD_US)
        } else {
            false
        }
    }

    @Synchronized
    fun stop() {
        manager?.unregisterListener(this)
        running = false
        gravity = null
        push = FloatArray(3)
        resting = null
        lastTap = 0
        engine = null
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    override fun onSensorChanged(event: SensorEvent) {
        val live = engine ?: return
        when (event.sensor.type) {
            Sensor.TYPE_LINEAR_ACCELERATION -> push = event.values.copyOf(3)
            Sensor.TYPE_GRAVITY -> {
                gravity = event.values.copyOf(3)
                feed(live, toG(gravity!!), toG(push))
            }
            Sensor.TYPE_ACCELEROMETER -> feelRaw(live, toG(event.values))
        }
    }

    private fun toG(values: FloatArray): DoubleArray =
        doubleArrayOf(-values[0] / SensorManager.GRAVITY_EARTH.toDouble(), -values[1] / SensorManager.GRAVITY_EARTH.toDouble(), -values[2] / SensorManager.GRAVITY_EARTH.toDouble())

    private fun feelRaw(live: DirectLiveEngine, reading: DoubleArray) {
        val rest = resting ?: reading.copyOf()
        for (axis in 0 until 3) rest[axis] += (reading[axis] - rest[axis]) * SMOOTHING
        resting = rest
        feed(live, rest, DoubleArray(3) { reading[it] - rest[it] })
    }

    private fun feed(live: DirectLiveEngine, g: DoubleArray, a: DoubleArray) {
        if (live.sharingMotion.isEmpty()) {
            stop()
            return
        }
        val impact = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
        val now = live.now
        if (impact >= TAP_IMPACT && now - lastTap >= TAP_GAP_MS) {
            lastTap = now
            for (conversation in live.sharingMotion) live.tap(conversation, DirectLivePoint(0.5, 0.0))
            DirectLiveHaptics.sent(context)
        }
        live.addMotion(DirectLiveMotionSample(now, g[0], g[1], g[2], a[0], a[1], a[2]))
    }

    companion object {
        const val RATE_HZ = 100
        const val PERIOD_US = 1_000_000 / RATE_HZ
        const val SMOOTHING = 0.1
        const val TAP_IMPACT = 1.4
        const val TAP_GAP_MS = 500L
    }
}
