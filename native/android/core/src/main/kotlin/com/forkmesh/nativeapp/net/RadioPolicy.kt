package com.forkmesh.nativeapp.net

enum class AudioActivity { IDLE, PLAYING, CALL }

enum class ScanDuty { LOW_POWER, BALANCED, LOW_LATENCY }

data class MeshSearchPass(
    val dialBonded: Boolean,
    val inquiry: Boolean,
    val bleScan: ScanDuty?,
    val nextPassMs: Long,
)

object RadioPolicy {
    const val SEARCH_INTERVAL_MS = 120_000L
    const val CALL_RECHECK_MS = 10_000L
    const val WINDOW_SLICE_MS = 1_000L

    private const val MODE_NORMAL = 0
    private const val MAJOR_COMPUTER = 0x0100
    private const val MAJOR_PHONE = 0x0200
    private const val MAJOR_UNCATEGORIZED = 0x1F00

    fun activity(audioMode: Int, musicActive: Boolean): AudioActivity = when {
        audioMode > MODE_NORMAL -> AudioActivity.CALL
        musicActive -> AudioActivity.PLAYING
        else -> AudioActivity.IDLE
    }

    fun meshPass(activity: AudioActivity): MeshSearchPass = when (activity) {
        AudioActivity.CALL -> MeshSearchPass(dialBonded = false, inquiry = false, bleScan = null, nextPassMs = CALL_RECHECK_MS)
        AudioActivity.PLAYING -> MeshSearchPass(dialBonded = true, inquiry = false, bleScan = ScanDuty.LOW_POWER, nextPassMs = SEARCH_INTERVAL_MS)
        AudioActivity.IDLE -> MeshSearchPass(dialBonded = true, inquiry = true, bleScan = ScanDuty.LOW_LATENCY, nextPassMs = SEARCH_INTERVAL_MS)
    }

    fun meshCandidate(majorDeviceClass: Int?): Boolean =
        majorDeviceClass == null ||
            majorDeviceClass == MAJOR_COMPUTER ||
            majorDeviceClass == MAJOR_PHONE ||
            majorDeviceClass == MAJOR_UNCATEGORIZED

    fun glassesDuty(activity: AudioActivity): ScanDuty =
        if (activity == AudioActivity.IDLE) ScanDuty.LOW_LATENCY else ScanDuty.LOW_POWER

    fun glassesCensus(activity: AudioActivity, foreground: Boolean): Boolean =
        foreground && activity != AudioActivity.CALL
}
