package com.forkmesh.nativeapp.feature.securedrive

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route

object SecureDriveFeature {
    private const val OWNER = "WS23"

    @Volatile
    private var instance: SecureDriveService? = null

    internal val service: SecureDriveService get() = instance ?: error("Secure Drive is not installed")

    fun install(app: App) {
        if (instance == null) instance = SecureDriveService(app)
        NavigationFeature.register(Route.Kind.SecureDrive, OWNER) { SecureDriveHomeScreen() }
    }
}
