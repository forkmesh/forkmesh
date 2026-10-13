package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.ops.AgentListSort
import com.forkmesh.nativeapp.agents.ops.AgentUsageModel
import com.forkmesh.nativeapp.agents.ops.BulkActionsModel
import com.forkmesh.nativeapp.agents.ops.BulkAgentAction
import com.forkmesh.nativeapp.agents.ops.BulkCandidate
import com.forkmesh.nativeapp.agents.ops.MergeCleanupModel
import com.forkmesh.nativeapp.agents.ops.MergeStats
import com.forkmesh.nativeapp.agents.ops.MergeVelocityWindow
import com.forkmesh.nativeapp.agents.ops.UsageFormat
import com.forkmesh.nativeapp.agents.ops.UsageProviderMeter
import com.forkmesh.nativeapp.agents.ops.UsageState
import com.forkmesh.nativeapp.code.ConflictResolution
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

@Composable
internal fun AgentSortMenu(sort: AgentListSort, onChange: (AgentListSort) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        FmIconButton("sort", "Sort agents: ${sort.title}", onPressed = { open = true }, color = Fm.colors.textPrimary)
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (order in AgentListSort.entries) {
                FmDropdownMenuItem(
                    text = order.title,
                    icon = if (order == sort) "check" else null,
                    onClick = {
                        open = false
                        onChange(order)
                    },
                )
            }
        }
    }
}

@Composable
internal fun BulkAgentActionsBar(candidates: List<BulkCandidate>, onFinished: (String) -> Unit) {
    val c = Fm.colors
    val model = remember { BulkActionsModel(AgentOpsFeature.client) }
    val tick = rememberOpsTick(model)
    val scope = rememberCoroutineScope()
    tick.let { }
    val canStop = model.eligible(BulkAgentAction.Stop, candidates).isNotEmpty()
    val canContinue = model.eligible(BulkAgentAction.ContinueRun, candidates).isNotEmpty()
    if (!canStop && !canContinue && model.errorText.isEmpty()) return
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        ChipRow {
            if (canStop) {
                FmToneButton("Stop all", onPressed = { scope.launch { opsIo { model.prepare(BulkAgentAction.Stop, candidates) } } }, icon = "stop_circle_outlined", tone = FmTone.Danger)
            }
            if (canContinue) {
                FmToneButton("Continue all", onPressed = { scope.launch { opsIo { model.prepare(BulkAgentAction.ContinueRun, candidates) } } }, icon = "play_arrow", tone = FmTone.Accent)
            }
        }
        if (model.errorText.isNotEmpty()) Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
    }
    val confirmation = model.confirmation
    if (confirmation != null) {
        FmConfirmDialog(
            title = model.pendingAction?.title ?: "Confirm",
            body = confirmation.summary,
            confirmLabel = model.pendingAction?.title ?: "Confirm",
            destructive = model.pendingAction == BulkAgentAction.Stop,
            busy = model.running,
            onConfirm = {
                scope.launch {
                    val outcome = opsIo { model.run() }
                    if (outcome != null) onFinished(outcome.message)
                }
            },
            onDismissRequest = { model.cancel() },
        )
    }
}

@Composable
internal fun AgentUsageCard(navigator: Navigator? = null) {
    val c = Fm.colors
    val context = LocalContext.current
    val model = remember { AgentUsageModel(AgentOpsFeature.client) }
    val tick = rememberOpsTick(model)
    LaunchedEffect(model) {
        opsIo { model.refresh() }
        AgentOpsFeature.scheduleUsageReminders(context.applicationContext, model.reminders, model.nowMs)
    }
    val runnerAccounts by RunnerAccountsFeature.state.collectAsState()
    tick.let { }
    val summary = model.summary ?: return
    if (summary.providers.isEmpty() && summary.totalSpendUsd == null) return
    FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmSectionHeader("Usage", icon = "speed", trailing = summary.totalSpendUsd?.let { spend ->
                { Text(UsageFormat.usd(spend), style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
            })
            for (provider in summary.providers) UsageProviderBlock(provider, model.nowMs)
            if (model.errorText.isNotEmpty()) Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
            if (navigator != null && runnerAccounts.hasAccounts) {
                FmToneButton("Runner accounts", onPressed = { RunnerAccountsFeature.open(navigator) }, icon = "manage_accounts")
            }
        }
    }
}

