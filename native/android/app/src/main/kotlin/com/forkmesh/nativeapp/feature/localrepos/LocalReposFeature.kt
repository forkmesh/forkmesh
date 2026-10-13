package com.forkmesh.nativeapp.feature.localrepos

import android.content.Context
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.InboxService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.chat.NodeSyncRepositories
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.projects.FlutterFiles
import com.forkmesh.nativeapp.localrepos.GitAuthor
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.LocalRepoNodeSync
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.localrepos.PublishService
import com.forkmesh.nativeapp.localrepos.PullRequestService
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

fun interface LocalRepoAgentLauncher {
    fun open(request: LocalRepoAgentRequest)
}

class LocalRepoAgentRequest(
    val navigator: Navigator,
    val record: LocalRepoRecord,
    val workspace: GitWorkspace,
    val author: GitAuthor,
    val pullRequests: PullRequestService?,
    val initialPrompt: String,
    val autoStart: Boolean,
)

class LocalCopy(val record: LocalRepoRecord, val open: () -> Unit, val sync: () -> Unit)

object LocalReposFeature {
    private val storeState = MutableStateFlow<LocalRepoStore?>(null)

    val store: StateFlow<LocalRepoStore?> get() = storeState

    var confirmPresence: (suspend (messenger: FmMessenger, reason: String) -> Boolean)? = null

    var agentLauncher: LocalRepoAgentLauncher? = null

    fun install(app: App) {
        val scope = Services.get<ProjectScope>()
        useProject(app, projectId = scope?.takeUnless { it.isLegacy }?.projectId)
        val store = storeState.value
        pullRequests()?.let { Services.put(it) }
        if (store != null) {
            ProjectLifecycle.onClose {
                runCatching { store.cancelActive() }
                if (storeState.value === store) storeState.value = null
            }
        }
    }

    fun nodeSyncRepositories(): NodeSyncRepositories? = storeState.value?.let(::LocalRepoNodeSync)

    suspend fun cloneForAgent(messenger: FmMessenger, status: FmStatus?, owner: String, name: String, description: String): LocalRepoRecord? {
        val store = storeState.value ?: return null
        if (Services.get<PublishService>() == null) return null
        return showCloneToDevice(
            store = store,
            messenger = messenger,
            requireAccount = { true },
            owner = owner,
            name = name,
            description = description,
            status = status,
        )
    }

    fun documentsDir(context: Context): File = FlutterFiles.documentsDir(context)

    fun useProject(context: Context, projectId: String?) {
        val store = LocalRepoStore.openInDocuments(documentsDir(context), projectId)
        Services.put(store)
        val api = Services.get<ApiService>()
        val prefs = Services.get<PrefStore>()
        if (api != null && prefs != null) {
            Services.put(PublishService(api, identity = { Services.get<DeviceIdentity>() }, prefs = prefs))
        }
        storeState.value = store
    }

    internal fun session(): AuthSession? = Services.get<AuthService>()?.core?.session

    internal fun pullRequests(): PullRequestService? {
        val publisher = Services.get<PublishService>() ?: return null
        val inbox = Services.get<InboxService>() ?: return null
        return PullRequestService(
            pushBranch = { workspace, owner, repo, branch -> publisher.push(workspace, owner, repo, branch) },
            submitPull = { owner, name, title, base, head, patch -> inbox.submitNewPull(owner, name, title, base, head, patch) },
        )
    }

    @Composable
    fun LocalReposSection(navigator: Navigator) {
        val store by storeState.collectAsState()
        val current = store ?: return
        LocalReposSectionContent(current, navigator)
    }

    fun localRepoScreen(
        record: LocalRepoRecord,
        agentPrompt: String? = null,
        agentAutoStart: Boolean = false,
    ): Screen? {
        val store = storeState.value ?: return null
        return LocalRepoScreen(store, record, agentPrompt, agentAutoStart)
    }

    @Composable
    fun rememberLocalRecords(): (Repository) -> LocalRepoRecord? {
        val store by storeState.collectAsState()
        val current = store
        var version by remember(current) { mutableIntStateOf(0) }
        DisposableEffect(current) {
            val listener: () -> Unit = { version++ }
            current?.addListener(listener)
            onDispose { current?.removeListener(listener) }
        }
        return remember(current, version) { { repo: Repository -> current?.recordFor(repo) } }
    }

    @Composable
    fun rememberLocalCopy(navigator: Navigator, repo: Repository, status: FmStatus? = null): LocalCopy? {
        val store by storeState.collectAsState()
        val current = store ?: return null
        if (Services.get<PublishService>() == null) return null
        val messenger = LocalFmMessenger.current
        val requireAccount = AccountFeature.rememberRequireAccount()
        val scope = rememberCoroutineScope()
        var version by remember(current) { mutableIntStateOf(0) }
        DisposableEffect(current) {
            val listener: () -> Unit = { version++ }
            current.addListener(listener)
            onDispose { current.removeListener(listener) }
        }
        LaunchedEffect(current) {
            withContext(Dispatchers.IO) { runCatching { current.ensureLoaded() } }
            version++
        }
        val record = remember(current, version, repo) { current.recordFor(repo) } ?: return null
        return remember(current, record, messenger, requireAccount, scope, navigator, status) {
            val open = { navigator.push(LocalRepoScreen(current, record)) }
            LocalCopy(record, open = open) {
                scope.launch { syncFromForkMesh(current, messenger, requireAccount, record, onOpen = open, status = status) }
            }
        }
    }

    @Composable
    fun rememberCloneToDevice(navigator: Navigator, status: FmStatus? = null): ((Repository) -> Unit)? {
        val store by storeState.collectAsState()
        val current = store ?: return null
        if (Services.get<PublishService>() == null) return null
        val messenger = LocalFmMessenger.current
        val requireAccount = AccountFeature.rememberRequireAccount()
        val scope = rememberCoroutineScope()
        return remember(current, messenger, requireAccount, scope, navigator, status) {
            { repo: Repository ->
                scope.launch {
                    val record = showCloneToDevice(
                        store = current,
                        messenger = messenger,
                        requireAccount = requireAccount,
                        owner = repo.displayOwner,
                        name = repo.name,
                        description = repo.description,
                        status = status,
                    ) ?: return@launch
                    navigator.push(LocalRepoScreen(current, record))
                }
            }
        }
    }
}
