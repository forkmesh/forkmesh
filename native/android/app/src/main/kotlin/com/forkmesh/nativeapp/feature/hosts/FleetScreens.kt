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
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.hosts.DiskUsage
import com.forkmesh.nativeapp.hosts.FleetAction
import com.forkmesh.nativeapp.hosts.FleetDeployMode
import com.forkmesh.nativeapp.hosts.FleetInput
import com.forkmesh.nativeapp.hosts.FleetStore
import com.forkmesh.nativeapp.hosts.FleetSummary
import com.forkmesh.nativeapp.hosts.ProvisionStageState
import com.forkmesh.nativeapp.hosts.ProvisionStatus
import com.forkmesh.nativeapp.hosts.RemoteConfirmation
import com.forkmesh.nativeapp.hosts.RemoteFormat
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
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
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

internal class PendingRemote(val title: String, val confirmation: RemoteConfirmation, val destructive: Boolean)

internal abstract class FleetActionScreen : Screen() {
    protected var pending by mutableStateOf<PendingRemote?>(null)
    protected var error by mutableStateOf("")
    protected var banner by mutableStateOf("")
    protected var preparing by mutableStateOf(false)

    protected fun prepare(scope: CoroutineScope, store: FleetStore, action: FleetAction) {
        if (preparing) return
        preparing = true
        scope.launch {
            try {
                val confirmation = offMain { store.prepare(action) }
                pending = PendingRemote(action.title, confirmation, action.isDestructive)
                error = ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = describeRemote(failure)
            } finally {
                preparing = false
            }
        }
    }

    @Composable
    protected fun PendingDialog(store: FleetStore) {
        pending?.let { item ->
            RemoteConfirmDialog(
                item.title,
                item.confirmation,
                destructive = item.destructive,
                perform = { confirmation, typed -> store.run(confirmation, typed) },
                onDone = { banner = it },
            ) { pending = null }
        }
    }

    protected fun LazyListScope.notices() {
        if (banner.isNotEmpty()) item { Text(banner, style = Fm.text.bodySmall.copy(color = Fm.colors.success)) }
        if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = Fm.colors.danger)) }
    }
}

