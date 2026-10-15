package com.forkmesh.nativeapp.feature.push

import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.push.DIRECT_MESSAGES_TOPIC
import com.forkmesh.nativeapp.push.UnreadDirectMessage
import com.forkmesh.nativeapp.push.directMessageAlert
import android.content.Context
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.NotificationPage
import com.forkmesh.nativeapp.model.PingSettings
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.push.BackgroundLinkKeys
import com.forkmesh.nativeapp.push.PingAlertLedger
import com.forkmesh.nativeapp.chat.ChatAccountCalling
import com.forkmesh.nativeapp.push.DirectMessageEvent
import com.forkmesh.nativeapp.push.DirectMessagePreviews
import com.forkmesh.nativeapp.push.NotificationContent
import com.forkmesh.nativeapp.push.pingAlertsHeld
import com.forkmesh.nativeapp.security.SecretStore

object PingAlerts {
    private const val SETTINGS_TTL_MS = 15 * 60_000L

    private class CachedSettings(val settings: PingSettings?, val at: Long)

    private val ledgers = HashMap<String, PingAlertLedger>()
    private val settingsCache = HashMap<String, CachedSettings>()

    private fun ledger(prefs: Prefs, projectId: String): PingAlertLedger =
        ledgers.getOrPut(projectId) { PingAlertLedger.fromJson(prefs.getString(BackgroundLinkKeys.ledger(projectId))) }

    private fun save(prefs: Prefs, projectId: String, ledger: PingAlertLedger) {
        prefs.putString(BackgroundLinkKeys.ledger(projectId), ledger.toJson())
    }

    fun prime(context: Context, projectId: String, restart: Boolean) {
        synchronized(this) {
            val prefs = Prefs(context)
            val ledger = ledger(prefs, projectId)
            val now = System.currentTimeMillis()
            if (restart) ledger.restart(now) else ledger.prime(now)
            save(prefs, projectId, ledger)
        }
    }

    fun forget(context: Context, projectId: String) {
        synchronized(this) {
            ledgers.remove(projectId)
            settingsCache.remove(projectId)
            Prefs(context).remove(BackgroundLinkKeys.ledger(projectId))
        }
    }

    fun settingsFor(projectId: String, api: ApiService, node: String): PingSettings? {
        val now = System.currentTimeMillis()
        synchronized(this) {
            settingsCache[projectId]?.takeIf { now - it.at < SETTINGS_TTL_MS }?.let { return it.settings }
        }
        val fresh = runCatching { api.pingSettings(node) }.getOrNull()
        synchronized(this) {
            if (fresh != null) settingsCache[projectId] = CachedSettings(fresh, now)
            return fresh ?: settingsCache[projectId]?.settings
        }
    }

    fun invalidateSettings(projectId: String) {
        synchronized(this) { settingsCache.remove(projectId) }
    }

    fun deliver(
        context: Context,
        projectId: String,
        projectLabel: String,
        multipleProjects: Boolean,
        rows: List<ForkNotification>,
        settings: PingSettings?,
    ): Int {
        val decision = synchronized(this) {
            val prefs = Prefs(context)
            val ledger = ledger(prefs, projectId)
            val result = ledger.decide(rows, settings, System.currentTimeMillis())
            save(prefs, projectId, ledger)
            result
        }
        for (id in decision.cancel) PingNotifier.cancel(context, projectId, id)
        for (ping in decision.post) PingNotifier.post(context, projectId, projectLabel, multipleProjects, ping)
        return decision.post.size
    }

    fun notificationContent(context: Context): NotificationContent =
        NotificationContent.of(Prefs(context).getString(NotificationContent.PREFS_KEY))

    fun setNotificationContent(context: Context, content: NotificationContent) {
        Prefs(context).putString(NotificationContent.PREFS_KEY, content.wire)
    }

    fun deliverDirectMessage(
        context: Context,
        projectId: String,
        projectLabel: String,
        multipleProjects: Boolean,
        api: ApiService,
        node: String,
        event: DirectMessageEvent? = null,
        secrets: SecretStore? = null,
    ) {
        val calling = ChatAccountCalling { method, path, body -> api.accountApiCall(method, path, body) }
        if (event != null) DirectMessagePreviews.confirmDelivery(calling, event)
        if (AppLifecycle.foreground.value) return
        val settings = if (node.isEmpty()) null else settingsFor(projectId, api, node)
        val now = System.currentTimeMillis()
        if (pingAlertsHeld(settings, now)) return
        val content = notificationContent(context)
        if (event != null && secrets != null && content != NotificationContent.NOTHING) {
            val preview = DirectMessagePreviews.fetch(calling, event, secrets)
            if (preview != null) {
                val notice = DirectMessagePreviews.notice(preview, content)
                val avatarPng = if (notice.sender.isEmpty()) "" else runCatching { api.accountAvatarPng(notice.sender) }.getOrDefault("")
                PingNotifier.postDirectMessagePreview(context, projectId, projectLabel, multipleProjects, preview, notice, content, avatarPng)
                return
            }
        }
        val unread = runCatching { api.unreadDirectMessage() }.getOrDefault(UnreadDirectMessage())
        val matches = event == null || unread.conversationId == event.conversationId
        val sender = if (content == NotificationContent.NOTHING || !matches) "" else unread.sender
        val alert = directMessageAlert(
            DIRECT_MESSAGES_TOPIC, settings, now, projectId, projectLabel, multipleProjects,
            sender, event?.conversationId ?: unread.conversationId,
        ) ?: return
        val avatarPng = if (alert.sender.isEmpty()) "" else runCatching { api.accountAvatarPng(alert.sender) }.getOrDefault("")
        PingNotifier.postDirectMessage(context, alert, avatarPng)
    }

    fun fetchAndDeliver(context: Context, projectId: String, projectLabel: String, multipleProjects: Boolean, api: ApiService, node: String): Boolean {
        if (node.isEmpty()) return false
        val page = runCatching { api.notifications(node) }.getOrNull() ?: return false
        val merged = runCatching { PingCache.get(context).save(projectId, node, page) }.getOrDefault(page)
        handOff(projectId, node, merged)
        val settings = settingsFor(projectId, api, node)
        deliver(context, projectId, projectLabel, multipleProjects, page.notifications, settings)
        return true
    }

    private fun handOff(projectId: String, node: String, page: NotificationPage) {
        val service = Services.get<NotificationService>() ?: return
        if (service.project != projectId) return
        runCatching { service.accept(node, page) }
    }
}
