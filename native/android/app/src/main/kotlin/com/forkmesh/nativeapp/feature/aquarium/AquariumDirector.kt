package com.forkmesh.nativeapp.feature.aquarium

import com.forkmesh.nativeapp.aquarium.AquariumRect
import com.forkmesh.nativeapp.aquarium.AquariumSwimmer
import com.forkmesh.nativeapp.aquarium.AquariumTank
import com.forkmesh.nativeapp.aquarium.GenieLandmark
import kotlin.random.Random

internal object AquariumDirector {
    const val FPS = 30.0

    var tank: AquariumTank? = null
        private set
    private var lastNanos: Long? = null
    private var roster: List<AquariumSwimmer>? = null
    private var shape: Triple<AquariumRect, Double, Double>? = null
    private var marks: List<GenieLandmark> = emptyList()

    val current: AquariumTank? get() = tank

    val spiderPlaying: Boolean get() = tank?.spiderPlaying ?: false

    fun see(next: List<GenieLandmark>) {
        marks = next
        tank?.see(next)
    }

    fun pause() {
        lastNanos = null
    }

    fun reset() {
        tank = null
        lastNanos = null
        roster = null
        shape = null
    }

    fun frame(nowNanos: Long, screen: AquariumRect, top: Double, floor: Double, swimmers: List<AquariumSwimmer>, calm: Boolean): AquariumTank {
        val random: () -> Double = { Random.nextDouble() }
        val next = tank ?: AquariumTank(screen, top, floor).also { it.see(marks) }
        val known = shape
        if (known != null && (known.first != screen || known.second != top || known.third != floor)) {
            next.resize(screen, top, floor)
        }
        shape = Triple(screen, top, floor)
        if (roster != swimmers) {
            next.roster(swimmers, random)
            roster = swimmers
        }
        next.spider = null
        next.calm = calm
        val seconds = lastNanos?.let { (nowNanos - it) / 1_000_000_000.0 } ?: 0.0
        lastNanos = nowNanos
        next.step(seconds, random)
        tank = next
        return next
    }
}