internal class FleetScreen : FleetActionScreen() {
    override val title: String get() = "Fleet"
    private var targetOn by mutableStateOf(false)
    private var desired by mutableIntStateOf(3)
    private var linkCode by mutableStateOf("")
    private var synced by mutableStateOf(false)
    private var deployOpen by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.fleet }
        val hosts = remember { HostsFeature.store() }
        val tick = rememberRemoteTick(store)
        var countdownTick by remember { mutableLongStateOf(0L) }
        LaunchedEffect(store) {
            offMain {
                store.refresh()
                hosts.refresh()
            }
        }
        LaunchedEffect(Unit) {
            while (true) {
                delay(1000)
                countdownTick++
            }
        }
        val summary = store.summary
        if (summary != null && !synced) {
            targetOn = summary.fleet.enabled
            desired = maxOf(1, summary.fleet.desired)
            synced = true
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { store.refreshAsync() }, color = c.textPrimary)
        }) {
            tick.let { }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                item { ReachLine(DesktopRemoteFeature.hub.reach) }
                if (store.unsupported) item { OutdatedNote() }
                if (summary == null) {
                    if (store.loading) item { CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) }
                    if (store.error.isNotEmpty() && !store.unsupported) item { Text(store.error, style = Fm.text.bodySmall.copy(color = c.danger)) }
                } else {
                    if (summary.provision.isVisible) {
                        item {
                            FmCard(Modifier.fillMaxWidth(), onTap = { navigator.push(ProvisionScreen()) }) {
                                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                    Text(summary.provision.headline, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                                    FmLinearProgressIndicator(progress = { summary.provision.progress.toFloat() })
                                    if (summary.provision.detail.isNotEmpty()) Text(summary.provision.detail, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                                }
                            }
                        }
                    }
                    costs(summary)
                    target(summary, store, scope, countdownTick)
                    operations(summary, store, scope, navigator)
                    item { FmSectionHeader("Mirror client") }
                    item {
                        Text(
                            listOf(summary.mirrorClientStatus, summary.mirrorClientHealth).filter { it.isNotEmpty() }.joinToString(" · ").ifEmpty { "Not running" },
                            style = Fm.text.bodySmall.copy(color = c.textPrimary),
                        )
                    }
                    if (summary.pushing > 0 || summary.pushPending > 0) {
                        item { Text("${summary.pushing} pushing · ${summary.pushPending} waiting", style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                    }
                    val rows = hosts.snapshot?.sorted.orEmpty()
                    if (rows.isNotEmpty()) {
                        item { FmSectionHeader("Per host", count = rows.size) }
                        items(rows, key = { "h-" + it.id }) { host ->
                            FmListRow(
                                host.name,
                                subtitle = host.planLine.ifEmpty { null },
                                leading = { Dot(healthColor(host.health)) },
                                onTap = { navigator.push(HostOperationsScreen(host.name)) },
                                minHeight = 48.dp,
                            )
                        }
                    }
                    if (summary.gateways.isNotEmpty()) {
                        item { FmSectionHeader("Unreachable mirror gateways", count = summary.gatewaysDown) }
                        items(summary.gateways, key = { "g-" + it.host }) { gateway ->
                            Column {
                                Text(gateway.host, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                                Text(gateway.line, style = Fm.text.bodySmall.copy(color = if (gateway.retired) c.textSecondary else c.warning))
                            }
                        }
                    }
                    if (summary.lastResult.isNotEmpty()) {
                        item { FmSectionHeader("Desktop's last result") }
                        item { SelectionContainer { Text(summary.lastResult, style = Fm.text.bodySmall.copy(color = c.textPrimary)) } }
                    }
                    if (store.outcomes.isNotEmpty()) {
                        item { FmSectionHeader("From this phone") }
                        items(store.outcomes, key = { it.id }) { RemoteOutcomeCard(it) }
                    }
                }
                notices()
            }
        }
        PendingDialog(store)
    }

    private fun LazyListScope.costs(summary: FleetSummary) {
        val costs = summary.costs
        item { FmSectionHeader("Costs") }
        item {
            FmCard(Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                    Row {
                        Column(Modifier.weight(1f)) {
                            Text(RemoteFormat.money(costs.monthlyCost), style = Fm.text.titleLarge.copy(color = Fm.colors.textPrimary))
                            Text("per month · ${RemoteFormat.money(costs.yearlyCost)} a year", style = Fm.text.labelSmall.copy(color = Fm.colors.textSecondary))
                        }
                        Column(horizontalAlignment = Alignment.End) {
                            Text("${costs.vultr}", style = Fm.text.titleLarge.copy(color = Fm.colors.textPrimary))
                            Text("Vultr of ${costs.hosts} hosts", style = Fm.text.labelSmall.copy(color = Fm.colors.textSecondary))
                        }
                    }
                    for (row in costs.byRegion + costs.byPlan) {
                        Row {
                            Text(if (row in costs.byRegion) row.key.uppercase() else row.key, style = Fm.text.labelSmall.copy(color = Fm.colors.textSecondary), modifier = Modifier.weight(1f))
                            Text("${row.count} · ${RemoteFormat.money(row.monthlyCost)}", style = Fm.text.labelSmall.copy(color = Fm.colors.textPrimary))
                        }
                    }
                    Text("From the plan prices the desktop saved when it created each server.", style = Fm.text.labelSmall.copy(color = Fm.colors.textTertiary))
                }
            }
        }
    }

    private fun LazyListScope.target(summary: FleetSummary, store: FleetStore, scope: CoroutineScope, tick: Long) {
        val fleet = summary.fleet
        item { FmSectionHeader("Fleet target") }
        item {
            tick.let { }
            val countdown = store.targetCountdown()
            val line = if (fleet.available && fleet.enabled && !fleet.reconciling && countdown >= 0) {
                "Keeps ${fleet.desired} healthy mirror${if (fleet.desired == 1) "" else "s"} · next check in ${RemoteFormat.duration(countdown)}"
            } else {
                fleet.line
            }
            Text(line, style = Fm.text.bodySmall.copy(color = Fm.colors.textPrimary))
        }
        if (fleet.available) {
            item {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Keep a fleet size", style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
                    FmSwitch(checked = targetOn, onCheckedChange = { targetOn = it })
                }
            }
            item {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Healthy mirrors: $desired", style = Fm.text.bodyMedium.copy(color = if (targetOn) Fm.colors.textPrimary else Fm.colors.textDisabled), modifier = Modifier.weight(1f))
                    FmIconButton("remove", "Fewer", onPressed = if (targetOn && desired > 1) ({ desired-- }) else null, busy = false)
                    FmIconButton("add", "More", onPressed = if (targetOn && desired < 12) ({ desired++ }) else null, busy = false)
                }
            }
            item {
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    FmToneButton(
                        "Apply",
                        onPressed = if (preparing || (targetOn == fleet.enabled && desired == fleet.desired)) null else ({ prepare(scope, store, FleetAction.Target(targetOn, desired)) }),
                        busy = preparing,
                    )
                    FmToneButton(
                        "Check the fleet now",
                        onPressed = if (preparing || !fleet.enabled || fleet.reconciling) null else ({ prepare(scope, store, FleetAction.Reconcile) }),
                    )
                }
            }
        }
        if (fleet.status.isNotEmpty()) item { Text(fleet.status, style = Fm.text.labelSmall.copy(color = Fm.colors.textSecondary)) }
    }

    private fun LazyListScope.operations(summary: FleetSummary, store: FleetStore, scope: CoroutineScope, navigator: Navigator) {
        item { FmSectionHeader("Operations") }
        item {
            FmListRow(
                "Deploy to all hosts",
                leading = { FmIcon("system_update_alt", size = 20.dp, tint = Fm.colors.textSecondary) },
                onTap = if (preparing || summary.busy) null else ({ deployOpen = !deployOpen }),
                minHeight = 48.dp,
            )
        }
        if (deployOpen) {
            items(FleetDeployMode.entries, key = { "d-" + it.wire }) { mode ->
                Row {
                    Spacer(Modifier.width(FmSpace.x5))
                    FmToneButton(mode.title, tone = if (mode == FleetDeployMode.Reinstall) FmTone.Danger else FmTone.Neutral, onPressed = {
                        deployOpen = false
                        prepare(scope, store, FleetAction.DeployAll(mode))
                    })
                }
            }
        }
        if (summary.provision.resumable) {
            item { LinkRow("Resume the paused deployment", "play_circle") { prepare(scope, store, FleetAction.Resume) } }
        }
        item { LinkRow("Mirror push a repository", "publish") { navigator.push(MirrorPushScreen()) } }
        item { LinkRow("Add an SSH host", "add_circle_outline") { navigator.push(AddSshHostScreen { banner = it }) } }
        item {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmTextField(
                    value = linkCode,
                    onValueChange = { linkCode = it.filter(Char::isDigit).take(6) },
                    hint = "Link code",
                    modifier = Modifier.weight(1f),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
                    textStyle = Fm.text.bodyLarge.copy(fontFamily = FmType.mono),
                )
                Spacer(Modifier.width(FmSpace.x2))
                FmToneButton("Link node", onPressed = if (preparing || !FleetInput.validLinkCode(linkCode)) null else ({ prepare(scope, store, FleetAction.LinkCode(linkCode)) }), busy = preparing)
            }
        }
        item {
            Text(
                "A node being installed shows a six-digit link code; entering it here registers the node under your account.",
                style = Fm.text.labelSmall.copy(color = Fm.colors.textTertiary),
            )
        }
    }
}

