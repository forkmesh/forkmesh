package com.forkmesh.nativeapp.feature.code.review

import com.forkmesh.nativeapp.ui.kit.confirmAction
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.ci.ActionsControlKind
import com.forkmesh.nativeapp.ci.ActionsRunStatus
import com.forkmesh.nativeapp.ci.ActionsRunSummary
import com.forkmesh.nativeapp.ci.CiLog
import com.forkmesh.nativeapp.ci.CiPrompts
import com.forkmesh.nativeapp.ci.DesktopActions
import com.forkmesh.nativeapp.ci.ReleaseAnnouncementDraft
import com.forkmesh.nativeapp.ci.RepoActionsModel
import com.forkmesh.nativeapp.ci.RepoMirrorsModel
import com.forkmesh.nativeapp.ci.RepoReleasesModel
import com.forkmesh.nativeapp.ci.RepoSecurityModel
import com.forkmesh.nativeapp.ci.RepoWebhookDraft
import com.forkmesh.nativeapp.ci.RepoWebhooksModel
import com.forkmesh.nativeapp.ci.SecuritySeverity
import com.forkmesh.nativeapp.control.DesktopReach
import com.forkmesh.nativeapp.control.HubControlCalling
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.EmptyCard
import com.forkmesh.nativeapp.feature.code.ErrorCard
import com.forkmesh.nativeapp.feature.code.InfoCard
import com.forkmesh.nativeapp.feature.code.Kv
import com.forkmesh.nativeapp.feature.code.LoadingCard
import com.forkmesh.nativeapp.feature.code.MobileCard
import com.forkmesh.nativeapp.feature.code.rememberCopyText
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

@Composable
private fun runColor(status: ActionsRunStatus) = when (status) {
    ActionsRunStatus.Success -> Fm.colors.success
    ActionsRunStatus.Failed, ActionsRunStatus.Rejected -> Fm.colors.danger
    ActionsRunStatus.AwaitingApproval -> Fm.colors.warning
    ActionsRunStatus.Running, ActionsRunStatus.Queued -> Fm.colors.accent
    else -> Fm.colors.textTertiary
}

private fun desktopActions(): DesktopActions? = runCatching { AgentsFeature.hub }.getOrNull()?.let { hub ->
    val name = (hub.reach as? DesktopReach.Connected)?.name ?: hub.desktop?.name.orEmpty()
    DesktopActions(HubControlCalling(hub), name)
}

internal fun actionsModel(repo: Repository): RepoActionsModel =
    RepoActionsModel(CodeServices.ci(), CodeServices.ref(repo), desktopActions())

@Composable
private fun RepoActionsModel.BumpOnChange(tick: Tick) {
    DisposableEffect(this) {
        val remove = addChangeListener { tick.bump() }
        onDispose { remove() }
    }
}

internal class ActionsScreen(private val repo: Repository, private val commit: String = "") : Screen() {
    private val model = actionsModel(repo).also { it.commitFilter = commit.trim().ifEmpty { null } }
    override val title: String get() = "Actions"

