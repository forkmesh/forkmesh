package com.forkmesh.nativeapp.feature.code.review

import com.forkmesh.nativeapp.ui.kit.confirmAction
import androidx.compose.foundation.background
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.LocalTextStyle
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
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.code.ConflictResolution
import com.forkmesh.nativeapp.code.diff.DiffFile
import com.forkmesh.nativeapp.code.diff.DiffLine
import com.forkmesh.nativeapp.code.diff.DiffLineKind
import com.forkmesh.nativeapp.collab.DiffLineSelection
import com.forkmesh.nativeapp.collab.DiffSide
import com.forkmesh.nativeapp.collab.MergeQueueAction
import com.forkmesh.nativeapp.collab.MergeQueueRemote
import com.forkmesh.nativeapp.collab.MergeQueueRepo
import com.forkmesh.nativeapp.collab.MergeQueueRequest
import com.forkmesh.nativeapp.collab.MergeQueueRules
import com.forkmesh.nativeapp.collab.PullMergeInputs
import com.forkmesh.nativeapp.collab.PullMergePlan
import com.forkmesh.nativeapp.collab.PullMergeRules
import com.forkmesh.nativeapp.collab.PullMergeState
import com.forkmesh.nativeapp.collab.PullReviewRecord
import com.forkmesh.nativeapp.collab.PullReviewSession
import com.forkmesh.nativeapp.collab.ReviewLedger
import com.forkmesh.nativeapp.collab.ReviewThread
import com.forkmesh.nativeapp.collab.ReviewThreadActions
import com.forkmesh.nativeapp.collab.ReviewVerdict
import com.forkmesh.nativeapp.control.AgentSessionLookup
import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.EmptyCard
import com.forkmesh.nativeapp.feature.code.ErrorCard
import com.forkmesh.nativeapp.feature.code.InfoCard
import com.forkmesh.nativeapp.feature.code.LoadingCard
import com.forkmesh.nativeapp.feature.code.MobileCard
import com.forkmesh.nativeapp.feature.code.navigationBarBottom
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch
import com.forkmesh.nativeapp.collab.PullState
import com.forkmesh.nativeapp.feature.code.collab.PullDoneCard

internal class PullReviewScreen(private val repo: Repository, private val number: Int) : Screen() {
    private val session = PullReviewSession(repo.owner, repo.name, number, CodeServices.ledgerReader(), CodeServices.writer(repo))
    override val title: String get() = "Review #$number"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val status = rememberFmStatus()
        LaunchedEffect(Unit) {
            io { session.load() }
            tick.bump()
        }
        tick.value
        DetailScaffold("Review #$number", navigator, actions = {
            if (session.canWrite && session.record != null) {
                FmToneButton("Submit${if (session.drafts.isEmpty) "" else " (${session.drafts.count})"}", onPressed = {
                    scope.launch {
                        val verdict = messenger.showModalBottomSheet<ReviewVerdict> { close ->
                            Column(Modifier.padding(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                Text("Submit review", style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
                                Note(session.drafts.summary)
                                for (option in ReviewVerdict.entries) {
                                    FmToneButton(option.title, onPressed = { close(option) }, modifier = Modifier.fillMaxWidth(),
                                        tone = if (option == ReviewVerdict.RequestChanges) FmTone.Danger else if (option == ReviewVerdict.Approve) FmTone.Success else FmTone.Neutral)
                                }
                            }
                        } ?: return@launch
                        val body = messenger.showPromptDialog(verdict.title, hint = "Summary (optional)", confirmLabel = "Send") ?: return@launch
                        val notice = io { session.submit(verdict, body) }
                        tick.bump()
                        status.report(notice.message, notice.isError)
                    }
                }, tone = FmTone.Accent, filled = true)
            }
        }) {
            val record = session.record
            ReviewList {
                if (status.message != null) item { FmInlineStatus(status) }
                when {
                    record == null && session.loadError != null -> item { ErrorCard(session.loadError!!, { scope.launch { io { session.load() }; tick.bump() } }) }
                    record == null -> item { LoadingCard("Loading the pull request…") }
                    else -> {
                        item {
                            InfoCard(record.title) {
                                Note("${record.head.ifEmpty { "patch" }} → ${record.base} · ${record.status}")
                                val snap = session.snapshot
                                Note("${snap.unresolvedThreads} open threads · ${snap.resolvedThreads} resolved · ${snap.outdatedThreads} outdated · ${session.viewedSummary}")
                                if (!session.canWrite) Note("Sign in on this device to comment.")
                            }
                        }
                        if (session.files.isEmpty()) item { EmptyCard("This pull request has no patch to review.") }
                        items(session.files, key = { it.path }) { file ->
                            val count = session.threadCount(file.path)
                            MobileCard(
                                icon = if (file.path in session.viewed) "check_circle_outline" else "description_outlined",
                                iconColor = if (file.path in session.viewed) Fm.colors.success else Fm.colors.textSecondary,
                                title = file.path,
                                subtitle = "+${file.additions} −${file.deletions}${if (count > 0) " · $count threads" else ""}",
                                onTap = { navigator.push(PullFileReviewScreen(session, file.path) { tick.bump() }) },
                            )
                        }
                        val general = session.snapshot.threads.filter { it.path.isEmpty() }
                        items(general, key = { it.id }) { thread -> ThreadCard(session, thread) { tick.bump() } }
                    }
                }
            }
        }
    }
}

@Composable
private fun ReviewList(content: androidx.compose.foundation.lazy.LazyListScope.() -> Unit) {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(
            start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x3, bottom = FmSpace.x6 + navigationBarBottom(),
        ),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        content = content,
    )
}

