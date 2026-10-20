package com.forkmesh.nativeapp.feature.tools

import android.app.ActivityManager
import com.forkmesh.nativeapp.ui.theme.FmType
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.composer.QuickLaunchOutcome
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.AppTab
import com.forkmesh.nativeapp.navigation.title
import com.forkmesh.nativeapp.tools.AppChangelog
import com.forkmesh.nativeapp.tools.AppTelemetry
import com.forkmesh.nativeapp.tools.DiagnosticsBundle
import com.forkmesh.nativeapp.tools.PerformanceAlert
import com.forkmesh.nativeapp.tools.PerformanceMonitorService
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.navigation.AppNavigation
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class InstalledVersion(val name: String, val code: String) {
    val label: String get() = AppChangelog.label(name, code)
}

object AppAboutFeature {
    private const val SENT_KEY = "performanceDiagnosticsSentAt"
    const val REFRESH_MS = 5_000L

    private val alertState = MutableStateFlow(PerformanceAlert.NONE)
    val alert: StateFlow<PerformanceAlert> = alertState.asStateFlow()

    fun installed(context: Context): InstalledVersion {
        val info = runCatching { context.packageManager.getPackageInfo(context.packageName, 0) }.getOrNull()
        val code = when {
            info == null -> 0L
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.P -> info.longVersionCode
            else -> @Suppress("DEPRECATION") info.versionCode.toLong()
        }
        return InstalledVersion(info?.versionName.orEmpty(), if (code > 0) code.toString() else "")
    }

    fun changelog(context: Context): AppChangelog {
        val text = runCatching { context.assets.open(AppChangelog.ASSET).bufferedReader().use { it.readText() } }.getOrNull()
        val version = installed(context)
        return AppChangelog.load(text, version.name, version.code)
    }

    private val lastSent: Long?
        get() = Services.get<PrefStore>()?.getString(SENT_KEY)?.toLongOrNull()

    fun exitDiagnostics(context: Context): List<PerformanceAlert.Diagnostic> {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return emptyList()
        val manager = context.getSystemService(ActivityManager::class.java) ?: return emptyList()
        return runCatching { manager.getHistoricalProcessExitReasons(context.packageName, 0, 5) }.getOrDefault(emptyList())
            .mapNotNull { exit ->
                val summary = when (exit.reason) {
                    ApplicationExitInfo.REASON_ANR -> "The app stopped responding (ANR)"
                    ApplicationExitInfo.REASON_CRASH -> "The app crashed"
                    ApplicationExitInfo.REASON_CRASH_NATIVE -> "The app crashed in native code"
                    else -> return@mapNotNull null
                }
                PerformanceAlert.Diagnostic(summary, exit.timestamp)
            }
    }

    suspend fun refresh(context: Context) {
        val events = Services.get<PerformanceMonitorService>()?.events.orEmpty()
        val exits = withContext(Dispatchers.IO) { exitDiagnostics(context) }
        alertState.value = PerformanceAlert.evaluate(
            events.map { PerformanceAlert.Sample(it) }, exits, lastSent, System.currentTimeMillis(),
        )
    }

