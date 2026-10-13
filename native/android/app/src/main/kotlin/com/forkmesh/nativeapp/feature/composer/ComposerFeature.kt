package com.forkmesh.nativeapp.feature.composer

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.agents.AgentLaunchOptions
import com.forkmesh.nativeapp.agents.AgentQueueResult
import com.forkmesh.nativeapp.agents.RunnerBranchName
import com.forkmesh.nativeapp.agents.AgentStartTarget
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.composer.PromptDraftStore
import com.forkmesh.nativeapp.composer.QuickAgentLauncher
import com.forkmesh.nativeapp.composer.QuickLaunchBackend
import com.forkmesh.nativeapp.composer.QuickLaunchRunOptions
import com.forkmesh.nativeapp.composer.ShellPromptTargets
import com.forkmesh.nativeapp.control.DesktopPromptImages
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object ComposerFeature {
    private val repoContextState = MutableStateFlow<String?>(null)
    private var quickState: QuickSendState? = null
    private var draftStore: PromptDraftStore? = null

    val repoContext: StateFlow<String?> = repoContextState.asStateFlow()

    val promptTargets = ShellPromptTargets()

    private val promptTargetTickState = MutableStateFlow(0)

    val promptTargetTick: StateFlow<Int> = promptTargetTickState.asStateFlow()

    init {
        promptTargets.addListener { promptTargetTickState.value = promptTargetTickState.value + 1 }
    }

    const val DEVICE_NAME = "this phone"

    fun install(app: App) {
        quickState = null
        draftStore = null
        repoContextState.value = null
        ProjectLifecycle.onClose {
            quickState = null
            draftStore = null
        }
    }

    fun projectPrefs(): PrefStore? {
        val base = Services.get<PrefStore>() ?: return null
        return Services.get<ProjectScope>()?.prefs(base) ?: base
    }

    val drafts: PromptDraftStore?
        get() = draftStore ?: projectPrefs()?.let { PromptDraftStore(it) }?.also { draftStore = it }

    internal fun quick(): QuickSendState =
        quickState ?: QuickSendState(QuickAgentLauncher(AppQuickLaunchBackend, projectPrefs(), DEVICE_NAME)).also { quickState = it }

    private val sharedAttachmentsState = MutableStateFlow<List<com.forkmesh.nativeapp.agents.PromptAttachment>>(emptyList())

    val sharedAttachments: StateFlow<List<com.forkmesh.nativeapp.agents.PromptAttachment>> = sharedAttachmentsState.asStateFlow()

    fun shareAttachments(attachments: List<com.forkmesh.nativeapp.agents.PromptAttachment>) {
        sharedAttachmentsState.value = sharedAttachmentsState.value + attachments
    }

    fun takeSharedAttachments(): List<com.forkmesh.nativeapp.agents.PromptAttachment> {
        val taken = sharedAttachmentsState.value
        sharedAttachmentsState.value = emptyList()
        return taken
    }

    fun followRepository(fullName: String?) {
        repoContextState.value = fullName
    }

    @Composable
    fun FollowRepository(fullName: String) {
        LaunchedEffect(fullName) { repoContextState.value = fullName }
    }
}

internal object AppQuickLaunchBackend : QuickLaunchBackend {
    override fun signedIn(): Boolean = AgentsFeature.signedIn()

    override fun loadTargets(): List<AgentStartTarget> {
        val repos = runCatching { Services.require<ApiService>().repositories() }.getOrDefault(emptyList())
        val records = runCatching {
            Services.get<LocalRepoStore>()?.let {
                it.ensureLoaded()
                it.list()
            } ?: emptyList()
        }.getOrDefault(emptyList())
        runCatching { AgentsFeature.hub.refreshAccountDesktops() }
        return AgentStartTarget.fold(repos, records)
    }

    override fun launchOptions(owner: String, name: String): AgentLaunchOptions = AgentsFeature.queue.launchOptions(owner, name)

    override fun launch(
        owner: String,
        name: String,
        prompt: String,
        provider: String,
        model: String,
        targetNode: String,
        title: String,
        orgScope: Boolean,
        attachments: List<PromptAttachment>,
    ): AgentQueueResult = AgentsFeature.queue.launch(
        owner,
        name,
        prompt = prompt,
        provider = provider,
        model = model,
        targetNode = targetNode,
        title = title,
        orgScope = orgScope,
        attachments = attachments,
        branchName = RunnerBranchName.slug(title),
    )

    override fun hasDeviceCredentials(): Boolean = runCatching { AgentsFeature.credentials.read() != null }.getOrDefault(false)

    override fun account(): String = AgentsFeature.accountName()

    override fun desktopName(): String =
        runCatching { AgentsFeature.hub.desktop?.let { it.name.ifEmpty { "Desktop" } } }.getOrNull() ?: ""

    override fun desktopDetail(): String = runCatching {
        val reach = AgentsFeature.hub.reach
        if (reach.isConnected) reach.label else "connects when you send"
    }.getOrDefault("")

    override fun startOnDesktop(
        owner: String,
        name: String,
        task: String,
        provider: String,
        model: String,
        options: QuickLaunchRunOptions,
        images: List<PromptAttachment>,
    ): Int {
        val params = linkedMapOf<String, Any?>("owner" to owner, "name" to name, "task" to task)
        if (provider.isNotEmpty()) params["provider"] = provider
        if (model.isNotEmpty()) params["model"] = model
        if (options.yolo) params["vibe"] = true
        if (!options.worktree) params["worktree"] = false
        val link = AgentsFeature.hub.link()
        val result = link.request("agents.start", DesktopPromptImages.params(params, images, link), 30_000)
        runCatching { AgentsFeature.control.refreshAsync() }
        return (result["id"] as? Number)?.toInt() ?: 0
    }
}
