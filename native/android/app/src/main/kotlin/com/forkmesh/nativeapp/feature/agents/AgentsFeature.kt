package com.forkmesh.nativeapp.feature.agents

import android.os.Build
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.agents.AgentCredentialStore
import com.forkmesh.nativeapp.agents.AgentEvent
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentLaunchTarget
import com.forkmesh.nativeapp.agents.AgentModelCatalog
import com.forkmesh.nativeapp.agents.AgentModelChoice
import com.forkmesh.nativeapp.agents.AgentQueueApi
import com.forkmesh.nativeapp.agents.RunnerAgentOps
import com.forkmesh.nativeapp.agents.AgentQueueResult
import com.forkmesh.nativeapp.agents.AgentSentPhotos
import com.forkmesh.nativeapp.agents.AgentSessionService
import com.forkmesh.nativeapp.agents.AgentStartPlan
import com.forkmesh.nativeapp.agents.EventLoopFrameChannel
import com.forkmesh.nativeapp.agents.NodeModelPreference
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.control.AgentControlCenter
import com.forkmesh.nativeapp.control.ControlHostSource
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.control.LocalControlService
import com.forkmesh.nativeapp.control.RelayControlConnection
import com.forkmesh.nativeapp.control.RelayControlOpener
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.control.agentRouteRowId
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountEventChannel
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.localrepos.GitAuthor
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.runners.RunnerSignInTarget
import com.forkmesh.nativeapp.security.DeployApprover
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.SecretStore
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.account.projects.ProjectScope
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class AgentBeat(val source: String, val key: String, val event: AgentEvent?)

object AgentsFeature {
    private lateinit var app: App
    private val signal = MutableStateFlow(0)
    private val beats = MutableSharedFlow<AgentBeat>(extraBufferCapacity = 256)

    val listSignal: StateFlow<Int> = signal.asStateFlow()

    private val live = MutableStateFlow(0)

    val liveSignal: StateFlow<Int> = live.asStateFlow()

    val agentBeats: SharedFlow<AgentBeat> = beats.asSharedFlow()

    @Volatile
    var meshSource: ControlHostSource? = null

    @Volatile
    var cloneToDevice: (suspend (FmMessenger, FmStatus?, owner: String, name: String, description: String) -> LocalRepoRecord?)? = null

    private val epoch = AtomicInteger()

    private class Runtime(private val app: App, private val beats: MutableSharedFlow<AgentBeat>, val epoch: Int) {
        private val scopePrefix: String = Services.get<ProjectScope>()?.namespace.orEmpty()

        val queue: AgentQueueApi by lazy { AgentQueueApi.from(Services.require()) }

        private val sessionsLazy = lazy {
            AgentSessionService(Services.require<PrefStore>(), scopePrefix = scopePrefix).apply {
                load()
                addListener {
                    val run = lastRun ?: return@addListener
                    beats.tryEmit(AgentBeat("device", run.task.repoName, run.events.lastOrNull()))
                }
            }
        }
        val sessions: AgentSessionService by sessionsLazy

        val credentials: AgentCredentialStore by lazy { AgentCredentialStore(Services.require<SecretStore>()) }

        val nodeModel: NodeModelPreference by lazy { NodeModelPreference(Services.require<PrefStore>()) }

        private val localControlLazy = lazy {
            val identity = Services.get<DeviceIdentity>() ?: return@lazy null
            LocalControlService(
                identity = identity,
                prefs = Services.require(),
                mesh = meshSource,
                scopePrefix = scopePrefix,
                userName = { accountName() },
                filesDirectory = { File(com.forkmesh.nativeapp.feature.account.projects.AppFiles.documentsDir(app), "ForkMesh from desktop") },
                platform = "android",
            ).also { service ->
                service.addEventTap { event -> beats.tryEmit(AgentBeat("desktop", event.event, null)) }
            }
        }
        val localControl: LocalControlService? by localControlLazy

        private val hubLazy = lazy {
            val prefs = Services.get<PrefStore>()?.let { base -> Services.get<ProjectScope>()?.prefs(base) ?: base }
            DesktopControlHub(localControl, relayOpener(epoch), accountDesktopsLoader = accountDesktopsLoader(epoch), prefs = prefs).also { hub ->
                ProjectLifecycle.scope.launch {
                    AppLifecycle.foreground.collect { if (current(epoch)) hub.keepWarm = it }
                }
            }
        }
        val hub: DesktopControlHub by hubLazy

