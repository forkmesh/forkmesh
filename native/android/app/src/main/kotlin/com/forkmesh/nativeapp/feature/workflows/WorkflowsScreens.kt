package com.forkmesh.nativeapp.feature.workflows

import androidx.compose.foundation.background
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmGroupHeader
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.feature.agents.opsIo
import com.forkmesh.nativeapp.feature.agents.rememberOpsTick
import com.forkmesh.nativeapp.workflows.DesktopWorkflowConfirmation
import com.forkmesh.nativeapp.workflows.DesktopWorkflowsText
import com.forkmesh.nativeapp.workflows.WorkflowCredential
import com.forkmesh.nativeapp.workflows.WorkflowDesktopSteps
import com.forkmesh.nativeapp.workflows.WorkflowDraft
import com.forkmesh.nativeapp.workflows.WorkflowField
import com.forkmesh.nativeapp.workflows.WorkflowNode
import com.forkmesh.nativeapp.workflows.WorkflowNodeKind
import com.forkmesh.nativeapp.workflows.WorkflowRun
import com.forkmesh.nativeapp.workflows.WorkflowSummary
import com.forkmesh.nativeapp.workflows.WorkflowTemplate
import com.forkmesh.nativeapp.workflows.WorkflowsText
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal object WorkflowLook {
    fun triggerIcon(type: String) = when (type) {
        "core.webhook" -> "webhook"
        "core.schedule" -> "schedule"
        "core.manual" -> "touch_app"
        else -> "account_tree"
    }

    fun stepIcon(type: String) = when (type) {
        "core.manual", "core.webhook", "core.schedule" -> triggerIcon(type)
        "core.http" -> "http"
        "core.set" -> "edit"
        "core.if" -> "call_split"
        "core.filter" -> "filter_alt"
        "core.merge" -> "merge_type"
        "core.limit", "core.split" -> "list"
        "core.noop" -> "radio_button_unchecked"
        "core.code" -> "code"
        "core.variable", "core.json" -> "data_object_rounded"
        "core.switch" -> "route_outlined"
        "core.repeat" -> "replay_outlined"
        "core.wait" -> "timer_outlined"
        "core.runWorkflow" -> "account_tree_outlined"
        "core.stop" -> "stop_circle_outlined"
        "core.comment" -> "sticky_note_2_outlined"
        "core.textCase" -> "text_snippet_outlined"
        "core.textReplace" -> "edit_note_outlined"
        "core.textMatch" -> "search"
        "core.textSplit" -> "vertical_split_outlined"
        "core.textCombine" -> "call_merge"
        "core.textEncode" -> "lock_outline"
        "core.hash" -> "tag"
        "core.textExtract" -> "alternate_email"
        "core.count" -> "format_list_bulleted"
        "core.math", "core.round", "core.formatNumber" -> "tune"
        "core.random" -> "casino_outlined"
        "core.statistics" -> "leaderboard_outlined"
        "core.combineItems" -> "layers_outlined"
        "core.sort" -> "sort"
        "core.dedupe" -> "filter_alt_off_outlined"
        "core.pick" -> "playlist_play_outlined"
        "core.url" -> "link"
        "core.dateFormat" -> "event_outlined"
        "core.dateAdjust" -> "update"
        "core.dateDiff" -> "timelapse"
        else -> "bolt"
    }

    fun statusIcon(status: String) = when (status) {
        "success" -> "check_circle"
        "error" -> "error_outline"
        else -> "radio_button_unchecked"
    }

    @Composable
    fun statusColor(status: String): Color = when (status) {
        "success" -> Fm.colors.success
        "error" -> Fm.colors.danger
        else -> Fm.colors.textSecondary
    }

    fun color(hex: String, fallback: Color): Color {
        val digits = hex.removePrefix("#")
        val value = if (digits.length == 6) digits.toLongOrNull(16) else null
        return if (value == null) fallback else Color(0xFF000000 or value)
    }
}

class WorkflowsScreen : Screen() {
    override val title: String get() = WorkflowsText.TITLE

