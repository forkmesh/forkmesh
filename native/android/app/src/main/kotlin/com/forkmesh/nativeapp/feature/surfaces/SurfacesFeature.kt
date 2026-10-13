package com.forkmesh.nativeapp.feature.surfaces

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.graphics.drawable.Icon
import android.net.Uri
import android.os.Build
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.MainActivity
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import com.forkmesh.nativeapp.feature.composer.PhotoIntake
import com.forkmesh.nativeapp.feature.inbox.InboxFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.inbox.NotificationCategoryId
import com.forkmesh.nativeapp.inbox.NotificationPayload
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.navigation.AgentFilter
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.ShellQuickAction
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.surfaces.AgentApprovalInput
import com.forkmesh.nativeapp.surfaces.AgentRunCard
import com.forkmesh.nativeapp.surfaces.AgentRunInput
import com.forkmesh.nativeapp.surfaces.AgentRunState
import com.forkmesh.nativeapp.surfaces.AgentsSurface
import com.forkmesh.nativeapp.surfaces.InboxSurface
import com.forkmesh.nativeapp.surfaces.SharedContent
import com.forkmesh.nativeapp.surfaces.SurfaceSection
import com.forkmesh.nativeapp.surfaces.SurfaceSnapshot
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

object SurfacesFeature {
    const val RUNS_CHANNEL = "agent-runs"
    const val EVENTS_CHANNEL = "agent-events"
    private const val RUNNING_ID = 7401
    private const val SNAPSHOT_FILE = "surfaces/snapshot.json"

    private val needsYouState = MutableStateFlow(0)
    private val agentsState = MutableStateFlow(AgentsSurface.empty)
    private var appRef: App? = null
    private var lastRuns: Map<String, AgentRunState> = emptyMap()
    private val epoch = java.util.concurrent.atomic.AtomicInteger()
    private var controlListener: (() -> Unit)? = null
    private var opsQueue: com.forkmesh.nativeapp.agents.ops.ApprovalsQueue? = null
    var approvals: () -> List<AgentApprovalInput> = { emptyList() }

    val needsYou: StateFlow<Int> = needsYouState.asStateFlow()
    val agents: StateFlow<AgentsSurface> = agentsState.asStateFlow()

    fun install(app: App) {
        appRef = app
        ensureChannels(app)
        forgetProject()
        ProjectLifecycle.scope.launch {
            NavigationFeature.recents.combine(com.forkmesh.nativeapp.feature.market.PackageGateFeature.surfaces) { recents, _ -> recents }
                .collect { publishShortcuts(app, it) }
        }
        ProjectLifecycle.scope.launch {
            AppLifecycle.foreground.collect { foreground -> if (foreground) attachControl() }
        }
        ProjectLifecycle.scope.launch {
            AgentsFeature.listSignal.collect { refreshAsync() }
        }
        Services.get<NotificationService>()?.let { service ->
            ProjectLifecycle.scope.launch { service.notifications.collect { refreshAsync() } }
        }
        Services.get<AuthService>()?.let { auth ->
            ProjectLifecycle.scope.launch {
                auth.session.collect { if (it == null) signedOut(app) }
            }
        }
        ProjectLifecycle.onClose {
            forgetProject()
            controlListener?.let { listener ->
                runCatching { AgentsFeature.control.removeListener(listener) }
                opsQueue?.removeListener(listener)
            }
            controlListener = null
            opsQueue = null
            approvals = { emptyList() }
            app.getSystemService(NotificationManager::class.java)?.cancel(RUNNING_ID)
        }
    }

    private fun attachControl() {
        if (controlListener != null || !AgentsFeature.signedIn()) return
        val listener: () -> Unit = { AppLifecycle.scope.launch { refreshAsync() } }
        runCatching { AgentsFeature.control.addListener(listener) }.onSuccess { controlListener = listener }
        runCatching { AgentsFeature.control.refreshAsync() }
        runCatching {
            val queue = com.forkmesh.nativeapp.feature.agents.AgentOpsFeature.approvals
            queue.addListener(listener)
            approvals = {
                queue.approvals.map { AgentApprovalInput(it.id, it.sessionId, it.question.ifEmpty { it.title }, it.askedAtMs) }
            }
            opsQueue = queue
        }
    }