    suspend fun prompt(context: Context, includeLogs: Boolean): String = withContext(Dispatchers.Default) {
        val now = System.currentTimeMillis()
        val reason = alertState.value.reason
        val version = installed(context)
        val shell = NavigationFeature.shell()
        val contextLines = buildList {
            add("app" to "${version.label} (${DiagnosticsRuntime.build.mode})")
            add("os" to "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
            add("device" to "${Build.MANUFACTURER} ${Build.MODEL}")
            if (reason.isNotEmpty()) add("alert" to reason)
            if (shell != null) {
                add("tab" to shell.selected.title)
                val path = shell.routes(shell.selected).map { it.title }
                add("screen" to if (path.isEmpty()) "tab root" else path.joinToString(" > "))
            }
            val recents = NavigationFeature.recents.value.take(5).map { it.title }
            if (recents.isNotEmpty()) add("recent screens" to recents.joinToString(", "))
        }
        val report = DiagnosticsBundle.report(
            contextLines,
            Services.get<AppTelemetry>(),
            Services.get<PerformanceMonitorService>()?.events.orEmpty(),
            exitDiagnostics(context),
            now,
        )
        DiagnosticsBundle.prompt(reason, report, includeLogs)
    }

    suspend fun send(prompt: String): String? {
        val quick = ComposerFeature.quick()
        quick.prepare(DiagnosticsBundle.REPOSITORY)
        val target = quick.repository
        if (target == null || "${target.owner}/${target.name}".lowercase() != DiagnosticsBundle.REPOSITORY) {
            return "${DiagnosticsBundle.REPOSITORY} is not among the repositories this phone can start agents in."
        }
        return when (val outcome = quick.send(prompt, emptyList(), agentOnly = true)) {
            is QuickLaunchOutcome.Queued -> {
                AgentsFeature.ring()
                openAgent(AgentsFeature.launchedSessionScreen(outcome.repo, outcome.session))
                markSent()
                null
            }
            is QuickLaunchOutcome.QueuedWithoutSession -> {
                AgentsFeature.ring()
                openAgent(null)
                markSent()
                null
            }
            is QuickLaunchOutcome.Desktop -> {
                openAgent(AgentsFeature.allAgentsScreen())
                markSent()
                null
            }
            is QuickLaunchOutcome.Device ->
                "Agents for ${DiagnosticsBundle.REPOSITORY} run on this phone, so the report cannot start one here."
            is QuickLaunchOutcome.NeedsFix -> outcome.message
            is QuickLaunchOutcome.Failed -> outcome.message
            QuickLaunchOutcome.Ignored -> "There was nothing to send."
            is QuickLaunchOutcome.Done -> outcome.message
        }
    }

    private fun openAgent(screen: Screen?) {
        val shell = NavigationFeature.shell()
        if (shell == null) {
            screen?.let { AppNavigation.navigator.push(it) }
            return
        }
        shell.select(AppTab.Agents, reselect = false)
        screen?.let { shell.navigator(AppTab.Agents).push(it) }
    }

    private fun markSent() {
        Services.get<PrefStore>()?.putString(SENT_KEY, System.currentTimeMillis().toString())
        alertState.value = PerformanceAlert.NONE
    }
}

@Composable
fun HubVersionRow(onWhatsNew: () -> Unit, onDiagnostics: () -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val version = remember { AppAboutFeature.installed(context) }
    val alert by AppAboutFeature.alert.collectAsState()
    LaunchedEffect(Unit) {
        while (true) {
            AppAboutFeature.refresh(context)
            delay(AppAboutFeature.REFRESH_MS)
        }
    }
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(c.bgElevated)
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(
            Modifier
                .weight(1f)
                .heightIn(min = 36.dp)
                .clickable(onClick = onWhatsNew)
                .semantics(mergeDescendants = true) {
                    role = Role.Button
                    contentDescription = "${version.label}. What's new"
                },
            verticalArrangement = Arrangement.spacedBy(2.dp, Alignment.CenterVertically),
        ) {
            Text(version.label, style = Fm.text.labelSmall.copy(color = c.textSecondary, fontFeatureSettings = "tnum"))
            Text("What's new", style = Fm.text.labelMedium.copy(color = c.accent))
        }
        Spacer(Modifier.width(FmSpace.x2))
        DiagnosticsButton(alert, onDiagnostics)
    }
}

