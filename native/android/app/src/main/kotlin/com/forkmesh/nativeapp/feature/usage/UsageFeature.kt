package com.forkmesh.nativeapp.feature.usage

import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route

object UsageFeature {
    private const val OWNER = "WS10"

    fun install() {
        NavigationFeature.register(Route.Kind.Usage, OWNER) { UsageScreen() }
    }
}
