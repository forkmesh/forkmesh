package com.forkmesh.nativeapp.feature.hosts

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.hosts.DesktopUpdateStage
import com.forkmesh.nativeapp.hosts.DesktopUpdateStore
import com.forkmesh.nativeapp.hosts.RemoteConfirmation
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

@Composable
internal fun DesktopUpdateCard(store: DesktopUpdateStore) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    var pending by remember { mutableStateOf<RemoteConfirmation?>(null) }
    var pendingComputerRestart by remember { mutableStateOf<RemoteConfirmation?>(null) }
    var preparing by remember { mutableStateOf(false) }
    var preparingComputerRestart by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    var notice by remember { mutableStateOf("") }
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            if (store.version.isNotEmpty()) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Version", style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                    Spacer(Modifier.weight(1f))
                    Text(store.version, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontFamily = FmType.mono))
                }
            }
            if (store.unsupported) {
                Text(
                    "This desktop's ForkMesh is too old to update from your phone. Update it once on the desktop.",
                    style = Fm.text.bodySmall.copy(color = c.warning),
                )
            } else {
                FmToneButton(
                    "Rebuild and restart",
                    icon = "refresh",
                    tone = FmTone.Accent,
                    onPressed = if (store.busy || preparing || !store.loaded) null else ({
                        preparing = true
                        notice = ""
                        scope.launch {
                            try {
                                pending = offMain { store.prepare() }
                                error = ""
                            } catch (cancelled: CancellationException) {
                                throw cancelled
                            } catch (failure: Exception) {
                                error = describeRemote(failure)
                            } finally {
                                preparing = false
                            }
                        }
                    }),
                )
                FmToneButton(
                    "Restart computer",
                    icon = "power_settings_new",
                    tone = FmTone.Danger,
                    onPressed = if (store.busy || preparingComputerRestart || !store.loaded) null else ({
                        preparingComputerRestart = true
                        notice = ""
                        scope.launch {
                            try {
                                pendingComputerRestart = offMain { store.prepareComputerRestart() }
                                error = ""
                            } catch (cancelled: CancellationException) {
                                throw cancelled
                            } catch (failure: Exception) {
                                error = describeRemote(failure)
                            } finally {
                                preparingComputerRestart = false
                            }
                        }
                    }),
                )
            }
            DesktopUpdateProgress(store)
            FmInlineStatusLine(notice, isError = false, onDismiss = { notice = "" })
            if (error.isNotEmpty()) Text(error, style = Fm.text.bodySmall.copy(color = c.danger))
            Text(
                "Rebuild and restart rebuilds ForkMesh from the desktop's source checkout. Restart computer restarts this entire computer and closes every running program.",
                style = Fm.text.labelSmall.copy(color = c.textTertiary),
            )
        }
    }
    pending?.let { confirmation ->
        RemoteConfirmDialog("Rebuild and restart", confirmation, perform = { conf, _ -> store.run(conf) }, onDone = { notice = it }) { pending = null }
    }
    pendingComputerRestart?.let { confirmation ->
        RemoteConfirmDialog(
            "Restart computer", confirmation, destructive = true,
            perform = { conf, _ -> store.restartComputer(conf) },
            onDone = { notice = it },
        ) { pendingComputerRestart = null }
    }
}

@Composable
private fun DesktopUpdateProgress(store: DesktopUpdateStore) {
    val c = Fm.colors
    val state = store.state
    when (store.stage) {
        DesktopUpdateStage.Running -> Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            FmLinearProgressIndicator(Modifier.fillMaxWidth(), progress = { state.percent / 100f })
            Text(state.headline.ifEmpty { "Starting the rebuild…" }, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        DesktopUpdateStage.Restarting -> Row(verticalAlignment = Alignment.CenterVertically) {
            CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
            Spacer(Modifier.width(FmSpace.x2))
            Text("Restarting. Waiting for the desktop to come back…", style = Fm.text.bodySmall.copy(color = c.textPrimary))
        }
        DesktopUpdateStage.Back -> Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon("check_circle_outline", size = 16.dp, tint = c.success)
            Spacer(Modifier.width(FmSpace.x2))
            Text(backLine(store), style = Fm.text.bodySmall.copy(color = c.success))
        }
        DesktopUpdateStage.Failed -> Row(verticalAlignment = Alignment.Top) {
            FmIcon("error_outline", size = 16.dp, tint = c.danger)
            Spacer(Modifier.width(FmSpace.x2))
            Text(state.error.ifEmpty { store.error }, style = Fm.text.bodySmall.copy(color = c.danger))
        }
        DesktopUpdateStage.Idle -> if (state.status.isNotEmpty() && store.lines.isNotEmpty()) {
            Text(state.status, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
    }
    if (store.lines.isNotEmpty()) DesktopUpdateLog(store.lines)
}

private fun backLine(store: DesktopUpdateStore): String = when {
    store.previousVersion.isNotEmpty() && store.previousVersion != store.version ->
        "Restarted on ${store.version} (was ${store.previousVersion})."
    store.version.isEmpty() -> "The desktop restarted."
    else -> "Restarted on ${store.version}."
}

@Composable
private fun DesktopUpdateLog(lines: List<String>) {
    val c = Fm.colors
    val list = rememberLazyListState()
    LaunchedEffect(lines.size, lines.lastOrNull()) {
        if (lines.isNotEmpty()) list.scrollToItem(lines.lastIndex)
    }
    SelectionContainer {
        LazyColumn(
            Modifier
                .fillMaxWidth()
                .height(220.dp)
                .background(c.bgOverlay, RoundedCornerShape(8.dp)),
            state = list,
            contentPadding = PaddingValues(FmSpace.x2),
        ) {
            itemsIndexed(lines) { _, line ->
                Text(line, style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, fontSize = 11.sp, color = lineColor(line)))
            }
        }
    }
}

@Composable
private fun lineColor(line: String): Color {
    val c = Fm.colors
    return when {
        line.startsWith("!!") -> c.danger
        line.startsWith("==>") -> c.accent
        line.startsWith("$ ") -> c.textSecondary
        else -> c.textPrimary
    }
}
