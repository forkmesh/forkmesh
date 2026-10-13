package com.forkmesh.nativeapp.feature.account

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PushBinder
import com.forkmesh.nativeapp.api.PushPlatform
import com.forkmesh.nativeapp.api.PushTestResult
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext

class PushNotificationService(api: ApiService, platform: PushPlatform? = null) {
    val binder = PushBinder(api, platform)

    private val permissionState = MutableStateFlow(binder.permission)
    private val environmentState = MutableStateFlow(binder.environment)
    private val registeredState = MutableStateFlow(binder.registered)
    private val relayConfiguredState = MutableStateFlow(binder.relayConfigured)
    private val lastErrorState = MutableStateFlow(binder.lastError)

    val supported: Boolean get() = binder.supported

    val permission: StateFlow<String> = permissionState.asStateFlow()

    val environment: StateFlow<String> = environmentState.asStateFlow()

    val registered: StateFlow<Boolean> = registeredState.asStateFlow()

    val relayConfigured: StateFlow<Boolean?> = relayConfiguredState.asStateFlow()

    val lastError: StateFlow<String> = lastErrorState.asStateFlow()

    var onOpened: (() -> Unit)?
        get() = binder.onOpened
        set(value) {
            binder.onOpened = value
        }

    var accountKeyProvider: (() -> String)?
        get() = binder.accountKeyProvider
        set(value) {
            binder.accountKeyProvider = value
        }

    private val listener: () -> Unit = { publish() }

    init {
        binder.addListener(listener)
    }

    private fun publish() {
        permissionState.value = binder.permission
        environmentState.value = binder.environment
        registeredState.value = binder.registered
        relayConfiguredState.value = binder.relayConfigured
        lastErrorState.value = binder.lastError
    }

    suspend fun start(accountKey: String) = withContext(Dispatchers.IO) { binder.start(accountKey) }

    suspend fun stop() = withContext(Dispatchers.IO) { binder.stop() }

    suspend fun rebind(accountKey: String) = withContext(Dispatchers.IO) { binder.rebind(accountKey) }

    suspend fun enable() = withContext(Dispatchers.IO) { binder.enable() }

    suspend fun sendTest(): PushTestResult = withContext(Dispatchers.IO) { binder.sendTest() }
}
