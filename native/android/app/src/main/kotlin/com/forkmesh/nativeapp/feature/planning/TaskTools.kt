package com.forkmesh.nativeapp.feature.planning

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
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
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.InboxService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.planning.IssueFiler
import com.forkmesh.nativeapp.planning.NoteConversions
import com.forkmesh.nativeapp.planning.PlanningConversion
import com.forkmesh.nativeapp.planning.PlanningErrors
import com.forkmesh.nativeapp.planning.TaskAgentInfo
import com.forkmesh.nativeapp.planning.TaskOperations
import com.forkmesh.nativeapp.tasks.TasksApi
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal fun taskOperations(): TaskOperations? {
    val api = Services.get<ApiService>() ?: return null
    val tasks = TasksApi(api)
    return TaskOperations { method, suffix, body -> tasks.call(method, suffix, body) }
}

internal fun issueFiler(): IssueFiler? {
    val inbox = Services.get<InboxService>() ?: return null
    return IssueFiler { owner, name, title, body -> inbox.submitNewIssue(owner, name, title, body) }
}

private enum class TaskPrompt(val title: String, val hint: String) {
    FOLLOW_UP("Follow-up task", "Title"),
    ASSIGN_AGENT("Assign to an agent", "owner/repository"),
    PRIORITY("Priority (1–99)", "50"),
    MOVE_TO_ISSUE("Move to an issue in", "owner/repository"),
}

@Composable
internal fun PromptDialog(title: String, hint: String, initial: String, confirm: String, onDismiss: () -> Unit, onSubmit: (String) -> Unit) {
    var text by remember { mutableStateOf(initial) }
    FmConfirmDialog(
        title = title,
        onConfirm = { onSubmit(text) },
        onDismissRequest = onDismiss,
        confirmLabel = confirm,
        fields = { FmTextField(text, { text = it }, hint = hint, autofocus = true) },
    )
}

class TaskToolsScreen(private val taskId: String) : Screen() {
    override val title: String get() = "Task"

