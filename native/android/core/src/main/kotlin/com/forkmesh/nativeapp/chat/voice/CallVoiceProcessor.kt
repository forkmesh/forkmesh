package com.forkmesh.nativeapp.chat.voice

import java.util.concurrent.atomic.AtomicReference

class CallVoiceProcessor(val sampleRate: Double = 48000.0, userPitchHz: Double = 0.0) {
    private class VoiceRequest(val voice: CallVoice?)

    private val engine = VoiceEngine(sampleRate, userPitchHz)
    private val pendingVoice = AtomicReference<VoiceRequest?>(null)
    private val pendingPitch = AtomicReference<Double?>(null)
    private val pendingNoise = AtomicReference<Double?>(null)

    @Volatile
    var voice: String = ""
        private set

    @Volatile
    var userPitchHz: Double = engine.userPitchHz()
        private set

    @Volatile
    var pitchHz: Double = 0.0
        private set

    val latencySamples: Int = engine.latencySamples

    fun setVoice(id: String) {
        val preset = CallVoiceCatalog.byId(id)
        voice = preset?.id ?: ""
        pendingVoice.set(VoiceRequest(preset))
    }

    fun setUserPitch(hz: Double) {
        pendingPitch.set(hz)
    }

    fun setNoiseReduction(amount: Double) {
        pendingNoise.set(amount)
    }

    fun process(samples: FloatArray, offset: Int = 0, count: Int = samples.size - offset) {
        pendingVoice.getAndSet(null)?.let { engine.setVoice(it.voice) }
        pendingPitch.getAndSet(null)?.let { engine.setUserPitch(it) }
        pendingNoise.getAndSet(null)?.let { engine.setNoiseReduction(it) }
        engine.process(samples, offset, count)
        userPitchHz = engine.userPitchHz()
        pitchHz = engine.lastPitchHz
    }
}
