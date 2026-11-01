package com.forkmesh.nativeapp.feature.insights

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
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
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.insights.DesktopLoad
import com.forkmesh.nativeapp.insights.DesktopStatsClient
import com.forkmesh.nativeapp.insights.EndpointRow
import com.forkmesh.nativeapp.insights.ExitReport
import com.forkmesh.nativeapp.insights.ExitReportStore
import com.forkmesh.nativeapp.insights.InsightsLoad
import com.forkmesh.nativeapp.insights.LogTimeline
import com.forkmesh.nativeapp.insights.NetworkSection
import com.forkmesh.nativeapp.insights.NetworkSnapshot
import com.forkmesh.nativeapp.insights.NodeDirectory
import com.forkmesh.nativeapp.insights.NodeDiagnosticSeverity
import com.forkmesh.nativeapp.insights.OnlineHistory
import com.forkmesh.nativeapp.insights.RepoInsightsSummary
import com.forkmesh.nativeapp.insights.RepoQuality
import com.forkmesh.nativeapp.insights.RepoStatsSnapshot
import com.forkmesh.nativeapp.insights.SentryIssue
import com.forkmesh.nativeapp.insights.ServiceState
import com.forkmesh.nativeapp.insights.StatsBoardModel
import com.forkmesh.nativeapp.insights.StatsCatalog
import com.forkmesh.nativeapp.insights.StatsFormat
import com.forkmesh.nativeapp.insights.StatsLayoutStore
import com.forkmesh.nativeapp.insights.StatsTone
import com.forkmesh.nativeapp.insights.StatusReport
import com.forkmesh.nativeapp.insights.StatusSystem
import com.forkmesh.nativeapp.insights.DesktopConfirmation
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.model.PullRequest
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
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
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal suspend fun <T> io(block: () -> T): T = withContext(Dispatchers.IO) { block() }

@Composable
private fun Loading() {
    CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp)
}

@Composable
private fun ErrorLine(text: String) {
    Text(text, style = Fm.text.bodySmall.copy(color = Fm.colors.danger))
}

