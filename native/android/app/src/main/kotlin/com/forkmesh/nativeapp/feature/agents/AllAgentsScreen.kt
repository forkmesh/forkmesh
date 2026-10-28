package com.forkmesh.nativeapp.feature.agents

import com.forkmesh.nativeapp.ui.kit.confirmAction
import com.forkmesh.nativeapp.agents.AgentAttachmentRef
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentLiveEvent
import com.forkmesh.nativeapp.agents.AgentLiveStream
import com.forkmesh.nativeapp.agents.withLive
import com.forkmesh.nativeapp.agents.AgentDeleteTarget
import com.forkmesh.nativeapp.agents.AgentSwipeDelete
import com.forkmesh.nativeapp.agents.AgentLineChips
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.agentProviderLabel
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.composer.PromptDraftStore
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
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
import com.forkmesh.nativeapp.code.ConflictResolution
import com.forkmesh.nativeapp.control.AgentControlCenter
import com.forkmesh.nativeapp.control.AgentTranscriptDelta
import com.forkmesh.nativeapp.control.DesktopTranscriptStream
import com.forkmesh.nativeapp.control.RunnerDesktopHandoff
import com.forkmesh.nativeapp.control.TranscriptBlock
import com.forkmesh.nativeapp.control.TranscriptBlockCache
import com.forkmesh.nativeapp.control.TranscriptFollower
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.control.cloudTranscriptDelta
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.Box
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import androidx.compose.ui.unit.dp
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import com.forkmesh.nativeapp.control.PromptDeck

internal val agentsNotice = FmStatus()

@Composable
internal fun rememberCenterTick(center: AgentControlCenter): Int {
    var tick by remember { mutableIntStateOf(0) }
    DisposableEffect(center) {
        val listener: () -> Unit = { tick++ }
        center.addListener(listener)
        center.hub.addListener(listener)
        onDispose {
            center.removeListener(listener)
            center.hub.removeListener(listener)
        }
    }
    return tick
}

internal class AllAgentsScreen : Screen() {
    override val title: String get() = "All agents"

    @Composable
    override fun Content(navigator: Navigator) {
        AgentReadMarks.Load()
        val c = Fm.colors
        val center = AgentsFeature.control
        val tick = rememberCenterTick(center)
        val signal by AgentsFeature.listSignal.collectAsState()
        LaunchedEffect(signal) { center.refreshAsync() }
        DetailScaffold(
            title,
            navigator,
            actions = {
                val watch = AgentWatchEntry.openControl
                if (watch != null && center.rows.isNotEmpty()) {
                    FmIconButton("graphic_eq", "Watch agents", onPressed = {
                        val live = center.rows.filter { it.isActive }
                        watch(navigator, if (live.isNotEmpty()) "Watch agents" else "Replay agents", live.ifEmpty { center.rows.take(3) })
                    }, color = c.textPrimary)
                }
                FmIconButton("leaderboard", "Leaderboard", onPressed = { navigator.push(MergeLeaderboardScreen()) }, color = c.textPrimary)
                FmIconButton("refresh", "Refresh", onPressed = { center.refreshAsync() }, color = c.textPrimary)
            },
        ) {
            tick.let { }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                item {
                    Column {
                        Text(center.hub.reach.label, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        if (center.desktopNote.isNotEmpty()) Text(center.desktopNote, style = Fm.text.bodySmall.copy(color = c.warning))
                        if (center.cloudNote.isNotEmpty()) Text(center.cloudNote, style = Fm.text.bodySmall.copy(color = c.warning))
                        FmInlineStatus(agentsNotice, Modifier.padding(top = FmSpace.x1))
                    }
                }
                if (center.rows.isEmpty() && !center.loading) {
                    item {
                        FmEmptyState(
                            icon = "smart_toy_outlined",
                            title = "No agents",
                            message = "Agents on your desktop (on this network or through the relay) and on your nodes show here.",
                        )
                    }
                }
                items(center.rows, key = { it.id }) { row ->
                    AgentTwoLineRow(
                        icon = when {
                            row.isRunner -> "memory_outlined"
                            row.isDesktop -> "desktop_windows_outlined"
                            else -> "cloud_outlined"
                        },
                        title = row.title,
                        detail = listOf(row.badgeLabel, row.repoLabel, if (row.isRunner) "" else row.node, row.provider, row.branchName)
                            .filter { it.isNotEmpty() }.joinToString(" · "),
                        status = { AgentChip(if (row.merged) "Merged" else row.statusLabel) },
                        onTap = {
                            AgentReadMarks.open(row)
                            navigator.push(UnifiedAgentScreen(row))
                        },
                        facts = AgentLineChips.facts(row),
                        unread = AgentReadMarks.unread(row),
                    )
                }
            }
        }
    }
}

