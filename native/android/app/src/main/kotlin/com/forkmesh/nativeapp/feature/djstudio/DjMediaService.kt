package com.forkmesh.nativeapp.feature.djstudio

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.net.Uri
import android.os.Build
import android.os.IBinder
import com.forkmesh.nativeapp.R

class DjMediaService : Service() {
    private var session: MediaSession? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        session = MediaSession(this, "DjStudio").apply {
            setCallback(object : MediaSession.Callback() {
                override fun onPlay() = DjStudioHub.current()?.resumeAll() ?: Unit
                override fun onPause() = DjStudioHub.current()?.pauseAll() ?: Unit
                override fun onStop() = DjStudioHub.current()?.stopTheMusic() ?: Unit
            })
            isActive = true
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_TOGGLE -> DjStudioHub.current()?.let { if (it.anyPlaying) it.pauseAll() else it.resumeAll() }
            ACTION_STOP -> {
                DjStudioHub.current()?.stopTheMusic()
                stopSelf()
                return START_NOT_STICKY
            }
        }
        promote()
        return START_NOT_STICKY
    }

    override fun onTimeout(startId: Int, fgsType: Int) {
        DjStudioHub.current()?.pauseAll()
        stopSelf()
    }

    override fun onDestroy() {
        instance = null
        session?.release()
        session = null
        super.onDestroy()
    }

    private fun promote() {
        val notification = build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    fun refresh() {
        getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION_ID, build())
    }

    private fun build(): Notification {
        val model = DjStudioHub.current()
        val playing = model?.anyPlaying == true
        val title = model?.nowPlayingTitle()?.ifEmpty { null } ?: "Video Mix DJ Studio"
        val detail = model?.nowPlayingDetail().orEmpty()
        session?.setMetadata(
            MediaMetadata.Builder().putString(MediaMetadata.METADATA_KEY_TITLE, title).putString(MediaMetadata.METADATA_KEY_ARTIST, detail).build(),
        )
        session?.setPlaybackState(
            PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or PlaybackState.ACTION_STOP or PlaybackState.ACTION_PLAY_PAUSE)
                .setState(if (playing) PlaybackState.STATE_PLAYING else PlaybackState.STATE_PAUSED, PlaybackState.PLAYBACK_POSITION_UNKNOWN, 1f)
                .build(),
        )
        val manager = getSystemService(NotificationManager::class.java)
        if (manager?.getNotificationChannel(CHANNEL) == null) {
            manager?.createNotificationChannel(NotificationChannel(CHANNEL, "DJ Studio", NotificationManager.IMPORTANCE_LOW).apply { setShowBadge(false) })
        }
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(Intent.ACTION_VIEW, Uri.parse("forkmesh://video-mix-dj-studio")).setPackage(packageName),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val toggle = PendingIntent.getService(this, 1, Intent(this, DjMediaService::class.java).setAction(ACTION_TOGGLE), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 2, Intent(this, DjMediaService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        val icon = Icon.createWithResource(this, R.drawable.ic_stat_ping)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(icon)
            .setContentTitle(title)
            .setContentText(detail)
            .setContentIntent(open)
            .setOngoing(playing)
            .setOnlyAlertOnce(true)
            .setCategory(Notification.CATEGORY_TRANSPORT)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .addAction(Notification.Action.Builder(icon, if (playing) "Pause" else "Play", toggle).build())
            .addAction(Notification.Action.Builder(icon, "Stop the Music", stop).build())
            .setStyle(Notification.MediaStyle().setMediaSession(session?.sessionToken).setShowActionsInCompactView(0, 1))
            .build()
    }

    companion object {
        private const val CHANNEL = "dj-studio"
        private const val NOTIFICATION_ID = 4045
        private const val ACTION_TOGGLE = "com.forkmesh.nativeapp.dj.TOGGLE"
        private const val ACTION_STOP = "com.forkmesh.nativeapp.dj.STOP"

        @Volatile private var instance: DjMediaService? = null

        fun update(context: Context, playing: Boolean) {
            val running = instance
            if (running != null) {
                running.refresh()
                if (!playing) running.stopForeground(STOP_FOREGROUND_DETACH)
                else running.promote()
                return
            }
            if (!playing) return
            runCatching { context.startForegroundService(Intent(context, DjMediaService::class.java)) }
        }

        fun end(context: Context) {
            instance?.let {
                it.stopForeground(STOP_FOREGROUND_REMOVE)
                it.stopSelf()
            } ?: runCatching { context.stopService(Intent(context, DjMediaService::class.java)) }
        }
    }
}
