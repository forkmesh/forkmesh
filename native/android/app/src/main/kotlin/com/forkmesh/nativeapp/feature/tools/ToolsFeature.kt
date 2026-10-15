package com.forkmesh.nativeapp.feature.tools

import androidx.compose.ui.Modifier
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.tools.vault.VaultDeviceAuth
import com.forkmesh.nativeapp.feature.tools.vault.VaultPlatform
import com.forkmesh.nativeapp.feature.tools.vault.VaultScreen
import com.forkmesh.nativeapp.feature.tools.vault.VaultService
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.security.SecretStore
import com.forkmesh.nativeapp.tools.AppLog
import com.forkmesh.nativeapp.tools.AppTelemetry
import com.forkmesh.nativeapp.tools.PerformanceMonitorService
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

object ToolsFeature {
    @Volatile
    var relayFrames: () -> Long = { 0L }

    @Volatile
    var relayLog: StateFlow<List<String>> = MutableStateFlow(emptyList())

    private val sharedText = MutableStateFlow<String?>(null)

    fun install(app: App) {
        DiagnosticsRuntime.install(app)
        installVault(app)
    }

    private fun installVault(app: App) {
        val api = Services.get<ApiService>() ?: return
        val store = Services.get<SecretStore>() ?: return
        val auth = Services.get<AuthService>()
        val vault = Services.put(
            VaultService(
                api,
                accountProvider = { auth?.core?.session?.nodeName ?: "" },
                platform = VaultPlatform(app),
                deviceAuth = VaultDeviceAuth(app),
                store = store,
            ),
        )
        ProjectLifecycle.scope.launch {
            AppLifecycle.foreground.collect { if (it) vault.appResumed() else vault.appPaused() }
        }
        ProjectLifecycle.onClose { runCatching { vault.lock() } }
        if (auth != null) {
            ProjectLifecycle.scope.launch {
                var last = auth.session.value?.nodeName ?: ""
                auth.session.collect { session ->
                    val name = session?.nodeName ?: ""
                    if (name == last) return@collect
                    last = name
                    if (name.isEmpty()) vault.signedOut() else vault.lock()
                }
            }
        }
    }

    @Composable
    fun ToolRows(navigator: Navigator, close: () -> Unit) {
        val requireAccount = AccountFeature.rememberRequireAccount()
        val scope = rememberCoroutineScope()
        fun openAccountTool(action: String, screen: () -> Screen) {
            close()
            scope.launch { if (requireAccount(action)) navigator.push(screen()) }
        }
        ToolSheetRow("sticky_note_2_outlined", "Notes") { openAccountTool("use notes") { NotesScreen() } }
        ToolSheetRow("task_alt", "Tasks") { openAccountTool("use tasks") { TasksScreen() } }
        ToolSheetRow("restaurant_outlined", "Calories") { openAccountTool("use the calorie counter") { CaloriesScreen() } }
        ToolSheetRow("key_outlined", "Vault") { openAccountTool("open your password vault") { VaultScreen() } }
    }

    fun onRelayLogLine(line: String) {
        Services.get<AppLog>()?.addRelayLine(line)
    }

    fun activityScreen(): Screen = ActivityScreen()

    @Composable
    fun TelemetryStrip(navigator: Navigator) {
        val telemetry = Services.get<AppTelemetry>() ?: return
        val settings = Services.get<SettingsService>()
        if (settings != null) {
            val shown by settings.showTelemetryStrip.collectAsState()
            if (!shown) return
        }
        TelemetryStripView(
            telemetry = telemetry,
            onOpenMetric = { metric -> navigator.push(TelemetryScreen(telemetry, focusMetric = metric)) },
            onOpenLogs = { category -> navigator.push(TelemetryScreen(telemetry, logCategory = category)) },
        )
    }

    @Composable
    fun TelemetryStripCompact(navigator: Navigator, modifier: Modifier = Modifier) {
        val telemetry = Services.get<AppTelemetry>() ?: return
        val settings = Services.get<SettingsService>()
        if (settings != null) {
            val shown by settings.showTelemetryStrip.collectAsState()
            if (!shown) return
        }
        TelemetryStripCompactView(telemetry, onOpen = { navigator.push(TelemetryScreen(telemetry)) }, modifier = modifier)
    }

    @Composable
    fun BugReportOverlay(screenName: String, content: @Composable () -> Unit) {
        BugReportOverlayView(screenName, sharedText, content)
    }

    fun onSharedText(navigator: Navigator, text: String) {
        navigator.popTo(navigator.stack.first())
        sharedText.value = text
    }
}

suspend fun <T> PerformanceMonitorService.trackSuspend(
    name: String,
    details: Map<String, Any?> = emptyMap(),
    action: suspend () -> T,
): T {
    if (!enabled) return action()
    val callStack = Throwable()
    val started = System.nanoTime()
    val id = beginOperation(name)
    var error: Throwable? = null
    try {
        return action()
    } catch (thrown: Throwable) {
        error = thrown
        throw thrown
    } finally {
        finishOperation(id, name, (System.nanoTime() - started) / 1000, details, error, error ?: callStack)
    }
}
