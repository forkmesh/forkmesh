package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
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
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.agents.RunnerAgentDiff
import com.forkmesh.nativeapp.agents.RunnerDiffModel
import com.forkmesh.nativeapp.agents.RunnerMergeModel
import com.forkmesh.nativeapp.agents.RunnerPullDraft
import com.forkmesh.nativeapp.agents.RunnerPullInbox
import com.forkmesh.nativeapp.agents.RunnerSessionRef
import com.forkmesh.nativeapp.agents.RunnerTaskCompleter
import com.forkmesh.nativeapp.agents.RunnerTaskLink
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.tasks.TasksApi
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.InboxService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.model.RepoComparison
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.DiffMenuAction
import com.forkmesh.nativeapp.feature.code.DiffReviewStream
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.feature.code.rememberCopyText
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.launch

internal fun UnifiedAgentRow.runnerRef(): RunnerSessionRef = RunnerSessionRef(repoOwner, repoName, cloudSessionId, orgScope)

internal object RunnerPullDevice : RunnerPullInbox {
    override fun defaultBranch(owner: String, repo: String): String {
        val branches = try {
            Services.require<ApiService>().branches(owner, repo)
        } catch (_: Exception) {
            emptyList()
        }
        return branches.firstOrNull { it.isDefault }?.name ?: branches.firstOrNull()?.name ?: "main"
    }

    override fun compare(owner: String, repo: String, base: String, head: String): RepoComparison =
        Services.require<ApiService>().compare(owner, repo, base, head)

    override fun submit(owner: String, repo: String, draft: RunnerPullDraft, comparison: RepoComparison): Int {
        val inbox = Services.get<InboxService>()
            ?: throw IllegalStateException("This device has no signing identity, so it cannot open pull requests.")
        return inbox.submitNewPull(
            owner,
            repo,
            title = draft.title,
            base = draft.base,
            head = draft.head,
            patch = comparison.patch,
            description = draft.description,
            authorName = Services.get<AuthService>()?.core?.session?.nodeName.orEmpty(),
            commits = comparison.commits,
        )
    }
}

internal object RunnerTaskDevice : RunnerTaskCompleter {
    override fun complete(taskId: String, note: String) {
        TasksApi(Services.require<ApiService>()).post(taskId, "/complete", mapOf("completionNote" to note))
    }
}

internal class RunnerDiffScreen(
    private val ref: RunnerSessionRef,
    private val agentTitle: String,
    private val mergeRow: UnifiedAgentRow? = null,
    private val onMerged: (String) -> Unit = {},
) : Screen() {
    override val title: String get() = "Changes"
    private var showMerge by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val model = remember(ref) { RunnerDiffModel(AgentsFeature.runnerOps, ref, Services.get<PrefStore>()) }
        val tick = rememberOpsTick(model)
        val scope = rememberCoroutineScope()
        val copyStatus = rememberFmStatus()
        val copy = rememberCopyText(copyStatus)
        LaunchedEffect(model) { opsIo { model.load() } }
        val row = mergeRow
        if (showMerge && row != null) {
            RunnerMergeDialog(row, cleanupOnly = false, onDone = { message ->
                navigator.pop()
                onMerged(message)
            }, onDismiss = { showMerge = false }, onActions = { message, commit ->
                navigator.pop()
                onMerged(message)
                navigator.push(CodeFeature.actionsScreen(Repository(row.repoOwner, row.repoName), commit))
            })
        }
        DetailScaffold(title, navigator, actions = {
            if (row != null) FmToneButton("Merge & clean up", onPressed = { showMerge = true }, icon = "call_merge", tone = FmTone.Success)
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { opsIo { model.load() } } }, color = c.textPrimary)
        }) {
            tick.let { }
            val diff = model.diff
            val review = model.review
            if (diff != null && review != null) {
                DiffReviewStream(
                    model = review,
                    status = copyStatus,
                    extra = buildList {
                        if (diff.diff.committed.isNotEmpty()) add(DiffMenuAction("Copy patch", "content_copy") { copy("Patch", diff.diff.committed) })
                    },
                    emptyMessage = "No changes against the base branch.",
                    onRefresh = { scope.launch { opsIo { model.load() } } },
                    header = {
                        RunnerDiffSummary(agentTitle, diff, model.errorText, model.loading) {
                            scope.launch { opsIo { model.loadMore() } }
                        }
                    },
                )
            } else {
                Column(Modifier.padding(FmSpace.x4)) {
                    Text(agentTitle, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                    if (model.loading) {
                        Text("Reading the changes from ForkMesh…", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                    }
                    if (model.errorText.isNotEmpty()) Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
                }
            }
        }
    }
}

