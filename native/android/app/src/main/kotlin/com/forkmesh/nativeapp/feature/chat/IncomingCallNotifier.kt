package com.forkmesh.nativeapp.feature.chat

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Person
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.RingtoneManager
import android.os.Build
import com.forkmesh.nativeapp.MainActivity
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.chat.DirectCallInvite
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.feature.push.PingNotifier
import com.forkmesh.nativeapp.platform.IntentTokens
import com.forkmesh.nativeapp.push.CallRings

object IncomingCallNotifier {
    const val CHANNEL = "incoming-calls"
    const val RING_CHANNEL = "incoming-call-rings"
    const val NOTIFICATION_ID = 7480
    const val RING_NOTIFICATION_ID = 7484
    const val ACTION_SHOW = "com.forkmesh.call.SHOW"
    const val ACTION_ACCEPT = "com.forkmesh.call.ACCEPT"
    const val ACTION_DECLINE = "com.forkmesh.call.DECLINE"
    const val EXTRA_CALL = "com.forkmesh.call.ID"

    fun ensureChannel(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(CHANNEL) != null) return
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL, "Incoming calls", NotificationManager.IMPORTANCE_HIGH).apply {
                description = "Rings when someone calls you in a direct message while ForkMesh is running."
                setSound(null, null)
                enableVibration(false)
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            },
        )
    }

    private fun ensureRingChannel(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(RING_CHANNEL) != null) return
        val ringtone = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
        manager.createNotificationChannel(
            NotificationChannel(RING_CHANNEL, "Incoming call alerts", NotificationManager.IMPORTANCE_HIGH).apply {
                description = "Rings when someone calls you in a direct message while ForkMesh is in the background."
                setSound(
                    ringtone,
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build(),
                )
                enableVibration(true)
                vibrationPattern = longArrayOf(0, 1000, 1000)
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            },
        )
    }

    fun postRing(context: Context, projectId: String, conversation: String, partner: String) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        if (!allowed(context, manager)) return
        ensureRingChannel(context)
        val open = PingNotifier.openIntent(context, CallRings.target(projectId, conversation))
        val builder = Notification.Builder(context, RING_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(CallRings.title(partner))
            .setContentText(CallRings.text(partner))
            .setCategory(Notification.CATEGORY_CALL)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setAutoCancel(true)
            .setShowWhen(false)
            .setTimeoutAfter(CallRings.RING_MS)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, "Answer", open).build())
        if (fullScreenAllowed(manager)) builder.setFullScreenIntent(open, true)
        val notification = builder.build()
        notification.flags = notification.flags or Notification.FLAG_INSISTENT
        runCatching { manager.notify(RING_NOTIFICATION_ID, notification) }
    }

    fun cancelRing(context: Context) {
        context.getSystemService(NotificationManager::class.java)?.cancel(RING_NOTIFICATION_ID)
    }

    private fun allowed(context: Context, manager: NotificationManager): Boolean {
        if (Build.VERSION.SDK_INT >= 33 &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            return false
        }
        return manager.areNotificationsEnabled()
    }

    private fun fullScreenAllowed(manager: NotificationManager): Boolean =
        Build.VERSION.SDK_INT < 34 || manager.canUseFullScreenIntent()

    private fun activityIntent(context: Context, action: String, callId: String, code: Int): PendingIntent {
        val intent = Intent(context, MainActivity::class.java).apply {
            this.action = action
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP
            putExtra(EXTRA_CALL, callId)
        }
        IntentTokens.stamp(context, intent)
        return PendingIntent.getActivity(context, code, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    }

    private fun declineIntent(context: Context, callId: String): PendingIntent {
        val intent = Intent(context, CallActionReceiver::class.java).apply {
            action = ACTION_DECLINE
            putExtra(EXTRA_CALL, callId)
        }
        IntentTokens.stamp(context, intent)
        return PendingIntent.getBroadcast(context, NOTIFICATION_ID + 2, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    }

    fun post(context: Context, invite: DirectCallInvite) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        if (!allowed(context, manager)) return
        ensureChannel(context)
        val show = activityIntent(context, ACTION_SHOW, invite.callId, NOTIFICATION_ID)
        val accept = activityIntent(context, ACTION_ACCEPT, invite.callId, NOTIFICATION_ID + 1)
        val decline = declineIntent(context, invite.callId)
        val fullScreen = fullScreenAllowed(manager)
        val caller = "@${invite.partner}"
        val kind = if (invite.media == DirectCallKind.VIDEO) "Incoming video call" else "Incoming voice call"
        fun base(): Notification.Builder = Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(caller)
            .setContentText(kind)
            .setCategory(Notification.CATEGORY_CALL)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setOngoing(true)
            .setAutoCancel(false)
            .setShowWhen(false)
            .setContentIntent(show)
            .apply { if (fullScreen) setFullScreenIntent(show, true) }
        if (fullScreen && Build.VERSION.SDK_INT >= 31) {
            val person = Person.Builder().setName(caller).setImportant(true).build()
            val styled = runCatching {
                manager.notify(
                    NOTIFICATION_ID,
                    base().setStyle(Notification.CallStyle.forIncomingCall(person, decline, accept)).build(),
                )
            }
            if (styled.isSuccess) return
        }
        val plain = base()
            .addAction(Notification.Action.Builder(null, "Decline", decline).build())
            .addAction(Notification.Action.Builder(null, "Accept", accept).build())
            .build()
        runCatching { manager.notify(NOTIFICATION_ID, plain) }
    }

    fun cancel(context: Context) {
        context.getSystemService(NotificationManager::class.java)?.cancel(NOTIFICATION_ID)
    }
}

class CallActionReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != IncomingCallNotifier.ACTION_DECLINE) return
        if (!IntentTokens.carries(context, intent)) return
        IncomingCallNotifier.cancel(context)
        DirectCallFeature.decline(intent.getStringExtra(IncomingCallNotifier.EXTRA_CALL).orEmpty())
    }
}
