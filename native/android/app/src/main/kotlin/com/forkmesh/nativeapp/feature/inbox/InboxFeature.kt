package com.forkmesh.nativeapp.feature.inbox

import android.app.Notification
import android.app.PendingIntent
import android.app.RemoteInput
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.graphics.drawable.Icon
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.MainActivity
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.feature.chat.ChatDepth
import com.forkmesh.nativeapp.feature.chat.ChatFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.push.PingCache
import com.forkmesh.nativeapp.inbox.InboxClient
import com.forkmesh.nativeapp.inbox.InboxItem
import com.forkmesh.nativeapp.inbox.InboxModel
import com.forkmesh.nativeapp.inbox.LocalPingLog
import com.forkmesh.nativeapp.inbox.NotificationActionId
import com.forkmesh.nativeapp.inbox.NotificationActionRouter
import com.forkmesh.nativeapp.inbox.NotificationCategoryId
import com.forkmesh.nativeapp.inbox.NotificationCommand
import com.forkmesh.nativeapp.inbox.NotificationPayload
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

interface AgentApprovalDecider {
    suspend fun decide(sessionId: String, approvalId: String, approve: Boolean)
}

interface AgentSessionActions {
    suspend fun continueRun(sessionId: String, text: String)
    suspend fun mergeAndCleanUp(sessionId: String)
    suspend fun retry(sessionId: String)
}

class ApprovalAnswer(val message: String, val ok: Boolean)

fun interface NotificationReplySender {
    suspend fun reply(channel: String, text: String)
}

object InboxFeature {
    const val ACTION_NOTIFICATION = "com.forkmesh.inbox.ACTION"
    const val EXTRA_ACTION = "fmAction"
    const val EXTRA_CATEGORY = "fmCategory"
    const val EXTRA_PROJECT = "fmProject"
    const val EXTRA_NOTIFICATION_TAG = "fmTag"
    const val EXTRA_NOTIFICATION_ID = "fmId"
    const val REMOTE_INPUT_KEY = "fmText"

    private val unreadState = MutableStateFlow(-1)
    private val pendingApprovalsState = MutableStateFlow(0)
    private val itemsTick = MutableStateFlow(0)
    private val filterState = MutableStateFlow(InboxFilter.All)
    private val actionProblemState = MutableStateFlow<String?>(null)
    private var modelInstance: InboxModel? = null
    private var desktopApprovals = 0
    private var appRef: App? = null

    val unread: StateFlow<Int> = unreadState.asStateFlow()
    val pendingApprovals: StateFlow<Int> = pendingApprovalsState.asStateFlow()
    val changes: StateFlow<Int> = itemsTick.asStateFlow()
    val filter: StateFlow<InboxFilter> = filterState.asStateFlow()
    val actionProblem: StateFlow<String?> = actionProblemState.asStateFlow()

    fun reportActionProblem(problem: String) {
        actionProblemState.value = problem
    }

    fun clearActionProblem() {
        actionProblemState.value = null
    }

    var approvals: AgentApprovalDecider? = OpsApprovals
    var sessionActions: AgentSessionActions? = null
    var replySender: NotificationReplySender = NotificationReplySender { channel, text ->
        val relay = ChatFeature.relay ?: error("Chat is not available.")
        withContext(Dispatchers.IO) {
            relay.switchConversation(channel)
            if (!relay.sendMessage(text)) error("Chat is offline.")
        }
    }

    val model: InboxModel? get() = modelInstance

    fun install(app: App) {
        appRef = app
        unreadState.value = -1
        pendingApprovalsState.value = 0
        val api = Services.get<ApiService>()
        val base = Services.get<PrefStore>()
        if (api == null || base == null) {
            modelInstance = null
            return
        }
        val prefs = Services.get<ProjectScope>()?.prefs(base) ?: base
        val auth = Services.get<AuthService>()
        val created = InboxModel(
            client = InboxClient { method, path, body -> api.accountApiCall(method, path, body) },
            node = { auth?.session?.value?.nodeName.orEmpty() },
            store = prefs,
            localPings = LocalPingLog(prefs),
            snapshots = PingCache.get(app),
            project = Services.get<ProjectScope>()?.projectId.orEmpty(),
        )
        created.onChanged = { Services.get<NotificationService>()?.refreshInBackground(silent = true) }
        modelInstance = created
        publish()
        val chat = ChatDepth.hub
        chat?.pings = created.localPings
        chat?.onPingsChanged = {
            created.refreshLocal()
            if (modelInstance === created) publish()
        }
        ProjectLifecycle.onClose {
            if (chat != null && chat.pings === created.localPings) {
                chat.pings = null
                chat.onPingsChanged = null
            }
            modelInstance = null
            sessionActions = null
            desktopApprovals = 0
        }
        val notifications = Services.get<NotificationService>() ?: return
        ProjectLifecycle.scope.launch {
            notifications.notifications.collect { if (it.isNotEmpty() || created.loaded) refresh() }
        }
    }