@Composable
private fun RunnerDiffSummary(agentTitle: String, runner: RunnerAgentDiff, errorText: String, loading: Boolean, onMore: () -> Unit) {
    val c = Fm.colors
    val diff = runner.diff
    Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x3, vertical = FmSpace.x2)) {
        Text(agentTitle, style = Fm.text.titleSmall.copy(color = c.textPrimary))
        Row {
            Text(
                "${diff.base.ifEmpty { "base" }}...${diff.branch}",
                style = Fm.text.bodySmall.copy(color = c.textPrimary, fontFamily = FmType.mono),
                modifier = Modifier.weight(1f),
                maxLines = 1,
            )
            Text("+${diff.added}", style = Fm.text.bodySmall.copy(color = c.success))
            Spacer(Modifier.width(FmSpace.x2))
            Text("−${diff.removed}", style = Fm.text.bodySmall.copy(color = c.danger))
        }
        val facts = listOf(
            if (runner.commits > 0) "${runner.commits} commit${if (runner.commits == 1) "" else "s"}" else "",
            "${diff.files.size} file${if (diff.files.size == 1) "" else "s"}",
            if (diff.merged) "Merged" else "",
        ).filter { it.isNotEmpty() }.joinToString(" · ")
        Text(facts, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        if (runner.syncNote.isNotEmpty()) {
            Text(runner.syncNote, style = Fm.text.bodySmall.copy(color = if (runner.clean == false) c.danger else c.warning))
        }
        if (runner.hasMore) {
            Row {
                Text(
                    "Showing ${runner.loadedFiles} of ${diff.files.size} files",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                    modifier = Modifier.weight(1f),
                )
                FmOutlinedButton("Load more", onClick = if (loading) null else onMore, icon = "expand_more")
            }
        }
        if (errorText.isNotEmpty()) Text(errorText, style = Fm.text.bodySmall.copy(color = c.danger))
    }
}

