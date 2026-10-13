package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.control.AgentControlCenter
import com.forkmesh.nativeapp.control.LocalAgentQueue
import com.forkmesh.nativeapp.control.QueueMove
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal fun queueWaitingLabel(position: Int): String = "Waiting — #${position + 1} in queue"

internal fun queuedRowFor(center: AgentControlCenter, sessionId: Int): UnifiedAgentRow? =
    center.rows.firstOrNull { it.isDesktop && !it.outside && it.localId == sessionId }

@Composable
internal fun DesktopQueueSummaryRow(navigator: Navigator) {
    val center = AgentsFeature.control
    rememberCenterTick(center)
    val scope = rememberCoroutineScope()
    val messenger = LocalFmMessenger.current
    val queue = center.queue ?: return
    if (center.hub.desktop == null) return
    AgentTwoLineRow(
        icon = "low_priority",
        title = queue.summary,
        detail = "Change how many run at once and the order the waiting ones start",
        status = {},
        onTap = { scope.launch { showDesktopQueueSheet(messenger, navigator) } },
    )
}

@Composable
internal fun QueueMoveToTopButton(row: UnifiedAgentRow, status: FmStatus) {
    val center = AgentsFeature.control
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    var busy by remember(row.id) { mutableStateOf(false) }
    if (center.queuePosition(row) <= 0) return
    if (busy) {
        FmCircularProgressIndicator(Modifier.size(16.dp).padding(2.dp))
        return
    }
    FmIconButton(
        "vertical_align_top",
        "Move to top of the queue",
        onPressed = {
            busy = true
            status.clear()
            scope.launch {
                try {
                    withContext(Dispatchers.IO) { center.moveQueued(row, QueueMove.Top) }
                    status.show("Moved ${row.title} to the top of the queue.")
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    status.error(controlError(error))
                } finally {
                    busy = false
                }
            }
        },
        color = c.textSecondary,
    )
}

@Composable
internal fun QueuedRunNote(row: UnifiedAgentRow, modifier: Modifier = Modifier) {
    val center = AgentsFeature.control
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    var busy by remember(row.id) { mutableStateOf(false) }
    val position = center.queuePosition(row)
    if (position < 0) {
        FmInlineStatus(status, modifier)
        return
    }
    Column(modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                queueWaitingLabel(position),
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
                modifier = Modifier.weight(1f),
            )
            if (position > 0) {
                FmOutlinedButton(
                    "Move to top",
                    onClick = if (busy) null else ({
                        busy = true
                        status.clear()
                        scope.launch {
                            try {
                                withContext(Dispatchers.IO) { center.moveQueued(row, QueueMove.Top) }
                                status.show("Moved to the top of the queue.")
                            } catch (cancelled: CancellationException) {
                                throw cancelled
                            } catch (error: Exception) {
                                status.error(controlError(error))
                            } finally {
                                busy = false
                            }
                        }
                    }),
                    icon = "vertical_align_top",
                )
            }
        }
        FmInlineStatus(status)
    }
}

internal suspend fun showDesktopQueueSheet(messenger: FmMessenger, navigator: Navigator) {
    messenger.showModalBottomSheet<Unit>(showDragHandle = true, isScrollControlled = true) { close ->
        DesktopQueueSheet(onOpen = { row ->
            close(Unit)
            navigator.push(UnifiedAgentScreen(row))
        })
    }
}

@Composable
private fun DesktopQueueSheet(onOpen: (UnifiedAgentRow) -> Unit) {
    val c = Fm.colors
    val center = AgentsFeature.control
    rememberCenterTick(center)
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    var busy by remember { mutableStateOf(false) }
    val queue: LocalAgentQueue? = center.queue
    val desktop = center.hub.currentLink?.desktopName?.ifEmpty { null } ?: "your desktop"

    val act: (String?, () -> Unit) -> Unit = act@{ done, block ->
        if (busy) return@act
        busy = true
        status.clear()
        scope.launch {
            try {
                withContext(Dispatchers.IO) { block() }
                if (done != null) status.show(done)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                status.error(controlError(error))
            } finally {
                busy = false
            }
        }
    }

    Column(
        Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = FmSpace.x4)
            .padding(bottom = FmSpace.x4),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "Queue on $desktop",
                style = Fm.text.titleMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis),
                modifier = Modifier.weight(1f),
            )
            if (busy) FmCircularProgressIndicator(Modifier.size(16.dp))
        }
        FmInlineStatus(status, Modifier.padding(top = FmSpace.x1))
        if (queue == null) {
            Spacer(Modifier.height(FmSpace.x2))
            Text(
                "Update ForkMesh on the desktop to manage its queue from here.",
                style = Fm.text.bodyMedium.copy(color = c.textSecondary),
            )
        } else {
            QueueSheetBody(queue, center, busy, onOpen, act)
        }
    }
}