@Composable
internal fun ThreadCard(session: ReviewThreadActions, thread: ReviewThread, onChanged: () -> Unit) {
    val scope = rememberCoroutineScope()
    val messenger = LocalFmMessenger.current
    val status = rememberFmStatus()
    val title = when {
        thread.path.isEmpty() -> "Thread"
        thread.lineStart <= 0 -> thread.path
        thread.lineEnd > thread.lineStart -> "${thread.path}:${thread.lineStart}–${thread.lineEnd}"
        else -> "${thread.path}:${thread.lineStart}"
    }
    InfoCard(title) {
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            if (thread.resolved) FmStatusBadge("Resolved", Fm.colors.success)
            if (thread.placementLabel.isNotEmpty()) FmStatusBadge(thread.placementLabel, Fm.colors.warning)
            if (thread.hasSuggestion) FmStatusBadge(thread.suggestionState.ifEmpty { "Suggestion" }, Fm.colors.accent)
        }
        for (event in thread.events) {
            if (event.body.isBlank() && event.type == "thread-state") continue
            Column {
                Text(event.displayAuthor.take(24) + if (session.isPending(event)) " · sending" else "", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                if (event.body.isNotBlank()) LinkedText(event.body, RepoRef(session.owner, session.name), style = LocalTextStyle.current.copy(color = Fm.colors.textPrimary))
                if (event.suggestionPatch.isNotEmpty()) Mono(event.suggestionPatch)
            }
        }
        if (session.canWrite) {
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmToneButton("Reply", onPressed = {
                    scope.launch {
                        val body = messenger.showPromptDialog("Reply", hint = "Reply", confirmLabel = "Send") ?: return@launch
                        val notice = io { session.reply(thread, body) }
                        status.report(notice.message, notice.isError)
                        onChanged()
                    }
                })
                FmToneButton(if (thread.resolved) "Reopen" else "Resolve", onPressed = {
                    scope.launch {
                        val notice = io { session.setResolved(thread, !thread.resolved) }
                        status.report(notice.message, notice.isError)
                        onChanged()
                    }
                })
            }
            FmInlineStatus(status)
        }
    }
}

