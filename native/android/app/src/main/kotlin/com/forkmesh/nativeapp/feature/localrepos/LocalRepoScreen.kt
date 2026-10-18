package com.forkmesh.nativeapp.feature.localrepos

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.RadioButton
import androidx.compose.material3.RadioButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import com.forkmesh.nativeapp.localrepos.GitTransferProgress
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.GitWorkspaceException
import com.forkmesh.nativeapp.localrepos.LocalBranch
import com.forkmesh.nativeapp.localrepos.LocalChange
import com.forkmesh.nativeapp.localrepos.LocalChangeKind
import com.forkmesh.nativeapp.localrepos.LocalCommit
import com.forkmesh.nativeapp.localrepos.LocalFileEntry
import com.forkmesh.nativeapp.localrepos.LocalFileKind
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.localrepos.LocalRepoStore
import com.forkmesh.nativeapp.localrepos.LocalRepoStoreException
import com.forkmesh.nativeapp.localrepos.LocalStatus
import com.forkmesh.nativeapp.localrepos.LocalSyncState
import com.forkmesh.nativeapp.localrepos.PublishOutcome
import com.forkmesh.nativeapp.localrepos.PublishService
import com.forkmesh.nativeapp.localrepos.PublishStatus
import com.forkmesh.nativeapp.localrepos.commitButtonLabel
import com.forkmesh.nativeapp.localrepos.commitRowSubtitle
import com.forkmesh.nativeapp.localrepos.committedMessage
import com.forkmesh.nativeapp.localrepos.committingAsText
import com.forkmesh.nativeapp.localrepos.fetchResultMessage
import com.forkmesh.nativeapp.localrepos.localBreadcrumbs
import com.forkmesh.nativeapp.localrepos.localChangeLabel
import com.forkmesh.nativeapp.localrepos.localFileMeta
import com.forkmesh.nativeapp.localrepos.localRepoAuthor
import com.forkmesh.nativeapp.localrepos.meshBannerText
import com.forkmesh.nativeapp.localrepos.publishHeadOf
import com.forkmesh.nativeapp.localrepos.pullResultMessage
import com.forkmesh.nativeapp.localrepos.pushResultMessage
import com.forkmesh.nativeapp.localrepos.switchBranchGuardMessage
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmBottomBar
import com.forkmesh.nativeapp.ui.kit.FmBranchLabel
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmChangeBadge
import com.forkmesh.nativeapp.ui.kit.FmChangeKind
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmGroupHeader
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private enum class LocalTab { Files, Changes, Branches, History }

private data class NewBranchRequest(val name: String, val checkout: Boolean)

private data class PublishRequest(val name: String, val description: String, val isPublic: Boolean)