@Composable
private fun UsageProviderBlock(provider: UsageProviderMeter, nowMs: Long) {
    val c = Fm.colors
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        val account = provider.activeAccount?.label
        Text(
            listOfNotNull(provider.label, account).joinToString(" · "),
            style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis),
        )
        for (window in provider.windows) {
            val tone = when (window.state) {
                UsageState.Exhausted -> c.danger
                UsageState.High -> c.warning
                UsageState.Ok -> c.success
                UsageState.Unknown -> c.textTertiary
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(window.label, style = Fm.text.bodySmall.copy(color = c.textSecondary), modifier = Modifier.width(88.dp))
                Box(Modifier.weight(1f).height(6.dp).clip(RoundedCornerShape(FmRadius.full)).background(c.bgOverlay)) {
                    val used = (window.usedPercent ?: 0) / 100f
                    Box(Modifier.fillMaxWidth(used).height(6.dp).background(tone))
                }
                Spacer(Modifier.width(FmSpace.x2))
                Text(
                    listOf(window.usedPercent?.let { "$it%" } ?: "—", window.resetLabel(nowMs)).filter { it.isNotEmpty() }.joinToString(" · "),
                    style = Fm.text.labelSmall.copy(color = c.textSecondary),
                )
            }
        }
        provider.credits?.let { Text(it.label, style = Fm.text.labelSmall.copy(color = c.textSecondary)) }
    }
}

@Composable
internal fun DesktopAgentOps(navigator: Navigator, onFinished: (String) -> Unit, candidates: List<BulkCandidate>) {
    val c = Fm.colors
    val reach = AgentsFeature.hub.desktop
    if (reach == null) return
    val queue = remember { AgentOpsFeature.approvals }
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        AgentApprovalsSection(navigator, queue)
        if (candidates.isNotEmpty()) BulkAgentActionsBar(candidates, onFinished)
        AgentUsageCard(navigator)
        ChipRow {
            FmOutlinedButton("Needs you", onClick = { navigator.push(AgentOpsFeature.needsYouScreen()) }, icon = "back_hand")
            FmOutlinedButton("Saved permissions", onClick = { navigator.push(AgentOpsFeature.grantsScreen()) }, icon = "verified_user_outlined")
            FmOutlinedButton("Recycle bin", onClick = { navigator.push(AgentOpsFeature.recycleBinScreen()) }, icon = "restore_from_trash_outlined")
        }
        if (queue.errorText.isNotEmpty()) Text(queue.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
    }
}

@Composable
internal fun DesktopSessionOpsCard(navigator: Navigator, session: AgentSession, repo: Repository) {
    if (AgentsFeature.hub.desktop == null) return
    val c = Fm.colors
    val queue = remember { AgentOpsFeature.approvals }
    val queueTick = rememberOpsTick(queue)
    val controls = remember { BulkActionsModel(AgentOpsFeature.client) }
    val controlsTick = rememberOpsTick(controls)
    val scope = rememberCoroutineScope()
    var showMerge by remember { mutableStateOf(false) }
    var note by remember { mutableStateOf("") }
    queueTick.let { }
    controlsTick.let { }
    val candidate = BulkCandidate(session.id, session.isActive, session.isActive, session.merged)
    val canMerge = session.branchName.isNotEmpty() && !session.merged && !session.isActive
    fun begin(action: BulkAgentAction) {
        scope.launch { opsIo { controls.prepare(action, listOf(candidate)) } }
    }
    FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text("On the desktop", style = Fm.text.titleSmall.copy(color = c.textPrimary))
            for (approval in queue.approvals(session.id)) {
                ApprovalRow(approval) { navigator.push(ApprovalScreen(approval.id)) }
            }
            ChipRow {
                if (candidate.allows(BulkAgentAction.Stop)) {
                    FmToneButton("Stop", onPressed = { begin(BulkAgentAction.Stop) }, icon = "stop_circle_outlined", tone = FmTone.Danger)
                }
                if (candidate.allows(BulkAgentAction.ContinueRun)) {
                    FmToneButton("Continue", onPressed = { begin(BulkAgentAction.ContinueRun) }, icon = "play_arrow", tone = FmTone.Accent)
                }
                if (canMerge) {
                    FmToneButton("Merge & clean up", onPressed = { showMerge = true }, icon = "call_merge", tone = FmTone.Success)
                }
                FmOutlinedButton("Worktrees", onClick = { navigator.push(WorktreesScreen(repo.owner, repo.name)) }, icon = "account_tree_outlined")
            }
            if (note.isNotEmpty()) Text(note, style = Fm.text.bodySmall.copy(color = c.textSecondary))
            if (controls.errorText.isNotEmpty()) Text(controls.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
        }
    }
    val confirmation = controls.confirmation
    if (confirmation != null) {
        FmConfirmDialog(
            title = controls.pendingAction?.title ?: "Confirm",
            body = confirmation.summary,
            confirmLabel = controls.pendingAction?.title ?: "Confirm",
            destructive = controls.pendingAction == BulkAgentAction.Stop,
            busy = controls.running,
            onConfirm = { scope.launch { opsIo { controls.run() }?.let { note = it.message } } },
            onDismissRequest = { controls.cancel() },
        )
    }
    if (showMerge) {
        MergeCleanupDialog(session.id, session.diffBase, onDone = { message -> note = message }, onDismiss = { showMerge = false })
    }
}

