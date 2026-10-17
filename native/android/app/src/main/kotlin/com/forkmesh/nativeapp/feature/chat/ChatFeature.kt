package com.forkmesh.nativeapp.feature.chat

import com.forkmesh.nativeapp.chat.DirectMessageFrames
import android.app.Activity
import android.app.Application
import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Bundle
import androidx.compose.runtime.Composable
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.chat.LocalMeshRoom
import com.forkmesh.nativeapp.chat.LocalNodeSyncApplier
import com.forkmesh.nativeapp.chat.MESH_CONVERSATION
import com.forkmesh.nativeapp.chat.MeshSnapshot
import com.forkmesh.nativeapp.chat.NodeSyncApplierSummary
import com.forkmesh.nativeapp.chat.NodeSyncAuthor
import com.forkmesh.nativeapp.chat.NodeSyncEngine
import com.forkmesh.nativeapp.chat.NodeSyncRepositories
import com.forkmesh.nativeapp.chat.NodeSyncSnapshot
import com.forkmesh.nativeapp.chat.RelayConnectionState
import com.forkmesh.nativeapp.chat.RelayRoom
import com.forkmesh.nativeapp.chat.RelaySnapshot
import com.forkmesh.nativeapp.chat.ChatHubSend
import com.forkmesh.nativeapp.chat.ChatReplyException
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.chat.guestComposeBlocked
import com.forkmesh.nativeapp.chat.meshAlert
import com.forkmesh.nativeapp.feature.push.PingNotifier
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import kotlinx.coroutines.delay
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.account.projects.FlutterFiles
import com.forkmesh.nativeapp.feature.account.projects.ProjectConnectionProbe
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.SecretStore
import com.forkmesh.nativeapp.tools.AppLog
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import java.io.File
import java.lang.ref.WeakReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

enum class RelayConnection { Connected, Connecting, Offline }

data class RelayShellState(
    val connection: RelayConnection = RelayConnection.Offline,
    val onlineCount: Int = 0,
    val unreadCount: Int = 0,
)

sealed class ChatSendResult {
    data object Sent : ChatSendResult()
    data object Empty : ChatSendResult()
    data class NeedsAccount(val action: String) : ChatSendResult()
    data class Failed(val message: String) : ChatSendResult()
}

object ChatFeature {
    private const val BLUETOOTH_PERMISSION_REQUEST = 4801
    private const val RETENTION_PRUNE_INTERVAL_MS = 5 * 60_000L

    private val state = MutableStateFlow(RelayShellState())
    private val relayState = MutableStateFlow<RelaySnapshot?>(null)
    private val meshState = MutableStateFlow<MeshSnapshot?>(null)
    private val nodeState = MutableStateFlow<NodeSyncSnapshot?>(null)
    private val applierState = MutableStateFlow<NodeSyncApplierSummary?>(null)

    val shellState: StateFlow<RelayShellState> get() = state
    val relaySnapshot: StateFlow<RelaySnapshot?> = relayState.asStateFlow()
    val meshSnapshot: StateFlow<MeshSnapshot?> = meshState.asStateFlow()
    val nodeSnapshot: StateFlow<NodeSyncSnapshot?> = nodeState.asStateFlow()
    val applierSummary: StateFlow<NodeSyncApplierSummary?> = applierState.asStateFlow()

    @Volatile
    var relay: RelayRoom? = null
        private set

    @Volatile
    var mesh: LocalMeshRoom? = null
        private set

    @Volatile
    var nodeSync: NodeSyncEngine? = null
        private set

    @Volatile
    private var applier: LocalNodeSyncApplier? = null

    @Volatile
    private var repositories: NodeSyncRepositories? = null

    @Volatile
    private var author: () -> NodeSyncAuthor = { NodeSyncAuthor("", "") }

    @Volatile
    var projectId: String = ""
        private set

    private var filesDir: File? = null
    private var transport: AndroidMeshTransport? = null
    private var resumedActivity: WeakReference<Activity>? = null
    private var trackingActivities = false
    private val relayLogState = MutableStateFlow<List<String>>(emptyList())

    @Volatile
    private var framesBefore = 0L

    @Volatile
    private var meshOnScreen = false

    @Volatile
    private var meshAlertShown = false

    private var appContext: Context? = null

    val relayLog: StateFlow<List<String>> = relayLogState.asStateFlow()

    fun relayFrames(): Long = framesBefore + (relay?.framesReceived?.toLong() ?: 0L)

