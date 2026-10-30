package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.ops.AgentListFilter
import com.forkmesh.nativeapp.agents.ops.AgentListItem
import com.forkmesh.nativeapp.agents.ops.AgentListOrdering
import com.forkmesh.nativeapp.agents.ops.AgentListSort
import com.forkmesh.nativeapp.agents.ops.AgentOpsConfirmation
import com.forkmesh.nativeapp.agents.ops.ApprovalLifetime
import com.forkmesh.nativeapp.agents.ops.ApprovalsQueue
import com.forkmesh.nativeapp.agents.ops.BulkCandidate
import com.forkmesh.nativeapp.agents.ops.CapabilityGrant
import com.forkmesh.nativeapp.agents.ops.CapabilityGrantsModel
import com.forkmesh.nativeapp.agents.ops.MergeLeaderboardModel
import com.forkmesh.nativeapp.agents.ops.OpsState
import com.forkmesh.nativeapp.agents.ops.PendingApproval
import com.forkmesh.nativeapp.agents.ops.WorktreesModel
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun rememberOpsTick(state: OpsState): Int {
    var tick by remember { mutableIntStateOf(0) }
    DisposableEffect(state) {
        val listener: () -> Unit = { tick++ }
        state.addListener(listener)
        onDispose { state.removeListener(listener) }
    }
    return tick
}

internal suspend fun <T> opsIo(block: () -> T): T = withContext(Dispatchers.IO) { block() }

@Composable
internal fun ApprovalRow(approval: PendingApproval, onTap: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .clickable(onClick = onTap)
            .padding(vertical = FmSpace.x2),
        verticalAlignment = Alignment.Top,
    ) {
        FmIcon(approval.kind.icon, size = 20.dp, tint = c.warning)
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(
                approval.headline,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis),
            )
            if (approval.subtitle.isNotEmpty()) {
                Text(approval.subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
        }
        FmIcon("chevron_right", size = 18.dp, tint = c.textDisabled)
    }
}

@Composable
internal fun AgentApprovalsSection(navigator: Navigator, queue: ApprovalsQueue) {
    val tick = rememberOpsTick(queue)
    LaunchedEffect(queue) { opsIo { queue.refresh() } }
    tick.let { }
    if (queue.isEmpty) return
    FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            FmSectionHeader("Needs you", count = queue.count)
            for (approval in queue.approvals) {
                ApprovalRow(approval) { navigator.push(ApprovalScreen(approval.id)) }
            }
        }
    }
}

