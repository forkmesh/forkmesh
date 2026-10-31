package com.forkmesh.nativeapp.feature.health

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.health.HealthPrefs
import com.forkmesh.nativeapp.health.StepsText
import java.time.Instant
import java.util.Locale

data class WalkLiveState(val startedAt: Instant, val steps: Int, val todaySteps: Int, val distanceM: Double)

class WalkLiveService : Service() {
    companion object {
        private const val ACTION_SHOW = "com.forkmesh.walk.SHOW"
        private const val ACTION_STOP = "com.forkmesh.walk.STOP"
        private const val CHANNEL = "walk_live"
        private const val NOTIFICATION_ID = 7480

        @Volatile
        private var current: WalkLiveState? = null

        @Volatile
        private var running = false

        private val context: Context get() = App.instance

        fun show(state: WalkLiveState) {
            current = state
            if (running) {
                context.getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION_ID, notification(context, state))
                return
            }
            runCatching { context.startForegroundService(Intent(context, WalkLiveService::class.java).setAction(ACTION_SHOW)) }
        }

        fun hide() {
            current = null
            if (!running) return
            runCatching { context.startService(Intent(context, WalkLiveService::class.java).setAction(ACTION_STOP)) }
        }

        private fun ensureChannel(context: Context) {
            val manager = context.getSystemService(NotificationManager::class.java) ?: return
            if (manager.getNotificationChannel(CHANNEL) != null) return
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL, "Walks", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "Shows the walk under way, its time, steps and distance."
                    setShowBadge(false)
                },
            )
        }

        private fun notification(context: Context, state: WalkLiveState): Notification {
            ensureChannel(context)
            val imperial = HealthPrefs.bool(StepsSensors.prefs, HealthPrefs.STEPS_IMPERIAL, Locale.getDefault().country == "US")
            val open = context.packageManager.getLaunchIntentForPackage(context.packageName)?.let {
                PendingIntent.getActivity(context, 4, it, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            }
            return Notification.Builder(context, CHANNEL)
                .setSmallIcon(R.drawable.ic_stat_ping)
                .setContentTitle("Walking · ${StepsText.count(state.steps)} steps")
                .setContentText("${StepsText.distance(state.distanceM, imperial)} · ${StepsText.count(state.todaySteps)} today")
                .setWhen(state.startedAt.toEpochMilli())
                .setShowWhen(true)
                .setUsesChronometer(true)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_WORKOUT)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .apply { open?.let(::setContentIntent) }
                .build()
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            shutDown()
            return START_NOT_STICKY
        }
        val state = current
        val promoted = runCatching { promote(state ?: WalkLiveState(Instant.now(), 0, 0, 0.0)) }.isSuccess
        if (!promoted || state == null) {
            shutDown()
            return START_NOT_STICKY
        }
        running = true
        return START_NOT_STICKY
    }

    private fun promote(state: WalkLiveState) {
        val built = notification(this, state)
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, built, ServiceInfo.FOREGROUND_SERVICE_TYPE_HEALTH)
        } else {
            startForeground(NOTIFICATION_ID, built)
        }
    }

    private fun shutDown() {
        running = false
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        running = false
        super.onDestroy()
    }
}