    override fun onDisposed() {
        model.stop()
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val status = rememberFmStatus()
        model.BumpOnChange(tick)
        LaunchedEffect(Unit) {
            while (true) {
                io { model.load() }
                tick.bump()
                val wait = if (model.needsFollowUp || model.hasActiveRuns) RepoActionsModel.FOLLOW_UP_DELAY_MS else 30_000L
                delay(wait)
            }
        }
        suspend fun control(kind: ActionsControlKind, runId: Int? = null, workflow: String? = null, branch: String? = null, enabled: Boolean? = null) {
            if (kind.isDestructive && !messenger.confirmAction(kind.label, kind.label)) return
            io { model.send(kind, runId, workflow, branch, enabled) }
            tick.bump()
            model.notice?.let { status.report(it.text, !it.good) }
        }
        tick.value
        DetailScaffold("Actions · ${repo.name}", navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { io { model.load() }; tick.bump() } })
        }) {
            val summary = model.summary
            ScreenList {
                if (status.message != null) item { FmInlineStatus(status) }
                if (summary == null) {
                    item { if (model.error != null) ErrorCard(model.error!!, { scope.launch { io { model.load() }; tick.bump() } }) else LoadingCard("Loading runs…") }
                    return@ScreenList
                }
                item {
                    InfoCard(if (summary.node.isEmpty()) "Actions" else "Runs on ${summary.node}") {
                        if (model.fromDesktop) Note("Live from ${summary.node} over its desktop connection.")
                        if (summary.actionsEnabled != null && model.canControl && !model.fromDesktop) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Text("Actions enabled", Modifier.weight(1f), color = Fm.colors.textPrimary)
                                FmSwitch(checked = summary.actionsEnabled == true, onCheckedChange = { on ->
                                    scope.launch { control(ActionsControlKind.SetRepositoryEnabled, enabled = on) }
                                })
                            }
                        }
                        if (!model.canControl) Note("This node does not accept remote control; runs are read-only here.")
                    }
                }
                if (model.workflows.isNotEmpty()) {
                    item {
                        InfoCard("Workflows") {
                            Stack {
                                for (workflow in model.workflows) {
                                    Row(verticalAlignment = Alignment.CenterVertically) {
                                        Column(Modifier.weight(1f)) {
                                            Text(workflow.displayName, color = Fm.colors.textPrimary, style = Fm.text.titleSmall)
                                            Note((workflow.events + workflow.runsOn).joinToString(" · "))
                                        }
                                        if (workflow.canDispatch && model.canControl) {
                                            FmToneButton("Run", onPressed = {
                                                scope.launch {
                                                    val branch = messenger.showPromptDialog(
                                                        "Run ${workflow.displayName}", hint = "Branch", initialValue = repo.defaultBranch, confirmLabel = "Run",
                                                    ) ?: return@launch
                                                    if (branch.isNotEmpty()) control(ActionsControlKind.Dispatch, workflow = workflow.path, branch = branch)
                                                }
                                            }, tone = FmTone.Accent)
                                        }
                                        if (model.canControl && !model.fromDesktop) {
                                            FmSwitch(checked = workflow.enabled, onCheckedChange = { on ->
                                                scope.launch { control(ActionsControlKind.SetWorkflowEnabled, workflow = workflow.path, enabled = on) }
                                            })
                                        }
                                    }
                                }
                                val filters = listOf<String?>(null) + model.workflows.map { it.path }
                                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                    for (filter in filters) {
                                        val label = filter?.let { path -> model.workflows.first { it.path == path }.displayName } ?: "All runs"
                                        FmToneButton(label, onPressed = { model.workflowFilter = filter; tick.bump() }, filled = model.workflowFilter == filter)
                                    }
                                }
                            }
                        }
                    }
                }
                val shownCommit = model.commitFilter
                if (shownCommit != null) {
                    item {
                        InfoCard("Runs for ${shownCommit.take(7)}") {
                            Note("Push workflows on the merged commit start on the cores shortly after the merge lands.")
                            FmToneButton("Show all runs", onPressed = { model.commitFilter = null; tick.bump() })
                        }
                    }
                }
                val runs = model.runs
                if (runs.isEmpty()) {
                    item {
                        EmptyCard(if (shownCommit != null && model.summary?.runs.orEmpty().isNotEmpty()) "No runs for ${shownCommit.take(7)} yet. Refresh in a moment." else model.emptyMessage)
                    }
                }
                items(runs, key = { it.id }) { run ->
                    val now = System.currentTimeMillis()
                    MobileCard(
                        icon = "play_circle_outline",
                        iconColor = runColor(run.status),
                        title = run.displayName,
                        subtitle = listOf(
                            run.refLabel,
                            run.shortCommit,
                            run.durationLabel(now),
                            if (run.status.isActive && run.steps.isNotEmpty()) "step ${run.stepsDone}/${run.steps.size}" else "",
                        ).filter { it.isNotEmpty() }.joinToString(" · "),
                        trailing = { FmStatusBadge(if (model.isPending(run.id)) "Sending…" else run.status.label, runColor(run.status)) },
                        onTap = { navigator.push(ActionRunScreen(repo, model, run.id)) },
                    )
                }
            }
        }
    }
}

