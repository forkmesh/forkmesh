package com.forkmesh.nativeapp.feature.insights

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.market.MarketFeature
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.insights.HttpMonitorProbe
import com.forkmesh.nativeapp.insights.ServiceState
import com.forkmesh.nativeapp.insights.StatusDot
import com.forkmesh.nativeapp.insights.StatusDotRow
import com.forkmesh.nativeapp.insights.StatusDots
import com.forkmesh.nativeapp.insights.StatusReport
import com.forkmesh.nativeapp.market.AccountPackagesState
import com.forkmesh.nativeapp.market.AccountPackagesStore
import com.forkmesh.nativeapp.market.MarketPackage
import com.forkmesh.nativeapp.market.StatusMonitorsStore
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

object StatusPageFeature {
    const val STATUS_PAGE = StatusDots.STATUS_PAGE

    private val installedState = MutableStateFlow(true)
    private val rowState = MutableStateFlow(StatusDotRow.EMPTY)
    private val versionState = MutableStateFlow(0)
    private var wake = Channel<Unit>(Channel.CONFLATED)
    private var loop: Job? = null

    @Volatile
    private var report: StatusReport? = null

    @Volatile
    private var probes: Map<String, Boolean> = emptyMap()

    @Volatile
    private var lastCycleAt = 0L

    @Volatile
    private var retryAt = Long.MAX_VALUE

    @Volatile
    private var forceNext = true

    @Volatile
    private var catalogLoaded = false

    val installed: StateFlow<Boolean> = installedState.asStateFlow()

    val row: StateFlow<StatusDotRow> = rowState.asStateFlow()

    val version: StateFlow<Int> = versionState.asStateFlow()

    var store: AccountPackagesStore? = null
        private set

    var monitors: StatusMonitorsStore? = null
        private set

    fun install(@Suppress("UNUSED_PARAMETER") app: App) {
        loop?.cancel()
        val prefs = PackageGateFeature.projectPrefs()
        val client = MarketFeature.client()
        val packages = AccountPackagesStore(client, prefs) { PackageGateFeature.legacy }
        val monitorStore = StatusMonitorsStore(client, prefs)
        val onChange = { publish() }
        val onMonitors = {
            forceNext = true
            publish()
        }
        packages.addListener(onChange)
        monitorStore.addListener(onMonitors)
        store = packages
        monitors = monitorStore
        report = null
        probes = emptyMap()
        lastCycleAt = 0L
        retryAt = Long.MAX_VALUE
        forceNext = true
        catalogLoaded = false
        wake = Channel(Channel.CONFLATED)
        publish()
        val scope = ProjectLifecycle.scope
        scope.launch {
            AppLifecycle.foreground.collect { front ->
                wake.trySend(Unit)
                if (front) refreshAccount()
            }
        }
        Services.get<AuthService>()?.session?.let { session ->
            scope.launch { session.map { it?.nodeName.orEmpty() }.distinctUntilChanged().drop(1).collect { refreshAccount() } }
        }
        loop = scope.launch { runLoop() }
        ProjectLifecycle.onClose {
            loop?.cancel()
            loop = null
            packages.removeListener(onChange)
            monitorStore.removeListener(onMonitors)
            if (store === packages) store = null
            if (monitors === monitorStore) monitors = null
        }
    }

    fun isInstalled(id: String): Boolean = store?.isInstalled(id) ?: true

    fun installedOrNull(id: String): Boolean? = store?.installedOrNull(id)

    fun rememberCatalog(packages: List<MarketPackage>) {
        store?.rememberCatalog(packages)
        if (packages.any { it.accountSync }) catalogLoaded = true
    }

    suspend fun refreshAccount() {
        val packages = store ?: return
        withContext(Dispatchers.IO) {
            if (!catalogLoaded) runCatching { packages.refreshCatalog() }.onSuccess { catalogLoaded = true }
            runCatching { packages.refresh() }
            if (packages.isInstalled(STATUS_PAGE)) runCatching { monitors?.refresh() }
        }
    }

