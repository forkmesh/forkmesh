package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.OwnerProof
import com.forkmesh.nativeapp.code.OwnerProofs
import com.forkmesh.nativeapp.code.RepoActivity
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.model.ActionLog
import com.forkmesh.nativeapp.model.ActionRun
import com.forkmesh.nativeapp.model.RepoActions
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private const val POLL_INTERVAL_MS = 10_000L

private fun actionsReadProof(repo: Repository): OwnerProof = OwnerProofs.actionsRead(
    session = Services.get<AuthService>()?.session?.value,
    identity = Services.get<DeviceIdentity>(),
    owner = repo.owner,
    name = repo.name,
)

@Stable
internal class ActionsTabState(val repo: Repository) {
    val actions = AsyncValue<RepoActions>()

    var polling by mutableStateOf(false)

    fun load() {
        actions.reload {
            val proof = actionsReadProof(repo)
            repoApi.repoActions(repo.owner, repo.name, ownerAccount = proof.ownerAccount, ts = proof.ts, sig = proof.sig)
        }
    }
}

@Composable
internal fun ActionsTab(state: ActionsTabState, navigator: Navigator) {
    val value = state.actions
    value.Bind()
    LaunchedEffect(value.data) {
        value.data?.let { state.polling = RepoActivity.hasActiveRuns(it) }
    }
    if (state.polling) {
        LaunchedEffect(Unit) {
            while (true) {
                delay(POLL_INTERVAL_MS)
                state.load()
            }
        }
    }
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    val actions = value.data ?: RepoActions()
    val error = value.error
    val polling = state.polling || RepoActivity.hasActiveRuns(actions)
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(top = 10.dp, bottom = 10.dp + navigationBarBottom()),
    ) {
        item {
            FmSectionHeader(
                title = "Workflows",
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                trailing = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (value.data != null) {
                            Text(
                                RepoActivity.actionsCountLabel(actions),
                                style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textSecondary),
                            )
                        }
                        if (polling) {
                            Spacer(Modifier.width(FmSpace.x2))
                            Text("Live refresh on")
                        }
                        Spacer(Modifier.width(FmSpace.x1))
                        FmIconButton("refresh", "Refresh actions", onPressed = { state.load() }, iconSize = 18.dp)
                    }
                },
            )
        }
        item {
            Text(
                "Desktop node runs workflows and owns rerun, cancel, approve, secrets, and artifacts. Mobile watches status " +
                    "and logs until signed desktop pairing is designed.",
                modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x3),
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
        }
        when {
            (value.waiting || !value.started) && actions.workflows.isEmpty() && actions.runs.isEmpty() -> item {
                LoadingCard("Loading actions…", Modifier.padding(FmSpace.x4))
            }
            error != null -> item { EmptyCard("$error", Modifier.padding(FmSpace.x4)) }
            actions.workflows.isEmpty() && actions.runs.isEmpty() -> item {
                EmptyCard(
                    "No action snapshots have been pushed by the desktop node for this repo yet.",
                    Modifier.padding(FmSpace.x4),
                )
            }
            else -> {
                dividedRows(actions.workflows) { workflow ->
                    MobileCard(
                        icon = "playlist_play_outlined",
                        iconColor = c.accent,
                        title = workflow.displayName,
                        subtitle = workflow.path,
                        chips = RepoActivity.workflowChips(workflow),
                    )
                }
                if (actions.runs.isNotEmpty()) {
                    item {
                        FmSectionHeader(
                            title = "Runs",
                            modifier = Modifier.padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x2),
                        )
                    }
                }
                dividedRows(actions.runs) { run ->
                    MobileCard(
                        icon = "terminal_outlined",
                        iconColor = c.accent,
                        title = run.displayName,
                        subtitle = RepoActivity.runSubtitle(run),
                        trailing = { ActionStatusBadge(run) },
                        onTap = {
                            scope.launch {
                                val proof = try {
                                    withContext(Dispatchers.IO) { actionsReadProof(state.repo) }
                                } catch (cancelled: CancellationException) {
                                    throw cancelled
                                } catch (_: Exception) {
                                    return@launch
                                }
                                navigator.push(ActionRunDetailScreen(state.repo, run, proof))
                            }
                        },
                        chips = RepoActivity.runChips(run),
                    )
                }
            }
        }
    }
}

@Composable
private fun ActionStatusBadge(run: ActionRun) {
    FmStatusBadge(label = run.statusLabel, color = toneColor(RepoActivity.actionStatusTone(run.statusLabel)))
}

internal class ActionRunDetailScreen(val repo: Repository, val run: ActionRun, private val proof: OwnerProof) : Screen() {
    private val log = AsyncValue<ActionLog>().apply {
        reload {
            repoApi.actionLog(repo.owner, repo.name, run.id, ownerAccount = proof.ownerAccount, ts = proof.ts, sig = proof.sig)
        }
    }

    override val title: String get() = "Action run #${run.id}"

    @Composable
    override fun Content(navigator: Navigator) {
        log.Bind()
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val commands = rememberDesktopCommands()
        val desktopRoute = rememberDesktopRoute(repo)
        DetailScaffold(title = title, navigator = navigator) {
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(start = 20.dp, top = 20.dp, end = 20.dp, bottom = 20.dp + navigationBarBottom()),
            ) {
                item {
                    InfoCard(run.displayName) {
                        ChipWrap {
                            RepoChip("info_outline", run.statusLabel)
                            if (run.refLabel.isNotEmpty()) RepoChip("account_tree_outlined", run.refLabel)
                            if (run.shortCommit.isNotEmpty()) RepoChip("commit_outlined", run.shortCommit)
                            if (run.workflowPath.isNotEmpty()) RepoChip("description_outlined", run.workflowPath)
                        }
                        Spacer(Modifier.height(12.dp))
                        Text(
                            "Desktop-controlled workflow. Mobile can inspect run output and queue signed requests; rerun, " +
                                "cancel, approval, and secret editing still require explicit approval on the desktop node.",
                            color = c.textSecondary,
                        )
                        Spacer(Modifier.height(12.dp))
                        if (desktopRoute) FmFilledButton(
                            com.forkmesh.nativeapp.control.DesktopCommandContract.RERUN_LABEL,
                            onClick = {
                                scope.launch {
                                    commands.request(
                                        repo = repo,
                                        command = "action.rerun",
                                        target = RepoActivity.rerunTarget(run),
                                        payload = RepoActivity.rerunPayload(run),
                                        successMessage = RepoActivity.RERUN_QUEUED,
                                    )
                                }
                            },
                            icon = "replay_outlined",
                        )
                        FmInlineStatus(commands.status, Modifier.padding(top = 8.dp))
                    }
                }
                item { Spacer(Modifier.height(12.dp)) }
                item {
                    val error = log.error
                    when {
                        log.waiting -> LoadingCard("Loading action log…")
                        error != null -> EmptyCard("$error")
                        else -> {
                            val detail = log.data ?: ActionLog(id = run.id, status = run.status)
                            InfoCard("Run log") {
                                ChipWrap {
                                    RepoChip("info_outline", detail.statusLabel)
                                    RepoChip("folder_outlined", repo.fullName)
                                }
                                Spacer(Modifier.height(12.dp))
                                Box {
                                    SelectionContainer {
                                        Text(
                                            RepoActivity.logText(detail.log),
                                            style = Fm.text.bodySmall.copy(
                                                fontFamily = FmType.mono,
                                                lineHeight = FmType.meta * 1.45f,
                                                color = c.textPrimary,
                                            ),
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
}
