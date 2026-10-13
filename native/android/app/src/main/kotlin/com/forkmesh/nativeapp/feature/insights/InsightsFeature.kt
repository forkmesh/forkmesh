package com.forkmesh.nativeapp.feature.insights

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import androidx.annotation.RequiresApi
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.control.HubControlCalling
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.chat.ChatFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.insights.ApiInsightsTransport
import com.forkmesh.nativeapp.insights.DesktopStatsClient
import com.forkmesh.nativeapp.insights.ExitReport
import com.forkmesh.nativeapp.insights.ExitReportStore
import com.forkmesh.nativeapp.insights.InsightsRequesting
import com.forkmesh.nativeapp.insights.NetworkSection
import com.forkmesh.nativeapp.insights.NodeDiagnosticFinding
import com.forkmesh.nativeapp.insights.NodeRow
import com.forkmesh.nativeapp.insights.RelayRow
import com.forkmesh.nativeapp.insights.StatsLayoutStore
import com.forkmesh.nativeapp.insights.StatsTone
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.account.projects.RelayFeature
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

object InsightsFeature {
    private var appRef: App? = null

    val app: App? get() = appRef

    val exitStore: ExitReportStore by lazy {
        ExitReportStore { File(appRef?.filesDir ?: File(System.getProperty("java.io.tmpdir") ?: "/tmp"), "diagnostics") }
    }

    fun install(app: App) {
        appRef = app
        val owner = "WS20"
        NavigationFeature.register(Route.Kind.Stats, owner) { StatsScreen() }
        NavigationFeature.register(Route.Kind.Status, owner) { StatusScreen() }
        NavigationFeature.register(Route.Kind.Network, owner) { route -> NetworkScreen(NetworkSection.of((route as Route.Network).section)) }
        StatusPageFeature.install(app)
        ProjectLifecycle.scope.launch(Dispatchers.IO) { collectExitReports(app) }
    }

    val transport: InsightsRequesting? get() = Services.get<ApiService>()?.let(::ApiInsightsTransport)

    val isAdmin: Boolean get() = Services.get<AuthService>()?.session?.value?.isAdmin == true

    val hostLabel: String get() = Services.get<ProjectsService>()?.directory?.active?.host.orEmpty()

    val projectScope: String get() = Services.get<ProjectsService>()?.directory?.activeId.orEmpty()

    val activeServerUrl: String get() = Services.get<ProjectsService>()?.directory?.activeServerUrl.orEmpty()

    fun statsLayoutStore(): StatsLayoutStore? = Services.get<PrefStore>()?.let { StatsLayoutStore(it, projectScope) }

    fun desktopClient(): DesktopStatsClient? =
        runCatching { AgentsFeature.hub }.getOrNull()?.takeIf { it.desktop != null }?.let { DesktopStatsClient(HubControlCalling(it)) }

    fun livePeers(): List<NodeRow> = runCatching { ChatFeature.relay?.roster().orEmpty() }.getOrDefault(emptyList()).map { NodeRow.fromMember(it, NodeDiagnosticFinding.parse(it.diagnostics)) }

    fun relayRows(): List<RelayRow> {
        val directory = Services.get<ProjectsService>()?.directory ?: return emptyList()
        return directory.projects.map { project ->
            RelayRow(
                id = project.id,
                label = project.label.ifEmpty { project.host },
                host = project.host,
                active = project.id == directory.activeId,
                verifiedAt = project.verifiedAt,
                declared = project.features.isDeclared,
                missing = RelayFeature.entries.filterNot(project.features::has).map { it.name },
            )
        }
    }

    fun statsScreen(): Screen = StatsScreen()

    fun statusScreen(): Screen = StatusScreen()

    fun networkScreen(section: NetworkSection = NetworkSection.Nodes): Screen = NetworkScreen(section)

    fun repoInsightsScreen(repo: Repository): Screen = RepoInsightsScreen(repo)

    fun desktopInsightsScreen(): Screen = DesktopInsightsScreen()

    fun exitReportsScreen(): Screen = ExitReportsScreen()

    private fun collectExitReports(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        runCatching { collectR(context) }
    }