@Composable
private fun QueueSheetBody(
    queue: LocalAgentQueue,
    center: AgentControlCenter,
    busy: Boolean,
    onOpen: (UnifiedAgentRow) -> Unit,
    act: (String?, () -> Unit) -> Unit,
) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        Spacer(Modifier.height(FmSpace.x1))
        Text(queue.summary, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        Spacer(Modifier.height(FmSpace.x3))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    "Run ${queue.limit} agent${if (queue.limit == 1) "" else "s"} at once",
                    style = Fm.text.bodyMedium.copy(color = c.textPrimary),
                )
                Text(
                    "Lowering it puts the newest running agents back in the queue.",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
            }
            FmIconButton(
                "remove",
                "Run fewer at once",
                onPressed = if (busy || queue.limit <= LocalAgentQueue.MIN_LIMIT) null else ({
                    val next = queue.limit - 1
                    act(null) { center.setQueueLimit(next) }
                }),
                color = c.textSecondary,
            )
            Text(
                "${queue.limit}",
                style = Fm.text.titleMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis),
                modifier = Modifier.width(32.dp),
            )
            FmIconButton(
                "add",
                "Run more at once",
                onPressed = if (busy || queue.limit >= queue.limitMax) null else ({
                    val next = queue.limit + 1
                    act(null) { center.setQueueLimit(next) }
                }),
                color = c.accent,
            )
        }
        Spacer(Modifier.height(FmSpace.x4))
        Text("Waiting", style = Fm.text.labelLarge.copy(color = c.textSecondary))
        Spacer(Modifier.height(FmSpace.x2))
        if (queue.order.isEmpty()) {
            Text(
                "Nothing is waiting. Agents started beyond the limit wait here, first in line starts next.",
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
        }
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            queue.order.forEachIndexed { index, id ->
                val row = queuedRowFor(center, id)
                QueuedSheetItem(
                    index = index,
                    last = index == queue.order.lastIndex,
                    sessionId = id,
                    row = row,
                    busy = busy,
                    onOpen = { if (row != null) onOpen(row) },
                    onMove = { move ->
                        if (row != null) act(null) { center.moveQueued(row, move) }
                    },
                    onStop = { if (row != null) act("Asked it to stop.") { center.stop(row) } },
                )
            }
        }
    }
}

@Composable
private fun QueuedSheetItem(
    index: Int,
    last: Boolean,
    sessionId: Int,
    row: UnifiedAgentRow?,
    busy: Boolean,
    onOpen: () -> Unit,
    onMove: (QueueMove) -> Unit,
    onStop: () -> Unit,
) {
    val c = Fm.colors
    val movable = row != null && !busy
    Column(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().clickable(enabled = row != null) { onOpen() },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                "#${index + 1}",
                style = Fm.text.labelLarge.copy(color = c.textSecondary),
                modifier = Modifier.width(36.dp),
            )
            Column(Modifier.weight(1f)) {
                Text(
                    row?.title ?: "Agent $sessionId",
                    style = Fm.text.bodyMedium.copy(color = c.textPrimary),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                val detail = row?.let { liveDesktopDetail(it) } ?: ""
                if (detail.isNotEmpty()) {
                    Text(
                        detail,
                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
        Row(Modifier.fillMaxWidth().padding(start = 28.dp), verticalAlignment = Alignment.CenterVertically) {
            FmIconButton(
                "vertical_align_top",
                "Move to top",
                onPressed = if (movable && index > 0) ({ onMove(QueueMove.Top) }) else null,
                color = c.textSecondary,
            )
            FmIconButton(
                "keyboard_arrow_up",
                "Move up",
                onPressed = if (movable && index > 0) ({ onMove(QueueMove.Up) }) else null,
                color = c.textSecondary,
            )
            FmIconButton(
                "keyboard_arrow_down",
                "Move down",
                onPressed = if (movable && !last) ({ onMove(QueueMove.Down) }) else null,
                color = c.textSecondary,
            )
            FmIconButton(
                "vertical_align_bottom",
                "Move to bottom",
                onPressed = if (movable && !last) ({ onMove(QueueMove.Bottom) }) else null,
                color = c.textSecondary,
            )
            Spacer(Modifier.weight(1f))
            if (row != null && row.canStop) {
                FmIconButton(
                    "stop_circle_outlined",
                    "Stop",
                    onPressed = if (busy) null else onStop,
                    color = c.danger,
                )
            }
        }
    }
}
