package com.forkmesh.nativeapp.feature.hosts

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.ControlTransport
import com.forkmesh.nativeapp.control.DesktopTerminalInfo
import com.forkmesh.nativeapp.control.DesktopReach
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.hosts.AddHostForm
import com.forkmesh.nativeapp.hosts.HostAction
import com.forkmesh.nativeapp.hosts.HostActionOutcome
import com.forkmesh.nativeapp.hosts.HostConfirmation
import com.forkmesh.nativeapp.hosts.HostFormat
import com.forkmesh.nativeapp.hosts.HostHealth
import com.forkmesh.nativeapp.hosts.HostRow
import com.forkmesh.nativeapp.hosts.HostsStore
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun rememberStoreTick(store: HostsStore): Int {
    var tick by remember { mutableIntStateOf(0) }
    DisposableEffect(store) {
        val listener: () -> Unit = { tick++ }
        store.addListener(listener)
        store.channel.addReachListener(listener)
        onDispose {
            store.removeListener(listener)
            store.channel.removeReachListener(listener)
        }
    }
    return tick
}

internal suspend fun <T> offMain(block: () -> T): T = withContext(Dispatchers.IO) { block() }

internal fun describe(error: Throwable): String = ControlErrorText.describe(error)

@Composable
internal fun healthColor(health: HostHealth): Color {
    val c = Fm.colors
    return when (health) {
        HostHealth.Online -> c.success
        HostHealth.Attention -> c.warning
        HostHealth.Offline -> c.danger
        HostHealth.Provisioning -> c.accent
        HostHealth.Unknown -> c.textDisabled
    }
}

@Composable
internal fun Dot(color: Color, size: Int = 10) {
    Box(Modifier.size(size.dp).background(color, CircleShape))
}

@Composable
internal fun ReachLine(reach: DesktopReach) {
    val c = Fm.colors
    val color = when (reach) {
        is DesktopReach.Connected -> c.success
        is DesktopReach.Connecting -> c.warning
        is DesktopReach.Offline -> c.danger
        DesktopReach.NotPaired -> c.textDisabled
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Dot(color, 8)
        Spacer(Modifier.width(FmSpace.x2))
        Text(reach.label, style = Fm.text.bodySmall.copy(color = c.textSecondary), modifier = Modifier.weight(1f))
        if (reach is DesktopReach.Connected) {
            FmStatusBadge(if (reach.transport == ControlTransport.Lan) "LAN" else "Relay", if (reach.transport == ControlTransport.Lan) c.success else c.accent)
        }
    }
}

@Composable
internal fun OutcomeCard(outcome: HostActionOutcome) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth()) {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon(if (outcome.ok) "check_circle_outline" else "error_outline", size = 18.dp, tint = if (outcome.ok) c.success else c.danger)
                Spacer(Modifier.width(FmSpace.x2))
                Text(outcome.headline, style = Fm.text.titleSmall.copy(color = if (outcome.ok) c.success else c.danger))
            }
            if (outcome.message.isNotEmpty()) {
                SelectionContainer {
                    Text(outcome.message, style = Fm.text.bodySmall.copy(color = c.textSecondary), maxLines = 6)
                }
            }
        }
    }
}

