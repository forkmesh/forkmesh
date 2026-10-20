package com.forkmesh.nativeapp.shell

data class ShellBarProgress(val startedAtMs: Long, val expectedMs: Long) {
    val estimated: Boolean get() = startedAtMs > 0 && expectedMs > 0

    fun fraction(nowMs: Long): Double? {
        if (!estimated) return null
        return ((nowMs - startedAtMs).toDouble() / expectedMs).coerceIn(0.0, 0.99)
    }

    fun spoken(nowMs: Long): String = fraction(nowMs)?.let { "Running, ${(it * 100).toInt()} percent" } ?: "Running"

    companion object {
        fun latest(runs: List<ShellBarProgress>): ShellBarProgress? {
            if (runs.isEmpty()) return null
            runs.firstOrNull { !it.estimated }?.let { return it }
            return runs.maxByOrNull { it.startedAtMs + it.expectedMs }
        }
    }
}
