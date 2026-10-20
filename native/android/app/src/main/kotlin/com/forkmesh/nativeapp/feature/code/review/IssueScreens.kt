package com.forkmesh.nativeapp.feature.code.review

import com.forkmesh.nativeapp.ui.kit.confirmAction
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.collab.IssueBoard
import com.forkmesh.nativeapp.collab.IssueBoardSession
import com.forkmesh.nativeapp.collab.IssueChecklist
import com.forkmesh.nativeapp.collab.IssueEdit
import com.forkmesh.nativeapp.collab.IssueFields
import com.forkmesh.nativeapp.collab.IssueLinks
import com.forkmesh.nativeapp.collab.IssueModelMentions
import com.forkmesh.nativeapp.collab.IssueTaskMove
import com.forkmesh.nativeapp.collab.IssueThreadText
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.EmptyCard
import com.forkmesh.nativeapp.feature.code.ErrorCard
import com.forkmesh.nativeapp.feature.code.InfoCard
import com.forkmesh.nativeapp.feature.code.LoadingCard
import com.forkmesh.nativeapp.feature.code.rememberCopyText
import com.forkmesh.nativeapp.feature.tools.TaskDetailScreen
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.tasks.TasksApi
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
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
import kotlinx.coroutines.launch

private fun openEventId(issue: Issue): String = issue.events.firstOrNull { it.type == "open" }?.id.orEmpty()

@Composable
internal fun IssueToolsCard(repo: Repository, issue: Issue, navigator: Navigator, onChanged: (Issue) -> Unit) {
    val scope = rememberCoroutineScope()
    val messenger = LocalFmMessenger.current
    val status = rememberFmStatus()
    val copy = rememberCopyText(status)
    val writer = remember(repo.fullName) { CodeServices.writer(repo) }
    val checklist = remember(issue.body) { IssueChecklist.items(issue.body) }
    var canManage by remember(repo.fullName) { mutableStateOf(false) }
    var moving by remember { mutableStateOf(false) }
    LaunchedEffect(repo.fullName) { canManage = io { CodeServices.canManageIssues(repo) } }
    suspend fun apply(edits: List<IssueEdit>, ok: String): Boolean {
        if (edits.isEmpty()) return false
        val failure = io { runCatching { writer.applyIssue(issue.number, edits) }.exceptionOrNull() }
        if (failure == null) status.show("$ok Pending the owner's node.") else status.error("Failed: ${failure.message ?: failure}")
        return failure == null
    }
    InfoCard("Issue tools") {
        if (checklist.isNotEmpty()) {
            val (done, total) = IssueChecklist.progress(issue.body)
            Note("Checklist $done of $total")
            for (item in checklist) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmCheckbox(checked = item.done, onCheckedChange = {
                        val target = openEventId(issue)
                        val body = IssueChecklist.toggled(issue.body, item.lineIndex) ?: return@FmCheckbox
                        if (target.isEmpty()) return@FmCheckbox
                        scope.launch {
                            if (apply(listOf(IssueEdit.editBody(target, body)), "Checklist updated.")) onChanged(issue.copy(body = body))
                        }
                    }, enabled = writer.signer != null)
                    Text(item.text, color = Fm.colors.textPrimary)
                }
            }
        }
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            if (writer.signer != null) {
                FmToneButton("Edit", onPressed = { navigator.push(IssueEditScreen(repo, issue, onChanged, status)) }, icon = "edit")
                FmToneButton(if (issue.isOpen) "Close" else "Reopen", onPressed = {
                    scope.launch { if (apply(listOf(IssueEdit.status(!issue.isOpen)), if (issue.isOpen) "Closed." else "Reopened.")) onChanged(issue.copy(status = if (issue.isOpen) "closed" else "open")) }
                })
                val target = openEventId(issue)
                if (target.isNotEmpty()) {
                    FmToneButton("+1", onPressed = { scope.launch { apply(listOf(IssueEdit.react(target, "+1")), "Reacted.") } })
                }
                FmToneButton("Ask AI", onPressed = {
                    scope.launch {
                        val question = messenger.showPromptDialog(
                            "Ask a model on this issue",
                            body = "Posts a comment that starts with @${IssueModelMentions.aliases.first()}; the owner's desktop answers in the thread.",
                            hint = "Question",
                            confirmLabel = "Post",
                        ) ?: return@launch
                        if (question.isNotEmpty()) apply(listOf(IssueEdit.comment("@${IssueModelMentions.aliases.first()} $question")), "Asked.")
                    }
                }, icon = "smart_toy_outlined")
                FmToneButton("Link a PR", onPressed = {
                    scope.launch {
                        val number = messenger.showPromptDialog("Link a pull request", hint = "Pull request number", confirmLabel = "Link")
                            ?.trim()?.removePrefix("#")?.toIntOrNull() ?: return@launch
                        apply(listOf(IssueEdit.comment(IssueLinks.linkComment(number))), "Linked.")
                    }
                })
            }
            FmToneButton("Copy", onPressed = { copy("Issue", IssueThreadText.copyText(repo.fullName, issue, includeThread = false)) })
            FmToneButton("Copy thread", onPressed = { copy("Thread", IssueThreadText.copyText(repo.fullName, issue, includeThread = true)) })
            FmToneButton("Send to prompt", onPressed = {
                navigator.push(AgentsFeature.startSheetScreen(IssueThreadText.promptText(repo.fullName, issue)))
            }, icon = "send")
            FmToneButton("Agent answer", onPressed = {
                navigator.push(AgentsFeature.startSheetScreen(IssueModelMentions.agentPrompt(issue.number, issue.title, "")))
            })
            if (canManage && writer.signer != null) {
                FmToneButton(if (moving) "Moving…" else "Move to task", onPressed = {
                    if (moving) return@FmToneButton
                    scope.launch {
                        if (!messenger.confirmAction(
                                "Move issue #${issue.number} to a task?",
                                "Move to task",
                                "An organization task is created from its title and description, then the issue is deleted. If the issue cannot be deleted, the new task is moved to trash.",
                            )
                        ) return@launch
                        moving = true
                        val result = io {
                            runCatching {
                                val tasks = TasksApi(CodeServices.api)
                                IssueTaskMove.move(issue, writer, CodeServices.account) { method, suffix, body -> tasks.call(method, suffix, body) }
                            }
                        }
                        moving = false
                        result.onSuccess { task ->
                            navigator.pop()
                            navigator.push(TaskDetailScreen(task.str("id"), CodeServices.account, canManage))
                        }.onFailure { status.error(it.message ?: "The issue could not be moved.") }
                    }
                }, icon = "checklist")
            }
            if (IssueFields.canDeleteOwn(issue, CodeServices.identity?.publicKeyB64url.orEmpty())) {
                FmToneButton("Delete", onPressed = {
                    scope.launch {
                        if (messenger.confirmAction("Delete issue #${issue.number}?", "Delete", "A signed delete hides the issue everywhere once the owner's node applies it. Only you, its author, can do this.")) {
                            if (apply(listOf(IssueEdit.deleteOwnIssue()), "Delete requested.")) navigator.pop()
                        }
                    }
                }, tone = FmTone.Danger)
            }
        }
        FmInlineStatus(status)
    }
}