internal class ApprovalScreen(private val approvalId: String) : Screen() {
    override val title: String get() = "Approval"
    private var lifetime by mutableStateOf(ApprovalLifetime.Once)
    private var working by mutableStateOf(false)
    private var outcome by mutableStateOf<String?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val queue = remember { AgentOpsFeature.approvals }
        val tick = rememberOpsTick(queue)
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        LaunchedEffect(queue) {
            queue.start()
            if (queue.approval(approvalId) == null) opsIo { queue.refresh() }
        }
        fun answer(allow: Boolean) {
            scope.launch {
                if (allow && !AgentOpsFeature.authenticate(messenger, "Approve the agent's request")) return@launch
                working = true
                val ok = opsIo { if (allow) queue.approve(approvalId, lifetime) else queue.deny(approvalId) }
                working = false
                if (ok) outcome = if (allow) "Approved" else "Denied"
            }
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { opsIo { queue.refresh() } } }, color = c.textPrimary)
        }) {
            tick.let { }
            val approval = queue.approval(approvalId)
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                when {
                    approval != null -> {
                        item {
                            FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                    Row(verticalAlignment = Alignment.CenterVertically) {
                                        FmIcon(approval.kind.icon, size = 20.dp, tint = c.warning)
                                        Spacer(Modifier.width(FmSpace.x2))
                                        Text(approval.kind.title, style = Fm.text.titleMedium.copy(color = c.textPrimary))
                                    }
                                    if (approval.subtitle.isNotEmpty()) Text(approval.subtitle, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                                }
                            }
                        }
                        item { FmSectionHeader("Request") }
                        item {
                            FmCard(Modifier.fillMaxWidth()) {
                                SelectionContainer {
                                    if (approval.command.isEmpty()) {
                                        Text(approval.question, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                                    } else {
                                        Text(approval.command, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontFamily = FmType.mono))
                                    }
                                }
                            }
                        }
                        item { FmSectionHeader("How long") }
                        items(approval.lifetimes, key = { it.wire }) { choice ->
                            LifetimeRow(choice, selected = choice == lifetime) { lifetime = choice }
                        }
                        item { Text(lifetime.detail, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                        item {
                            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                                FmFilledButton(lifetime.title, onClick = if (working) null else ({ answer(true) }), icon = "check")
                                FmOutlinedButton("Deny", onClick = if (working) null else ({ answer(false) }), icon = "block")
                                if (working) CircularProgressIndicator(Modifier.padding(start = FmSpace.x2), strokeWidth = 2.dp)
                            }
                        }
                        item {
                            Text(
                                "The desktop checks a fresh one-use confirmation and writes the answer to its log.",
                                style = Fm.text.bodySmall.copy(color = c.textTertiary),
                            )
                        }
                    }
                    outcome != null -> item {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            FmIcon("check_circle_outline", size = 20.dp, tint = c.success)
                            Spacer(Modifier.width(FmSpace.x2))
                            Text(outcome ?: "", style = Fm.text.titleSmall.copy(color = c.success))
                        }
                    }
                    queue.loading -> item {
                        Box(Modifier.fillMaxWidth().padding(FmSpace.x5), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
                    }
                    else -> item {
                        Box(Modifier.fillMaxWidth().height(260.dp)) {
                            FmEmptyState(
                                icon = "check_circle_outline",
                                title = "Nothing to answer",
                                message = "This request was answered, or the agent stopped waiting.",
                            )
                        }
                    }
                }
                if (queue.errorText.isNotEmpty()) {
                    item { Text(queue.errorText, style = Fm.text.bodySmall.copy(color = c.danger)) }
                }
            }
        }
    }
}

@Composable
private fun LifetimeRow(choice: ApprovalLifetime, selected: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .clickable(onClick = onTap)
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(if (selected) "radio_button_checked" else "radio_button_unchecked", size = 20.dp, tint = if (selected) c.accent else c.textTertiary)
        Spacer(Modifier.width(FmSpace.x3))
        Text(choice.title, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
    }
}

internal class CapabilityGrantsScreen : Screen() {
    override val title: String get() = "Saved permissions"
    private var pending by mutableStateOf<Pair<CapabilityGrant, AgentOpsConfirmation>?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val model = remember { CapabilityGrantsModel(AgentOpsFeature.client) }
        val tick = rememberOpsTick(model)
        val scope = rememberCoroutineScope()
        LaunchedEffect(model) { opsIo { model.refresh() } }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { opsIo { model.refresh() } } }, color = c.textPrimary)
        }) {
            tick.let { }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                if (model.errorText.isNotEmpty()) item { Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger)) }
                if (model.grants.isEmpty() && !model.loading) {
                    item {
                        Box(Modifier.fillMaxWidth().height(260.dp)) {
                            FmEmptyState(
                                icon = "verified_user_outlined",
                                title = "No saved permissions",
                                message = "\"Always allow\" answers you give an agent are saved on the desktop and listed here.",
                            )
                        }
                    }
                }
                items(model.grants, key = { it.id }) { grant ->
                    FmCard(Modifier.fillMaxWidth()) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            FmIcon(grant.kind.icon, size = 20.dp, tint = if (grant.allows) c.success else c.danger)
                            Spacer(Modifier.width(FmSpace.x3))
                            Column(Modifier.weight(1f)) {
                                Text(grant.capability, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis))
                                val detail = listOf(grant.resource, grant.worktree, grant.lifetime).filter { it.isNotEmpty() }.joinToString(" · ")
                                if (detail.isNotEmpty()) Text(detail, style = Fm.text.bodySmall.copy(color = c.textSecondary), maxLines = 2)
                            }
                            FmStatusBadge(if (grant.allows) "Allow" else "Deny", if (grant.allows) c.success else c.danger)
                            Spacer(Modifier.width(FmSpace.x2))
                            FmOutlinedButton(
                                "Revoke",
                                onClick = if (grant.id in model.revoking) null else ({
                                    scope.launch {
                                        val confirmation = opsIo { model.prepareRevoke(grant) }
                                        if (confirmation != null) pending = grant to confirmation
                                    }
                                }),
                            )
                        }
                    }
                }
            }
            pending?.let { (grant, confirmation) ->
                FmConfirmDialog(
                    title = "Revoke",
                    body = confirmation.summary,
                    confirmLabel = "Revoke",
                    destructive = true,
                    onConfirm = {
                        pending = null
                        scope.launch { opsIo { model.revoke(grant, confirmation) } }
                    },
                    onDismissRequest = { pending = null },
                )
            }
        }
    }
}