    fun adopt(state: AccountPackagesState) {
        if (state.changed.contains(STATUS_PAGE) && store?.isInstalled(STATUS_PAGE) == false) monitors?.clear()
        if (state.changed.isNotEmpty()) forceNext = true
        publish()
    }

    private fun publish() {
        val packages = store
        val on = packages?.shows(STATUS_PAGE) ?: true
        if (on != installedState.value) forceNext = true
        installedState.value = on
        rowState.value = if (packages == null || !on) {
            StatusDotRow.EMPTY
        } else {
            val custom = monitors?.monitors.orEmpty().map { StatusDots.customDot(it, probes[it.id]) }
            StatusDots.rowFor(report, packages.catalog, packages::shows, packages.settings(STATUS_PAGE), custom)
        }
        versionState.value = versionState.value + 1
        PackageGateFeature.publish()
        wake.trySend(Unit)
    }

    private suspend fun runLoop() {
        while (true) {
            val active = AppLifecycle.foreground.value && store?.shows(STATUS_PAGE) == true
            var wait = Long.MAX_VALUE
            if (active) {
                val due = minOf(lastCycleAt + rowState.value.refreshMs, retryAt) - System.currentTimeMillis()
                if (forceNext || lastCycleAt == 0L || due <= 0) {
                    forceNext = false
                    val fresh = cycle()
                    retryAt = if (fresh) Long.MAX_VALUE else lastCycleAt + StatusDots.TROUBLED_REFRESH_MS
                    continue
                }
                wait = due
            }
            val channel = wake
            withTimeoutOrNull(wait) { channel.receive() }
        }
    }

    private suspend fun cycle(): Boolean {
        val transport = InsightsFeature.transport
        val fetched = if (transport == null) {
            null
        } else {
            withContext(Dispatchers.IO) { runCatching { StatusReport.fromJson(transport.call("GET", "/api/status", null)) }.getOrNull() }
        }
        if (fetched != null) report = fetched
        val list = monitors?.monitors.orEmpty()
        probes = coroutineScope {
            list.map { monitor -> async(Dispatchers.IO) { monitor.id to HttpMonitorProbe.ok(monitor.url) } }.awaitAll().toMap()
        }
        lastCycleAt = System.currentTimeMillis()
        publish()
        return fetched != null
    }

    @Composable
    fun Dots(onOpen: () -> Unit, modifier: Modifier = Modifier) {
        val on by installed.collectAsState()
        val current by row.collectAsState()
        if (!on || current.dots.isEmpty()) return
        ShellStatusDots(current, onOpen, modifier)
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ShellStatusDots(row: StatusDotRow, onOpen: () -> Unit, modifier: Modifier) {
    FlowRow(
        modifier
            .fillMaxWidth()
            .heightIn(min = 20.dp)
            .clickable(onClick = onOpen)
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = "Status: ${row.description}. Opens the Status page"
            }
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        horizontalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterHorizontally),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        row.dots.forEach { ShellStatusDot(it) }
    }
}

@Composable
internal fun ShellStatusDot(dot: StatusDot, size: androidx.compose.ui.unit.Dp = 7.dp) {
    val color: Color = when (dot.state) {
        ServiceState.Operational -> Fm.colors.success
        ServiceState.Degraded -> Fm.colors.warning
        ServiceState.Down -> Fm.colors.danger
        ServiceState.Unknown, ServiceState.Future -> Fm.colors.textTertiary
    }
    val blink = dot.troubled && !Fm.reducedMotion
    val alpha = if (blink) {
        val value by rememberInfiniteTransition(label = "status-dot").animateFloat(
            initialValue = 1f,
            targetValue = 0.25f,
            animationSpec = infiniteRepeatable(tween(600), RepeatMode.Reverse),
            label = "status-dot-alpha",
        )
        value
    } else {
        1f
    }
    Box(Modifier.size(size).alpha(alpha).clip(CircleShape).background(color))
}
