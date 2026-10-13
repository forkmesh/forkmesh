package com.forkmesh.nativeapp.feature.hosts

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.control.LocalControlService
import com.forkmesh.nativeapp.control.LocalControlStatus
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.hosts.DesktopBackupResult
import com.forkmesh.nativeapp.hosts.DesktopBackupStatus
import com.forkmesh.nativeapp.hosts.DesktopHealth
import com.forkmesh.nativeapp.hosts.DesktopLogTail
import com.forkmesh.nativeapp.hosts.DiagnosticsAction
import com.forkmesh.nativeapp.hosts.LogSeverity
import com.forkmesh.nativeapp.hosts.RemoteConfirmation
import com.forkmesh.nativeapp.hosts.RemoteFormat
import com.forkmesh.nativeapp.hosts.StallRecord
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

@Composable
internal fun HealthGauges(health: DesktopHealth, compact: Boolean) {
    val c = Fm.colors
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        Gauge("CPU", if (health.cpuPercent >= 0) RemoteFormat.percent(health.cpuPercent) + " · load " + health.loadLine else "—", (health.cpuPercent / 100).coerceIn(0.0, 1.0))
        Gauge("Memory", health.memoryLine, health.memoryUsedShare)
        Gauge("Disk", health.diskLine, health.diskUsedShare)
        if (health.onBattery) {
            val share = health.batteryShare
            Gauge("Battery", health.batteryLine, share, if (share <= 0.2) c.danger else if (share <= 0.4) c.warning else c.accent)
        }
        if (!compact) {
            if (health.appResident > 0) Text("ForkMesh uses ${RemoteFormat.bytes(health.appResident)}", style = Fm.text.bodySmall.copy(color = c.textSecondary))
            if (health.openFiles >= 0) {
                val limit = if (health.openFileLimit > 0) " of ${health.openFileLimit}" else ""
                Text("${health.openFiles}$limit open files · ${maxOf(0, health.threads)} threads", style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
        }
    }
}

@Composable
private fun Gauge(label: String, line: String, share: Double, tintOverride: Color? = null) {
    val c = Fm.colors
    val tint = tintOverride ?: when {
        share > 0.9 -> c.danger
        share > 0.75 -> c.warning
        else -> c.accent
    }
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        Row {
            Text(label, style = Fm.text.labelMedium.copy(color = c.textSecondary), modifier = Modifier.weight(1f))
            Text(line, style = Fm.text.labelMedium.copy(color = c.textPrimary))
        }
        ShareBar(share, tint)
    }
}

internal class DesktopsScreen : Screen() {
    override val title: String get() = "Desktops"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { DesktopRemoteFeature.diagnostics }
        val tick = rememberRemoteTick(store)
        val hub = DesktopRemoteFeature.hub
        LaunchedEffect(store) { offMain { store.refreshHealth() } }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { Thread { store.refreshHealth() }.start() }, color = c.textPrimary)
        }) {
            tick.let { }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                val desktop = hub.desktop
                if (desktop != null) {
                    item {
                        FmCard(Modifier.fillMaxWidth(), onTap = { navigator.push(DesktopScreen()) }) {
                            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    FmIcon("desktop_windows", size = 22.dp, tint = c.textPrimary)
                                    Spacer(Modifier.width(FmSpace.x2))
                                    Text(desktop.name.ifEmpty { "Desktop" }, style = Fm.text.titleMedium.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                                    store.health?.let { FmStatusBadge(it.level.label, healthLevelColor(it.level)) }
                                }
                                ReachLine(hub.reach)
                                store.health?.let { HealthGauges(it, compact = true) }
                            }
                        }
                    }
                    if (store.unsupported) item { Text("Update ForkMesh on this desktop to see its health here.", style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                    else if (store.error.isNotEmpty() && hub.reach.isConnected) item { Text(store.error, style = Fm.text.bodySmall.copy(color = c.danger)) }
                } else {
                    item { DesktopDiscoveryStatus(hub.local) }
                }
                item {
                    FmOutlinedButton("Desktop on this network", onClick = { navigator.push(AgentsFeature.localDesktopScreen()) }, icon = "wifi")
                }
            }
        }
    }
}

