package com.forkmesh.nativeapp

import android.app.Application
import com.forkmesh.nativeapp.feature.Features
import com.forkmesh.nativeapp.ui.kit.FmSoundCues

class App : Application() {
    override fun onCreate() {
        super.onCreate()
        instance = this
        Features.install(this)
        FmSoundCues.warmUp()
    }

    companion object {
        lateinit var instance: App
            private set
    }
}
