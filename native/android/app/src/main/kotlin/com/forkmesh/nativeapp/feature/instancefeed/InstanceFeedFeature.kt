package com.forkmesh.nativeapp.feature.instancefeed

import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route

object InstanceFeedFeature {
    private const val OWNER = "WS20"

    fun install() {
        NavigationFeature.register(Route.Kind.InstanceFeed, OWNER) { route -> InstanceFeedScreen(post = (route as? Route.InstanceFeed)?.post) }
    }
}