    fun tab(): Screen = InboxScreen(null)

    fun screen(filter: InboxFilter): Screen = InboxScreen(filter)

    fun itemScreen(item: InboxItem): Screen = InboxItemScreen(item)

    fun insertPing(ping: ForkNotification): InboxItem? {
        val model = modelInstance ?: return null
        val item = model.insert(ping) ?: return null
        publish()
        return item
    }

    fun selectFilter(filter: InboxFilter) {
        filterState.value = filter
    }

    internal fun setFilter(filter: InboxFilter) {
        filterState.value = filter
    }

    internal fun publish() {
        val model = modelInstance ?: return
        if (model.loaded) unreadState.value = model.unreadTotal
        pendingApprovalsState.value = (model.unreadCounts[InboxFilter.Approvals] ?: 0) + desktopApprovals
        itemsTick.value = itemsTick.value + 1
    }

    fun setDesktopApprovals(count: Int) {
        desktopApprovals = count
        pendingApprovalsState.value = (modelInstance?.unreadCounts?.get(InboxFilter.Approvals) ?: 0) + count
    }

    suspend fun refresh() {
        val model = modelInstance ?: return
        withContext(Dispatchers.IO) { model.load() }
        publish()
    }

    fun recordLocalFailure(surface: String, title: String, message: String) {
        val model = modelInstance ?: return
        model.localPings.record(surface, title, message) ?: return
        model.refreshLocal()
        publish()
    }

    suspend fun decide(sessionId: String, approvalId: String, approve: Boolean): ApprovalAnswer {
        val handler = approvals ?: return ApprovalAnswer("Approvals are answered on the desktop for now.", false)
        return try {
            handler.decide(sessionId, approvalId, approve)
            ApprovalAnswer(if (approve) "Approved." else "Denied.", true)
        } catch (error: Exception) {
            ApprovalAnswer(error.message ?: "The desktop did not answer.", false)
        }
    }

    fun payload(ping: ForkNotification): NotificationPayload {
        val category = NotificationCategoryId.of(ping.meta.str("category")) ?: NotificationCategoryId.forPing(ping.kind, ping.meta.str("topic"))
        return NotificationPayload(
            category = category,
            alertId = ping.id,
            href = ping.href,
            topic = ping.meta.str("topic"),
            sessionId = ping.meta.str("sessionId"),
            approvalId = ping.meta.str("approvalId"),
            repo = ping.repo,
            number = ping.meta.int("number"),
            channel = ping.meta.str("channel"),
            runId = ping.meta.str("runId"),
        )
    }

