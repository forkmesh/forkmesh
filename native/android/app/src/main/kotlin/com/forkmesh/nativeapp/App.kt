package com.forkmesh.nativeapp

import android.app.Application
import com.forkmesh.nativeapp.feature.Features
import com.forkmesh.nativeapp.feature.chat.DirectCallFeature
import com.forkmesh.nativeapp.feature.chat.GroupCallFeature
import com.forkmesh.nativeapp.feature.glasses.GlassesAlarm
import com.forkmesh.nativeapp.platform.OtherAppCall
import com.forkmesh.nativeapp.ui.kit.FmSoundCues

class App : Application() {
    override fun onCreate() {
        super.onCreate()
        instance = this
        Features.install(this)
        stepBackDuringOtherCalls()
        FmSoundCues.warmUp()
    }

    private fun stepBackDuringOtherCalls() {
        OtherAppCall.ownCall = { DirectCallFeature.engine?.phase != null || GroupCallFeature.active }
        OtherAppCall.install(this)
        var quieted = false
        OtherAppCall.onChange { inCall ->
            if (inCall != quieted) {
                quieted = inCall
                FmSoundCues.quieten(inCall)
            }
            if (inCall) GlassesAlarm.hushIfRinging()
        }
    }

    companion object {
        lateinit var instance: App
            private set
    }
}