    @RequiresApi(Build.VERSION_CODES.R)
    private fun collectR(context: Context) {
        val manager = context.getSystemService(ActivityManager::class.java) ?: return
        val newest = exitStore.newestTimestamp()
        val version = runCatching { context.packageManager.getPackageInfo(context.packageName, 0).versionName.orEmpty() }.getOrDefault("")
        val os = "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})"
        val reports = manager.getHistoricalProcessExitReasons(context.packageName, 0, 20)
            .filter { it.timestamp > newest }
            .mapNotNull { info -> ExitReport.fromExit(info.reason, info.timestamp, info.pid, info.description.orEmpty(), trace(info), version, os) }
        if (reports.isNotEmpty()) exitStore.append(reports)
    }

    @RequiresApi(Build.VERSION_CODES.R)
    private fun trace(info: ApplicationExitInfo): String = runCatching {
        info.traceInputStream?.use { stream -> stream.readNBytesCompat(ExitReport.DETAIL_LIMIT) }.orEmpty()
    }.getOrDefault("")

    private fun java.io.InputStream.readNBytesCompat(limit: Int): String {
        val buffer = ByteArray(limit)
        var total = 0
        while (total < limit) {
            val read = read(buffer, total, limit - total)
            if (read < 0) break
            total += read
        }
        return String(buffer, 0, total, Charsets.UTF_8)
    }
}

@Composable
internal fun toneColor(tone: StatsTone): Color = when (tone) {
    StatsTone.Good -> Fm.colors.success
    StatsTone.Warning -> Fm.colors.warning
    StatsTone.Critical -> Fm.colors.danger
    StatsTone.None -> Fm.colors.textSecondary
}

@Composable
internal fun Sparkline(values: List<Double>, color: Color, modifier: Modifier = Modifier, height: Dp = 28.dp) {
    if (values.size < 2) return
    Canvas(modifier.fillMaxWidth().height(height)) {
        val max = values.maxOrNull() ?: 0.0
        val min = values.minOrNull() ?: 0.0
        val span = (max - min).takeIf { it > 0 } ?: 1.0
        val step = size.width / (values.size - 1)
        var previous: Offset? = null
        values.forEachIndexed { index, value ->
            val point = Offset(index * step, (size.height - ((value - min) / span * size.height)).toFloat())
            previous?.let { drawLine(color, it, point, strokeWidth = 2.dp.toPx()) }
            previous = point
        }
    }
}

@Composable
internal fun BarChart(values: List<Double>, color: Color, modifier: Modifier = Modifier, height: Dp = 80.dp, stacked: List<List<Pair<Double, Color>>>? = null) {
    if (values.isEmpty() && stacked.isNullOrEmpty()) return
    val track = Fm.colors.textPrimary.copy(alpha = 0.08f)
    Canvas(modifier.fillMaxWidth().height(height)) {
        val count = stacked?.size ?: values.size
        val totals = stacked?.map { parts -> parts.sumOf { it.first } } ?: values
        val max = (totals.maxOrNull() ?: 0.0).takeIf { it > 0 } ?: 1.0
        val slot = size.width / count
        val width = (slot * 0.7f).coerceAtLeast(1f)
        for (index in 0 until count) {
            val x = index * slot + (slot - width) / 2
            drawRect(track, Offset(x, 0f), Size(width, size.height))
            if (stacked != null) {
                var top = size.height
                for ((value, tint) in stacked[index]) {
                    val h = (value / max * size.height).toFloat()
                    top -= h
                    drawRect(tint, Offset(x, top), Size(width, h))
                }
            } else {
                val h = (values[index] / max * size.height).toFloat()
                drawRect(color, Offset(x, size.height - h), Size(width, h))
            }
        }
    }
}

@Composable
internal fun LineChart(series: List<Pair<List<Double>, Color>>, modifier: Modifier = Modifier, height: Dp = 120.dp) {
    val longest = series.maxOfOrNull { it.first.size } ?: 0
    if (longest < 2) return
    Canvas(modifier.fillMaxWidth().height(height)) {
        val max = (series.flatMap { it.first }.maxOrNull() ?: 0.0).takeIf { it > 0 } ?: 1.0
        val step = size.width / (longest - 1)
        for ((values, color) in series) {
            var previous: Offset? = null
            values.forEachIndexed { index, value ->
                val point = Offset(index * step, (size.height - value / max * size.height).toFloat())
                previous?.let { drawLine(color, it, point, strokeWidth = 2.dp.toPx()) }
                drawCircle(color, 2.dp.toPx(), point, style = Stroke(1.dp.toPx()))
                previous = point
            }
        }
    }
}

@Composable
internal fun Legend(items: List<Pair<String, Color>>) {
    Column {
        for ((label, color) in items) Text("■ $label", style = Fm.text.labelSmall.copy(color = color))
    }
}