internal class ProvisionScreen : FleetActionScreen() {
    override val title: String get() = "Deployment"
    private var status by mutableStateOf<ProvisionStatus?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.fleet }
        val tick = rememberRemoteTick(store)
        suspend fun load() {
            try {
                status = offMain { store.provision() }
                error = ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = describeRemote(failure)
            }
        }
        LaunchedEffect(tick) { load() }
        DetailScaffold(status?.node?.ifEmpty { null } ?: title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } }, color = c.textPrimary)
        }) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                val current = status
                if (current != null) {
                    item {
                        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                            Text(current.headline, style = Fm.text.titleMedium.copy(color = c.textPrimary))
                            FmLinearProgressIndicator(progress = { current.progress.toFloat() })
                            if (current.message.isNotEmpty()) Text(current.message, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                            if (current.gap.isNotEmpty()) Text(current.gap, style = Fm.text.bodySmall.copy(color = c.warning))
                            if (current.attempts > 1) Text("Attempt ${current.attempts}", style = Fm.text.labelSmall.copy(color = c.textTertiary))
                        }
                    }
                    if (current.stages.isNotEmpty()) {
                        item { FmSectionHeader("Stages") }
                        items(current.stages, key = { it.number }) { stage ->
                            val (icon, tint) = when (stage.state) {
                                ProvisionStageState.Done -> "check_circle_outline" to c.success
                                ProvisionStageState.Active -> "pending" to c.accent
                                ProvisionStageState.Failed -> "error_outline" to c.danger
                                ProvisionStageState.Pending -> "radio_button_unchecked" to c.textDisabled
                            }
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                FmIcon(icon, size = 18.dp, tint = tint)
                                Spacer(Modifier.width(FmSpace.x2))
                                Column(Modifier.weight(1f)) {
                                    Text("${stage.number}. ${stage.name}", style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                                    if (stage.detail.isNotEmpty()) Text(stage.detail, style = Fm.text.labelSmall.copy(color = c.textSecondary))
                                }
                            }
                        }
                    }
                    if (current.resumable) {
                        item { FmToneButton("Resume deployment", icon = "play_circle", tone = FmTone.Accent, onPressed = if (preparing) null else ({ prepare(scope, store, FleetAction.Resume) })) }
                    }
                    if (current.log.isNotEmpty()) {
                        item { FmSectionHeader("Deployment log") }
                        items(current.log.takeLast(200)) { line ->
                            Text(line, style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textSecondary))
                        }
                    }
                } else if (error.isEmpty()) {
                    item { CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) }
                }
                notices()
            }
        }
        PendingDialog(store)
    }
}

