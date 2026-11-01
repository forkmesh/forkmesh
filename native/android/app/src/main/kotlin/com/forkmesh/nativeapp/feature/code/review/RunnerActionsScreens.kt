@file:OptIn(ExperimentalLayoutApi::class)

package com.forkmesh.nativeapp.feature.code.review

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ci.ActionSecrets
import com.forkmesh.nativeapp.ci.ActionSecretsModel
import com.forkmesh.nativeapp.ci.CiLog
import com.forkmesh.nativeapp.ci.PullChecksAction
import com.forkmesh.nativeapp.ci.PullChecksModel
import com.forkmesh.nativeapp.ci.RepoActionsModel
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.feature.account.projects.UserPresence
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.EmptyCard
import com.forkmesh.nativeapp.feature.code.ErrorCard
import com.forkmesh.nativeapp.feature.code.InfoCard
import com.forkmesh.nativeapp.feature.code.Kv
import com.forkmesh.nativeapp.feature.code.LoadingCard
import com.forkmesh.nativeapp.feature.code.MobileCard
import com.forkmesh.nativeapp.feature.code.navigationBarBottom
import com.forkmesh.nativeapp.feature.code.rememberCopyText
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.inbox.NotificationCommand
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.confirmAction
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

internal object PendingChecksApproval {
    private const val MAX_AGE_MS = 5 * 60_000L

    private class Entry(val command: NotificationCommand.ApproveChecks, val atMs: Long)

    private val state = MutableStateFlow<Entry?>(null)
    private val tickState = MutableStateFlow(0)
    val changes: StateFlow<Int> = tickState.asStateFlow()

    fun request(command: NotificationCommand.ApproveChecks) {
        state.value = Entry(command, System.currentTimeMillis())
        tickState.value = tickState.value + 1
    }

    fun take(repo: RepoRef, number: Int): NotificationCommand.ApproveChecks? {
        val entry = state.value ?: return null
        val command = entry.command
        if (System.currentTimeMillis() - entry.atMs > MAX_AGE_MS) {
            state.compareAndSet(entry, null)
            return null
        }
        if (!command.repo.owner.equals(repo.owner, ignoreCase = true) ||
            !command.repo.name.equals(repo.name, ignoreCase = true) ||
            command.number != number
        ) {
            return null
        }
        return if (state.compareAndSet(entry, null)) command else null
    }
}

