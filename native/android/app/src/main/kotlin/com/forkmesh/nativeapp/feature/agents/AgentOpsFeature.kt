package com.forkmesh.nativeapp.feature.agents

import android.app.AlarmManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.agents.ops.AgentApprovalNotification
import com.forkmesh.nativeapp.agents.ops.AgentOpsClient
import com.forkmesh.nativeapp.agents.ops.AgentListFilter
import com.forkmesh.nativeapp.agents.ops.ApprovalsQueue
import com.forkmesh.nativeapp.agents.ops.UsageReminder
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.control.HubControlCalling
import com.forkmesh.nativeapp.feature.account.projects.UserPresence
import com.forkmesh.nativeapp.feature.push.PingNotifier
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object AgentOpsFeature {
    private val lock = Any()
    private lateinit var app: App

    private class Runtime(val hub: DesktopControlHub) {
        val client = AgentOpsClient(HubControlCalling(hub))
        val approvals = ApprovalsQueue(client) { block ->
            ProjectLifecycle.scope.launch(Dispatchers.IO) { block() }
        }

        fun dispose() {
            approvals.stop()
        }
    }

    @Volatile
    private var runtime: Runtime? = null

    fun install(app: App) {
        this.app = app
        ProjectLifecycle.onClose { reset() }
    }

    private fun runtime(): Runtime = synchronized(lock) {
        val hub = AgentsFeature.hub
        val existing = runtime
        if (existing != null && existing.hub === hub) return existing
        existing?.dispose()
        Runtime(hub).also {
            runtime = it
            it.approvals.start()
        }
    }

    val client: AgentOpsClient get() = runtime().client

    val approvals: ApprovalsQueue get() = runtime().approvals

    fun reset() {
        synchronized(lock) {
            runtime?.dispose()
            runtime = null
        }
    }

    fun approvalScreen(approvalId: String): Screen = ApprovalScreen(approvalId.substringAfterLast('/'))

    fun agentsScreen(filter: AgentListFilter): Screen = DesktopAgentsScreen(filter)

    fun needsYouScreen(): Screen = DesktopAgentsScreen(AgentListFilter.NeedsYou)

    fun worktreesScreen(owner: String, name: String): Screen = WorktreesScreen(owner, name)

    fun grantsScreen(): Screen = CapabilityGrantsScreen()

    fun recycleBinScreen(): Screen = RecycleBinScreen()

    suspend fun authenticate(messenger: FmMessenger, reason: String): Boolean = UserPresence.confirm(app, messenger, reason)

    suspend fun answerFromNotification(payload: Map<String, Any?>, actionWire: String, messenger: FmMessenger?): Boolean {
        val note = AgentApprovalNotification.from(payload) ?: return false
        val action = AgentApprovalNotification.Action.of(actionWire) ?: return false
        if (action.opensApp) return false
        if (action.requiresAuthentication && messenger != null && !authenticate(messenger, "Approve the agent's request")) return false
        val queue = approvals
        return withContext(Dispatchers.IO) { note.perform(action, queue) }
    }

    fun scheduleUsageReminders(context: Context, reminders: List<UsageReminder>, nowMs: Long) {
        if (!PingNotifier.allowed(context)) return
        val alarms = context.getSystemService(AlarmManager::class.java) ?: return
        for (reminder in reminders) {
            if (reminder.fireAtMs <= nowMs) continue
            val intent = Intent(context, UsageReminderReceiver::class.java)
                .putExtra(UsageReminderReceiver.EXTRA_ID, reminder.id)
                .putExtra(UsageReminderReceiver.EXTRA_TITLE, reminder.title)
                .putExtra(UsageReminderReceiver.EXTRA_BODY, reminder.body)
            val pending = PendingIntent.getBroadcast(
                context,
                reminder.id.hashCode(),
                intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
            )
            val fireAt = maxOf(reminder.fireAtMs, nowMs + 60_000)
            runCatching { alarms.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, fireAt, pending) }
        }
    }
}

class UsageReminderReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (!PingNotifier.allowed(context)) return
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(CHANNEL) == null) {
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL, "Agent usage", NotificationManager.IMPORTANCE_DEFAULT).apply {
                    description = "Tells you when an agent provider's usage limit has reset."
                },
            )
        }
        val id = intent.getStringExtra(EXTRA_ID) ?: return
        val launch = context.packageManager.getLaunchIntentForPackage(context.packageName)
        val open = launch?.let {
            PendingIntent.getActivity(context, id.hashCode(), it, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        }
        val notification = Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(intent.getStringExtra(EXTRA_TITLE) ?: "")
            .setContentText(intent.getStringExtra(EXTRA_BODY) ?: "")
            .setAutoCancel(true)
            .setGroup(CHANNEL)
            .apply { if (open != null) setContentIntent(open) }
            .build()
        manager.notify(id, id.hashCode(), notification)
    }

    companion object {
        const val CHANNEL = "agent-usage"
        const val EXTRA_ID = "id"
        const val EXTRA_TITLE = "title"
        const val EXTRA_BODY = "body"
    }
}
