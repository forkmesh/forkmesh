package com.forkmesh.nativeapp.push

import kotlin.random.Random

class ReconnectBackoff(
    private val initialMs: Long = INITIAL_MS,
    private val maxMs: Long = MAX_MS,
    private val jitter: Double = JITTER,
    private val random: () -> Double = { Random.nextDouble() },
) {
    init {
        require(initialMs > 0 && maxMs >= initialMs)
        require(jitter in 0.0..1.0)
    }

    private var base = initialMs

    var failures: Int = 0
        private set

    @Synchronized
    fun nextDelayMs(): Long {
        val spread = base * jitter
        val delay = (base - spread + 2 * spread * random().coerceIn(0.0, 1.0)).toLong()
        failures++
        base = minOf(base * 2, maxMs)
        return delay.coerceIn(1, maxMs)
    }

    @Synchronized
    fun reset() {
        base = initialMs
        failures = 0
    }

    companion object {
        const val INITIAL_MS = 2_000L
        const val MAX_MS = 5 * 60_000L
        const val JITTER = 0.3
    }
}