internal fun standaloneActionRunScreen(repo: Repository, runId: Int): Screen = ActionRunScreen(repo, actionsModel(repo), runId, ownsModel = true)

internal class ActionRunScreen(
    private val repo: Repository,
    private val model: RepoActionsModel,
    private val runId: Int,
    private val ownsModel: Boolean = false,
) : Screen() {
    override val title: String get() = "Run #$runId"

    override fun onDisposed() {
        model.unfollow(runId)
        if (ownsModel) model.stop()
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val runStatus = rememberFmStatus()
        val logStatus = rememberFmStatus()
        val copy = rememberCopyText(logStatus)
        val desktopRoute = com.forkmesh.nativeapp.feature.code.rememberDesktopRoute(repo)
        val commands = com.forkmesh.nativeapp.feature.code.rememberDesktopCommands()
        model.BumpOnChange(tick)
        LaunchedEffect(Unit) {
            io {
                if (model.summary == null) model.load()
                model.follow(runId)
            }
            tick.bump()
            while (true) {
                delay(RepoActionsModel.FOLLOW_UP_DELAY_MS)
                if (model.run(runId)?.status?.isActive == true || model.needsFollowUp) {
                    io {
                        model.load()
                        model.loadLog(runId)
                    }
                    tick.bump()
                }
            }
        }
        tick.value
        val run = model.run(runId)
        DetailScaffold("Run #$runId", navigator) {
            ScreenList {
                if (run == null) {
                    item { EmptyCard("This run is no longer listed.") }
                    return@ScreenList
                }
                item {
                    InfoCard(run.displayName) {
                        FmStatusBadge(run.status.label, runColor(run.status))
                        Kv("Ref", run.refLabel)
                        if (model.fromDesktop) Kv("Runs on", model.summary?.node.orEmpty())
                        Kv("Commit", run.shortCommit)
                        Kv("Duration", run.durationLabel(System.currentTimeMillis()).ifEmpty { "—" })
                        if (run.pullNumber > 0) Kv("Pull request", "#${run.pullNumber}")
                        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            for (kind in model.actions(run)) {
                                FmToneButton(kind.label, onPressed = {
                                    scope.launch {
                                        if (kind.isDestructive && !messenger.confirmAction(kind.label, kind.label)) return@launch
                                        io { model.send(kind, runId = run.id) }
                                        tick.bump()
                                        model.notice?.let { runStatus.report(it.text, !it.good) }
                                    }
                                }, tone = if (kind.isDestructive) FmTone.Danger else FmTone.Accent)
                            }
                        }
                        if (!model.canControl && desktopRoute && run.status.isFinished) {
                            FmToneButton(com.forkmesh.nativeapp.control.DesktopCommandContract.RERUN_LABEL, onPressed = {
                                scope.launch {
                                    val payload = linkedMapOf<String, Any?>("runId" to run.id.toLong())
                                    if (run.workflowPath.isNotEmpty()) payload["workflowPath"] = run.workflowPath
                                    commands.request(
                                        repo = repo,
                                        command = "action.rerun",
                                        target = com.forkmesh.nativeapp.control.DesktopCommandContract.runTarget(run.id.toLong()),
                                        payload = payload,
                                        successMessage = com.forkmesh.nativeapp.code.RepoActivity.RERUN_QUEUED,
                                    )
                                }
                            }, icon = "replay_outlined")
                        }
                        if (run.status == ActionsRunStatus.Failed) {
                            FmToneButton("Send to an agent", onPressed = {
                                navigator.push(AgentsFeature.startSheetScreen(CiPrompts.fixFailedRun(run, CodeServices.ref(repo)).text))
                            }, icon = "smart_toy_outlined", tone = FmTone.Accent)
                        }
                        FmInlineStatus(runStatus)
                        FmInlineStatus(commands.status)
                    }
                }
                if (run.steps.isNotEmpty()) {
                    item {
                        InfoCard("Steps ${run.stepsDone}/${run.steps.size}") {
                            for (step in run.steps) {
                                Row {
                                    Text(step.name, Modifier.weight(1f), color = Fm.colors.textPrimary)
                                    Note(step.status)
                                }
                            }
                        }
                    }
                }
                item {
                    InfoCard(if (model.fromDesktop && run.status.isActive) "Log (live)" else "Log") {
                        val lines = CiLog.lines(run.logTail)
                        if (lines.isEmpty()) Note("No log yet.")
                        SelectionContainer {
                            Column(Modifier.horizontalScroll(rememberScrollState())) {
                                for (line in lines) Mono(line)
                            }
                        }
                        if (lines.isNotEmpty()) FmToneButton("Copy log", onPressed = { copy("Log", lines.joinToString("\n")) })
                        FmInlineStatus(logStatus)
                    }
                }
            }
        }
    }
}

