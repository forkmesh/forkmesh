package com.forkmesh.nativeapp.feature.push

import com.forkmesh.nativeapp.chat.MESH_ALERT_HREF
import com.forkmesh.nativeapp.chat.MeshAlert
import com.forkmesh.nativeapp.chat.shown
import com.forkmesh.nativeapp.push.DirectMessageAlert
import com.forkmesh.nativeapp.push.DIRECT_MESSAGE_HREF
import com.forkmesh.nativeapp.push.DIRECT_MESSAGE_PING_ID
import com.forkmesh.nativeapp.push.DirectMessagePreview
import com.forkmesh.nativeapp.push.DirectMessagePreviews
import com.forkmesh.nativeapp.push.NotificationContent
import com.forkmesh.nativeapp.push.PreviewLine
import com.forkmesh.nativeapp.push.PreviewNotice
import android.app.Person
import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.drawable.Icon
import android.media.AudioAttributes
import android.media.RingtoneManager
import android.os.Build
import android.util.Base64
import com.forkmesh.nativeapp.MainActivity
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.push.PushTarget
import com.forkmesh.nativeapp.push.pingAlarm
import com.forkmesh.nativeapp.push.pingIcon
import com.forkmesh.nativeapp.push.pingNotificationTitle
import com.forkmesh.nativeapp.push.withPingIcon

object PingNotifier {
    const val PINGS_CHANNEL = "pings"
    const val LINK_CHANNEL = "background-link"
    const val ALARMS_CHANNEL = "desktop-alarms"
    const val MESH_CHANNEL = "mesh-messages"
    const val MESH_TAG = "mesh"
    const val MESH_NOTIFICATION_ID = 7304
    const val ACTION_OPEN = "com.forkmesh.push.OPEN"
    const val LINK_NOTIFICATION_ID = 7301
    const val PUBLIC_PING_BODY = "New notification"

