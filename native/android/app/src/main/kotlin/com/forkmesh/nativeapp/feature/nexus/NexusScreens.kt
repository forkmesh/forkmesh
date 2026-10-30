package com.forkmesh.nativeapp.feature.nexus

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.nexus.AuthorizationAttestation
import com.forkmesh.nativeapp.nexus.CertificateSummary
import com.forkmesh.nativeapp.nexus.ConnectivityResult
import com.forkmesh.nativeapp.nexus.Device
import com.forkmesh.nativeapp.nexus.DnsLookup
import com.forkmesh.nativeapp.nexus.DnsRecordType
import com.forkmesh.nativeapp.nexus.EvidenceLevel
import com.forkmesh.nativeapp.nexus.HttpInspection
import com.forkmesh.nativeapp.nexus.PortList
import com.forkmesh.nativeapp.nexus.PortPreset
import com.forkmesh.nativeapp.nexus.PortResult
import com.forkmesh.nativeapp.nexus.PortState
import com.forkmesh.nativeapp.nexus.ScanSession
import com.forkmesh.nativeapp.nexus.SecurityInsights
import com.forkmesh.nativeapp.nexus.SecurityObservation
import com.forkmesh.nativeapp.nexus.SecuritySeverity
import com.forkmesh.nativeapp.nexus.SessionStatus
import com.forkmesh.nativeapp.nexus.TrustOutcome
import com.forkmesh.nativeapp.ui.kit.FmActionMenu
import com.forkmesh.nativeapp.ui.kit.FmActionMenuButton
import com.forkmesh.nativeapp.ui.kit.FmBottomBar
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmGroupHeader
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessageTools
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.launch
import java.text.DateFormat
import java.util.Date
import java.util.Locale

object NexusFeature {
    private const val OWNER = "existing"

    fun install() {
        NavigationFeature.register(Route.Kind.Nexus, OWNER) { NexusScreen() }
    }
}

private enum class NexusTab(val title: String, val label: String, val icon: String) {
    Overview("NEXUS", "Overview", "monitor_heart"),
    Discover("Discover", "Discover", "cell_tower"),
    Scan("Scan", "Scan", "my_location"),
    Inspect("Inspect", "Inspect", "search"),
    History("History", "History", "history"),
    Settings("Settings", "Settings", "settings"),
}

class NexusScreen : Screen() {
    override val title: String get() = "NEXUS"
    override val hidesShellChrome: Boolean get() = true

    private var tab by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val state = remember { NexusState.shared(context) }
        val current = NexusTab.entries[tab]
        var menu by remember { mutableStateOf(false) }
        var clearing by remember { mutableStateOf(false) }
        Dialogs(state)
        if (clearing) {
            FmConfirmDialog(
                title = "Clear history?",
                body = "Every saved scan, discovery and device on this phone is removed.",
                confirmLabel = "Clear",
                destructive = true,
                onConfirm = { clearing = false; state.clearHistory() },
                onDismissRequest = { clearing = false },
            )
        }
        DetailScaffold(current.title, navigator, pluginMenu = current == NexusTab.Overview, actions = {
            when (current) {
                NexusTab.Discover -> FmIconButton("refresh", "Discover", if (state.running) null else ({ state.discover() }))
                NexusTab.History -> Box {
                    FmIconButton("more_horiz", "More", { menu = true })
                    FmActionMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        FmActionMenuButton("Clear history", { menu = false; clearing = true }, destructive = true, enabled = state.scans.isNotEmpty() || state.discoveries.isNotEmpty())
                    }
                }
                else -> Unit
            }
        }) {
            Column(Modifier.fillMaxSize()) {
                if (state.running) ActivityStrip(state)
                Box(Modifier.weight(1f).fillMaxWidth()) {
                    when (current) {
                        NexusTab.Overview -> OverviewTab(state, navigator) { tab = it.ordinal }
                        NexusTab.Discover -> DiscoverTab(state, navigator)
                        NexusTab.Scan -> ScanTab(state, navigator)
                        NexusTab.Inspect -> InspectTab(state)
                        NexusTab.History -> HistoryTab(state, navigator)
                        NexusTab.Settings -> SettingsTab(state)
                    }
                }
                TabBar(current) { tab = it.ordinal }
            }
        }
    }
}