@Composable
private fun DesktopDiscoveryStatus(local: LocalControlService?) {
    val c = Fm.colors
    val hosts = local?.hosts.orEmpty()
    val (heading, body) = when (local?.status) {
        LocalControlStatus.Connecting -> "Connecting to ${local?.desktopName.orEmpty()}…" to ""
        LocalControlStatus.AwaitingApproval -> "Approve this phone on ${local?.desktopName.orEmpty()}" to
            "The desktop is asking whether to let this phone control it. Allow it only if it shows the same code."
        LocalControlStatus.Failed -> "Not connected" to local?.error.orEmpty()
        LocalControlStatus.Connected -> "Connected to ${local?.desktopName.orEmpty()}" to ""
        else -> if (hosts.isEmpty()) {
            "Looking for your desktop" to "Open ForkMesh on the desktop while this phone is on the same Wi-Fi. A desktop signed in to your account connects by itself."
        } else {
            "Not connected" to "Found ${hosts.joinToString { it.label }} on this network. Open Desktop on this network to connect."
        }
    }
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        Text(heading, style = Fm.text.titleSmall.copy(color = c.textPrimary))
        if (body.isNotEmpty()) {
            Text(body, style = Fm.text.bodyMedium.copy(color = if (local?.status == LocalControlStatus.Failed) c.danger else c.textSecondary))
        }
        val code = local?.pairingCode.orEmpty()
        if (local?.status == LocalControlStatus.AwaitingApproval && code.isNotEmpty()) {
            Text(code.chunked(3).joinToString(" "), style = Fm.text.displaySmall.copy(color = c.textPrimary, fontFamily = FmType.mono))
        }
    }
}

internal class DesktopScreen : Screen() {
    override val title: String get() = DesktopRemoteFeature.hub.desktop?.name?.ifEmpty { null } ?: "Desktop"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { DesktopRemoteFeature.diagnostics }
        val update = remember { DesktopRemoteFeature.update }
        val tick = rememberRemoteTick(store)
        val updateTick = rememberRemoteTick(update)
        val hub = DesktopRemoteFeature.hub
        LaunchedEffect(store) {
            offMain {
                update.refresh()
                store.refreshHealth()
                store.refreshBackups()
            }
        }
        DisposableEffect(update) {
            onDispose { Thread { update.unwatch() }.start() }
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { Thread { store.refreshHealth(); store.refreshBackups(); update.refresh() }.start() }, color = c.textPrimary)
        }) {
            tick.let { }
            updateTick.let { }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                item {
                    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                        ReachLine(hub.reach)
                        if (store.unsupported) OutdatedNote()
                    }
                }
                val health = store.health
                if (health != null) {
                    item { FmSectionHeader("Health") }
                    item {
                        FmCard(Modifier.fillMaxWidth()) {
                            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    FmStatusBadge(health.level.label, healthLevelColor(health.level))
                                    Spacer(Modifier.weight(1f))
                                    Text(health.agentsLine, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                                }
                                HealthGauges(health, compact = false)
                                if (health.pressureLine.isNotEmpty()) Text(health.pressureLine, style = Fm.text.labelSmall.copy(color = c.textTertiary))
                            }
                        }
                    }
                } else if (store.loading) {
                    item { CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) }
                }
                item { FmSectionHeader("On the desktop") }
                item { LinkRow("Files", "folder_outlined") { navigator.push(FileRootsScreen()) } }
                item { LinkRow("Log", "article_outlined") { navigator.push(DesktopLogScreen()) } }
                item {
                    val stalls = health?.stalls ?: 0
                    LinkRow("UI stalls", "hourglass_empty", if (stalls > 0) "$stalls" else "") { navigator.push(StallsScreen()) }
                }
                item { LinkRow("Memory", "memory") { navigator.push(ProcessesScreen()) } }
                item { LinkRow("Fleet", "dns_outlined") { navigator.push(FleetScreen()) } }
                item { FmSectionHeader("ForkMesh on this desktop") }
                item { DesktopUpdateCard(update) }
                store.backups?.let { backups ->
                    item { FmSectionHeader("Backups") }
                    item { BackupsCard(backups) }
                }
                if (store.error.isNotEmpty()) item { Text(store.error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
    }
}

@Composable
internal fun LinkRow(title: String, icon: String, trailing: String = "", onTap: () -> Unit) {
    val c = Fm.colors
    FmListRow(
        title,
        leading = { FmIcon(icon, size = 20.dp, tint = c.textSecondary) },
        trailing = if (trailing.isEmpty()) null else ({ Text(trailing, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }),
        onTap = onTap,
        minHeight = 48.dp,
    )
}

@Composable
private fun BackupsCard(backups: DesktopBackupStatus) {
    val c = Fm.colors
    val tone = when (backups.result) {
        DesktopBackupResult.Ok -> c.success
        DesktopBackupResult.Running -> c.accent
        DesktopBackupResult.Failed -> c.danger
        else -> c.textSecondary
    }
    FmCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmStatusBadge(backups.result.label, tone)
                Spacer(Modifier.weight(1f))
                if (backups.enabled) Text("Keeps ${backups.keep}", style = Fm.text.labelSmall.copy(color = c.textTertiary))
            }
            if (backups.hasBackup) Text("Last backup ${relativeTime(backups.lastBackupAtMs)} · ${backups.sizeLine}", style = Fm.text.bodySmall.copy(color = c.textSecondary))
            if (backups.nextDueAtMs > 0 && backups.enabled) Text("Next one ${relativeTime(backups.nextDueAtMs)}", style = Fm.text.bodySmall.copy(color = c.textSecondary))
            if (backups.location.isNotEmpty()) Text(backups.location, style = Fm.text.labelSmall.copy(color = c.textTertiary))
            if (backups.errorText.isNotEmpty()) Text(backups.errorText, style = Fm.text.bodySmall.copy(color = c.danger))
        }
    }
}

