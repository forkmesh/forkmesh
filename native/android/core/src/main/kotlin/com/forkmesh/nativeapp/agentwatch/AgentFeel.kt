package com.forkmesh.nativeapp.agentwatch

import kotlin.math.max
import kotlin.math.min

data class Pulse(val atMs: Int, val ms: Int, val intensity: Double, val sharpness: Double = 0.5) {
    val clamped: Pulse get() = Pulse(atMs, ms, min(max(intensity, 0.0), 1.0), min(max(sharpness, 0.0), 1.0))
}

fun feelFor(motion: AgentMotion, voice: Voice): List<Pulse> {
    val s = voice.sharpness
    val weight = 1.0 - voice.octave * 0.12
    fun p(at: Int, ms: Int, intensity: Double, sharpness: Double? = null) = Pulse(at, ms, intensity * weight, sharpness ?: s)
    return when (motion) {
        AgentMotion.speak -> listOf(p(0, 10, 0.3))
        AgentMotion.think -> listOf(p(0, 160, 0.14, 0.1))
        AgentMotion.read, AgentMotion.scan -> listOf(p(0, 8, 0.35, 0.9))
        AgentMotion.search -> listOf(p(0, 8, 0.3, 0.9), p(55, 8, 0.35, 0.9), p(110, 8, 0.4, 0.9))
        AgentMotion.web, AgentMotion.mcp -> listOf(p(0, 8, 0.4, 0.9), p(40, 60, 0.15, 0.3))
        AgentMotion.write -> listOf(p(0, 18, 0.7), p(120, 12, 0.3))
        AgentMotion.create -> listOf(p(0, 12, 0.35), p(60, 20, 0.75))
        AgentMotion.run -> listOf(p(0, 14, 0.65, 0.95))
        AgentMotion.delegate, AgentMotion.spawn -> listOf(p(0, 10, 0.3), p(70, 10, 0.5), p(140, 14, 0.75))
        AgentMotion.plan, AgentMotion.todo -> listOf(p(0, 8, 0.35), p(90, 8, 0.35))
        AgentMotion.arrive -> listOf(p(0, 8, 0.22, 0.4))
        AgentMotion.prompt -> listOf(p(0, 24, 0.9))
        AgentMotion.notice -> listOf(p(0, 40, 0.55, 0.7), p(90, 40, 0.55, 0.7))
        AgentMotion.finish -> listOf(p(0, 12, 0.4), p(160, 12, 0.6), p(320, 40, 1.0))
        AgentMotion.test -> listOf(p(0, 8, 0.4, 0.9), p(55, 8, 0.4, 0.9), p(110, 8, 0.4, 0.9), p(165, 8, 0.4, 0.9))
        AgentMotion.build, AgentMotion.install -> listOf(p(0, 140, 0.6, 0.05))
        AgentMotion.git -> listOf(p(0, 14, 0.6, 0.6), p(90, 14, 0.4, 0.3))
        AgentMotion.wait -> listOf(p(0, 220, 0.12, 0.05))
        AgentMotion.ask -> listOf(p(0, 14, 0.4), p(140, 20, 0.85))
        AgentMotion.skill -> listOf(p(0, 10, 0.55, 0.8), p(50, 30, 0.2, 0.2))
        AgentMotion.fumble -> listOf(p(0, 180, 0.8, 0.3))
        AgentMotion.fail -> listOf(p(0, 120, 0.9, 0.3), p(200, 260, 1.0, 0.1))
        AgentMotion.start -> listOf(p(0, 20, 0.7), p(120, 12, 0.35))
        AgentMotion.denied -> listOf(p(0, 60, 0.9, 0.4), p(110, 120, 0.8, 0.2))
        AgentMotion.image -> listOf(p(0, 10, 0.35, 0.8), p(60, 10, 0.25, 0.8))
        AgentMotion.compact -> listOf(p(0, 260, 0.2, 0.05))
        AgentMotion.retry -> listOf(p(0, 8, 0.35, 0.9), p(60, 8, 0.35, 0.9))
        AgentMotion.logError -> listOf(p(0, 40, 0.5, 0.3))
        else -> listOf(p(0, 6, 0.15))
    }
}