@Composable
private fun Dialogs(state: NexusState) {
    state.error?.let { message ->
        FmConfirmDialog(title = "NEXUS", body = message, hideCancel = true, onConfirm = { state.error = null }, onDismissRequest = { state.error = null })
    }
    state.attestationPrompt?.let { prompt ->
        FmConfirmDialog(
            title = "Authorized testing",
            body = "$prompt\n\n“${AuthorizationAttestation.DEFAULT_STATEMENT}”",
            confirmLabel = "I confirm",
            onConfirm = { state.confirmAttestation() },
            onDismissRequest = { state.dismissAttestation() },
        )
    }
}

@Composable
private fun TabBar(current: NexusTab, onSelect: (NexusTab) -> Unit) {
    val c = Fm.colors
    FmBottomBar(leading = {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            for (item in NexusTab.entries) {
                val selected = item == current
                val tint = if (selected) c.accent else c.textTertiary
                Column(
                    Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .clickable(role = Role.Tab) { onSelect(item) },
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                ) {
                    FmIcon(item.icon, size = 20.dp, tint = tint, outlined = !selected)
                    Text(item.label, maxLines = 1, style = Fm.text.labelSmall.copy(color = tint, fontSize = 10.sp, letterSpacing = 0.sp))
                }
            }
        }
    }, height = 56.dp)
}

@Composable
private fun ActivityStrip(state: NexusState) {
    Row(
        Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(start = FmSpace.x4, end = FmSpace.x2, top = FmSpace.x1, bottom = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(state.activity, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary), maxLines = 1, overflow = TextOverflow.Ellipsis)
            Spacer(Modifier.height(FmSpace.x1))
            FmLinearProgressIndicator(progress = { state.progress.toFloat() })
        }
        Spacer(Modifier.width(FmSpace.x2))
        FmToneButton("Stop", { state.stop() }, tone = FmTone.Danger)
    }
}

@Composable
private fun NexusList(content: LazyListScope.() -> Unit) {
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x5)) { content() }
}

private fun LazyListScope.section(title: String, footer: String? = null, content: @Composable ColumnScope.() -> Unit) {
    item(key = "section:$title") {
        Column {
            FmGroupHeader(title)
            Column(
                Modifier
                    .padding(horizontal = FmSpace.x4)
                    .fillMaxWidth()
                    .background(Fm.colors.bgRaised, RoundedCornerShape(FmRadius.lg)),
                content = content,
            )
            if (footer != null) {
                Text(footer, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary), modifier = Modifier.padding(horizontal = FmSpace.x5, vertical = FmSpace.x2))
            }
        }
    }
}

@Composable
private fun LabeledRow(label: String, value: String, mono: Boolean = false) {
    Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x3), verticalAlignment = Alignment.Top) {
        Text(label, style = Fm.text.bodyMedium, modifier = Modifier.weight(0.42f))
        Spacer(Modifier.width(FmSpace.x3))
        SelectionContainer(Modifier.weight(0.58f)) {
            Text(
                value,
                textAlign = TextAlign.End,
                modifier = Modifier.fillMaxWidth(),
                style = if (mono) Fm.mono(color = Fm.colors.textSecondary) else Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
            )
        }
    }
}

@Composable
private fun Divider() = FmRowDivider(indent = FmSpace.x4)

@Composable
private fun NoteRow(icon: String, text: String) {
    Row(Modifier.fillMaxWidth().padding(FmSpace.x4), verticalAlignment = Alignment.Top) {
        FmIcon(icon, size = 18.dp, tint = Fm.colors.textTertiary, outlined = true)
        Spacer(Modifier.width(FmSpace.x3))
        Text(text, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
    }
}

@Composable
private fun EmptyRow(text: String) {
    Text(text, style = Fm.text.bodyMedium.copy(color = Fm.colors.textTertiary), modifier = Modifier.padding(FmSpace.x4))
}

@Composable
private fun Metric(value: String, label: String, icon: String, modifier: Modifier) {
    Column(modifier.padding(FmSpace.x3)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon(icon, size = 14.dp, tint = Fm.colors.textTertiary, outlined = true)
            Spacer(Modifier.width(FmSpace.x1))
            Text(label, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary), maxLines = 1)
        }
        Spacer(Modifier.height(FmSpace.x1))
        Text(value, style = Fm.text.titleLarge.copy(fontFeatureSettings = FmType.tabular))
    }
}