@Composable
private fun DiagnosticsButton(alert: PerformanceAlert, onClick: () -> Unit) {
    val c = Fm.colors
    val tint = if (alert.detected) Color.White else c.textSecondary
    Row(
        Modifier
            .heightIn(min = 44.dp)
            .clip(RoundedCornerShape(50))
            .background(if (alert.detected) c.danger else c.bgOverlay)
            .clickable(onClick = onClick)
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = if (alert.detected) "Send diagnostics to an agent. ${alert.reason}" else "Send diagnostics to an agent"
            }
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(if (alert.detected) "warning_amber_outlined" else "speed_rounded", size = 16.dp, tint = tint)
        Spacer(Modifier.width(FmSpace.x1))
        Text(alert.label, style = Fm.text.labelMedium.copy(color = tint), maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun PageHeader(title: String, onBack: () -> Unit, trailing: @Composable () -> Unit = {}) {
    BackHandler(onBack = onBack)
    Row(
        Modifier.fillMaxWidth().padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmTextButton(onClick = onBack) {
            FmIcon("arrow_back", size = 20.dp)
            Spacer(Modifier.width(FmSpace.x1))
            Text("You")
        }
        Text(
            title,
            modifier = Modifier.weight(1f).semantics { heading() },
            style = Fm.text.titleMedium,
            maxLines = 1,
        )
        trailing()
    }
}

@Composable
fun WhatsNewPage(onBack: () -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val changelog = remember { AppAboutFeature.changelog(context) }
    Column(Modifier.fillMaxWidth()) {
        PageHeader("What's new", onBack)
        LazyColumn(Modifier.fillMaxWidth(), contentPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x6)) {
            changelog.entries.forEachIndexed { index, entry ->
                item(key = "head:${entry.build}:$index") {
                    Column(Modifier.padding(top = FmSpace.x4, bottom = FmSpace.x2).semantics(mergeDescendants = true) { heading() }) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(entry.heading, style = Fm.text.titleSmall)
                            if (index == 0) {
                                Spacer(Modifier.width(FmSpace.x2))
                                Text(
                                    "Installed",
                                    style = Fm.text.labelSmall.copy(color = c.accent),
                                    modifier = Modifier.clip(RoundedCornerShape(50)).background(c.accentSubtle).padding(horizontal = FmSpace.x2, vertical = 2.dp),
                                )
                            }
                        }
                        if (entry.date.isNotEmpty()) Text(entry.formattedDate(), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                    }
                }
                item(key = "body:${entry.build}:$index") {
                    Column(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(FmRadius.md)).background(c.bgOverlay).padding(FmSpace.x3),
                        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
                    ) {
                        if (entry.sections.isEmpty()) {
                            Text(
                                if (index == 0) "No notes were written for this build." else "No notes for this build.",
                                style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                            )
                        }
                        entry.sections.forEach { section ->
                            Column(Modifier.semantics(mergeDescendants = true) {}, verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                Text(section.title, style = Fm.text.labelLarge.copy(color = c.accent))
                                section.items.forEach { line ->
                                    Row {
                                        Text("•", style = Fm.text.bodyMedium.copy(color = c.textTertiary))
                                        Spacer(Modifier.width(FmSpace.x2))
                                        Text(line, style = Fm.text.bodyMedium)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

private val SENT_CONTENTS = listOf(
    "App version and build",
    "Phone model and Android version",
    "Frame, main-thread and memory timings, and the slowest events",
    "Recent crash and not-responding reports from Android",
    "The screen you are on and your recent screens",
)

@Composable
fun DiagnosticsSendPage(onBack: () -> Unit, onSent: () -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val alert by AppAboutFeature.alert.collectAsState()
    var includeLogs by remember { mutableStateOf(true) }
    var preview by remember { mutableStateOf("") }
    var showPreview by remember { mutableStateOf(false) }
    var sending by remember { mutableStateOf(false) }
    var problem by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(includeLogs) {
        preview = ""
        preview = AppAboutFeature.prompt(context, includeLogs)
    }
    Column(Modifier.fillMaxWidth()) {
        PageHeader("Send diagnostics", onBack) {
            FmTextButton(
                onClick = {
                    if (sending || preview.isEmpty()) return@FmTextButton
                    sending = true
                    problem = null
                    scope.launch {
                        val failure = AppAboutFeature.send(preview)
                        sending = false
                        if (failure == null) onSent() else problem = failure
                    }
                },
                enabled = !sending && preview.isNotEmpty(),
            ) { Text(if (sending) "Sending…" else "Send") }
        }
        LazyColumn(Modifier.fillMaxWidth(), contentPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x6)) {
            item(key = "summary") {
                Row(Modifier.padding(vertical = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    FmIcon(
                        if (alert.detected) "warning_amber_outlined" else "speed_rounded",
                        size = 22.dp,
                        tint = if (alert.detected) c.danger else c.accent,
                    )
                    Spacer(Modifier.width(FmSpace.x3))
                    Column {
                        Text(if (alert.detected) "Performance issue detected" else "No issue detected right now", style = Fm.text.titleSmall)
                        Text(
                            if (alert.detected) alert.reason else "A report still helps an agent find slow spots.",
                            style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        )
                    }
                }
            }
            item(key = "contents-head") {
                Text("What is sent", style = Fm.text.labelLarge.copy(color = c.textSecondary), modifier = Modifier.padding(top = FmSpace.x3, bottom = FmSpace.x1))
            }
            items(SENT_CONTENTS, key = { it }) { line ->
                Row(Modifier.padding(vertical = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
                    FmIcon("check", size = 16.dp, tint = c.accent)
                    Spacer(Modifier.width(FmSpace.x2))
                    Text(line, style = Fm.text.bodyMedium)
                }
            }
            item(key = "logs") {
                Row(Modifier.padding(vertical = FmSpace.x2).heightIn(min = 44.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("Include recent log lines", style = Fm.text.bodyMedium, modifier = Modifier.weight(1f))
                    FmSwitch(checked = includeLogs, onCheckedChange = { includeLogs = it })
                }
            }
            item(key = "privacy") {
                Text(
                    "Tokens, passwords, emails, phone numbers, link queries and message contents are removed on this phone first. " +
                        "A coding agent on ${DiagnosticsBundle.REPOSITORY} reads the report, fixes the cause on a branch and adds tests.",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                    modifier = Modifier.padding(vertical = FmSpace.x2),
                )
            }
            problem?.let { message ->
                item(key = "problem") { Text(message, style = Fm.text.bodySmall.copy(color = c.danger), modifier = Modifier.padding(vertical = FmSpace.x2)) }
            }
            item(key = "preview") {
                Column {
                    FmTextButton(onClick = { showPreview = !showPreview }) { Text(if (showPreview) "Hide what is sent" else "Show what is sent") }
                    if (showPreview) {
                        Text(
                            preview.ifEmpty { "Building the report…" },
                            style = Fm.text.bodySmall.copy(color = c.textSecondary, fontFamily = FmType.mono),
                            modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(FmRadius.md)).background(c.bgOverlay).padding(FmSpace.x3),
                        )
                    }
                }
            }
        }
    }
}