        private val controlLazy = lazy { AgentControlCenter(hub, queue) }
        val control: AgentControlCenter by controlLazy

        fun dispose() {
            if (controlLazy.isInitialized()) runCatching { control.dispose() }
            if (hubLazy.isInitialized()) runCatching { hub.dispose() }
            if (localControlLazy.isInitialized()) runCatching { localControl?.dispose() }
            if (sessionsLazy.isInitialized()) runCatching { sessions.dispose() }
        }
    }

    @Volatile
    private var runtime: Runtime? = null

    private fun runtime(): Runtime = runtime ?: synchronized(this) {
        runtime ?: Runtime(app, beats, epoch.get()).also { runtime = it }
    }

    private fun current(started: Int): Boolean = started == epoch.get()

    private fun forgetProjectCaches() {
        RunnerAttentionFeature.reset()
        RunnerAccountsFeature.reset()
        AgentAttachmentCache.clear()
        AgentSentPhotos.shared.clear()
    }

    val queue: AgentQueueApi get() = runtime().queue

    val runnerOps: RunnerAgentOps get() = RunnerAgentOps(queue, approver = DeployApprover({ Services.get<DeviceIdentity>() }))

    val sessions: AgentSessionService get() = runtime().sessions

    val credentials: AgentCredentialStore get() = runtime().credentials

    val nodeModel: NodeModelPreference get() = runtime().nodeModel

    val localControl: LocalControlService? get() = runtime().localControl

    val hub: DesktopControlHub get() = runtime().hub

    val control: AgentControlCenter get() = runtime().control

    val sentPhotos: AgentSentPhotos get() = AgentSentPhotos.shared

    fun install(app: App) {
        this.app = app
        epoch.incrementAndGet()
        forgetProjectCaches()
        val previous = synchronized(this) { runtime.also { runtime = null } }
        previous?.dispose()
        val current = runtime()
        ProjectLifecycle.onClose {
            epoch.incrementAndGet()
            synchronized(this@AgentsFeature) { if (runtime === current) runtime = null }
            forgetProjectCaches()
            withContext(Dispatchers.IO) { current.dispose() }
        }
        if (Services.get<LocalRepoStore>() == null) {
            Services.put(LocalRepoStore.open(app.filesDir))
        }
        val channel = Services.get<AccountEventChannel>()
        if (channel != null) {
            val previous = channel.onTopic
            channel.onTopic = { topic ->
                previous?.invoke(topic)
                if (topic == "agents" || topic == "cloud-agents") ring()
                if (topic == "cloud-agents-live") live.value = live.value + 1
            }
        }
    }

    fun ring() {
        signal.value = signal.value + 1
        beats.tryEmit(AgentBeat("relay", "agents", null))
    }

    fun accountName(): String = Services.get<AuthService>()?.session?.value?.nodeName ?: ""

    fun ownerAccount(repo: Repository): String {
        val node = accountName().trim().lowercase()
        return node.ifEmpty { repo.owner }
    }

    fun hasDesktopNode(): Boolean {
        val session = Services.get<AuthService>()?.session?.value ?: return true
        return session.desktopDevices.isNotEmpty()
    }

    fun signedIn(): Boolean = Services.get<AuthService>()?.session?.value != null

    fun author(): GitAuthor {
        val session = Services.get<AuthService>()?.session?.value
        return GitAuthor.fromAccount(session?.nodeName ?: "", session?.email ?: "")
    }

    internal fun relayOpener(started: Int = epoch.get()): RelayControlOpener = RelayControlOpener { desktop ->
        if (!current(started)) throw com.forkmesh.nativeapp.control.LocalControlException("not_connected")
        val api = Services.require<ApiService>()
        val identity = Services.get<DeviceIdentity>() ?: throw com.forkmesh.nativeapp.control.LocalControlException("not_connected")
        val channel = EventLoopFrameChannel(api, { api.sessionTokenProvider?.invoke() ?: "" }, desktop.name.trim().lowercase())
        val connection = RelayControlConnection.open(
            channel = channel,
            identity = identity,
            desktopKey = desktop.key,
            node = desktop.name,
            deviceName = Build.MODEL ?: "",
            user = accountName(),
            platform = "android",
        )
        if (!current(started)) {
            runCatching { connection.close() }
            throw com.forkmesh.nativeapp.control.LocalControlException("not_connected")
        }
        connection
    }