@Composable
internal fun RunnerChecksCard(repo: Repository, number: Int, navigator: Navigator) {
    val ref = CodeServices.ref(repo)
    val model = remember(repo.fullName, number) { PullChecksModel(CodeServices.ci(), ref, number) }
    val tick = rememberTick()
    val scope = rememberCoroutineScope()
    val messenger = LocalFmMessenger.current
    val context = LocalContext.current
    val status = rememberFmStatus()
    val pendingTick by PendingChecksApproval.changes.collectAsState()

    suspend fun perform(action: PullChecksAction, head: String? = null) {
        if (model.busy != null) return
        val shown = (head ?: model.checks?.head.orEmpty()).take(7)
        val author = model.checks?.approval?.author.orEmpty()
        if (action.needsPresence) {
            val reason = if (action == PullChecksAction.Approve) {
                "Confirm it's you to run the checks for pull request #$number at $shown."
            } else {
                "Confirm it's you to always run checks for @$author."
            }
            if (!UserPresence.confirm(context, messenger, reason)) return
        }
        status.clear()
        tick.bump()
        io { model.perform(action, head) }
        tick.bump()
        model.notice?.let { status.report(it.text, !it.good) }
    }

    LaunchedEffect(repo.fullName, number) {
        while (true) {
            io { model.load() }
            tick.bump()
            val active = model.hasActiveRuns || model.checks?.approval?.required == true
            delay(if (active) 10_000L else 60_000L)
        }
    }
    LaunchedEffect(repo.fullName, number, pendingTick) {
        val request = PendingChecksApproval.take(ref, number) ?: return@LaunchedEffect
        perform(PullChecksAction.Approve, request.head)
    }
    tick.value
    if (!model.visible && status.message == null) return
    val checks = model.checks
    Column(Modifier.fillMaxWidth()) {
        Spacer(Modifier.height(12.dp))
        InfoCard("Checks", Modifier.testTag("pull-runner-checks")) {
            Stack {
                model.error?.let { problem ->
                    Note(problem, Fm.colors.danger)
                    FmToneButton("Retry", onPressed = { scope.launch { io { model.load() }; tick.bump() } })
                }
                if (checks != null) {
                    Note(checks.summary)
                    if (checks.approval.required && checks.approval.reasonLabel.isNotEmpty()) {
                        Note(checks.approval.reasonLabel, Fm.colors.warning)
                    }
                    if (checks.shortHead.isNotEmpty()) Kv("Commit", checks.shortHead)
                    if (checks.approval.approvedBy.isNotEmpty()) Kv("Approved by", "@${checks.approval.approvedBy}")
                    if (checks.approval.trustLabel.isNotEmpty()) Note(checks.approval.trustLabel)
                    if (checks.previousHeads > 0) {
                        Note(if (checks.previousHeads == 1) "Checks for 1 earlier commit are not shown." else "Checks for ${checks.previousHeads} earlier commits are not shown.")
                    }
                    for (run in checks.runs) {
                        Row(
                            Modifier.fillMaxWidth().clickable { navigator.push(standaloneActionRunScreen(repo, run.id)) }.padding(vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(run.displayName, Modifier.weight(1f), color = Fm.colors.textPrimary)
                            FmStatusBadge(run.status.label, runColor(run.status))
                        }
                    }
                    val actions = model.actions
                    if (actions.isNotEmpty() || model.busy != null) {
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            for (action in actions) {
                                FmToneButton(
                                    action.label(checks.approval.author),
                                    onPressed = { scope.launch { perform(action) } },
                                    icon = when (action) {
                                        PullChecksAction.Approve -> "play_circle_outline"
                                        PullChecksAction.Trust -> "verified_user_outlined"
                                        PullChecksAction.Untrust -> "block"
                                    },
                                    tone = when (action) {
                                        PullChecksAction.Approve -> FmTone.Success
                                        PullChecksAction.Trust -> FmTone.Accent
                                        PullChecksAction.Untrust -> FmTone.Neutral
                                    },
                                    filled = action == PullChecksAction.Approve,
                                    modifier = Modifier.testTag("pull-checks-${action.raw}"),
                                )
                            }
                            if (model.busy != null) FmToneButton("Sending…", onPressed = null, busy = true)
                        }
                    }
                }
                FmInlineStatus(status)
            }
        }
    }
}

internal class RunLogScreen(
    private val repo: Repository,
    private val model: RepoActionsModel,
    private val runId: Int,
) : Screen() {
    override val title: String get() = "Log · run #$runId"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val status = rememberFmStatus()
        val copy = rememberCopyText(status)
        LaunchedEffect(Unit) {
            io { model.loadLog(runId) }
            tick.bump()
            while (true) {
                delay(RepoActionsModel.FOLLOW_UP_DELAY_MS)
                if (model.run(runId)?.status?.isActive == true) {
                    io {
                        model.load()
                        model.loadLog(runId)
                    }
                    tick.bump()
                }
            }
        }
        tick.value
        val log = model.fullLog(runId) ?: model.run(runId)?.logTail.orEmpty()
        val lines = remember(log) { CiLog.fullLines(log) }
        val chunks = remember(lines) { CiLog.chunks(lines) }
        val active = model.run(runId)?.status?.isActive == true
        DetailScaffold("Log · run #$runId", navigator, actions = {
            FmIconButton("content_copy", "Copy log", onPressed = { copy("Log", lines.joinToString("\n")) })
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { io { model.loadLog(runId) }; tick.bump() } })
        }) {
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x3, bottom = FmSpace.x6 + navigationBarBottom()),
            ) {
                item {
                    Column(Modifier.padding(bottom = FmSpace.x2)) {
                        Note(listOf(if (lines.size == 1) "1 line" else "${lines.size} lines", if (active) "live" else "").filter { it.isNotEmpty() }.joinToString(" · "))
                        model.logProblems[runId]?.let { Note(it, Fm.colors.danger) }
                        FmInlineStatus(status)
                    }
                }
                if (lines.isEmpty()) item { Note("No log yet.") }
                items(chunks.size) { index ->
                    SelectionContainer { Mono(chunks[index]) }
                }
            }
        }
    }
}

