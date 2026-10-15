package com.forkmesh.nativeapp.feature.appstorestatus

import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route

object AppStoreStatusFeature {
    private const val OWNER = "WS20"

    fun install() {
        NavigationFeature.register(Route.Kind.AppStoreStatus, OWNER) { AppStoreStatusScreen() }
    }
}