internal class HostsScreen : Screen() {
    override val title: String get() = "Hosts"
    private var adding by mutableStateOf(false)
    private val status = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { HostsFeature.store() }
        val tick = rememberStoreTick(store)
        LaunchedEffect(store) { offMain { store.refresh() } }
        val snapshot = store.snapshot
        DetailScaffold(
            title,
            navigator,
            actions = {
                FmIconButton(
                    "add",
                    "Add host",
                    onPressed = if (snapshot?.canCreate == true && !snapshot.busy) ({ navigator.push(AddHostScreen(status)) }) else null,
                    color = c.textPrimary,
                )
                FmIconButton("hub_outlined", "Fleet", onPressed = { navigator.push(FleetScreen()) }, color = c.textPrimary)
                FmIconButton("refresh", "Refresh", onPressed = { store.refreshAsync() }, color = c.textPrimary)
            },
        ) {
            tick.let { }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                item {
                    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                        ReachLine(store.reach)
                        if (snapshot != null) {
                            Text(snapshot.summary, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                            if (snapshot.provisioning.isNotEmpty()) {
                                Text("Creating ${snapshot.provisioning}…", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                            }
                        }
                        if (store.error.isNotEmpty()) Text(store.error, style = Fm.text.bodySmall.copy(color = c.danger))
                        FmInlineStatus(status)
                    }
                }
                store.outcomes.firstOrNull()?.let { outcome ->
                    item { FmSectionHeader("Latest") }
                    item { OutcomeCard(outcome) }
                }
                if (snapshot != null) {
                    item { FmSectionHeader("Hosts", count = snapshot.hosts.size) }
                    if (snapshot.hosts.isEmpty()) {
                        item { Text("The desktop has no saved hosts yet.", style = Fm.text.bodyMedium.copy(color = c.textSecondary)) }
                    }
                    items(snapshot.sorted, key = { it.id }) { host ->
                        HostRowCard(host, store.isRunningAny(host.name)) { navigator.push(HostDetailScreen(host.name)) }
                    }
                } else if (store.loading) {
                    item {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                            Spacer(Modifier.width(FmSpace.x2))
                            Text("Asking the desktop…", style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                        }
                    }
                } else if (store.reach == DesktopReach.NotPaired) {
                    item {
                        FmEmptyState(
                            icon = "dns_outlined",
                            title = "No desktop paired",
                            message = "Open ForkMesh on your phone while it is on the same Wi-Fi as your desktop and approve it there. After that, hosts and agents work from anywhere, encrypted end to end through the relay.",
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun HostRowCard(host: HostRow, running: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth(), onTap = onTap) {
        Row(verticalAlignment = Alignment.Top) {
            Box(Modifier.padding(top = 5.dp)) { Dot(healthColor(host.health)) }
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(host.name, style = Fm.text.titleSmall.copy(color = c.textPrimary, fontWeight = FontWeight.SemiBold))
                    if (running) {
                        Spacer(Modifier.width(FmSpace.x2))
                        CircularProgressIndicator(Modifier.size(12.dp), strokeWidth = 1.5.dp)
                    }
                }
                Text(host.healthLine, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                val details = listOf(host.address, host.planLine, if (host.version.isEmpty()) "" else "v${host.version}")
                    .filter { it.isNotEmpty() }
                    .joinToString(" · ")
                if (details.isNotEmpty()) Text(details, style = Fm.text.labelSmall.copy(color = c.textTertiary), maxLines = 1)
            }
            FmIcon("chevron_right", size = 20.dp, tint = c.textDisabled)
        }
    }
}

private fun actionIcon(action: HostAction): String = when (action) {
    HostAction.Update -> "system_update_alt"
    HostAction.InstallBinary -> "upload"
    HostAction.Terminal -> "terminal"
    HostAction.Restart -> "restart_alt"
    HostAction.Stop -> "stop_circle"
    HostAction.Start -> "play_circle"
    HostAction.Install -> "add_circle_outline"
    HostAction.Destroy -> "delete_outline"
}

internal class HostDetailScreen(private val hostName: String) : Screen() {
    override val title: String get() = hostName
    private var detail by mutableStateOf<HostRow?>(null)
    private var error by mutableStateOf("")
    private var preparing by mutableStateOf<HostAction?>(null)
    private var confirming by mutableStateOf<HostConfirmation?>(null)
    private var sessions by mutableStateOf<List<DesktopTerminalInfo>>(emptyList())
    private val status = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { HostsFeature.store() }
        val tick = rememberStoreTick(store)
        suspend fun load() {
            try {
                detail = offMain { store.detail(hostName) }
                error = ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = describe(failure)
            }
            sessions = try {
                offMain { store.openTerminals(hostName) }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                emptyList()
            }
        }
        fun prepare(action: HostAction) {
            preparing = action
            status.clear()
            scope.launch {
                try {
                    confirming = offMain { store.prepare(action, hostName) }
                    error = ""
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = describe(failure)
                } finally {
                    preparing = null
                }
            }
        }
        LaunchedEffect(store.snapshot) { load() }
        val host = detail ?: store.host(hostName)
        DetailScaffold(
            hostName,
            navigator,
            actions = { FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } }, color = c.textPrimary) },
        ) {
            tick.let { }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                if (host != null) {
                    item {
                        FmCard(Modifier.fillMaxWidth()) {
                            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Dot(healthColor(host.health))
                                    Spacer(Modifier.width(FmSpace.x2))
                                    Text(host.healthLine, style = Fm.text.titleMedium.copy(color = c.textPrimary))
                                }
                                Fact("Address", host.address)
                                Fact("Hostname", host.dnsHostname)
                                Fact("SSH user", host.user)
                                Fact("Provider", host.provider)
                                Fact("Plan", host.planLine)
                                Fact("ForkMesh", host.version)
                                Fact("Install", host.status)
                            }
                        }
                    }
                    if (HostAction.Terminal in host.actions || sessions.isNotEmpty()) {
                        item { FmSectionHeader("Terminal") }
                        if (HostAction.Terminal in host.actions) {
                            item {
                                ActionRow(HostAction.Terminal, preparing == HostAction.Terminal, enabled = preparing == null) {
                                    prepare(HostAction.Terminal)
                                }
                            }
                        }
                        items(sessions, key = { it.terminal }) { session ->
                            FmCard(Modifier.fillMaxWidth(), onTap = { navigator.push(HostTerminalScreen(host.name, session.terminal)) }) {
                                Column {
                                    Text(
                                        if (session.running) "Open session" else "Ended session",
                                        style = Fm.text.bodyMedium.copy(color = c.textPrimary),
                                    )
                                    val from = if (session.device.isEmpty()) "" else "from ${session.device}"
                                    if (from.isNotEmpty()) Text(from, style = Fm.text.labelSmall.copy(color = c.textTertiary))
                                }
                            }
                        }
                        item {
                            Text(
                                "A shell on the host${if (host.user.isEmpty()) "" else " as ${host.user}"}, opened by the desktop with its saved key. " +
                                    "It stays open when you leave it; close it from its screen.",
                                style = Fm.text.labelSmall.copy(color = c.textTertiary),
                            )
                        }
                    }
                    val actions = host.actions.filterNot { it.opensTerminal }
                    if (actions.isNotEmpty()) {
                        item { FmSectionHeader("Actions") }
                        if (status.message != null) item(key = "status") { FmInlineStatus(status) }
                        items(actions, key = { it.wire }) { action ->
                            val busy = preparing == action || store.isRunning(action, host.name)
                            ActionRow(action, busy, enabled = preparing == null && !store.isRunning(action, host.name)) {
                                prepare(action)
                            }
                        }
                    }
                    item { FmSectionHeader("Log") }
                    item {
                        FmOutlinedButton("Live log", onClick = { navigator.push(HostLogScreen(host.name)) }, icon = "article_outlined")
                    }
                    item {
                        FmOutlinedButton("More operations", onClick = { navigator.push(HostOperationsScreen(host.name)) }, icon = "build_outlined")
                    }
                    if (host.recentLog.isNotEmpty()) {
                        items(host.recentLog.takeLast(12)) { line ->
                            Text(line, style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textSecondary), maxLines = 3)
                        }
                    }
                    if (host.lastResult.isNotEmpty()) {
                        item { FmSectionHeader("Desktop's last result") }
                        item {
                            SelectionContainer { Text(host.lastResult, style = Fm.text.bodySmall.copy(color = c.textPrimary)) }
                        }
                    }
                    val mine = store.outcomesFor(host.name)
                    if (mine.isNotEmpty()) {
                        item { FmSectionHeader("From this phone") }
                        items(mine, key = { it.id }) { OutcomeCard(it) }
                    }
                } else if (error.isEmpty()) {
                    item { CircularProgressIndicator(Modifier.size(24.dp)) }
                }
                if (error.isNotEmpty()) {
                    item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
                }
            }
        }
        confirming?.let { confirmation ->
            HostActionDialog(
                store,
                confirmation,
                status = status,
                onTerminal = { terminal -> navigator.push(HostTerminalScreen(hostName, terminal)) },
            ) { confirming = null }
        }
    }
}