    fun install(app: App) {
        val settings = Services.get<SettingsService>() ?: return
        val identity = Services.get<DeviceIdentity>() ?: return
        val secrets = Services.get<SecretStore>() ?: return
        val prefs = Services.get<PrefStore>() ?: return
        val auth = Services.get<AuthService>()
        val api = Services.get<ApiService>()
        val projectScope = Services.get<ProjectScope>() ?: ProjectScope.legacy
        projectId = if (projectScope.isLegacy) "" else projectScope.projectId
        appContext = app
        filesDir = FlutterFiles.projectSupportDir(app, projectScope)
        repositories = null
        if (!trackingActivities) {
            trackingActivities = true
            app.registerActivityLifecycleCallbacks(ActivityTracker)
        }

        val native = AndroidMeshTransport(app) { permissions ->
            val activity = resumedActivity?.get()
            if (activity == null || activity.isFinishing) {
                false
            } else {
                activity.requestPermissions(permissions, BLUETOOTH_PERMISSION_REQUEST)
                true
            }
        }
        transport = native

        val account: () -> String = { auth?.core?.session?.nodeName ?: "" }

        val room = RelayRoom(settings.core, identity, secrets, platform = "android")
        room.accountProvider = account
        room.roomPassphraseProvider = {
            if ((auth?.core?.session?.sessionToken ?: "").isEmpty() || api == null) "" else api.roomChatPassphrase()
        }
        room.onLogLine = { line -> Services.get<AppLog>()?.addRelayLine(line) }
        val relayListener: () -> Unit = { publishRelay(room) }
        room.addListener(relayListener)
        relay = room
        Services.put(room)
        ChatDepth.install(app, room, settings, projectScope)
        registerRoutes()
        publishRelay(room)

        val local = buildMesh(settings, identity, secrets, native, account)
        local.wakeSender = { tokens ->
            if ((auth?.core?.session?.sessionToken ?: "").isNotEmpty() && api != null) {
                AppLifecycle.scope.launch(Dispatchers.IO) {
                    runCatching { api.accountApiCall("POST", "/api/push/mesh", mapOf("tokens" to tokens)) }
                }
            }
        }
        local.onIncoming = { message, unread -> alertMesh(app, message, unread) }
        auth?.let { local.setChatRetention(it.chatRetention.value) }
        mesh = local
        publishMesh(local)
        native.onAdapterReady = { local.retryBluetooth() }
        val networkWatch = watchNetworks(app, local)

        val sync = NodeSyncEngine(settings.core, identity, session = { auth?.core?.session }, prefs = prefs, projectId = { projectId })
        sync.onNewItems = { Services.get<NotificationService>()?.refreshInBackground(silent = true) }
        sync.onUnauthorized = { code -> AppLifecycle.scope.launch { auth?.handleUnauthorized(code) } }
        sync.addListener { nodeState.value = sync.snapshot() }
        nodeSync = sync
        Services.put(sync)
        rebuildApplier()

        settings.core.addListener {
            room.identityChanged()
            sync.refreshSession()
        }

        Services.put<ProjectConnectionProbe>(
            ProjectConnectionProbe {
                when (room.state) {
                    RelayConnectionState.CONNECTED -> true
                    RelayConnectionState.CONNECTING -> null
                    RelayConnectionState.OFFLINE -> null
                }
            },
        )
        ProjectLifecycle.onClose {
            room.removeListener(relayListener)
            room.onLogLine = null
            withContext(Dispatchers.IO) {
                runCatching { sync.stop() }
                applier?.dispose()
                applier = null
                framesBefore += room.framesReceived.toLong()
                AppLifecycle.scope.launch(Dispatchers.IO) { runCatching { room.disconnect() } }
                networkWatch?.let { callback ->
                    runCatching { app.getSystemService(ConnectivityManager::class.java)?.unregisterNetworkCallback(callback) }
                }
                runCatching {
                    local.stop()
                    local.dispose()
                }
                runCatching { native.dispose() }
            }
            if (transport === native) transport = null
            if (relay === room) relay = null
            if (mesh === local) mesh = null
            if (nodeSync === sync) nodeSync = null
            repositories = null
            applierState.value = null
        }

        val scope = ProjectLifecycle.scope
        scope.launch(Dispatchers.IO) {
            local.start()
            local.pruneExpired()
            publishMesh(local)
            room.connect()
        }
        scope.launch(Dispatchers.IO) {
            sync.loadCarried()
            sync.start()
            nodeState.value = sync.snapshot()
        }
        if (auth != null) {
            scope.launch {
                auth.session.collect {
                    room.identityChanged()
                    sync.refreshSession()
                }
            }
            scope.launch {
                auth.chatRetention.collect { retention ->
                    withContext(Dispatchers.IO) {
                        ChatDepth.hub?.setRetention(retention)
                        local.setChatRetention(retention)
                        local.pruneExpired()
                    }
                }
            }
            scope.launch(Dispatchers.IO) {
                while (true) {
                    delay(RETENTION_PRUNE_INTERVAL_MS)
                    ChatDepth.hub?.pruneExpired()
                    local.pruneExpired()
                }
            }
            scope.launch {
                combine(auth.session, auth.userStatus) { session, status -> if (session == null) null else status }
                    .distinctUntilChanged()
                    .collect { room.setOwnStatus(it) }
            }
        }
        scope.launch {
            combine(settings.serverUrl, settings.room, settings.passphrase) { url, name, phrase -> Triple(url, name, phrase) }
                .distinctUntilChanged()
                .drop(1)
                .collect {
                    withContext(Dispatchers.IO) {
                        if (room.state != RelayConnectionState.OFFLINE) room.connect()
                    }
                }
        }
        scope.launch {
            AppLifecycle.foreground.collect { foreground ->
                sync.setForeground(foreground)
                if (foreground) {
                    room.onResumed()
                    local.announceNow()
                }
            }
        }
    }

