package com.forkmesh.nativeapp.feature.tools

import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.collab.ReferenceLinks
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.notes.jsonInt
import com.forkmesh.nativeapp.notes.jsonObject
import com.forkmesh.nativeapp.notes.jsonObjects
import com.forkmesh.nativeapp.notes.jsonString
import com.forkmesh.nativeapp.tasks.TaskBoard
import com.forkmesh.nativeapp.tasks.TaskFilter
import com.forkmesh.nativeapp.tasks.TaskPerson
import com.forkmesh.nativeapp.tasks.TasksApi
import com.forkmesh.nativeapp.tasks.TasksLogic
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmCountPill
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.shell.ShellBadges
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal fun tasksApi(): TasksApi? = Services.get<ApiService>()?.let(::TasksApi)

@Composable
private fun statusColor(task: JsonObject) = when (jsonString(task["status"])) {
    "active" -> Fm.colors.accent
    "done" -> Fm.colors.success
    else -> Fm.colors.textTertiary
}

class TasksScreen(private val initialFilter: Int = 0) : Screen() {
    override val title: String get() = "Tasks"

    private var board by mutableStateOf<TaskBoard?>(null)
    private var loading by mutableStateOf(true)
    private var loadingMore by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var filter by mutableStateOf(TaskFilter(tab = initialFilter))
    private var generation by mutableIntStateOf(0)
    private var creating by mutableStateOf(false)
    private val listStatus = FmStatus()
    private val moreStatus = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val api = tasksApi() ?: return
        val scope = rememberCoroutineScope()
        val top = navigator.top
        LaunchedEffect(top) { if (top === this@TasksScreen) generation++ }
        LaunchedEffect(generation) {
            if (generation == 0) return@LaunchedEffect
            loading = true
            error = ""
            try {
                board = withContext(Dispatchers.IO) { api.board() }
                board?.let(ShellBadges::adoptTasks)
            } catch (thrown: Exception) {
                error = TasksLogic.errorText(thrown, "Tasks are unavailable right now.")
            }
            loading = false
        }
        val current = board
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar("Tasks", navigator) {
                    current?.let { com.forkmesh.nativeapp.feature.planning.TaskListTools(it.tasks, it.canManage, listStatus) { generation++ } }
                    FmIconButton("refresh", "Refresh", onPressed = { generation++ }, color = Fm.colors.textPrimary)
                }
                when {
                    loading && current == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    current == null -> FmEmptyState("task_alt", "Tasks are unavailable", message = error)
                    else -> {
                        val shown = filter.within(current)
                        val lists = remember(current, shown.person) { current.lists(shown.person) }
                        FmSegmentTabBar(
                            tabs = listOf(
                                FmSegmentTab("Mine", lists[0].size, icon = "person"),
                                FmSegmentTab("Open", lists[1].size, icon = "radio_button_unchecked"),
                                FmSegmentTab("Done", lists[2].size, icon = "check_circle"),
                            ),
                            index = shown.tab,
                            onChanged = { filter = shown.selectTab(it) },
                        )
                        TaskPeopleStrip(current, shown.person) { filter = shown.selectPerson(it) }
                        FmInlineStatus(listStatus, Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
                        val visible = lists[shown.tab]
                        if (visible.isEmpty() && current.nextOffset == null) {
                            FmEmptyState("task_alt", TasksLogic.emptyTitle(shown.tab, current.people.firstOrNull { it.key == shown.person }))
                        } else {
                            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 96.dp)) {
                                items(visible, key = { jsonString(it["id"]) }) { task ->
                                    val color = statusColor(task)
                                    FmListRow(
                                        title = jsonString(task["title"], "Organization task"),
                                        subtitle = TasksLogic.rowSubtitle(task),
                                        leading = {
                                            FmIconTile(
                                                when (jsonString(task["status"])) {
                                                    "active" -> "timelapse"
                                                    "done" -> "task_alt"
                                                    else -> "radio_button_unchecked"
                                                },
                                                tint = color,
                                            )
                                        },
                                        trailing = { FmStatusBadge(TasksLogic.statusLabel(task), color) },
                                        onTap = { navigator.push(TaskDetailScreen(jsonString(task["id"]), current.actor, current.canManage)) },
                                    )
                                    FmRowDivider()
                                }
                                val offset = current.nextOffset
                                if (offset != null) {
                                    item { FmInlineStatus(moreStatus, Modifier.padding(horizontal = 16.dp).padding(top = 8.dp)) }
                                    item {
                                        AccountOutlinedButton(
                                            if (loadingMore) "Loading…" else "Load more tasks",
                                            if (loadingMore) null else ({
                                                loadingMore = true
                                                moreStatus.clear()
                                                scope.launch {
                                                    try {
                                                        val page = withContext(Dispatchers.IO) { api.board(offset) }
                                                        board = board?.merged(page)
                                                    } catch (thrown: Exception) {
                                                        moreStatus.error(TasksLogic.errorText(thrown, "More tasks could not be loaded."))
                                                    }
                                                    loadingMore = false
                                                }
                                            }),
                                            modifier = Modifier.fillMaxWidth().padding(16.dp),
                                        )
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (error.isEmpty() && !loading && current != null) {
                FmExtendedFloatingActionButton(
                    "New task",
                    onClick = { creating = true },
                    modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
                    icon = "add",
                )
            }
        }
        if (creating && current != null) {
            TaskFormSheet(api, current.actor, current.canManage, current.members, current.departments, null, onDismiss = { creating = false }) { created ->
                creating = false
                navigator.push(TaskDetailScreen(jsonString(created["id"]), current.actor, current.canManage))
            }
        }
    }
}

class TaskDetailScreen(private val taskId: String, private val actor: String, private val canManage: Boolean) : Screen() {
    override val title: String get() = "Task"

    private var task by mutableStateOf<JsonObject?>(null)
    private var loading by mutableStateOf(true)
    private var busy by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var reply by mutableStateOf("")
    private var menu by mutableStateOf(false)
    private var editing by mutableStateOf(false)
    private var deleting by mutableStateOf(false)
    private var finishing by mutableStateOf(false)
    private var checkingIn by mutableStateOf(false)
    private var reminding by mutableStateOf(false)
    private val actionStatus = FmStatus()
    private val replyStatus = FmStatus()

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val api = tasksApi() ?: return
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            try {
                task = withContext(Dispatchers.IO) { api.task(taskId) }
            } catch (thrown: Exception) {
                error = TasksLogic.errorText(thrown, "This task could not be opened.")
            }
            loading = false
        }
        fun act(suffix: String, body: JsonObject = emptyMap(), success: String = "", status: FmStatus = actionStatus, after: () -> Unit = {}) {
            if (busy) return
            busy = true
            status.clear()
            scope.launch {
                try {
                    withContext(Dispatchers.IO) { api.post(taskId, suffix, body) }?.let { task = it }
                    if (success.isNotEmpty()) status.show(success)
                    after()
                } catch (thrown: Exception) {
                    status.error(TasksLogic.errorText(thrown, "That did not go through."))
                }
                busy = false
            }
        }
        val current = task
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas).imePadding()) {
            ToolsAppBar("Task", navigator) {
                if (current != null) com.forkmesh.nativeapp.feature.planning.TaskToolsButton(taskId, navigator)
                if (current != null && canManage) {
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem("Edit", onClick = { menu = false; editing = true })
                            FmDropdownMenuItem("Delete", onClick = { menu = false; deleting = true })
                        }
                    }
                }
            }
            when {
                loading && current == null -> Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                current == null -> Box(Modifier.weight(1f)) { FmEmptyState("task_alt", "Task unavailable", message = error) }
                else -> {
                    val actions = TasksLogic.actions(current, actor, canManage)
                    val details = jsonString(current["details"]).trim()
                    val completionNote = jsonString(current["completionNote"]).trim()
                    val checkin = current["lastCheckin"]?.let(::jsonObject)
                    val reminder = current["lastReminder"]?.let(::jsonObject)
                    val replies = jsonObjects(current["replies"])
                    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(16.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            FmStatusBadge(TasksLogic.statusLabel(current), statusColor(current))
                            Spacer(Modifier.width(8.dp))
                            Text(TasksLogic.departmentLabel(jsonString(current["department"], "general")), style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                        }
                        Spacer(Modifier.height(8.dp))
                        SelectionContainer { Text(jsonString(current["title"], "Organization task"), style = Fm.text.titleLarge) }
                        if (actions.start || actions.stop || actions.finish || actions.reopen || actions.remind) {
                            Spacer(Modifier.height(12.dp))
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                if (actions.start) FmToneButton("Start", onPressed = if (busy) null else ({ act("/start", success = "Timer started") }), icon = "play_arrow", tone = FmTone.Accent, filled = true)
                                if (actions.stop) FmToneButton("Stop", onPressed = if (busy) null else ({ act("/stop", success = "Timer stopped") }), icon = "pause", tone = FmTone.Warning)
                                if (actions.checkin) FmToneButton("Check in", onPressed = if (busy) null else ({ checkingIn = true }), icon = "flag_outlined")
                                if (actions.remind) FmToneButton("Remind", onPressed = if (busy) null else ({ reminding = true }), icon = "notifications_active_outlined")
                                if (actions.finish) FmToneButton("Finish", onPressed = if (busy) null else ({ finishing = true }), icon = "check", tone = FmTone.Success)
                                if (actions.reopen) FmToneButton("Reopen", onPressed = if (busy) null else ({ act("/undo", success = "Task reopened") }), icon = "undo")
                            }
                        }
                        FmInlineStatus(actionStatus, Modifier.padding(top = 8.dp))
                        Spacer(Modifier.height(12.dp))
                        FmCard(padding = PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
                            Column {
                                FmKeyValueRow("Assignee", TasksLogic.assigneeLabel(current))
                                if (jsonString(current["createdBy"]).isNotEmpty()) FmKeyValueRow("Created by", "@${jsonString(current["createdBy"])}")
                                FmKeyValueRow("Priority", "${jsonInt(current["priority"])}")
                                if (jsonString(current["repository"]).isNotEmpty()) FmKeyValueRow("Repository", jsonString(current["repository"]))
                                FmKeyValueRow("Time spent", TasksLogic.elapsed(jsonInt(current["elapsedMs"])))
                                FmKeyValueRow("Created", relativeTime(jsonInt(current["createdAt"])))
                                if (jsonInt(current["completedAt"]) > 0) FmKeyValueRow("Finished", relativeTime(jsonInt(current["completedAt"])))
                            }
                        }
                        val linkRepo = ReferenceLinks.repoFor(slug = jsonString(current["repository"]))
                        if (details.isNotEmpty()) {
                            Spacer(Modifier.height(16.dp))
                            FmSectionHeader("Details")
                            SelectionContainer { LinkedText(details, linkRepo, style = Fm.text.bodyMedium) }
                        }
                        if (checkin != null && jsonString(checkin["state"]).isNotEmpty()) {
                            Spacer(Modifier.height(16.dp))
                            FmSectionHeader("Last check-in")
                            val parts = mutableListOf(TasksLogic.checkinLabel(jsonString(checkin["state"])), relativeTime(jsonInt(checkin["at"])))
                            if (jsonString(checkin["note"]).isNotEmpty()) parts.add(jsonString(checkin["note"]))
                            LinkedText(parts.joinToString(" · "), linkRepo, style = Fm.text.bodyMedium)
                        }
                        if (reminder != null && jsonInt(reminder["at"]) > 0) {
                            Spacer(Modifier.height(16.dp))
                            FmSectionHeader("Last reminder")
                            val parts = mutableListOf<String>()
                            if (jsonString(reminder["by"]).isNotEmpty()) parts.add("@${jsonString(reminder["by"])}")
                            parts.add(relativeTime(jsonInt(reminder["at"])))
                            if (jsonString(reminder["note"]).isNotEmpty()) parts.add(jsonString(reminder["note"]))
                            LinkedText(parts.joinToString(" · "), linkRepo, style = Fm.text.bodyMedium)
                            Text(TasksLogic.reminderStatus(reminder), style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
                        }
                        if (completionNote.isNotEmpty()) {
                            Spacer(Modifier.height(16.dp))
                            FmSectionHeader("Completion note")
                            SelectionContainer { LinkedText(completionNote, linkRepo, style = Fm.text.bodyMedium) }
                        }
                        Spacer(Modifier.height(16.dp))
                        FmSectionHeader("Replies", count = replies.size)
                        if (replies.isEmpty()) Text("No replies yet.", style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
                        for (entry in replies) {
                            Column(Modifier.padding(bottom = 12.dp)) {
                                Text("@${jsonString(entry["author"])} · ${relativeTime(jsonInt(entry["createdAt"]))}", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                                Spacer(Modifier.height(4.dp))
                                SelectionContainer { LinkedText(jsonString(entry["body"]), linkRepo, style = Fm.text.bodyMedium) }
                            }
                        }
                    }
                    FmInlineStatus(replyStatus, Modifier.background(Fm.colors.bgRaised).padding(horizontal = 16.dp))
                    Row(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(horizontal = 16.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                        FmTextField(value = reply, onValueChange = { reply = it.take(2000) }, hint = "Reply", maxLines = 4, modifier = Modifier.weight(1f))
                        Spacer(Modifier.width(8.dp))
                        FmIconButton("send", "Send reply", onPressed = if (busy) null else ({
                            val body = reply.trim()
                            if (body.isNotEmpty()) act("/replies", mapOf("body" to body), status = replyStatus, after = { reply = "" })
                        }))
                    }
                }
            }
        }
        if (finishing) {
            var note by remember { mutableStateOf("") }
            FmAlertDialog(
                onDismissRequest = { finishing = false },
                title = "Finish task",
                actions = {
                    FmTextButton(onClick = { finishing = false }) { Text("Cancel") }
                    FmTextButton(onClick = {
                        finishing = false
                        val trimmed = note.trim()
                        act("/complete", if (trimmed.isEmpty()) emptyMap() else mapOf("completionNote" to trimmed), "Task finished and sent to review")
                    }) { Text("Finish") }
                },
            ) {
                FmTextField(value = note, onValueChange = { note = it.take(4000) }, hint = "What did you do? (optional)", minLines = 2, maxLines = 6, autofocus = true)
            }
        }
        if (reminding) {
            var note by remember { mutableStateOf("") }
            val assignee = current?.let { jsonString(it["assignee"]) }.orEmpty()
            FmAlertDialog(
                onDismissRequest = { reminding = false },
                title = "Remind @$assignee",
                actions = {
                    FmTextButton(onClick = { reminding = false }) { Text("Cancel") }
                    FmTextButton(onClick = {
                        reminding = false
                        val trimmed = note.trim()
                        act("/remind", if (trimmed.isEmpty()) emptyMap() else mapOf("note" to trimmed), "Reminder sent to @$assignee")
                    }) { Text("Send reminder") }
                },
            ) {
                FmTextField(value = note, onValueChange = { note = it.take(500) }, hint = "Note (optional)", minLines = 2, maxLines = 4, autofocus = true)
            }
        }
        if (checkingIn) CheckinSheet(onDismiss = { checkingIn = false }) { state, note ->
            checkingIn = false
            act("/checkin", if (note.isEmpty()) mapOf("state" to state) else mapOf("state" to state, "note" to note), "Checked in")
        }
        if (editing && current != null) {
            TaskFormSheet(api, actor, canManage, emptyList(), emptyList(), current, onDismiss = { editing = false }) { updated ->
                editing = false
                task = updated
            }
        }
        if (deleting) {
            FmConfirmDialog(
                title = "Delete task?",
                body = "The task, its replies, and its history go away.",
                confirmLabel = "Delete",
                destructive = true,
                onConfirm = {
                    deleting = false
                    actionStatus.clear()
                    scope.launch {
                        try {
                            withContext(Dispatchers.IO) { api.delete(taskId) }
                            navigator.pop()
                        } catch (thrown: Exception) {
                            actionStatus.error(TasksLogic.errorText(thrown, "The task was not deleted."))
                        }
                    }
                },
                onDismissRequest = { deleting = false },
            )
        }
    }
}