@Composable
private fun Fact(label: String, value: String) {
    if (value.isEmpty()) return
    val c = Fm.colors
    Row {
        Text(label, style = Fm.text.bodySmall.copy(color = c.textTertiary), modifier = Modifier.width(96.dp))
        SelectionContainer { Text(value, style = Fm.text.bodySmall.copy(color = c.textPrimary)) }
    }
}

@Composable
private fun ActionRow(action: HostAction, busy: Boolean, enabled: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    val tint = if (action.isDestructive) c.danger else c.textPrimary
    FmCard(Modifier.fillMaxWidth(), onTap = if (enabled) onTap else null) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon(actionIcon(action), size = 20.dp, tint = if (enabled) tint else c.textDisabled)
            Spacer(Modifier.width(FmSpace.x3))
            Text(action.title, style = Fm.text.bodyLarge.copy(color = if (enabled) tint else c.textDisabled), modifier = Modifier.weight(1f))
            if (busy) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        }
    }
}

@Composable
internal fun HostActionDialog(
    store: HostsStore,
    confirmation: HostConfirmation,
    plan: String = "",
    region: String = "",
    status: FmStatus? = null,
    onDone: (Boolean) -> Unit = {},
    onTerminal: (String) -> Unit = {},
    onDismiss: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    var typed by remember(confirmation.token) { mutableStateOf("") }
    var running by remember(confirmation.token) { mutableStateOf(false) }
    var error by remember(confirmation.token) { mutableStateOf("") }
    val openedAt by remember(confirmation.token) { mutableLongStateOf(System.currentTimeMillis()) }
    FmConfirmDialog(
        title = confirmation.action.title,
        body = confirmation.summary + if (confirmation.action.opensTerminal) {
            "\n\nThe desktop opens the connection with its own key, which never leaves it. Opening and closing the session are written to the desktop's log."
        } else {
            "\n\nThe desktop runs this with its own keys; they never leave it. Every change is written to the desktop's log."
        },
        confirmLabel = confirmation.action.title,
        destructive = confirmation.action.isDestructive,
        busy = running,
        onDismissRequest = { if (!running) onDismiss() },
        onConfirm = {
            if (running || !confirmation.accepts(typed)) return@FmConfirmDialog
            if (System.currentTimeMillis() - openedAt > confirmation.expiresInMs) {
                error = ControlErrorText.describe("confirmation_expired")
                return@FmConfirmDialog
            }
            running = true
            scope.launch {
                try {
                    if (confirmation.action.opensTerminal) {
                        val terminal = offMain { store.openTerminal(confirmation) }
                        onDismiss()
                        onTerminal(terminal)
                        return@launch
                    }
                    val message = offMain { store.run(confirmation, typed, plan, region) }
                    if (message.isNotEmpty()) status?.show(message)
                    onDone(true)
                    onDismiss()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = describe(failure)
                } finally {
                    running = false
                }
            }
        },
        fields = if (!confirmation.needsTypedName && error.isEmpty()) null else ({
            if (confirmation.needsTypedName) {
                FmTextField(
                    value = typed,
                    onValueChange = { typed = it },
                    hint = confirmation.typeToConfirm,
                    helperText = "Type ${confirmation.typeToConfirm} to confirm",
                    keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
                    textStyle = Fm.text.bodyLarge.copy(fontFamily = FmType.mono),
                )
            }
            if (error.isNotEmpty()) Text(error, style = Fm.text.bodySmall.copy(color = Fm.colors.danger))
        }),
    )
}