    private fun forgetProject() {
        epoch.incrementAndGet()
        lastRuns = emptyMap()
        agentsState.value = AgentsSurface.empty
        needsYouState.value = 0
    }

    private fun signedOut(context: Context) {
        val snapshot = SurfaceSnapshot.signedOut(System.currentTimeMillis())
        writeSnapshot(context, snapshot)
        needsYouState.value = 0
        agentsState.value = AgentsSurface.empty
        context.getSystemService(NotificationManager::class.java)?.cancel(RUNNING_ID)
    }

    suspend fun refreshAsync() {
        val context = appRef ?: return
        val started = epoch.get()
        val signedIn = Services.get<AuthService>()?.session?.value != null
        if (!signedIn) return
        val rows = runCatching { if (controlListener != null) AgentsFeature.control.rows else emptyList() }.getOrDefault(emptyList())
        val runs = rows.map { row ->
            AgentRunInput(
                id = row.id,
                title = row.title,
                repoOwner = row.repoOwner,
                repoName = row.repoName,
                status = row.status,
                isActive = row.isActive,
                provider = row.provider,
                model = row.model,
                detail = row.statusLabel,
                updatedAtMs = row.updatedAtMs,
                localId = row.localId,
                isDesktop = row.isDesktop,
                canStop = row.canStop,
                route = if (row.cloudSessionId.isNotEmpty() && row.repoOwner.isNotEmpty()) {
                    Route.LaunchedAgent(RepoRef(row.repoOwner, row.repoName), row.cloudSessionId)
                } else {
                    null
                },
            )
        }
        val agents = AgentsSurface.build(runs, approvals())
        val notifications = Services.get<NotificationService>()?.notifications?.value.orEmpty()
        val unread = notifications.count { it.isUnread }
        val snapshot = SurfaceSnapshot(System.currentTimeMillis(), true, agents, InboxSurface.build(unread, notifications))
        if (started != epoch.get()) return
        agentsState.value = agents
        needsYouState.value = agents.needsYou
        InboxFeature.setDesktopApprovals(opsQueue?.count ?: 0)
        val changed = withContext(Dispatchers.IO) { if (started == epoch.get()) writeSnapshot(context, snapshot) else emptySet() }
        if (started != epoch.get()) return
        if (SurfaceSection.Agents in changed) AgentsWidgetProvider.updateAll(context)
        if (SurfaceSection.Inbox in changed) InboxWidgetProvider.updateAll(context)
        updateRunningNotification(context, agents)
        postTransitions(context, agents)
    }

    private fun snapshotFile(context: Context) = File(context.filesDir, SNAPSHOT_FILE)

    fun readSnapshot(context: Context): SurfaceSnapshot? =
        runCatching { snapshotFile(context).takeIf { it.isFile }?.readText() }.getOrNull()?.let(SurfaceSnapshot::decode)

    private fun writeSnapshot(context: Context, snapshot: SurfaceSnapshot): Set<SurfaceSection> {
        val file = snapshotFile(context)
        val changed = snapshot.changedSections(readSnapshot(context))
        if (changed.isEmpty()) return changed
        return runCatching {
            file.parentFile?.mkdirs()
            val temp = File(file.parentFile, "${file.name}.tmp")
            temp.writeText(snapshot.encode())
            if (!temp.renameTo(file)) {
                file.writeText(snapshot.encode())
                temp.delete()
            }
            changed
        }.getOrDefault(emptySet())
    }

