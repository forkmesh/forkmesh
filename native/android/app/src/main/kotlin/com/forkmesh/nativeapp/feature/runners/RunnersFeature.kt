package com.forkmesh.nativeapp.feature.runners

import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route

object RunnersFeature {
    private const val OWNER = "WS7"

    fun install() {
        NavigationFeature.register(Route.Kind.Runners, OWNER) { RunnersScreen() }
    }
}
