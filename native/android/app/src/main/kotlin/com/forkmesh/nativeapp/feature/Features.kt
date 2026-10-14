package com.forkmesh.nativeapp.feature

import android.content.Intent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.chat.NodeSyncAuthor
import com.forkmesh.nativeapp.control.MeshControlHosts
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.shell.PingCelebrationOverlay
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.projects.UserPresence
import com.forkmesh.nativeapp.feature.agents.AgentBranchDiff
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.agentwatch.AgentWatchFeature
import com.forkmesh.nativeapp.feature.chat.ChatFeature
import com.forkmesh.nativeapp.feature.chat.DirectCallOverlay
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.hosts.HostsFeature
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.feature.market.StarterPackScreen
import com.forkmesh.nativeapp.feature.localrepos.LocalRepoAgentLauncher
import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import com.forkmesh.nativeapp.feature.localrepos.LocalReposFeature
import com.forkmesh.nativeapp.feature.push.PushFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.inbox.InboxFeature
import com.forkmesh.nativeapp.feature.inbox.NotificationReplySender
import com.forkmesh.nativeapp.feature.surfaces.SurfacesFeature
import com.forkmesh.nativeapp.feature.tools.ToolsFeature
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.shell.HomeShell
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.NavigatorHost

object Features {
    fun install(app: App) {
        AccountFeature.install(app)
        com.forkmesh.nativeapp.feature.market.PackageGateFeature.install(app)
        LocalReposFeature.install(app)
        ChatFeature.install(app)
        AgentsFeature.install(app)
        HostsFeature.install(app)
        com.forkmesh.nativeapp.feature.agents.AgentOpsFeature.install(app)
        com.forkmesh.nativeapp.feature.teammates.TeammatesFeature.install(app)
        ComposerFeature.install(app)
        AgentWatchFeature.install(app)
        ToolsFeature.install(app)
        CodeFeature.install(app)
        PushFeature.install(app)
        NavigationFeature.install(app)
        com.forkmesh.nativeapp.feature.hosts.DesktopRemoteFeature.install(app)
        com.forkmesh.nativeapp.feature.insights.InsightsFeature.install(app)
        com.forkmesh.nativeapp.feature.admin.AdminFeature.install(app)
        com.forkmesh.nativeapp.feature.planning.PlanningFeature.install(app)
        com.forkmesh.nativeapp.feature.glasses.GlassesFeature.install(app)
        com.forkmesh.nativeapp.feature.facechanger.FaceChangerFeature.install()
        com.forkmesh.nativeapp.feature.people.PeopleFeature.install()
        com.forkmesh.nativeapp.feature.docs.DocsFeature.install()
        com.forkmesh.nativeapp.feature.market.MarketFeature.install(app)
        InboxFeature.install(app)
        SurfacesFeature.install(app)
        com.forkmesh.nativeapp.feature.coves.CovesFeature.install(app)
        com.forkmesh.nativeapp.feature.account.settings.SettingsArea.install(app)
        connectAreas(app)
    }

    private fun connectAreas(app: App) {
        AgentsFeature.meshSource = ChatFeature.mesh?.let(::MeshControlHosts)
        AgentsFeature.cloneToDevice = { messenger, status, owner, name, description ->
            LocalReposFeature.cloneForAgent(messenger, status, owner, name, description)
        }
        AgentBranchDiff.screen = { repo, head, base, title -> CodeFeature.branchDiffScreen(repo, head, base, title) }
        LocalReposFeature.confirmPresence = { messenger, reason -> UserPresence.confirm(app, messenger, reason) }
        LocalReposFeature.agentLauncher = LocalRepoAgentLauncher { request ->
            request.navigator.push(
                AgentsFeature.onDeviceAgentScreen(request.record, request.initialPrompt, autoStart = request.autoStart),
            )
        }
        ChatFeature.useNodeSyncRepositories(LocalReposFeature.nodeSyncRepositories()) {
            AgentsFeature.author().let { NodeSyncAuthor(it.name, it.email) }
        }
        ToolsFeature.relayLog = ChatFeature.relayLog
        ToolsFeature.relayFrames = ChatFeature::relayFrames
        InboxFeature.sessionActions = com.forkmesh.nativeapp.feature.agents.AgentSessionActionsBridge
        InboxFeature.replySender = NotificationReplySender { channel, text -> ChatFeature.replyToConversation(channel, text) }
    }

    @Composable
    fun Root(navigator: Navigator, onExit: () -> Unit) {
        Services.get<AuthService>()?.let { auth ->
            val hasAccess by auth.hasAppAccess.collectAsState()
            var hadAccess by remember { mutableStateOf(hasAccess) }
            LaunchedEffect(hasAccess) {
                if (hasAccess && !hadAccess) navigator.setRoot(HomeShell())
                hadAccess = hasAccess
            }
        }
        AccountFeature.Gate {
            val starterPackPending by PackageGateFeature.starterPackPending.collectAsState()
            if (starterPackPending) {
                StarterPackScreen(onContinue = PackageGateFeature::chooseStarterPack)
            } else {
                Box(Modifier.fillMaxSize(), propagateMinConstraints = true) {
                    NavigatorHost(navigator, onExit)
                    PingCelebrationOverlay()
                    DirectCallOverlay()
                }
            }
        }
    }

    fun onIntent(navigator: Navigator, intent: Intent?) {
        if (PushFeature.onIntent(navigator, intent)) return
        if (InboxFeature.onIntent(intent)) return
        if (NavigationFeature.onIntent(navigator, intent)) return
        SurfacesFeature.onShare(intent)
    }
}
