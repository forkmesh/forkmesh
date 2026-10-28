package com.forkmesh.nativeapp.feature.code.collab

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.collab.PullCompose
import com.forkmesh.nativeapp.collab.dartErrorText
import com.forkmesh.nativeapp.model.RemoteFork
import com.forkmesh.nativeapp.model.RepoComparison
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmInboxPendingNote
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal suspend fun CollabEnv.showNewPullDialog(repo: Repository, initialHead: String = "") {
    if (!requireAccount("open a pull request")) return
    val env = this
    val notice = messenger.showDialog<String> { done ->
        NewPullForm(env, repo, initialHead, done)
    } ?: return
    status.show(notice)
}

@Composable
private fun NewPullForm(env: CollabEnv, repo: Repository, initialHead: String, done: (String?) -> Unit) {
    val colors = Fm.colors
    val haptics = FmHaptics.current
    val scope = rememberCoroutineScope()
    var title by remember { mutableStateOf("") }
    var body by remember { mutableStateOf("") }
    var base by remember { mutableStateOf(repo.defaultBranch) }
    var head by remember { mutableStateOf(initialHead) }
    var patch by remember { mutableStateOf("") }
    var pasting by remember { mutableStateOf(false) }
    var target by remember { mutableStateOf<RemoteFork?>(null) }
    var branches by remember { mutableStateOf<List<String>>(emptyList()) }
    var forks by remember { mutableStateOf<List<RemoteFork>>(emptyList()) }
    var loaded by remember { mutableStateOf(false) }
    var comparison by remember { mutableStateOf<RepoComparison?>(null) }
    var comparing by remember { mutableStateOf(false) }
    var compareError by remember { mutableStateOf("") }
    var sending by remember { mutableStateOf(false) }
    var errorText by remember { mutableStateOf("") }

    suspend fun compare() {
        val wantBase = base.trim()
        val wantHead = head.trim()
        compareError = ""
        PullCompose.compareProblem(wantBase, wantHead)?.let {
            comparison = null
            compareError = it
            return
        }
        comparing = true
        try {
            val result = withContext(Dispatchers.IO) { env.api.compare(repo.owner, repo.name, wantBase, wantHead) }
            if (wantBase != base.trim() || wantHead != head.trim()) return
            val outcome = PullCompose.compared(result, wantHead, title)
            comparison = outcome.comparison
            compareError = outcome.error
            title = outcome.title
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
            comparison = null
            compareError = PullCompose.COMPARE_FAILED
        } finally {
            comparing = false
        }
    }

    LaunchedEffect(repo.owner, repo.name) {
        val loadedBranches = withContext(Dispatchers.IO) { runCatching { env.api.branches(repo.owner, repo.name) }.getOrNull() }.orEmpty()
        forks = withContext(Dispatchers.IO) { runCatching { env.api.remoteForks(repo.owner, repo.name) }.getOrNull() }.orEmpty()
        branches = loadedBranches.map { it.name }
        if (base.isEmpty()) (loadedBranches.firstOrNull { it.isDefault }?.name ?: branches.firstOrNull())?.let { base = it }
        if (branches.isEmpty()) pasting = true
        loaded = true
        if (head.isNotEmpty() && comparison == null && !pasting) compare()
    }

    val effectivePatch = if (pasting) patch else comparison?.patch.orEmpty()
    val effectiveCommits = if (pasting) "" else comparison?.commits.orEmpty()

    fun submit() {
        PullCompose.submitProblem(title, effectivePatch, pasting, target, env.api.instanceHost)?.let {
            errorText = it
            haptics.failure()
            return
        }
        sending = true
        errorText = ""
        val fork = target
        val cleanTitle = title.trim()
        val cleanBase = base.trim()
        val cleanHead = head.trim()
        val description = PullCompose.description(body)
        scope.launch {
            val outcome = try {
                withContext(Dispatchers.IO) {
                    if (fork != null) {
                        val signedHead = RemoteFork.signedHead(env.api.instanceHost, repo.displayOwner, repo.name, cleanHead)
                        val number = env.inbox.openRemotePull(fork, cleanTitle, cleanBase, signedHead, effectivePatch, effectiveCommits, description)
                        PullCompose.sentRemote(fork, number)
                    } else {
                        env.inbox.submitNewPull(repo.owner, repo.name, cleanTitle, cleanBase, cleanHead, effectivePatch, description = description, commits = effectiveCommits)
                        PullCompose.sentLocal()
                    }
                } to null
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                null to (if (fork != null) PullCompose.remoteError(error, fork) else dartErrorText(error))
            }
            sending = false
            val message = outcome.first
            if (message != null) {
                haptics.success()
                done(message)
            } else {
                errorText = outcome.second.orEmpty()
                haptics.failure()
            }
        }
    }

    CollabDialog(
        title = PullCompose.TITLE,
        width = 520.dp,
        onDismiss = { done(null) },
        actions = {
            FmDialogActions(
                onCancel = { done(null) },
                onConfirm = if (PullCompose.canSubmit(title, effectivePatch, pasting, target) && !sending) ({ submit() }) else null,
                confirmLabel = PullCompose.confirmLabel(target),
                busy = sending,
            )
        },
    ) {
        SessionIdentityBar("Posting")
        Spacer(Modifier.height(8.dp))
        if (forks.isNotEmpty()) {
            FmField("Send to") {
                PickerMenu(
                    text = PullCompose.targetLabel(target),
                    icon = if (target == null) "folder_outlined" else "public",
                    label = "Send to",
                    choices = listOf<RemoteFork?>(null) + forks,
                    selected = target,
                    name = { PullCompose.targetLabel(it) },
                    tag = "new-pull-target",
                ) {
                    target = it
                    if (it != null) pasting = false
                }
            }
            if (target != null) {
                Text(PullCompose.REMOTE_NOTE, style = Fm.text.bodySmall.copy(color = colors.textTertiary))
            }
            Spacer(Modifier.height(10.dp))
        }
        if (pasting) {
            Row(verticalAlignment = Alignment.Top) {
                FmField("Base branch", Modifier.weight(1f)) { FmTextField(base, { base = it }) }
                Spacer(Modifier.width(10.dp))
                FmField("Head branch", Modifier.weight(1f)) { FmTextField(head, { head = it }) }
            }
            Spacer(Modifier.height(10.dp))
            FmField("Unified diff (patch)") {
                FmTextField(
                    patch,
                    { patch = it },
                    minLines = 5,
                    maxLines = 14,
                    textStyle = Fm.text.bodySmall.copy(fontFamily = FmType.mono, color = colors.textPrimary),
                )
            }
            if (branches.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                FmToneButton(PullCompose.USE_BRANCHES, onPressed = { pasting = false }, icon = "difference_outlined", tone = FmTone.Accent)
            }
        } else {
            Row(verticalAlignment = Alignment.Top) {
                FmField("Base", Modifier.weight(1f)) {
                    PickerMenu(
                        text = base.ifEmpty { "Choose…" },
                        icon = "account_tree_outlined",
                        label = "Base branch",
                        choices = branches,
                        selected = base,
                        name = { it },
                        tag = "new-pull-base",
                    ) {
                        base = it
                        comparison = null
                        if (head.isNotEmpty()) scope.launch { compare() }
                    }
                }
                Spacer(Modifier.width(10.dp))
                FmField("Compare", Modifier.weight(1f)) {
                    PickerMenu(
                        text = head.ifEmpty { "Choose…" },
                        icon = "account_tree_outlined",
                        label = "Head branch",
                        choices = PullCompose.headChoices(branches, base),
                        selected = head,
                        name = { it },
                        tag = "new-pull-head",
                    ) {
                        head = it
                        comparison = null
                        scope.launch { compare() }
                    }
                }
            }
            Spacer(Modifier.height(10.dp))
            val compared = comparison
            when {
                comparing || !loaded -> Text(PullCompose.COMPARING, style = Fm.text.bodySmall.copy(color = colors.textTertiary))
                compared != null -> Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        FmIcon("check_circle_outline", size = 16.dp, tint = colors.success)
                        Spacer(Modifier.width(6.dp))
                        Text(PullCompose.ableToMerge(compared), style = Fm.text.bodySmall.copy(color = colors.success))
                    }
                    PatchFilesCard(compared.patch)
                }
                compareError.isNotEmpty() -> Text(compareError, style = Fm.text.bodySmall.copy(color = colors.warning))
                head.isEmpty() -> Text(PullCompose.PICK_BRANCHES, style = Fm.text.bodySmall.copy(color = colors.textTertiary))
                else -> FmToneButton(PullCompose.COMPARE_LABEL, onPressed = { scope.launch { compare() } }, icon = "difference_outlined", tone = FmTone.Accent)
            }
            if (target == null) {
                Spacer(Modifier.height(8.dp))
                FmToneButton(PullCompose.PASTE_PATCH, onPressed = { pasting = true }, icon = "content_paste", tone = FmTone.Accent)
            }
        }
        Spacer(Modifier.height(10.dp))
        FmField("Title") { FmTextField(title, { title = it }, Modifier.testTag("new-pull-title")) }
        Spacer(Modifier.height(10.dp))
        FmField("Description") {
            FmTextField(body, { body = it }, Modifier.testTag("new-pull-body"), hint = PullCompose.DESCRIPTION_HINT, minLines = 4, maxLines = 10)
        }
        if (errorText.isNotEmpty()) {
            Spacer(Modifier.height(8.dp))
            Text(errorText, style = Fm.text.bodySmall.copy(color = colors.danger))
        }
        if (target == null) {
            Spacer(Modifier.height(12.dp))
            FmInboxPendingNote()
        }
    }
}