@Composable
internal fun MergeCleanupDialog(sessionId: Int, base: String, onDone: (String) -> Unit, onDismiss: () -> Unit) {
    val c = Fm.colors
    val model = remember(sessionId) { MergeCleanupModel(AgentOpsFeature.client, sessionId) }
    val tick = rememberOpsTick(model)
    val scope = rememberCoroutineScope()
    LaunchedEffect(model) { opsIo { model.mergeNow() } }
    tick.let { }
    val outcome = model.outcome
    if (outcome != null) {
        val close = {
            onDone(outcome.message())
            onDismiss()
        }
        val base = outcome.stats?.base.orEmpty()
        FmConfirmDialog(
            title = "Merged!",
            body = if (base.isEmpty()) "Merged ${outcome.branch} and cleaned up." else "${outcome.branch} is part of $base now.",
            confirmLabel = "Done",
            hideCancel = true,
            onConfirm = close,
            onDismissRequest = close,
            fields = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    outcome.stats?.let { MergeStatsBlock(it) }
                    Spacer(Modifier.height(FmSpace.x3))
                    TodayActivityBlock()
                }
            },
        )
        return
    }
    if (model.conflicted) {
        MergeConflictFixDialog(model.errorText, onFix = { AgentsFeature.control.fixDesktopConflicts(sessionId, base) }, onDone = onDone, onDismiss = onDismiss)
        return
    }
    val failed = model.errorText.isNotEmpty() && !model.busy
    FmConfirmDialog(
        title = "Merge & clean up",
        body = if (failed) null else "Merging and cleaning up on the desktop…",
        confirmLabel = "Try again",
        busy = !failed,
        onConfirm = { scope.launch { opsIo { model.mergeNow() } } },
        onDismissRequest = onDismiss,
        fields = {
            if (model.errorText.isNotEmpty()) Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
        },
    )
}

@Composable
internal fun MergeConflictFixDialog(message: String, onFix: () -> Unit, onDone: (String) -> Unit, onDismiss: () -> Unit) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    var sending by remember { mutableStateOf(false) }
    var errorText by remember { mutableStateOf("") }
    FmConfirmDialog(
        title = "Merge conflicts",
        body = message,
        confirmLabel = "Fix with agent",
        busy = sending,
        onConfirm = {
            if (!sending) {
                sending = true
                errorText = ""
                scope.launch {
                    try {
                        opsIo { onFix() }
                        onDone(ConflictResolution.FIX_SENT)
                        onDismiss()
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (error: Exception) {
                        errorText = controlError(error)
                    } finally {
                        sending = false
                    }
                }
            }
        },
        onDismissRequest = onDismiss,
        fields = {
            if (errorText.isNotEmpty()) Text(errorText, style = Fm.text.bodySmall.copy(color = c.danger))
        },
    )
}

@Composable
internal fun MergeStatsBlock(stats: MergeStats) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth().padding(top = FmSpace.x3), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        if (stats.hasLanding) {
            Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text("LANDED", modifier = Modifier.weight(1f), style = Fm.text.labelSmall.copy(color = c.textTertiary))
                    Text(stats.shortCommit, style = Fm.text.bodySmall.copy(color = c.textTertiary, fontFamily = FontFamily.Monospace))
                }
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                    stats.commits?.takeIf { it > 0 }?.let { MergeFigure("$it", if (it == 1) "commit" else "commits") }
                    MergeFigure("${stats.files}", if (stats.files == 1) "file" else "files")
                    Spacer(Modifier.weight(1f))
                    Text(stats.insertionsText, modifier = Modifier.alignByBaseline(), style = Fm.text.titleSmall.copy(color = c.success))
                    Text(stats.deletionsText, modifier = Modifier.alignByBaseline(), style = Fm.text.titleSmall.copy(color = c.danger))
                }
                stats.insertionShare?.let { MergeChurnBar(it) }
            }
        }
        if (stats.velocity.isNotEmpty()) {
            Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text("MERGE VELOCITY", modifier = Modifier.weight(1f), style = Fm.text.labelSmall.copy(color = c.textTertiary))
                    if (stats.base.isNotEmpty()) {
                        Text("into ${stats.base}", maxLines = 1, style = Fm.text.labelSmall.copy(color = c.textTertiary))
                    }
                }
                for (window in stats.velocity) MergeVelocityRow(window)
            }
        }
    }
}

