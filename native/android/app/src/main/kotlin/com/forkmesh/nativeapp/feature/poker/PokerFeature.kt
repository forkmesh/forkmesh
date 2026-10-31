package com.forkmesh.nativeapp.feature.poker

import android.content.Context
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountEventChannel
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.poker.PokerApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.launch

object PokerFeature {
    private const val OWNER = "S046"
    private var current: PokerModel? = null

    fun install() {
        NavigationFeature.register(Route.Kind.Poker, OWNER) { route -> PokerScreen((route as? Route.Poker)?.table) }
        val channel = Services.get<AccountEventChannel>()
        if (channel != null) {
            val previous = channel.onTopic
            channel.onTopic = { topic ->
                previous?.invoke(topic)
                if (topic == PokerApi.TOPIC) ring()
            }
            val previousConnected = channel.onConnected
            channel.onConnected = {
                previousConnected?.invoke()
                ring()
            }
        }
        val scope = ProjectLifecycle.scope
        scope.launch { AppLifecycle.foreground.drop(1).collect { foreground -> if (foreground) current?.ring() } }
        ProjectLifecycle.onClose {
            current?.dispose()
            current = null
        }
    }

    private fun ring() {
        ProjectLifecycle.scope.launch(Dispatchers.Main) { current?.ring() }
    }

    internal fun model(context: Context): PokerModel = current ?: PokerModel.create(context).also { current = it }
}