internal class DesktopLogScreen : Screen() {
    override val title: String get() = "Desktop log"
    private var severity by mutableStateOf(LogSeverity.All)
    private var query by mutableStateOf("")
    private var version by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.diagnostics }
        val tail = remember(store) { DesktopLogTail(store) }
        fun reload() {
            tail.severity = severity
            tail.query = query.trim()
            scope.launch {
                offMain { tail.loadLatest() }
                version++
            }
        }
        LaunchedEffect(tail) { reload() }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Newest", onPressed = { reload() }, color = c.textPrimary)
        }) {
            version.let { }
            Column(Modifier.fillMaxSize()) {
                val tabs = LogSeverity.entries.filter { it != LogSeverity.Info }
                FmSegmentTabBar(
                    tabs.map { FmSegmentTab(it.title) },
                    tabs.indexOf(severity).coerceAtLeast(0),
                    onChanged = {
                        severity = tabs[it]
                        reload()
                    },
                )
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(value = query, onValueChange = { query = it }, hint = "Search the log", modifier = Modifier.weight(1f))
                    FmIconButton("search", "Search", onPressed = { reload() }, color = c.textPrimary)
                }
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x3), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    if (tail.more) {
                        item {
                            FmToneButton("Older lines", onPressed = if (tail.loading) null else ({
                                scope.launch {
                                    offMain { tail.loadOlder() }
                                    version++
                                }
                            }))
                        }
                    }
                    if (tail.lines.isEmpty() && !tail.loading) {
                        item { Text(if (tail.error.isEmpty()) "No lines match." else tail.error, style = Fm.text.bodySmall.copy(color = if (tail.error.isEmpty()) c.textSecondary else c.danger)) }
                    }
                    items(tail.lines, key = { it.offset }) { line ->
                        val tint = when (line.severity) {
                            LogSeverity.Error -> c.danger
                            LogSeverity.Warning -> c.warning
                            else -> c.textSecondary
                        }
                        SelectionContainer {
                            Text(line.text, style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = tint))
                        }
                    }
                    if (tail.lines.isNotEmpty()) {
                        item { Text("${tail.lines.size} lines · ${tail.errorCount} errors", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                    }
                }
            }
        }
    }
}

internal class StallsScreen : Screen() {
    override val title: String get() = "UI stalls"
    private var pending by mutableStateOf<RemoteConfirmation?>(null)
    private var error by mutableStateOf("")
    private var notice by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.diagnostics }
        val tick = rememberRemoteTick(store)
        LaunchedEffect(store) { offMain { store.refreshStalls() } }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { Thread { store.refreshStalls() }.start() }, color = c.textPrimary)
        }) {
            tick.let { }
            val list = store.stalls
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                if (list == null) {
                    item { if (store.error.isEmpty()) CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) else Text(store.error, color = c.danger) }
                } else if (list.stalls.isEmpty()) {
                    item { FmEmptyState(icon = "hourglass_empty", title = "No stalls recorded", message = "The desktop's interface has not frozen since its log was last cleared.") }
                } else {
                    if (list.canSend) {
                        item {
                            FmToneButton("Send all to an agent", icon = "auto_awesome", tone = FmTone.Accent, onPressed = {
                                scope.launch {
                                    try {
                                        pending = offMain { store.prepare(DiagnosticsAction.SendStalls) }
                                    } catch (cancelled: CancellationException) {
                                        throw cancelled
                                    } catch (failure: Exception) {
                                        error = describeRemote(failure)
                                    }
                                }
                            })
                        }
                    }
                    if (notice.isNotEmpty()) item { FmInlineStatusLine(notice, isError = false, onDismiss = { notice = "" }) }
                    item { FmSectionHeader("Recorded", count = list.stalls.size) }
                    items(list.stalls, key = { it.index }) { stall ->
                        FmListRow(stall.summary.ifEmpty { "Stall ${stall.index}" }, subtitle = "${stall.whenText} · ${stall.lines} lines", onTap = { navigator.push(StallDetailScreen(stall.index, list.canSend)) })
                    }
                }
                if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
        pending?.let { confirmation ->
            RemoteConfirmDialog("Send to an agent", confirmation, perform = { conf, _ -> store.run(conf) }, onDone = { notice = it }) { pending = null }
        }
    }
}

