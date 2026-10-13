package com.forkmesh.nativeapp.feature.push

import com.forkmesh.nativeapp.chat.MESH_ALERT_HREF
import com.forkmesh.nativeapp.chat.MESH_CONVERSATION
import com.forkmesh.nativeapp.feature.chat.ChatDepth
import com.forkmesh.nativeapp.feature.chat.ChatFeature
import com.forkmesh.nativeapp.shell.HomeShell
import com.forkmesh.nativeapp.push.directMessageConversation
import com.forkmesh.nativeapp.push.directMessageTargetConversation
import com.forkmesh.nativeapp.push.isDirectMessageTarget
import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.feature.account.notifications.PingDetailScreen
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.agents.RunnerAttentionFeature
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.runners.RunnerAttention
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.inbox.InboxFeature
import com.forkmesh.nativeapp.inbox.InboxItem
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.navigation.AppTab
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.push.PushTarget
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.navigation.AppNavigation
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object PushFeature {
    private const val PERMISSION_ASKED_KEY = "push/permissionAsked"
    private const val PERMISSION_REQUEST = 7303

    const val BACKGROUND_TITLE = "Stay connected in the background"
    const val BACKGROUND_NOTE =
        "Keeps one connection to this project open while the app is closed, so pings and nearby #mesh messages arrive right away. " +
            "It uses a little battery and shows a quiet notification while it runs. " +
            "Turn it off and they wait until you open the app."

    @Volatile
    private var globalsReady = false

    @Volatile
    private var appContext: Context? = null

    fun install(app: App) {
        appContext = app
        if (!globalsReady) {
            globalsReady = true
            PingNotifier.ensureChannels(app)
            AppLifecycle.scope.launch {
                AppLifecycle.foreground.collect { foreground -> if (foreground) BackgroundLink.sync(app) }
            }
        }
        val projects = BackgroundLink.projectsService() ?: return
        BackgroundLink.projects = projects
        val projectId = projects.directory.active.id
        val scope = ProjectLifecycle.scope
        val auth = Services.get<AuthService>() ?: return
        scope.launch {
            var hadSession: Boolean? = null
            var lastNode = ""
            auth.session.map { it?.sessionToken.orEmpty() to it?.nodeName.orEmpty() }.distinctUntilChanged { a, b -> a.first == b.first }.collect { (token, node) ->
                val signedIn = token.isNotEmpty()
                if (hadSession == true && !signedIn) {
                    PingNotifier.cancelProject(app, projectId)
                    PingAlerts.forget(app, projectId)
                    PingCache.get(app).clear(projectId, lastNode)
                }
                if (node.isNotEmpty()) lastNode = node
                hadSession = signedIn
                BackgroundLink.sync(app)
            }
        }
        val notifications = Services.get<NotificationService>() ?: return
        scope.launch {
            notifications.notifications.collect { rows ->
                if (rows.isEmpty()) return@collect
                val node = auth.core.session?.nodeName.orEmpty()
                val api = Services.get<ApiService>() ?: return@collect
                if (node.isEmpty()) return@collect
                withContext(Dispatchers.IO) {
                    val settings = PingAlerts.settingsFor(projectId, api, node)
                    val label = projects.directory.byId(projectId)?.label ?: projectId
                    val many = BackgroundLink.accounts().size > 1
                    PingAlerts.deliver(app, projectId, label, many, rows, settings)
                }
            }
        }
    }

    fun onIntent(navigator: Navigator, intent: Intent?): Boolean {
        if (intent?.action != PingNotifier.ACTION_OPEN) return false
        val target = PushTarget.from(
            intent.getStringExtra(PushTarget.EXTRA_PROJECT),
            intent.getStringExtra(PushTarget.EXTRA_PING),
            intent.getStringExtra(PushTarget.EXTRA_HREF),
            intent.getStringExtra(PushTarget.EXTRA_AGENT),
            intent.getStringExtra(PushTarget.EXTRA_ROW),
        )
        intent.action = null
        intent.removeExtra(PushTarget.EXTRA_PROJECT)
        intent.removeExtra(PushTarget.EXTRA_PING)
        intent.removeExtra(PushTarget.EXTRA_HREF)
        intent.removeExtra(PushTarget.EXTRA_AGENT)
        intent.removeExtra(PushTarget.EXTRA_ROW)
        if (target == null) return true
        AppLifecycle.scope.launch { open(navigator, target) }
        return true
    }

    suspend fun open(navigator: Navigator, target: PushTarget) {
        val projects = BackgroundLink.projectsService()
        var nav = navigator
        if (projects != null && target.projectId.isNotEmpty() && target.projectId != projects.directory.active.id &&
            projects.directory.byId(target.projectId) != null
        ) {
            val result = projects.switchTo(target.projectId)
            if (!result.ok) return
            nav = AppNavigation.navigator
        }
        if (target.href == MESH_ALERT_HREF) {
            openMesh(nav)
            return
        }
        if (isDirectMessageTarget(target)) {
            openDirectMessages(nav, target)
            return
        }
        val ping = remember(target)
        val link = target.link
        val runner = RunnerAttention.pingTarget(target.href)
        when {
            target.agent.isNotEmpty() -> {
                nav.push(AgentsFeature.agentScreen(target.agent))
                markReadLater(target.pingId, ping)
            }
            runner != null -> {
                RunnerAttentionFeature.refresh(force = true)
                RunnerAttentionFeature.open(nav, runner) { url -> openExternalUrl(App.instance, url) }
                markReadLater(target.pingId, ping)
            }
            link != null -> {
                openLink(nav, link)
                markReadLater(target.pingId, ping)
            }
            ping != null -> {
                openPing(nav, ping)
                Services.get<NotificationService>()?.refreshInBackground(silent = true)
            }
            else -> {
                openInbox(nav)
                markReadLater(target.pingId, null)
            }
        }
    }

    private fun remember(target: PushTarget): ForkNotification? {
        if (target.pingId.isEmpty()) return null
        val service = Services.get<NotificationService>()
        val row = target.ping ?: service?.find(target.pingId) ?: snapshotRow(service, target.pingId) ?: return null
        service?.insert(row)
        InboxFeature.insertPing(row)
        return row
    }

    private fun snapshotRow(service: NotificationService?, id: String): ForkNotification? {
        val context = appContext ?: return null
        val node = Services.get<AuthService>()?.core?.session?.nodeName.orEmpty()
        if (node.isEmpty()) return null
        return PingCache.get(context).find(service?.project ?: Services.get<ProjectScope>()?.projectId.orEmpty(), node, id)
    }

    private fun openPing(navigator: Navigator, ping: ForkNotification) {
        val root = navigator.stack.firstOrNull() as? HomeShell
        val item = InboxFeature.model?.item(ping.id) ?: InboxFeature.insertPing(ping)
        if (root != null && item != null) {
            navigator.popTo(root)
            root.select(AppTab.Inbox, reselect = false)
            val inbox = root.navigator(AppTab.Inbox)
            inbox.popTo(inbox.stack.first())
            inbox.push(InboxFeature.itemScreen(item))
            return
        }
        if (Services.get<NotificationService>() != null) navigator.push(PingDetailScreen(ping))
    }

    private fun openInbox(navigator: Navigator) {
        val root = navigator.stack.firstOrNull() as? HomeShell ?: return
        navigator.popTo(root)
        root.select(AppTab.Inbox, reselect = false)
        root.navigator(AppTab.Inbox).let { it.popTo(it.stack.first()) }
    }

    private fun markReadLater(pingId: String, ping: ForkNotification?) {
        if (pingId.isEmpty()) return
        AppLifecycle.scope.launch {
            val model = InboxFeature.model
            val item = model?.item(pingId) ?: ping?.let { InboxItem.fromPing(it) }
            if (model != null && item != null) {
                withContext(Dispatchers.IO) { runCatching { model.markRead(item) } }
                InboxFeature.publish()
            } else {
                val api = Services.get<ApiService>()
                val node = Services.get<AuthService>()?.core?.session?.nodeName.orEmpty()
                if (api != null && node.isNotEmpty()) {
                    withContext(Dispatchers.IO) { runCatching { api.markNotificationsRead(node, listOf(pingId)) } }
                }
            }
            Services.get<NotificationService>()?.refreshInBackground(silent = true)
        }
    }

    private suspend fun openDirectMessages(navigator: Navigator, target: PushTarget) {
        val direct = directMessageTargetConversation(target)
        val hub = ChatDepth.hub
        val relay = ChatFeature.relay
        if (direct.isNotEmpty() && hub != null) {
            withContext(Dispatchers.IO) {
                runCatching {
                    hub.direct?.refresh()
                    hub.select(direct)
                }
            }
        } else if (relay != null) {
            val conversation = withContext(Dispatchers.IO) {
                runCatching { directMessageConversation(relay.unreadConversations, MESH_CONVERSATION) }.getOrNull()
            }
            if (conversation != null) withContext(Dispatchers.IO) { runCatching { relay.switchConversation(conversation) } }
        }
        val root = navigator.stack.firstOrNull() as? HomeShell ?: return
        navigator.popTo(root)
        root.showTab("Chat")
    }

    private suspend fun openMesh(navigator: Navigator) {
        ChatFeature.relay?.let { relay -> withContext(Dispatchers.IO) { runCatching { relay.switchConversation(MESH_CONVERSATION) } } }
        val root = navigator.stack.firstOrNull() as? HomeShell ?: return
        navigator.popTo(root)
        root.showTab("Chat")
    }

    fun openLink(navigator: Navigator, link: NotificationDeepLink) {
        if (link.initialTab == RepoDetailTab.Agents && link.session > 0) {
            navigator.push(AgentsFeature.sessionScreen(link.repo, link.session))
            return
        }
        navigator.push(CodeFeature.repoDetailScreen(link.repo, link.initialTab, link))
    }

    private fun Context.activity(): Activity? {
        var current: Context? = this
        while (current is ContextWrapper) {
            if (current is Activity) return current
            current = current.baseContext
        }
        return null
    }

    private fun needsPermission(context: Context): Boolean =
        Build.VERSION.SDK_INT >= 33 &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED

    fun requestNotificationPermission(context: Context) {
        if (needsPermission(context)) requestPermission(context)
    }

    private fun requestPermission(context: Context) {
        if (Build.VERSION.SDK_INT < 33) return
        context.activity()?.requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), PERMISSION_REQUEST)
    }

    @Composable
    fun PermissionPrompt() {
        val context = LocalContext.current
        val auth = Services.get<AuthService>() ?: return
        val session by auth.session.collectAsState()
        val mesh by ChatFeature.meshSnapshot.collectAsState()
        val nearby = (mesh?.peerCount ?: 0) > 0
        LaunchedEffect(session?.sessionToken, nearby) {
            if (session?.sessionToken.isNullOrEmpty() && !nearby) return@LaunchedEffect
            if (!needsPermission(context)) return@LaunchedEffect
            val prefs = Prefs(context)
            if (prefs.getBool(PERMISSION_ASKED_KEY, false)) return@LaunchedEffect
            prefs.putBool(PERMISSION_ASKED_KEY, true)
            requestPermission(context)
        }
    }

    @Composable
    fun BackgroundCard() {
        val context = LocalContext.current
        val projectId = Services.get<ProjectScope>()?.projectId ?: BackgroundLink.projectsService()?.directory?.active?.id.orEmpty()
        val foreground by AppLifecycle.foreground.collectAsState()
        var enabled by remember(projectId) { mutableStateOf(BackgroundLink.isEnabled(context, projectId)) }
        val permitted = remember(foreground) { PingNotifier.allowed(context) }
        val unrestricted = remember(foreground) {
            context.getSystemService(PowerManager::class.java)?.isIgnoringBatteryOptimizations(context.packageName) == true
        }
        val c = Fm.colors
        Column(Modifier.fillMaxWidth()) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(BACKGROUND_TITLE, style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis))
                    VGap(FmSpace.x1)
                    Text(BACKGROUND_NOTE, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                }
                HGap(FmSpace.x3)
                FmSwitch(
                    checked = enabled,
                    onCheckedChange = {
                        enabled = it
                        BackgroundLink.setEnabled(context, projectId, it)
                    },
                    modifier = Modifier.testTag("ping-background-link").semantics { contentDescription = BACKGROUND_TITLE },
                )
            }
            if (!permitted) {
                VGap(FmSpace.x3)
                Text("Notifications are off for ForkMesh, so pings cannot reach this phone.", style = Fm.text.bodySmall.copy(color = c.warning))
                VGap(FmSpace.x2)
                AccountOutlinedButton(
                    label = "Allow notifications",
                    icon = "notifications_active_outlined",
                    onClick = {
                        val prefs = Prefs(context)
                        if (needsPermission(context) && !prefs.getBool(PERMISSION_ASKED_KEY + "/settings", false)) {
                            prefs.putBool(PERMISSION_ASKED_KEY + "/settings", true)
                            requestPermission(context)
                        } else {
                            openAppNotificationSettings(context)
                        }
                    },
                )
            } else if (enabled && !unrestricted) {
                VGap(FmSpace.x3)
                Text(
                    "Some phones stop background connections to save battery. If pings arrive late, let ForkMesh run unrestricted.",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
                VGap(FmSpace.x2)
                AccountOutlinedButton(
                    label = "Battery settings",
                    icon = "battery_saver_outlined",
                    onClick = { openBatterySettings(context) },
                )
            }
        }
    }

    private fun openAppNotificationSettings(context: Context) {
        val intent = Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
            .putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { context.startActivity(intent) }
    }

    private fun openBatterySettings(context: Context) {
        val intent = Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { context.startActivity(intent) }.onFailure {
            runCatching {
                context.startActivity(
                    Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${context.packageName}"))
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                )
            }
        }
    }
}