internal class IssueEditScreen(
    private val repo: Repository,
    private val issue: Issue,
    private val onChanged: (Issue) -> Unit,
    private val resultStatus: FmStatus? = null,
) : Screen() {
    override val title: String get() = "Edit #${issue.number}"

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val status = rememberFmStatus()
        var title by remember { mutableStateOf(issue.title) }
        var labels by remember { mutableStateOf(issue.labels.joinToString(", ")) }
        var assignees by remember { mutableStateOf(issue.assignees.joinToString(", ")) }
        var milestone by remember { mutableStateOf(issue.milestone) }
        var priority by remember { mutableIntStateOf(issue.priority) }
        var progress by remember { mutableFloatStateOf(issue.progress.toFloat()) }
        var open by remember { mutableStateOf(issue.isOpen) }
        var busy by remember { mutableStateOf(false) }
        DetailScaffold(title, navigator, status = status, actions = {
            FmToneButton(if (busy) "Saving…" else "Save", onPressed = {
                if (busy) return@FmToneButton
                val edits = IssueFields.diff(
                    issue,
                    title = title,
                    labels = IssueFields.parseList(labels),
                    assignees = IssueFields.parseList(assignees),
                    priority = priority,
                    progress = progress.toInt(),
                    milestone = milestone,
                    open = open,
                )
                if (edits.isEmpty()) {
                    navigator.pop()
                    return@FmToneButton
                }
                scope.launch {
                    busy = true
                    val writer = CodeServices.writer(repo)
                    val failure = io { runCatching { writer.applyIssue(issue.number, edits) }.exceptionOrNull() }
                    busy = false
                    if (failure != null) {
                        status.error("Failed: ${failure.message ?: failure}")
                    } else {
                        onChanged(
                            issue.copy(
                                title = title.trim().ifEmpty { issue.title },
                                labels = IssueFields.cleanList(IssueFields.parseList(labels), IssueEdit.MAX_LABELS, IssueEdit.MAX_LABEL_LENGTH),
                                assignees = IssueFields.cleanList(IssueFields.parseList(assignees).map { it.lowercase() }, IssueEdit.MAX_ASSIGNEES, 64),
                                priority = IssueFields.clampPriority(priority),
                                progress = progress.toInt(),
                                milestone = milestone.trim(),
                                status = if (open) "open" else "closed",
                            ),
                        )
                        resultStatus?.show("Saved ${edits.size} change${if (edits.size == 1) "" else "s"}. Pending the owner's node.")
                        navigator.pop()
                    }
                }
            }, tone = FmTone.Accent, filled = true)
        }) {
            ScreenList {
                item {
                    InfoCard("Details") {
                        FmTextField(title, { title = it }, hint = "Title")
                        FmTextField(labels, { labels = it }, hint = "Labels, comma separated")
                        FmTextField(assignees, { assignees = it }, hint = "Assignees, comma separated")
                        FmTextField(milestone, { milestone = it }, hint = "Milestone")
                        Note("Priority: ${IssueFields.priorityLabel(priority)}")
                        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            for ((value, label) in IssueFields.PRIORITIES) FmToneButton(label, onPressed = { priority = value }, filled = priority == value)
                        }
                        Note("Progress: ${progress.toInt()}%")
                        Slider(progress, {
                            if (it != progress) FmSoundCues.play(SoundCue.SELECT)
                            progress = it
                        }, valueRange = 0f..100f, steps = 19)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text("Open", Modifier.weight(1f), color = Fm.colors.textPrimary)
                            FmSwitch(open, { open = it })
                        }
                    }
                }
            }
        }
    }
}