    internal fun accountDesktopsLoader(started: Int = epoch.get()): () -> List<com.forkmesh.nativeapp.control.PairedDesktop> = {
        check(current(started)) { "project changed" }
        val api = Services.require<ApiService>()
        val listed = api.listAccountDesktops()
        check(current(started)) { "project changed" }
        listed.desktops.map { com.forkmesh.nativeapp.control.PairedDesktop(it.key, it.node, listed.account, 0) }
    }

    fun loginChannel(node: String): EventLoopFrameChannel {
        val api = Services.require<ApiService>()
        return EventLoopFrameChannel(api, { api.sessionTokenProvider?.invoke() ?: "" }, node.trim().lowercase())
    }

    fun tab(): Screen = AgentsScreen()

    fun startSheetScreen(initialPrompt: String = "", initialAttachments: List<PromptAttachment> = emptyList()): Screen =
        AgentStartScreen(initialPrompt, initialAttachments)

    suspend fun queueRun(
        repo: Repository,
        prompt: String,
        attachments: List<PromptAttachment> = emptyList(),
        model: AgentModelChoice = nodeModel.read(),
        targetNode: String = "",
    ): AgentQueueResult = withContext(Dispatchers.IO) {
        val options = queue.launchOptions(repo.owner, repo.name)
        val node = targetNode.ifEmpty { options.preferredTarget?.node ?: "" }
        val result = queue.launch(
            repo.owner,
            repo.name,
            prompt = prompt,
            provider = AgentStartPlan.providerFor(model),
            model = model.alias,
            targetNode = node,
            title = AgentStartPlan.titleFor(prompt),
            orgScope = options.orgScope,
            attachments = attachments,
        )
        result.session?.let { sentPhotos.record(it.sessionId, prompt.trim(), attachments) }
        ring()
        result
    }

    fun launchedSessionScreen(repo: Repository, session: AgentLaunchSession): Screen =
        if (session.sessionId.isEmpty()) LaunchedAgentSessionScreen(repo, session)
        else UnifiedAgentScreen(UnifiedAgentRow.fromCloud(session.copy(owner = repo.owner, repo = repo.name)))

    fun sessionScreen(repo: Repository, sessionId: Int): Screen = AgentSessionLoaderScreen(repo, sessionId)

    fun onDeviceAgentScreen(
        record: LocalRepoRecord,
        prompt: String = "",
        attachments: List<PromptAttachment> = emptyList(),
        autoStart: Boolean = false,
    ): Screen = AgentScreen(record, prompt, attachments, autoStart)

    fun localDesktopScreen(): Screen = LocalDesktopScreen()

    fun allAgentsScreen(): Screen = AllAgentsScreen()

    fun agentScreen(id: String): Screen = if (id.isBlank()) AllAgentsScreen() else AgentRouteScreen(agentRouteRowId(id))

    fun loginTerminalScreen(node: String, provider: String): Screen = AgentLoginTerminalScreen(node, provider)

    fun desktopTerminalScreen(terminal: String = ""): Screen = DesktopTerminalScreen(terminal)

    fun openLoginTerminal(navigator: Navigator, node: String, provider: String) {
        navigator.push(loginTerminalScreen(node, provider))
    }

    fun openRunnerSignIn(navigator: Navigator, runner: String, provider: String) {
        RunnerAttentionFeature.open(navigator, RunnerSignInTarget(runner.trim().lowercase(), provider.trim().lowercase())) { url ->
            openExternalUrl(App.instance, url)
        }
    }

    fun openSignIn(navigator: Navigator, target: AgentLaunchTarget, provider: String) {
        if (target.isRunner) {
            openRunnerSignIn(navigator, target.signInNode, provider)
        } else {
            openLoginTerminal(navigator, target.node, provider)
        }
    }

    val defaultNodeModel: AgentModelChoice get() = AgentModelCatalog.DEFAULT_NODE_MODEL
}
