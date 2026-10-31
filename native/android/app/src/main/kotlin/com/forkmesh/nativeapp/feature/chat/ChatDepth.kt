package com.forkmesh.nativeapp.feature.chat

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.api.ServerUrl
import com.forkmesh.nativeapp.apps.AppsApi
import com.forkmesh.nativeapp.apps.ChatApps
import com.forkmesh.nativeapp.chat.ChannelGroupsStore
import com.forkmesh.nativeapp.chat.ChatAccountCalling
import com.forkmesh.nativeapp.chat.ChatBlocks
import com.forkmesh.nativeapp.chat.NodeOwners
import com.forkmesh.nativeapp.chat.ChatHistoryCodec
import com.forkmesh.nativeapp.chat.ChatHistoryStore
import com.forkmesh.nativeapp.chat.ChatHub
import com.forkmesh.nativeapp.chat.ChatLinkPreviewService
import com.forkmesh.nativeapp.chat.ChatLinkUrlConnectionHttp
import com.forkmesh.nativeapp.chat.ChatReadMarks
import com.forkmesh.nativeapp.chat.ChatWelcomeAnnouncer
import com.forkmesh.nativeapp.chat.DirectMeshLinks
import com.forkmesh.nativeapp.chat.DirectMessageService
import com.forkmesh.nativeapp.chat.MeshRoute
import com.forkmesh.nativeapp.chat.DiscordBridge
import com.forkmesh.nativeapp.chat.FamilyDiscordControl
import com.forkmesh.nativeapp.chat.LocalMeshRoom
import com.forkmesh.nativeapp.chat.MESH_CONVERSATION
import com.forkmesh.nativeapp.chat.OfficeChannelService
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.chat.RelayConnectionState
import com.forkmesh.nativeapp.chat.RelayRoom
import com.forkmesh.nativeapp.chat.RelayRoomChat
import com.forkmesh.nativeapp.chat.WebSocketOfficeConnector
import com.forkmesh.nativeapp.control.DesktopControlFamily
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountEventChannel
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.account.projects.AppFiles
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.push.CallRings
import com.forkmesh.nativeapp.push.DIRECT_MESSAGES_TOPIC
import com.forkmesh.nativeapp.security.SecretStore
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object ChatDepth {
    private const val SAVE_DELAY_MS = 2_000L
    private const val LAST_CONVERSATION_KEY = "chat/lastConversation"

    private val revisionState = MutableStateFlow(0)
    private val noticeFlow = MutableSharedFlow<String>(extraBufferCapacity = 8)
    private val pendingIssueState = MutableStateFlow<String?>(null)
    private val revealState = MutableStateFlow(0)

    val revision: StateFlow<Int> = revisionState.asStateFlow()
    val notices: SharedFlow<String> = noticeFlow.asSharedFlow()
    val pendingIssue: StateFlow<String?> = pendingIssueState.asStateFlow()
    val revealRequests: StateFlow<Int> = revealState.asStateFlow()

    @Volatile
    var hub: ChatHub? = null
        private set

    @Volatile
    private var conversationChosen = false

    @Volatile
    private var lastConversationKey = LAST_CONVERSATION_KEY

    @Volatile
    var office: OfficeChannelService? = null
        private set

    @Volatile
    var direct: DirectMessageService? = null
        private set

    @Volatile
    var groups: ChannelGroupsStore? = null
        private set

    @Volatile
    var apps: ChatApps? = null
        private set

    @Volatile
    var linkPreviews: ChatLinkPreviewService? = null
        private set

    @Volatile
    private var discordBridge: DiscordBridge? = null

    @Volatile
    private var discordHub: DesktopControlHub? = null

    private var saveJob: Job? = null

    fun bump() {
        revisionState.value = revisionState.value + 1
    }

    fun post(notice: String) {
        if (notice.isNotEmpty()) noticeFlow.tryEmit(notice)
    }

    fun requestIssue(argument: String) {
        pendingIssueState.value = argument
    }

    fun clearIssue() {
        pendingIssueState.value = null
    }

    fun discord(): DiscordBridge? {
        val desktops = runCatching { AgentsFeature.hub }.getOrNull() ?: return null
        synchronized(this) {
            val current = discordBridge
            if (current != null && discordHub === desktops) return current
            current?.stopWatching()
            val next = DiscordBridge(FamilyDiscordControl(DesktopControlFamily(DesktopControlFamilyName.Discord, desktops)))
            next.addListener { bump() }
            discordBridge = next
            discordHub = desktops
            return next
        }
    }

    fun restoreLastConversation() {
        if (conversationChosen) return
        conversationChosen = true
        val chat = hub ?: return
        val room = ChatFeature.relay ?: return
        val last = Services.get<PrefStore>()?.getString(lastConversationKey).orEmpty()
        if (last.isEmpty() || last == room.currentConversation) return
        if (last.startsWith("#") && !OfficeFrames.isOffice(last) && last != MESH_CONVERSATION && last !in room.channels) {
            room.addChannel(last)
        }
        chat.select(last)
    }

    fun install(app: App, room: RelayRoom, settings: SettingsService, scopeInfo: ProjectScope) {
        val auth = Services.get<AuthService>()
        val api = Services.get<ApiService>()
        val secrets = Services.get<SecretStore>()
        val prefs = Services.get<PrefStore>()
        val chat = ChatHub(RelayRoomChat(room))
        room.frameObserver = { frame -> chat.extras.apply(frame) }
        val lastKey = scopeInfo.key(LAST_CONVERSATION_KEY)
        lastConversationKey = lastKey
        conversationChosen = false
        room.onConversationSwitched = { conversation ->
            conversationChosen = true
            if (conversation.isNotEmpty()) prefs?.putString(lastKey, conversation)
        }
        chat.onNotice = { post(it) }
        chat.onReveal = { revealState.value = revealState.value + 1 }
        chat.selfName = { auth?.session?.value?.nodeName ?: room.persona.name }
        chat.extras.onChange = {
            bump()
            scheduleSave()
        }
        auth?.let { chat.setRetention(it.chatRetention.value) }
        hub = chat

        val historyScope = {
            ChatHistoryCodec.projectScope(scopeInfo.projectId.ifEmpty { settings.core.serverUrl }, settings.core.room)
        }
        val formerScopes = { ChatHistoryCodec.formerScopes(settings.core.serverUrl, settings.core.room) }
        val historyDir = File(AppFiles.projectSupportDir(app, scopeInfo), "chat-history")
        chat.attachHistory(ChatHistoryStore(historyDir), formerScopes, historyScope)
        room.addListener { scheduleSave() }
        val previewProject = scopeInfo.projectId.ifEmpty { settings.core.serverUrl }
        val previews = ChatLinkPreviewService(
            http = ChatLinkUrlConnectionHttp(),
            prefs = prefs,
            fileSource = { File(app.cacheDir, ChatLinkPreviewService.fileName(previewProject)) },
            saveLater = { task ->
                ProjectLifecycle.scope.launch(Dispatchers.IO) {
                    delay(SAVE_DELAY_MS)
                    task.run()
                }
            },
        )
        ChatLinkPreviewUi.bind(previews)
        linkPreviews = previews
        if (prefs != null) groups = ChannelGroupsStore.forProject(prefs, scopeInfo.projectId, settings.core.serverUrl, settings.core.room)

        if (api != null && secrets != null) {
            val calling = ChatAccountCalling { method, path, body -> api.accountApiCall(method, path, body) }
            val service = OfficeChannelService(
                api = calling,
                connector = WebSocketOfficeConnector,
                secureStore = secrets,
                origin = { ServerUrl.parse(settings.core.serverUrl).httpOrigin },
                selfId = { room.selfId },
                displayName = { auth?.session?.value?.nodeName ?: room.persona.name },
                account = { room.persona.ownerUser },
                nodeKey = { room.nodeKey },
                owners = { room.owners },
            )
            service.addListener {
                bump()
                scheduleSave()
            }
            office = service
            chat.office = service
            GroupCallFeature.install(app, service) { auth?.session?.value?.nodeName.orEmpty() }
            val directService = DirectMessageService(
                api = calling,
                connector = WebSocketOfficeConnector,
                secureStore = secrets,
                origin = { ServerUrl.parse(settings.core.serverUrl).httpOrigin },
                selfId = { room.selfId },
                displayName = { auth?.session?.value?.nodeName ?: room.persona.name },
                account = { room.persona.ownerUser },
            )
            directService.addListener {
                bump()
                scheduleSave()
            }
            directService.mesh = object : DirectMeshLinks {
                private fun room(): LocalMeshRoom? = Services.get<LocalMeshRoom>()

                override fun linkedKids(): Map<String, MeshRoute> = room()?.linkedKids() ?: emptyMap()

                override fun peerFocused(kid: String, channel: String): Boolean = room()?.peerFocused(kid, channel) == true

                override fun sendDirect(kid: String, frame: Map<String, Any?>): Boolean = room()?.sendDirect(kid, frame) == true

                override fun setFocus(channel: String, on: Boolean, kids: Set<String>) {
                    room()?.setFocus(channel, on, kids)
                }
            }
            direct = directService
            chat.direct = directService
            DirectLiveFeature.install(app, chat, directService)
            chat.calls = DirectCallFeature.install(
                context = app,
                direct = directService,
                selfId = { room.selfId },
                accountName = { auth?.session?.value?.nodeName.orEmpty() },
            )
            val readMarks = ChatReadMarks(calling)
            chat.readMarks = readMarks
            Services.get<AccountEventChannel>()?.let { channel ->
                val previous = channel.onTopic
                channel.onTopic = { topic ->
                    previous?.invoke(topic)
                    if (topic == DIRECT_MESSAGES_TOPIC && direct === directService) {
                        ProjectLifecycle.scope.launch(Dispatchers.IO) { directService.refresh() }
                    } else if (topic == ChatReadMarks.TOPIC && hub === chat) {
                        ProjectLifecycle.scope.launch(Dispatchers.IO) { chat.applyReadMarks(force = true) }
                    }
                }
                val previousEvent = channel.onEvent
                channel.onEvent = { topic, frame ->
                    previousEvent?.invoke(topic, frame)
                    val ringing = CallRings.conversation(frame)
                    if (ringing != null && direct === directService) {
                        ProjectLifecycle.scope.launch(Dispatchers.IO) { runCatching { directService.openRinging(ringing) } }
                    }
                }
                val previousConnected = channel.onConnected
                channel.onConnected = {
                    previousConnected?.invoke()
                    if (hub === chat) ProjectLifecycle.scope.launch(Dispatchers.IO) { chat.applyReadMarks() }
                }
            }
            chat.forkbot = calling
            val owners = NodeOwners(calling)
            owners.addListener {
                chat.extras.relabelBots()
                service.relabelBots()
            }
            room.owners = owners
            val blocks = ChatBlocks(calling, prefs)
            blocks.onChange = { bump() }
            chat.blocks = blocks
            val appsService = ChatApps(AppsApi(api))
            appsService.onChange = { bump() }
            appsService.onNotice = { post(it) }
            appsService.enabled = { appsAllowed() }
            apps = appsService
            chat.apps = appsService
        }

        ProjectLifecycle.onClose {
            withContext(Dispatchers.IO) {
                chat.calls?.let { DirectCallFeature.uninstall(it, direct) }
                DirectLiveFeature.uninstall(chat)
                runCatching { chat.saveHistory() }
                office?.closeAll()
                direct?.closeAll()
                discordBridge?.stopWatching()
                previews.leaveProject()
            }
            if (linkPreviews === previews) linkPreviews = null
            ChatLinkPreviewUi.clear()
            room.frameObserver = null
            room.owners = null
            chat.calls = null
            if (hub === chat) hub = null
            office = null
            direct = null
            apps = null
            groups = null
            discordBridge = null
            discordHub = null
            pendingIssueState.value = null
        }

        val scope = ProjectLifecycle.scope
        if (auth != null) {
            scope.launch {
                var seen = false
                auth.session.map { it?.sessionToken.orEmpty() }.distinctUntilChanged().collect { token ->
                    val changed = seen
                    seen = true
                    withContext(Dispatchers.IO) {
                        if (changed) apps?.reset()
                        office?.let { if (token.isEmpty()) it.closeAll() else it.refresh() }
                        direct?.let {
                            if (token.isEmpty()) {
                                chat.directSessionChanged("")
                                chat.live?.clear()
                                it.clear()
                            } else {
                                chat.directSessionChanged(auth.session.value?.nodeName.orEmpty())
                                it.refresh()
                            }
                        }
                        if (token.isEmpty()) chat.readMarks?.clear() else chat.applyReadMarks(force = true)
                        room.owners?.clear()
                        chat.blocks?.let { if (token.isEmpty()) it.clear() else it.signedIn() }
                    }
                }
            }
            scope.launch(Dispatchers.IO) { welcome(room, auth) }
        }
    }

    private fun appsAllowed(): Boolean {
        val auth = Services.get<AuthService>() ?: return false
        val session = auth.session.value ?: return false
        return session.sessionToken.isNotEmpty() && session.sessionKind != "preview" && !auth.isGuest.value
    }

    suspend fun loadAppCommands() {
        if (!appsAllowed()) return
        val service = apps ?: return
        if (service.loaded) return
        withContext(Dispatchers.IO) { service.load() }
    }

    private fun scheduleSave() {
        val chat = hub ?: return
        saveJob?.cancel()
        saveJob = ProjectLifecycle.scope.launch(Dispatchers.IO) {
            delay(SAVE_DELAY_MS)
            runCatching { chat.saveHistory() }
        }
    }

    private suspend fun welcome(room: RelayRoom, auth: AuthService) {
        repeat(60) {
            if (room.state == RelayConnectionState.CONNECTED) return@repeat
            delay(1_000)
        }
        if (room.state != RelayConnectionState.CONNECTED) return
        val session = auth.session.value ?: return
        if (session.sessionKind == "preview") return
        val prefs = Services.get<PrefStore>() ?: return
        delay(3_000)
        val announcer = ChatWelcomeAnnouncer(prefs)
        val account = session.nodeName
        val already = room.messages(ChatWelcomeAnnouncer.CHANNEL).any {
            it.senderName.equals(account, ignoreCase = true) && it.text == ChatWelcomeAnnouncer.TEXT
        }
        val should = announcer.shouldAnnounce(
            account = account,
            isGuest = auth.isGuest.value,
            emailVerified = session.emailVerified,
            createdAtMs = session.createdAt,
            nowMs = System.currentTimeMillis(),
            alreadyInRoom = already,
        )
        if (!should) return
        announcer.markAnnounced(account)
        room.addChannel(ChatWelcomeAnnouncer.CHANNEL)
        room.sendMessage(ChatWelcomeAnnouncer.TEXT, ChatWelcomeAnnouncer.CHANNEL)
    }
}