internal class PullFileReviewScreen(
    private val session: PullReviewSession,
    private val path: String,
    private val onChanged: () -> Unit,
) : Screen() {
    override val title: String get() = path.substringAfterLast('/')

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val status = rememberFmStatus()
        var body by remember { mutableStateOf("") }
        var suggestion by remember { mutableStateOf("") }
        tick.value
        val file: DiffFile? = session.file(path)
        val byLine = session.snapshot.threadsByLine(path)
        val selection = session.selection?.takeIf { it.path == path }
        fun changed() {
            tick.bump()
            onChanged()
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton(
                if (path in session.viewed) "check_circle" else "check_circle_outline",
                "Viewed",
                onPressed = { session.toggleViewed(path); changed() },
            )
        }) {
            Column(Modifier.fillMaxSize()) {
                LazyColumn(Modifier.weight(1f).fillMaxWidth()) {
                    if (file == null) {
                        item { Box(Modifier.padding(FmSpace.x4)) { EmptyCard("This file is not in the patch.") } }
                    } else {
                        for (hunk in file.hunks) {
                            item { Mono(hunk.header, Fm.colors.textTertiary) }
                            items(hunk.lines) { line ->
                                val picked = selection != null && selection.contains(line)
                                DiffRow(line, picked) {
                                    session.tap(path, line)
                                    changed()
                                }
                                val side = if (line.kind == DiffLineKind.Removed) DiffSide.Old else DiffSide.New
                                val number = if (side == DiffSide.Old) line.oldNumber else line.newNumber
                                for (thread in byLine[side]?.get(number).orEmpty()) {
                                    Box(Modifier.padding(horizontal = FmSpace.x3, vertical = FmSpace.x1)) { ThreadCard(session, thread) { tick.bump() } }
                                }
                            }
                        }
                    }
                    val outdated = session.snapshot.outdatedThreads(path)
                    if (outdated.isNotEmpty()) {
                        item { Box(Modifier.padding(FmSpace.x3)) { Note("Outdated threads") } }
                        items(outdated, key = { it.id }) { thread ->
                            Box(Modifier.padding(horizontal = FmSpace.x3, vertical = FmSpace.x1)) { ThreadCard(session, thread) { tick.bump() } }
                        }
                    }
                }
                if (selection != null && session.canWrite) {
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .background(Fm.colors.bgRaised)
                            .padding(FmSpace.x3),
                        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
                    ) {
                        Note(selection.label)
                        FmTextField(body, { body = it }, hint = "Comment", minLines = 2, maxLines = 6)
                        FmTextField(suggestion, { suggestion = it }, hint = "Suggested replacement (optional)", maxLines = 6)
                        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            FmToneButton("Comment now", onPressed = {
                                scope.launch {
                                    val notice = io { session.commentNow(body, suggestion) }
                                    if (!notice.isError) {
                                        body = ""
                                        suggestion = ""
                                    }
                                    status.report(notice.message, notice.isError)
                                    changed()
                                }
                            }, tone = FmTone.Accent, filled = true)
                            FmToneButton("Add to review", onPressed = {
                                val problem = session.addDraft(body, suggestion)
                                if (problem == null) {
                                    body = ""
                                    suggestion = ""
                                    status.clear()
                                } else {
                                    status.error(problem)
                                }
                                changed()
                            })
                            FmToneButton("Cancel", onPressed = { session.clearSelection(); changed() })
                        }
                        FmInlineStatus(status)
                    }
                } else if (status.message != null) {
                    FmInlineStatus(
                        status,
                        Modifier
                            .background(Fm.colors.bgRaised)
                            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x1),
                    )
                }
            }
        }
    }
}

@Composable
private fun DiffRow(line: DiffLine, selected: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    val background = when {
        selected -> c.accent.copy(alpha = 0.22f)
        line.kind == DiffLineKind.Added -> c.success.copy(alpha = 0.10f)
        line.kind == DiffLineKind.Removed -> c.danger.copy(alpha = 0.10f)
        else -> c.bgBase
    }
    val marker = when (line.kind) {
        DiffLineKind.Added -> "+"
        DiffLineKind.Removed -> "-"
        DiffLineKind.Context -> " "
    }
    Row(
        Modifier
            .fillMaxWidth()
            .background(background)
            .clickable(onClick = onTap)
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = FmSpace.x2),
    ) {
        Text(if (line.oldNumber > 0) "${line.oldNumber}" else "", Modifier.width(36.dp), color = c.textTertiary, style = Fm.text.bodySmall.copy(fontFamily = FmType.mono))
        Text(if (line.newNumber > 0) "${line.newNumber}" else "", Modifier.width(36.dp), color = c.textTertiary, style = Fm.text.bodySmall.copy(fontFamily = FmType.mono))
        Text("$marker${line.text}", color = c.textPrimary, style = Fm.text.bodySmall.copy(fontFamily = FmType.mono), softWrap = false)
    }
}

