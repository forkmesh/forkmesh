package com.forkmesh.nativeapp.feature.agents

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.runners.RunnerAttention
import com.forkmesh.nativeapp.runners.RunnerAttentionApi
import com.forkmesh.nativeapp.runners.RunnerAttentionItem
import com.forkmesh.nativeapp.runners.RunnerAttentionStore
import com.forkmesh.nativeapp.runners.RunnerSignInTarget
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import java.text.DateFormat
import java.util.Date
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

object RunnerAttentionFeature {
    private val itemsState = MutableStateFlow<List<RunnerAttentionItem>>(emptyList())

    val items: StateFlow<List<RunnerAttentionItem>> = itemsState.asStateFlow()

    @Volatile
    private var store: RunnerAttentionStore? = null

    @Volatile
    private var storeApi: ApiService? = null

    private fun store(): RunnerAttentionStore? {
        val api = Services.get<ApiService>() ?: return null
        store?.takeIf { storeApi === api }?.let { return it }
        return synchronized(this) {
            store?.takeIf { storeApi === api } ?: RunnerAttentionStore(RunnerAttentionApi.from(api)).also { created ->
                created.addListener { items -> if (store === created) itemsState.value = items }
                store = created
                storeApi = api
            }
        }
    }

    fun api(): RunnerAttentionApi? = Services.get<ApiService>()?.let { RunnerAttentionApi.from(it) }

    fun reset() {
        synchronized(this) {
            store = null
            storeApi = null
        }
        itemsState.value = emptyList()
    }

    fun refresh(force: Boolean = false) {
        AppLifecycle.scope.launch(Dispatchers.IO) { runCatching { store()?.refresh(force) } }
    }

    fun refreshAfterSignIn() {
        AppLifecycle.scope.launch(Dispatchers.IO) {
            for (wait in listOf(0L, 6_000L, 14_000L)) {
                delay(wait)
                runCatching { store()?.refresh(force = true) }
                RunnerAccountsFeature.refresh(force = true)
            }
        }
    }

    fun signInScreen(runner: String, provider: String, openLoginId: String = ""): Screen =
        RunnerSignInScreen(runner, provider, openLoginId)

    fun open(navigator: Navigator, item: RunnerAttentionItem, openUrl: (String) -> Unit) {
        if (!item.canConnect) {
            api()?.let { openUrl(it.connectUrl(item)) }
            return
        }
        navigator.push(signInScreen(item.runner, item.provider, item.openLoginId))
    }

    fun open(navigator: Navigator, target: RunnerSignInTarget, openUrl: (String) -> Unit) {
        val known = items.value.firstOrNull { it.runner == target.runner && it.provider == target.provider }
        if (known != null) {
            open(navigator, known, openUrl)
            return
        }
        navigator.push(signInScreen(target.runner, target.provider, target.openLoginId))
    }

    fun expiryText(item: RunnerAttentionItem): String =
        if (item.expiresAtMs <= 0) "" else DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(item.expiresAtMs))

    @Composable
    fun Watcher() {
        LaunchedEffect(Unit) { refresh() }
        LaunchedEffect(Unit) {
            var wasForeground = AppLifecycle.foreground.value
            AppLifecycle.foreground.collect { now ->
                if (now && !wasForeground) refresh()
                wasForeground = now
            }
        }
        LaunchedEffect(Unit) {
            val service = Services.get<NotificationService>() ?: return@LaunchedEffect
            var seen: Set<String>? = null
            service.notifications.collect { list ->
                val ids = list.filter { it.isUnread && RunnerAttention.isRunnerPing(it.kind, it.href) }.map { it.id }.toSet()
                val before = seen
                if (before != null && (ids - before).isNotEmpty()) refresh(force = true)
                seen = ids
            }
        }
    }
}