internal class ActionSecretsScreen(private val repo: Repository) : Screen() {
    private val model = ActionSecretsModel(CodeServices.ci(), CodeServices.ref(repo))
    override val title: String get() = "Secrets"

    @Composable
    override fun Content(navigator: Navigator) {
        val tick = rememberTick()
        val scope = rememberCoroutineScope()
        val messenger = LocalFmMessenger.current
        val status = rememberFmStatus()
        var name by remember { mutableStateOf("") }
        var value by remember { mutableStateOf("") }
        LaunchedEffect(Unit) {
            io { model.load() }
            tick.bump()
        }
        tick.value
        val cleanName = ActionSecrets.cleanName(name)
        val replacing = cleanName.isNotEmpty() && model.exists(cleanName)
        DetailScaffold("Secrets · ${repo.name}", navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { io { model.load() }; tick.bump() } })
        }) {
            ScreenList {
                item {
                    InfoCard(if (replacing) "Replace $cleanName" else "Add a secret") {
                        Stack {
                            Note("Workflows that this instance's runner runs read secrets as environment variables. A saved value cannot be read back, here or anywhere else.")
                            FmTextField(
                                value = name,
                                onValueChange = { name = it.uppercase().filter { ch -> ch.isLetterOrDigit() || ch == '_' }.take(64) },
                                hint = "NAME",
                                errorText = if (name.isEmpty()) null else ActionSecrets.nameProblem(cleanName),
                                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Characters, autoCorrectEnabled = false),
                                modifier = Modifier.testTag("secret-name"),
                            )
                            FmTextField(
                                value = value,
                                onValueChange = { value = it },
                                hint = "Value",
                                helperText = "1 byte to 48 KiB.",
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, autoCorrectEnabled = false),
                                visualTransformation = PasswordVisualTransformation(),
                                modifier = Modifier.testTag("secret-value"),
                            )
                            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                FmToneButton(
                                    if (model.busy) "Saving…" else if (replacing) "Replace" else "Save",
                                    onPressed = {
                                        scope.launch {
                                            if (replacing && !messenger.confirmAction("Replace $cleanName?", "Replace", "Workflows get the new value from their next run.", destructive = false)) return@launch
                                            val ok = io { model.save(name, value) }
                                            if (ok) {
                                                name = ""
                                                value = ""
                                            }
                                            tick.bump()
                                            model.notice?.let { status.report(it.text, !it.good) }
                                        }
                                    },
                                    icon = "key_outlined",
                                    tone = FmTone.Accent,
                                    filled = true,
                                    busy = model.busy,
                                )
                                if (name.isNotEmpty() || value.isNotEmpty()) {
                                    FmToneButton("Clear", onPressed = { name = ""; value = ""; status.clear() })
                                }
                            }
                            FmInlineStatus(status)
                        }
                    }
                }
                val problem = model.error
                if (problem != null) item { ErrorCard(problem, { scope.launch { io { model.load() }; tick.bump() } }) }
                if (!model.loaded) item { LoadingCard("Loading secrets…") }
                if (model.loaded && problem == null && model.secrets.isEmpty()) item { EmptyCard("This repository has no secrets yet.") }
                items(model.secrets, key = { it.name }) { secret ->
                    MobileCard(
                        icon = "key_outlined",
                        iconColor = Fm.colors.textTertiary,
                        title = secret.name,
                        subtitle = listOf(
                            if (secret.updatedAtMs > 0) "Updated ${relativeTime(secret.updatedAtMs)}" else "",
                            if (secret.updatedBy.isNotEmpty()) "by @${secret.updatedBy}" else "",
                        ).filter { it.isNotEmpty() }.joinToString(" "),
                        trailing = {
                            FmIconButton("delete_outline", "Delete ${secret.name}", onPressed = {
                                scope.launch {
                                    if (!messenger.confirmAction("Delete ${secret.name}?", "Delete", "Workflows that read it get an empty value from their next run.")) return@launch
                                    io { model.delete(secret.name) }
                                    tick.bump()
                                    model.notice?.let { status.report(it.text, !it.good) }
                                }
                            }, color = Fm.colors.danger)
                        },
                        onTap = {
                            name = secret.name
                            value = ""
                            status.clear()
                        },
                    )
                }
            }
        }
    }
}