@Composable
internal fun PullWorkflowCards(repo: Repository, pull: PublishedPull, navigator: Navigator) {
    val scope = rememberCoroutineScope()
    val messenger = LocalFmMessenger.current
    val tick = rememberTick()
    var record by remember(pull.number) { mutableStateOf<PullReviewRecord?>(null) }
    var isOwner by remember(pull.number) { mutableStateOf<Boolean?>(null) }
    var plan by remember(pull.number) { mutableStateOf<PullMergePlan?>(null) }
    var planLoaded by remember(pull.number) { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var fixing by remember { mutableStateOf(false) }
    var queue by remember { mutableStateOf<MergeQueueRepo?>(null) }
    val mergeStatus = rememberFmStatus()
    val queueStatus = rememberFmStatus()
    val remote = remember { runCatching { CodeServices.mergeQueue() }.getOrNull() }
    suspend fun refresh() {
        planLoaded = false
        record = io { runCatching { ReviewLedger(CodeServices.ledgerReader(), repo.owner, repo.name).load(pull.number) }.getOrNull() }
        isOwner = io { CodeServices.canMergePulls(repo) }
        plan = if (isOwner == true) io { CodeServices.mergeClient().plan(repo.owner, repo.name, pull) } else null
        planLoaded = true
        queue = remote?.let { io { runCatching { it.list(repo.owner, repo.name).firstOrNull() }.getOrNull() } }
        tick.bump()
    }
    LaunchedEffect(pull.number) { refresh() }
    tick.value
    val state = PullMergeRules.state(
        PullMergeInputs(pull.status, record?.author.orEmpty(), record?.events.orEmpty(), isOwner, plan, planLoaded),
    )
    LaunchedEffect(pull.number, state is PullMergeState.Conflicts) {
        if (state is PullMergeState.Conflicts) {
            io { runCatching { AgentsFeature.control.refresh() } }
            tick.bump()
        }
    }
    val pullState = if (state == PullMergeState.Merged) PullState.Merged else PullState.of(pull)
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        if (pullState != PullState.Open) {
            PullDoneCard(pull, pullState) {
                Row(Modifier.padding(top = FmSpace.x2), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    FmToneButton("Review files", onPressed = { navigator.push(PullReviewScreen(repo, pull.number)) }, icon = "rate_review_outlined")
                }
                FmInlineStatus(mergeStatus)
            }
        } else InfoCard("Merge") {
            FmStatusBadge(mergeStateLabel(state), toneOf(state.tone))
            Note(PullMergeRules.headline(state, pull.base))
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmToneButton("Review files", onPressed = { navigator.push(PullReviewScreen(repo, pull.number)) }, icon = "rate_review_outlined")
                if (state.canMerge && plan != null) {
                    FmToneButton(if (busy) "Merging…" else "Merge pull request", onPressed = {
                        if (busy) return@FmToneButton
                        scope.launch {
                            if (state.needsWaiver &&
                                !messenger.showConfirmDialog(PullMergeRules.WAIVER_TITLE, PullMergeRules.WAIVER_COPY, "Merge anyway", destructive = true)
                            ) {
                                return@launch
                            }
                            busy = true
                            val outcome = io {
                                runCatching { CodeServices.mergeClient().merge(repo.owner, repo.name, plan!!, state.needsWaiver).message }
                                    .fold({ it to false }, { (it.message ?: "$it") to true })
                            }
                            busy = false
                            mergeStatus.report(outcome.first, outcome.second)
                            refresh()
                        }
                    }, tone = FmTone.Success, filled = true)
                }
                FmToneButton("Re-check", onPressed = { scope.launch { refresh() } })
            }
            if (state is PullMergeState.Conflicts) {
                val agent = AgentSessionLookup.forPull(AgentsFeature.control.rows, repo.owner, repo.name, pull.number, pull.head)
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    if (agent != null && !agent.isActive) {
                        FmToneButton(if (fixing) "Asking…" else "Fix with agent", onPressed = {
                            if (fixing) return@FmToneButton
                            fixing = true
                            scope.launch {
                                val outcome = io {
                                    runCatching { AgentsFeature.control.fixConflicts(agent, state.files); ConflictResolution.FIX_SENT }
                                        .fold({ it to false }, { ControlErrorText.describe(it) to true })
                                }
                                fixing = false
                                mergeStatus.report(outcome.first, outcome.second)
                            }
                        }, icon = "smart_toy_outlined", tone = FmTone.Accent, filled = true)
                    }
                    if (pull.head.isNotEmpty()) {
                        FmToneButton("Resolve with an agent", onPressed = {
                            navigator.push(AgentsFeature.startSheetScreen(ConflictResolution.agentPrompt(pull.head, pull.base, state.files)))
                        }, icon = "smart_toy_outlined", tone = if (agent == null) FmTone.Accent else FmTone.Neutral)
                    }
                }
            }
            FmInlineStatus(mergeStatus)
        }
        ChecksSummaryCard(repo, pull.number, navigator)
        val q = queue
        if (remote != null && q != null) {
            InfoCard("Merge queue") {
                Note(q.summary)
                q.place(pull.number)?.let { Note("This pull request is #$it in the queue.") }
                if (MergeQueueRules.canEnqueue(pull.status, q, pull.number)) {
                    FmToneButton(MergeQueueAction.Enqueue.title, onPressed = {
                        scope.launch {
                            val outcome = io {
                                runCatching { remote.perform(MergeQueueRequest(repo.owner, repo.name, MergeQueueAction.Enqueue, pull.number)).message }
                                    .fold({ it.ifEmpty { "Added to the merge queue." } to false }, { MergeQueueRemote.message(it) to true })
                            }
                            queueStatus.report(outcome.first, outcome.second)
                            refresh()
                        }
                    })
                }
                FmToneButton("Open the queue", onPressed = { navigator.push(MergeQueueScreen(repo)) })
                FmInlineStatus(queueStatus)
            }
        }
    }
}