internal class StatsScreen : Screen() {
    override val title: String get() = "Stats"
    private var version by mutableIntStateOf(0)
    private var editing by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val model = remember {
            val transport = InsightsFeature.transport ?: com.forkmesh.nativeapp.insights.InsightsRequesting { _, _, _ -> throw IllegalStateException("Sign in first.") }
            val client = InsightsFeature.desktopClient()
            StatsBoardModel(
                transport,
                InsightsFeature.statsLayoutStore() ?: StatsLayoutStore(MemoryPrefStore()),
                desktop = client?.let { paired -> { paired.health() } },
                desktopTiles = client?.let { paired -> { paired.tiles() } },
            )
        }
        fun reload() {
            scope.launch {
                io { model.reload { scope.launch { version++ } } }
                version++
            }
        }
        LaunchedEffect(model) { reload() }
        DetailScaffold(title, navigator, largeTitle = true, actions = {
            FmIconButton(if (editing) "check" else "edit", if (editing) "Done" else "Edit", onPressed = { editing = !editing }, color = c.textPrimary)
            FmIconButton("refresh", "Refresh", onPressed = { reload() }, color = c.textPrimary)
        }) {
            version.let { }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                if (model.failures.isNotEmpty()) item { Text("Could not load: ${model.failures.joinToString(", ")}", style = Fm.text.bodySmall.copy(color = c.warning)) }
                if (model.desktopNote.isNotEmpty()) item { Text(model.desktopNote, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                if (model.desktopTilesNote.isNotEmpty()) item { Text(model.desktopTilesNote, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                val visible = model.visible
                val rows = ArrayList<List<com.forkmesh.nativeapp.insights.StatsTilePlacement>>()
                var pending = ArrayList<com.forkmesh.nativeapp.insights.StatsTilePlacement>()
                for (placement in visible) {
                    if (placement.width >= 2 || editing) {
                        if (pending.isNotEmpty()) rows.add(pending)
                        pending = ArrayList()
                        rows.add(listOf(placement))
                    } else {
                        pending.add(placement)
                        if (pending.size == 2) {
                            rows.add(pending)
                            pending = ArrayList()
                        }
                    }
                }
                if (pending.isNotEmpty()) rows.add(pending)
                items(rows.size) { index ->
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        for (placement in rows[index]) {
                            Box(Modifier.weight(1f)) {
                                Tile(model, placement.id, editing) {
                                    version++
                                }
                            }
                        }
                        if (rows[index].size == 1 && rows[index][0].width < 2 && !editing) Spacer(Modifier.weight(1f))
                    }
                }
                if (editing) {
                    val addable = model.addable
                    if (addable.isNotEmpty()) {
                        item { FmSectionHeader("Add a tile") }
                        for ((group, entries) in addable.groupBy { it.group }) {
                            item(key = "add-group-$group") { Text(group, style = Fm.text.labelMedium.copy(color = c.textSecondary)) }
                            items(entries, key = { "add-" + it.id }) { entry ->
                                FmListRow(entry.title, subtitle = entry.hint, leading = { FmIcon(entry.icon, size = 20.dp, tint = c.textSecondary) }, onTap = {
                                    model.add(entry.id)
                                    version++
                                }, showChevron = false)
                            }
                        }
                    }
                    item { FmToneButton("Reset the board", onPressed = { model.reset(); version++ }) }
                }
                item { FmSectionHeader("More") }
                item { FmListRow("Status", leading = { FmIcon("verified_outlined", size = 20.dp, tint = c.textSecondary) }, onTap = { navigator.push(StatusScreen()) }, minHeight = 48.dp) }
                item { FmListRow("Network", leading = { FmIcon("hub_outlined", size = 20.dp, tint = c.textSecondary) }, onTap = { navigator.push(NetworkScreen(NetworkSection.Nodes)) }, minHeight = 48.dp) }
                item { FmListRow("Desktop log timeline and Sentry", leading = { FmIcon("timeline", size = 20.dp, tint = c.textSecondary) }, onTap = { navigator.push(DesktopInsightsScreen()) }, minHeight = 48.dp) }
                item { FmListRow("This app's crash reports", leading = { FmIcon("bug_report", size = 20.dp, tint = c.textSecondary) }, onTap = { navigator.push(ExitReportsScreen()) }, minHeight = 48.dp) }
                if (model.updatedAtMs > 0) item { Text("Updated ${relativeTime(model.updatedAtMs)}", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
            }
        }
    }

    @Composable
    private fun Tile(model: StatsBoardModel, id: String, editing: Boolean, changed: () -> Unit) {
        val c = Fm.colors
        val entry = StatsCatalog.entry(id) ?: return
        val data = model.data(id)
        val loading = model.isLoading(id)
        FmCard(Modifier.fillMaxWidth()) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmIcon(entry.icon, size = 16.dp, tint = c.textTertiary)
                    Spacer(Modifier.width(FmSpace.x1))
                    Text(entry.title, style = Fm.text.labelMedium.copy(color = c.textSecondary), modifier = Modifier.weight(1f), maxLines = 1)
                    if (loading && data != null) {
                        CircularProgressIndicator(Modifier.size(12.dp), strokeWidth = 1.5.dp)
                    } else if (data != null && data.status != StatsTone.None) {
                        Box(Modifier.size(8.dp).background(toneColor(data.status), RoundedCornerShape(4.dp)))
                    }
                }
                when {
                    data == null && loading -> Loading()
                    data == null -> Text("—", style = Fm.text.titleLarge.copy(color = c.textDisabled))
                    !data.available -> {
                        Text("—", style = Fm.text.titleLarge.copy(color = c.textDisabled))
                        if (data.note.isNotEmpty()) Text(data.note, style = Fm.text.labelSmall.copy(color = c.textTertiary), maxLines = 2)
                    }
                    else -> {
                        Row(verticalAlignment = Alignment.Bottom) {
                            Text(data.value, style = Fm.text.titleLarge.copy(color = c.textPrimary, fontWeight = FontWeight.SemiBold))
                            data.trendPercent?.let { percent ->
                                Spacer(Modifier.width(FmSpace.x2))
                                Text(
                                    (if (percent >= 0) "+" else "") + StatsFormat.percent(percent, 0),
                                    style = Fm.text.labelSmall.copy(color = toneColor(data.trendTone)),
                                )
                            }
                        }
                        if (data.note.isNotEmpty()) Text(data.note, style = Fm.text.labelSmall.copy(color = c.textTertiary), maxLines = 2)
                        if (data.series.categorical) {
                            BarChart(data.series.values, c.accent, height = 28.dp)
                        } else if (!data.series.isEmpty) {
                            Sparkline(data.series.values, c.accent)
                        }
                    }
                }
                if (editing) {
                    Row {
                        val index = model.layout.indexOfFirst { it.id == id }
                        FmIconButton("arrow_upward", "Move up", onPressed = if (index > 0) ({ model.move(id, index - 1); changed() }) else null, busy = false)
                        FmIconButton("arrow_downward", "Move down", onPressed = if (index >= 0 && index < model.layout.size - 1) ({ model.move(id, index + 1); changed() }) else null, busy = false)
                        FmIconButton("width_wide", "Wide", onPressed = { model.toggleWide(id); changed() })
                        FmIconButton("delete_outline", "Remove", onPressed = { model.remove(id); changed() })
                    }
                }
            }
        }
    }
}

