package com.forkmesh.nativeapp.feature.workflows

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.agents.opsIo
import com.forkmesh.nativeapp.feature.agents.rememberOpsTick
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmGroupHeader
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmType
import com.forkmesh.nativeapp.workflows.DesktopWorkflowConfirmation
import com.forkmesh.nativeapp.workflows.DesktopWorkflowDetail
import com.forkmesh.nativeapp.workflows.DesktopWorkflowRow
import com.forkmesh.nativeapp.workflows.DesktopWorkflowRun
import com.forkmesh.nativeapp.workflows.DesktopWorkflowsStore
import com.forkmesh.nativeapp.workflows.DesktopWorkflowsText
import com.forkmesh.nativeapp.workflows.WorkflowsText
import kotlinx.coroutines.launch
import java.text.DateFormat
import java.util.Date

internal object DesktopWorkflowLook {
    fun icon(row: DesktopWorkflowRow) = if (row.specialAgent) "smart_toy_outlined" else "desktop_windows_outlined"

    fun at(millis: Long): String = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(millis))

    @Composable
    fun tint(row: DesktopWorkflowRow): Color = when {
        row.running -> Fm.colors.accent
        row.lastStatus == "success" -> Fm.colors.success
        row.lastStatus == "error" -> Fm.colors.danger
        row.specialAgent -> Fm.colors.accent
        else -> Fm.colors.textSecondary
    }

    @Composable
    fun statusColor(status: String): Color = when (status) {
        "success" -> Fm.colors.success
        "error" -> Fm.colors.danger
        "running" -> Fm.colors.accent
        else -> Fm.colors.textSecondary
    }

    @Composable
    fun StatusIcon(status: String) {
        if (status == "running") {
            FmCircularProgressIndicator(strokeWidth = 2.dp, modifier = Modifier.size(18.dp))
        } else {
            FmIcon(WorkflowLook.statusIcon(status), size = 20.dp, tint = statusColor(status))
        }
    }
}