internal class AgentRouteScreen(private val rowId: String) : Screen() {
    private var looked by mutableStateOf(false)
    private var retrying by mutableStateOf(false)

    override val title: String get() = "Agent"

    @Composable
    override fun Content(navigator: Navigator) {
        val center = AgentsFeature.control
        val tick = rememberCenterTick(center)
        val scope = rememberCoroutineScope()
        LaunchedEffect(tick, looked) {
            center.row(rowId)?.let { navigator.replace(UnifiedAgentScreen(it)) }
        }
        LaunchedEffect(Unit) {
            if (center.row(rowId) == null) withContext(Dispatchers.IO) { center.refresh() }
            looked = true
        }
        DetailScaffold(title, navigator) {
            tick.let { }
            if (!looked || retrying || center.loading || center.othersLoading) {
                AgentRouteLoading()
            } else {
                AgentRouteMissing(center.desktopNote, retrying) {
                    if (!retrying) {
                        retrying = true
                        scope.launch {
                            try {
                                withContext(Dispatchers.IO) { center.refresh() }
                            } finally {
                                retrying = false
                            }
                        }
                    }
                }
            }
        }
    }
}

private val routeLoadingLines = listOf(0.92f, 0.78f, 0.86f, 0.6f, 0.9f, 0.7f)

@Composable
private fun AgentRouteLoading() {
    Column(
        Modifier
            .fillMaxSize()
            .padding(horizontal = 12.dp)
            .padding(top = 4.dp)
            .semantics(mergeDescendants = true) { contentDescription = "Opening agent" },
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        com.forkmesh.nativeapp.ui.kit.FmSkeleton(width = 220.dp, height = 18.dp)
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
            com.forkmesh.nativeapp.ui.kit.FmSkeleton(width = 54.dp, height = 16.dp, radius = com.forkmesh.nativeapp.ui.theme.FmRadius.md)
            com.forkmesh.nativeapp.ui.kit.FmSkeleton(width = 120.dp, height = 10.dp)
            Spacer(Modifier.weight(1f))
            com.forkmesh.nativeapp.ui.kit.FmSkeleton(width = 56.dp, height = 10.dp)
        }
        Spacer(Modifier.height(FmSpace.x3))
        for (share in routeLoadingLines) {
            com.forkmesh.nativeapp.ui.kit.FmSkeleton(Modifier.fillMaxWidth(share), height = 11.dp)
        }
    }
}

@Composable
private fun AgentRouteMissing(note: String, retrying: Boolean, onRetry: () -> Unit) {
    Box(Modifier.fillMaxSize()) {
        FmEmptyState(
            icon = "smart_toy_outlined",
            title = "Agent not found",
            message = if (note.isEmpty()) "The desktop or the relay no longer lists this run." else "Your desktop did not answer: $note",
        )
        FmToneButton(
            "Try again",
            onPressed = { if (!retrying) onRetry() },
            icon = "refresh",
            modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = FmSpace.x6),
        )
    }
}

