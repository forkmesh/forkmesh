package com.forkmesh.nativeapp.feature.agents

import android.text.format.DateUtils
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.CircularProgressIndicator
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
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.ops.RecycleBinModel
import com.forkmesh.nativeapp.agents.ops.RecycledAgent
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.launch

internal class RecycleBinScreen : Screen() {
    override val title: String get() = "Recycle bin"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val model = remember { RecycleBinModel(AgentOpsFeature.client) }
        val tick = rememberOpsTick(model)
        val scope = rememberCoroutineScope()
        var retentionOpen by remember { mutableStateOf(false) }
        LaunchedEffect(model) { opsIo { model.refresh() } }
        val restore: (List<Int>) -> Unit = { ids ->
            scope.launch {
                if (opsIo { model.restore(ids) }) AgentsFeature.control.refreshAsync()
            }
        }
        val purge: (List<Int>) -> Unit = { ids -> scope.launch { opsIo { model.preparePurge(ids) } } }
        DetailScaffold(title, navigator, actions = {
            if (!model.bin.isEmpty) {
                FmIconButton(
                    if (model.selectedIds.isEmpty()) "select_all" else "deselect",
                    if (model.selectedIds.isEmpty()) "Select all" else "Clear selection",
                    onPressed = { if (model.selectedIds.isEmpty()) model.selectAll() else model.clearSelection() },
                    color = c.textPrimary,
                )
            }
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { opsIo { model.refresh() } } }, color = c.textPrimary)
        }) {
            tick.let { }
            val bin = model.bin
            val selected = model.selectedIds
            val targets = selected.ifEmpty { bin.ids }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                if (model.errorText.isNotEmpty()) item { Text(model.errorText, style = Fm.text.bodySmall.copy(color = c.danger)) }
                if (model.loading && !model.loaded) {
                    item { Box(Modifier.fillMaxWidth().height(120.dp), contentAlignment = Alignment.Center) { CircularProgressIndicator() } }
                }
                if (model.loaded && !model.unsupported) {
                    item {
                        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            if (!bin.isEmpty) Text(bin.hint, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                            Box {
                                FmToneButton(
                                    "Keep deleted agents for ${retentionLabel(bin.retentionHours)}",
                                    onPressed = if (model.busy) null else ({ retentionOpen = true }),
                                    icon = "schedule",
                                )
                                FmDropdownMenu(expanded = retentionOpen, onDismissRequest = { retentionOpen = false }) {
                                    for (hours in RETENTION_CHOICES.filter { it in bin.minHours..bin.maxHours }) {
                                        FmDropdownMenuItem(
                                            retentionLabel(hours),
                                            onClick = {
                                                retentionOpen = false
                                                scope.launch { opsIo { model.setRetention(hours) } }
                                            },
                                            icon = if (hours == bin.retentionHours) "check" else null,
                                        )
                                    }
                                }
                            }
                            if (!bin.isEmpty) {
                                ChipRow {
                                    FmToneButton(
                                        if (selected.isEmpty()) "Restore all" else "Restore ${selected.size}",
                                        onPressed = if (model.busy) null else ({ restore(targets) }),
                                        icon = "restore_from_trash_outlined",
                                        tone = FmTone.Success,
                                    )
                                    FmToneButton(
                                        if (selected.isEmpty()) "Empty bin" else "Delete ${selected.size}",
                                        onPressed = if (model.busy) null else ({ purge(targets) }),
                                        icon = "delete_forever_outlined",
                                        tone = FmTone.Danger,
                                    )
                                }
                            }
                            if (model.note.isNotEmpty()) Text(model.note, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        }
                    }
                }
                if (model.loaded && bin.isEmpty && model.errorText.isEmpty()) {
                    item {
                        Box(Modifier.fillMaxWidth().height(260.dp)) {
                            FmEmptyState(
                                icon = "delete_outline",
                                title = "The recycle bin is empty",
                                message = "Agents you delete on the desktop or from here wait in its recycle bin until they are removed for good.",
                            )
                        }
                    }
                }
                items(bin.agents, key = { it.id }) { agent ->
                    RecycledRow(
                        agent,
                        checked = agent.id in model.selection,
                        busy = model.busy,
                        onToggle = { model.toggle(agent.id) },
                        onRestore = { restore(listOf(agent.id)) },
                        onPurge = { purge(listOf(agent.id)) },
                    )
                }
                if (bin.truncated) {
                    item {
                        Text(
                            "Showing the newest ${bin.agents.size} of ${bin.count}. Open the recycle bin on the desktop for the rest.",
                            style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        )
                    }
                }
            }
            model.confirmation?.let { confirmation ->
                FmConfirmDialog(
                    title = "Delete permanently",
                    body = confirmation.summary,
                    confirmLabel = "Delete permanently",
                    destructive = true,
                    onConfirm = { scope.launch { opsIo { model.purge() } } },
                    onDismissRequest = { model.cancelPurge() },
                )
            }
        }
    }

    @Composable
    private fun RecycledRow(
        agent: RecycledAgent,
        checked: Boolean,
        busy: Boolean,
        onToggle: () -> Unit,
        onRestore: () -> Unit,
        onPurge: () -> Unit,
    ) {
        val c = Fm.colors
        FmCard(Modifier.fillMaxWidth().clickable(onClick = onToggle)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmCheckbox(checked = checked, onCheckedChange = { onToggle() })
                Spacer(Modifier.width(FmSpace.x2))
                Column(Modifier.weight(1f)) {
                    Text(
                        agent.headline,
                        style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis),
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        listOf("#${agent.id}", agent.repo, agent.branch).filter { it.isNotEmpty() }.joinToString(" · "),
                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(deletedLine(agent), style = Fm.text.labelSmall.copy(color = c.textSecondary))
                }
                FmIconButton("restore_from_trash_outlined", "Restore", onPressed = if (busy) null else onRestore, color = c.success)
                FmIconButton("delete_forever_outlined", "Delete permanently", onPressed = if (busy) null else onPurge, color = c.danger)
            }
        }
    }

    private fun deletedLine(agent: RecycledAgent): String {
        if (agent.deletedAtMs <= 0) return agent.removal
        val ago = DateUtils.getRelativeTimeSpanString(agent.deletedAtMs, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS)
        return "Deleted $ago · ${agent.removal}"
    }

    private fun retentionLabel(hours: Int): String = when {
        hours >= 48 && hours % 24 == 0 -> "${hours / 24} days"
        hours == 1 -> "1 hour"
        else -> "$hours hours"
    }

    private companion object {
        val RETENTION_CHOICES = listOf(1, 6, 12, 24, 48, 72, 168, 336, 720)
    }
}