internal class StallDetailScreen(private val index: Int, private val canSend: Boolean) : Screen() {
    override val title: String get() = "Stall"
    private var record by mutableStateOf<StallRecord?>(null)
    private var error by mutableStateOf("")
    private var notice by mutableStateOf("")
    private var pending by mutableStateOf<RemoteConfirmation?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.diagnostics }
        LaunchedEffect(index) {
            try {
                record = offMain { store.stall(index) }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = describeRemote(failure)
            }
        }
        DetailScaffold(title, navigator, actions = {
            if (canSend) FmIconButton("auto_awesome", "Send to an agent", onPressed = {
                scope.launch {
                    try {
                        pending = offMain { store.prepare(DiagnosticsAction.SendStall(index)) }
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Exception) {
                        error = describeRemote(failure)
                    }
                }
            }, color = c.textPrimary)
        }) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                if (notice.isNotEmpty()) item { FmInlineStatusLine(notice, isError = false, onDismiss = { notice = "" }) }
                record?.let { stall ->
                    item { Text(stall.summary, style = Fm.text.titleSmall.copy(color = c.textPrimary, fontWeight = FontWeight.SemiBold)) }
                    item { Text(stall.whenText, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                    item { SelectionContainer { Text(stall.text, style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textPrimary)) } }
                    if (stall.truncated) item { Text("Only the start of this record is shown.", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                }
                if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
        pending?.let { confirmation ->
            RemoteConfirmDialog("Send to an agent", confirmation, perform = { conf, _ -> store.run(conf) }, onDone = { notice = it }) { pending = null }
        }
    }
}

internal class ProcessesScreen : Screen() {
    override val title: String get() = "Memory"
    private var pending by mutableStateOf<RemoteConfirmation?>(null)
    private var error by mutableStateOf("")
    private var notice by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.diagnostics }
        val tick = rememberRemoteTick(store)
        LaunchedEffect(store) { offMain { store.refreshProcesses() } }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { Thread { store.refreshProcesses() }.start() }, color = c.textPrimary)
        }) {
            tick.let { }
            val list = store.processes
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                if (list == null) {
                    item { if (store.error.isEmpty()) CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) else Text(store.error, color = c.danger) }
                } else {
                    if (notice.isNotEmpty()) item { FmInlineStatusLine(notice, isError = false, onDismiss = { notice = "" }) }
                    if (list.agentProcesses.isNotEmpty()) {
                        item { FmSectionHeader("Agent runs") }
                        items(list.agentProcesses, key = { "a${it.pid}" }) { process ->
                            val agent = process.agent!!
                            FmCard(Modifier.fillMaxWidth()) {
                                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                    Row(verticalAlignment = Alignment.CenterVertically) {
                                        Text(agent.label.ifEmpty { "Session ${agent.session}" }, style = Fm.text.titleSmall.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                                        Text(RemoteFormat.bytes(process.rssBytes), style = Fm.text.labelMedium.copy(color = c.textSecondary))
                                    }
                                    ShareBar(list.share(process))
                                    if (agent.stoppable) {
                                        FmToneButton("Stop", tone = FmTone.Danger, onPressed = {
                                            scope.launch {
                                                try {
                                                    pending = offMain { store.prepare(DiagnosticsAction.StopAgent(agent.session)) }
                                                } catch (cancelled: CancellationException) {
                                                    throw cancelled
                                                } catch (failure: Exception) {
                                                    error = describeRemote(failure)
                                                }
                                            }
                                        })
                                    }
                                }
                            }
                        }
                    }
                    item { FmSectionHeader("Largest memory users") }
                    items(list.processes, key = { it.pid }) { process ->
                        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                            Row {
                                Text(process.name, style = Fm.text.bodyMedium.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                                Text(RemoteFormat.bytes(process.rssBytes), style = Fm.text.labelMedium.copy(color = c.textSecondary))
                            }
                            ShareBar(list.share(process))
                            if (process.user.isNotEmpty()) Text("pid ${process.pid} · ${process.user}", style = Fm.text.labelSmall.copy(color = c.textTertiary))
                        }
                    }
                }
                if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
        pending?.let { confirmation ->
            RemoteConfirmDialog("Stop agent", confirmation, destructive = true, perform = { conf, _ -> store.run(conf) }, onDone = { notice = it }) { pending = null }
        }
    }
}
