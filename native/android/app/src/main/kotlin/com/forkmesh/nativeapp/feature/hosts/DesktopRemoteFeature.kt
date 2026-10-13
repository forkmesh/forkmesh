package com.forkmesh.nativeapp.feature.hosts

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
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
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.control.HubControlCalling
import com.forkmesh.nativeapp.feature.account.projects.UserPresence
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.hosts.DesktopHealthLevel
import com.forkmesh.nativeapp.hosts.DesktopUpdateStore
import com.forkmesh.nativeapp.hosts.DiagnosticsStore
import com.forkmesh.nativeapp.hosts.FilesBrowserStore
import com.forkmesh.nativeapp.hosts.FleetStore
import com.forkmesh.nativeapp.hosts.RemoteActionOutcome
import com.forkmesh.nativeapp.hosts.RemoteConfirmation
import com.forkmesh.nativeapp.hosts.RemoteErrorText
import com.forkmesh.nativeapp.hosts.RemoteStoreBase
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

object DesktopRemoteFeature {
    private class Runtime(val hub: DesktopControlHub) {
        val calling = HubControlCalling(hub)
        val fleet by lazy { FleetStore(calling) }
        val files by lazy { FilesBrowserStore(calling) }
        val diagnostics by lazy { DiagnosticsStore(calling) }
        val update by lazy { DesktopUpdateStore(calling) }
        private val built = ArrayList<RemoteStoreBase>()

        fun <T : RemoteStoreBase> track(store: T): T {
            synchronized(built) { if (built.none { it === store }) built.add(store) }
            return store
        }

        fun dispose() = synchronized(built) {
            built.forEach { it.stop() }
            built.clear()
        }
    }

    private val lock = Any()
    private var current: Runtime? = null
    private var appRef: App? = null

    fun install(app: App) {
        appRef = app
        ProjectLifecycle.onClose { reset() }
        val owner = "WS16"
        NavigationFeature.register(Route.Kind.Desktops, owner) { DesktopsScreen() }
        NavigationFeature.register(Route.Kind.Desktop, owner) { route -> desktopScreen((route as Route.Desktop).section) }
        NavigationFeature.register(Route.Kind.Provision, owner) { ProvisionScreen() }
        NavigationFeature.register(Route.Kind.Node, owner) { route -> NodeScreen((route as Route.Node).name) }
        NavigationFeature.register(Route.Kind.Host, owner) { route ->
            route as Route.Host
            when (route.section) {
                "operations", "ops" -> HostOperationsScreen(route.name)
                "disk" -> DiskUsageScreen(route.name, "/")
                "log" -> HostLogScreen(route.name)
                else -> HostsFeature.hostScreen(route.name)
            }
        }
    }

    private fun runtime(): Runtime = synchronized(lock) {
        val hub = AgentsFeature.hub
        val existing = current
        if (existing != null && existing.hub === hub) return existing
        existing?.dispose()
        Runtime(hub).also { current = it }
    }

    fun reset() = synchronized(lock) {
        current?.dispose()
        current = null
    }

    val hub: DesktopControlHub get() = runtime().hub

    val fleet: FleetStore get() = runtime().let { it.track(it.fleet) }

    val files: FilesBrowserStore get() = runtime().let { it.track(it.files) }

    val diagnostics: DiagnosticsStore get() = runtime().let { it.track(it.diagnostics) }

    val update: DesktopUpdateStore get() = runtime().let { it.track(it.update) }

    val app: App? get() = appRef

    fun desktopsScreen(): Screen = DesktopsScreen()

    fun desktopScreen(section: String?): Screen = when (section) {
        "files" -> FileRootsScreen()
        "log" -> DesktopLogScreen()
        "stalls" -> StallsScreen()
        "processes", "memory" -> ProcessesScreen()
        "fleet" -> FleetScreen()
        else -> DesktopScreen()
    }

    fun fleetScreen(): Screen = FleetScreen()

    fun hostOperationsScreen(host: String): Screen = HostOperationsScreen(host)

    fun nodeScreen(name: String): Screen = NodeScreen(name)
}

@Composable
internal fun rememberRemoteTick(store: RemoteStoreBase): Int {
    var tick by remember { mutableIntStateOf(0) }
    DisposableEffect(store) {
        val listener: () -> Unit = { tick++ }
        store.addListener(listener)
        val hub = DesktopRemoteFeature.hub
        hub.addListener(listener)
        onDispose {
            store.removeListener(listener)
            hub.removeListener(listener)
        }
    }
    return tick
}

