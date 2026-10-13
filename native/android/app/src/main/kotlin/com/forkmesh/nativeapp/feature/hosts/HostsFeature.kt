package com.forkmesh.nativeapp.feature.hosts

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
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.hosts.HostLogStream
import com.forkmesh.nativeapp.hosts.HostsStore
import com.forkmesh.nativeapp.hosts.HubHostsChannel
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

object HostsFeature {
    private val lock = Any()
    private var current: HostsStore? = null

    fun install(app: App) {
        ProjectLifecycle.onClose { reset() }
    }

    val hub: DesktopControlHub get() = AgentsFeature.hub

    fun store(): HostsStore = synchronized(lock) {
        val hub = AgentsFeature.hub
        val existing = current
        if (existing != null && (existing.channel as? HubHostsChannel)?.hub === hub) return existing
        existing?.dispose()
        HostsStore(HubHostsChannel(hub)).also { current = it }
    }

    fun logStream(host: String): HostLogStream = HostLogStream(HubHostsChannel(AgentsFeature.hub), host)

    fun reset() {
        synchronized(lock) {
            current?.dispose()
            current = null
        }
    }

    fun hostsScreen(): Screen = HostsScreen()

    fun hostScreen(name: String): Screen = HostDetailScreen(name)

    @Composable
    fun ToolRow(navigator: Navigator, close: () -> Unit) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(FmRadius.md))
                .clickable {
                    close()
                    navigator.push(hostsScreen())
                }
                .padding(horizontal = FmSpace.x2, vertical = FmSpace.x3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon("dns_outlined", size = 28.dp, tint = Fm.colors.textTertiary)
            Spacer(Modifier.width(FmSpace.x3))
            Text(
                "Hosts",
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