internal class AddHostScreen(private val status: FmStatus? = null) : Screen() {
    override val title: String get() = "Add host"
    private var form by mutableStateOf<AddHostForm?>(null)
    private var error by mutableStateOf("")
    private var preparing by mutableStateOf(false)
    private var confirmation by mutableStateOf<HostConfirmation?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { HostsFeature.store() }
        LaunchedEffect(Unit) {
            try {
                val options = offMain { store.createOptions() }
                val next = AddHostForm(options)
                form = next
                error = if (options.vultr) "" else next.problem ?: ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = describe(failure)
            }
        }
        DetailScaffold(title, navigator) {
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                val current = form
                if (current != null) {
                    item { FmSectionHeader("Name") }
                    item {
                        FmTextField(
                            value = current.name,
                            onValueChange = { form = current.copy(name = it) },
                            hint = current.options.suggestedName.ifEmpty { "Automatic" },
                            helperText = "Leave empty to use the next free mirror name.",
                            keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
                        )
                    }
                    item { FmSectionHeader("Plan") }
                    items(current.options.plans, key = { "plan-" + it.id }) { plan ->
                        ChoiceRow(
                            label = if (plan.id == current.options.defaultPlan) "${plan.label} (default)" else plan.label,
                            detail = plan.id,
                            selected = plan.id == current.planId,
                        ) { form = current.choosePlan(plan.id) }
                    }
                    item { FmSectionHeader("Region") }
                    items(current.regions, key = { "region-$it" }) { code ->
                        ChoiceRow(HostFormat.region(code), "", code == current.region) { form = current.copy(region = code) }
                    }
                    item {
                        Text(
                            "The desktop's own choices: every plan with at least 1 GB of memory and IPv4 in a US region, cheapest first. The preselected plan and region are what the desktop picks by itself.",
                            style = Fm.text.bodySmall.copy(color = c.textTertiary),
                        )
                    }
                    current.problem?.let { problem ->
                        item { Text(problem, style = Fm.text.bodySmall.copy(color = c.warning)) }
                    }
                    item {
                        val name = current.effectiveName
                        FmOutlinedButton(
                            if (name.isEmpty()) "Create host" else "Create $name",
                            onClick = if (preparing || current.problem != null) null else ({
                                preparing = true
                                scope.launch {
                                    try {
                                        confirmation = offMain { store.prepare(HostAction.Install, current.name.trim()) }
                                        error = ""
                                    } catch (cancelled: CancellationException) {
                                        throw cancelled
                                    } catch (failure: Exception) {
                                        error = describe(failure)
                                    } finally {
                                        preparing = false
                                    }
                                }
                            }),
                            icon = "add_circle_outline",
                        )
                    }
                } else if (error.isEmpty()) {
                    item {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                            Spacer(Modifier.width(FmSpace.x2))
                            Text("Asking Vultr through the desktop…", style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                        }
                    }
                }
                if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
        confirmation?.let { pending ->
            HostActionDialog(
                store,
                pending,
                plan = form?.requestedPlan ?: "",
                region = form?.requestedRegion ?: "",
                status = status,
                onDone = { started -> if (started) navigator.pop() },
            ) { confirmation = null }
        }
    }
}

@Composable
private fun ChoiceRow(label: String, detail: String, selected: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onTap)
            .padding(vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(if (selected) "radio_button_checked" else "radio_button_unchecked", size = 20.dp, tint = if (selected) c.accent else c.textTertiary)
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(label, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
            if (detail.isNotEmpty()) Text(detail, style = Fm.text.labelSmall.copy(color = c.textTertiary))
        }
    }
}