    private var task by mutableStateOf<JsonMap?>(null)
    private var history by mutableStateOf(emptyList<com.forkmesh.nativeapp.planning.TaskHistoryEntry>())
    private var error by mutableStateOf("")
    private var loading by mutableStateOf(true)
    private var busy by mutableStateOf(false)
    private var generation by mutableIntStateOf(1)
    private var prompt by mutableStateOf<TaskPrompt?>(null)
    private val status = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val api = Services.get<ApiService>() ?: return
        val ops = remember { taskOperations() } ?: return
        val scope = rememberCoroutineScope()
        LaunchedEffect(generation) {
            loading = true
            try {
                task = withContext(Dispatchers.IO) { TasksApi(api).task(taskId) }
                withContext(Dispatchers.IO) { runCatching { ops.loadHistory(taskId) } }
                history = ops.history
                error = ""
            } catch (thrown: Exception) {
                error = PlanningErrors.text(thrown, "The task could not be loaded.")
            }
            loading = false
        }
        fun run(done: String, work: () -> Unit) {
            if (busy) return
            busy = true
            status.clear()
            scope.launch {
                val problem = withContext(Dispatchers.IO) { runCatching(work).exceptionOrNull() }
                busy = false
                if (problem == null) status.show(done) else status.error(problem.message ?: "That did not go through.")
                if (problem == null) generation++
            }
        }
        val current = task
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(current?.str("title")?.ifEmpty { null } ?: "Task", navigator) {
                FmIconButton("refresh", "Refresh", onPressed = { generation++ }, color = Fm.colors.textPrimary)
            }
            when {
                loading && current == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                current == null -> FmEmptyState("task_alt", "Task unavailable", message = error)
                else -> LazyColumn(Modifier.fillMaxSize()) {
                    val agent = TaskAgentInfo.of(current)
                    item(key = "status") { FmInlineStatus(status, Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) }
                    if (agent != null) item { FmListRow(agent.label, leading = { FmIcon("smart_toy", tint = Fm.colors.accent) }, showChevron = false) }
                    item { FmSectionHeader("Tools", Modifier.padding(16.dp)) }
                    item {
                        FmListRow("Start an agent", leading = { FmIcon("play_arrow") }, onTap = {
                            navigator.push(AgentsFeature.startSheetScreen(PlanningConversion.agentPrompt(current)))
                        })
                    }
                    item { FmListRow("Follow-up task", leading = { FmIcon("subdirectory_arrow_right") }, onTap = { prompt = TaskPrompt.FOLLOW_UP }) }
                    item { FmListRow("Assign to an agent", leading = { FmIcon("smart_toy") }, onTap = { prompt = TaskPrompt.ASSIGN_AGENT }) }
                    item { FmListRow("Priority", subtitle = current.int("priority", 50).toString(), leading = { FmIcon("low_priority") }, onTap = { prompt = TaskPrompt.PRIORITY }) }
                    item { FmListRow("Move to an issue", leading = { FmIcon("bug_report") }, onTap = { prompt = TaskPrompt.MOVE_TO_ISSUE }) }
                    item { FmSectionHeader("History", Modifier.padding(16.dp), count = history.size) }
                    items(history, key = { it.id.ifEmpty { it.createdAtMs.toString() } }) { entry ->
                        FmListRow(
                            entry.summary.ifEmpty { entry.kind },
                            subtitle = "@${entry.actor} · ${relativeTime(entry.createdAtMs)}",
                            showChevron = false,
                        )
                    }
                }
            }
        }
        val open = prompt
        if (open != null && current != null) {
            val initial = when (open) {
                TaskPrompt.PRIORITY -> current.int("priority", 50).toString()
                TaskPrompt.ASSIGN_AGENT, TaskPrompt.MOVE_TO_ISSUE -> current.str("repository")
                TaskPrompt.FOLLOW_UP -> ""
            }
            PromptDialog(open.title, open.hint, initial, "OK", onDismiss = { prompt = null }) { value ->
                prompt = null
                when (open) {
                    TaskPrompt.FOLLOW_UP -> run("Follow-up created") { ops.followUp(taskId, value) }
                    TaskPrompt.ASSIGN_AGENT -> run("Assigned to an agent") { ops.assignToAgent(taskId, value) }
                    TaskPrompt.PRIORITY -> run("Priority changed") { ops.setPriority(taskId, value.trim().toIntOrNull() ?: 0) }
                    TaskPrompt.MOVE_TO_ISSUE -> {
                        val filer = issueFiler()
                        if (filer == null) {
                            status.error("Sign in on this device to file issues.")
                        } else {
                            run("Moved to an issue") { ops.moveToIssue(current, value, filer) }
                            navigator.pop()
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun TaskListTools(tasks: List<JsonMap>, canManage: Boolean, status: FmStatus, onChanged: () -> Unit) {
    val scope = rememberCoroutineScope()
    var menu by remember { mutableStateOf(false) }
    var adding by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf(false) }
    Box {
        FmIconButton("playlist_add", "Task tools", onPressed = { menu = true }, color = Fm.colors.textPrimary)
        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            FmDropdownMenuItem("Quick add", onClick = { menu = false; adding = true })
            if (canManage) FmDropdownMenuItem("Delete closed tasks", onClick = { menu = false; deleting = true }, textColor = Fm.colors.danger)
        }
    }
    if (adding) {
        PromptDialog("Quick add", "Title", "", "Add", onDismiss = { adding = false }) { title ->
            adding = false
            val ops = taskOperations() ?: return@PromptDialog
            status.clear()
            scope.launch {
                val problem = withContext(Dispatchers.IO) { runCatching { ops.quickAdd(title) }.exceptionOrNull() }
                if (problem == null) status.show("Task added") else status.error(problem.message ?: "The task was not added.")
                if (problem == null) onChanged()
            }
        }
    }
    if (deleting) {
        val closed = tasks.count { it.str("status") == "done" }
        FmConfirmDialog(
            title = "Delete closed tasks?",
            body = "$closed closed task${if (closed == 1) "" else "s"} will be deleted for everyone.",
            confirmLabel = "Delete",
            destructive = true,
            onDismissRequest = { deleting = false },
            onConfirm = {
                deleting = false
                val ops = taskOperations() ?: return@FmConfirmDialog
                status.clear()
                scope.launch {
                    val (deleted, failed) = withContext(Dispatchers.IO) { ops.deleteClosed(tasks) }
                    status.report(if (failed > 0) "Deleted $deleted, $failed failed" else "Deleted $deleted", failed > 0)
                    onChanged()
                }
            },
        )
    }
}

@Composable
fun TaskToolsButton(taskId: String, navigator: Navigator) {
    FmIconButton("tune", "Task tools", onPressed = { navigator.push(TaskToolsScreen(taskId)) }, color = Fm.colors.textPrimary)
}

internal fun notePublicUrl(note: JsonMap?, id: String?): String? {
    if (note?.str("visibility") != "public" || id.isNullOrEmpty()) return null
    val origin = Services.get<ApiService>()?.gitOrigin ?: return null
    return "$origin/notes/$id"
}

@Composable
fun NoteConvertItems(note: JsonMap?, id: String?, title: () -> String, markdown: () -> String, status: FmStatus, scope: CoroutineScope, dismiss: () -> Unit) {
    var asking by remember { mutableStateOf(false) }
    FmDropdownMenuItem("Turn into a task", enabled = id != null, onClick = {
        dismiss()
        val api = Services.get<ApiService>() ?: return@FmDropdownMenuItem
        val tasks = TasksApi(api)
        val noteTitle = title()
        val text = markdown()
        val link = notePublicUrl(note, id)
        status.clear()
        scope.launch {
            val problem = withContext(Dispatchers.IO) {
                runCatching { NoteConversions.toTask(noteTitle, text, link) { m, s, b -> tasks.call(m, s, b) } }.exceptionOrNull()
            }
            if (problem == null) status.show("Task created from this note") else status.error(problem.message ?: "The task was not created.")
        }
    })
    FmDropdownMenuItem("Turn into an issue", enabled = id != null, onClick = { asking = true })
    if (asking) {
        PromptDialog("Issue in which repository?", "owner/repository", "", "File issue", onDismiss = { asking = false; dismiss() }) { repository ->
            asking = false
            dismiss()
            val filer = issueFiler()
            if (filer == null) {
                status.error("Sign in on this device to file issues.")
                return@PromptDialog
            }
            val noteTitle = title()
            val text = markdown()
            val link = notePublicUrl(note, id)
            status.clear()
            scope.launch {
                val problem = withContext(Dispatchers.IO) { NoteConversions.toIssue(noteTitle, text, link, repository, filer) }
                if (problem == null) status.show("Issue filed from this note") else status.error(problem)
            }
        }
    }
}