@Composable
private fun RowScope.MergeFigure(value: String, unit: String) {
    val c = Fm.colors
    Text(value, modifier = Modifier.alignByBaseline(), style = Fm.text.titleSmall.copy(color = c.textPrimary))
    Text(unit, modifier = Modifier.alignByBaseline().padding(start = 3.dp), style = Fm.text.bodySmall.copy(color = c.textSecondary))
}

@Composable
private fun MergeChurnBar(share: Float) {
    val c = Fm.colors
    Canvas(Modifier.fillMaxWidth().height(6.dp)) {
        val height = size.height
        val radius = CornerRadius(height / 2, height / 2)
        val both = share > 0f && share < 1f
        val gap = if (both) 2.dp.toPx() else 0f
        val room = size.width - gap
        val floor = minOf(4.dp.toPx(), room / 2)
        val added = when {
            share <= 0f -> 0f
            share >= 1f -> room
            else -> (room * share).coerceIn(floor, room - floor)
        }
        val removed = if (share >= 1f) 0f else room - added
        if (added > 0f) drawRoundRect(c.success, Offset.Zero, Size(added, height), radius)
        if (removed > 0f) drawRoundRect(c.danger, Offset(size.width - removed, 0f), Size(removed, height), radius)
    }
}

@Composable
private fun MergeVelocityRow(window: MergeVelocityWindow) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().clearAndSetSemantics { contentDescription = window.spoken },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(window.label, modifier = Modifier.width(64.dp), style = Fm.text.bodySmall.copy(color = c.textSecondary))
        MergeVelocityChart(window, Modifier.weight(1f).height(26.dp))
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.widthIn(min = 72.dp), horizontalAlignment = Alignment.End) {
            Text("${window.count}", style = Fm.text.titleSmall.copy(color = c.textPrimary))
            Text(window.detail, maxLines = 1, style = Fm.text.labelSmall.copy(color = c.textTertiary))
        }
    }
}

@Composable
private fun MergeVelocityChart(window: MergeVelocityWindow, modifier: Modifier) {
    val c = Fm.colors
    Canvas(modifier) {
        val series = window.series
        if (series.isEmpty()) {
            val height = 6.dp.toPx()
            val top = (size.height - height) / 2
            val radius = CornerRadius(height / 2, height / 2)
            drawRoundRect(c.bgOverlay, Offset(0f, top), Size(size.width, height), radius)
            if (window.share > 0f) {
                drawRoundRect(c.success, Offset(0f, top), Size(maxOf(size.width * window.share, height), height), radius)
            }
            if (window.avgShare > 0f) {
                val tick = 2.dp.toPx()
                val x = (size.width * window.avgShare - tick / 2).coerceIn(0f, size.width - tick)
                val reach = 4.dp.toPx()
                drawRoundRect(c.textSecondary, Offset(x, top - reach), Size(tick, height + reach * 2), CornerRadius(tick / 2, tick / 2))
            }
            return@Canvas
        }
        val count = series.size
        val gap = (if (count > 30) 1 else 2).dp.toPx()
        val width = maxOf((size.width - gap * (count - 1)) / count, 1f)
        val peak = maxOf(window.peak, 1).toFloat()
        val line = 1.dp.toPx()
        drawRect(c.border, Offset(0f, size.height - line), Size(size.width, line))
        series.forEachIndexed { index, value ->
            if (value <= 0) return@forEachIndexed
            val height = maxOf(size.height * value / peak, 3.dp.toPx())
            val x = index * (width + gap)
            val radius = minOf(2.dp.toPx(), width / 2)
            val color = if (index == count - 1) c.success else c.success.copy(alpha = 0.55f)
            val bar = Path().apply {
                addRoundRect(
                    RoundRect(
                        Rect(x, size.height - height, x + width, size.height),
                        topLeft = CornerRadius(radius, radius),
                        topRight = CornerRadius(radius, radius),
                    ),
                )
            }
            drawPath(bar, color)
        }
    }
}
