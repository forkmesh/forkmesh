package com.forkmesh.nativeapp.feature.reticulum

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.net.Uri
import com.forkmesh.nativeapp.MainActivity
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.feature.push.PingNotifier

object ReticulumNotifier {
    private const val CHANNEL = "reticulum-messages"
    private const val TAG = "reticulum"
    private const val NOTIFICATION_ID = 7310

    fun post(context: Context, sender: String, text: String) {
        if (!PingNotifier.allowed(context)) return
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(CHANNEL) == null) {
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL, "Reticulum messages", NotificationManager.IMPORTANCE_HIGH).apply {
                    description = "Messages that reach this phone over the Reticulum mesh"
                },
            )
        }
        val open = Intent(Intent.ACTION_VIEW, Uri.parse("forkmesh://reticulum"), context, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        val intent = PendingIntent.getActivity(context, NOTIFICATION_ID, open, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val hidden = Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle("Reticulum")
            .setContentText("New message")
            .build()
        val notification = Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle("Reticulum message from $sender")
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setCategory(Notification.CATEGORY_MESSAGE)
            .setAutoCancel(true)
            .setVisibility(Notification.VISIBILITY_PRIVATE)
            .setPublicVersion(hidden)
            .setContentIntent(intent)
            .build()
        manager.notify(TAG, NOTIFICATION_ID, notification)
    }

    fun cancel(context: Context) {
        context.getSystemService(NotificationManager::class.java)?.cancel(TAG, NOTIFICATION_ID)
    }
}