internal class IssueBoardScreen(private val repo: Repository) : Screen() {
    private val session = IssueBoardSession(
        repo.owner,
        repo.name,
        CodeServices.prefs ?: MemoryPrefStore(),
        CodeServices.writer(repo),
    ) { CodeServices.api.publishedIssues(repo.owner, repo.name) }

    override val title: String get() = "Board"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val status = rememberFmStatus()
        var query by remember { mutableStateOf("") }
        LaunchedEffect(Unit) {
            io { session.refresh() }
            tick.bump()
        }
        tick.value
        DetailScaffold("Board · ${repo.name}", navigator, actions = {
            FmIconButton("view_column", "Columns", onPressed = {
                scope.launch {
                    val text = messenger.showPromptDialog(
                        "Board columns",
                        body = "Comma separated. The last column is Done; the others set a status: label.",
                        initialValue = session.columns.joinToString(", "),
                        confirmLabel = "Save",
                    ) ?: return@launch
                    session.saveColumns(IssueFields.parseList(text))?.let { status.error(it) } ?: status.clear()
                    tick.bump()
                }
            })
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { io { session.refresh() }; tick.bump() } })
        }) {
            Column(Modifier.fillMaxSize()) {
                FmTextField(query, { query = it; session.query = it; tick.bump() }, Modifier.padding(FmSpace.x3), hint = "Filter by title, #number or label")
                FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x3))
                when {
                    session.loadError != null && session.issues.isEmpty() ->
                        Column(Modifier.padding(FmSpace.x4)) { ErrorCard(session.loadError!!, { scope.launch { io { session.refresh() }; tick.bump() } }) }
                    session.loading && session.issues.isEmpty() -> Column(Modifier.padding(FmSpace.x4)) { LoadingCard("Loading issues…") }
                    else -> LazyRow(
                        Modifier.fillMaxSize(),
                        contentPadding = androidx.compose.foundation.layout.PaddingValues(FmSpace.x3),
                        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                    ) {
                        items(session.layout, key = { it.name }) { column ->
                            Column(Modifier.width(280.dp).fillMaxHeight()) {
                                Text("${column.name} · ${column.issues.size}", style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
                                LazyColumn(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                    items(column.issues, key = { it.number }) { issue ->
                                        val (left, right) = IssueBoard.neighbours(column.name, session.columns)
                                        FmCard(padding = androidx.compose.foundation.layout.PaddingValues(FmSpace.x3)) {
                                            Column {
                                                Text("#${issue.number} ${issue.title}", color = Fm.colors.textPrimary)
                                                if (issue.priority > 0) Note(IssueFields.priorityLabel(issue.priority))
                                                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                                    val moving = session.movingNumber == issue.number
                                                    left?.let { target ->
                                                        FmToneButton("← $target", onPressed = {
                                                            if (!moving) scope.launch { io { session.move(issue, target) }?.let { status.report(it.message, it.isError) }; tick.bump() }
                                                        })
                                                    }
                                                    right?.let { target ->
                                                        FmToneButton("$target →", onPressed = {
                                                            if (!moving) scope.launch { io { session.move(issue, target) }?.let { status.report(it.message, it.isError) }; tick.bump() }
                                                        })
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                if (column.issues.isEmpty()) Note("No issues.")
                            }
                        }
                    }
                }
            }
        }
    }
}

internal class ReprioritizeScreen(private val repo: Repository) : Screen() {
    override val title: String get() = "Reprioritize"

    @Composable
    override fun Content(navigator: Navigator) {
        var open by remember { mutableIntStateOf(-1) }
        LaunchedEffect(Unit) {
            open = io { runCatching { CodeServices.api.publishedIssues(repo.owner, repo.name).count { it.isOpen } }.getOrDefault(0) }
        }
        DetailScaffold("Reprioritize backlog", navigator) {
            ScreenList {
                item {
                    if (open < 0) {
                        LoadingCard("Counting open issues…")
                    } else if (open == 0) {
                        EmptyCard("There are no open issues.")
                    } else {
                        InfoCard("$open open issues") {
                            Note("An agent reads the issues and sets each one's priority.")
                            FmToneButton("By impact and effort", onPressed = {
                                navigator.push(AgentsFeature.startSheetScreen(IssueModelMentions.reprioritizePrompt(open, fromReadme = false)))
                            })
                            FmToneButton("From the README", onPressed = {
                                navigator.push(AgentsFeature.startSheetScreen(IssueModelMentions.reprioritizePrompt(open, fromReadme = true)))
                            })
                        }
                    }
                }
            }
        }
    }
}