@Composable
internal fun DesktopWorkflowsSection(
    store: DesktopWorkflowsStore,
    rows: List<DesktopWorkflowRow>,
    onOpen: (DesktopWorkflowRow) -> Unit,
    onRun: (DesktopWorkflowRow) -> Unit,
) {
    Column(Modifier.fillMaxWidth()) {
        FmGroupHeader(DesktopWorkflowsText.SECTION_TITLE, count = rows.size.takeIf { it > 0 })
        val note = when {
            store.error.isNotEmpty() && rows.isEmpty() -> if (store.unsupported) store.error else "${store.error} ${DesktopWorkflowsText.OFFLINE}"
            !store.loaded -> null
            rows.isEmpty() -> DesktopWorkflowsText.EMPTY
            else -> ""
        }
        when {
            note == null -> FmCircularProgressIndicator(strokeWidth = 2.dp, modifier = Modifier.padding(16.dp).size(20.dp))
            note.isNotEmpty() -> Text(note, style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
                                      modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
        }
        for (row in rows) {
            FmListRow(
                title = row.name,
                subtitle = DesktopWorkflowsText.subtitle(row) { relativeTime(it) },
                leading = { FmIconTile(DesktopWorkflowLook.icon(row), tint = DesktopWorkflowLook.tint(row)) },
                trailing = when {
                    row.running -> ({ FmCircularProgressIndicator(strokeWidth = 2.dp, modifier = Modifier.padding(8.dp).size(20.dp)) })
                    row.enabled && row.error.isEmpty() -> ({
                        FmIconButton("play_arrow", "Run ${row.name} on the desktop", onPressed = { onRun(row) }, color = Fm.colors.accent)
                    })
                    else -> null
                },
                onTap = { onOpen(row) },
            )
            FmRowDivider()
        }
    }
}

class DesktopWorkflowScreen(private val rowId: String) : Screen() {
    override val title: String get() = WorkflowsDesktop.store?.row(rowId)?.name ?: "Workflow"

    private var detail by mutableStateOf<DesktopWorkflowDetail?>(null)
    private var loadError by mutableStateOf("")
    private var busy by mutableStateOf(false)
    private var assignment by mutableStateOf("")
    private var pending by mutableStateOf<DesktopWorkflowConfirmation?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val store = remember { WorkflowsDesktop.store }
        val status = rememberFmStatus()
        val scope = rememberCoroutineScope()
        val tick = store?.let { rememberOpsTick(it) } ?: 0
        val row = tick.let { store?.row(rowId) }
        LaunchedEffect(store, store?.bells) { if (store != null) load(store) }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(row?.name ?: detail?.name ?: "Workflow", navigator, pluginMenu = false) {
                FmIconButton("refresh", "Refresh", onPressed = {
                    if (store != null) scope.launch {
                        opsIo { store.refresh() }
                        load(store)
                    }
                }, color = Fm.colors.textPrimary)
            }
            if (store == null || row == null) {
                FmEmptyState("desktop_windows_outlined", "This workflow is no longer on the desktop")
                return@Column
            }
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).imePadding().padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                FmInlineStatus(status)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmIconTile(DesktopWorkflowLook.icon(row), tint = Fm.colors.accent)
                    Column(Modifier.weight(1f)) {
                        Text(row.name, style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
                        Text(DesktopWorkflowsText.facts(row) { DesktopWorkflowLook.at(it) }, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                    }
                    FmStatusBadge(
                        if (row.running) "Running" else if (row.enabled) "On" else "Off",
                        if (row.running) Fm.colors.accent else if (row.enabled) Fm.colors.success else Fm.colors.textSecondary,
                    )
                }
                if (row.description.isNotEmpty()) Text(row.description, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
                if (row.error.isNotEmpty()) Text(row.error, style = Fm.text.bodySmall, color = Fm.colors.danger)
                FmSectionHeader("Run")
                if (row.running) {
                    Text(if (row.runningStep.isEmpty()) "Running on the desktop." else "Running ${row.runningStep} on the desktop.",
                         style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    FmToneButton(
                        if (busy) "Asking the desktop…" else "Stop",
                        onPressed = if (busy) null else ({ scope.launch { prepare(store, "stop", row, status) } }),
                        icon = "stop", tone = FmTone.Danger, modifier = Modifier.fillMaxWidth(),
                    )
                } else {
                    FmTextField(
                        value = assignment, onValueChange = { assignment = it },
                        hint = if (row.specialAgent) "Assignment (optional)" else "Input for the run (optional)",
                        maxLines = 4,
                    )
                    FmToneButton(
                        if (busy) "Asking the desktop…" else "Run on the desktop",
                        onPressed = if (busy || !row.enabled || row.error.isNotEmpty()) null else ({ scope.launch { prepare(store, "run", row, status) } }),
                        icon = "play_arrow", tone = FmTone.Accent, filled = true, modifier = Modifier.fillMaxWidth(),
                        busy = busy,
                    )
                    if (!row.enabled) Text("It is turned off on the desktop.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
                detail?.let { current ->
                    if (current.nodes.isNotEmpty()) {
                        val latest = current.latest
                        FmSectionHeader("Steps", count = current.nodes.size)
                        FmCard(padding = PaddingValues(0.dp)) {
                            Column {
                                current.nodes.forEachIndexed { index, step ->
                                    if (index > 0) FmRowDivider()
                                    val stepStatus = latest?.steps?.firstOrNull { it.nodeId == step.id }?.status.orEmpty()
                                    FmListRow(
                                        title = step.name,
                                        subtitle = step.type,
                                        leading = { FmIconTile(WorkflowLook.stepIcon(step.type)) },
                                        trailing = if (stepStatus.isEmpty()) null else ({ DesktopWorkflowLook.StatusIcon(stepStatus) }),
                                        showChevron = false,
                                    )
                                }
                            }
                        }
                    }
                    val runs = current.shownRuns
                    if (runs.isNotEmpty()) {
                        FmSectionHeader("Runs on the desktop", count = runs.size)
                        FmCard(padding = PaddingValues(0.dp)) {
                            Column {
                                runs.forEachIndexed { index, run ->
                                    if (index > 0) FmRowDivider()
                                    FmListRow(
                                        title = WorkflowsText.status(run.status),
                                        subtitle = listOf(if (run.startedAt > 0) relativeTime(run.startedAt) else "", run.trigger, run.summary)
                                            .filter { it.isNotEmpty() }.joinToString(" · "),
                                        leading = { DesktopWorkflowLook.StatusIcon(run.status) },
                                        onTap = { navigator.push(DesktopWorkflowRunScreen(run)) },
                                    )
                                }
                            }
                        }
                    }
                }
                if (loadError.isNotEmpty()) Text(loadError, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                Spacer(Modifier.height(48.dp))
            }
        }
        pending?.let { confirmation ->
            val stopping = confirmation.action == "stop"
            FmConfirmDialog(
                title = if (stopping) "Stop it?" else "Run on the desktop?",
                body = confirmation.summary.ifEmpty { null },
                confirmLabel = if (stopping) "Stop" else "Run",
                destructive = stopping,
                onDismissRequest = { pending = null },
                onConfirm = {
                    pending = null
                    if (store != null) scope.launch { perform(store, confirmation, status) }
                },
            )
        }
    }

    private suspend fun load(store: DesktopWorkflowsStore) {
        val row = store.row(rowId) ?: return
        try {
            detail = opsIo { store.detail(row) }
            loadError = ""
        } catch (thrown: Exception) {
            loadError = DesktopWorkflowsText.describe(thrown)
        }
    }

    private suspend fun prepare(store: DesktopWorkflowsStore, action: String, row: DesktopWorkflowRow, status: com.forkmesh.nativeapp.ui.kit.FmStatus) {
        busy = true
        try {
            pending = opsIo { store.prepare(action, row, if (action == "run") assignment else "") }
        } catch (thrown: Exception) {
            status.error(DesktopWorkflowsText.describe(thrown))
        } finally {
            busy = false
        }
    }

    private suspend fun perform(store: DesktopWorkflowsStore, confirmation: DesktopWorkflowConfirmation, status: com.forkmesh.nativeapp.ui.kit.FmStatus) {
        busy = true
        try {
            val message = opsIo { store.perform(confirmation) }
            if (confirmation.action == "run") assignment = ""
            status.show(message.ifEmpty { "Sent to the desktop." })
            load(store)
        } catch (thrown: Exception) {
            status.error(DesktopWorkflowsText.describe(thrown))
        } finally {
            busy = false
        }
    }
}

class DesktopWorkflowRunScreen(private val run: DesktopWorkflowRun) : Screen() {
    override val title: String get() = WorkflowsText.status(run.status)

    @Composable
    override fun Content(navigator: Navigator) {
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(WorkflowsText.status(run.status), navigator, pluginMenu = false)
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(listOf(if (run.startedAt > 0) DesktopWorkflowLook.at(run.startedAt) else "", run.trigger).filter { it.isNotEmpty() }.joinToString(" · "),
                     style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                if (run.error.isNotEmpty()) Text(run.error, style = Fm.text.bodyMedium, color = Fm.colors.danger)
                if (run.output.isNotEmpty()) {
                    FmCard { Text(run.output, style = Fm.text.bodySmall, color = Fm.colors.textPrimary, modifier = Modifier.fillMaxWidth()) }
                }
                for (step in run.steps) {
                    FmCard {
                        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                FmIcon(WorkflowLook.statusIcon(step.status), size = 18.dp, tint = DesktopWorkflowLook.statusColor(step.status))
                                Text(step.name, style = Fm.text.titleSmall, color = Fm.colors.textPrimary, maxLines = 1, modifier = Modifier.weight(1f))
                                Text(if (step.ms > 0) "${step.ms / 1000} s" else WorkflowsText.status(step.status),
                                     style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                            }
                            if (step.error.isNotEmpty()) Text(step.error, style = Fm.text.bodySmall, color = Fm.colors.danger)
                            if (step.output.isNotEmpty()) Text(step.output, style = Fm.text.bodySmall, color = Fm.colors.textPrimary, maxLines = 12)
                            if (step.logs.isNotEmpty()) {
                                Text(step.logs.joinToString("\n"), style = Fm.text.bodySmall.copy(fontFamily = FmType.mono), color = Fm.colors.textSecondary,
                                     modifier = Modifier.fillMaxWidth())
                            }
                        }
                    }
                }
                Spacer(Modifier.height(48.dp))
            }
        }
    }
}