@Composable
private fun FindingRow(finding: SecurityObservation) {
    val warning = finding.severity == SecuritySeverity.Warning
    val tint = when (finding.severity) {
        SecuritySeverity.Warning -> Fm.colors.warning
        SecuritySeverity.Attention -> Fm.colors.accent
        SecuritySeverity.Informational -> Fm.colors.textTertiary
    }
    Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x3), verticalAlignment = Alignment.Top) {
        FmIcon(if (warning) "warning" else "info", size = 18.dp, tint = tint, outlined = true)
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(finding.title, style = Fm.text.bodyMedium.copy(color = if (warning) Fm.colors.warning else Fm.colors.textPrimary))
            Text(finding.detail, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
        }
    }
}

@Composable
private fun DeviceRow(device: Device, onTap: () -> Unit) {
    FmListRow(
        title = device.title,
        subtitle = device.primaryAddress?.toString() ?: device.services.firstOrNull()?.type ?: "Network service",
        leading = { FmIconTile(device.kind.icon, tint = Fm.colors.accent) },
        trailing = if (device.openPorts.isEmpty()) null else ({ Text("${device.openPorts.size}", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary)) }),
        onTap = onTap,
    )
}

@Composable
private fun ScanRow(session: ScanSession, onTap: () -> Unit) {
    FmListRow(
        title = session.target,
        subtitle = "${session.status.title} · ${session.counts.open} open · ${session.ports.count} ports · ${shortDate(session.startedAt)}",
        leading = { FmIconTile("my_location", tint = if (session.counts.open > 0) Fm.colors.accent else null) },
        onTap = onTap,
    )
}

private fun shortDate(millis: Long): String = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(millis))

private fun ms(value: Double?): String = value?.let { String.format(Locale.ROOT, "%.0f ms", it) } ?: "—"

@Composable
private fun OverviewTab(state: NexusState, navigator: Navigator, select: (NexusTab) -> Unit) {
    val latest = state.scans.firstOrNull()
    val findings = latest?.let { SecurityInsights.portChanges(it, state.scans.getOrNull(1)) }.orEmpty()
    NexusList {
        item(key = "metrics") {
            Row(
                Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3).fillMaxWidth().background(Fm.colors.bgRaised, RoundedCornerShape(FmRadius.lg)),
            ) {
                Metric("${state.devices.size}", "Devices", "lan", Modifier.weight(1f))
                Metric("${latest?.counts?.open ?: 0}", "Open ports", "lock_open", Modifier.weight(1f))
                Metric("${state.scans.size}", "Scans", "my_location", Modifier.weight(1f))
            }
        }
        if (!state.running) {
            item(key = "activity") { NoteRow("bolt", state.activity) }
        }
        section("Notable observations") {
            if (findings.isEmpty() && state.changes.isEmpty()) {
                Column(Modifier.fillMaxWidth().padding(FmSpace.x5), horizontalAlignment = Alignment.CenterHorizontally) {
                    FmIcon("verified_user", size = 32.dp, tint = Fm.colors.textTertiary, outlined = true)
                    Spacer(Modifier.height(FmSpace.x2))
                    Text("No observations yet", style = Fm.text.titleSmall)
                    Text("Run a discovery or scan to build a network picture.", textAlign = TextAlign.Center, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
                }
            }
            findings.forEachIndexed { index, finding ->
                if (index > 0) Divider()
                FindingRow(finding)
            }
            state.changes.take(8).forEachIndexed { index, change ->
                if (index > 0 || findings.isNotEmpty()) Divider()
                FmListRow(title = change.kind.title, subtitle = change.summary, leading = { FmIconTile(change.kind.icon) }, showChevron = false)
            }
        }
        section("Recent devices") {
            if (state.devices.isEmpty()) EmptyRow("No devices discovered yet.")
            state.devices.take(5).forEachIndexed { index, device ->
                if (index > 0) FmRowDivider()
                DeviceRow(device) { navigator.push(NexusDeviceScreen(device)) }
            }
        }
        item(key = "shortcuts") {
            Row(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmToneButton("Discover", { select(NexusTab.Discover); state.discover() }, icon = "cell_tower", tone = FmTone.Accent)
                FmToneButton("Scan", { select(NexusTab.Scan) }, icon = "my_location")
            }
        }
    }
}

