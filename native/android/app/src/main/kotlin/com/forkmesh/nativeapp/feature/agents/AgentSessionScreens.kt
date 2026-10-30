package com.forkmesh.nativeapp.feature.agents

import com.forkmesh.nativeapp.ui.kit.FmSheetAction
import com.forkmesh.nativeapp.ui.kit.showActionSheet
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.composer.PromptDraftStore
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.agents.AgentAttachmentRef
import com.forkmesh.nativeapp.agents.AgentAttachmentReconcile
import com.forkmesh.nativeapp.agents.AgentLaunchException
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentLiveEvent
import com.forkmesh.nativeapp.agents.AgentModelCatalog
import com.forkmesh.nativeapp.agents.AgentQueueApi
import com.forkmesh.nativeapp.agents.AgentSwipeDelete
import com.forkmesh.nativeapp.agents.AgentTranscriptAttachments
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.agentRetryDetail
import com.forkmesh.nativeapp.agents.withLive
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.control.desktopAttachments
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.AgentTranscript
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmMessageActions
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.compose.runtime.remember
import com.forkmesh.nativeapp.control.AgentTranscriptFormat
import com.forkmesh.nativeapp.control.PromptDeck

internal fun openPull(navigator: Navigator, repo: Repository, number: Int) {
    if (number <= 0) return
    navigator.push(
        CodeFeature.repoDetailScreen(repo, RepoDetailTab.Pulls, NotificationDeepLink(repo, RepoDetailTab.Pulls, number)),
    )
}

internal class AgentSessionLoaderScreen(private val repo: Repository, private val sessionId: Int) : Screen() {
    private var session by mutableStateOf<AgentSession?>(null)
    private var error by mutableStateOf<String?>(null)

    override val title: String get() = "Agent session #$sessionId"

