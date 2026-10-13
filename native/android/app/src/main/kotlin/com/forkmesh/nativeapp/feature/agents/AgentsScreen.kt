package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentDeleteTarget
import com.forkmesh.nativeapp.agents.AgentEntry
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentLineChips
import com.forkmesh.nativeapp.agents.AgentListView
import com.forkmesh.nativeapp.agents.AgentRepoGroup
import com.forkmesh.nativeapp.agents.AgentSwipeDelete
import com.forkmesh.nativeapp.agents.AgentsList
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.control.AgentStopAll
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.control.desktopAgents
import com.forkmesh.nativeapp.control.stopAll
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.feature.composer.AgentQuickStart
import com.forkmesh.nativeapp.feature.composer.showAgentQuickStartSheet
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmNeedsDesktopNote
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object AgentWatchEntry {
    @Volatile
    var open: ((navigator: Navigator, title: String, targets: List<Pair<Repository, com.forkmesh.nativeapp.model.AgentSession>>) -> Unit)? = null

    @Volatile
    var openDevice: ((navigator: Navigator, repoName: String) -> Unit)? = null

    @Volatile
    var openAll: ((navigator: Navigator, groups: List<AgentRepoGroup>) -> Unit)? = null

    @Volatile
    var openControl: ((navigator: Navigator, title: String, rows: List<com.forkmesh.nativeapp.control.UnifiedAgentRow>) -> Unit)? = null
}

internal class AgentsScreen : Screen() {
    private var desktopSort by mutableStateOf(com.forkmesh.nativeapp.agents.ops.AgentListSort.RecentActivity)
    private var groups by mutableStateOf<List<AgentRepoGroup>>(emptyList())
    private var loading by mutableStateOf(false)
    private var loaded by mutableStateOf(false)
    private var error by mutableStateOf<String?>(null)
    private var generation by mutableIntStateOf(0)
    private var tabIndex by mutableIntStateOf(0)
    private val status = FmStatus()

    override val title: String get() = "Agents"

    private val shown: List<AgentRepoGroup>
        get() = AgentsList.covered(groups, AgentsFeature.control.rows, AgentsFeature.control.desktopCover)

    fun reload() {
        generation++
    }

    private fun forget(target: AgentDeleteTarget) {
        groups = AgentSwipeDelete.forget(groups, target)
        reload()
    }