    private var workflows by mutableStateOf<List<WorkflowSummary>>(emptyList())
    private var loading by mutableStateOf(true)
    private var error by mutableStateOf("")
    private var generation by mutableIntStateOf(0)
    private var creating by mutableStateOf(false)
    private var opening by mutableStateOf("")
    private var accountPending by mutableStateOf<WorkflowDraft?>(null)
    private var desktopPending by mutableStateOf<DesktopWorkflowConfirmation?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val session = remember { WorkflowsSession.get() }
        val desktop = remember { WorkflowsDesktop.store }
        val status = rememberFmStatus()
        val scope = rememberCoroutineScope()
        LaunchedEffect(generation) {
            val current = session ?: return@LaunchedEffect
            loading = true
            error = ""
            try {
                workflows = withContext(Dispatchers.IO) {
                    current.loadCatalog()
                    current.api.list()
                }
            } catch (thrown: Exception) {
                error = WorkflowsText.describe(thrown, "Workflows are unavailable right now.")
            }
            loading = false
        }
        LaunchedEffect(generation, desktop) { desktop?.let { opsIo { it.refresh() } } }
        val top = navigator.top
        LaunchedEffect(top) { if (top === this@WorkflowsScreen) generation++ }
        val tick = desktop?.let { rememberOpsTick(it) } ?: 0
        val desktopRows = tick.let { desktop?.rows ?: emptyList() }
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar(WorkflowsText.TITLE, navigator) {
                    FmIconButton("lock_outline", "Credentials", onPressed = { session?.let { navigator.push(WorkflowCredentialsScreen(it)) } },
                                 color = Fm.colors.textPrimary)
                    FmIconButton("refresh", "Refresh", onPressed = { generation++ }, color = Fm.colors.textPrimary)
                }
                when {
                    session == null -> FmEmptyState("account_tree", WorkflowsText.UNAVAILABLE)
                    loading && workflows.isEmpty() && desktopRows.isEmpty() ->
                        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    error.isNotEmpty() && workflows.isEmpty() && desktop == null -> FmEmptyState("error_outline", WorkflowsText.UNAVAILABLE, message = error)
                    workflows.isEmpty() && desktop == null -> FmEmptyState("account_tree", WorkflowsText.EMPTY_TITLE, message = WorkflowsText.EMPTY_MESSAGE)
                    else -> LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 96.dp)) {
                        item { FmInlineStatus(status, Modifier.padding(horizontal = 16.dp)) }
                        if (error.isNotEmpty() && workflows.isEmpty()) {
                            item { Text(error, style = Fm.text.bodySmall, color = Fm.colors.danger, modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) }
                        }
                        if (desktop != null) {
                            item {
                                FmGroupHeader(DesktopWorkflowsText.ACCOUNT_TITLE, count = workflows.size.takeIf { it > 0 })
                                if (workflows.isEmpty() && !loading && error.isEmpty()) {
                                    Text(WorkflowsText.EMPTY_MESSAGE, style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
                                         modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
                                }
                            }
                        }
                        items(workflows, key = { it.id }) { workflow ->
                            FmListRow(
                                title = workflow.name,
                                subtitle = subtitle(workflow),
                                leading = { FmIconTile(WorkflowLook.triggerIcon(workflow.trigger), tint = WorkflowLook.statusColor(workflow.lastStatus)) },
                                trailing = accountTrailing(session, workflow, status, scope),
                                onTap = { navigator.push(WorkflowEditorScreen(session, workflow.id, null)) },
                            )
                            FmRowDivider()
                        }
                        if (desktop != null) {
                            item {
                                DesktopWorkflowsSection(desktop, desktopRows, onOpen = { navigator.push(DesktopWorkflowScreen(it.id)) }) { row ->
                                    scope.launch {
                                        try {
                                            desktopPending = opsIo { desktop.prepare("run", row) }
                                        } catch (thrown: Exception) {
                                            status.error(DesktopWorkflowsText.describe(thrown))
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (session != null) {
                FmExtendedFloatingActionButton(
                    "New workflow",
                    onClick = { creating = true },
                    modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
                    icon = "add",
                )
            }
        }
        if (creating && session != null) {
            NewWorkflowSheet(session, onDismiss = { creating = false }) { draft ->
                creating = false
                navigator.push(WorkflowEditorScreen(session, null, draft))
            }
        }
        accountPending?.let { draft ->
            FmConfirmDialog(
                title = "Run on your desktop?",
                body = WorkflowDesktopSteps.confirmMessage(draft),
                confirmLabel = "Run",
                onDismissRequest = { accountPending = null },
                onConfirm = {
                    accountPending = null
                    if (session != null) scope.launch { runOnDesktop(session, draft, navigator, status) }
                },
            )
        }
        desktopPending?.let { confirmation ->
            FmConfirmDialog(
                title = "Run on the desktop?",
                body = confirmation.summary.ifEmpty { null },
                confirmLabel = "Run",
                onDismissRequest = { desktopPending = null },
                onConfirm = {
                    desktopPending = null
                    val store = desktop ?: return@FmConfirmDialog
                    scope.launch {
                        try {
                            val message = opsIo { store.perform(confirmation) }
                            status.show(message.ifEmpty { "Sent to the desktop." })
                        } catch (thrown: Exception) {
                            status.error(DesktopWorkflowsText.describe(thrown))
                        }
                    }
                },
            )
        }
    }

    private fun accountTrailing(
        session: WorkflowsSession,
        workflow: WorkflowSummary,
        status: com.forkmesh.nativeapp.ui.kit.FmStatus,
        scope: kotlinx.coroutines.CoroutineScope,
    ): (@Composable () -> Unit)? {
        val running = session.desktopRunning.value == workflow.id || opening == workflow.id
        if (!running && !workflow.runsOnDesktop) return if (workflow.active) ({ FmStatusBadge("Active", Fm.colors.accent) }) else null
        return {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                if (workflow.active) FmStatusBadge("Active", Fm.colors.accent)
                if (running) {
                    FmCircularProgressIndicator(strokeWidth = 2.dp, modifier = Modifier.padding(8.dp).size(20.dp))
                } else {
                    FmIconButton(
                        "play_arrow", "Run ${workflow.name} on your desktop",
                        onPressed = if (session.desktopRunning.value.isEmpty() && opening.isEmpty()) ({
                            scope.launch { openForDesktop(session, workflow, status) }
                        }) else null,
                        color = Fm.colors.accent,
                    )
                }
            }
        }
    }

    private suspend fun openForDesktop(session: WorkflowsSession, workflow: WorkflowSummary, status: com.forkmesh.nativeapp.ui.kit.FmStatus) {
        if (!session.canRunOnDesktop) {
            status.error(WorkflowsText.DESKTOP_MISSING)
            return
        }
        opening = workflow.id
        try {
            accountPending = withContext(Dispatchers.IO) { session.api.open(workflow.id) }
        } catch (thrown: Exception) {
            status.error(WorkflowsText.describe(thrown, "This workflow could not be opened."))
        } finally {
            opening = ""
        }
    }

    private suspend fun runOnDesktop(session: WorkflowsSession, draft: WorkflowDraft, navigator: Navigator, status: com.forkmesh.nativeapp.ui.kit.FmStatus) {
        if (session.desktopRunning.value.isNotEmpty()) return
        val run = withContext(Dispatchers.IO) { session.runOnDesktop(draft) }
        if (run.succeeded) status.show("Done on your desktop.") else status.error(run.error.ifEmpty { "The run failed on your desktop." })
        generation++
        navigator.push(WorkflowRunScreen(run, draft))
    }

    private fun subtitle(workflow: WorkflowSummary): String {
        val parts = mutableListOf(WorkflowsText.trigger(workflow.trigger))
        if (workflow.scheduleMinutes > 0) parts.add(WorkflowsText.every(workflow.scheduleMinutes))
        parts.add(if (workflow.steps == 1) "1 step" else "${workflow.steps} steps")
        if (workflow.lastRunAt > 0) parts.add(WorkflowsText.status(workflow.lastStatus) + " " + relativeTime(workflow.lastRunAt))
        return parts.joinToString(" · ")
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun NewWorkflowSheet(session: WorkflowsSession, onDismiss: () -> Unit, onOpen: (WorkflowDraft) -> Unit) {
    var templates by remember { mutableStateOf<List<WorkflowTemplate>>(emptyList()) }
    var opening by remember { mutableStateOf("") }
    val status = rememberFmStatus()
    val scope = rememberCoroutineScope()
    LaunchedEffect(Unit) {
        try {
            templates = withContext(Dispatchers.IO) { session.api.templates() }
        } catch (thrown: Exception) {
            status.error(WorkflowsText.describe(thrown, "Examples are unavailable right now."))
        }
    }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        FmPanelHeader(title = "New workflow")
        FmInlineStatus(status, Modifier.padding(horizontal = 16.dp))
        LazyColumn(Modifier.fillMaxWidth(), contentPadding = PaddingValues(bottom = 24.dp)) {
            item {
                FmListRow(
                    title = "Blank workflow",
                    subtitle = "A manual trigger and nothing else",
                    leading = { FmIconTile("add", tint = Fm.colors.accent) },
                    onTap = { onOpen(WorkflowDraft.BLANK) },
                )
                FmGroupHeader("Examples", count = templates.size.takeIf { it > 0 })
            }
            items(templates, key = { it.key }) { template ->
                FmListRow(
                    title = template.name,
                    subtitle = template.description,
                    leading = { FmIconTile("account_tree") },
                    trailing = if (opening == template.key) ({ FmCircularProgressIndicator(strokeWidth = 2.dp) }) else null,
                    onTap = {
                        if (opening.isEmpty()) {
                            opening = template.key
                            scope.launch {
                                try {
                                    onOpen(withContext(Dispatchers.IO) { session.api.template(template.key) })
                                } catch (thrown: Exception) {
                                    status.error(WorkflowsText.describe(thrown, "This example could not be opened."))
                                }
                                opening = ""
                            }
                        }
                    },
                )
                FmRowDivider()
            }
        }
    }
}

class WorkflowEditorScreen(private val session: WorkflowsSession, private val workflowId: String?, initial: WorkflowDraft?) : Screen() {
    override val title: String get() = draft?.name ?: WorkflowsText.TITLE

    var draft by mutableStateOf(initial)
    private var saved by mutableStateOf<WorkflowDraft?>(null)
    private var runs by mutableStateOf<List<WorkflowRun>>(emptyList())
    private var busy by mutableStateOf("")
    private var loadError by mutableStateOf("")
    private var adding by mutableStateOf<Pair<String?, List<String>>?>(null)
    private var confirmDelete by mutableStateOf(false)
    private var confirmDesktop by mutableStateOf(false)
    private var menu by mutableStateOf(false)

    private val dirty: Boolean get() = draft != null && draft != saved

    @Composable
    override fun Content(navigator: Navigator) {
        val status = rememberFmStatus()
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            try {
                withContext(Dispatchers.IO) {
                    session.loadCatalog()
                    runCatching { session.loadCredentials() }
                }
                if (draft == null && workflowId != null) {
                    val opened = withContext(Dispatchers.IO) { session.api.open(workflowId) }
                    draft = opened
                    saved = opened
                }
                refreshRuns()
            } catch (thrown: Exception) {
                loadError = WorkflowsText.describe(thrown, "This workflow could not be opened.")
            }
        }
        val catalog = session.catalog.value
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar(draft?.name ?: WorkflowsText.TITLE, navigator, pluginMenu = false) {
                    if (busy == "save") {
                        FmCircularProgressIndicator(strokeWidth = 2.dp)
                    } else if (dirty) {
                        FmTextButton(onClick = { scope.launch { save(status) } }) { Text("Save") }
                    }
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem("Credentials", onClick = {
                                menu = false
                                navigator.push(WorkflowCredentialsScreen(session))
                            }, icon = "lock_outline")
                            if (draft?.id?.isNotEmpty() == true) {
                                FmDropdownMenuItem("Delete workflow", onClick = {
                                    menu = false
                                    confirmDelete = true
                                }, icon = "delete_outline", textColor = Fm.colors.danger)
                            }
                        }
                    }
                }
                val current = draft
                when {
                    current != null -> Editor(current, navigator, status)
                    loadError.isNotEmpty() -> FmEmptyState("error_outline", WorkflowsText.UNAVAILABLE, message = loadError)
                    else -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                }
            }
        }
        adding?.let { (after, outputs) ->
            StepPickerSheet(catalog.search(""), catalog.categories, outputs, onDismiss = { adding = null }) { kind, output ->
                adding = null
                val current = draft ?: return@StepPickerSheet
                val (next, step) = current.addStep(kind, after, output)
                draft = next
                navigator.push(WorkflowStepScreen(session, this, step.id))
            }
        }
        if (confirmDesktop) {
            FmConfirmDialog(
                title = "Run on your desktop?",
                body = draft?.let { WorkflowDesktopSteps.confirmMessage(it) },
                confirmLabel = "Run",
                onDismissRequest = { confirmDesktop = false },
                onConfirm = {
                    confirmDesktop = false
                    scope.launch { runOnDesktop(navigator, status) }
                },
            )
        }
        if (confirmDelete) {
            FmConfirmDialog(
                title = "Delete this workflow?",
                body = "Its run history goes with it. Credentials stay on your account.",
                confirmLabel = "Delete",
                destructive = true,
                onDismissRequest = { confirmDelete = false },
                onConfirm = {
                    confirmDelete = false
                    val id = draft?.id ?: return@FmConfirmDialog
                    scope.launch {
                        try {
                            withContext(Dispatchers.IO) { session.api.delete(id) }
                            navigator.pop()
                        } catch (thrown: Exception) {
                            status.error(WorkflowsText.describe(thrown, "This workflow could not be deleted."))
                        }
                    }
                },
            )
        }
    }

    @Composable
    private fun Editor(current: WorkflowDraft, navigator: Navigator, status: com.forkmesh.nativeapp.ui.kit.FmStatus) {
        val catalog = session.catalog.value
        val scope = rememberCoroutineScope()
        val steps = current.ordered.filter { !it.isTrigger }
        val latest = runs.firstOrNull { it.hasDetail }
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).imePadding().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            FmInlineStatus(status)
            FmTextField(value = current.name, onValueChange = { draft = current.copy(name = it) }, hint = "Workflow name")
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Active", style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
                    Text("Webhooks and schedules start it on ForkMesh while it is active.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
                FmSwitch(checked = current.active, onCheckedChange = { draft = current.copy(active = it) })
            }
            FmSectionHeader("Trigger")
            FmCard(padding = PaddingValues(0.dp)) {
                val trigger = current.trigger
                if (trigger != null) {
                    FmListRow(
                        title = trigger.name,
                        subtitle = when (trigger.type) {
                            "core.webhook" -> if (current.id.isEmpty()) "Save to get the secret URL" else "Called by its secret URL"
                            "core.schedule" -> WorkflowsText.every(trigger.text("every").toIntOrNull() ?: 60)
                            else -> "Runs when you press Run"
                        },
                        leading = { FmIconTile(WorkflowLook.triggerIcon(trigger.type), tint = WorkflowLook.color(catalog.kind(trigger.type)?.color ?: "", Fm.colors.accent)) },
                        onTap = { navigator.push(WorkflowStepScreen(session, this@WorkflowEditorScreen, trigger.id)) },
                    )
                } else {
                    FmListRow(
                        title = "Add a trigger",
                        subtitle = "Manual, webhook or schedule",
                        leading = { FmIconTile("add", tint = Fm.colors.accent) },
                        onTap = { catalog.kind("core.manual")?.let { draft = current.setTrigger(it) } },
                    )
                }
            }
            FmSectionHeader("Steps", count = steps.size.takeIf { it > 0 })
            if (steps.isNotEmpty()) {
                FmCard(padding = PaddingValues(0.dp)) {
                    Column {
                        steps.forEachIndexed { index, node ->
                            if (index > 0) FmRowDivider()
                            StepRow(current, node, latest, navigator)
                        }
                    }
                }
            }
            FmToneButton("Add step", onPressed = {
                val anchor = steps.lastOrNull() ?: current.trigger
                adding = anchor?.id to (anchor?.let { catalog.outputs(it.type) } ?: listOf("main"))
            }, icon = "add", modifier = Modifier.fillMaxWidth())
            FmSectionHeader("Run")
            if (WorkflowDesktopSteps.hasDesktopSteps(current)) {
                FmToneButton(
                    if (busy == "desktop") "Running on your desktop…" else "Run on your desktop",
                    onPressed = if (busy.isEmpty() && session.canRunOnDesktop) ({ confirmDesktop = true }) else null,
                    icon = "desktop_windows_outlined", tone = FmTone.Accent, filled = true, modifier = Modifier.fillMaxWidth(),
                )
                Text(if (session.canRunOnDesktop) WorkflowsText.DESKTOP_RUN_NOTE else WorkflowsText.DESKTOP_MISSING,
                     style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            } else {
                FmToneButton(
                    if (busy == "phone") "Running on this phone…" else "Run on this phone",
                    onPressed = if (busy.isEmpty() && session.engine != null) ({ scope.launch { runOnPhone(navigator) } }) else null,
                    icon = "smartphone", tone = FmTone.Accent, filled = true, modifier = Modifier.fillMaxWidth(),
                )
                FmToneButton(
                    if (busy == "server") "Running on ForkMesh…" else "Run on ForkMesh",
                    onPressed = if (busy.isEmpty()) ({ scope.launch { runOnServer(navigator, status) } }) else null,
                    icon = "cloud_outlined", modifier = Modifier.fillMaxWidth(),
                )
                Text(WorkflowsText.PHONE_RUN_NOTE, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                val missing = session.missingOnPhone(current)
                if (missing.isNotEmpty()) {
                    FmCard(onTap = { navigator.push(WorkflowCredentialsScreen(session)) }) {
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            FmIcon("lock_outline", size = 18.dp, tint = Fm.colors.warning)
                            Text("Save a credential on this phone for ${missing.joinToString(", ")} to run those steps here.",
                                 style = Fm.text.bodySmall, color = Fm.colors.textPrimary)
                        }
                    }
                }
            }
            if (runs.isNotEmpty()) {
                FmSectionHeader("Runs", count = runs.size)
                FmCard(padding = PaddingValues(0.dp)) {
                    Column {
                        runs.take(30).forEachIndexed { index, run ->
                            if (index > 0) FmRowDivider()
                            FmListRow(
                                title = WorkflowsText.status(run.status),
                                subtitle = listOf(if (run.phone) "On this phone" else if (run.desktop) "On your desktop" else "On ForkMesh",
                                                  if (run.startedAt > 0) relativeTime(run.startedAt) else "", run.error)
                                    .filter { it.isNotEmpty() }.joinToString(" · "),
                                leading = { FmIcon(WorkflowLook.statusIcon(run.status), size = 22.dp, tint = WorkflowLook.statusColor(run.status)) },
                                onTap = { scope.launch { openRun(run, navigator, status) } },
                            )
                        }
                    }
                }
            }
            Spacer(Modifier.height(64.dp))
        }
    }

    @Composable
    private fun StepRow(current: WorkflowDraft, node: WorkflowNode, latest: WorkflowRun?, navigator: Navigator) {
        val catalog = session.catalog.value
        val inputs = current.inputs(node.id).map { catalog.branch(it, current) }
        val subtitle = (listOf(catalog.title(node.type), catalog.summary(node)) + inputs.ifEmpty { listOf("Not connected") })
            .filter { it.isNotEmpty() }.joinToString(" · ")
        val live = if (current.id.isNotEmpty() && session.desktopRunning.value == current.id) session.desktopLive.value[node.id] ?: "waiting" else null
        val runStatus = live ?: (latest?.nodes?.get(node.id) as? Map<*, *>)?.get("status") as? String ?: ""
        FmListRow(
            title = node.name,
            subtitle = subtitle,
            leading = { FmIconTile(WorkflowLook.stepIcon(node.type), tint = WorkflowLook.color(catalog.kind(node.type)?.color ?: "", Fm.colors.accent)) },
            trailing = when (runStatus) {
                "", "waiting" -> null
                "running" -> ({ FmCircularProgressIndicator(strokeWidth = 2.dp, modifier = Modifier.size(18.dp)) })
                else -> ({ FmIcon(WorkflowLook.statusIcon(runStatus), size = 18.dp, tint = WorkflowLook.statusColor(runStatus)) })
            },
            onTap = { navigator.push(WorkflowStepScreen(session, this, node.id)) },
            onLongPress = { adding = node.id to catalog.outputs(node.type) },
        )
    }

    private suspend fun refreshRuns() {
        val id = draft?.id.orEmpty()
        if (id.isEmpty()) {
            runs = session.phoneRuns["draft"] ?: emptyList()
            return
        }
        val server = runCatching { withContext(Dispatchers.IO) { session.api.runs(id) } }.getOrDefault(emptyList())
        runs = ((session.phoneRuns[id] ?: emptyList()) + server).sortedByDescending { it.startedAt }
    }

    private suspend fun save(status: com.forkmesh.nativeapp.ui.kit.FmStatus): Boolean {
        val current = draft ?: return false
        if (busy.isNotEmpty()) return false
        val problem = session.check(current)
        if (problem.isNotEmpty()) {
            status.error(problem)
            return false
        }
        busy = "save"
        return try {
            val stored = withContext(Dispatchers.IO) { session.api.save(current) }
            draft = stored
            saved = stored
            status.show("Saved")
            true
        } catch (thrown: Exception) {
            status.error(WorkflowsText.describe(thrown, "This workflow could not be saved."))
            false
        } finally {
            busy = ""
        }
    }

    private suspend fun runOnPhone(navigator: Navigator) {
        val current = draft ?: return
        busy = "phone"
        val run = withContext(Dispatchers.IO) { session.runOnPhone(current) }
        busy = ""
        refreshRuns()
        if (run != null) navigator.push(WorkflowRunScreen(run, current))
    }

    private suspend fun runOnDesktop(navigator: Navigator, status: com.forkmesh.nativeapp.ui.kit.FmStatus) {
        if (dirty || draft?.id.isNullOrEmpty()) {
            if (!save(status)) return
        }
        val current = draft ?: return
        if (busy.isNotEmpty()) return
        busy = "desktop"
        val run = try {
            withContext(Dispatchers.IO) { session.runOnDesktop(current) }
        } finally {
            busy = ""
        }
        refreshRuns()
        if (runs.none { it.id == run.id }) runs = listOf(run) + runs
        if (run.succeeded) status.show("Done on your desktop.") else status.error(run.error.ifEmpty { "The run failed on your desktop." })
        navigator.push(WorkflowRunScreen(run, current))
    }

    private suspend fun runOnServer(navigator: Navigator, status: com.forkmesh.nativeapp.ui.kit.FmStatus) {
        if (dirty || draft?.id.isNullOrEmpty()) {
            if (!save(status)) return
        }
        val current = draft ?: return
        busy = "server"
        try {
            val run = withContext(Dispatchers.IO) { session.api.run(current.id) }
            refreshRuns()
            navigator.push(WorkflowRunScreen(run, current))
        } catch (thrown: Exception) {
            status.error(WorkflowsText.describe(thrown, "ForkMesh could not run this workflow."))
        } finally {
            busy = ""
        }
    }

    private suspend fun openRun(run: WorkflowRun, navigator: Navigator, status: com.forkmesh.nativeapp.ui.kit.FmStatus) {
        val current = draft ?: return
        if (run.hasDetail || run.phone) {
            navigator.push(WorkflowRunScreen(run, current))
            return
        }
        try {
            navigator.push(WorkflowRunScreen(withContext(Dispatchers.IO) { session.api.runDetail(current.id, run.id) }, current))
        } catch (thrown: Exception) {
            status.error(WorkflowsText.describe(thrown, "This run could not be opened."))
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun StepPickerSheet(
    kinds: List<WorkflowNodeKind>,
    categories: List<String>,
    outputs: List<String>,
    onDismiss: () -> Unit,
    onPick: (WorkflowNodeKind, String) -> Unit,
) {
    var query by remember { mutableStateOf("") }
    var output by remember { mutableStateOf(outputs.firstOrNull() ?: "main") }
    val needle = query.trim().lowercase()
    val found = kinds.filter { needle.isEmpty() || needle in it.name.lowercase() || needle in it.group.lowercase() || needle in it.description.lowercase() }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        FmPanelHeader(title = "Add a step")
        Column(Modifier.padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            FmTextField(value = query, onValueChange = { query = it }, hint = "Search steps and integrations")
            if (outputs.size > 1) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (name in outputs) {
                        FmToneButton(name.replaceFirstChar { it.uppercase() }, onPressed = { output = name },
                                     tone = if (output == name) FmTone.Accent else FmTone.Neutral, filled = output == name)
                    }
                }
            }
        }
        LazyColumn(Modifier.fillMaxWidth().imePadding(), contentPadding = PaddingValues(bottom = 24.dp)) {
            val groups = listOf("Core" to found.filter { !it.isApp }) + categories.map { category -> category to found.filter { it.isApp && it.group == category } }
            for ((title, list) in groups) {
                if (list.isEmpty()) continue
                item(key = "group-$title") { FmGroupHeader(title, count = list.size) }
                items(list, key = { it.type }) { kind ->
                    FmListRow(
                        title = kind.name,
                        subtitle = kind.description,
                        leading = { FmIconTile(WorkflowLook.stepIcon(kind.type), tint = WorkflowLook.color(kind.color, Fm.colors.accent)) },
                        onTap = { onPick(kind, output) },
                    )
                    FmRowDivider()
                }
            }
        }
    }
}

