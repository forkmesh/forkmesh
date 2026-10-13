package com.forkmesh.nativeapp.agentwatch

object AgentWatchDefaults {
    fun reducedMotion(animatorDurationScale: Float?, animatorsEnabled: Boolean = true): Boolean =
        !animatorsEnabled || animatorDurationScale == 0f

    fun feel(stored: Boolean?, reducedMotion: Boolean): Boolean = stored ?: !reducedMotion
}