    private fun buildMesh(
        settings: SettingsService,
        identity: DeviceIdentity,
        secrets: SecretStore,
        native: AndroidMeshTransport,
        account: () -> String,
    ): LocalMeshRoom {
        val historyFile = filesDir?.let { dir -> File(dir, "local_mesh_history.json") }
        val local = LocalMeshRoom(settings.core, identity, secrets, native = native, platform = "android", historyFile = { historyFile })
        local.accountProvider = account
        local.onDirect = { kid, frame -> ChatDepth.direct?.acceptMeshFrame(kid, frame) }
        local.onLinksChanged = { ChatDepth.direct?.meshLinksChanged() }
        local.addListener { publishMesh(local) }
        Services.put(local)
        return local
    }

    private fun watchNetworks(app: App, local: LocalMeshRoom): ConnectivityManager.NetworkCallback? {
        val connectivity = app.getSystemService(ConnectivityManager::class.java) ?: return null
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = local.announceNow()

            override fun onLost(network: Network) = local.announceNow()

            override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) = local.announceNow()
        }
        val request = NetworkRequest.Builder()
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
            .build()
        return try {
            connectivity.registerNetworkCallback(request, callback)
            callback
        } catch (_: RuntimeException) {
            null
        }
    }

    fun useNodeSyncRepositories(repos: NodeSyncRepositories?, author: () -> NodeSyncAuthor) {
        repositories = repos
        this.author = author
        rebuildApplier()
    }

    private fun rebuildApplier() {
        val sync = nodeSync ?: return
        val prefs = Services.get<PrefStore>() ?: return
        applier?.dispose()
        applier = null
        applierState.value = null
        val repos = repositories ?: run {
            sync.applier = null
            return
        }
        val key = if (projectId.isEmpty()) LocalNodeSyncApplier.APPLIED_PREFS_KEY else "p/$projectId/${LocalNodeSyncApplier.APPLIED_PREFS_KEY}"
        val next = LocalNodeSyncApplier(repos, author = { author() }, prefs = prefs, ledgerKey = key)
        next.installOn(sync)
        next.addListener { applierState.value = next.summary() }
        applier = next
        applierState.value = next.summary()
    }

    private fun publishRelay(room: RelayRoom) {
        val snapshot = room.snapshot()
        relayState.value = snapshot
        relayLogState.value = snapshot.log
        state.value = RelayShellState(
            connection = when (snapshot.state) {
                RelayConnectionState.CONNECTED -> RelayConnection.Connected
                RelayConnectionState.CONNECTING -> RelayConnection.Connecting
                RelayConnectionState.OFFLINE -> RelayConnection.Offline
            },
            onlineCount = snapshot.onlineCount,
            unreadCount = snapshot.badgeUnread,
        )
    }

    private fun publishMesh(local: LocalMeshRoom) {
        val snapshot = local.snapshot()
        meshState.value = snapshot
        if (snapshot.unread == 0 && meshAlertShown) {
            meshAlertShown = false
            appContext?.let { PingNotifier.cancelMesh(it) }
        }
    }

    private fun alertMesh(context: Context, message: ChatMessage, unread: Int) {
        val watching = meshOnScreen && AppLifecycle.foreground.value
        val alert = meshAlert(message, unread, watching, System.currentTimeMillis()) ?: return
        meshAlertShown = true
        PingNotifier.postMesh(context, alert, message.timestamp)
    }

    fun setMeshOnScreen(visible: Boolean) {
        meshOnScreen = visible
    }

    fun tab(): Screen = ChatScreen()

    fun markAllRead() {
        val room = relay ?: return
        AppLifecycle.scope.launch(Dispatchers.IO) { ChatDepth.hub?.markAllRead() ?: room.markAllRead() }
    }

    fun markBadgeSeen() {
        val room = relay ?: return
        AppLifecycle.scope.launch(Dispatchers.IO) { room.markBadgeSeen() }
    }

    fun reconnect() {
        val room = relay ?: return
        AppLifecycle.scope.launch(Dispatchers.IO) { room.connect() }
    }

    fun disconnect() {
        val room = relay ?: return
        AppLifecycle.scope.launch(Dispatchers.IO) { room.disconnect() }
    }

    suspend fun sendChatText(text: String): ChatSendResult {
        if (text.isBlank()) return ChatSendResult.Empty
        val room = relay ?: return ChatSendResult.Failed("Chat is not available.")
        val conversation = room.currentConversation
        val guest = Services.get<AuthService>()?.isGuest?.value == true
        guestComposeBlocked(guest, conversation)?.let { return ChatSendResult.NeedsAccount(it) }
        return withContext(Dispatchers.IO) {
            val chat = ChatDepth.hub
            if (conversation == MESH_CONVERSATION) {
                val id = mesh?.sendMessage(text) ?: ""
                if (id.isEmpty()) ChatSendResult.Failed("No one is on this network or Bluetooth yet.") else ChatSendResult.Sent
            } else if (chat != null) {
                when (val result = chat.sendText(conversation, text)) {
                    ChatHubSend.Sent -> ChatSendResult.Sent
                    ChatHubSend.Empty -> ChatSendResult.Empty
                    is ChatHubSend.Issue -> {
                        ChatDepth.requestIssue(result.argument)
                        ChatSendResult.Sent
                    }
                    is ChatHubSend.Failed -> ChatSendResult.Failed(result.message)
                }
            } else if (room.sendMessage(text)) {
                ChatSendResult.Sent
            } else {
                room.connect()
                ChatSendResult.Failed("Chat is offline. Reconnecting...")
            }
        }
    }

    fun openConversation(name: String) {
        AppLifecycle.scope.launch(Dispatchers.IO) {
            val chat = ChatDepth.hub
            if (chat == null) {
                runCatching { relay?.switchConversation(name) }
                return@launch
            }
            val target = chat.resolve(name)
            if (target != null) {
                chat.select(target)
            } else {
                if (name.startsWith("@")) ChatDepth.post("${name.drop(1)} is not online right now.")
                chat.select(name)
            }
        }
    }

    fun threadScreen(channel: String, rootId: String): Screen =
        ChatThreadScreen(if (channel.startsWith("#") || DirectMessageFrames.isDirect(channel)) channel else "#$channel", rootId)

    fun discordScreen(guild: String = "", channel: String? = null): Screen = DiscordScreen(guild, channel)

    suspend fun replyToConversation(channel: String, text: String) {
        val chat = ChatDepth.hub ?: throw ChatReplyException(ChatReplyException.Kind.OFFLINE)
        withContext(Dispatchers.IO) {
            val room = relay
            if (room != null && room.state != RelayConnectionState.CONNECTED && !OfficeFrames.isOffice(channel)) {
                room.connect()
                val deadline = System.currentTimeMillis() + 10_000
                while (room.state != RelayConnectionState.CONNECTED && System.currentTimeMillis() < deadline) delay(100)
            }
            chat.reply(channel, text)
        }
    }

    private fun registerRoutes() {
        NavigationFeature.register(Route.Kind.Discord, "WS13") { route ->
            val discord = route as? Route.Discord
            DiscordScreen(discord?.guild.orEmpty(), discord?.channel)
        }
    }

    @Composable
    fun NodeActivitySection(navigator: Navigator) {
        NodeActivityCardView()
    }

    private object ActivityTracker : Application.ActivityLifecycleCallbacks {
        override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {}

        override fun onActivityStarted(activity: Activity) {}

        override fun onActivityResumed(activity: Activity) {
            resumedActivity = WeakReference(activity)
            mesh?.retryBluetooth()
        }

        override fun onActivityPaused(activity: Activity) {
            if (resumedActivity?.get() === activity) resumedActivity = null
        }

        override fun onActivityStopped(activity: Activity) {}

        override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}

        override fun onActivityDestroyed(activity: Activity) {
            if (resumedActivity?.get() === activity) resumedActivity = null
        }
    }
}