internal class MergeLeaderboardScreen : Screen() {
    override val title: String get() = "Leaderboard"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val model = remember { MergeLeaderboardModel(AgentOpsFeature.client) }
        val tick = rememberOpsTick(model)
        val scope = rememberCoroutineScope()
        LaunchedEffect(model) { opsIo { model.refresh() } }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { opsIo { model.refresh() } } }, color = c.textPrimary)
        }) {
            tick.let { }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                item {
                    Column {
                        Text("Merge leaderboard", style = Fm.text.titleSmall.copy(color = c.textPrimary))
                        if (model.board.subtitle.isNotEmpty()) Text(model.board.subtitle, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        if (model.errorText.isNotEmpty()) Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
                    }
                }
                if (!model.loaded) {
                    item { CircularProgressIndicator() }
                }
                model.board.stats?.let { stats ->
                    item { FmCard(Modifier.fillMaxWidth()) { MergeStatsBlock(stats) } }
                }
                if (model.loaded && model.errorText.isEmpty()) {
                    item { FmCard(Modifier.fillMaxWidth()) { TodayActivityBlock() } }
                }
            }
        }
    }
}

internal class WorktreesScreen(private val owner: String, private val name: String, private val desktopKey: String = "") : Screen() {
    override val title: String get() = "Worktrees"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val model = remember { WorktreesModel(AgentOpsFeature.client(desktopKey), owner, name) }
        val tick = rememberOpsTick(model)
        val scope = rememberCoroutineScope()
        LaunchedEffect(model) { opsIo { model.refresh() } }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { opsIo { model.refresh() } } }, color = c.textPrimary)
        }) {
            tick.let { }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                item {
                    Column {
                        Text("$owner/$name", style = Fm.text.titleSmall.copy(color = c.textPrimary))
                        if (model.list.base.isNotEmpty()) Text("Base branch ${model.list.base}", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        if (model.errorText.isNotEmpty()) Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
                    }
                }
                if (model.loaded && model.list.worktrees.isEmpty() && model.errorText.isEmpty()) {
                    item { Text("The desktop has no worktrees for this repository.", style = Fm.text.bodyMedium.copy(color = c.textSecondary)) }
                }
                items(model.list.worktrees, key = { it.id }) { worktree ->
                    FmCard(Modifier.fillMaxWidth()) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            FmIcon(if (worktree.isMain) "folder_outlined" else "account_tree_outlined", size = 20.dp, tint = c.accent)
                            Spacer(Modifier.width(FmSpace.x3))
                            Column(Modifier.weight(1f)) {
                                Text(worktree.branch.ifEmpty { worktree.name }, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis))
                                val detail = listOfNotNull(
                                    worktree.syncLabel,
                                    worktree.sessionId?.let { "Agent #$it" },
                                    if (worktree.locked) "Locked" else null,
                                    if (worktree.prunable) "Prunable" else null,
                                ).joinToString(" · ")
                                Text(detail, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                            }
                        }
                    }
                }
            }
        }
    }
}

