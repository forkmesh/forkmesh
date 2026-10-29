package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.agents.ops.AgentDeleteModel
import com.forkmesh.nativeapp.agents.ops.AgentDiff
import com.forkmesh.nativeapp.agents.ops.AgentDiffModel
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.DiffMenuAction
import com.forkmesh.nativeapp.feature.code.DiffReviewStream
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

internal class AgentDiffScreen(
    private val sessionId: Int,
    private val agentTitle: String,
    private val canMerge: Boolean = false,
    private val base: String = "",
    private val onMerged: (String) -> Unit = {},
    private val desktopKey: String = "",
) : Screen() {
    override val title: String get() = "Changes"
    private var showMerge by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val model = remember(sessionId) { AgentDiffModel(AgentOpsFeature.client(desktopKey), sessionId, Services.get<PrefStore>()) }
        val tick = rememberOpsTick(model)
        val scope = rememberCoroutineScope()
        val copyStatus = rememberFmStatus()
        val copy = rememberCopyText(copyStatus)
        LaunchedEffect(model) { opsIo { model.load() } }
        if (showMerge) {
            MergeCleanupDialog(sessionId, base, desktopKey = desktopKey, onDone = { message ->
                navigator.pop()
                onMerged(message)
            }, onDismiss = { showMerge = false })
        }
        DetailScaffold(title, navigator, actions = {
            if (canMerge) FmToneButton("Merge & clean up", onPressed = { showMerge = true }, icon = "call_merge", tone = FmTone.Success)
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
                        if (diff.committed.isNotEmpty()) add(DiffMenuAction("Copy committed patch", "content_copy") { copy("Patch", diff.committed) })
                        if (diff.uncommitted.isNotEmpty()) add(DiffMenuAction("Copy uncommitted patch", "content_copy") { copy("Patch", diff.uncommitted) })
                    },
                    emptyMessage = "No changes against the base branch.",
                    onRefresh = { scope.launch { opsIo { model.load() } } },
                    header = { AgentDiffSummary(agentTitle, diff, model.errorText) },
                )
            } else {
                Column(Modifier.padding(FmSpace.x4)) {
                    Text(agentTitle, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                    if (model.loading) {
                        Text("Reading the changes from the desktop…", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                    }
                    if (model.errorText.isNotEmpty()) Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
                }
            }
        }
    }
}

@Composable
private fun AgentDiffSummary(agentTitle: String, diff: AgentDiff, errorText: String) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
    ) {
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
        if (diff.truncated) {
            Text("Truncated: the full diff is on the desktop.", style = Fm.text.bodySmall.copy(color = c.warning))
        }
        if (errorText.isNotEmpty()) Text(errorText, style = Fm.text.bodySmall.copy(color = c.danger))
        if (diff.untracked.isNotEmpty()) {
            Spacer(Modifier.height(FmSpace.x1))
            Text("New files not yet added", style = Fm.text.labelMedium.copy(color = c.textSecondary))
            for (path in diff.untracked) {
                Text(path, style = Fm.text.bodySmall.copy(color = c.textSecondary, fontFamily = FmType.mono))
            }
        }
    }
}

@Composable
internal fun AgentDeleteDialog(sessionId: Int, onDone: (String) -> Unit, onDismiss: () -> Unit, desktopKey: String = "") {
    val c = Fm.colors
    val model = remember(sessionId, desktopKey) { AgentDeleteModel(AgentOpsFeature.client(desktopKey), sessionId) }
    val tick = rememberOpsTick(model)
    val scope = rememberCoroutineScope()
    LaunchedEffect(model) { opsIo { model.prepare() } }
    tick.let { }
    FmConfirmDialog(
        title = "Delete agent",
        body = model.confirmation?.summary ?: if (model.preparing) "Asking the desktop…" else "",
        confirmLabel = "Delete",
        destructive = true,
        busy = model.running || model.preparing,
        onConfirm = {
            scope.launch {
                val ok = opsIo { model.run() }
                if (ok) {
                    onDone(model.report?.summary("on the desktop") ?: "Deleted on the desktop.")
                    onDismiss()
                }
            }
        },
        onDismissRequest = onDismiss,
        fields = {
            if (model.errorText.isNotEmpty()) Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
        },
    )
}
