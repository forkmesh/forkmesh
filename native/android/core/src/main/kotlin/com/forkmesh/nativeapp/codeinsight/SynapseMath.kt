package com.forkmesh.nativeapp.codeinsight

import kotlin.math.abs

internal object SynapseMath {
    fun length(x: Float, y: Float, z: Float): Float {
        val ax = abs(x)
        val ay = abs(y)
        val az = abs(z)
        val a = if (ax < ay) (if (ay < az) az else ay) else (if (ax < az) az else ax)
        if (a == 0f) return 0f
        val qx = ax / a
        val qy = ay / a
        val qz = az / a
        return a * kotlin.math.sqrt(qx * qx + qy * qy + qz * qz)
    }

    fun fuzzyIsNull(value: Float): Boolean = abs(value) <= 0.00001f

    fun sin(x: Float): Float = Math.sin(x.toDouble()).toFloat()

    fun cos(x: Float): Float = Math.cos(x.toDouble()).toFloat()

    fun atan2(y: Float, x: Float): Float = Math.atan2(y.toDouble(), x.toDouble()).toFloat()

    fun asin(x: Float): Float = Math.asin(x.toDouble()).toFloat()

    fun log(x: Float): Float = Math.log(x.toDouble()).toFloat()

    fun log1p(x: Float): Float = Math.log1p(x.toDouble()).toFloat()

    fun cbrt(x: Float): Float = Math.cbrt(x.toDouble()).toFloat()

    fun pow(x: Float, y: Float): Float = Math.pow(x.toDouble(), y.toDouble()).toFloat()

    fun mixHash(input: Int): Int {
        var value = input
        value = value xor (value ushr 16)
        value *= 0x7feb352d
        value = value xor (value ushr 15)
        value *= 0x846ca68b.toInt()
        value = value xor (value ushr 16)
        return value
    }

    fun unitFromHash(value: Int): Float = (value and 0xffffff).toFloat() / 16777215.0f

    fun jitter(seed: Int, out: FloatArray) {
        val u = unitFromHash(mixHash(seed xor 0x9e3779b9.toInt()))
        val v = unitFromHash(mixHash(seed xor 0x85ebca6b.toInt()))
        val w = unitFromHash(mixHash(seed xor 0xc2b2ae35.toInt()))
        val theta = u * 6.2831853f
        val z = v * 2.0f - 1.0f
        val r = kotlin.math.sqrt(maxOf(0.0f, 1.0f - z * z))
        val radius = cbrt(maxOf(0.02f, w))
        out[0] = r * cos(theta) * radius
        out[1] = z * radius
        out[2] = r * sin(theta) * radius
    }
}