    fun ensureChannels(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(PINGS_CHANNEL) == null) {
            manager.createNotificationChannel(
                NotificationChannel(PINGS_CHANNEL, "Pings", NotificationManager.IMPORTANCE_DEFAULT).apply {
                    description = "Mentions, code pushes, reviews and everything else in your Pings inbox."
                },
            )
        }
        if (manager.getNotificationChannel(ALARMS_CHANNEL) == null) {
            val sound = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM)
                ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
            manager.createNotificationChannel(
                NotificationChannel(ALARMS_CHANNEL, "Desktop alarms", NotificationManager.IMPORTANCE_HIGH).apply {
                    description = "Rings like an alarm when your desktop loses power and switches to battery."
                    setSound(
                        sound,
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_ALARM)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build(),
                    )
                    enableVibration(true)
                    vibrationPattern = longArrayOf(0, 600, 400, 600, 400, 600)
                    setBypassDnd(true)
                },
            )
        }
        if (manager.getNotificationChannel(MESH_CHANNEL) == null) {
            manager.createNotificationChannel(
                NotificationChannel(MESH_CHANNEL, "Nearby #mesh messages", NotificationManager.IMPORTANCE_HIGH).apply {
                    description = "Messages from devices near you in #mesh, delivered over Wi-Fi and Bluetooth with no internet."
                },
            )
        }
        if (manager.getNotificationChannel(LINK_CHANNEL) == null) {
            manager.createNotificationChannel(
                NotificationChannel(LINK_CHANNEL, "Background connection", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "Shown while ForkMesh stays connected to deliver pings with the app closed."
                    setShowBadge(false)
                },
            )
        }
    }

    fun allowed(context: Context): Boolean {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return false
        if (Build.VERSION.SDK_INT >= 33 &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            return false
        }
        return manager.areNotificationsEnabled()
    }

    fun post(context: Context, projectId: String, projectLabel: String, multipleProjects: Boolean, ping: ForkNotification) {
        if (!allowed(context)) return
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        ensureChannels(context)
        val target = PushTarget.forPing(projectId, ping)
        val (title, body) = pingNotificationTitle(ping.title, ping.body, projectLabel, multipleProjects)
        val alarm = pingAlarm(ping.kind, ping.meta).isNotEmpty()
        val builder = Notification.Builder(context, if (alarm) ALARMS_CHANNEL else PINGS_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(withPingIcon(title, pingIcon(ping.kind, ping.meta)))
            .setContentText(body)
            .setStyle(Notification.BigTextStyle().bigText(body))
            .setAutoCancel(true)
            .setCategory(if (alarm) Notification.CATEGORY_ALARM else Notification.CATEGORY_SOCIAL)
            .setContentIntent(openIntent(context, target))
            .setVisibility(Notification.VISIBILITY_PRIVATE)
            .setPublicVersion(publicVersion(context, if (alarm) ALARMS_CHANNEL else PINGS_CHANNEL, PUBLIC_PING_BODY))
        if (!alarm) builder.setGroup("pings/$projectId")
        if (ping.ts > 0) builder.setWhen(ping.ts).setShowWhen(true)
        com.forkmesh.nativeapp.feature.inbox.InboxFeature.addActions(context, builder, projectId, projectId, target.notificationId(), ping)
        val notification = builder.build()
        if (alarm) notification.flags = notification.flags or Notification.FLAG_INSISTENT
        manager.notify(projectId, target.notificationId(), notification)
    }

    fun postDirectMessage(context: Context, alert: DirectMessageAlert, avatarPng: String = "") {
        if (!allowed(context)) return
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        ensureChannels(context)
        val target = alert.target
        val builder = Notification.Builder(context, PINGS_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(alert.title)
            .setContentText(alert.body)
            .setAutoCancel(true)
            .setCategory(Notification.CATEGORY_MESSAGE)
            .setGroup("pings/${target.projectId}")
            .setWhen(System.currentTimeMillis())
            .setShowWhen(true)
            .setVisibility(Notification.VISIBILITY_PRIVATE)
            .setPublicVersion(publicVersion(context, PINGS_CHANNEL, DirectMessagePreviews.GENERIC_BODY))
            .setContentIntent(openIntent(context, target))
        avatarBitmap(avatarPng)?.let { builder.setLargeIcon(Icon.createWithBitmap(it)) }
        manager.notify(target.projectId, target.notificationId(), builder.build())
    }

    private val previewLines = HashMap<String, List<PreviewLine>>()

    fun postDirectMessagePreview(
        context: Context,
        projectId: String,
        projectLabel: String,
        multipleProjects: Boolean,
        preview: DirectMessagePreview,
        notice: PreviewNotice,
        content: NotificationContent,
        avatarPng: String = "",
    ) {
        if (!allowed(context)) return
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        ensureChannels(context)
        val target = PushTarget(projectId, "$DIRECT_MESSAGE_PING_ID/${preview.conversationId}", "$DIRECT_MESSAGE_HREF/${preview.conversationId}")
        val id = target.notificationId()
        val key = "$projectId\n${preview.conversationId}"
        val active = runCatching { manager.activeNotifications.any { it.tag == projectId && it.id == id } }.getOrDefault(false)
        val line = PreviewLine(preview.messageId, notice.sender, notice.messageText, preview.timestamp.takeIf { it > 0 } ?: System.currentTimeMillis())
        val lines = synchronized(previewLines) {
            val merged = DirectMessagePreviews.merge(if (active) previewLines[key].orEmpty() else emptyList(), line) ?: return
            previewLines[key] = merged
            merged
        }
        val hidden = publicVersion(context, PINGS_CHANNEL, DirectMessagePreviews.GENERIC_BODY)
        val body = if (content == NotificationContent.NAME_ONLY && lines.size > 1) "${lines.size} new messages" else notice.body
        val builder = Notification.Builder(context, PINGS_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(notice.title)
            .setContentText(body)
            .setAutoCancel(true)
            .setCategory(Notification.CATEGORY_MESSAGE)
            .setGroup("pings/$projectId")
            .setWhen(line.timestamp)
            .setShowWhen(true)
            .setVisibility(Notification.VISIBILITY_PRIVATE)
            .setPublicVersion(hidden)
            .setContentIntent(openIntent(context, target))
        if (multipleProjects && projectLabel.isNotEmpty()) builder.setSubText(projectLabel)
        val avatar = avatarBitmap(avatarPng)?.let { Icon.createWithBitmap(it) }
        avatar?.let { builder.setLargeIcon(it) }
        if (content == NotificationContent.NAME_AND_MESSAGE && Build.VERSION.SDK_INT >= 28) {
            val style = Notification.MessagingStyle(Person.Builder().setName("You").build())
            for (item in lines) {
                val person = Person.Builder().setName(item.sender.ifEmpty { notice.title })
                if (avatar != null && item.sender == notice.sender) person.setIcon(avatar)
                style.addMessage(Notification.MessagingStyle.Message(item.text, item.timestamp, person.build()))
            }
            builder.setStyle(style)
        } else if (content == NotificationContent.NAME_AND_MESSAGE) {
            builder.setStyle(Notification.BigTextStyle().bigText(lines.joinToString("\n") { it.text }))
        }
        manager.notify(projectId, id, builder.build())
    }

    fun postMesh(context: Context, alert: MeshAlert, whenMs: Long) {
        if (!allowed(context)) return
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        ensureChannels(context)
        val target = PushTarget("", MESH_TAG, MESH_ALERT_HREF)
        val shown = alert.shown(PingAlerts.notificationContent(context))
        val builder = Notification.Builder(context, MESH_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(shown.title)
            .setContentText(shown.body)
            .setStyle(Notification.BigTextStyle().bigText(shown.body))
            .setAutoCancel(true)
            .setCategory(Notification.CATEGORY_MESSAGE)
            .setWhen(whenMs)
            .setShowWhen(true)
            .setVisibility(Notification.VISIBILITY_PRIVATE)
            .setPublicVersion(publicVersion(context, MESH_CHANNEL, DirectMessagePreviews.GENERIC_BODY))
            .setContentIntent(openIntent(context, target))
        manager.notify(MESH_TAG, MESH_NOTIFICATION_ID, builder.build())
    }

    private fun publicVersion(context: Context, channel: String, body: String): Notification =
        Notification.Builder(context, channel)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(DirectMessagePreviews.GENERIC_TITLE)
            .setContentText(body)
            .build()

    fun cancelMesh(context: Context) {
        context.getSystemService(NotificationManager::class.java)?.cancel(MESH_TAG, MESH_NOTIFICATION_ID)
    }

    private fun avatarBitmap(avatarPng: String): Bitmap? {
        if (avatarPng.isEmpty()) return null
        return try {
            val bytes = Base64.decode(avatarPng, Base64.DEFAULT)
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        } catch (_: IllegalArgumentException) {
            null
        }
    }

    fun cancel(context: Context, projectId: String, pingId: String) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        manager.cancel(projectId, PushTarget(projectId, pingId, "").notificationId())
    }

    fun cancelProject(context: Context, projectId: String) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        manager.activeNotifications.filter { it.tag == projectId }.forEach { manager.cancel(it.tag, it.id) }
    }

    fun openIntent(context: Context, target: PushTarget?): PendingIntent {
        val intent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP
            if (target != null) {
                action = ACTION_OPEN
                putExtra(PushTarget.EXTRA_PROJECT, target.projectId)
                putExtra(PushTarget.EXTRA_PING, target.pingId)
                putExtra(PushTarget.EXTRA_HREF, target.href)
                putExtra(PushTarget.EXTRA_AGENT, target.agent)
                target.rowExtra()?.let { putExtra(PushTarget.EXTRA_ROW, it) }
            }
        }
        val code = target?.notificationId() ?: 0
        return PendingIntent.getActivity(context, code, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    }

    fun linkNotification(context: Context, text: String, title: String = "Connected for pings"): Notification {
        ensureChannels(context)
        return Notification.Builder(context, LINK_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(title)
            .setContentText(text.ifEmpty { "Listening for pings" })
            .setOngoing(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setContentIntent(openIntent(context, null))
            .build()
    }
}