@Composable
private fun TaskPeopleStrip(board: TaskBoard, selected: String, onSelect: (String) -> Unit) {
    val people = remember(board) { board.people }
    val counts = remember(board) { board.personOpenCounts() }
    LazyRow(
        Modifier.fillMaxWidth().background(Fm.colors.bgRaised),
        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        items(people, key = { it.key }) { person ->
            val active = person.key == selected
            val open = counts[person.key] ?: 0
            Column(
                Modifier
                    .width(68.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .background(if (active) Fm.colors.accentSubtle else Color.Transparent)
                    .selectable(selected = active, role = Role.Button, onClick = { onSelect(person.key) })
                    .clearAndSetSemantics { contentDescription = "${person.label}, $open open" }
                    .padding(vertical = 6.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Box {
                    Box(
                        Modifier.size(42.dp).border(2.dp, if (active) Fm.colors.accent else Color.Transparent, CircleShape).padding(3.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        TaskPersonAvatar(person, board.actor)
                    }
                    FmCountPill(open, Modifier.align(Alignment.TopEnd).offset(x = 10.dp, y = (-4).dp), active = active)
                }
                Spacer(Modifier.height(4.dp))
                Text(
                    when {
                        person.name.isEmpty() -> person.label
                        person.name == board.actor -> "You"
                        else -> person.name
                    },
                    style = Fm.text.labelSmall.copy(color = if (active) Fm.colors.textPrimary else Fm.colors.textSecondary),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
    FmRowDivider()
}

@Composable
private fun TaskPersonAvatar(person: TaskPerson, actor: String) {
    val icon = when (person.key) {
        TaskPerson.EVERYONE -> "groups"
        TaskPerson.BOT -> "smart_toy"
        TaskPerson.UNASSIGNED -> "person_off"
        else -> null
    }
    if (icon != null) {
        Box(Modifier.size(36.dp).background(Fm.colors.bgElevated, CircleShape), contentAlignment = Alignment.Center) {
            FmIcon(icon, size = 20.dp, tint = Fm.colors.textSecondary)
        }
        return
    }
    val own = if (person.name == actor) Services.get<AuthService>()?.session?.collectAsState()?.value?.avatarPng.orEmpty() else ""
    val avatar by produceState(own, person.name, own) {
        if (own.isNotEmpty()) return@produceState
        val api = Services.get<ApiService>() ?: return@produceState
        value = withContext(Dispatchers.IO) { runCatching { api.accountAvatarPng(person.name) }.getOrDefault("") }
    }
    FmAvatar(name = person.name, size = 36.dp, avatarPng = avatar, textStyle = Fm.text.titleSmall)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CheckinSheet(onDismiss: () -> Unit, onSubmit: (String, String) -> Unit) {
    var state by remember { mutableStateOf("going_well") }
    var note by remember { mutableStateOf("") }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding()) {
            FmPanelHeader(title = "Check in")
            for (option in TasksLogic.CHECKIN_STATES) {
                Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(selected = state == option, onClick = { state = option })
                    Text(TasksLogic.checkinLabel(option), style = Fm.text.bodyLarge)
                }
            }
            FmTextField(value = note, onValueChange = { note = it }, hint = "Note (optional)", maxLines = 4, modifier = Modifier.padding(horizontal = 16.dp))
            AccountFilledButton("Check in", "flag_outlined", onClick = { onSubmit(state, note.trim()) }, modifier = Modifier.fillMaxWidth().padding(16.dp))
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
private fun TaskFormSheet(
    api: TasksApi,
    actor: String,
    canManage: Boolean,
    members: List<String>,
    departments: List<String>,
    existing: JsonObject?,
    onDismiss: () -> Unit,
    onSaved: (JsonObject) -> Unit,
) {
    val creating = existing == null
    var title by remember { mutableStateOf(jsonString(existing?.get("title"))) }
    var details by remember { mutableStateOf(jsonString(existing?.get("details"))) }
    var department by remember { mutableStateOf("general") }
    var assignee by remember { mutableStateOf(actor) }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    val departmentChoices = departments.ifEmpty { listOf("general") }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.imePadding().verticalScroll(rememberScrollState())) {
            FmPanelHeader(title = if (creating) "New task" else "Edit task")
            Column(Modifier.padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                FmTextField(value = title, onValueChange = { title = it.take(160) }, hint = "Title", autofocus = creating)
                FmTextField(value = details, onValueChange = { details = it.take(4000) }, hint = "Details", minLines = 3, maxLines = 8)
                if (creating) {
                    Text("Department", style = Fm.text.labelMedium)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        for (choice in departmentChoices) {
                            FmToneButton(TasksLogic.departmentLabel(choice), onPressed = { department = choice }, tone = if (department == choice) FmTone.Accent else FmTone.Neutral, filled = department == choice)
                        }
                    }
                    if (canManage) {
                        Text("Assignee", style = Fm.text.labelMedium)
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            for (name in TasksLogic.assignees(actor, canManage, members) + "") {
                                val label = when {
                                    name.isEmpty() -> "Unassigned"
                                    name == actor -> "@$name (you)"
                                    else -> "@$name"
                                }
                                FmToneButton(label, onPressed = { assignee = name }, tone = if (assignee == name) FmTone.Accent else FmTone.Neutral, filled = assignee == name)
                            }
                        }
                    }
                }
                FmInlineStatus(status)
            }
            AccountFilledButton(
                if (busy) "Saving…" else if (creating) "Create task" else "Save",
                "check",
                enabled = !busy,
                modifier = Modifier.fillMaxWidth().padding(16.dp),
                onClick = {
                    if (title.trim().isEmpty() || busy) return@AccountFilledButton
                    busy = true
                    status.clear()
                    scope.launch {
                        try {
                            val saved = withContext(Dispatchers.IO) {
                                if (existing == null) api.create(title, details, department, assignee) else api.update(jsonString(existing["id"]), title, details)
                            }
                            onSaved(saved)
                        } catch (thrown: Exception) {
                            busy = false
                            status.error(TasksLogic.errorText(thrown, "The task was not saved."))
                        }
                    }
                },
            )
        }
    }
}
