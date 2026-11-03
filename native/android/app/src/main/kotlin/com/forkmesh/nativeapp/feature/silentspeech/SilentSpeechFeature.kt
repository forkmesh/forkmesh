package com.forkmesh.nativeapp.feature.silentspeech

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route

object SilentSpeechFeature {
    private const val OWNER = "existing"

    fun install(app: App) {
        SilentSpeechListener.install(app)
        NavigationFeature.register(Route.Kind.SilentSpeech, OWNER) { SilentSpeechScreen() }
    }
}