    @Composable
    override fun Content(navigator: Navigator) {
        LaunchedEffect(Unit) {
            try {
                val page = withContext(Dispatchers.IO) {
                    Services.require<ApiService>().agentSessionPage(repo.owner, repo.name, AgentsFeature.ownerAccount(repo))
                }
                val found = page.sessions.firstOrNull { it.id == sessionId }
                if (found == null) error = "This agent session is not on the relay, or it is sealed to another device."
                else navigator.replace(AgentSessionDetailScreen(repo, found, AgentsFeature.ownerAccount(repo)))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = failure.message ?: failure.toString()
            }
        }
        DetailScaffold(title, navigator) {
            val failure = error
            if (failure == null) FmLinearProgressIndicator(Modifier.fillMaxWidth())
            else Text(failure, Modifier.padding(FmSpace.x5), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
        }
    }
}

internal class AgentSessionDetailScreen(
    private val repo: Repository,
    private val session: AgentSession,
    private val ownerAccount: String,
) : Screen() {
    private var transcript by mutableStateOf<AgentTranscript?>(null)
    private var desktopRefs by mutableStateOf<List<AgentAttachmentRef>?>(null)
    private var error by mutableStateOf<String?>(null)
    private var refreshing by mutableStateOf(false)
    private var showDelete by mutableStateOf(false)
    private val status = FmStatus()

    override val title: String get() = "Agent session #${session.id}"

    private suspend fun load() {
        try {
            transcript = withContext(Dispatchers.IO) {
                Services.require<ApiService>().agentTranscript(repo.owner, repo.name, session.id, ownerAccount)
            }
            error = null
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            error = failure.message ?: failure.toString()
        }
        loadDesktopRefs()
    }

    private suspend fun loadDesktopRefs() {
        try {
            val listed = withContext(Dispatchers.IO) { AgentsFeature.control.desktopAttachments(repo.name, session.id) }
            if (!listed.isNullOrEmpty()) desktopRefs = listed
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) { load() }
        DetailScaffold(
            title,
            navigator,
            actions = {
                FmIconButton("delete_outline", "Delete agent", onPressed = {
                    if (AgentsFeature.hub.desktop == null) status.error(AgentSwipeDelete.DESKTOP_OUT_OF_REACH) else showDelete = true
                }, color = Fm.colors.textPrimary)
            },
        ) {
            Column(Modifier.fillMaxSize()) {
                val said = transcript?.takeUnless { it.sealedForOtherDevice }?.transcript.orEmpty()
                AgentPromptDeck(
                    remember(said) { PromptDeck.of(AgentTranscriptFormat.blocks(said)) },
                    Modifier.padding(horizontal = FmSpace.x5, vertical = FmSpace.x1),
                    tag = "agent-session-prompt-deck",
                )
                FmPullToRefreshBox(
                    isRefreshing = refreshing,
                    onRefresh = {
                        refreshing = true
                        scope.launch {
                            try {
                                load()
                            } finally {
                                refreshing = false
                            }
                        }
                    },
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                ) {
                    LazyColumn(
                        Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(FmSpace.x5),
                        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
                    ) {
                        if (status.message != null) item { FmInlineStatus(status) }
                        item { SummaryCard(navigator) }
                        item { DesktopSessionOpsCard(navigator, session, repo) }
                        item { TranscriptCard() }
                    }
                }
            }
        }
        if (showDelete) {
            AgentDeleteDialog(session.id, onDone = { message ->
                agentsNotice.show(message)
                scope.launch { withContext(Dispatchers.IO) { runCatching { AgentsFeature.control.refresh() } } }
                navigator.pop()
            }, onDismiss = { showDelete = false })
        }
    }

    @Composable
    private fun SummaryCard(navigator: Navigator) {
        val c = Fm.colors
        val limit = session.rateLimitCountdownLabel()
        val failureReason = session.statusFailureReason(400)
        FmCard(radius = FmRadius.lg) {
            Column(Modifier.fillMaxWidth()) {
                Text(session.displayTitle, style = Fm.text.titleMedium.copy(color = c.textPrimary))
                Spacer(Modifier.height(FmSpace.x3))
                ChipRow {
                    AgentChip(session.providerLabel)
                    AgentChip(session.latestStatusSummary(120))
                    if (limit.isNotEmpty()) AgentChip(limit)
                    if (session.model.isNotEmpty()) AgentChip(session.model)
                    if (session.issueNumber > 0) AgentChip("#${session.issueNumber}")
                    if (session.branchName.isNotEmpty()) {
                        BranchChip(session.branchName, branchTap(navigator))
                    }
                    if (session.numTurns > 0) AgentChip("${session.numTurns} turns")
                    if (session.durationLabel.isNotEmpty()) AgentChip(session.durationLabel)
                    if (session.costLabel.isNotEmpty()) AgentChip(session.costLabel)
                    if (session.prLabel.isNotEmpty()) AgentChip(session.prLabel)
                    if (session.diffLabel.isNotEmpty()) AgentChip(session.diffLabel)
                    if (session.conflicted) AgentChip("Conflict")
                    if (session.tokenLabel.isNotEmpty()) AgentChip(session.tokenLabel)
                }
                Spacer(Modifier.height(FmSpace.x3))
                Text(
                    "Desktop-controlled session. Mobile can watch progress and open results here; to start, steer, stop or " +
                        "continue it, connect to the desktop on the same network (Agents → Desktop on this network) or reach it " +
                        "through the relay from All agents.",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
                Spacer(Modifier.height(FmSpace.x3))
                ChipRow {
                    val watch = AgentWatchEntry.open
                    if (watch != null) {
                        FmOutlinedButton(
                            if (session.isActive) "Watch & listen" else "Replay & listen",
                            onClick = { watch(navigator, if (session.isActive) "Watch agent" else "Replay agent", listOf(repo to session)) },
                            icon = "graphic_eq",
                        )
                    }
                    val diff = branchTap(navigator)
                    if (session.branchName.isNotEmpty() && diff != null) {
                        FmOutlinedButton("View changes", onClick = diff, icon = "difference_outlined")
                    }
                    if (session.prNumber > 0) {
                        FmOutlinedButton("Open PR #${session.prNumber}", onClick = { openPull(navigator, repo, session.prNumber) }, icon = "call_merge_outlined")
                    }
                }
                if (failureReason.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x3))
                    Text(failureReason, style = Fm.text.bodyMedium.copy(color = c.danger))
                }
            }
        }
    }

    private fun branchTap(navigator: Navigator): (() -> Unit)? {
        if (AgentBranchDiff.screen == null) return null
        return { AgentBranchDiff.open(navigator, repo, session.branchName, session.diffBase, session.displayTitle) }
    }