    fun addActions(context: Context, builder: Notification.Builder, projectId: String, tag: String, id: Int, ping: ForkNotification) {
        val payload = payload(ping)
        payload.category.actions.filter { it != NotificationActionId.Open }.forEachIndexed { index, action ->
            val intent = actionIntent(context, action, payload, projectId, tag, id, ping.kind)
            val code = (id * 31 + index + 1)
            val pending = if (action.opensApp) {
                PendingIntent.getActivity(context, code, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            } else {
                PendingIntent.getBroadcast(context, code, intent, PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            }
            val built = Notification.Action.Builder(Icon.createWithResource(context, R.drawable.ic_stat_ping), action.title, pending)
            if (action.isTextInput) {
                built.addRemoteInput(RemoteInput.Builder(REMOTE_INPUT_KEY).setLabel(action.title).build())
            }
            builder.addAction(built.build())
        }
        if (payload.category.isTimeSensitive) builder.setCategory(Notification.CATEGORY_REMINDER)
    }

    private fun actionIntent(
        context: Context,
        action: NotificationActionId,
        payload: NotificationPayload,
        projectId: String,
        tag: String,
        id: Int,
        kind: String,
    ): Intent {
        val intent = if (action.opensApp) {
            Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        } else {
            Intent(context, NotificationActionReceiver::class.java)
        }
        intent.action = ACTION_NOTIFICATION
        intent.putExtra(EXTRA_ACTION, action.id)
        intent.putExtra(EXTRA_CATEGORY, payload.category.id)
        intent.putExtra(EXTRA_PROJECT, projectId)
        intent.putExtra(EXTRA_NOTIFICATION_TAG, tag)
        intent.putExtra(EXTRA_NOTIFICATION_ID, id)
        payload.fields(kind).forEach { (key, value) -> intent.putExtra("fm.$key", value) }
        return intent
    }

    private fun payloadFrom(intent: Intent): NotificationPayload =
        NotificationPayload.from(intent.getStringExtra(EXTRA_CATEGORY), NotificationPayload.keys.associateWith { intent.getStringExtra("fm.$it") })

    fun onIntent(intent: Intent?): Boolean {
        if (intent?.action != ACTION_NOTIFICATION) return false
        val payload = payloadFrom(intent)
        val command = NotificationActionRouter.command(intent.getStringExtra(EXTRA_ACTION), payload, null)
        cancel(intent)
        intent.action = null
        AppLifecycle.scope.launch { runInApp(command) }
        return true
    }

    private suspend fun runInApp(command: NotificationCommand) {
        when (command) {
            is NotificationCommand.Open -> command.route?.let(NavigationFeature::open)
            is NotificationCommand.Compose -> NavigationFeature.compose(command.target)
            is NotificationCommand.Approve -> NavigationFeature.open(Route.Approval("${command.sessionId}/${command.approvalId}"))
            is NotificationCommand.Merge -> NavigationFeature.open(Route.Agent(command.sessionId))
            is NotificationCommand.ApprovePull -> NavigationFeature.open(Route.Pull(command.repo, command.number, com.forkmesh.nativeapp.navigation.PullTab.Conversation))
            else -> runInBackground(command)
        }
    }

    internal suspend fun runInBackground(command: NotificationCommand): String? = try {
        when (command) {
            is NotificationCommand.MarkRead -> {
                val api = Services.get<ApiService>() ?: error("Sign in first.")
                val node = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()
                withContext(Dispatchers.IO) { api.markNotificationsRead(node, listOf(command.alertId)) }
                Services.get<NotificationService>()?.refreshInBackground(silent = true)
                null
            }
            is NotificationCommand.Reply -> {
                replySender.reply(command.channel, command.text)
                null
            }
            is NotificationCommand.Deny -> decide(command.sessionId, command.approvalId, false).let { if (it.ok) null else it.message }
            is NotificationCommand.Approve -> decide(command.sessionId, command.approvalId, true).let { if (it.ok) null else it.message }
            is NotificationCommand.ContinueRun -> {
                (sessionActions ?: error("Continue this run from the Agents tab.")).continueRun(command.sessionId, command.text)
                null
            }
            is NotificationCommand.Retry -> {
                (sessionActions ?: error("Retry this run from the Agents tab.")).retry(command.sessionId)
                null
            }
            is NotificationCommand.Merge -> {
                (sessionActions ?: error("Merge this run from the Agents tab.")).mergeAndCleanUp(command.sessionId)
                null
            }
            is NotificationCommand.Ignore -> null
            else -> null
        }
    } catch (error: Exception) {
        error.message ?: "That did not work."
    }

    internal fun cancel(intent: Intent) {
        val context = appRef ?: return
        val manager = context.getSystemService(android.app.NotificationManager::class.java) ?: return
        val id = intent.getIntExtra(EXTRA_NOTIFICATION_ID, 0)
        if (id != 0) manager.cancel(intent.getStringExtra(EXTRA_NOTIFICATION_TAG), id)
    }

    internal fun payloadFromIntent(intent: Intent): NotificationPayload = payloadFrom(intent)
}

private object OpsApprovals : AgentApprovalDecider {
    override suspend fun decide(sessionId: String, approvalId: String, approve: Boolean) {
        if (approve) error("Open the approval to approve it; approving asks you to confirm it is you.")
        val wire = if (approve) "agent-approval.approve" else "agent-approval.deny"
        val payload = mapOf("category" to "agent-approval", "approvalId" to approvalId, "sessionId" to sessionId)
        val done = com.forkmesh.nativeapp.feature.agents.AgentOpsFeature.answerFromNotification(payload, wire, null)
        if (!done) error("The desktop did not take the answer. Open the agent to answer it.")
    }
}

class NotificationActionReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != InboxFeature.ACTION_NOTIFICATION) return
        val payload = InboxFeature.payloadFromIntent(intent)
        val text = RemoteInput.getResultsFromIntent(intent)?.getCharSequence(InboxFeature.REMOTE_INPUT_KEY)?.toString()
        val command = NotificationActionRouter.command(intent.getStringExtra(InboxFeature.EXTRA_ACTION), payload, text)
        val pending = goAsync()
        AppLifecycle.scope.launch {
            try {
                val problem = InboxFeature.runInBackground(command)
                if (problem == null) {
                    InboxFeature.clearActionProblem()
                    InboxFeature.cancel(intent)
                } else {
                    InboxFeature.reportActionProblem(problem)
                }
            } finally {
                pending.finish()
            }
        }
    }
}
