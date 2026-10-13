package com.forkmesh.nativeapp.feature.glasses

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import com.forkmesh.nativeapp.glasses.GlassesHoming
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

object GlassesCompass : SensorEventListener {
    private val mutableHeading = MutableStateFlow<Double?>(null)
    val heading: StateFlow<Double?> = mutableHeading

    private var manager: SensorManager? = null
    private val rotation = FloatArray(9)

    fun start(context: Context) {
        if (manager != null) return
        val sensors = context.applicationContext.getSystemService(SensorManager::class.java) ?: return
        val sensor = sensors.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
            ?: sensors.getDefaultSensor(Sensor.TYPE_GEOMAGNETIC_ROTATION_VECTOR)
            ?: return
        if (sensors.registerListener(this, sensor, SensorManager.SENSOR_DELAY_UI)) manager = sensors
    }

    fun stop() {
        manager?.unregisterListener(this)
        manager = null
        mutableHeading.value = null
    }

    override fun onSensorChanged(event: SensorEvent) {
        SensorManager.getRotationMatrixFromVector(rotation, event.values)
        val next = GlassesHoming.heading(rotation) ?: return
        mutableHeading.value = GlassesHoming.smoothHeading(mutableHeading.value, next)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
}
