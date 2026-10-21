package com.forkmesh.nativeapp.feature.health

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.SystemClock
import androidx.core.content.ContextCompat
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.health.HealthDates
import com.forkmesh.nativeapp.health.HealthPrefs
import com.forkmesh.nativeapp.health.HealthStepDay
import com.forkmesh.nativeapp.health.MotionSample
import com.forkmesh.nativeapp.health.SleepBridge
import com.forkmesh.nativeapp.health.SleepRestLog
import com.forkmesh.nativeapp.health.SleepRestSample
import com.forkmesh.nativeapp.health.StepsBridge
import com.forkmesh.nativeapp.health.StepsDailyLog
import com.forkmesh.nativeapp.platform.AppLifecycle
import java.time.Instant
import java.time.ZoneId
import java.util.concurrent.CopyOnWriteArrayList

object StepsSensors {
    private const val ACCELEROMETER_PERIOD_US = 20_000

    private val dayListeners = CopyOnWriteArrayList<(HealthStepDay) -> Unit>()
    private val motionListeners = CopyOnWriteArrayList<(List<MotionSample>) -> Unit>()
    private var counterOn = false
    private var accelerometerOn = false

    private val context: Context get() = App.instance

    private val manager: SensorManager? get() = context.getSystemService(SensorManager::class.java)

    val prefs: PrefStore? get() = Services.get<PrefStore>()

    val stepSensorAvailable: Boolean get() = manager?.getDefaultSensor(Sensor.TYPE_STEP_COUNTER) != null

    val accelerometerAvailable: Boolean get() = manager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null

    fun permissionGranted(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.Q ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.ACTIVITY_RECOGNITION) == PackageManager.PERMISSION_GRANTED

    val permissionName: String get() = Manifest.permission.ACTIVITY_RECOGNITION

    val needsPermission: Boolean get() = !permissionGranted()

    private val zone: ZoneId get() = ZoneId.systemDefault()

    private val counterListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            val store = prefs ?: return
            val now = System.currentTimeMillis()
            val reading = StepsDailyLog.Reading(event.values[0].toLong(), now, now - SystemClock.elapsedRealtime())
            if (HealthPrefs.bool(store, HealthPrefs.SLEEP)) SleepRestLog.observe(store, reading)
            if (HealthPrefs.bool(store, HealthPrefs.STEPS)) {
                val total = StepsDailyLog.record(store, reading, zone)
                publish(total)
            }
        }

        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
    }

    private val accelerometerListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            val gravity = SensorManager.GRAVITY_EARTH
            val sample = MotionSample(
                (event.values[0] / gravity).toDouble(), (event.values[1] / gravity).toDouble(),
                (event.values[2] / gravity).toDouble(), event.timestamp / 1_000_000_000.0,
            )
            val list = listOf(sample)
            for (listener in motionListeners) listener(list)
        }

        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
    }

    private fun publish(total: Int) {
        val day = StepsDailyLog.dayOf(HealthDates.isoDay(Instant.now(), zone), total)
        for (listener in dayListeners) listener(day)
    }

    fun todayDay(): HealthStepDay {
        val store = prefs
        val today = HealthDates.isoDay(Instant.now(), zone)
        return StepsDailyLog.dayOf(today, if (store == null) 0 else StepsDailyLog.steps(store, today))
    }

    fun resetBaseline() {
        prefs?.remove(StepsDailyLog.LAST_KEY)
    }

    fun resetRestBaseline() {
        prefs?.remove(SleepRestLog.LAST_KEY)
    }

    @Synchronized
    fun refresh() {
        val sensors = manager ?: return
        val store = prefs
        val wantCounter = store != null &&
            (HealthPrefs.bool(store, HealthPrefs.STEPS) || HealthPrefs.bool(store, HealthPrefs.SLEEP)) &&
            permissionGranted()
        val counter = sensors.getDefaultSensor(Sensor.TYPE_STEP_COUNTER)
        if (wantCounter && counter != null && !counterOn) {
            counterOn = sensors.registerListener(counterListener, counter, SensorManager.SENSOR_DELAY_NORMAL)
        } else if ((!wantCounter || counter == null) && counterOn) {
            sensors.unregisterListener(counterListener)
            counterOn = false
        }
        val accelerometer = sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val wantAccelerometer = motionListeners.isNotEmpty() && AppLifecycle.foreground.value && accelerometer != null
        if (wantAccelerometer && !accelerometerOn) {
            accelerometerOn = sensors.registerListener(accelerometerListener, accelerometer, ACCELEROMETER_PERIOD_US)
        } else if (!wantAccelerometer && accelerometerOn) {
            sensors.unregisterListener(accelerometerListener)
            accelerometerOn = false
        }
    }

    internal fun addDayListener(listener: (HealthStepDay) -> Unit) {
        dayListeners.add(listener)
        refresh()
    }

    internal fun removeDayListener(listener: (HealthStepDay) -> Unit) {
        dayListeners.remove(listener)
        refresh()
    }

    internal fun addMotionListener(listener: (List<MotionSample>) -> Unit) {
        motionListeners.add(listener)
        refresh()
    }

    internal fun removeMotionListener(listener: (List<MotionSample>) -> Unit) {
        motionListeners.remove(listener)
        refresh()
    }
}

class StepsSensorBridge : StepsBridge {
    private var day: ((HealthStepDay) -> Unit)? = null
    private var motion: ((List<MotionSample>) -> Unit)? = null

    override val available: Boolean get() = StepsSensors.stepSensorAvailable

    override val motionAvailable: Boolean get() = StepsSensors.accelerometerAvailable

    override fun authorize(): Boolean {
        if (!StepsSensors.permissionGranted()) return false
        StepsSensors.resetBaseline()
        StepsSensors.refresh()
        return true
    }

    override fun stepDays(count: Int, now: Instant): List<HealthStepDay> {
        val store = StepsSensors.prefs ?: return emptyList()
        return StepsDailyLog.stepDays(store, count, now.toEpochMilli(), ZoneId.systemDefault())
    }

    override fun startLiveSteps(update: (HealthStepDay) -> Unit): Boolean {
        stopLiveSteps()
        day = update
        StepsSensors.addDayListener(update)
        update(StepsSensors.todayDay())
        return true
    }

    override fun stopLiveSteps() {
        day?.let { StepsSensors.removeDayListener(it) }
        day = null
    }

    override fun startMotionSamples(update: (List<MotionSample>) -> Unit): Boolean {
        if (!StepsSensors.accelerometerAvailable) return false
        stopMotionSamples()
        motion = update
        StepsSensors.addMotionListener(update)
        return true
    }

    override fun stopMotionSamples() {
        motion?.let { StepsSensors.removeMotionListener(it) }
        motion = null
    }
}

class SleepSensorBridge : SleepBridge {
    override val motionAvailable: Boolean get() = StepsSensors.stepSensorAvailable

    override fun authorizeSleep(): Boolean {
        if (!StepsSensors.permissionGranted()) return false
        StepsSensors.resetRestBaseline()
        StepsSensors.refresh()
        return true
    }

    override fun restHistory(from: Instant, to: Instant): List<SleepRestSample>? {
        val store = StepsSensors.prefs ?: return null
        if (!StepsSensors.permissionGranted()) return null
        return SleepRestLog.samples(store, from.toEpochMilli(), to.toEpochMilli())
    }
}
