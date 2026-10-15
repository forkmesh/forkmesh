package com.forkmesh.nativeapp.feature.rssreader

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route

object RssReaderFeature {
    private const val OWNER = "WS20"

    @Volatile
    private var instance: RssReaderService? = null

    val service: RssReaderService get() = instance ?: error("RSS Reader is not installed")

    fun install(app: App) {
        if (instance == null) instance = RssReaderService(app)
        NavigationFeature.register(Route.Kind.RssReader, OWNER) { RssFeedsScreen() }
    }
}