@Composable
private fun <T> PickerMenu(
    text: String,
    icon: String,
    label: String,
    choices: List<T>,
    selected: T,
    name: (T) -> String,
    tag: String,
    onPick: (T) -> Unit,
) {
    val colors = Fm.colors
    var open by remember { mutableStateOf(false) }
    val enabled = choices.isNotEmpty()
    Box {
        Row(
            Modifier
                .fillMaxWidth()
                .height(40.dp)
                .clip(RoundedCornerShape(FmRadius.md))
                .background(colors.bgOverlay)
                .clickable(enabled = enabled) { open = true }
                .semantics {
                    role = Role.Button
                    contentDescription = "$label: $text"
                }
                .testTag(tag)
                .padding(horizontal = FmSpace.x3),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            FmIcon(icon, size = 16.dp, tint = colors.textSecondary)
            Text(
                text,
                Modifier.weight(1f),
                style = Fm.text.bodyMedium.copy(color = if (enabled) colors.textPrimary else colors.textTertiary),
                maxLines = 1,
                overflow = TextOverflow.MiddleEllipsis,
            )
            FmIcon("keyboard_arrow_down_rounded", size = 17.dp, tint = colors.textTertiary)
        }
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (choice in choices) {
                FmDropdownMenuItem(
                    text = name(choice),
                    onClick = {
                        open = false
                        onPick(choice)
                    },
                    icon = if (choice == selected) "check_circle" else null,
                )
            }
        }
    }
}
