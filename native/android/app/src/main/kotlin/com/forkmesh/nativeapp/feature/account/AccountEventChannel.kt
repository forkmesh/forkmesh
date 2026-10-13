package com.forkmesh.nativeapp.feature.account

import com.forkmesh.nativeapp.api.AccountEventLoop
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

class AccountEventChannel(
    api: ApiService,
    sessionToken: () -> String,
    scope: CoroutineScope = AppLifecycle.scope,
) {
    val loop = AccountEventLoop(api, sessionToken)

    private val connectedState = MutableStateFlow(false)
    private val startedState = MutableStateFlow(false)

    val connected: StateFlow<Boolean> = connectedState.asStateFlow()

    val started: StateFlow<Boolean> = startedState.asStateFlow()

    var onTopic: ((String) -> Unit)?
        get() = loop.onTopic
        set(value) {
            loop.onTopic = value
        }

    var onConnected: (() -> Unit)?
        get() = loop.onConnected
        set(value) {
            loop.onConnected = value
        }

    var onSignedOut: (() -> Unit)?
        get() = loop.onSignedOut
        set(value) {
            loop.onSignedOut = value
        }

    private val lifecycleJob: Job

    init {
        loop.onStateChanged = {
            connectedState.value = loop.connected
            startedState.value = loop.started
        }
        lifecycleJob = scope.launch {
            AppLifecycle.foreground.collect { foreground -> loop.setForeground(foreground) }
        }
    }

    fun start() = loop.start()

    fun stop() = loop.stop()

    fun restart() = loop.restart()

    fun dispose() {
        lifecycleJob.cancel()
        loop.dispose()
    }
}