internal class StatusScreen : Screen() {
    override val title: String get() = "Status"
    private var report by mutableStateOf<InsightsLoad<StatusReport>>(InsightsLoad.Idle)
    private var loadedAt by mutableStateOf(0L)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        fun reload() {
            val transport = InsightsFeature.transport ?: return
            if (report.valueOrNull == null) report = InsightsLoad.Loading
            scope.launch {
                val current = report
                report = io { InsightsLoad.attempt(current) { StatusReport.fromJson(transport.call("GET", "/api/status", null)) } }
                if (report is InsightsLoad.Loaded) loadedAt = System.currentTimeMillis()
            }
        }
        LaunchedEffect(Unit) { reload() }
        DetailScaffold(title, navigator, largeTitle = true, actions = { FmIconButton("refresh", "Refresh", onPressed = { reload() }, color = c.textPrimary) }) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                val value = report.valueOrNull
                when {
                    value != null -> {
                        item {
                            FmCard(Modifier.fillMaxWidth()) {
                                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                    Row(verticalAlignment = Alignment.CenterVertically) {
                                        FmStatusBadge(value.overall.label, toneColor(value.overall.tone))
                                        Spacer(Modifier.width(FmSpace.x2))
                                        Text(value.headline, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                                    }
                                    value.websiteUptime24h?.let { Text("Website uptime ${StatsFormat.percent(it, 2)} in 24 h", style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                                    val current = value.current
                                    val facts = listOfNotNull(
                                        current.onlineNodes?.let { "$it nodes online" },
                                        current.catalogRepos?.let { "$it repositories" },
                                        current.errors24h?.let { "$it errors in 24 h" },
                                        current.mainnodeOnline?.let { if (it) "verified mirror online" else "verified mirror offline" },
                                    )
                                    if (facts.isNotEmpty()) Text(facts.joinToString(" · "), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                                }
                            }
                        }
                        item { FmSectionHeader("Systems", count = value.serviceSystems.size) }
                        items(value.serviceSystems, key = { it.id }) { SystemRow(it) }
                        if (value.nodeSystems.isNotEmpty()) {
                            item { FmSectionHeader("Nodes", count = value.nodeSystems.size) }
                            items(value.nodeSystems, key = { it.id }) { SystemRow(it) }
                        }
                        if (value.mirrorSystems.isNotEmpty()) {
                            item { FmSectionHeader("Mirrors", count = value.mirrorSystems.size) }
                            items(value.mirrorSystems, key = { it.id }) { SystemRow(it) }
                        }
                        if (loadedAt > 0) item { Text("Checked ${relativeTime(loadedAt)}", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                    }
                    report is InsightsLoad.Failed -> item { ErrorLine((report as InsightsLoad.Failed).error.message) }
                    InsightsFeature.transport == null -> item { FmEmptyState(icon = "cloud_off", title = "Not connected", message = "Choose a project first.") }
                    else -> item { Loading() }
                }
            }
        }
    }

    @Composable
    private fun SystemRow(system: StatusSystem) {
        val c = Fm.colors
        FmCard(Modifier.fillMaxWidth()) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(8.dp).background(toneColor(system.state.tone), RoundedCornerShape(4.dp)))
                    Spacer(Modifier.width(FmSpace.x2))
                    Text(system.label, style = Fm.text.bodyMedium.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                    Text(system.uptime24h?.let { StatsFormat.percent(it, 2) } ?: system.state.label, style = Fm.text.labelSmall.copy(color = c.textSecondary))
                }
                if (system.minutes.isNotEmpty()) {
                    BarChart(
                        emptyList(),
                        c.success,
                        height = 14.dp,
                        stacked = system.minutes.map { minute ->
                            listOf(1.0 to when (minute.state) {
                                ServiceState.Operational -> c.success
                                ServiceState.Degraded -> c.warning
                                ServiceState.Down -> c.danger
                                else -> c.textDisabled
                            })
                        },
                    )
                }
                if (system.reason.isNotEmpty() && system.state != ServiceState.Operational) Text(system.reason, style = Fm.text.labelSmall.copy(color = c.danger))
                if (system.detail.isNotEmpty()) Text(system.detail, style = Fm.text.labelSmall.copy(color = c.textTertiary))
            }
        }
    }
}