internal class ReleasesScreen(private val repo: Repository) : Screen() {
    private val model = RepoReleasesModel(CodeServices.ci(), CodeServices.ref(repo))
    override val title: String get() = "Releases"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        var tab by remember { mutableIntStateOf(0) }
        LaunchedEffect(Unit) {
            io { model.load() }
            tick.bump()
        }
        tick.value
        DetailScaffold("Releases · ${repo.name}", navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { io { model.load(refresh = true) }; tick.bump() } })
        }) {
            Column(Modifier.fillMaxSize()) {
                FmSegmentTabBar(listOf(FmSegmentTab("Releases", model.releases.size), FmSegmentTab("Artifacts", model.artifacts.size)), tab, { tab = it })
                Box(Modifier.weight(1f)) {
                    ScreenList {
                        when {
                            model.loading && model.releases.isEmpty() -> item { LoadingCard("Loading releases…") }
                            model.error != null -> item { ErrorCard(model.error!!, { scope.launch { io { model.load(true) }; tick.bump() } }) }
                            model.releases.isEmpty() -> item { EmptyCard("No releases yet.") }
                            tab == 0 -> items(model.releases, key = { it.tag }) { release ->
                                val announceStatus = rememberFmStatus()
                                InfoCard(release.tag) {
                                    Note(listOf(release.channel, release.shortCommit, release.createdDate, release.assetSummary).filter { it.isNotEmpty() }.joinToString(" · "))
                                    for (asset in release.assets) Kv(asset.name, "${asset.platformLabel} · ${asset.sizeLabel} · ${asset.downloadLabel}")
                                    FmToneButton("Announce on the Fediverse", onPressed = {
                                        scope.launch {
                                            val draft = ReleaseAnnouncementDraft.forRelease(release, CodeServices.ref(repo))
                                            val text = messenger.showPromptDialog(
                                                draft.title, body = "Posts this text from ${repo.fullName}'s Fediverse account.",
                                                initialValue = draft.body, confirmLabel = "Announce",
                                            ) ?: return@launch
                                            draft.body = text
                                            val identity = CodeServices.identity
                                            val sign: ((ByteArray) -> String)? = identity?.let { id -> { bytes: ByteArray -> id.sign(bytes) } }
                                            val result = io { model.announce(draft, release.tag, CodeServices.account, sign, System.currentTimeMillis()) }
                                            if (result.isNotEmpty()) announceStatus.report(result, !result.startsWith("Announced"))
                                        }
                                    }, icon = "campaign", tone = FmTone.Accent)
                                    FmInlineStatus(announceStatus)
                                }
                            }
                            else -> {
                                item { Note("${model.artifacts.size} artifacts · ${com.forkmesh.nativeapp.code.CodeMapLayout.sizeLabel(model.artifactsTotalBytes)}") }
                                items(model.artifacts, key = { it.id }) { row ->
                                    MobileCard(
                                        icon = "inventory_2_outlined",
                                        iconColor = Fm.colors.accent,
                                        title = row.asset.name,
                                        subtitle = "${row.tag} · ${row.asset.platformLabel} · ${row.asset.sizeLabel} · ${row.asset.shortSha}",
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

internal class SecurityScreen(private val repo: Repository) : Screen() {
    private val model = RepoSecurityModel(CodeServices.ci(), CodeServices.ref(repo))
    override val title: String get() = "Security"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            io { model.load() }
            tick.bump()
        }
        tick.value
        DetailScaffold("Security · ${repo.name}", navigator) {
            ScreenList {
                val scan = model.latest
                if (model.loading && scan == null) {
                    item { LoadingCard("Loading the latest scan…") }
                    return@ScreenList
                }
                model.error?.let { item { ErrorCard(it, { scope.launch { io { model.load() }; tick.bump() } }) } }
                model.scanStatus?.let { item { Note(it.label) } }
                if (scan == null) {
                    item { EmptyCard("No security scan has reported for this repository yet.") }
                    return@ScreenList
                }
                item {
                    InfoCard(scan.headline) {
                        Kv("Commit", scan.shortCommit)
                        Kv("Scanner", scan.scanner.ifEmpty { "—" })
                        Kv("Scanned", scan.scannedAt.ifEmpty { scan.receivedAt })
                        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            FmToneButton("All", onPressed = { model.severityFilter = null; tick.bump() }, filled = model.severityFilter == null)
                            for (severity in SecuritySeverity.entries) {
                                val count = scan.counts[severity] ?: 0
                                if (count > 0) FmToneButton("${severity.label} $count", onPressed = { model.severityFilter = severity; tick.bump() }, filled = model.severityFilter == severity)
                            }
                        }
                    }
                }
                if (!scan.hasDetail) item { Note("This scan reported counts only.") }
                items(model.findings, key = { it.id }) { finding ->
                    val notice = rememberFmStatus()
                    InfoCard(finding.summary.ifEmpty { finding.ruleId }) {
                        FmStatusBadge(finding.severity.label, if (finding.severity.ordinal <= 1) Fm.colors.danger else Fm.colors.warning)
                        Mono(finding.location)
                        if (finding.recommendation.isNotEmpty()) Note(finding.recommendation)
                        if (finding.advisoryIds.isNotEmpty()) Note(finding.advisoryIds.joinToString(", "))
                        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            for (status in RepoSecurityModel.TRIAGE_STATUSES) {
                                FmToneButton(status.replaceFirstChar { it.uppercase() }, onPressed = {
                                    scope.launch {
                                        if (!io { model.triage(finding, status) }) model.error?.let { notice.error(it) } else notice.clear()
                                        tick.bump()
                                    }
                                }, filled = finding.reviewStatus == status)
                            }
                        }
                        FmInlineStatus(notice)
                    }
                }
            }
        }
    }
}

internal class WebhooksScreen(private val repo: Repository) : Screen() {
    private val model = RepoWebhooksModel(CodeServices.ci(), CodeServices.ref(repo))
    override val title: String get() = "Webhooks"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val secretStatus = rememberFmStatus()
        val copy = rememberCopyText(secretStatus)
        var url by remember { mutableStateOf("") }
        var description by remember { mutableStateOf("") }
        var events by remember { mutableStateOf(setOf<String>()) }
        LaunchedEffect(Unit) {
            io { model.load() }
            tick.bump()
        }
        tick.value
        DetailScaffold("Webhooks · ${repo.name}", navigator) {
            ScreenList {
                model.error?.let { item { Note(it, Fm.colors.danger) } }
                model.revealedSecret?.let { (id, secret) ->
                    item {
                        InfoCard("Signing secret") {
                            Note("Copy it now. It is shown once, for webhook $id.")
                            Mono(secret)
                            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                FmToneButton("Copy", onPressed = { copy("Secret", secret) })
                                FmToneButton("Done", onPressed = { model.dismissSecret(); secretStatus.clear(); tick.bump() })
                            }
                            FmInlineStatus(secretStatus)
                        }
                    }
                }
                if (model.loading && model.webhooks.isEmpty()) item { LoadingCard("Loading webhooks…") }
                items(model.webhooks, key = { it.id }) { hook ->
                    val hookStatus = rememberFmStatus()
                    InfoCard(hook.url) {
                        Note(hook.healthLabel + if (hook.description.isEmpty()) "" else " · ${hook.description}")
                        Note(hook.events.joinToString(", "))
                        for (delivery in model.deliveries(hook.id).take(5)) Note("${delivery.eventType} · ${delivery.status} · ${delivery.outcome}")
                        if (!hook.managed) {
                            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                FmToneButton("Test", onPressed = { scope.launch { val result = io { model.test(hook.id) }; hookStatus.report(result, !result.startsWith("Test delivered")); tick.bump() } })
                                FmToneButton(if (hook.active) "Pause" else "Resume", onPressed = { scope.launch { io { model.setActive(hook.id, !hook.active) }; tick.bump() } })
                                FmToneButton("Rotate secret", onPressed = { scope.launch { io { model.rotate(hook.id) }; tick.bump() } })
                                FmToneButton("Delete", onPressed = {
                                    scope.launch {
                                        if (messenger.confirmAction("Delete this webhook?", "Delete")) {
                                            io { model.delete(hook.id) }
                                            tick.bump()
                                        }
                                    }
                                }, tone = FmTone.Danger)
                            }
                            FmInlineStatus(hookStatus)
                        }
                    }
                }
                item {
                    InfoCard("Add a webhook") {
                        if (!model.canAdd) Note("This repository has the most webhooks it can have (${model.maxWebhooks}).")
                        FmTextField(url, { url = it }, hint = "https://example.com/hook")
                        FmTextField(description, { description = it }, hint = "Description (optional)")
                        for (type in model.eventTypes) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) {
                                    Text(type.label, color = Fm.colors.textPrimary)
                                    if (type.hint.isNotEmpty()) Note(type.hint)
                                }
                                FmSwitch(checked = type.type in events, onCheckedChange = { on -> events = if (on) events + type.type else events - type.type })
                            }
                        }
                        FmToneButton("Add webhook", onPressed = {
                            scope.launch {
                                if (io { model.create(RepoWebhookDraft(url, events, description)) }) {
                                    url = ""
                                    description = ""
                                    events = emptySet()
                                }
                                tick.bump()
                            }
                        }, tone = FmTone.Accent, filled = true)
                    }
                }
            }
        }
    }
}

