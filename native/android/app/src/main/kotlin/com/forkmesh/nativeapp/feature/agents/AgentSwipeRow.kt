package com.forkmesh.nativeapp.feature.agents

import com.forkmesh.nativeapp.ui.kit.confirmAction
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.SwipeToDismissBox
import androidx.compose.material3.SwipeToDismissBoxValue
import androidx.compose.material3.rememberSwipeToDismissBoxState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentDeleteTarget
import com.forkmesh.nativeapp.agents.AgentSwipeDelete
import com.forkmesh.nativeapp.agents.ops.AgentDeleteModel
import com.forkmesh.nativeapp.agents.ops.AgentDeleteOutcome
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun AgentSwipeToDelete(
    target: AgentDeleteTarget?,
    status: FmStatus,
    onDeleted: (AgentDeleteTarget) -> Unit,
    content: @Composable () -> Unit,
) {
    if (target == null) {
        content()
        return
    }
    val c = Fm.colors
    val messenger = LocalFmMessenger.current
    val current by rememberUpdatedState(target)
    var deleting by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val state = rememberSwipeToDismissBoxState()
    SwipeToDismissBox(
        state = state,
        enableDismissFromStartToEnd = false,
        onDismiss = { value ->
            scope.launch { state.reset() }
            if (value == SwipeToDismissBoxValue.EndToStart && !deleting) {
                val pending = current
                deleting = true
                scope.launch {
                    try {
                        if (pending.needsConfirmation && !confirmOutsideDelete(messenger)) return@launch
                        status.clear()
                        val outcome = deleteAgentTarget(pending)
                        if (!outcome.failed) FmSoundCues.play(SoundCue.DELETE)
                        status.report(outcome.message, outcome.failed)
                        if (!outcome.failed) onDeleted(pending)
                    } finally {
                        deleting = false
                    }
                }
            }
        },
        backgroundContent = {
            Box(
                Modifier
                    .fillMaxSize()
                    .clip(RoundedCornerShape(FmRadius.md))
                    .background(c.danger)
                    .padding(horizontal = FmSpace.x4),
                contentAlignment = Alignment.CenterEnd,
            ) {
                FmIcon("delete_outline", size = 22.dp, tint = c.bgBase)
            }
        },
    ) {
        content()
    }
}

internal suspend fun confirmOutsideDelete(messenger: FmMessenger): Boolean = messenger.confirmAction(AgentSwipeDelete.OUTSIDE_CONFIRM_TITLE, "Delete", AgentSwipeDelete.OUTSIDE_CONFIRM_TEXT)

internal suspend fun deleteAgentTarget(target: AgentDeleteTarget): AgentDeleteOutcome = when (target) {
    is AgentDeleteTarget.Desktop, is AgentDeleteTarget.Outside -> {
        if (AgentsFeature.hub.desktop == null) {
            AgentDeleteOutcome.failed(AgentSwipeDelete.DESKTOP_OUT_OF_REACH)
        } else {
            val model = AgentDeleteModel(AgentOpsFeature.client, target.desktopId)
            when {
                !opsIo { model.prepare(); model.run() } ->
                    AgentDeleteOutcome.failed(model.errorText.ifEmpty { "Could not delete the agent." })
                target is AgentDeleteTarget.Outside -> AgentDeleteOutcome.deleted("Ended and removed on the desktop.")
                else -> AgentDeleteOutcome.deleted(model.report?.summary("on the desktop") ?: "Deleted on the desktop.")
            }
        }
    }
    is AgentDeleteTarget.Run -> try {
        val row = target.row
        val report = opsIo {
            AgentsFeature.runnerOps.delete(row.runnerRef(), row.branchName, active = row.isActive, pull = row.prNumber, merged = row.merged)
        }
        AgentDeleteOutcome.deleted(report.summary("on ForkMesh"))
    } catch (error: Exception) {
        AgentDeleteOutcome.failed(error.message ?: error.toString())
    }
}
