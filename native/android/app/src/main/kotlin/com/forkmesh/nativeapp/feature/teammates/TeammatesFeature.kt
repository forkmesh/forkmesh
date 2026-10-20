package com.forkmesh.nativeapp.feature.teammates

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.control.HubControlCalling
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.teammates.ShortcutsStore
import com.forkmesh.nativeapp.teammates.TeammatesStore
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

object TeammatesFeature {
    private val lock = Any()
    internal lateinit var app: App

    private class Runtime(val hub: DesktopControlHub) {
        private val launch: (() -> Unit) -> Unit = { block -> ProjectLifecycle.scope.launch(Dispatchers.IO) { block() } }
        val teammates = TeammatesStore(HubControlCalling(hub), launch)
        val shortcuts = ShortcutsStore(HubControlCalling(hub), launch)

        fun dispose() {
            teammates.stop()
            shortcuts.stop()
        }
    }

    @Volatile
    private var runtime: Runtime? = null

    fun install(app: App) {
        this.app = app
        NavigationFeature.register(Route.Kind.Shortcuts, "WS17") { route -> shortcutsScreen((route as? Route.Shortcuts)?.repo) }
        ProjectLifecycle.onClose { reset() }
    }

    private fun runtime(): Runtime = synchronized(lock) {
        val hub = AgentsFeature.hub
        val existing = runtime
        if (existing != null && existing.hub === hub) return existing
        existing?.dispose()
        Runtime(hub).also { runtime = it }
    }

    val hub: DesktopControlHub get() = AgentsFeature.hub

    val teammates: TeammatesStore get() = runtime().teammates

    val shortcuts: ShortcutsStore get() = runtime().shortcuts

    val startedShortcuts: ShortcutsStore? get() = runtime?.shortcuts

    fun reset() {
        synchronized(lock) {
            runtime?.dispose()
            runtime = null
        }
    }

    fun teammatesScreen(): Screen = TeammatesScreen()

    fun teammateScreen(id: String): Screen = TeammateScreen(id)

    fun teammateEditorScreen(id: String?): Screen = TeammateEditorScreen(id)

    fun shortcutsScreen(repo: RepoRef?): Screen =
        ShortcutsScreen((repo ?: CodeFeature.currentRepository()?.let { RepoRef(it.owner, it.name) })?.slug.orEmpty())

    @Composable
    fun ToolRows(navigator: Navigator, close: () -> Unit) {
        ToolRow("groups_outlined", "Special Agents") {
            close()
            navigator.push(teammatesScreen())
        }
    }

    @Composable
    private fun ToolRow(icon: String, label: String, onClick: () -> Unit) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(FmRadius.md))
                .clickable(onClick = onClick)
                .padding(horizontal = FmSpace.x2, vertical = FmSpace.x3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon(icon, size = 28.dp, tint = Fm.colors.textTertiary)
            Spacer(Modifier.width(FmSpace.x3))
            Text(
                label,
                style = MaterialTheme.typography.titleSmall,
                color = Fm.colors.textSecondary,
                modifier = Modifier.weight(1f),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            FmIcon("chevron_right", size = 20.dp, tint = Fm.colors.textDisabled)
        }
    }
}