internal fun describeRemote(error: Throwable): String = RemoteErrorText.describe(error)

@Composable
internal fun ShareBar(share: Double, tint: Color = Fm.colors.accent) {
    val c = Fm.colors
    Box(
        Modifier
            .fillMaxWidth()
            .height(6.dp)
            .background(c.textPrimary.copy(alpha = 0.12f), RoundedCornerShape(3.dp)),
    ) {
        Box(
            Modifier
                .fillMaxWidth(share.coerceIn(0.0, 1.0).toFloat().coerceAtLeast(0.01f))
                .fillMaxHeight()
                .background(tint, RoundedCornerShape(3.dp)),
        )
    }
}

@Composable
internal fun healthLevelColor(level: DesktopHealthLevel): Color = when (level) {
    DesktopHealthLevel.Ok -> Fm.colors.success
    DesktopHealthLevel.Busy -> Fm.colors.warning
    DesktopHealthLevel.Strained -> Fm.colors.danger
    DesktopHealthLevel.Unknown -> Fm.colors.textDisabled
}

@Composable
internal fun RemoteOutcomeCard(outcome: RemoteActionOutcome) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.Top) {
            FmIcon(if (outcome.ok) "check_circle_outline" else "error_outline", size = 18.dp, tint = if (outcome.ok) c.success else c.danger)
            Spacer(Modifier.width(FmSpace.x2))
            Column(Modifier.weight(1f)) {
                Text(outcome.headline, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                if (outcome.message.isNotEmpty()) {
                    SelectionContainer { Text(outcome.message, style = Fm.text.bodySmall.copy(color = c.textSecondary), maxLines = 6) }
                }
            }
        }
    }
}

@Composable
internal fun OutdatedNote() {
    Row(verticalAlignment = Alignment.CenterVertically) {
        FmIcon("system_update_alt", size = 16.dp, tint = Fm.colors.warning)
        Spacer(Modifier.width(FmSpace.x2))
        Text(
            "This desktop's ForkMesh does not answer this yet. Update it from the desktop's Settings.",
            style = Fm.text.bodySmall.copy(color = Fm.colors.warning),
        )
    }
}

@Composable
internal fun RemoteConfirmDialog(
    title: String,
    confirmation: RemoteConfirmation,
    destructive: Boolean = false,
    perform: (RemoteConfirmation, String) -> String,
    onDone: (String) -> Unit = {},
    onDismiss: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    val messenger = LocalFmMessenger.current
    var typed by remember(confirmation.token) { mutableStateOf("") }
    var running by remember(confirmation.token) { mutableStateOf(false) }
    var error by remember(confirmation.token) { mutableStateOf("") }
    val openedAt by remember(confirmation.token) { mutableLongStateOf(System.currentTimeMillis()) }
    FmConfirmDialog(
        title = title,
        body = confirmation.summary + "\n\nThe desktop runs this with its own keys; they never leave it. Every change is written to the desktop's log.",
        confirmLabel = title,
        destructive = destructive,
        busy = running,
        onDismissRequest = { if (!running) onDismiss() },
        onConfirm = {
            if (running || !confirmation.accepts(typed)) return@FmConfirmDialog
            if (System.currentTimeMillis() - openedAt > confirmation.expiresInMs) {
                error = RemoteErrorText.describe("confirmation_expired")
                return@FmConfirmDialog
            }
            running = true
            scope.launch {
                try {
                    if (confirmation.needsDeviceAuthentication) {
                        val context = DesktopRemoteFeature.app
                        val ok = context != null && UserPresence.confirm(context, messenger, title)
                        if (!ok) {
                            error = "Your fingerprint, face or screen lock is needed for this."
                            return@launch
                        }
                    }
                    val message = offMain { perform(confirmation, typed) }
                    onDone(message)
                    onDismiss()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = describeRemote(failure)
                } finally {
                    running = false
                }
            }
        },
        fields = if (!confirmation.needsTypedConfirmation && error.isEmpty()) null else ({
            if (confirmation.needsTypedConfirmation) {
                FmTextField(
                    value = typed,
                    onValueChange = { typed = it },
                    hint = confirmation.typeToConfirm,
                    helperText = "Type ${confirmation.typeToConfirm} to confirm. Your screen lock confirms it is you before the desktop acts.",
                    keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
                    textStyle = Fm.text.bodyLarge.copy(fontFamily = FmType.mono),
                )
            }
            if (error.isNotEmpty()) Text(error, style = Fm.text.bodySmall.copy(color = Fm.colors.danger))
        }),
    )
}