class WorkflowStepScreen(private val session: WorkflowsSession, private val editor: WorkflowEditorScreen, private val nodeId: String) : Screen() {
    override val title: String get() = editor.draft?.node(nodeId)?.name ?: "Step"

    private var nameText by mutableStateOf<String?>(null)
    private var nameError by mutableStateOf("")
    private var addingCredential by mutableStateOf(false)

    private fun update(change: (WorkflowNode) -> WorkflowNode) {
        val draft = editor.draft ?: return
        val node = draft.node(nodeId) ?: return
        editor.draft = draft.update(change(node))
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val draft = editor.draft
        val node = draft?.node(nodeId)
        val catalog = session.catalog.value
        val kind = node?.let { catalog.kind(it.type) }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(node?.name ?: "Step", navigator, pluginMenu = false)
            if (draft == null || node == null) {
                FmEmptyState("error_outline", "This step was removed")
                return@Column
            }
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).imePadding().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmIconTile(WorkflowLook.stepIcon(node.type), tint = WorkflowLook.color(kind?.color ?: "", Fm.colors.accent))
                    Column(Modifier.weight(1f)) {
                        Text(kind?.name ?: node.type, style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
                        kind?.description?.takeIf { it.isNotEmpty() }?.let { Text(it, style = Fm.text.bodySmall, color = Fm.colors.textSecondary) }
                    }
                }
                Label("Name")
                FmTextField(
                    value = nameText ?: node.name,
                    onValueChange = { text ->
                        nameText = text
                        val name = text.trim()
                        nameError = when {
                            name.isEmpty() -> "Every step needs a name."
                            draft.nameTaken(name, nodeId) -> "Another step is already called $name."
                            else -> ""
                        }
                        if (nameError.isEmpty()) update { it.copy(name = name.take(60)) }
                    },
                    errorText = nameError.ifEmpty { null },
                )
                if (node.isTrigger) {
                    Label("Trigger")
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        for (trigger in catalog.triggers) {
                            FmToneButton(trigger.name, onPressed = { editor.draft = editor.draft?.setTrigger(trigger) },
                                         tone = if (trigger.type == node.type) FmTone.Accent else FmTone.Neutral, filled = trigger.type == node.type)
                        }
                    }
                }
                if (kind != null && kind.isApp) {
                    Label("Operation")
                    Choice(
                        kind.operations.map { it.key to it.name },
                        node.operation.ifEmpty { kind.operations.firstOrNull()?.key ?: "" },
                    ) { picked -> update { it.withParam("operation", picked) } }
                    if (kind.needsCredential) {
                        Label("Credential")
                        val choices = session.credentials.value.filter { it.integration == kind.appKey }
                        Choice(listOf("" to "Choose a credential") + choices.map { it.id to it.name.ifEmpty { kind.name } }, node.credential) { picked ->
                            update { it.copy(credential = picked) }
                        }
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            if (node.credential.isNotEmpty()) {
                                val onPhone = node.credential in session.phoneIds.value
                                FmStatusBadge(if (onPhone) "On this phone" else "Not on this phone", if (onPhone) Fm.colors.success else Fm.colors.warning)
                            }
                            Spacer(Modifier.weight(1f))
                            FmTextButton(onClick = { addingCredential = true }) { Text("New credential") }
                        }
                    }
                }
                val fields = kind?.fields(node.operation) ?: emptyList()
                for (field in fields) {
                    Label(if (field.required) field.label + " *" else field.label)
                    FieldEditor(field, node, catalog.operators) { value -> update { it.withParam(field.key, value) } }
                }
                if (fields.isNotEmpty()) {
                    Text("Use {{ \$json.field }} for the item's values, {{ \$node[\"Step\"].json.field }} for another step's and {{ \$trigger.field }} for the trigger's.",
                         style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
                if (!node.isTrigger) {
                    Toggle("Turned off (passes items through)", node.disabled) { on -> update { it.copy(disabled = on) } }
                    Toggle("Continue when this step fails", node.continueOnFail) { on -> update { it.copy(continueOnFail = on) } }
                    FmToneButton("Delete step", onPressed = {
                        editor.draft = editor.draft?.remove(nodeId)
                        navigator.pop()
                    }, icon = "delete_outline", tone = FmTone.Danger, modifier = Modifier.fillMaxWidth())
                }
                Spacer(Modifier.height(64.dp))
            }
        }
        if (addingCredential && kind != null) {
            CredentialSheet(session, kind.appKey, null, onDismiss = { addingCredential = false }) { created ->
                addingCredential = false
                if (created != null) update { it.copy(credential = created.id) }
            }
        }
    }
}