internal class AddSshHostScreen(private val onAdded: (String) -> Unit = {}) : FleetActionScreen() {
    override val title: String get() = "Add host"
    private var name by mutableStateOf("")
    private var address by mutableStateOf("")
    private var user by mutableStateOf("root")

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.fleet }
        val valid = FleetInput.validHostName(name) && FleetInput.validAddress(address) && FleetInput.validUser(user)
        DetailScaffold(title, navigator) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                item {
                    FmTextField(
                        value = name, onValueChange = { name = it }, hint = "Name",
                        errorText = if (name.isNotEmpty() && !FleetInput.validHostName(name)) "Letters, digits, dots, dashes and underscores." else null,
                        keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
                    )
                }
                item {
                    FmTextField(
                        value = address, onValueChange = { address = it }, hint = "IP address or host name",
                        errorText = if (address.isNotEmpty() && !FleetInput.validAddress(address)) "Enter an IP address or a host name." else null,
                        keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false, keyboardType = KeyboardType.Uri),
                    )
                }
                item {
                    FmTextField(
                        value = user, onValueChange = { user = it }, hint = "SSH user",
                        errorText = if (user.isNotEmpty() && !FleetInput.validUser(user)) "Enter a valid SSH user name." else null,
                        keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
                    )
                }
                item {
                    Text(
                        "The desktop connects with its own fleet SSH key. If the host asks for a password the first time, the desktop shows the prompt there.",
                        style = Fm.text.labelSmall.copy(color = c.textTertiary),
                    )
                }
                item { FmToneButton("Next", tone = FmTone.Accent, onPressed = if (!valid || preparing) null else ({ prepare(scope, store, FleetAction.Add(name, address, user)) }), busy = preparing) }
                notices()
            }
        }
        pending?.let { item ->
            RemoteConfirmDialog(
                item.title,
                item.confirmation,
                perform = { confirmation, typed -> store.run(confirmation, typed) },
                onDone = {
                    onAdded(it)
                    navigator.pop()
                },
            ) { pending = null }
        }
    }
}