internal class NetworkScreen(initial: NetworkSection) : Screen() {
    override val title: String get() = "Network"
    private var section by mutableStateOf(initial)
    private var query by mutableStateOf("")
    private var snapshot by mutableStateOf<InsightsLoad<NetworkSnapshot>>(InsightsLoad.Idle)
    private var history by mutableStateOf<InsightsLoad<OnlineHistory>>(InsightsLoad.Idle)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        fun reload() {
            val transport = InsightsFeature.transport ?: return
            if (snapshot.valueOrNull == null) snapshot = InsightsLoad.Loading
            if (history.valueOrNull == null) history = InsightsLoad.Loading
            scope.launch {
                val s = snapshot
                snapshot = io { InsightsLoad.attempt(s) { NetworkSnapshot.fromJson(transport.call("GET", "/api/network/stats", null)) } }
            }
            scope.launch {
                val h = history
                history = io { InsightsLoad.attempt(h) { OnlineHistory.fromJson(transport.call("GET", "/api/network/online-history", null)) } }
            }
        }
        LaunchedEffect(Unit) { reload() }
        DetailScaffold(title, navigator, largeTitle = true, actions = { FmIconButton("refresh", "Refresh", onPressed = { reload() }, color = c.textPrimary) }) {
            Column(Modifier.fillMaxSize()) {
                FmSegmentTabBar(NetworkSection.entries.map { FmSegmentTab(it.title) }, section.ordinal, onChanged = { section = NetworkSection.entries[it] })
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    when (section) {
                        NetworkSection.Nodes -> {
                            item { FmTextField(value = query, onValueChange = { query = it }, hint = "Search nodes, owners or platforms") }
                            val rows = NodeDirectory.filter(NodeDirectory.merge(InsightsFeature.livePeers(), snapshot.valueOrNull?.onlineNodes.orEmpty()), query)
                            val warnings = rows.count { it.worst.rank >= NodeDiagnosticSeverity.Warning.rank }
                            if (warnings > 0) item { Text("$warnings node${if (warnings == 1) "" else "s"} with health warnings", style = Fm.text.bodySmall.copy(color = c.warning)) }
                            snapshot.errorOrNull?.let { item { ErrorLine(it.message) } }
                            items(rows, key = { it.id }) { row ->
                                FmCard(Modifier.fillMaxWidth(), onTap = { navigator.push(com.forkmesh.nativeapp.feature.hosts.DesktopRemoteFeature.nodeScreen(row.name)) }) {
                                    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                        Row(verticalAlignment = Alignment.CenterVertically) {
                                            Box(Modifier.size(8.dp).background(if (row.online) c.success else c.textDisabled, RoundedCornerShape(4.dp)))
                                            Spacer(Modifier.width(FmSpace.x2))
                                            Text(row.name + if (row.isSelf) " (this phone)" else "", style = Fm.text.bodyMedium.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                                            if (row.worst.rank >= NodeDiagnosticSeverity.Warning.rank) FmStatusBadge(row.worst.label, toneColor(row.worst.tone))
                                        }
                                        Text(row.subtitle, style = Fm.text.labelSmall.copy(color = c.textSecondary))
                                        for (finding in row.diagnostics.filter { it.severity.rank >= NodeDiagnosticSeverity.Warning.rank }) {
                                            Text(finding.message, style = Fm.text.labelSmall.copy(color = toneColor(finding.severity.tone)))
                                        }
                                    }
                                }
                            }
                        }
                        NetworkSection.Relays -> {
                            items(InsightsFeature.relayRows(), key = { it.id }) { relay ->
                                FmListRow(
                                    relay.label + if (relay.active) " · active" else "",
                                    subtitle = listOf(
                                        relay.host,
                                        if (relay.verifiedAt > 0) "checked ${relativeTime(relay.verifiedAt)}" else "",
                                        if (!relay.declared) "features not declared" else if (relay.missing.isNotEmpty()) "missing ${relay.missing.joinToString(", ")}" else "",
                                    ).filter { it.isNotEmpty() }.joinToString(" · "),
                                    showChevron = false,
                                )
                            }
                        }
                        NetworkSection.Endpoints -> {
                            val endpoints = EndpointRow.forRelay(InsightsFeature.activeServerUrl)
                            if (endpoints.isEmpty()) item { Text("No project is active.", style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                            items(endpoints, key = { it.name }) { endpoint ->
                                FmCard(Modifier.fillMaxWidth()) {
                                    Column {
                                        Text(endpoint.name, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                                        SelectionContainer { Text(endpoint.url, style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textSecondary)) }
                                        Text(endpoint.purpose, style = Fm.text.labelSmall.copy(color = c.textTertiary))
                                    }
                                }
                            }
                        }
                        NetworkSection.Activity -> {
                            val value = history.valueOrNull
                            val stats = snapshot.valueOrNull
                            if (stats != null) item { Text("${stats.repos} repositories · ${stats.hosts} hosts · ${stats.onlineNodes.size} nodes online", style = Fm.text.bodySmall.copy(color = c.textPrimary)) }
                            if (value != null && value.hours.isNotEmpty()) {
                                item { FmSectionHeader("Node minutes online, last 24 hours") }
                                item { BarChart(value.hours.map { it.nodeMinutes.toDouble() }, c.accent) }
                                value.peakHour?.let { peak -> item { Text("Busiest hour: ${peak.nodeMinutes} node minutes", style = Fm.text.labelSmall.copy(color = c.textTertiary)) } }
                                if (value.nodes.isNotEmpty()) {
                                    item { FmSectionHeader("Most online") }
                                    items(value.nodes, key = { "n-" + it.id }) { node ->
                                        Row {
                                            Text(node.label.ifEmpty { node.id }, style = Fm.text.bodySmall.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                                            Text(StatsFormat.compactDuration(node.totalMinutes * 60_000L), style = Fm.text.labelSmall.copy(color = c.textSecondary))
                                        }
                                    }
                                }
                            } else if (history is InsightsLoad.Failed) {
                                item { ErrorLine((history as InsightsLoad.Failed).error.message) }
                            } else {
                                item { Loading() }
                            }
                        }
                    }
                }
            }
        }
    }
}

internal class DesktopInsightsScreen : Screen() {
    override val title: String get() = "Desktop insights"
    private var timeline by mutableStateOf<DesktopLoad<LogTimeline>>(DesktopLoad.Idle)
    private var sentry by mutableStateOf<DesktopLoad<List<SentryIssue>>>(DesktopLoad.Idle)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val client = remember { InsightsFeature.desktopClient() }
        fun reload() {
            val desk = client ?: return
            if (timeline.valueOrNull == null) timeline = DesktopLoad.Loading
            if (sentry.valueOrNull == null) sentry = DesktopLoad.Loading
            scope.launch { timeline = io { DesktopLoad.attempt { desk.timeline() } } }
            scope.launch { sentry = io { DesktopLoad.attempt { desk.sentry() } } }
        }
        LaunchedEffect(client) { reload() }
        DetailScaffold(title, navigator, actions = { FmIconButton("refresh", "Refresh", onPressed = { reload() }, color = c.textPrimary) }) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                if (client == null) {
                    item { FmEmptyState(icon = "desktop_windows", title = "No desktop paired", message = "Pair a desktop to see its log timeline and Sentry issues.") }
                    return@LazyColumn
                }
                item { FmSectionHeader("Log activity, last 30 days") }
                when (val load = timeline) {
                    is DesktopLoad.Loaded -> {
                        val days = load.value.days
                        item {
                            BarChart(
                                emptyList(),
                                c.accent,
                                stacked = days.map { listOf(it.info.toDouble() to c.textDisabled, it.warnings.toDouble() to c.warning, it.errors.toDouble() to c.danger) },
                            )
                        }
                        item { Legend(listOf("Errors" to c.danger, "Warnings" to c.warning, "Info" to c.textDisabled)) }
                        load.value.busiest?.let { busiest -> item { Text("Busiest day: ${busiest.total} lines, ${busiest.errors} errors", style = Fm.text.labelSmall.copy(color = c.textTertiary)) } }
                    }
                    else -> item { LoadState(load) }
                }
                item { FmSectionHeader("Sentry: unresolved issues") }
                when (val load = sentry) {
                    is DesktopLoad.Loaded -> {
                        if (load.value.isEmpty()) item { Text("No unresolved issues.", style = Fm.text.bodySmall.copy(color = c.success)) }
                        items(load.value, key = { it.id }) { issue ->
                            FmListRow(
                                issue.title.ifEmpty { issue.id },
                                subtitle = listOf(issue.project, "${issue.count} events", if (issue.lastSeen > 0) relativeTime(issue.lastSeen) else "").filter { it.isNotEmpty() }.joinToString(" · "),
                                leading = { Box(Modifier.size(8.dp).background(toneColor(issue.tone), RoundedCornerShape(4.dp))) },
                                showChevron = false,
                            )
                        }
                    }
                    else -> item { LoadState(load) }
                }
            }
        }
    }
}

