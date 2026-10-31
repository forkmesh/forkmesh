package com.forkmesh.nativeapp.chat.voice

import java.util.Locale

data class CallVoice(
    val id: String,
    val label: String,
    val pitchHz: Double,
    val tract: Double,
    val brightness: Double,
    val breath: Double,
    val range: Double = 1.0,
    val jitter: Double = 0.0,
    val tremor: Double = 0.0,
)

object CallVoiceCatalog {
    const val MATCH = "face"
    const val OFF = "off"
    const val OWN_LABEL = "Your own"

    val ratioLimits = 0.45 to 2.6
    val formantLimits = 0.72 to 1.42
    val callerHzLimits = 70.0 to 330.0
    val tractLimits = 0.94 to 1.26
    val rangeLimits = 0.0 to 1.5
    val jitterLimits = 0.0 to 0.04
    val tremorLimits = 0.0 to 0.05

    val voices: List<CallVoice> = listOf(
        CallVoice("maya", "Maya", 196.0, 1.14, 0.0, 0.04),
        CallVoice("theo", "Theo", 124.0, 1.03, 0.1, 0.0),
        CallVoice("kofi", "Kofi", 96.0, 0.93, -0.15, 0.0),
        CallVoice("ingrid", "Ingrid", 228.0, 1.21, 0.12, 0.08),
        CallVoice("ren", "Ren", 210.0, 1.17, 0.05, 0.02),
        CallVoice("deep", "Deep man", 84.0, 0.9, -0.25, 0.0, range = 0.85),
        CallVoice("older-man", "Older man", 128.0, 0.97, -0.1, 0.1, range = 0.8, jitter = 0.012, tremor = 0.012),
        CallVoice("young-man", "Young man", 138.0, 1.06, 0.12, 0.02, range = 1.15),
        CallVoice("warm", "Warm woman", 188.0, 1.12, -0.1, 0.06),
        CallVoice("low", "Low woman", 165.0, 1.1, -0.05, 0.03, range = 0.9),
        CallVoice("older-woman", "Older woman", 176.0, 1.12, -0.08, 0.1, range = 0.85, jitter = 0.012, tremor = 0.015),
        CallVoice("teen", "Teen", 252.0, 1.25, 0.15, 0.04, range = 1.2),
        CallVoice("neutral", "Neutral", 158.0, 1.08, 0.0, 0.03, range = 0.9),
        CallVoice("robot", "Robot", 118.0, 1.0, 0.1, 0.0, range = 0.0),
    )

    fun byId(id: String?): CallVoice? = voices.firstOrNull { it.id == id }

    fun normalizeChoice(choice: String?): String {
        val value = (choice ?: "").trim().lowercase(Locale.ROOT)
        if (value == OFF) return OFF
        if (byId(value) != null) return value
        return MATCH
    }

    fun resolve(choice: String?, faceId: String?): String = when (val value = normalizeChoice(choice)) {
        OFF -> ""
        MATCH -> byId(faceId ?: "")?.id ?: ""
        else -> value
    }

    fun label(id: String?): String = byId(id)?.label ?: OWN_LABEL

    fun choices(): List<Pair<String, String>> =
        listOf(MATCH to "Match face", OFF to "Your own voice") + voices.map { it.id to it.label }
}