internal class MirrorPushScreen : FleetActionScreen() {
    override val title: String get() = "Mirror push"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.fleet }
        val tick = rememberRemoteTick(store)
        LaunchedEffect(store) { offMain { store.loadRepos() } }
        DetailScaffold(title, navigator) {
            tick.let { }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                if (store.repos.isEmpty()) item { Text(if (store.error.isEmpty()) "Loading the desktop's repositories…" else store.error, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                items(store.repos, key = { it.repo }) { repo ->
                    FmListRow(
                        repo.repo,
                        subtitle = listOf(if (repo.isPrivate) "private" else "", if (repo.published) "published" else "", if (repo.pushing) "pushing" else "").filter { it.isNotEmpty() }.joinToString(" · ").ifEmpty { null },
                        onTap = if (preparing || repo.pushing) null else ({ prepare(scope, store, FleetAction.MirrorPush(repo.repo)) }),
                        minHeight = 48.dp,
                    )
                }
                notices()
            }
        }
        PendingDialog(store)
    }
}

internal class HostOperationsScreen(private val hostName: String) : FleetActionScreen() {
    override val title: String get() = hostName

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.fleet }
        val hosts = remember { HostsFeature.store() }
        val tick = rememberRemoteTick(store)
        LaunchedEffect(store) { offMain { if (store.summary == null) store.refresh() } }
        val host = hosts.host(hostName)
        DetailScaffold(title, navigator) {
            tick.let { }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                if (host != null) {
                    item {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Dot(healthColor(host.health))
                            Spacer(Modifier.width(FmSpace.x2))
                            Text(host.healthLine, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                        }
                    }
                    if (host.address.isNotEmpty()) item { Text(host.address + if (host.planLine.isEmpty()) "" else " · " + host.planLine, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                }
                item { FmSectionHeader("Operations") }
                item { LinkRow("Disk usage", "storage") { navigator.push(DiskUsageScreen(hostName, "/")) } }
                item { OperationRow(store, FleetAction.AgentClis(hostName), "terminal", scope) }
                item { OperationRow(store, FleetAction.Actions(hostName, true), "play_circle", scope) }
                item { OperationRow(store, FleetAction.Actions(hostName, false), "pause_circle", scope) }
                item { Text("Actions variables and provider sign-ins stay on the desktop; set them there.", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                item { FmSectionHeader("Remove") }
                item { OperationRow(store, FleetAction.Uninstall(hostName), "delete_sweep", scope) }
                item { OperationRow(store, FleetAction.Forget(hostName), "remove_circle_outline", scope) }
                item { Text("To destroy the server itself, use Destroy on the host's page.", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                val outcomes = store.outcomes.filter { it.subject.lowercase().startsWith(hostName.lowercase()) }
                if (outcomes.isNotEmpty()) {
                    item { FmSectionHeader("From this phone") }
                    items(outcomes, key = { it.id }) { RemoteOutcomeCard(it) }
                }
                notices()
            }
        }
        PendingDialog(store)
    }

    @Composable
    private fun OperationRow(store: FleetStore, action: FleetAction, icon: String, scope: CoroutineScope) {
        val c = Fm.colors
        val running = store.isRunning(action.wireName, hostName.lowercase()) || store.isRunning(action.wireName, hostName)
        val tint = if (action.isDestructive) c.danger else c.textPrimary
        FmListRow(
            action.title,
            titleColor = if (preparing || running) c.textDisabled else tint,
            leading = { FmIcon(icon, size = 20.dp, tint = if (action.isDestructive) c.danger else c.textSecondary) },
            trailing = if (running) ({ CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp) }) else null,
            onTap = if (preparing || running) null else ({ prepare(scope, store, action) }),
            showChevron = false,
            minHeight = 48.dp,
        )
    }
}

internal class DiskUsageScreen(private val host: String, private val path: String) : Screen() {
    override val title: String get() = path
    private var usage by mutableStateOf<DiskUsage?>(null)
    private var error by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.fleet }
        suspend fun load() {
            try {
                usage = offMain { store.diskUsage(host, path) }
                error = ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = describeRemote(failure)
            }
        }
        LaunchedEffect(host, path) { load() }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } }, color = c.textPrimary)
        }) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                val current = usage
                if (current != null) {
                    item { Text("${RemoteFormat.bytes(current.total)} on $host", style = Fm.text.titleSmall.copy(color = c.textPrimary)) }
                    current.parentPath?.let { parent ->
                        item { LinkRow("Up to $parent", "arrow_upward") { navigator.push(DiskUsageScreen(host, parent)) } }
                    }
                    items(current.sortedChildren, key = { it.path }) { entry ->
                        FmCard(Modifier.fillMaxWidth(), onTap = { navigator.push(DiskUsageScreen(host, entry.path)) }) {
                            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                Row {
                                    Text(entry.name, style = Fm.text.bodyMedium.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                                    Text(RemoteFormat.bytes(entry.bytes), style = Fm.text.labelMedium.copy(color = c.textSecondary))
                                }
                                ShareBar(current.share(entry))
                            }
                        }
                    }
                } else if (error.isEmpty()) {
                    item {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                            Spacer(Modifier.width(FmSpace.x2))
                            Text("Measuring on the host… this can take a minute.", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        }
                    }
                }
                if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
    }
}