private fun mergeStateLabel(state: PullMergeState): String = when (state) {
    PullMergeState.Merged -> "Merged"
    PullMergeState.Closed -> "Closed"
    PullMergeState.Checking -> "Checking"
    PullMergeState.NotOwner -> "Not an owner"
    PullMergeState.NotPinned -> "Not pinned"
    is PullMergeState.Conflicts -> "Conflicts"
    is PullMergeState.ChangesRequested -> "Changes requested"
    PullMergeState.NeedsIndependentReview -> "No independent review"
    PullMergeState.Ready -> "Ready"
}

internal class MergeQueueScreen(private val repo: Repository?) : Screen() {
    override val title: String get() = "Merge queue"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val remote = remember { runCatching { CodeServices.mergeQueue() }.getOrNull() }
        val status = rememberFmStatus()
        var repos by remember { mutableStateOf<List<MergeQueueRepo>?>(null) }
        var error by remember { mutableStateOf<String?>(null) }
        suspend fun load() {
            val r = remote ?: run {
                error = "Connect to your desktop to see its merge queue."
                return
            }
            io {
                runCatching { r.list(repo?.owner.orEmpty(), repo?.name.orEmpty()) }
                    .onSuccess { repos = it; error = null }
                    .onFailure { error = MergeQueueRemote.message(it) }
            }
            tick.bump()
        }
        suspend fun perform(request: MergeQueueRequest) {
            val r = remote ?: return
            if (request.action.isDestructive && !messenger.confirmAction(request.action.title, request.action.title)) return
            val outcome = io { runCatching { r.perform(request).message }.fold({ it to false }, { MergeQueueRemote.message(it) to true }) }
            if (outcome.first.isNotEmpty()) status.report(outcome.first, outcome.second) else status.clear()
            load()
        }
        LaunchedEffect(Unit) { load() }
        tick.value
        DetailScaffold("Merge queue", navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } })
        }) {
            ScreenList {
                if (status.message != null) item { FmInlineStatus(status) }
                error?.let { item { ErrorCard(it, { scope.launch { load() } }) } }
                val list = repos
                if (list == null && error == null) item { LoadingCard("Asking the desktop…") }
                if (list != null && list.isEmpty()) item { EmptyCard("The desktop has no merge queues.") }
                items(list.orEmpty(), key = { it.id }) { q ->
                    InfoCard(q.id) {
                        Note(q.summary)
                        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            for (action in MergeQueueRules.actions(q)) {
                                FmToneButton(action.title, onPressed = { scope.launch { perform(MergeQueueRequest(q.owner, q.name, action)) } },
                                    tone = if (action.isDestructive) FmTone.Danger else FmTone.Neutral)
                            }
                        }
                        for (entry in q.entries) {
                            Column {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Text("#${entry.number} ${entry.title}", Modifier.weight(1f), color = Fm.colors.textPrimary)
                                    FmStatusBadge(entry.state.title, if (entry.state.needsAttention) Fm.colors.danger else if (entry.state.isRunning) Fm.colors.accent else Fm.colors.textTertiary)
                                }
                                if (entry.detail.isNotEmpty()) Note(entry.detail)
                                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                    for ((action, delta) in MergeQueueRules.entryActions(entry, q)) {
                                        val label = if (action == MergeQueueAction.Move) (if (delta < 0) "Up" else "Down") else action.title
                                        FmToneButton(label, onPressed = { scope.launch { perform(MergeQueueRequest(q.owner, q.name, action, entry.number, delta)) } })
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