internal class LocalRepoScreen(
    private val store: LocalRepoStore,
    private val record: LocalRepoRecord,
    private val agentPrompt: String? = null,
    private val agentAutoStart: Boolean = false,
) : Screen() {
    private val workspace: GitWorkspace = store.workspace(record)
    private var tab by mutableStateOf(LocalTab.Files)
    private var directory by mutableStateOf("")
    private var files by mutableStateOf<List<LocalFileEntry>?>(null)
    private var status by mutableStateOf(LocalStatus.empty)
    private var branches by mutableStateOf<List<LocalBranch>>(emptyList())
    private var log by mutableStateOf<List<LocalCommit>>(emptyList())
    private var branch by mutableStateOf(record.branch)
    private var busyLabel by mutableStateOf<String?>(null)
    private var loadError by mutableStateOf<String?>(null)
    private var publishedAs by mutableStateOf(record.publishedAs)
    private var origin by mutableStateOf(record.origin)
    private var sync by mutableStateOf<LocalSyncState?>(null)
    private var commitMessage by mutableStateOf("")
    private val notice = FmStatus()
    private var disposed = false
    private var started = false
    private var pendingRefresh = false

    private lateinit var messenger: FmMessenger
    private lateinit var requireAccount: suspend (String) -> Boolean
    private lateinit var scope: CoroutineScope
    private lateinit var navigator: Navigator

    override val title: String get() = record.name

    private val isPublished: Boolean get() = !publishedAs.isNullOrEmpty()

    private val publisher: PublishService? get() = Services.get()

    private val author get() = localRepoAuthor(LocalReposFeature.session())

    override fun onDisposed() {
        disposed = true
        workspace.dispose()
    }

    private suspend fun <T> io(block: () -> T): T = withContext(Dispatchers.IO) { block() }

    private fun reportError(message: String) {
        notice.error(message)
    }

    private suspend fun guard(label: String, onError: ((String) -> Unit)? = null, body: suspend () -> Unit): Boolean {
        if (disposed) return false
        busyLabel = label
        try {
            body()
            return true
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: GitWorkspaceException) {
            if (!disposed) (onError ?: ::reportError)(error.message)
            return false
        } catch (error: LocalRepoStoreException) {
            if (!disposed) (onError ?: ::reportError)(error.message)
            return false
        } finally {
            if (!disposed) busyLabel = null
        }
    }

    private suspend fun refreshAll() {
        guard("Reading repository", onError = { loadError = it }) {
            val summary = io { workspace.summary() }
            val listed = io { workspace.listFiles(directory) }
            val current = io { workspace.status() }
            val allBranches = io { workspace.branches() }
            val history = io { workspace.log() }
            if (disposed) return@guard
            branch = summary.branch
            files = listed
            status = current
            branches = allBranches
            log = history
            loadError = null
            io { store.updateSummary(record.name, summary) }
        }
        if (isPublished) refreshSync()
    }

    private suspend fun refreshSync() {
        if (branch.isEmpty()) return
        sync = try {
            io { workspace.syncState(branch) }
        } catch (_: GitWorkspaceException) {
            null
        }
    }

    private suspend fun refreshFiles() {
        guard("Listing files") {
            val listed = io { workspace.listFiles(directory) }
            if (!disposed) files = listed
        }
    }

    private suspend fun refreshStatus() {
        guard("Checking status") {
            val current = io { workspace.status() }
            if (!disposed) status = current
        }
    }

    private fun launch(block: suspend () -> Unit) {
        scope.launch { block() }
    }

    private fun openAgent(prompt: String = "", autoStart: Boolean = false) {
        val launcher = LocalReposFeature.agentLauncher ?: return
        launcher.open(
            LocalRepoAgentRequest(
                navigator = navigator,
                record = record,
                workspace = workspace,
                author = author,
                pullRequests = LocalReposFeature.pullRequests(),
                initialPrompt = prompt,
                autoStart = autoStart,
            ),
        )
    }

    private fun openDirectory(path: String) = launch {
        directory = path
        files = null
        refreshFiles()
    }

    private fun openFile(entry: LocalFileEntry) {
        navigator.push(LocalFileEditorScreen(workspace, entry.path, record.name) { changed -> if (changed) pendingRefresh = true })
    }

    private fun newFile() = launch {
        if (!requireAccount("create files")) return@launch
        val path = messenger.showPromptDialog(
            title = "New file",
            hint = "docs/notes.md",
            initialValue = if (directory.isEmpty()) "" else "$directory/",
            confirmLabel = "Create",
        )
        if (path.isNullOrEmpty()) return@launch
        if (!guard("Creating file") { io { workspace.writeFile(path, "") } }) return@launch
        refreshAll()
        openFile(LocalFileEntry(name = path.substringAfterLast('/'), path = path, kind = LocalFileKind.file, size = 0, tracked = false))
    }

    private fun stage(paths: List<String>) = launch {
        if (!requireAccount("stage changes")) return@launch
        if (guard("Staging") { io { workspace.stage(paths) } }) refreshStatus()
    }

    private fun unstage(paths: List<String>) = launch {
        if (!requireAccount("stage changes")) return@launch
        if (guard("Unstaging") { io { workspace.unstage(paths) } }) refreshStatus()
    }

    private fun stageAll() = launch {
        if (!requireAccount("stage changes")) return@launch
        if (guard("Staging all") { io { workspace.stageAll() } }) refreshStatus()
    }

    private fun unstageAll() = launch {
        if (!requireAccount("stage changes")) return@launch
        if (guard("Unstaging all") { io { workspace.unstageAll() } }) refreshStatus()
    }

    private fun discard(path: String) = launch {
        if (!requireAccount("discard changes")) return@launch
        val ok = messenger.showConfirmDialog(
            title = "Discard changes?",
            body = "Changes to $path are dropped and cannot be recovered.",
            confirmLabel = "Discard",
            destructive = true,
        )
        if (!ok) return@launch
        if (guard("Discarding") { io { workspace.discard(listOf(path)) } }) refreshAll()
    }

    private fun commit() = launch {
        if (!requireAccount("commit")) return@launch
        val message = commitMessage.trim()
        val who = author
        var id = ""
        if (!guard("Committing") { id = io { workspace.commit(message, who) } }) return@launch
        commitMessage = ""
        notice.show(committedMessage(id))
        refreshAll()
    }

    private fun checkout(target: LocalBranch) = launch {
        if (target.isCurrent) return@launch
        if (!status.isClean) {
            notice.error(switchBranchGuardMessage)
            tab = LocalTab.Changes
            return@launch
        }
        if (guard("Switching to ${target.name}") { io { workspace.checkout(target.name) } }) refreshAll()
    }

    private fun newBranch() = launch {
        if (!requireAccount("create branches")) return@launch
        val clean = status.isClean
        val request = messenger.showDialog<NewBranchRequest> { done -> NewBranchDialog(clean, branch, done) } ?: return@launch
        if (request.name.isEmpty()) return@launch
        if (guard("Creating branch") { io { workspace.createBranch(request.name, checkout = request.checkout) } }) refreshAll()
    }

    private fun requirePublishCapability(): Boolean {
        val refusal = PublishService.describeSession(LocalReposFeature.session()) ?: return true
        launch {
            messenger.showConfirmDialog(title = "This device is not a node yet", body = refusal, confirmLabel = "OK", hideCancel = true)
        }
        return false
    }

    private fun publish(): Unit = launch {
        if (!requireAccount("publish to ForkMesh")) return@launch
        val service = publisher ?: return@launch
        if (!requirePublishCapability()) return@launch
        if (log.isEmpty()) {
            notice.error("Make a commit before publishing.")
            return@launch
        }
        val request = messenger.showDialog<PublishRequest> { done ->
            PublishRepositoryDialog(record.name, record.description, done)
        } ?: return@launch
        val session = LocalReposFeature.session() ?: return@launch
        val head = publishHeadOf(log, branches.size)
        var outcome: PublishOutcome? = null
        runNetwork("Publishing ${request.name}") { report ->
            val result = io {
                service.publish(
                    session = session,
                    workspace = workspace,
                    repo = request.name,
                    branch = branch,
                    isPublic = request.isPublic,
                    description = request.description,
                    head = head,
                    forkedFrom = if (record.isCloned) record.publishedAs ?: "" else "",
                    onProgress = report,
                )
            }
            if (result.isSuccess) {
                io { store.markPublished(record.name, result.publishedAs) }
                if (!disposed) publishedAs = result.publishedAs
            }
            outcome = result
        }
        val result = outcome
        if (result != null) {
            if (result.status == PublishStatus.mirrored) offerRename(result) else notice.report(result.message, !result.isSuccess)
        }
        refreshSync()
    }

    private suspend fun offerRename(outcome: PublishOutcome) {
        val again = messenger.showConfirmDialog(title = "That name is taken", body = outcome.message, confirmLabel = "Choose another name")
        if (again && !disposed) publish()
    }

    private fun push() = launch {
        if (!requireAccount("push")) return@launch
        val service = publisher ?: return@launch
        val target = publishedAs ?: ""
        if (!target.contains('/')) return@launch
        if (!requirePublishCapability()) return@launch
        val owner = target.substringBefore('/')
        val repo = target.substringAfterLast('/')
        val current = branch
        runNetwork("Pushing $current") { report ->
            val result = io { service.push(workspace, owner, repo, current, onProgress = report) }
            notice.report(pushResultMessage(result, current, target), result.refs.any { it.isRejected })
        }
        refreshSync()
    }

    private fun fetch() = launch {
        val service = publisher ?: return@launch
        val target = publishedAs ?: ""
        if (!target.contains('/')) return@launch
        val owner = target.substringBefore('/')
        val repo = target.substringAfterLast('/')
        runNetwork("Fetching $target") { report ->
            val result = io { service.fetch(workspace, owner, repo, onProgress = report) }
            notice.show(fetchResultMessage(result))
        }
        refreshSync()
    }

    private fun pull() = launch {
        if (!requireAccount("sync")) return@launch
        val service = publisher ?: return@launch
        val target = publishedAs ?: ""
        if (!target.contains('/')) return@launch
        val owner = target.substringBefore('/')
        val repo = target.substringAfterLast('/')
        val current = branch
        runNetwork("Syncing $current") { report ->
            val result = io { service.pull(workspace, owner, repo, current, onProgress = report) }
            notice.report(pullResultMessage(result), result.needsAttention)
        }
        refreshAll()
    }

    private suspend fun runNetwork(label: String, body: suspend (report: (GitTransferProgress) -> Unit) -> Unit) {
        if (disposed) return
        busyLabel = label
        try {
            body { progress -> if (!disposed) busyLabel = progress.label }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: GitWorkspaceException) {
            if (!disposed) reportError(error.message)
        } catch (error: LocalRepoStoreException) {
            if (!disposed) reportError(error.message)
        } finally {
            if (!disposed) busyLabel = null
        }
    }

    private fun deleteRepository() = launch {
        if (!requireAccount("delete repositories")) return@launch
        val reason = "Delete \"${record.name}\" from this device. Commits that were never published are lost."
        val confirm = LocalReposFeature.confirmPresence
        val confirmed = if (confirm != null) {
            confirm(messenger, reason)
        } else {
            messenger.showConfirmDialog(title = "Delete repository?", body = reason, confirmLabel = "Delete", destructive = true)
        }
        if (!confirmed) return@launch
        if (guard("Deleting repository") { io { store.delete(record.name) } }) navigator.pop()
    }

    @Composable
    override fun Content(navigator: Navigator) {
        ComposerFeature.FollowRepository(record.name)
        this.navigator = navigator
        messenger = LocalFmMessenger.current
        requireAccount = AccountFeature.rememberRequireAccount()
        scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            if (!started) {
                started = true
                refreshAll()
                val prompt = agentPrompt ?: ""
                if (prompt.isNotBlank()) openAgent(prompt, agentAutoStart)
            } else if (pendingRefresh) {
                pendingRefresh = false
                refreshAll()
            }
        }
        val c = Fm.colors
        Box(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            Column(Modifier.fillMaxSize()) {
                AppBar()
                FmSegmentTabBar(
                    tabs = listOf(
                        FmSegmentTab("Files"),
                        FmSegmentTab("Changes", status.changeCount),
                        FmSegmentTab("Branches", branches.size),
                        FmSegmentTab("History", log.size),
                    ),
                    index = tab.ordinal,
                    onChanged = { tab = LocalTab.entries[it] },
                )
                if (isPublished) MeshBanner()
                Box(
                    Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                ) {
                    val error = loadError
                    if (error != null) {
                        ErrorView(error)
                    } else {
                        when (tab) {
                            LocalTab.Files -> FilesTab()
                            LocalTab.Changes -> ChangesTab()
                            LocalTab.Branches -> BranchesTab()
                            LocalTab.History -> HistoryTab()
                        }
                    }
                }
                FmInlineStatus(notice, Modifier.background(c.bgRaised).padding(horizontal = FmSpace.x4))
                Box(Modifier.windowInsetsPadding(WindowInsets.navigationBars)) {
                    FmBottomBar(
                        leading = {
                            FmBranchLabel(branch.ifEmpty { "HEAD" }, onTap = { tab = LocalTab.Branches }, trailingIcon = "unfold_more")
                        },
                        actions = { MeshActions() },
                    )
                }
            }
            val label = busyLabel
            if (label != null) LocalGitBusyOverlay(label, onCancel = workspace::cancel)
        }
    }

    @Composable
    private fun AppBar() {
        val c = Fm.colors
        var menu by remember { mutableStateOf(false) }
        Row(
            Modifier
                .fillMaxWidth()
                .background(c.bgRaised)
                .windowInsetsPadding(fmSafeAreaInsets(bottom = false))
                .height(56.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIconButton("arrow_back", "Back", onPressed = { navigator.pop() }, color = c.textPrimary, iconSize = 24.dp)
            Spacer(Modifier.width(FmSpace.x2))
            Text(record.name, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.titleLarge)
            val idle = busyLabel == null
            if (LocalReposFeature.agentLauncher != null) {
                FmIconButton("auto_awesome_outlined", "Coding agent", onPressed = if (idle) ({ openAgent() }) else null, color = c.textPrimary, iconSize = 24.dp)
            }
            FmIconButton("refresh", "Refresh repository", onPressed = if (idle) ({ launch { refreshAll() } }) else null, color = c.textPrimary, iconSize = 24.dp)
            Box {
                FmIconButton("more_vert", "More", onPressed = { menu = true }, color = c.textPrimary, iconSize = 24.dp)
                FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    if (isPublished) {
                        FmDropdownMenuItem(
                            text = "Fetch from ForkMesh",
                            icon = "download_outlined",
                            enabled = idle,
                            onClick = {
                                menu = false
                                fetch()
                            },
                        )
                    }
                    for (plugin in com.forkmesh.nativeapp.feature.codeinsight.CodeInsightFeature.installedPlugins()) {
                        FmDropdownMenuItem(
                            text = plugin.title,
                            icon = plugin.icon,
                            onClick = {
                                menu = false
                                navigator.push(com.forkmesh.nativeapp.feature.codeinsight.CodeInsightFeature.localScreen(plugin, record.name))
                            },
                        )
                    }
                    FmDropdownMenuItem(
                        text = "Delete repository",
                        icon = "delete_outline",
                        textColor = c.danger,
                        onClick = {
                            menu = false
                            deleteRepository()
                        },
                    )
                }
            }
        }
    }

    @Composable
    private fun MeshBanner() {
        val c = Fm.colors
        Row(
            Modifier
                .fillMaxWidth()
                .background(c.bgRaised)
                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon("public", size = 14.dp, tint = c.textTertiary)
            Spacer(Modifier.width(FmSpace.x2))
            Text(
                meshBannerText(publishedAs, origin),
                modifier = Modifier.weight(1f),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textSecondary),
            )
            val current = sync
            if (current != null) {
                Spacer(Modifier.width(FmSpace.x2))
                Text(
                    current.label,
                    maxLines = 1,
                    style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textTertiary),
                )
            }
        }
    }

    @Composable
    private fun RowScope.MeshActions() {
        val c = Fm.colors
        val idle = busyLabel == null
        if (!isPublished) {
            FmStatusBadge("Local only", c.textSecondary)
            Spacer(Modifier.width(FmSpace.x2))
            FmToneButton(label = "Publish to ForkMesh", onPressed = if (idle) ::publish else null, icon = "cloud_upload_outlined", tone = FmTone.Accent, filled = true)
            return
        }
        FmToneButton(label = "Sync", onPressed = if (idle) ({ pull() }) else null, icon = "sync")
        Spacer(Modifier.width(FmSpace.x2))
        FmToneButton(label = "Push", onPressed = if (idle) ({ push() }) else null, icon = "upload_outlined", tone = FmTone.Accent, filled = true)
    }

    @Composable
    private fun ErrorView(message: String) {
        Box(
            Modifier
                .fillMaxSize()
                .padding(FmSpace.x4),
            contentAlignment = Alignment.Center,
        ) {
            FmCard(radius = FmRadius.lg) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    FmEmptyState("error_outline", "Could not open repository", message = message)
                    Spacer(Modifier.height(FmSpace.x3))
                    FmToneButton(label = "Retry", onPressed = { launch { refreshAll() } })
                }
            }
        }
    }

    @Composable
    private fun FilesTab() {
        val c = Fm.colors
        val listed = files
        Column(Modifier.fillMaxSize()) {
            Breadcrumb()
            Box(Modifier.weight(1f)) {
                when {
                    listed == null -> FmSkeletonList(rows = 6, label = "Loading files")
                    listed.isEmpty() -> FmEmptyState("folder_open_outlined", "Empty folder", message = "Create a file to start editing.")
                    else -> LazyColumn(
                        Modifier
                            .fillMaxSize()
                            .background(c.listSurface),
                        contentPadding = PaddingValues(bottom = FmSpace.x5),
                    ) {
                        items(listed.size) { index ->
                            if (index > 0) FmRowDivider()
                            FileRow(listed[index])
                        }
                    }
                }
            }
        }
    }

    @Composable
    private fun Breadcrumb() {
        val c = Fm.colors
        val crumbs = localBreadcrumbs(directory)
        Row(
            Modifier
                .fillMaxWidth()
                .height(48.dp)
                .background(c.bgRaised)
                .drawBehind { drawRect(c.border, topLeft = androidx.compose.ui.geometry.Offset(0f, size.height - 1.dp.toPx()), size = Size(size.width, 1.dp.toPx())) }
                .padding(horizontal = FmSpace.x3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Row(
                Modifier
                    .weight(1f)
                    .horizontalScroll(rememberScrollState(), reverseScrolling = true),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                crumbs.forEachIndexed { index, crumb ->
                    if (index > 0) FmIcon("chevron_right", size = 16.dp, tint = c.textDisabled)
                    Text(
                        crumb.label,
                        modifier = Modifier
                            .clickable { openDirectory(crumb.target) }
                            .padding(horizontal = FmSpace.x1, vertical = FmSpace.x2),
                        style = Fm.text.bodySmall.copy(
                            color = if (crumb.current) c.textPrimary else c.textSecondary,
                            fontWeight = if (crumb.current) FmType.emphasis else FmType.regular,
                        ),
                    )
                }
            }
            Spacer(Modifier.width(FmSpace.x2))
            FmToneButton(label = "New file", onPressed = if (busyLabel == null) ({ newFile() }) else null, icon = "note_add_outlined", tone = FmTone.Accent)
        }
    }

    @Composable
    private fun FileRow(entry: LocalFileEntry) {
        val c = Fm.colors
        val icon = when (entry.kind) {
            LocalFileKind.directory -> "folder_outlined"
            LocalFileKind.symlink -> "link"
            LocalFileKind.file -> "description_outlined"
        }
        FmListRow(
            title = entry.name,
            subtitle = localFileMeta(entry),
            leading = { FmIconTile(icon, tint = if (entry.isDirectory) c.accent else null) },
            trailing = if (entry.tracked) null else ({ FmStatusBadge("New", c.success) }),
            showChevron = entry.isDirectory,
            onTap = if (entry.kind == LocalFileKind.symlink) null else ({ if (entry.isDirectory) openDirectory(entry.path) else openFile(entry) }),
        )
    }

    @Composable
    private fun ChangesTab() {
        val c = Fm.colors
        val current = status
        val who = author
        val canCommit = current.staged.isNotEmpty() && commitMessage.trim().isNotEmpty() && busyLabel == null
        Column(Modifier.fillMaxSize()) {
            Box(Modifier.weight(1f)) {
                if (current.isClean) {
                    FmEmptyState("check_circle_outline", "Working tree clean", message = "Edit files to see changes here.")
                } else {
                    LazyColumn(
                        Modifier
                            .fillMaxSize()
                            .background(c.listSurface),
                        contentPadding = PaddingValues(bottom = FmSpace.x4),
                    ) {
                        if (current.staged.isNotEmpty()) {
                            item {
                                FmGroupHeader("Staged", count = current.staged.size, color = c.success) {
                                    FmToneButton(label = "Unstage all", onPressed = { unstageAll() }, height = 28.dp)
                                }
                            }
                            items(current.staged.size) { index ->
                                val change = current.staged[index]
                                ChangeRow(change) {
                                    FmToneButton(label = "Unstage", tooltip = "Unstage ${change.path}", onPressed = { unstage(listOf(change.path)) }, height = 28.dp)
                                }
                            }
                        }
                        if (current.unstaged.isNotEmpty()) {
                            item {
                                FmGroupHeader("Changes", count = current.unstaged.size, color = c.warning) {
                                    FmToneButton(label = "Stage all", onPressed = { stageAll() }, height = 28.dp)
                                }
                            }
                            items(current.unstaged.size) { index ->
                                val change = current.unstaged[index]
                                ChangeRow(change, onLongPress = { discard(change.path) }) {
                                    FmToneButton(label = "Stage", tone = FmTone.Success, tooltip = "Stage ${change.path}", onPressed = { stage(listOf(change.path)) }, height = 28.dp)
                                }
                            }
                        }
                        if (current.untracked.isNotEmpty()) {
                            item {
                                FmGroupHeader("Untracked", count = current.untracked.size) {
                                    if (current.unstaged.isEmpty()) FmToneButton(label = "Stage all", onPressed = { stageAll() }, height = 28.dp)
                                }
                            }
                            items(current.untracked.size) { index ->
                                val change = current.untracked[index]
                                ChangeRow(change, onLongPress = { discard(change.path) }) {
                                    FmToneButton(label = "Stage", tone = FmTone.Success, tooltip = "Stage ${change.path}", onPressed = { stage(listOf(change.path)) }, height = 28.dp)
                                }
                            }
                        }
                    }
                }
            }
            Column(
                Modifier
                    .fillMaxWidth()
                    .background(c.bgRaised)
                    .drawBehind { drawRect(c.border, size = Size(size.width, 1.dp.toPx())) }
                    .imePadding()
                    .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
            ) {
                FmTextField(
                    value = commitMessage,
                    onValueChange = { commitMessage = it },
                    modifier = Modifier.fillMaxWidth(),
                    hint = "Commit message",
                    maxLines = 4,
                )
                Spacer(Modifier.height(FmSpace.x2))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        committingAsText(who),
                        modifier = Modifier.weight(1f),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textTertiary),
                    )
                    Spacer(Modifier.width(FmSpace.x2))
                    FmToneButton(
                        label = commitButtonLabel(current.staged.size),
                        onPressed = if (canCommit) ({ commit() }) else null,
                        icon = "check",
                        tone = FmTone.Accent,
                        filled = true,
                    )
                }
            }
        }
    }

    @Composable
    private fun ChangeRow(change: LocalChange, onLongPress: (() -> Unit)? = null, action: @Composable () -> Unit) {
        val kind = when (change.kind) {
            LocalChangeKind.added, LocalChangeKind.untracked -> FmChangeKind.Added
            LocalChangeKind.modified -> FmChangeKind.Modified
            LocalChangeKind.deleted -> FmChangeKind.Deleted
        }
        FmListRow(
            title = change.path,
            subtitle = localChangeLabel(change),
            leading = { FmChangeBadge(kind, size = 28.dp) },
            trailing = action,
            showChevron = false,
            minHeight = 52.dp,
            onLongPress = onLongPress,
        )
    }

    @Composable
    private fun BranchesTab() {
        val c = Fm.colors
        Column(Modifier.fillMaxSize()) {
            Row(
                Modifier.padding(start = FmSpace.x4, top = FmSpace.x3, end = FmSpace.x4, bottom = FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmSectionHeader("Branches", Modifier.weight(1f), icon = "account_tree_outlined", count = branches.size)
                Spacer(Modifier.width(FmSpace.x3))
                FmToneButton(label = "New branch", onPressed = if (busyLabel == null) ({ newBranch() }) else null, icon = "add", tone = FmTone.Accent)
            }
            if (branches.isEmpty()) {
                FmEmptyState("account_tree_outlined", "No branches yet", message = "Make a first commit to create branches.")
                return@Column
            }
            LazyColumn(
                Modifier
                    .fillMaxSize()
                    .background(c.listSurface),
            ) {
                items(branches.size) { index ->
                    if (index > 0) FmRowDivider()
                    val item = branches[index]
                    FmListRow(
                        title = item.name,
                        titleContent = { FmBranchLabel(item.name, color = if (item.isCurrent) c.success else c.textPrimary) },
                        subtitle = item.tipId?.take(7) ?: "No commits yet",
                        leading = {
                            FmIconTile(if (item.isCurrent) "check_circle" else "radio_button_unchecked", tint = if (item.isCurrent) c.success else null)
                        },
                        trailing = if (item.isCurrent) ({ FmStatusBadge("Current", c.success) }) else null,
                        showChevron = !item.isCurrent,
                        onTap = { checkout(item) },
                    )
                }
            }
        }
    }

    @Composable
    private fun HistoryTab() {
        val c = Fm.colors
        if (log.isEmpty()) {
            FmEmptyState("history", "No commits yet", message = "Stage changes and commit to build history.")
            return
        }
        LazyColumn(
            Modifier
                .fillMaxSize()
                .background(c.listSurface),
            contentPadding = PaddingValues(bottom = FmSpace.x5),
        ) {
            items(log.size) { index ->
                if (index > 0) FmRowDivider()
                val entry = log[index]
                FmListRow(
                    title = entry.subject.ifEmpty { "(no message)" },
                    subtitle = commitRowSubtitle(entry),
                    leading = { FmIconTile("commit", tint = if (index == 0) c.accent else null) },
                    showChevron = false,
                    onTap = { launch { showCommit(entry) } },
                )
            }
        }
    }

    private suspend fun showCommit(entry: LocalCommit) {
        val time = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss.SSS").format(Instant.ofEpochMilli(entry.time).atZone(ZoneId.systemDefault()))
        messenger.showDialog<Unit> { done ->
            FmAlertDialog(
                onDismissRequest = { done(null) },
                title = entry.subject,
                titleMaxLines = 2,
                actions = {
                    androidx.compose.material3.TextButton(onClick = { done(null) }) { Text("Close") }
                },
            ) {
                SelectionContainer {
                    Text(
                        "${entry.id}\n${entry.authorName} <${entry.authorEmail}>\n$time\n\n${entry.message}",
                        style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                    )
                }
            }
        }
    }
}