internal class MirrorNodesScreen(private val repo: Repository) : Screen() {
    private val model = RepoMirrorsModel(CodeServices.ci(), CodeServices.ref(repo))
    override val title: String get() = "Mirror nodes"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            io { model.load() }
            tick.bump()
        }
        tick.value
        DetailScaffold("Mirror nodes · ${repo.name}", navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { io { model.load() }; tick.bump() } })
        }) {
            ScreenList {
                if (model.syncLabel.isNotEmpty()) {
                    item {
                        InfoCard(model.syncLabel) {
                            Note(model.syncSummary)
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Text("Online only", Modifier.weight(1f), color = Fm.colors.textPrimary)
                                FmSwitch(checked = model.onlineOnly, onCheckedChange = { model.onlineOnly = it; tick.bump() })
                            }
                        }
                    }
                }
                model.error?.let { item { ErrorCard(it, { scope.launch { io { model.load() }; tick.bump() } }) } }
                if (model.loading && model.nodes.isEmpty()) item { LoadingCard("Loading mirror nodes…") }
                items(model.shown, key = { it.id }) { node ->
                    val color = when (node.light) {
                        "current" -> Fm.colors.success
                        "behind" -> Fm.colors.warning
                        "offline" -> Fm.colors.textTertiary
                        else -> Fm.colors.accent
                    }
                    MobileCard(
                        icon = "dns_outlined",
                        iconColor = color,
                        title = node.machineName.ifEmpty { node.node },
                        subtitle = node.subtitle,
                        trailing = { FmStatusBadge(node.light, color) },
                    )
                }
            }
        }
    }
}

@Composable
internal fun ChecksSummaryCard(repo: Repository, pull: Int, navigator: Navigator) {
    val model = remember(repo.fullName) { actionsModel(repo) }
    val tick = rememberTick()
    model.BumpOnChange(tick)
    DisposableEffect(model) { onDispose { model.stop() } }
    LaunchedEffect(repo.fullName) {
        io { model.load() }
        tick.bump()
    }
    tick.value
    val checks = model.checks(pull)
    InfoCard("Checks") {
        Note(checks.label)
        for (run in checks.runs) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(run.displayName, Modifier.weight(1f), color = Fm.colors.textPrimary)
                FmStatusBadge(run.status.label, runColor(run.status))
            }
        }
        if (checks.runs.isNotEmpty()) FmToneButton("Open Actions", onPressed = { navigator.push(ActionsScreen(repo)) })
    }
}