@Composable
internal fun RunnerMergeDialog(
    row: UnifiedAgentRow,
    cleanupOnly: Boolean,
    onDone: (String) -> Unit,
    onDismiss: () -> Unit,
    updateOnly: Boolean = false,
    onActions: ((String, String) -> Unit)? = null,
) {
    val c = Fm.colors
    val model = remember(row.id, cleanupOnly, updateOnly) {
        RunnerMergeModel(
            AgentsFeature.runnerOps,
            row.runnerRef(),
            row.branchName,
            cleanupOnly = cleanupOnly,
            updateOnly = updateOnly,
            base = row.baseBranch,
            tasks = if (cleanupOnly || updateOnly) null else RunnerTaskDevice,
        )
    }
    val tick = rememberOpsTick(model)
    val scope = rememberCoroutineScope()
    LaunchedEffect(model) { opsIo { model.run() } }
    tick.let { }
    val result = model.result
    if (result != null) {
        val close = {
            onDone(result.summary)
            onDismiss()
        }
        if (cleanupOnly) {
            FmConfirmDialog(title = "Cleaned up", body = result.summary, confirmLabel = "Done", hideCancel = true, onConfirm = close, onDismissRequest = close)
            return
        }
        if (updateOnly) {
            FmConfirmDialog(
                title = if (result.status == "already") "Up to date" else "Updated",
                body = result.summary,
                confirmLabel = "Done",
                hideCancel = true,
                onConfirm = close,
                onDismissRequest = close,
            )
            return
        }
        val base = result.base.ifEmpty { result.stats?.base.orEmpty() }
        FmConfirmDialog(
            title = "Merged!",
            body = if (base.isEmpty()) "Merged ${result.branch} and cleaned up." else "${result.branch.ifEmpty { row.branchName }} is part of $base now.",
            confirmLabel = "Done",
            hideCancel = true,
            onConfirm = close,
            onDismissRequest = close,
            fields = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    result.stats?.let { MergeStatsBlock(it) }
                    if (result.cleanupPending.isNotEmpty()) {
                        Text(
                            "The branch is still on ${result.cleanupPending.joinToString(", ")}; Clean up removes it there once they catch up.",
                            style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        )
                    }
                    if (model.taskNote.isNotEmpty()) {
                        Text(
                            model.taskNote,
                            style = Fm.text.bodySmall.copy(color = if (model.taskNote == RunnerTaskLink.COMPLETED) c.textSecondary else c.warning),
                        )
                    }
                    if (onActions != null && row.repoName.isNotEmpty()) {
                        Spacer(Modifier.height(FmSpace.x2))
                        FmToneButton("Actions", onPressed = {
                            onDismiss()
                            onActions(result.summary, result.mergeOid)
                        }, icon = "play_circle_outline", tone = FmTone.Accent)
                    }
                    Spacer(Modifier.height(FmSpace.x3))
                    TodayActivityBlock()
                }
            },
        )
        return
    }
    if (model.conflicted) {
        MergeConflictFixDialog(model.errorText, onFix = { AgentsFeature.control.fixConflicts(row, model.conflicts) }, onDone = onDone, onDismiss = onDismiss)
        return
    }
    val base = row.baseBranch.ifEmpty { "its base branch" }
    val failed = model.errorText.isNotEmpty() && !model.running
    FmConfirmDialog(
        title = when {
            cleanupOnly -> "Clean up"
            updateOnly -> "Update from $base"
            else -> "Merge & clean up"
        },
        body = when {
            failed -> null
            cleanupOnly -> "Removing ${row.branchName} on ForkMesh; its last commit is kept so it can be recovered…"
            updateOnly -> "Merging $base into ${row.branchName} on ForkMesh…"
            else -> "Merging ${row.branchName} into $base on ForkMesh…"
        },
        confirmLabel = "Try again",
        busy = !failed,
        onConfirm = { scope.launch { opsIo { model.run() } } },
        onDismissRequest = onDismiss,
        fields = {
            if (model.errorText.isNotEmpty()) Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
        },
    )
}

@Composable
internal fun RunnerDeleteDialog(row: UnifiedAgentRow, onDone: (String) -> Unit, onDismiss: () -> Unit, active: Boolean = row.isActive) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    var running by remember(row.id) { mutableStateOf(false) }
    var errorText by remember(row.id) { mutableStateOf("") }
    val branch = if (row.branchName.isEmpty()) "" else " Its branch ${row.branchName} is deleted on ForkMesh too."
    FmConfirmDialog(
        title = "Delete agent",
        body = (if (active) "Stop this run, then delete it and its transcript?" else "Delete this run and its transcript?") + branch,
        confirmLabel = "Delete",
        destructive = true,
        busy = running,
        onConfirm = {
            running = true
            scope.launch {
                try {
                    val report = opsIo {
                        AgentsFeature.runnerOps.delete(
                            row.runnerRef(), row.branchName, active = active, pull = row.prNumber, merged = row.merged,
                        )
                    }
                    onDone(report.summary("on ForkMesh"))
                    onDismiss()
                } catch (error: Exception) {
                    errorText = error.message ?: error.toString()
                } finally {
                    running = false
                }
            }
        },
        onDismissRequest = onDismiss,
        fields = {
            if (errorText.isNotEmpty()) Text(errorText, style = Fm.text.bodySmall.copy(color = c.danger))
        },
    )
}