@Composable
private fun NewBranchDialog(clean: Boolean, from: String, done: (NewBranchRequest?) -> Unit) {
    var name by remember { mutableStateOf("") }
    var switchTo by remember { mutableStateOf(clean) }
    val c = Fm.colors
    FmAlertDialog(
        onDismissRequest = { done(null) },
        title = "New branch",
        titleMaxLines = 2,
        actions = {
            FmDialogActions(onCancel = { done(null) }, onConfirm = { done(NewBranchRequest(name.trim(), switchTo)) }, confirmLabel = "Create")
        },
    ) {
        FmTextField(name, { name = it }, hint = "feature/my-change", helperText = "Created from $from", autofocus = true)
        Spacer(Modifier.height(FmSpace.x2))
        Row(
            Modifier
                .fillMaxWidth()
                .clickable(enabled = clean) { switchTo = !switchTo },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmCheckbox(
                checked = switchTo,
                onCheckedChange = if (clean) ({ switchTo = it }) else null,
                enabled = clean,
                colors = CheckboxDefaults.colors(checkedColor = c.accent),
            )
            Column {
                Text("Switch to it", style = Fm.text.bodyLarge)
                if (!clean) Text("Commit your changes first to switch.", style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
        }
    }
}

@Composable
private fun PublishRepositoryDialog(initialName: String, initialDescription: String, done: (PublishRequest?) -> Unit) {
    var name by remember { mutableStateOf(initialName) }
    var description by remember { mutableStateOf(initialDescription) }
    var problem by remember { mutableStateOf<String?>(null) }
    val c = Fm.colors
    FmAlertDialog(
        onDismissRequest = { done(null) },
        title = "Publish to ForkMesh",
        actions = {
            FmDialogActions(
                onCancel = { done(null) },
                onConfirm = {
                    val trimmed = name.trim()
                    val error = PublishService.validatePublishName(trimmed)
                    if (error != null) problem = error else done(PublishRequest(trimmed, description.trim(), isPublic = true))
                },
                confirmLabel = "Publish",
            )
        },
    ) {
        FmField("Name on ForkMesh") {
            FmTextField(
                value = name,
                onValueChange = {
                    name = it
                    problem = null
                },
                helperText = "Letters, digits, dots, dashes, underscores.",
                errorText = problem,
                autofocus = true,
            )
        }
        Spacer(Modifier.height(FmSpace.x3))
        FmField("Description (optional)") { FmTextField(description, { description = it }, maxLines = 3) }
        Spacer(Modifier.height(FmSpace.x3))
        VisibilityOption(selected = true, enabled = true, title = "Public", subtitle = "Anyone on ForkMesh can clone it.")
        VisibilityOption(
            selected = false,
            enabled = false,
            title = "Private",
            subtitle = "Needs owner-sealed encryption, which this app cannot do yet — publish privately from the desktop node.",
        )
        Spacer(Modifier.height(FmSpace.x2))
        Text(
            "Pushes this branch to ForkMesh and publishes a signed catalog record from this device.",
            style = Fm.text.bodySmall.copy(color = c.textTertiary),
        )
    }
}

@Composable
private fun VisibilityOption(selected: Boolean, enabled: Boolean, title: String, subtitle: String) {
    val c = Fm.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        RadioButton(selected = selected, onClick = null, enabled = enabled, colors = RadioButtonDefaults.colors(selectedColor = c.accent))
        Spacer(Modifier.width(FmSpace.x2))
        Column(Modifier.padding(vertical = FmSpace.x1)) {
            Text(title, style = Fm.text.bodyLarge.copy(color = if (enabled) c.textPrimary else c.textDisabled))
            Text(subtitle, style = Fm.text.bodySmall.copy(color = if (enabled) c.textSecondary else c.textDisabled))
        }
    }
}