@Composable
private fun Label(text: String) {
    Text(text, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
}

@Composable
private fun Toggle(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
        FmSwitch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun Choice(options: List<Pair<String, String>>, selected: String, onPick: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        FmToneButton(options.firstOrNull { it.first == selected }?.second ?: selected.ifEmpty { "Choose" }, onPressed = { open = true },
                     icon = "arrow_drop_down", modifier = Modifier.fillMaxWidth())
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for ((value, label) in options) {
                FmDropdownMenuItem(label, onClick = {
                    open = false
                    onPick(value)
                })
            }
        }
    }
}

@Composable
private fun FieldEditor(field: WorkflowField, node: WorkflowNode, operators: List<Pair<String, String>>, onChange: (String) -> Unit) {
    val value = if (node.params.containsKey(field.key)) node.text(field.key) else field.defaultValue
    when (field.type) {
        "boolean" -> Toggle(field.label, value.lowercase() in setOf("true", "1", "yes", "on")) { onChange(if (it) "true" else "false") }
        "select" -> Choice(if (field.options.isEmpty() && field.key == "operator") operators else field.options, value, onChange)
        else -> FmTextField(
            value = value,
            onValueChange = onChange,
            hint = field.placeholder.ifEmpty { null },
            minLines = if (field.multiline) 3 else 1,
            maxLines = if (field.multiline) 12 else 1,
            textStyle = if (field.mono) Fm.text.bodyMedium.copy(fontFamily = FmType.mono) else Fm.text.bodyLarge,
        )
    }
}