@Composable
private fun <T> LoadState(load: DesktopLoad<T>) {
    val c = Fm.colors
    when (load) {
        DesktopLoad.Unsupported -> Text("Your desktop does not offer this yet. Update ForkMesh on the desktop.", style = Fm.text.bodySmall.copy(color = c.warning))
        is DesktopLoad.Failed -> ErrorLine(load.message)
        else -> Loading()
    }
}

internal class RepoInsightsScreen(private val repo: Repository) : Screen() {
    override val title: String get() = "Insights"
    private var summary by mutableStateOf<InsightsLoad<RepoInsightsSummary>>(InsightsLoad.Idle)
    private var quality by mutableStateOf<DesktopLoad<RepoQuality>>(DesktopLoad.Idle)
    private var stats by mutableStateOf<DesktopLoad<RepoStatsSnapshot>>(DesktopLoad.Idle)
    private var pendingRatchet by mutableStateOf<DesktopConfirmation?>(null)
    private var ratchetError by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val client = remember { InsightsFeature.desktopClient() }
        val repoName = "${repo.displayOwner}/${repo.name}"
        fun reload() {
            val api = Services.get<ApiService>() ?: return
            if (summary.valueOrNull == null) summary = InsightsLoad.Loading
            scope.launch {
                val current = summary
                summary = io {
                    InsightsLoad.attempt(current) {
                        val commits = runCatching { api.commits(repo.owner, repo.name) }.getOrDefault(emptyList())
                        val issues = runCatching { api.publishedIssues(repo.owner, repo.name) }.getOrDefault(emptyList())
                        val pulls = runCatching { api.publishedPulls(repo.owner, repo.name) }.getOrDefault(emptyList())
                            .map { PullRequest(it.number, it.title, status = it.status) }
                        RepoInsightsSummary.build(commits, issues, pulls, repo, System.currentTimeMillis())
                    }
                }
            }
            val desk = client ?: return
            if (stats.valueOrNull == null) stats = DesktopLoad.Loading
            if (quality.valueOrNull == null) quality = DesktopLoad.Loading
            scope.launch {
                stats = io { DesktopLoad.attempt { desk.repoStats(repoName) } }
                quality = io { DesktopLoad.attempt { desk.quality(repoName) } }
            }
        }
        LaunchedEffect(repo) { reload() }
        DetailScaffold("$repoName insights", navigator, actions = { FmIconButton("refresh", "Refresh", onPressed = { reload() }, color = c.textPrimary) }) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                val value = summary.valueOrNull
                if (value != null) {
                    item {
                        Text(
                            listOf(value.language, "${value.stars} stars", "${value.forks} forks", "${value.mirrors} mirrors").filter { it.isNotEmpty() }.joinToString(" · "),
                            style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        )
                    }
                    item { FmSectionHeader("Commits, last ${RepoInsightsSummary.WEEKS} weeks") }
                    item { BarChart(value.weekly.map { it.commits.toDouble() }, c.accent) }
                    item { Text("${value.commitsInWindow} commits in the window, ${value.commitsSeen} read", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                    if (value.contributors.isNotEmpty()) {
                        item { FmSectionHeader("Top contributors") }
                        items(value.contributors, key = { "c-" + it.name }) { person ->
                            Row {
                                Text(person.name, style = Fm.text.bodySmall.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                                Text("${person.commits}", style = Fm.text.labelMedium.copy(color = c.textSecondary))
                            }
                        }
                    }
                    item { FmSectionHeader("Issues and pull requests") }
                    item {
                        Text(
                            "${value.openIssues} open issues, ${value.closedIssues} closed · ${value.openPulls} open PRs, ${value.mergedPulls} merged, ${value.closedPulls} closed",
                            style = Fm.text.bodySmall.copy(color = c.textPrimary),
                        )
                    }
                    if (value.burnup.isNotEmpty()) {
                        item { FmSectionHeader("Burnup") }
                        item {
                            LineChart(
                                listOf(
                                    value.burnup.map { it.total.toDouble() } to c.textSecondary,
                                    value.burnup.map { it.closedCount.toDouble() } to c.success,
                                ),
                            )
                        }
                        item { Legend(listOf("All issues" to c.textSecondary, "Closed" to c.success)) }
                    }
                } else if (summary is InsightsLoad.Failed) {
                    item { ErrorLine((summary as InsightsLoad.Failed).error.message) }
                } else {
                    item { Loading() }
                }
                item { FmSectionHeader("On your desktop") }
                if (client == null) {
                    item { Text("Pair a desktop to see quality scores and the ratchet.", style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                } else {
                    when (val load = stats) {
                        is DesktopLoad.Loaded -> {
                            val snapshot = load.value
                            item { Text("${snapshot.commits} commits · ${snapshot.files} files · ${StatsFormat.compactNumber(snapshot.lines.toDouble())} lines", style = Fm.text.bodySmall.copy(color = c.textPrimary)) }
                            item {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Column(Modifier.weight(1f)) {
                                        Text("Ratchet mode", style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                                        if (snapshot.ratchetNote.isNotEmpty()) Text(snapshot.ratchetNote, style = Fm.text.labelSmall.copy(color = c.textTertiary))
                                    }
                                    FmSwitch(checked = snapshot.ratchetEnabled, onCheckedChange = { enabled ->
                                        ratchetError = ""
                                        scope.launch {
                                            try {
                                                pendingRatchet = io { client.prepareRatchet(repoName, enabled) }
                                            } catch (cancelled: CancellationException) {
                                                throw cancelled
                                            } catch (failure: Exception) {
                                                ratchetError = DesktopStatsClient.explain(failure)
                                            }
                                        }
                                    })
                                }
                            }
                        }
                        else -> item { LoadState(load) }
                    }
                    if (ratchetError.isNotEmpty()) item { ErrorLine(ratchetError) }
                    when (val load = quality) {
                        is DesktopLoad.Loaded -> {
                            val q = load.value
                            item {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Text("Quality ${q.score}", style = Fm.text.titleSmall.copy(color = toneColor(q.tone)))
                                    Spacer(Modifier.width(FmSpace.x2))
                                    if (q.grade.isNotEmpty()) FmStatusBadge(q.grade, toneColor(q.tone))
                                }
                            }
                            items(q.files.take(30), key = { "q-" + it.path }) { file ->
                                Column {
                                    Row {
                                        Text(file.path, style = Fm.text.labelMedium.copy(fontFamily = FmType.mono, color = c.textPrimary), modifier = Modifier.weight(1f), maxLines = 1)
                                        Text("${file.score}", style = Fm.text.labelMedium.copy(color = c.textSecondary))
                                    }
                                    for (finding in file.findings.take(3)) Text(finding, style = Fm.text.labelSmall.copy(color = c.textTertiary))
                                }
                            }
                        }
                        else -> item { LoadState(load) }
                    }
                }
            }
        }
        pendingRatchet?.let { confirmation ->
            val desk = client
            FmConfirmDialog(
                title = "Ratchet mode",
                body = confirmation.summary,
                confirmLabel = "Confirm",
                onDismissRequest = { pendingRatchet = null },
                onConfirm = {
                    pendingRatchet = null
                    if (desk == null) return@FmConfirmDialog
                    scope.launch {
                        try {
                            stats = DesktopLoad.Loaded(io { desk.runRatchet(confirmation) })
                        } catch (cancelled: CancellationException) {
                            throw cancelled
                        } catch (failure: Exception) {
                            ratchetError = DesktopStatsClient.explain(failure)
                        }
                    }
                },
            )
        }
    }
}

internal class ExitReportsScreen : Screen() {
    override val title: String get() = "Crash reports"
    private var reports by mutableStateOf<List<ExitReport>>(emptyList())
    private var loaded by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val clipboard = LocalClipboardManager.current
        val store = InsightsFeature.exitStore
        fun reload() {
            scope.launch {
                reports = io { store.load() }
                loaded = true
            }
        }
        LaunchedEffect(Unit) { reload() }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("delete_sweep", "Clear", onPressed = if (reports.isEmpty()) null else ({ scope.launch { io { store.clear() }; reload() } }), color = c.textPrimary, busy = false)
        }) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                if (loaded && reports.isEmpty()) {
                    item {
                        FmEmptyState(
                            icon = "verified_outlined",
                            title = "No crashes recorded",
                            message = "Android reports how this app last stopped. Crashes, freezes and memory kills show up here after the next launch.",
                        )
                    }
                }
                val counts = ExitReportStore.counts(reports)
                if (counts.isNotEmpty()) item { Text(counts.joinToString(" · ") { "${it.second} ${it.first.title.lowercase()}" }, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                items(reports, key = { it.id }) { report ->
                    FmCard(Modifier.fillMaxWidth()) {
                        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                FmIcon(report.kind.icon, size = 18.dp, tint = c.danger)
                                Spacer(Modifier.width(FmSpace.x2))
                                Text(report.kind.title, style = Fm.text.titleSmall.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                                Text(relativeTime(report.receivedAt), style = Fm.text.labelSmall.copy(color = c.textTertiary))
                            }
                            SelectionContainer { Text(report.summary, style = Fm.text.bodySmall.copy(color = c.textPrimary)) }
                            Text(listOf(report.appVersion, report.osVersion).filter { it.isNotEmpty() }.joinToString(" · "), style = Fm.text.labelSmall.copy(color = c.textTertiary))
                            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                FmToneButton("Send to an agent", icon = "auto_awesome", tone = FmTone.Accent, onPressed = { navigator.push(AgentsFeature.startSheetScreen(report.agentPrompt())) })
                                FmToneButton("Copy", onPressed = { clipboard.setText(AnnotatedString(report.stallLogLine + "\n\n" + report.detail)) })
                                FmToneButton("Remove", onPressed = { scope.launch { io { store.remove(report.id) }; reload() } })
                            }
                        }
                    }
                }
                item { Spacer(Modifier.height(FmSpace.x4)) }
            }
        }
    }
}
