package com.forkmesh.nativeapp.feature.facechanger

import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route

object FaceChangerFeature {
    private const val OWNER = "existing"

    fun install() {
        NavigationFeature.register(Route.Kind.FaceChanger, OWNER) { FaceChangerScreen() }
    }
}