    private suspend fun load(before: List<AgentRepoGroup>): List<AgentRepoGroup> = withContext(Dispatchers.IO) {
        val api = Services.require<ApiService>()
        val queue = AgentsFeature.queue
        val repos = api.repositories()
        coroutineScope {
            repos.map { repo ->
                async {
                    val previous = AgentsList.groupFor(before, repo)
                    val sealed = async {
                        runCatching { api.agentSessionPage(repo.owner, repo.name, AgentsFeature.ownerAccount(repo)) }.getOrNull()
                    }
                    val queued = async { runCatching { queue.launchOptions(repo.owner, repo.name) }.getOrNull() }
                    val options = queued.await()
                    val pulls: List<PublishedPull>? = when {
                        options == null -> null
                        AgentsList.needsPulls(options.sessions + (previous?.launched ?: emptyList())) ->
                            runCatching { api.publishedPulls(repo.owner, repo.name) }.getOrNull()
                        else -> emptyList()
                    }
                    AgentsList.regroup(repo, previous, sealed.await(), options, pulls)
                }
            }.awaitAll()
        }.let(AgentsList::visible)
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val signal by AgentsFeature.listSignal.collectAsState()
        val center = AgentsFeature.control
        rememberCenterTick(center)
        LaunchedEffect(generation, signal) {
            if (signal > 0 && loaded) delay(600)
            center.refreshAsync()
            loading = true
            try {
                groups = load(groups)
                error = null
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = failure.message ?: failure.toString()
            } finally {
                loading = false
                loaded = true
            }
        }
        val hasDesktopNode = AgentsFeature.hasDesktopNode()
        val context = LocalContext.current
        val prefs = remember { Prefs(context.applicationContext) }
        AgentReadMarks.Load()
        var listView by remember { mutableStateOf(AgentListView.fromKey(prefs.getString(AgentListView.PREF_KEY))) }
        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            Header(navigator, hasDesktopNode, listView) {
                listView = listView.next
                prefs.putString(AgentListView.PREF_KEY, listView.key)
            }
            RunnerAttentionBanner(navigator)
            RunnerAccountsEntry(navigator)
            val tabs = listOf(
                FmSegmentTab("Agents", AgentsList.sessionCount(shown)),
                FmSegmentTab("Cloud", AgentsList.cloudRunning(shown)),
                FmSegmentTab("Desktop", AgentsList.desktopRunning(shown) + liveDesktopRows().count { it.isActive }),
                FmSegmentTab("New"),
            )
            FmSegmentTabBar(tabs, tabIndex, onChanged = { tabIndex = it })
            FmPullToRefreshBox(isRefreshing = loading && loaded, onRefresh = { reload() }, modifier = Modifier.weight(1f)) {
                CompositionLocalProvider(LocalAgentListView provides listView) {
                    when (tabIndex) {
                        0 -> ListTab(navigator, hasDesktopNode)
                        1 -> CloudTab(navigator, hasDesktopNode)
                        2 -> DesktopTab(navigator, hasDesktopNode)
                        else -> Column(Modifier.fillMaxSize().padding(FmSpace.x4)) {
                            AgentQuickStart(navigator, onStarted = { reload() })
                        }
                    }
                }
            }
        }
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    private fun Header(navigator: Navigator, hasDesktopNode: Boolean, listView: AgentListView, onNextView: () -> Unit) {
        val c = Fm.colors
        val headerScope = rememberCoroutineScope()
        val headerMessenger = LocalFmMessenger.current
        var confirmStopAll by remember { mutableStateOf(false) }
        var stoppingAll by remember { mutableStateOf(false) }
        var stopCount by remember { mutableIntStateOf(0) }
        val stopTargets = AgentStopAll.targets(AgentsFeature.control.rows)
        Column(Modifier.fillMaxWidth().padding(FmSpace.x4)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Agents", style = Fm.text.displaySmall.copy(color = c.textPrimary))
                    Spacer(Modifier.height(FmSpace.x1))
                    Text(
                        if (hasDesktopNode) "Send a prompt to a mirror and follow it to its branch, pull request and merge."
                        else "Send a prompt to a mirror; with no node to take it, it runs on this phone.",
                        style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                    )
                }
                if (stoppingAll || stopTargets.isNotEmpty()) {
                    FmIconButton("stop_circle_outlined", "Stop all agents", onPressed = if (stoppingAll) null else ({
                        stopCount = stopTargets.size
                        confirmStopAll = true
                    }), color = c.danger)
                }
                FmIconButton("add", "Start an agent", onPressed = { headerScope.launch { showAgentQuickStartSheet(headerMessenger, navigator, onStarted = { reload() }) } }, color = c.accent)
                val watch = AgentWatchEntry.openAll
                if (watch != null && (shown.any { it.sessions.isNotEmpty() } || AgentsFeature.control.rows.isNotEmpty())) {
                    FmIconButton("graphic_eq", "Watch, hear and feel agents", onPressed = { watch(navigator, shown) }, color = c.accent)
                }
                FmIconButton(
                    when (listView) {
                        AgentListView.Compact -> "view_list_outlined"
                        AgentListView.Zen -> "dark_mode_outlined"
                        AgentListView.Full -> "visibility_outlined"
                    },
                    "Agent list view: ${listView.caption}",
                    onPressed = onNextView,
                    color = c.textSecondary,
                )
                if (AgentsFeature.control.hub.desktop != null) {
                    FmIconButton("terminal", "Terminal", onPressed = { navigator.push(DesktopTerminalScreen()) }, color = c.textSecondary)
                    FmIconButton("delete_outline", "Deleted agents on the desktop", onPressed = { navigator.push(AgentOpsFeature.recycleBinScreen()) }, color = c.textSecondary)
                }
                FmIconButton("devices_outlined", "All agents", onPressed = { navigator.push(AllAgentsScreen()) }, color = c.textSecondary)
                FmIconButton("refresh", "Refresh agents", onPressed = { reload() }, color = c.textSecondary)
            }
            Spacer(Modifier.height(FmSpace.x3))
            val running = AgentsList.running(shown)
            val cloud = AgentsList.cloudRunning(shown)
            val sessions = AgentsList.sessionCount(shown)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                val style = Fm.text.bodySmall.copy(color = c.textSecondary)
                Text("${running + liveDesktopRows().count { it.isActive }} running", style = style)
                if (cloud > 0) Text("$cloud in the cloud", style = style)
                Text("$sessions session${if (sessions == 1) "" else "s"}", style = style)
                Text("${shown.size} repo${if (shown.size == 1) "" else "s"}", style = style)
                LocalDesktopChip(navigator)
            }
            FmInlineStatus(agentsNotice, Modifier.padding(top = FmSpace.x2))
            FmInlineStatus(status, Modifier.padding(top = FmSpace.x2))
        }
        if (confirmStopAll) {
            FmConfirmDialog(
                title = AgentStopAll.question(stopCount),
                confirmLabel = "Stop all",
                destructive = true,
                busy = stoppingAll,
                onConfirm = {
                    if (!stoppingAll) {
                        stoppingAll = true
                        headerScope.launch {
                            val outcome = withContext(Dispatchers.IO) { AgentsFeature.control.stopAll() }
                            stoppingAll = false
                            confirmStopAll = false
                            status.report(outcome.message, outcome.failed > 0)
                            reload()
                        }
                    }
                },
                onDismissRequest = { if (!stoppingAll) confirmStopAll = false },
            )
        }
    }

    @Composable
    private fun Pending(): Boolean {
        val c = Fm.colors
        if (!loaded && loading) {
            Box(Modifier.fillMaxWidth().height(320.dp)) { FmSkeletonList(rows = 4, label = "Loading agents") }
            return true
        }
        val failure = error
        if (failure != null && shown.isEmpty()) {
            FmCard(radius = FmRadius.lg, modifier = Modifier.padding(FmSpace.x4)) {
                Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
                    Text(failure, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                    Spacer(Modifier.height(FmSpace.x3))
                    FmOutlinedButton("Retry", onClick = { reload() })
                }
            }
            return true
        }
        return false
    }

    @Composable
    private fun Scroll(content: androidx.compose.foundation.lazy.LazyListScope.() -> Unit) {
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x3, bottom = FmSpace.x5),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            content = content,
        )
    }

    private fun liveDesktopRows(): List<UnifiedAgentRow> = desktopAgents(
        AgentsFeature.control.rows,
        shown.flatMap { g -> g.sessions.map { g.repo.name to it.id } },
    )

    @Composable
    private fun ListTab(navigator: Navigator, hasDesktopNode: Boolean) {
        val entries = AgentsList.flatten(shown)
        val sealed = AgentsList.sealed(shown)
        val live = liveDesktopRows()
        val controlRows = AgentsFeature.control.rows
        Scroll {
            liveDesktopItems(navigator, live, status, onDeleted = ::forget)
            item {
                if (Pending()) return@item
                if (entries.isEmpty() && sealed == 0 && live.isEmpty()) {
                    Column {
                        Box(Modifier.fillMaxWidth().height(260.dp)) {
                            FmEmptyState(
                                icon = "smart_toy_outlined",
                                title = "No agent sessions yet",
                                message = "Send a prompt from the New tab and follow it here from the mirror that runs it to " +
                                    "its branch, pull request and merge.",
                            )
                        }
                        if (!hasDesktopNode) {
                            Spacer(Modifier.height(FmSpace.x3))
                            FmNeedsDesktopNote(
                                icon = "smart_toy_outlined",
                                detail = "This account has no desktop or server node enrolled. A prompt still goes to a mirror " +
                                    "of the repository when one can run agents; otherwise it runs on this phone, against a " +
                                    "mirror of the repository, on your own provider key.",
                            )
                        }
                    }
                }
            }
            items(entries, key = { entryKey(it) }) { entry ->
                AgentSwipeToDelete(AgentSwipeDelete.forEntry(entry, controlRows), status, onDeleted = ::forget) {
                    when (entry) {
                        is AgentEntry.Desktop -> DesktopSessionRow(navigator, entry.repo, entry.session)
                        is AgentEntry.Launched -> LaunchedSessionRow(navigator, entry.repo, entry.session, entry.pulls, entry.cloud, status) { reload() }
                    }
                }
            }
            if (sealed > 0 && loaded) item { SealedSessionsNotice(sealed) }
        }
    }

    private fun entryKey(entry: AgentEntry): String = when (entry) {
        is AgentEntry.Desktop -> "d:${entry.repo.fullName}:${entry.session.id}"
        is AgentEntry.Launched -> "l:${entry.repo.fullName}:${entry.session.sessionId}"
    }

    @Composable
    private fun CloudTab(navigator: Navigator, hasDesktopNode: Boolean) {
        val runs = AgentsList.cloudRuns(shown)
        Scroll {
            item {
                if (Pending()) return@item
                if (runs.isEmpty()) {
                    Box(Modifier.fillMaxWidth().height(260.dp)) {
                        FmEmptyState(
                            icon = "cloud_outlined",
                            title = "Nothing running in the cloud",
                            message = if (hasDesktopNode) {
                                "Work sent to a mirror shows here while it runs, and stays after it finishes with its branch and pull request."
                            } else {
                                "Send a prompt from the New tab and the mirror that takes it shows here."
                            },
                        )
                    }
                }
            }
            items(runs, key = { "c:${it.first.fullName}:${it.second.sessionId}" }) { (repo, session) ->
                LaunchedSessionRow(navigator, repo, session, AgentsList.pullsFor(shown, repo), cloud = true, status = status) { reload() }
            }
        }
    }

    @Composable
    private fun DesktopTab(navigator: Navigator, hasDesktopNode: Boolean) {
        val needing = AgentOpsFeature.approvals.sessionIdsNeedingYou
        val running = com.forkmesh.nativeapp.agents.ops.AgentListOrdering.sorted(
            AgentsList.desktopRunningRows(shown),
            desktopSort,
            System.currentTimeMillis(),
        ) { com.forkmesh.nativeapp.agents.ops.AgentListItem(it.second, needsYou = it.second.id in needing) }
        val sealed = AgentsList.sealed(shown)
        val live = liveDesktopRows()
        Scroll {
            item {
                DesktopAgentOps(
                    navigator,
                    onFinished = { reload() },
                    candidates = running.map { (_, session) ->
                        com.forkmesh.nativeapp.agents.ops.BulkCandidate(session.id, session.isActive, session.isActive, session.merged)
                    },
                )
            }
            if (running.isNotEmpty()) item { AgentSortMenu(desktopSort) { desktopSort = it } }
            item {
                if (Pending()) return@item
                if (running.isEmpty() && live.isEmpty()) {
                    Box(Modifier.fillMaxWidth().height(260.dp)) {
                        FmEmptyState(
                            icon = "desktop_windows_outlined",
                            title = "Nothing running on your desktop",
                            message = if (hasDesktopNode) "Agents your desktop nodes are running show here with their status."
                            else "This account has no desktop node enrolled.",
                        )
                    }
                }
            }
            items(running, key = { "r:${it.first.fullName}:${it.second.id}" }) { (repo, session) ->
                AgentTwoLineRow(
                    icon = "desktop_windows_outlined",
                    title = session.displayTitle,
                    detail = "",
                    status = { AgentStatusBadge(session) },
                    onTap = {
                        AgentReadMarks.open(repo.name, session)
                        navigator.push(AgentSessionDetailScreen(repo, session, AgentsFeature.ownerAccount(repo)))
                    },
                    facts = AgentLineChips.facts(session),
                    unread = AgentReadMarks.unread(repo.name, session),
                )
            }
            liveDesktopItems(navigator, live, status)
            if (sealed > 0 && loaded) item { SealedSessionsNotice(sealed) }
        }
    }
}