internal class NodeScreen(private val name: String) : FleetActionScreen() {
    override val title: String get() = name

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.fleet }
        val hosts = remember { HostsFeature.store() }
        val tick = rememberRemoteTick(store)
        LaunchedEffect(store) { offMain { store.refresh() } }
        val summary = store.summary
        DetailScaffold(title, navigator) {
            tick.let { }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                if (hosts.host(name) != null) {
                    item { LinkRow("Saved host on the desktop", "dns_outlined") { navigator.push(HostsFeature.hostScreen(name)) } }
                }
                if (summary == null) {
                    item { if (store.error.isEmpty()) CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) else Text(store.error, style = Fm.text.bodySmall.copy(color = c.danger)) }
                } else if (summary.nodeDeleteInProgress) {
                    item {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                            Spacer(Modifier.width(FmSpace.x2))
                            Text("Deleting ${summary.nodeDeleteTarget}…", style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                        }
                    }
                } else if (summary.admin) {
                    item {
                        FmToneButton("Delete node completely", icon = "delete_outline", tone = FmTone.Danger, onPressed = if (preparing) null else ({ prepare(scope, store, FleetAction.DeleteNode(name)) }))
                    }
                    item {
                        Text(
                            "Destroys its Vultr server if it has one, removes its DNS and forgets it on the desktop. The desktop asks you to type the name.",
                            style = Fm.text.labelSmall.copy(color = c.textTertiary),
                        )
                    }
                } else {
                    item { FmStatusBadge("Admins only", c.textSecondary) }
                    item { Text("Only a platform admin can delete a node.", style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                }
                notices()
            }
        }
        PendingDialog(store)
    }
}
