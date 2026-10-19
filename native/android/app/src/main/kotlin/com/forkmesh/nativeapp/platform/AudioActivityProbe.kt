package com.forkmesh.nativeapp.platform

import android.bluetooth.le.ScanSettings
import android.content.Context
import android.media.AudioManager
import com.forkmesh.nativeapp.net.AudioActivity
import com.forkmesh.nativeapp.net.RadioPolicy
import com.forkmesh.nativeapp.net.ScanDuty

object AudioActivityProbe {
    fun current(context: Context): AudioActivity {
        val audio = context.getSystemService(AudioManager::class.java) ?: return AudioActivity.IDLE
        return runCatching { RadioPolicy.activity(audio.mode, audio.isMusicActive) }.getOrDefault(AudioActivity.IDLE)
    }

    fun inCall(context: Context): Boolean = current(context) == AudioActivity.CALL
}

val ScanDuty.scanMode: Int
    get() = when (this) {
        ScanDuty.LOW_POWER -> ScanSettings.SCAN_MODE_LOW_POWER
        ScanDuty.BALANCED -> ScanSettings.SCAN_MODE_BALANCED
        ScanDuty.LOW_LATENCY -> ScanSettings.SCAN_MODE_LOW_LATENCY
    }