internal fun LazyListScope.liveDesktopItems(
    navigator: Navigator,
    rows: List<UnifiedAgentRow>,
    status: FmStatus,
    onDeleted: ((AgentDeleteTarget) -> Unit)? = null,
) {
    val note = AgentsFeature.control.desktopNote
    if (rows.isEmpty() && note.isNotEmpty()) {
        item(key = "live-desktop-note") {
            Text(
                "Your desktop's agents could not be loaded: $note",
                style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
                modifier = Modifier.padding(top = FmSpace.x1),
            )
        }
    }
    if (rows.isEmpty()) {
        if (AgentsFeature.control.supportsQueue) item(key = "live-desktop-queue") { DesktopQueueSummaryRow(navigator) }
        return
    }
    item(key = "live-desktop-header") {
        val c = Fm.colors
        val desktop = rows.first().node.ifEmpty { "your desktop" }
        Row(Modifier.fillMaxWidth().padding(top = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
            Text(
                "On $desktop",
                style = Fm.text.labelLarge.copy(color = c.textSecondary),
                modifier = Modifier.weight(1f),
            )
            val watch = AgentWatchEntry.openControl
            if (watch != null) {
                val live = rows.filter { it.isActive }
                FmToneButton("Watch", onPressed = {
                    watch(navigator, if (live.isNotEmpty()) "Watch agents" else "Replay agents", live.ifEmpty { rows.take(3) })
                }, icon = "graphic_eq")
            }
        }
    }
    if (AgentsFeature.control.supportsQueue) item(key = "live-desktop-queue") { DesktopQueueSummaryRow(navigator) }
    items(rows, key = { "live:${it.id}" }) { row ->
        val position = AgentsFeature.control.queuePosition(row)
        val target = if (onDeleted == null) null else AgentSwipeDelete.forRow(row)
        AgentSwipeToDelete(target, status, onDeleted = { onDeleted?.invoke(it) }) {
            AgentTwoLineRow(
                icon = if (row.outside) "terminal_outlined" else "desktop_windows_outlined",
                title = row.title,
                detail = if (position < 0) liveDesktopDetail(row)
                else listOf(queueWaitingLabel(position), liveDesktopDetail(row)).filter { it.isNotEmpty() }.joinToString(" · "),
                status = { AgentChip(row.statusLabel) },
                onTap = {
                    AgentReadMarks.open(row)
                    navigator.push(UnifiedAgentScreen(row))
                },
                trailing = if (position > 0) ({ QueueMoveToTopButton(row, status) }) else null,
                facts = AgentLineChips.facts(row),
                unread = AgentReadMarks.unread(row),
            )
        }
    }
}

internal fun liveDesktopDetail(row: UnifiedAgentRow): String {
    val parts = if (row.outside) {
        if (row.outsideUser.isNotEmpty()) {
            listOf(row.kindLabel, "in ${row.outsideUser}'s login")
        } else {
            listOf(row.kindLabel, "started outside ForkMesh", row.repoLabel.ifEmpty { row.cwd })
        }
    } else {
        listOf(row.repoLabel, row.provider)
    }
    return parts.filter { it.isNotEmpty() }.joinToString(" · ")
}

@Composable
internal fun DesktopSessionRow(navigator: Navigator, repo: Repository, session: com.forkmesh.nativeapp.model.AgentSession) {
    AgentTwoLineRow(
        icon = "smart_toy_outlined",
        title = session.displayTitle,
        detail = AgentsList.desktopDetail(repo, session),
        status = { AgentStatusBadge(session) },
        onTap = {
            AgentReadMarks.open(repo.name, session)
            navigator.push(AgentSessionDetailScreen(repo, session, AgentsFeature.ownerAccount(repo)))
        },
        facts = AgentLineChips.facts(session),
        unread = AgentReadMarks.unread(repo.name, session),
    )
}

@Composable
internal fun LaunchedSessionRow(
    navigator: Navigator,
    repo: Repository,
    session: AgentLaunchSession,
    pulls: List<PublishedPull>,
    cloud: Boolean,
    status: FmStatus,
    onChanged: () -> Unit,
) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    var busy by androidx.compose.runtime.remember(session.sessionId) { mutableStateOf(false) }
    AgentTwoLineRow(
        icon = when {
            session.isOnRunner -> "memory_outlined"
            cloud -> "cloud_outlined"
            else -> "bolt_outlined"
        },
        title = session.displayTitle,
        detail = AgentsList.launchedDetail(repo, session, pulls, cloud),
        status = { AgentChip(session.statusLabel) },
        onTap = {
            AgentReadMarks.open(session)
            if (session.sessionId.isNotEmpty()) {
                navigator.push(UnifiedAgentScreen(UnifiedAgentRow.fromCloud(session.copy(owner = repo.owner, repo = repo.name))))
            } else {
                navigator.push(LaunchedAgentSessionScreen(repo, session, onChanged))
            }
        },
        facts = AgentLineChips.facts(session, cloud),
        unread = AgentReadMarks.unread(session),
        trailing = if (!session.isRetryable) null else {
            {
                if (busy) {
                    com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator(Modifier.size(16.dp).padding(2.dp))
                } else {
                    FmIconButton(
                        "refresh",
                        if (session.canResume) "Retry on its branch" else "Retry",
                        onPressed = {
                            busy = true
                            status.clear()
                            scope.launch {
                                try {
                                    val outcome = withContext(Dispatchers.IO) { AgentsFeature.queue.retry(repo.owner, repo.name, session) }
                                    outcome.session?.let { AgentsFeature.sentPhotos.rename(session.sessionId, it.sessionId) }
                                    status.show(outcome.label)
                                    onChanged()
                                } catch (cancelled: CancellationException) {
                                    throw cancelled
                                } catch (failure: Exception) {
                                    status.error(failure.message ?: failure.toString())
                                } finally {
                                    busy = false
                                }
                            }
                        },
                        color = c.textSecondary,
                    )
                }
            }
        },
    )
}