class WorkflowRunScreen(private val run: WorkflowRun, private val draft: WorkflowDraft) : Screen() {
    override val title: String get() = WorkflowsText.status(run.status)

    @Composable
    override fun Content(navigator: Navigator) {
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(WorkflowsText.status(run.status), navigator, pluginMenu = false)
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmIconTile(
                        if (run.phone) "smartphone" else if (run.desktop) "desktop_windows_outlined" else "cloud_outlined",
                        tint = if (run.succeeded) Fm.colors.success else Fm.colors.danger,
                    )
                    Column {
                        Text(if (run.phone) "Ran on this phone" else if (run.desktop) "Ran on your desktop" else "Ran on ForkMesh",
                             style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
                        val parts = mutableListOf<String>()
                        if (run.startedAt > 0) parts.add(relativeTime(run.startedAt))
                        if (run.startedAt > 0 && run.finishedAt >= run.startedAt) parts.add("${run.finishedAt - run.startedAt} ms")
                        if (!run.desktop) parts.add(if (run.requests == 1L) "1 request" else "${run.requests} requests")
                        Text(parts.joinToString(" · "), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                    }
                }
                if (run.error.isNotEmpty()) {
                    FmCard(color = Fm.colors.danger.copy(alpha = 0.12f)) {
                        Text(run.error, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    }
                }
                val steps = run.steps(draft)
                if (steps.isEmpty()) Text("This run has no step details.", style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
                for (step in steps) {
                    FmCard {
                        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                FmIcon(WorkflowLook.statusIcon(step.status), size = 18.dp, tint = WorkflowLook.statusColor(step.status))
                                Text(step.name, style = Fm.text.titleSmall, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
                                Text(if (step.status == "skipped") "Skipped" else "${step.ms} ms", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                            }
                            if (step.error.isNotEmpty()) Text(step.error, style = Fm.text.bodySmall, color = Fm.colors.danger)
                            for ((name, count, items) in step.outputs) {
                                val prefix = if (step.outputs.size == 1) "" else name.replaceFirstChar { it.uppercase() } + ": "
                                Text(prefix + if (count == 1L) "1 item" else "$count items", style = Fm.text.labelMedium, color = Fm.colors.textSecondary)
                                for (item in items) {
                                    Text(Json.stringify(item), style = Fm.text.bodySmall.copy(fontFamily = FmType.mono), color = Fm.colors.textSecondary, maxLines = 6)
                                }
                                if (count > items.size) Text("and ${count - items.size} more", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                            }
                        }
                    }
                }
                Spacer(Modifier.height(48.dp))
            }
        }
    }
}

class WorkflowCredentialsScreen(private val session: WorkflowsSession) : Screen() {
    override val title: String get() = "Credentials"

    private var loading by mutableStateOf(true)
    private var error by mutableStateOf("")
    private var creating by mutableStateOf(false)
    private var editing by mutableStateOf<WorkflowCredential?>(null)
    private var deleting by mutableStateOf<WorkflowCredential?>(null)
    private var generation by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LaunchedEffect(generation) {
            loading = true
            error = try {
                withContext(Dispatchers.IO) {
                    session.loadCatalog()
                    session.loadCredentials()
                }
                ""
            } catch (thrown: Exception) {
                WorkflowsText.describe(thrown, "Credentials are unavailable right now.")
            }
            loading = false
        }
        val credentials = session.credentials.value
        val catalog = session.catalog.value
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar("Credentials", navigator, pluginMenu = false) {
                    FmIconButton("refresh", "Refresh", onPressed = { generation++ }, color = Fm.colors.textPrimary)
                }
                when {
                    loading && credentials.isEmpty() -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    credentials.isEmpty() -> FmEmptyState(
                        "lock_outline", if (error.isEmpty()) "No credentials yet" else "Credentials are unavailable",
                        message = error.ifEmpty { "A credential holds the API key or token an integration step signs in with." },
                    )
                    else -> LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 96.dp)) {
                        item {
                            Text(
                                "Your account keeps credentials encrypted and never sends their values back. To run a step on this phone, save its values here too: they stay in this phone's secure storage.",
                                style = Fm.text.bodySmall, color = Fm.colors.textSecondary, modifier = Modifier.padding(16.dp),
                            )
                        }
                        items(credentials, key = { it.id }) { credential ->
                            val app = catalog.kind("app." + credential.integration)
                            val onPhone = credential.id in session.phoneIds.value
                            FmListRow(
                                title = credential.name.ifEmpty { app?.name ?: credential.integration },
                                subtitle = listOf(app?.name ?: credential.integration, credential.hintText).filter { it.isNotEmpty() }.joinToString(" · "),
                                leading = { FmIconTile("lock_outline", tint = WorkflowLook.color(app?.color ?: "", Fm.colors.accent)) },
                                trailing = { FmStatusBadge(if (onPhone) "On this phone" else "Account only", if (onPhone) Fm.colors.success else Fm.colors.textSecondary) },
                                onTap = { editing = credential },
                                onLongPress = { deleting = credential },
                            )
                            FmRowDivider()
                        }
                    }
                }
            }
            FmExtendedFloatingActionButton("Credential", onClick = { creating = true }, modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp), icon = "add")
        }
        if (creating) CredentialSheet(session, "", null, onDismiss = { creating = false }) { creating = false }
        editing?.let { credential ->
            CredentialSheet(session, credential.integration, credential, onDismiss = { editing = null }) { editing = null }
        }
        deleting?.let { credential ->
            FmConfirmDialog(
                title = "Delete this credential?",
                body = "Steps that use it stop working on ForkMesh and on this phone.",
                confirmLabel = "Delete",
                destructive = true,
                onDismissRequest = { deleting = null },
                onConfirm = {
                    deleting = null
                    scope.launch {
                        try {
                            withContext(Dispatchers.IO) { session.api.deleteCredential(credential.id) }
                            session.removePhone(credential.id)
                            session.credentials.value = session.credentials.value.filter { it.id != credential.id }
                        } catch (thrown: Exception) {
                            error = WorkflowsText.describe(thrown, "This credential could not be deleted.")
                        }
                    }
                },
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CredentialSheet(
    session: WorkflowsSession,
    integration: String,
    credential: WorkflowCredential?,
    onDismiss: () -> Unit,
    onDone: (WorkflowCredential?) -> Unit,
) {
    val catalog = session.catalog.value
    var app by remember { mutableStateOf(integration) }
    var name by remember { mutableStateOf("") }
    var values by remember { mutableStateOf<Map<String, String>>(emptyMap()) }
    var keepOnPhone by remember { mutableStateOf(true) }
    var saving by remember { mutableStateOf(false) }
    val status = rememberFmStatus()
    val scope = rememberCoroutineScope()
    val kind = catalog.kind("app.$app")
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        FmPanelHeader(title = if (credential == null) "New credential" else credential.name.ifEmpty { "Credential" })
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).imePadding().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            FmInlineStatus(status)
            if (credential == null) {
                Label("Integration")
                Choice(listOf("" to "Choose an integration") + catalog.integrations.filter { it.needsCredential }.map { it.appKey to it.name }, app) { app = it }
                Label("Name")
                FmTextField(value = name, onValueChange = { name = it }, hint = (kind?.name ?: "Integration") + " account")
            } else {
                Text("Enter the values again to keep a copy in this phone's secure storage. ForkMesh never sends saved values back.",
                     style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            }
            for (field in kind?.credentialFields ?: emptyList()) {
                Label(field.label)
                FmTextField(
                    value = values[field.key] ?: "",
                    onValueChange = { values = values + (field.key to it) },
                    hint = field.placeholder.ifEmpty { null },
                    visualTransformation = if (field.secret) PasswordVisualTransformation() else VisualTransformation.None,
                )
            }
            if (credential == null) Toggle("Keep a copy on this phone for phone runs", keepOnPhone) { keepOnPhone = it }
            FmToneButton(
                if (saving) "Saving…" else if (credential == null) "Save credential" else "Save on this phone",
                onPressed = if (saving || kind == null) null else ({
                    val filled = values.mapValues { it.value.trim() }
                    if (credential != null) {
                        session.savePhone(credential, filled)
                        onDone(credential)
                    } else {
                        saving = true
                        scope.launch {
                            try {
                                val created = withContext(Dispatchers.IO) { session.api.createCredential(kind.appKey, name.trim(), filled) }
                                if (keepOnPhone) session.savePhone(created, filled)
                                session.credentials.value = listOf(created) + session.credentials.value
                                onDone(created)
                            } catch (thrown: Exception) {
                                status.error(WorkflowsText.describe(thrown, "This credential could not be saved."))
                            }
                            saving = false
                        }
                    }
                }),
                tone = FmTone.Accent, filled = true, modifier = Modifier.fillMaxWidth(),
            )
            if (credential != null && credential.id in session.phoneIds.value) {
                FmToneButton("Remove from this phone", onPressed = {
                    session.removePhone(credential.id)
                    onDone(null)
                }, tone = FmTone.Danger, modifier = Modifier.fillMaxWidth())
            }
        }
    }
}