internal class DesktopAgentsScreen(initialFilter: AgentListFilter) : Screen() {
    private var filter by mutableStateOf(initialFilter)
    private var sort by mutableStateOf(AgentListSort.Status)
    override val title: String get() = if (filter == AgentListFilter.NeedsYou) "Needs you" else "Desktop agents"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val center = AgentsFeature.control
        val queue = remember { AgentOpsFeature.approvals }
        val tick = rememberCenterTick(center)
        val queueTick = rememberOpsTick(queue)
        LaunchedEffect(center) { center.refreshAsync() }
        DetailScaffold(title, navigator, actions = {
            AgentSortMenu(sort) { sort = it }
            FmIconButton("refresh", "Refresh", onPressed = { center.refreshAsync() }, color = c.textPrimary)
        }) {
            tick.let { }
            queueTick.let { }
            val snapshot = center.desktopSnapshot
            val link = center.hub.currentLink
            val needing = queue.sessionIdsNeedingYou
            val rows = snapshot?.sessions.orEmpty()
            val ordered = AgentListOrdering.sorted(
                rows.filter { AgentListOrdering.matches(AgentListItem(it.session, it.title, needsYou = it.session.id in needing), filter, System.currentTimeMillis()) },
                sort,
                System.currentTimeMillis(),
            ) { AgentListItem(it.session, it.title, needsYou = it.session.id in needing) }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                item {
                    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                        Text(center.hub.reach.label, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        if (center.desktopNote.isNotEmpty()) Text(center.desktopNote, style = Fm.text.bodySmall.copy(color = c.warning))
                        FmInlineStatus(agentsNotice)
                        AgentFilterChips(filter) { filter = it }
                    }
                }
                item { AgentApprovalsSection(navigator, queue) }
                if (rows.isNotEmpty()) {
                    item {
                        BulkAgentActionsBar(
                            rows.map { BulkCandidate(it.session.id, it.session.isActive, it.stoppable, it.session.merged) },
                        ) { center.refreshAsync() }
                    }
                }
                if (ordered.isEmpty() && !center.loading) {
                    item {
                        Box(Modifier.fillMaxWidth().height(220.dp)) {
                            FmEmptyState(
                                icon = "desktop_windows_outlined",
                                title = if (filter == AgentListFilter.NeedsYou) "Nothing needs you" else "No desktop agents",
                                message = "Pair this phone with your desktop to see and manage its agents.",
                            )
                        }
                    }
                }
                items(ordered, key = { it.session.id }) { row ->
                    AgentTwoLineRow(
                        icon = "desktop_windows_outlined",
                        title = row.title,
                        detail = listOf(row.repoLabel, row.session.providerLabel, row.session.model).filter { it.isNotEmpty() }.joinToString(" · "),
                        status = {
                            if (row.session.id in needing) FmStatusBadge("Needs you", c.warning) else AgentStatusBadge(row.session)
                        },
                        onTap = {
                            if (link != null) navigator.push(UnifiedAgentScreen(UnifiedAgentRow.fromDesktop(row, link.transport, link.desktopName)))
                        },
                    )
                }
            }
        }
    }
}

@Composable
internal fun AgentFilterChips(selected: AgentListFilter, onSelect: (AgentListFilter) -> Unit) {
    ChipRow {
        for (choice in listOf(AgentListFilter.All, AgentListFilter.NeedsYou, AgentListFilter.Running, AgentListFilter.Queued, AgentListFilter.Recent)) {
            val c = Fm.colors
            FmStatusBadge(
                choice.title,
                if (choice == selected) c.accent else c.textSecondary,
                Modifier.clip(RoundedCornerShape(FmRadius.full)).clickable { onSelect(choice) },
            )
        }
    }
}
