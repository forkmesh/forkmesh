package com.forkmesh.nativeapp.feature.account

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.NotificationInbox
import com.forkmesh.nativeapp.inbox.PingSnapshots
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationPage
import com.forkmesh.nativeapp.model.PingCelebration
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class NotificationService(
    api: ApiService,
    nodeName: () -> String,
    private val scope: CoroutineScope = AppLifecycle.scope,
    snapshots: PingSnapshots? = null,
    val project: String = "",
) {
    val inbox = NotificationInbox(api, nodeName, snapshots, project)

    private val notificationsState = MutableStateFlow(inbox.notifications)
    private val unreadState = MutableStateFlow(inbox.unreadCount)
    private val totalState = MutableStateFlow(inbox.inboxTotal)
    private val loadingState = MutableStateFlow(inbox.loading)
    private val errorState = MutableStateFlow(inbox.error)
    private val loadedState = MutableStateFlow(inbox.hasLoaded)

    val notifications: StateFlow<List<ForkNotification>> = notificationsState.asStateFlow()

    val unreadCount: StateFlow<Int> = unreadState.asStateFlow()

    val inboxTotal: StateFlow<Int> = totalState.asStateFlow()

    val loading: StateFlow<Boolean> = loadingState.asStateFlow()

    val error: StateFlow<String> = errorState.asStateFlow()

    val hasLoaded: StateFlow<Boolean> = loadedState.asStateFlow()

    data class Celebration(val count: Int, val ping: ForkNotification)

    private val celebrationState = MutableStateFlow<Celebration?>(null)
    private val celebrated = mutableSetOf<String>()

    val celebration: StateFlow<Celebration?> = celebrationState.asStateFlow()

    private val listener: () -> Unit = { publish() }

    init {
        inbox.addListener(listener)
    }

    fun publish() {
        notificationsState.value = inbox.notifications
        unreadState.value = inbox.unreadCount
        totalState.value = inbox.inboxTotal
        loadingState.value = inbox.loading
        errorState.value = inbox.error
        loadedState.value = inbox.hasLoaded
        celebrateFresh()
    }

    @Synchronized
    private fun celebrateFresh() {
        val fresh = PingCelebration.fresh(inbox.notifications, celebrated, System.currentTimeMillis())
        val newest = fresh.maxByOrNull { it.ts } ?: return
        fresh.forEach { celebrated.add(it.id) }
        celebrationState.value = Celebration((celebrationState.value?.count ?: 0) + 1, newest)
    }

    fun start() {
        if (inbox.needsRefresh()) refreshInBackground()
    }

    suspend fun refresh(silent: Boolean = false) {
        withContext(Dispatchers.IO) { inbox.refresh(silent) }
        publish()
    }

    fun refreshInBackground(silent: Boolean = false) {
        scope.launch { refresh(silent) }
    }

    fun find(id: String): ForkNotification? = inbox.find(id)

    fun insert(row: ForkNotification): Boolean {
        val added = inbox.insert(row)
        publish()
        return added
    }

    fun accept(node: String, page: NotificationPage) {
        inbox.accept(node, page)
        publish()
    }

    suspend fun markAllRead() {
        withContext(Dispatchers.IO) { inbox.markAllRead() }
        publish()
    }

    suspend fun deleteAll() {
        withContext(Dispatchers.IO) { inbox.deleteAll() }
        publish()
    }

    suspend fun delete(notification: ForkNotification) {
        try {
            withContext(Dispatchers.IO) { inbox.delete(notification) }
        } finally {
            publish()
        }
    }

    suspend fun markRead(notification: ForkNotification) {
        withContext(Dispatchers.IO) { inbox.markRead(notification) }
        publish()
    }

    fun dispose() {
        inbox.removeListener(listener)
    }
}