    private fun ensureChannels(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(RUNS_CHANNEL) == null) {
            manager.createNotificationChannel(
                NotificationChannel(RUNS_CHANNEL, "Running agents", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "A quiet notification while your agents run."
                    setShowBadge(false)
                },
            )
        }
        if (manager.getNotificationChannel(EVENTS_CHANNEL) == null) {
            manager.createNotificationChannel(
                NotificationChannel(EVENTS_CHANNEL, "Agent events", NotificationManager.IMPORTANCE_HIGH).apply {
                    description = "An agent finished, failed or needs your answer."
                },
            )
        }
    }

    fun routePendingIntent(context: Context, route: Route, code: Int): PendingIntent {
        val intent = NavigationFeature.routeIntent(context, route) ?: Intent(context, MainActivity::class.java)
        return PendingIntent.getActivity(context, code, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    }

    private fun allowed(context: Context): Boolean {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return false
        if (Build.VERSION.SDK_INT >= 33 &&
            context.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            return false
        }
        return manager.areNotificationsEnabled()
    }

    private fun updateRunningNotification(context: Context, agents: AgentsSurface) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        val active = agents.active
        if (active.isEmpty() || !allowed(context)) {
            manager.cancel(RUNNING_ID)
            return
        }
        val latest = active.first()
        val target = if (active.size == 1) latest.route else Route.Agents(AgentFilter.Running)
        val title = if (active.size == 1) latest.title else agents.summaryLine.replaceFirstChar { it.uppercase() }
        val text = if (active.size == 1) listOf(latest.state.label, latest.repo, latest.detail).filter { it.isNotEmpty() }.joinToString(" · ") else latest.title
        val builder = Notification.Builder(context, RUNS_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(title)
            .setContentText(text)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(Notification.CATEGORY_PROGRESS)
            .setTimeoutAfter(15 * 60 * 1000L)
            .setContentIntent(routePendingIntent(context, target, RUNNING_ID))
        if (latest.startedAtMs > 0) builder.setWhen(latest.startedAtMs).setUsesChronometer(true).setShowWhen(true)
        manager.notify(RUNNING_ID, builder.build())
    }

    private fun postTransitions(context: Context, agents: AgentsSurface) {
        val previous = lastRuns
        lastRuns = agents.runs.associate { it.id to it.state }
        if (previous.isEmpty() || AppLifecycle.foreground.value || !allowed(context)) return
        for (card in agents.runs) {
            val before = previous[card.id] ?: continue
            if (before == card.state) continue
            val category = when {
                card.state == AgentRunState.NeedsApproval -> NotificationCategoryId.AgentApproval
                before.isActive && card.state == AgentRunState.Completed -> NotificationCategoryId.AgentFinished
                before.isActive && card.state == AgentRunState.Failed -> NotificationCategoryId.AgentFailed
                else -> null
            } ?: continue
            postAgentEvent(context, card, category)
        }
    }

    private fun postAgentEvent(context: Context, card: AgentRunCard, category: NotificationCategoryId) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        val id = 7500 + (card.id.hashCode() and 0x3fff)
        val title = when (category) {
            NotificationCategoryId.AgentApproval -> "An agent needs you"
            NotificationCategoryId.AgentFailed -> "An agent run failed"
            else -> "An agent finished"
        }
        val body = listOf(card.title, card.repo, card.detail).filter { it.isNotEmpty() }.joinToString(" · ")
        val builder = Notification.Builder(context, EVENTS_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(title)
            .setContentText(body)
            .setStyle(Notification.BigTextStyle().bigText(body))
            .setAutoCancel(true)
            .setCategory(if (category.isTimeSensitive) Notification.CATEGORY_REMINDER else Notification.CATEGORY_STATUS)
            .setContentIntent(routePendingIntent(context, card.route, id))
        val ping = ForkNotification(
            id = card.id,
            kind = when (category) {
                NotificationCategoryId.AgentApproval -> "agent_approval"
                NotificationCategoryId.AgentFailed -> "agent_failed"
                else -> "agent_finished"
            },
            title = card.title,
            repo = card.repo,
            meta = mapOf("sessionId" to card.id, "approvalId" to card.approvalId.substringAfter('/')),
        )
        InboxFeature.addActions(context, builder, "", "agent-events", id, ping)
        manager.notify("agent-events", id, builder.build())
    }

    fun publishShortcuts(context: Context, recents: List<Route>) {
        val manager = context.getSystemService(ShortcutManager::class.java) ?: return
        val entries = ShellQuickAction.visible(recents, com.forkmesh.nativeapp.feature.market.PackageGateFeature.surfaces.value.routeKinds)
        val shortcuts = entries.take(manager.maxShortcutCountPerActivity.coerceAtLeast(1)).mapIndexed { index, item ->
            val intent = Intent(context, MainActivity::class.java).apply {
                action = NavigationFeature.ACTION_SHORTCUT
                putExtra(NavigationFeature.EXTRA_SHORTCUT, item.action.id)
                putExtra(NavigationFeature.EXTRA_PAYLOAD, item.payload)
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
            }
            ShortcutInfo.Builder(context, "${item.action.id}:${item.payload}")
                .setShortLabel(item.title.take(24))
                .setLongLabel(item.title.take(40))
                .setIcon(Icon.createWithResource(context, R.mipmap.ic_launcher))
                .setIntent(intent)
                .setRank(index)
                .build()
        }
        runCatching { manager.dynamicShortcuts = shortcuts }
    }

    fun onShare(intent: Intent?): Boolean {
        val context = appRef ?: return false
        val action = intent?.action ?: return false
        if (action != Intent.ACTION_SEND && action != Intent.ACTION_SEND_MULTIPLE) return false
        val text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT)?.toString()
        val subject = intent.getStringExtra(Intent.EXTRA_SUBJECT)
        val uris = ArrayList<Uri>()
        if (action == Intent.ACTION_SEND) {
            @Suppress("DEPRECATION")
            (intent.getParcelableExtra<Uri>(Intent.EXTRA_STREAM))?.let(uris::add)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM)?.let(uris::addAll)
        }
        intent.action = null
        val shared = SharedContent.from(text, subject)
        val images = uris.filter { uri -> context.contentResolver.getType(uri)?.startsWith("image/") == true }.take(SharedContent.MAX_IMAGES)
        if (shared == null && images.isEmpty()) return true
        AppLifecycle.scope.launch {
            val attachments = ArrayList<PromptAttachment>()
            for (uri in images) runCatching { PhotoIntake.fromUri(context, uri) }.getOrNull()?.let(attachments::add)
            if (attachments.isNotEmpty()) ComposerFeature.shareAttachments(attachments)
            NavigationFeature.compose(shared?.composeTarget ?: com.forkmesh.nativeapp.navigation.ComposeTarget())
        }
        return true
    }
}

@Composable
fun RunningAgentAccessory() {
    val agents by SurfacesFeature.agents.collectAsState()
    val active = agents.active
    if (active.isEmpty()) return
    val c = Fm.colors
    val single = active.size == 1
    val label = if (single) active.first().title else "${active.size} running"
    val detail = if (single) listOf(active.first().state.label, active.first().repo).filter { it.isNotEmpty() }.joinToString(" · ") else agents.summaryLine
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x1)
            .clip(RoundedCornerShape(24.dp))
            .background(c.bgRaised)
            .clickable { NavigationFeature.open(if (single) active.first().route else Route.Agents(AgentFilter.Running)) }
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2)
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = "$label, $detail"
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        androidx.compose.foundation.layout.Box(Modifier.size(8.dp).background(if (agents.needsYou > 0) c.warning else c.success, CircleShape))
        Spacer(Modifier.width(FmSpace.x2))
        FmIcon("auto_awesome", size = 18.dp, tint = c.textSecondary)
        Spacer(Modifier.width(FmSpace.x2))
        Text(label, Modifier.weight(1f), style = Fm.text.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(detail, style = Fm.text.bodySmall.copy(color = c.textTertiary), maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}