@Composable
private fun DiscoverTab(state: NexusState, navigator: Navigator) {
    var query by remember { mutableStateOf("") }
    val filtered = if (query.isBlank()) state.devices else state.devices.filter { query.trim().lowercase() in it.searchText }
    Column(Modifier.fillMaxSize()) {
        FmTextField(
            query, { query = it },
            Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            hint = "Search devices, services, or addresses",
            leading = { FmIcon("search", size = 18.dp, tint = Fm.colors.textTertiary) },
            keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
        )
        if (filtered.isEmpty()) {
            FmEmptyState("cell_tower", "No devices", message = "Browse your authorized local network for advertised services (mDNS). Tap the refresh button to start.")
        } else {
            NexusList {
                item(key = "devices") {
                    Column(Modifier.padding(horizontal = FmSpace.x4).background(Fm.colors.bgRaised, RoundedCornerShape(FmRadius.lg))) {
                        filtered.forEachIndexed { index, device ->
                            if (index > 0) FmRowDivider()
                            DeviceRow(device) { navigator.push(NexusDeviceScreen(device)) }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ScanTab(state: NexusState, navigator: Navigator) {
    var target by remember { mutableStateOf("") }
    var ports by remember { mutableStateOf(PortPreset.Quick.ports.toString()) }
    var problem by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(Unit) { if (target.isBlank()) state.localNetwork()?.let { target = it } }
    problem?.let { FmConfirmDialog(title = "Scan setup", body = it, hideCancel = true, onConfirm = { problem = null }, onDismissRequest = { problem = null }) }
    NexusList {
        section("Target") {
            FmTextField(
                target, { target = it }, Modifier.padding(FmSpace.x3), hint = "Host, IP, range, or CIDR",
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false, keyboardType = KeyboardType.Uri),
            )
            Divider()
            FmListRow(
                title = "Use this network",
                leading = { FmIcon("wifi", size = 20.dp, tint = Fm.colors.accent, outlined = true) },
                titleColor = Fm.colors.accent,
                showChevron = false,
                minHeight = 44.dp,
                onTap = { state.localNetwork()?.let { target = it } ?: run { problem = "No local network found. Join a Wi-Fi network, or type a range." } },
            )
        }
        section("Ports") {
            FmTextField(ports, { ports = it }, Modifier.padding(FmSpace.x3), hint = "Ports", keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, autoCorrectEnabled = false))
            Row(Modifier.horizontalScroll(rememberScrollState()).padding(start = FmSpace.x3, end = FmSpace.x3, bottom = FmSpace.x3), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                for (preset in PortPreset.scanChoices) {
                    val chosen = preset.ports.toString() == ports
                    FmToneButton(preset.title, { ports = preset.ports.toString() }, icon = preset.icon, tone = if (chosen) FmTone.Accent else FmTone.Neutral)
                }
            }
        }
        item(key = "start") {
            Box(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x3)) {
                FmFilledButton(
                    "Start TCP connect scan",
                    if (state.running || target.isBlank()) null else ({
                        runCatching { PortList.parse(ports) }
                            .onSuccess { state.scan(target.trim(), it) }
                            .onFailure { problem = it.message }
                    }),
                    Modifier.fillMaxWidth(),
                    icon = "play_arrow",
                )
            }
        }
        section("Recent scans") {
            if (state.scans.isEmpty()) EmptyRow("No scans yet.")
            state.scans.take(10).forEachIndexed { index, session ->
                if (index > 0) FmRowDivider()
                ScanRow(session) { navigator.push(NexusScanScreen(session)) }
            }
        }
    }
}

@Composable
private fun InspectTab(state: NexusState) {
    val scope = rememberCoroutineScope()
    var dnsName by remember { mutableStateOf("") }
    var dnsType by remember { mutableStateOf(DnsRecordType.A) }
    var typeMenu by remember { mutableStateOf(false) }
    var dns by remember { mutableStateOf<DnsLookup?>(null) }
    var url by remember { mutableStateOf("https://") }
    var web by remember { mutableStateOf<HttpInspection?>(null) }
    var host by remember { mutableStateOf("") }
    var port by remember { mutableStateOf("443") }
    var reach by remember { mutableStateOf<ConnectivityResult?>(null) }
    var busy by remember { mutableStateOf<String?>(null) }
    fun run(kind: String, block: suspend () -> Unit) {
        busy = kind
        scope.launch {
            try {
                block()
            } catch (failure: Exception) {
                state.error = failure.message ?: "The inspection failed."
            } finally {
                busy = null
            }
        }
    }
    val plain = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false, keyboardType = KeyboardType.Uri)
    NexusList {
        section("DNS explorer") {
            Row(Modifier.padding(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
                FmTextField(dnsName, { dnsName = it }, Modifier.weight(1f), hint = "Hostname or IP", keyboardOptions = plain)
                Spacer(Modifier.width(FmSpace.x2))
                Box {
                    FmToneButton(dnsType.title, { typeMenu = true }, icon = "expand_more")
                    FmDropdownMenu(expanded = typeMenu, onDismissRequest = { typeMenu = false }) {
                        for (type in DnsRecordType.lookupChoices) FmDropdownMenuItem(type.title, { dnsType = type; typeMenu = false })
                    }
                }
                Spacer(Modifier.width(FmSpace.x2))
                FmToneButton("Lookup", if (dnsName.isBlank() || busy != null) null else ({ run("dns") { dns = state.lookup(dnsName, dnsType) } }), tone = FmTone.Accent)
            }
            dns?.let { result ->
                Divider()
                Text(
                    "${result.answers.size} answers · ${result.latencyMilliseconds.toInt()} ms via ${result.resolver}" + (result.error?.let { " · $it" } ?: ""),
                    style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
                    modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                )
                for (record in result.answers) {
                    Divider()
                    LabeledRow("${record.type.title} · ${record.name}", "${record.value}\nTTL ${record.ttl}s", mono = true)
                }
            }
        }
        section("HTTP and TLS") {
            Row(Modifier.padding(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
                FmTextField(url, { url = it }, Modifier.weight(1f), hint = "https://example.com", keyboardOptions = plain)
                Spacer(Modifier.width(FmSpace.x2))
                FmToneButton("Inspect", if (busy != null) null else ({ run("web") { web = state.inspect(url) } }), tone = FmTone.Accent)
            }
            web?.let { WebResult(it) }
        }
        section("Reachability") {
            Row(Modifier.padding(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
                FmTextField(host, { host = it }, Modifier.weight(1f), hint = "Host", keyboardOptions = plain)
                Spacer(Modifier.width(FmSpace.x2))
                FmTextField(port, { port = it.filter(Char::isDigit).take(5) }, Modifier.width(84.dp), hint = "Port", keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
                Spacer(Modifier.width(FmSpace.x2))
                val portNumber = port.toIntOrNull()?.takeIf { it in 1..65535 }
                FmToneButton("Check", if (host.isBlank() || portNumber == null || busy != null) null else ({ run("reach") { reach = state.reach(host, portNumber) } }), tone = FmTone.Accent)
            }
            reach?.let { result ->
                Divider()
                LabeledRow(result.endpoint.toString(), if (result.reachable) "Reachable · ${ms(result.latencyMilliseconds)}" else (result.failure?.title ?: "Not reachable"))
                if (result.resolvedAddresses.isNotEmpty()) {
                    Divider()
                    LabeledRow("Addresses", result.resolvedAddresses.joinToString("\n"), mono = true)
                }
                result.failure?.let {
                    Divider()
                    NoteRow("info", it.detail)
                }
            }
        }
        if (busy != null) item(key = "busy") { FmLinearProgressIndicator(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) }
        section("Android capability") {
            NoteRow("info", "TCP reachability, DNS and HTTPS inspection are native. ICMP ping and raw-packet traceroute are unavailable to Android apps.")
        }
    }
}

@Composable
private fun ColumnScope.WebResult(result: HttpInspection) {
    Divider()
    LabeledRow("Status", "${result.statusCode} ${result.reason}".trim())
    Divider()
    LabeledRow("Time", "${ms(result.elapsedMilliseconds)} total · ${ms(result.timings.connectMilliseconds)} connect · ${ms(result.timings.firstByteMilliseconds)} first byte")
    if (result.finalUrl != result.requestedUrl) {
        Divider()
        LabeledRow("Final URL", result.finalUrl, mono = true)
    }
    if (result.redirects.isNotEmpty()) {
        Divider()
        LabeledRow("Redirects", result.redirects.joinToString("\n"), mono = true)
    }
    result.tls?.let { tls ->
        Divider()
        LabeledRow("TLS", listOfNotNull(tls.protocolVersion, tls.cipherSuite, tls.handshakeMilliseconds?.let { "handshake ${ms(it)}" }).joinToString("\n").ifEmpty { "—" })
        Divider()
        Row(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
            FmStatusBadge(if (tls.trust is TrustOutcome.Trusted) "Trusted" else "Not trusted", if (tls.trust.isTrusted) Fm.colors.success else Fm.colors.warning)
            Spacer(Modifier.width(FmSpace.x2))
            Text(tls.trust.summary, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
        }
        tls.chain.forEachIndexed { index, certificate ->
            Divider()
            CertificateRow(index, certificate)
        }
    }
    for (finding in result.securityObservations) {
        Divider()
        FindingRow(finding)
    }
    if (result.headers.isNotEmpty()) {
        Divider()
        LabeledRow("Headers", result.headers.joinToString("\n") { "${it.name}: ${it.value}" }, mono = true)
    }
}

@Composable
private fun CertificateRow(index: Int, certificate: CertificateSummary) {
    var open by remember { mutableStateOf(index == 0) }
    Column(Modifier.fillMaxWidth().clickable { open = !open }.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon("workspace_premium", size = 18.dp, tint = Fm.colors.textTertiary, outlined = true)
            Spacer(Modifier.width(FmSpace.x2))
            Text(certificate.displayName, style = Fm.text.bodyMedium, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(if (index == 0) "Leaf" else if (certificate.isCertificateAuthority) "CA" else "#$index", style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
        }
        if (open) {
            val days = certificate.daysUntilExpiry()
            val lines = listOf(
                "Issuer" to (certificate.issuerCommonName ?: certificate.issuer),
                "Valid" to "${shortDate(certificate.notBefore)} – ${shortDate(certificate.notAfter)} (${if (days >= 0) "$days days left" else "expired"})",
                "Names" to certificate.subjectAlternativeNames.take(8).joinToString(", ").ifEmpty { "—" },
                "Key" to "${certificate.publicKeyAlgorithm}${certificate.publicKeyBits?.let { " $it-bit" } ?: ""} · ${certificate.signatureAlgorithm}",
                "Serial" to certificate.serialNumber,
                "SHA-256" to certificate.sha256Fingerprint,
            )
            Spacer(Modifier.height(FmSpace.x2))
            SelectionContainer {
                Column {
                    for ((label, value) in lines) {
                        Text(label, style = Fm.text.labelSmall.copy(color = Fm.colors.textTertiary), modifier = Modifier.padding(top = FmSpace.x1))
                        Text(value, style = Fm.mono(color = Fm.colors.textSecondary))
                    }
                }
            }
        }
    }
}

@Composable
private fun HistoryTab(state: NexusState, navigator: Navigator) {
    if (state.scans.isEmpty() && state.discoveries.isEmpty()) {
        FmEmptyState("history", "No history", message = "Completed scans and discoveries appear here.")
        return
    }
    NexusList {
        section("Scans") {
            if (state.scans.isEmpty()) EmptyRow("No scans yet.")
            state.scans.forEachIndexed { index, session ->
                if (index > 0) FmRowDivider()
                ScanRow(session) { navigator.push(NexusScanScreen(session)) }
            }
        }
        section("Discoveries") {
            if (state.discoveries.isEmpty()) EmptyRow("No discoveries yet.")
            state.discoveries.forEachIndexed { index, session ->
                if (index > 0) Divider()
                LabeledRow("${session.status.title} · ${shortDate(session.startedAt)}", "${session.devices.size} devices" + (session.network?.let { "\n$it" } ?: ""))
            }
        }
    }
}

@Composable
private fun Stepper(label: String, value: String, onMinus: (() -> Unit)?, onPlus: (() -> Unit)?) {
    Row(Modifier.fillMaxWidth().padding(start = FmSpace.x4, end = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
        Text("$label $value", style = Fm.text.bodyMedium, modifier = Modifier.weight(1f))
        FmIconButton("remove", "Less", onMinus)
        FmIconButton("add", "More", onPlus)
    }
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().clickable { onChange(!checked) }.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = Fm.text.bodyMedium, modifier = Modifier.weight(1f))
        FmSwitch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun SettingsTab(state: NexusState) {
    NexusList {
        section("Scan defaults") {
            Stepper("Timeout", String.format(Locale.ROOT, "%.1f seconds", state.timeoutSeconds),
                if (state.timeoutSeconds > 0.2) ({ state.updateTimeout(state.timeoutSeconds - 0.1) }) else null,
                if (state.timeoutSeconds < 30) ({ state.updateTimeout(state.timeoutSeconds + 0.1) }) else null)
            Divider()
            Stepper("", "${state.concurrency} concurrent connections",
                if (state.concurrency > 1) ({ state.updateConcurrency(state.concurrency - 1) }) else null,
                if (state.concurrency < 128) ({ state.updateConcurrency(state.concurrency + 1) }) else null)
        }
        section(
            "Authorization",
            footer = "NEXUS always requires confirmation that you own or are authorized to test an internet target. Internet ranges remain blocked unless explicitly authorized.",
        ) {
            SwitchRow("Allow internet targets", state.allowPublicTargets, state::updateAllowPublic)
        }
        section("History", footer = "Scans, discoveries and devices are kept on this phone only. Nothing is uploaded.") {
            SwitchRow("Keep history on this phone", state.keepHistory, state::updateKeepHistory)
        }
        section("Privacy and permissions") {
            NoteRow("pan_tool", "Service discovery uses Android's network service browser (mDNS) on the Wi-Fi you are joined to. NEXUS does not read ARP tables, MAC addresses, or device vendors.")
        }
    }
}

class NexusDeviceScreen(private val device: Device) : Screen() {
    override val title: String get() = device.title
    override val hidesShellChrome: Boolean get() = true

    @Composable
    override fun Content(navigator: Navigator) {
        DetailScaffold(device.title, navigator, pluginMenu = false) {
            NexusList {
                section("Identity") {
                    LabeledRow("Name", device.title)
                    Divider()
                    LabeledRow("Type", device.kind.title)
                    device.identification?.let {
                        Divider()
                        LabeledRow(it.evidence.title, it.basis)
                    }
                    for (address in device.addresses) {
                        Divider()
                        LabeledRow("Address", address.toString(), mono = true)
                    }
                    for (name in device.hostnames) {
                        Divider()
                        LabeledRow("Hostname", name, mono = true)
                    }
                    Divider()
                    LabeledRow("Seen", "${shortDate(device.firstSeen)} – ${shortDate(device.lastSeen)}")
                }
                section("Discovered services") {
                    if (device.services.isEmpty()) EmptyRow("None advertised.")
                    device.services.forEachIndexed { index, service ->
                        if (index > 0) Divider()
                        LabeledRow(service.type, service.name + (service.port?.let { "\nport $it" } ?: "") + service.txt.entries.take(6).joinToString("") { "\n${it.key}=${it.value}" })
                    }
                }
                section("Open TCP ports") {
                    if (device.openPorts.isEmpty()) EmptyRow("Scan this device to list its open ports.")
                    device.openPorts.forEachIndexed { index, result ->
                        if (index > 0) Divider()
                        LabeledRow("${result.port} · ${result.serviceName}", result.state.title)
                    }
                }
            }
        }
    }
}

class NexusScanScreen(private val session: ScanSession) : Screen() {
    override val title: String get() = session.target
    override val hidesShellChrome: Boolean get() = true

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val state = remember { NexusState.shared(context) }
        var menu by remember { mutableStateOf(false) }
        DetailScaffold(session.target, navigator, pluginMenu = false, actions = {
            Box {
                FmIconButton("ios_share", "Share report", { menu = true })
                FmActionMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    FmActionMenuButton("Share as JSON", { menu = false; state.share(context, session, csv = false) })
                    FmActionMenuButton("Share as CSV", { menu = false; state.share(context, session, csv = true) })
                }
            }
        }) {
            NexusList {
                section("Summary") {
                    LabeledRow("Status", session.status.title + ((session.status as? SessionStatus.Failed)?.reason?.let { ": $it" } ?: ""))
                    Divider()
                    LabeledRow("Engine", session.engine.title)
                    Divider()
                    LabeledRow("Started", shortDate(session.startedAt))
                    session.durationSeconds?.let {
                        Divider()
                        LabeledRow("Duration", String.format(Locale.ROOT, "%.1f s", it))
                    }
                    Divider()
                    LabeledRow("Ports", session.ports.toString(), mono = true)
                    Divider()
                    LabeledRow("Results", PortState.entries.filter { session.counts[it] > 0 }.joinToString("\n") { "${session.counts[it]} ${it.title.lowercase()}" }.ifEmpty { "None" })
                    session.attestation?.let {
                        Divider()
                        LabeledRow("Authorized", it.statement)
                    }
                }
                section("Resolved hosts") {
                    session.resolvedHosts.forEachIndexed { index, host ->
                        if (index > 0) Divider()
                        LabeledRow(host.name, host.addresses.joinToString("\n"), mono = true)
                    }
                }
                val byHost = session.openResults.groupBy { it.hostKey }
                if (byHost.isEmpty()) {
                    section("Open TCP ports") { EmptyRow("No open ports were found.") }
                }
                for ((host, results) in byHost) {
                    section("Open on $host") {
                        results.forEachIndexed { index, result ->
                            if (index > 0) Divider()
                            PortRow(result)
                        }
                    }
                }
                val other = session.results.filter { it.state != PortState.Open }
                if (other.isNotEmpty()) {
                    section("Other results", footer = "A timeout cannot tell a firewall from an offline host, so NEXUS reports it as “No response”.") {
                        other.take(50).forEachIndexed { index, result ->
                            if (index > 0) Divider()
                            LabeledRow("${result.hostKey}:${result.port}", result.state.title)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PortRow(result: PortResult) {
    var open by remember { mutableStateOf(false) }
    val service = result.service
    Column(Modifier.fillMaxWidth().clickable(enabled = service?.banner != null) { open = !open }.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("${result.port}", style = Fm.mono(weight = FmType.medium), modifier = Modifier.width(64.dp))
            Column(Modifier.weight(1f)) {
                Text(listOfNotNull(result.serviceName, service?.productDescription).joinToString(" · "), style = Fm.text.bodyMedium)
                Text(ms(result.latencyMilliseconds), style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
            }
            if (service != null) {
                FmStatusBadge(service.evidence.title, if (service.evidence == EvidenceLevel.Confirmed) Fm.colors.success else Fm.colors.textTertiary)
            }
        }
        val banner = service?.banner
        if (open && banner != null) {
            Spacer(Modifier.height(FmSpace.x2))
            Text(service.basis, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
            SelectionContainer { Text(banner, style = Fm.mono(color = Fm.colors.textSecondary)) }
            FmMessageTools(banner)
        }
    }
}