    @Composable
    private fun TranscriptCard() {
        val c = Fm.colors
        FmCard(radius = FmRadius.lg) {
            Column(Modifier.fillMaxWidth()) {
                val detail = transcript
                val failure = error
                when {
                    failure != null -> Text(failure, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                    detail == null -> FmLinearProgressIndicator(Modifier.fillMaxWidth())
                    else -> {
                        val status = if (detail.status.isNotEmpty()) AgentSession(session.id, status = detail.status).statusLabel else session.statusLabel
                        Text("Transcript", style = Fm.text.titleSmall.copy(color = c.textPrimary))
                        Spacer(Modifier.height(FmSpace.x3))
                        ChipRow {
                            AgentChip(status)
                            AgentChip(repo.fullName)
                        }
                        Spacer(Modifier.height(FmSpace.x3))
                        val refs = desktopRefs ?: AgentTranscriptAttachments.references(detail.transcript)
                        if (refs.isNotEmpty()) {
                            Text("Attachments", style = Fm.text.labelMedium.copy(color = c.textSecondary))
                            Spacer(Modifier.height(FmSpace.x2))
                            AgentAttachmentStrip(refs)
                            Spacer(Modifier.height(FmSpace.x3))
                        }
                        val text = when {
                            detail.sealedForOtherDevice ->
                                "This transcript is encrypted for another owner device. Sign in on this device and let it enroll; " +
                                    "new sessions are then sealed to it too."
                            detail.transcript.trim().isEmpty() -> "No transcript has been pushed by the desktop node yet."
                            else -> detail.transcript.trim()
                        }
                        SelectionContainer {
                            Text(text, style = Fm.text.bodySmall.copy(fontFamily = FmType.mono, color = c.textPrimary))
                        }
                    }
                }
            }
        }
    }
}

internal class LaunchedAgentSessionScreen(
    private val repo: Repository,
    initial: AgentLaunchSession,
    private val onChanged: () -> Unit = {},
) : Screen() {
    private var session by mutableStateOf(initial)
    private var followUp by mutableStateOf("")
    private var attachments by mutableStateOf<List<PromptAttachment>>(emptyList())
    private var busy by mutableStateOf(false)
    private var error by mutableStateOf<String?>(null)
    private var notice by mutableStateOf<String?>(null)
    private var refreshing by mutableStateOf(false)

    override val title: String get() = session.displayTitle

    override fun onDisposed() = onChanged()

    private var following = false
    private var followAgain = false
    private var liveText = ""
    private var liveAt = 0L
    private var hasLive = false
    private var streaming = false

    private suspend fun follow() {
        if (following) {
            followAgain = true
            return
        }
        following = true
        try {
            do {
                followAgain = false
                refresh(live = true)
            } while (followAgain)
        } finally {
            following = false
        }
    }

    private suspend fun refresh(live: Boolean = false) {
        try {
            val current = withContext(Dispatchers.IO) {
                runCatching { AgentsFeature.queue.session(repo.owner, repo.name, session.sessionId, session.orgScope) }.getOrNull()
                    ?: AgentsFeature.queue.launchOptions(repo.owner, repo.name).sessions.firstOrNull { it.sessionId == session.sessionId }
                    ?: session
            }
            if (!current.isActive) hasLive = false
            session = if (hasLive) current.withLive(liveText, liveAt) else current
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            if (!live) error = failure.message ?: failure.toString()
        }
    }

    private suspend fun followLive() {
        hasLive = false
        streaming = false
        val events = Channel<AgentLiveEvent>(Channel.UNLIMITED)
        val stream = AgentsFeature.queue.live(repo.owner, repo.name, session.sessionId, session.orgScope) { events.trySend(it) }
        try {
            stream.start()
            for (first in events) {
                var event = first
                var changed = false
                var reread = false
                var done = false
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
                            reread = true
                            done = true
                        }
                        is AgentLiveEvent.Refused -> {
                            streaming = false
                            done = true
                        }
                        is AgentLiveEvent.Dropped -> {
                            if (streaming) reread = true
                            streaming = false
                        }
                    }
                    event = events.tryReceive().getOrNull() ?: break
                }
                if (changed && hasLive) session = session.withLive(liveText, liveAt)
                if (reread) follow()
                if (done) break
            }
        } finally {
            streaming = false
            withContext(NonCancellable + Dispatchers.IO) { stream.stop() }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        LaunchedEffect(Unit) {
            launch { AgentsFeature.listSignal.collect { launch { follow() } } }
            launch { AgentsFeature.liveSignal.drop(1).collect { if (session.isActive && !streaming) launch { follow() } } }
        }
        LaunchedEffect(session.sessionId, session.isActive) {
            if (session.isActive) followLive()
        }

        fun run(action: suspend () -> Unit) {
            if (busy) return
            busy = true
            error = null
            scope.launch {
                try {
                    action()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: AgentLaunchException) {
                    error = failure.message
                } catch (failure: Exception) {
                    error = failure.message ?: failure.toString()
                } finally {
                    busy = false
                }
            }
        }

        val send: () -> Unit = {
            val text = followUp.trim()
            val photos = attachments
            if (text.isNotEmpty() || photos.isNotEmpty()) {
                run {
                    withContext(Dispatchers.IO) {
                        AgentsFeature.queue.steer(repo.owner, repo.name, session.sessionId, text, session.orgScope, photos)
                    }
                    AgentsFeature.sentPhotos.record(session.sessionId, text.ifEmpty { "See the attached photos." }, photos)
                    followUp = ""
                    attachments = emptyList()
                    refresh()
                }
            }
        }
        val retry: () -> Unit = {
            notice = null
            run {
                val outcome = withContext(Dispatchers.IO) { AgentsFeature.queue.retry(repo.owner, repo.name, session) }
                outcome.session?.let {
                    AgentsFeature.sentPhotos.rename(session.sessionId, it.sessionId)
                    session = it
                }
                notice = outcome.label
                refresh()
            }
        }
        val stop: () -> Unit = {
            run {
                val result = withContext(Dispatchers.IO) { AgentsFeature.queue.stop(repo.owner, repo.name, session.sessionId, session.orgScope) }
                notice = result.label
                refresh()
            }
        }
        val drop: () -> Unit = {
            run {
                val confirmed = messenger.showActionSheet(
                    title = "Drop this session?",
                    message = "The queue entry and its record here go away. Work already running on the node keeps going there.",
                    actions = listOf(FmSheetAction("Drop", destructive = true)),
                    cancelLabel = "Keep",
                ) == 0
                if (confirmed) {
                    withContext(Dispatchers.IO) { AgentsFeature.queue.cancel(repo.owner, repo.name, session.sessionId, session.orgScope) }
                    navigator.pop()
                }
            }
        }

        DetailScaffold(
            session.displayTitle,
            navigator,
            actions = {
                FmIconButton("delete_outline", "Delete", onPressed = if (busy) null else drop, color = c.textPrimary)
            },
        ) {
            Column(Modifier.fillMaxSize()) {
                AgentPromptDeck(
                    remember(session.history) { PromptDeck.said(session.history.filter { it.isTask }.map { it.text }) },
                    Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                    tag = "session-prompt-deck",
                )
                FmPullToRefreshBox(
                    isRefreshing = refreshing,
                    onRefresh = {
                        refreshing = true
                        scope.launch {
                            try {
                                refresh()
                            } finally {
                                refreshing = false
                            }
                        }
                    },
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                ) {
                    LazyColumn(
                        Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(FmSpace.x4),
                        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
                    ) {
                        item { Header(navigator) }
                        if (session.isRetryable) item { RetryCard(retry) }
                        val note = session.diagnosticNote
                        if (note.isNotEmpty()) item { Text(note, style = Fm.text.bodyMedium.copy(color = c.warning)) }
                        if (session.lastError.isNotEmpty()) item { Text(session.lastError, style = Fm.text.bodyMedium.copy(color = c.danger)) }
                        if (session.history.isEmpty()) {
                            item { Text("No transcript yet. It fills in as the node works.", style = Fm.text.bodyMedium.copy(color = c.textSecondary)) }
                        }
                        itemsIndexed(session.history) { _, message ->
                            Column {
                                FmMessageActions(message.text, Modifier.fillMaxWidth()) {
                                    Column {
                                        Text(
                                            if (message.isUser) message.author.ifEmpty { "You" } else "Agent",
                                            style = Fm.text.bodySmall.copy(color = c.textTertiary, fontWeight = FmType.emphasis),
                                        )
                                        Spacer(Modifier.height(FmSpace.x1))
                                        LinkedText(message.text, RepoRef(repo.owner, repo.name), style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                                    }
                                }
                                val refs = turnAttachments(message)
                                if (refs.isNotEmpty()) {
                                    Spacer(Modifier.height(FmSpace.x2))
                                    AgentAttachmentStrip(refs)
                                }
                            }
                        }
                        notice?.let { item { Text(it, style = Fm.text.bodyMedium.copy(color = c.success)) } }
                        error?.let { item { Text(it, style = Fm.text.bodyMedium.copy(color = c.danger)) } }
                    }
                }
                if (!session.isPromptable) {
                    Text(
                        if (session.isActive) "Follow-ups open once the mirror has picked this up."
                        else "This session never started on a node, so there is nothing to resume.",
                        Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
                        style = Fm.text.bodySmall.copy(color = c.textTertiary),
                    )
                } else {
                    AgentPromptComposer(
                        text = followUp,
                        onTextChange = { followUp = it },
                        draftSurface = PromptDraftStore.agentSurface("cloud/${repo.owner}/${repo.name}#${session.sessionId}"),
                        hint = if (session.isActive) "Tell the agent what to do next…" else "Continue this session on its branch…",
                        onSend = send,
                        busy = busy,
                        running = session.isActive,
                        onStop = if (busy) null else stop,
                        attachments = attachments,
                        onAttachmentsChanged = { attachments = it },
                        modelLabel = session.requestedModel.takeIf { it.isNotEmpty() }?.let(AgentModelCatalog::shortLabel),
                    )
                }
            }
        }
    }

    private fun turnAttachments(message: com.forkmesh.nativeapp.agents.AgentLaunchMessage): List<AgentAttachmentRef> {
        if (message.attachments.isNotEmpty()) {
            val sent = AgentsFeature.sentPhotos.photos(session.sessionId, message.text.trim())
            val pairs = if (sent != null) AgentAttachmentReconcile.pair(sent, message.attachments) else null
            val refs = session.attachmentRefs(message, repo.owner, repo.name)
            if (pairs != null) {
                for ((index, pair) in pairs.withIndex()) AgentAttachmentCache.put(refs[index], pair.second.bytes)
            }
            return refs
        }
        if (message.isUser) {
            val sent = AgentsFeature.sentPhotos.photos(session.sessionId, message.text.trim())
            if (sent != null) return sent.map(AgentAttachmentCache::local)
        }
        return AgentTranscriptAttachments.references(message.text)
    }

    @Composable
    private fun Header(navigator: Navigator) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            ChipRow {
                AgentChip(session.statusLabel)
                if (session.targetNode.isNotEmpty()) AgentChip(session.targetNode)
                if (session.provider.isNotEmpty()) AgentChip(session.provider)
                if (session.requestedModel.isNotEmpty()) AgentChip(session.requestedModel)
                if (session.createPr) AgentChip("opens PR")
                if (session.branchName.isNotEmpty()) BranchChip(session.branchName, branchTap(navigator))
            }
        }
    }

    private fun branchTap(navigator: Navigator): (() -> Unit)? {
        if (AgentBranchDiff.screen == null) return null
        return { AgentBranchDiff.open(navigator, repo, session.branchName, session.baseBranch, session.displayTitle) }
    }

    @Composable
    private fun RetryCard(onRetry: () -> Unit) {
        val c = Fm.colors
        FmCard(radius = FmRadius.lg) {
            Column(Modifier.fillMaxWidth()) {
                Text(if (session.isStopped) "This run stopped" else "This run failed", style = Fm.text.titleSmall.copy(color = c.textPrimary))
                Spacer(Modifier.height(FmSpace.x1))
                Text(agentRetryDetail(session), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                Spacer(Modifier.height(FmSpace.x3))
                Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.CenterStart) {
                    FmFilledButton(
                        if (session.canResume) "Retry on its branch" else "Retry",
                        onClick = if (busy) null else onRetry,
                        icon = "refresh",
                    )
                }
            }
        }
    }
}

internal val maxPromptChars: Int get() = AgentQueueApi.MAX_AGENT_PROMPT_CHARS
