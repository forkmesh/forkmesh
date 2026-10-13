package com.forkmesh.nativeapp.feature.agents

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.runners.RunnerAccount
import com.forkmesh.nativeapp.runners.RunnerAccountsState
import com.forkmesh.nativeapp.runners.RunnerAccountsStore
import com.forkmesh.nativeapp.runners.RunnerAttentionApi
import com.forkmesh.nativeapp.ui.navigation.Navigator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

object RunnerAccountsFeature {
    private val stateFlow = MutableStateFlow(RunnerAccountsState())

    val state: StateFlow<RunnerAccountsState> = stateFlow.asStateFlow()

    @Volatile
    private var store: RunnerAccountsStore? = null

    @Volatile
    private var storeApi: ApiService? = null

    private fun store(): RunnerAccountsStore? {
        val api = Services.get<ApiService>() ?: return null
        store?.takeIf { storeApi === api }?.let { return it }
        return synchronized(this) {
            store?.takeIf { storeApi === api } ?: RunnerAccountsStore(RunnerAttentionApi.from(api)).also { created ->
                created.addListener { next -> if (store === created) stateFlow.value = next }
                store = created
                storeApi = api
                stateFlow.value = created.state
            }
        }
    }

    fun reset() {
        synchronized(this) {
            store = null
            storeApi = null
        }
        stateFlow.value = RunnerAccountsState()
    }

    fun refresh(force: Boolean = false) {
        AppLifecycle.scope.launch(Dispatchers.IO) {
            runCatching {
                val current = store()
                if (current == null) {
                    stateFlow.value = RunnerAccountsState(loaded = true, hidden = true)
                } else {
                    current.refresh(force)
                    current.followPending()
                }
            }
        }
    }

    fun activate(runner: String, provider: String, account: RunnerAccount) {
        AppLifecycle.scope.launch(Dispatchers.IO) { runCatching { store()?.activate(runner, provider, account) } }
    }

    fun open(navigator: Navigator) {
        navigator.push(RunnerAccountsScreen())
    }

    fun signIn(navigator: Navigator, runner: String, provider: String) {
        navigator.push(RunnerAttentionFeature.signInScreen(runner, provider))
    }
}