internal class UnifiedAgentScreen(initial: UnifiedAgentRow) : Screen() {
    private var row by mutableStateOf(initial)
    private val follower = TranscriptFollower()
    private val blockCache = TranscriptBlockCache()
    private var text by mutableStateOf("")
    private var blocks by mutableStateOf<List<TranscriptBlock>>(emptyList())
    private var active by mutableStateOf(initial.isActive)
    private var message by mutableStateOf("")
    private var photos by mutableStateOf<List<PromptAttachment>>(emptyList())
    private var attachments by mutableStateOf<List<AgentAttachmentRef>>(emptyList())
    private var busy by mutableStateOf(false)
    private var showMerge by mutableStateOf(false)
    private var showDelete by mutableStateOf(false)
    private var showCleanup by mutableStateOf(false)
    private var showUpdate by mutableStateOf(false)
    private var creatingPull by mutableStateOf(false)
    private var refreshing by mutableStateOf(false)
    private var pulls by mutableIntStateOf(0)
    private var sent by mutableIntStateOf(0)
    @Volatile private var liveStream: DesktopTranscriptStream? = null
    private val status = FmStatus()
    private val sendStatus = FmStatus()

    override val title: String get() = row.title

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val center = AgentsFeature.control
        val tick = rememberCenterTick(center)
        LaunchedEffect(tick) { center.rows.firstOrNull { it.id == row.id }?.let { row = it } }
        LaunchedEffect(row.id, pulls) {
            suspend fun show(delta: AgentTranscriptDelta) {
                follower.apply(delta)
                val snapshot = follower.text
                blocks = withContext(Dispatchers.Default) { blockCache.update(snapshot) }
                text = snapshot
                attachments = follower.attachments
                active = delta.active
            }
            if (!row.isDesktop && row.cloudSessionId.isNotEmpty()) {
                followCloud(center) { show(it) }
                return@LaunchedEffect
            }
            val deltas = Channel<AgentTranscriptDelta>(Channel.UNLIMITED)
            val stream = center.stream(row, follower.next, onEnded = { deltas.close() }) { deltas.trySend(it) }
            try {
                if (stream != null && withContext(Dispatchers.IO) { stream.start() }) {
                    refreshing = false
                    liveStream = stream
                    for (delta in deltas) show(delta)
                    active = false
                    return@LaunchedEffect
                }
                while (true) {
                    try {
                        show(withContext(Dispatchers.IO) { center.transcript(row, follower.next) })
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (error: Exception) {
                        text = text.ifEmpty { controlError(error) }
                    }
                    refreshing = false
                    if (!active) break
                    delay(3000)
                }
            } finally {
                if (liveStream === stream) liveStream = null
                stream?.let { opened -> withContext(NonCancellable + Dispatchers.IO) { opened.stop() } }
            }
        }
        fun act(done: String, target: FmStatus = status, onFailure: () -> Unit = {}, notice: () -> String = { "" }, block: () -> Unit) {
            busy = true
            status.clear()
            sendStatus.clear()
            scope.launch {
                try {
                    withContext(Dispatchers.IO) { block() }
                    target.show(notice().ifEmpty { done })
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    onFailure()
                    target.error(controlError(error))
                } finally {
                    busy = false
                }
            }
        }
        fun moveToDesktop() {
            scope.launch {
                val name = center.hub.currentLink?.desktopName?.takeIf { it.isNotEmpty() }
                    ?: center.hub.desktop?.name?.takeIf { it.isNotEmpty() }
                    ?: "your desktop"
                val runner = row.runner.ifEmpty { row.node.ifEmpty { "the runner" } }
                val body = if (row.isActive || active) {
                    "The run on $runner is stopped and deleted, and the desktop picks it up on the same branch with the conversation so far."
                } else {
                    "The desktop picks it up on the same branch with the conversation so far, and the run on $runner is deleted."
                }
                if (!messenger.confirmAction("Continue on $name?", "Continue on desktop", body, destructive = false)) return@launch
                busy = true
                status.clear()
                try {
                    val move = withContext(Dispatchers.IO) { center.continueOnDesktop(row) }
                    if (move.runnerKept) {
                        status.error(move.message)
                        pulls++
                    } else {
                        agentsNotice.show(move.message)
                        navigator.pop()
                    }
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    status.error(controlError(error))
                } finally {
                    busy = false
                }
            }
        }
        fun openTerminal() {
            scope.launch {
                if (row.isActive || active) {
                    val label = agentProviderLabel(row.provider)
                    val confirmed = messenger.confirmAction(
                        "Open in $label's terminal?",
                        "Open terminal",
                        "The desktop stops this run and resumes its conversation in $label in a terminal you watch and " +
                            "type into here. Send \"continue\" there to pick the work up.",
                        destructive = false,
                    )
                    if (!confirmed) return@launch
                }
                navigator.push(DesktopTerminalScreen(session = row.localId, owner = row.repoOwner, name = row.repoName, provider = row.provider))
            }
        }
        fun requestDelete() {
            val target = AgentSwipeDelete.forRow(row) ?: return
            if (target is AgentDeleteTarget.Run) {
                showDelete = true
                return
            }
            if (center.hub.desktop == null) {
                status.error(AgentSwipeDelete.DESKTOP_OUT_OF_REACH)
                return
            }
            if (target !is AgentDeleteTarget.Outside) {
                showDelete = true
                return
            }
            scope.launch {
                if (!confirmOutsideDelete(messenger)) return@launch
                busy = true
                status.clear()
                try {
                    val outcome = deleteAgentTarget(target)
                    if (outcome.failed) {
                        status.error(outcome.message)
                    } else {
                        agentsNotice.show(outcome.message)
                        scope.launch { withContext(Dispatchers.IO) { runCatching { center.refresh() } } }
                        leave(navigator)
                    }
                } finally {
                    busy = false
                }
            }
        }
        fun createPull() {
            if (creatingPull || busy) return
            creatingPull = true
            busy = true
            status.clear()
            val repo = Repository(row.repoOwner, row.repoName)
            scope.launch {
                try {
                    val opened = withContext(Dispatchers.IO) { AgentsFeature.runnerOps.openPull(row.runnerRef(), RunnerPullDevice) }
                    row = row.copy(prNumber = opened.number, canOpenPull = false)
                    status.show(opened.message, "Open") { openPull(navigator, repo, opened.number) }
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    status.error(error.message ?: error.toString())
                } finally {
                    creatingPull = false
                    busy = false
                    scope.launch { withContext(Dispatchers.IO) { runCatching { center.refresh() } } }
                }
            }
        }
        DetailScaffold(row.title, navigator) {
            Column(Modifier.fillMaxSize()) {
                Row(
                    Modifier.fillMaxWidth().padding(FmSpace.x3),
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FmStatusBadge(row.badgeLabel, c.textSecondary)
                    AgentChip(if (row.merged) "Merged" else row.statusLabel)
                    Box(Modifier.weight(1f), contentAlignment = Alignment.CenterEnd) {
                        Row(
                            Modifier.horizontalScroll(rememberScrollState(Int.MAX_VALUE)),
                            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            val watch = AgentWatchEntry.openControl
                            if (watch != null) {
                                FmIconButton("graphic_eq", if (row.isActive) "Watch & listen" else "Replay & listen", onPressed = {
                                    watch(navigator, if (row.isActive) "Watch agent" else "Replay agent", listOf(row))
                                }, color = c.accent)
                            }
                            if (RunnerDesktopHandoff.canMove(row, center.hub.desktop != null)) {
                                FmIconButton("desktop_windows_outlined", "Continue on desktop", onPressed = { if (!busy) moveToDesktop() }, color = c.accent)
                            }
                            if (row.isPhoneTerminal && !row.elsewhere) {
                                FmOutlinedButton("Terminal", onClick = { navigator.push(DesktopTerminalScreen(row.terminal)) }, icon = "terminal")
                            } else if (row.offersTerminal && !row.elsewhere) {
                                FmOutlinedButton("Terminal", onClick = if (busy) null else ({ openTerminal() }), icon = "terminal")
                            }
                            if (row.canStop) FmOutlinedButton("Stop", onClick = if (busy) null else ({ act(if (row.outside) "Interrupted it." else "Asked it to stop.") { center.stop(row) } }))
                            if (row.canContinue) FmOutlinedButton("Continue", onClick = if (busy) null else ({ act("Continuing.") { center.continueRun(row) } }))
                        }
                    }
                }
                if (row.isDesktop && !row.outside && !row.elsewhere && row.localId > 0) {
                    QueuedRunNote(row, Modifier.padding(horizontal = FmSpace.x3).padding(bottom = FmSpace.x2))
                }
                if (row.outside) {
                    Text(
                        row.outsideSummary,
                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3).padding(bottom = FmSpace.x2),
                    )
                    if (row.localId != 0) {
                        Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3)) {
                            ChipRow {
                                FmToneButton("Delete", onPressed = { if (!busy) requestDelete() }, icon = "delete_outline", tone = FmTone.Danger)
                            }
                        }
                    }
                }
                if (row.isDesktop && row.localId > 0) {
                    val idle = !active && !row.isActive
                    Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3)) {
                        ChipRow {
                            if (idle) FmToneButton("Merge & clean up", onPressed = { showMerge = true }, icon = "call_merge", tone = FmTone.Success)
                            if (idle && row.conflicted) {
                                FmToneButton("Fix conflicts", onPressed = { if (!busy) act(ConflictResolution.FIX_SENT) { center.fixConflicts(row) } }, icon = "smart_toy_outlined", tone = FmTone.Accent)
                            }
                            if (row.repoName.isNotEmpty() && !row.elsewhere) {
                                FmOutlinedButton("Worktrees", onClick = { navigator.push(WorktreesScreen(row.repoOwner, row.repoName)) }, icon = "account_tree_outlined")
                            }
                            if (!row.outside) FmToneButton("Delete", onPressed = { if (!busy) requestDelete() }, icon = "delete_outline", tone = FmTone.Danger)
                        }
                    }
                }
                if (!row.isDesktop && row.cloudSessionId.isNotEmpty()) {
                    val idle = !row.isActive && !active
                    Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3)) {
                        ChipRow {
                            if (idle && row.canMergeBranch) FmToneButton("Merge & clean up", onPressed = { showMerge = true }, icon = "call_merge", tone = FmTone.Success)
                            if (idle && row.canMergeBranch) {
                                FmToneButton("Update from ${row.baseBranch.ifEmpty { "base" }}", onPressed = { if (!busy) showUpdate = true }, icon = "sync", tone = FmTone.Accent)
                            }
                            if (idle && row.canMergeBranch && row.conflicted) {
                                FmToneButton("Fix conflicts", onPressed = { if (!busy) act(ConflictResolution.FIX_SENT) { center.fixConflicts(row) } }, icon = "smart_toy_outlined", tone = FmTone.Accent)
                            }
                            if (idle && row.hasBranchOps && row.needsCleanup) FmToneButton("Clean up", onPressed = { showCleanup = true }, icon = "cleaning_services_outlined", tone = FmTone.Accent)
                            if (row.prNumber > 0 && row.repoName.isNotEmpty()) {
                                FmOutlinedButton("PR #${row.prNumber}", onClick = { openPull(navigator, Repository(row.repoOwner, row.repoName), row.prNumber) }, icon = "call_merge_outlined")
                            } else if (idle && row.canCreatePull) {
                                FmToneButton(
                                    if (creatingPull) "Creating PR…" else "Create PR",
                                    onPressed = if (busy) null else ({ createPull() }),
                                    icon = "add_circle_outlined",
                                    tone = FmTone.Accent,
                                )
                            }
                            FmToneButton("Delete", onPressed = { if (!busy) requestDelete() }, icon = "delete_outline", tone = FmTone.Danger)
                        }
                    }
                }
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3, vertical = FmSpace.x1))
                val merged: (String) -> Unit = { message -> afterMerge(message, center, navigator, scope) }
                if (creatingPull) {
                    FmLinearProgressIndicator(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3, vertical = FmSpace.x1))
                }
                if (row.isDesktop && row.localId > 0) {
                    AgentBranchDiffRow(row.branchName, row.baseBranch) {
                        navigator.push(AgentDiffScreen(row.localId, row.title, canMerge = !active && !row.isActive, base = row.baseBranch, onMerged = merged, desktopKey = row.desktopKey))
                    }
                } else if (row.hasBranchOps) {
                    AgentBranchDiffRow(row.branchName, row.baseBranch) {
                        val mergeRow = if (row.canMergeBranch && !active && !row.isActive) row else null
                        navigator.push(RunnerDiffScreen(row.runnerRef(), row.title, mergeRow, merged))
                    }
                } else if (!row.isDesktop && row.branchName.isNotEmpty() && row.repoName.isNotEmpty() && AgentBranchDiff.screen != null) {
                    AgentBranchDiffRow(row.branchName, row.baseBranch) {
                        AgentBranchDiff.open(navigator, Repository(row.repoOwner, row.repoName), row.branchName, row.baseBranch, row.title)
                    }
                }
                AgentPromptDeck(
                    remember(blocks) { PromptDeck.of(blocks) },
                    Modifier.padding(horizontal = FmSpace.x3).padding(bottom = 4.dp),
                    tag = "agent-run-prompt-deck",
                )
                val list = rememberLazyListState()
                FmPullToRefreshBox(
                    isRefreshing = refreshing,
                    onRefresh = {
                        refreshing = true
                        scope.launch {
                            val live = liveStream
                            var followed = false
                            try {
                                withContext(Dispatchers.IO) {
                                    runCatching { center.refresh() }
                                    followed = live?.refollow() == true
                                }
                            } finally {
                                if (followed) refreshing = false else pulls++
                            }
                        }
                    },
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                ) {
                    TranscriptFollowBox(list, tail = text.length to blocks.size, modifier = Modifier.fillMaxSize(), pin = sent, tag = "agent-run-transcript") {
                        LazyColumn(Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(horizontal = FmSpace.x3, vertical = FmSpace.x2)) {
                            if (blocks.isEmpty()) {
                                item {
                                    SelectionContainer {
                                        Text(text.ifEmpty { "Nothing in the transcript yet." }, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                                    }
                                }
                            } else {
                                transcriptBlocks(blocks, if (row.repoOwner.isEmpty() || row.repoName.isEmpty()) null else RepoRef(row.repoOwner, row.repoName))
                            }
                            if (attachments.isNotEmpty()) item {
                                Spacer(Modifier.height(FmSpace.x3))
                                AgentAttachmentStrip(attachments)
                            }
                            transcriptEnd()
                        }
                    }
                }
                if (row.canPrompt) {
                    FmInlineStatus(sendStatus, Modifier.padding(horizontal = FmSpace.x3, vertical = FmSpace.x1))
                    AgentPromptComposer(
                        text = message,
                        onTextChange = { message = it },
                        draftSurface = PromptDraftStore.agentSurface("unified/${row.id}"),
                        hint = row.composerHint,
                        attachments = if (row.outside) emptyList() else photos,
                        onAttachmentsChanged = if (row.outside) null else ({ photos = it }),
                        onSend = {
                            val next = message.trim()
                            val sending = photos
                            if (next.isNotEmpty() || sending.isNotEmpty()) {
                                sent++
                                message = ""
                                photos = emptyList()
                                act("Sent.", sendStatus, onFailure = {
                                    if (message.isEmpty()) message = next
                                    if (photos.isEmpty()) photos = sending
                                }, notice = { center.promptNotice }) {
                                    center.prompt(row, next, sending)
                                    val live = liveStream
                                    if (live != null) live.refollow() else scope.launch { pulls++ }
                                }
                            }
                        },
                        busy = busy,
                    )
                }
            }
        }
        if (showMerge && !row.isDesktop) {
            RunnerMergeDialog(row, cleanupOnly = false, onDone = { message ->
                afterMerge(message, center, navigator, scope)
            }, onDismiss = { showMerge = false }, onActions = { message, commit ->
                agentsNotice.show(message)
                scope.launch { withContext(Dispatchers.IO) { center.refresh() } }
                leave(navigator)
                navigator.push(CodeFeature.actionsScreen(Repository(row.repoOwner, row.repoName), commit))
            })
        }
        if (showUpdate && !row.isDesktop) {
            RunnerMergeDialog(row, cleanupOnly = false, updateOnly = true, onDone = { message ->
                status.show(message)
                pulls++
                scope.launch { withContext(Dispatchers.IO) { runCatching { center.refresh() } } }
            }, onDismiss = {
                showUpdate = false
                scope.launch { withContext(Dispatchers.IO) { runCatching { center.refresh() } } }
            })
        }
        if (showCleanup) {
            RunnerMergeDialog(row, cleanupOnly = true, onDone = { message ->
                status.show(message)
                scope.launch { withContext(Dispatchers.IO) { center.refresh() } }
            }, onDismiss = { showCleanup = false })
        }
        if (showDelete && !row.isDesktop) {
            RunnerDeleteDialog(row, active = row.isActive || active, onDone = { message ->
                agentsNotice.show(message)
                scope.launch { withContext(Dispatchers.IO) { center.refresh() } }
                navigator.pop()
            }, onDismiss = { showDelete = false })
        }
        if (showMerge && row.isDesktop) {
            MergeCleanupDialog(row.localId, row.baseBranch, desktopKey = row.desktopKey, onDone = { message ->
                afterMerge(message, center, navigator, scope)
            }, onDismiss = { showMerge = false })
        }
        if (showDelete && row.isDesktop && !row.outside) {
            AgentDeleteDialog(row.localId, desktopKey = row.desktopKey, onDone = { message ->
                agentsNotice.show(message)
                scope.launch { withContext(Dispatchers.IO) { center.refresh() } }
                navigator.pop()
            }, onDismiss = { showDelete = false })
        }
    }

    private fun afterMerge(message: String, center: AgentControlCenter, navigator: Navigator, scope: CoroutineScope) {
        scope.launch { withContext(Dispatchers.IO) { center.refresh() } }
        if (message == ConflictResolution.FIX_SENT) {
            status.show(message)
        } else {
            agentsNotice.show(message)
            leave(navigator)
        }
    }

    private fun leave(navigator: Navigator) {
        if (this !in navigator.stack) return
        navigator.popTo(this)
        navigator.pop()
    }

    private suspend fun followCloud(center: AgentControlCenter, show: suspend (AgentTranscriptDelta) -> Unit) {
        val events = Channel<AgentLiveEvent>(Channel.UNLIMITED)
        var session: AgentLaunchSession? = null
        var liveText = ""
        var liveAt = 0L
        var hasLive = false
        var streaming = false
        var allowStream = true
        var stream: AgentLiveStream? = null
        suspend fun render() {
            val current = session ?: return
            show(cloudTranscriptDelta(row, if (hasLive) current.withLive(liveText, liveAt) else current))
        }
        try {
            while (true) {
                try {
                    session = withContext(Dispatchers.IO) { center.cloudSession(row) }
                    render()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    text = text.ifEmpty { controlError(error) }
                }
                refreshing = false
                if (!active) break
                if (stream == null && allowStream) {
                    val opened = center.cloudLive(row) { events.trySend(it) }
                    if (opened == null) {
                        allowStream = false
                    } else {
                        stream = opened
                        opened.start()
                    }
                }
                var reread = false
                val until = System.currentTimeMillis() + if (streaming) CLOUD_SAFETY_POLL_MS else CLOUD_POLL_MS
                while (!reread) {
                    val left = until - System.currentTimeMillis()
                    if (left <= 0) break
                    var event: AgentLiveEvent = withTimeoutOrNull(left) { events.receive() } ?: break
                    var changed = false
                    while (true) {
                        when (event) {
                            is AgentLiveEvent.Text -> {
                                liveText = event.text
                                liveAt = event.atMs
                                hasLive = true
                                streaming = true
                                changed = true
                            }
                            is AgentLiveEvent.Ended -> {
                                hasLive = false
                                streaming = false
                                allowStream = false
                                stream = null
                                reread = true
                            }
                            is AgentLiveEvent.Refused -> {
                                streaming = false
                                allowStream = false
                                stream = null
                            }
                            is AgentLiveEvent.Dropped -> {
                                if (streaming) reread = true
                                streaming = false
                            }
                        }
                        event = events.tryReceive().getOrNull() ?: break
                    }
                    if (changed && !reread) render()
                }
            }
        } finally {
            stream?.let { opened -> withContext(NonCancellable + Dispatchers.IO) { opened.stop() } }
        }
    }
}

private const val CLOUD_POLL_MS = 3_000L
private const val CLOUD_SAFETY_POLL_MS = 30_000L
