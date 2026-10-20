package com.forkmesh.nativeapp.feature.glasses

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.glasses.GlassesSighting
import com.forkmesh.nativeapp.glasses.GlassesSignatures
import com.forkmesh.nativeapp.platform.OtherAppCall

internal class GlassesAlarm private constructor(private val context: Context) {
    companion object {
        const val CHANNEL = "glasses_alarm"
        const val NOTIFICATION_ID = 7471
        const val RING_MS = 30_000L
        private val PATTERN = longArrayOf(0, 900, 250, 900, 250, 900, 600)
        private val AMPLITUDES = intArrayOf(0, 255, 0, 255, 0, 255, 0)

        fun ensureChannel(context: Context) {
            val manager = context.getSystemService(NotificationManager::class.java) ?: return
            if (manager.getNotificationChannel(CHANNEL) != null) return
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL, "Smart glasses nearby", NotificationManager.IMPORTANCE_HIGH).apply {
                    description = "Rings like an alarm and vibrates when Glasses Alert finds Meta smart glasses near you."
                    setSound(null, null)
                    enableVibration(false)
                    setBypassDnd(true)
                    lockscreenVisibility = Notification.VISIBILITY_PUBLIC
                },
            )
        }

        @Volatile private var shared: GlassesAlarm? = null

        fun get(context: Context): GlassesAlarm =
            shared ?: synchronized(this) { shared ?: GlassesAlarm(context.applicationContext).also { shared = it } }

        fun hushIfRinging() {
            shared?.hush()
        }

        fun canBreakTotalSilence(context: Context): Boolean =
            context.getSystemService(NotificationManager::class.java)?.isNotificationPolicyAccessGranted == true
    }

    private val main = Handler(Looper.getMainLooper())
    private val audio = context.getSystemService(AudioManager::class.java)
    private val notifications = context.getSystemService(NotificationManager::class.java)
    private val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
        context.getSystemService(VibratorManager::class.java)?.defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
    }
    private var ringtone: Ringtone? = null
    private var savedAlarmVolume: Int? = null
    private var savedFilter: Int? = null
    private val stopper = Runnable { stop() }

    @Volatile var ringing: Boolean = false
        private set

    fun ring(sighting: GlassesSighting) {
        ensureChannel(context)
        notifications?.notify(NOTIFICATION_ID, notification(sighting))
        main.removeCallbacks(stopper)
        if (!ringing && !OtherAppCall.inProgress) {
            liftTotalSilence()
            raiseAlarmVolume()
            playSound()
        }
        vibrate()
        ringing = true
        GlassesWatchService.publishRinging(true)
        main.postDelayed(stopper, RING_MS)
    }

    fun hush() {
        main.post {
            if (ringtone == null) return@post
            runCatching { ringtone?.stop() }
            ringtone = null
            savedAlarmVolume?.let { volume -> runCatching { audio?.setStreamVolume(AudioManager.STREAM_ALARM, volume, 0) } }
            savedAlarmVolume = null
        }
    }

    fun stop() {
        main.removeCallbacks(stopper)
        runCatching { ringtone?.stop() }
        ringtone = null
        runCatching { vibrator?.cancel() }
        savedAlarmVolume?.let { volume -> runCatching { audio?.setStreamVolume(AudioManager.STREAM_ALARM, volume, 0) } }
        savedAlarmVolume = null
        savedFilter?.let { filter -> runCatching { notifications?.setInterruptionFilter(filter) } }
        savedFilter = null
        ringing = false
        GlassesWatchService.publishRinging(false)
    }

    private fun liftTotalSilence() {
        val manager = notifications ?: return
        if (!manager.isNotificationPolicyAccessGranted) return
        val current = manager.currentInterruptionFilter
        if (current != NotificationManager.INTERRUPTION_FILTER_NONE) return
        if (runCatching { manager.setInterruptionFilter(NotificationManager.INTERRUPTION_FILTER_ALARMS) }.isSuccess) {
            savedFilter = current
        }
    }

    private fun raiseAlarmVolume() {
        val manager = audio ?: return
        val current = manager.getStreamVolume(AudioManager.STREAM_ALARM)
        val max = manager.getStreamMaxVolume(AudioManager.STREAM_ALARM)
        if (current >= max) return
        if (runCatching { manager.setStreamVolume(AudioManager.STREAM_ALARM, max, 0) }.isSuccess) savedAlarmVolume = current
    }

    private fun playSound() {
        val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM)
            ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
            ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
            ?: return
        val tone = RingtoneManager.getRingtone(context, uri) ?: return
        tone.audioAttributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_ALARM)
            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
            .build()
        if (Build.VERSION.SDK_INT >= 28) {
            tone.isLooping = true
            tone.volume = 1f
        }
        runCatching { tone.play() }
        ringtone = tone
    }

    private fun vibrate() {
        val motor = vibrator ?: return
        if (!motor.hasVibrator()) return
        val effect = if (motor.hasAmplitudeControl()) {
            VibrationEffect.createWaveform(PATTERN, AMPLITUDES, 0)
        } else {
            VibrationEffect.createWaveform(PATTERN, 0)
        }
        runCatching {
            motor.cancel()
            if (Build.VERSION.SDK_INT >= 33) {
                motor.vibrate(effect, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_ALARM))
            } else {
                @Suppress("DEPRECATION")
                motor.vibrate(effect, AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ALARM).build())
            }
        }
    }

    private fun notification(sighting: GlassesSighting): Notification {
        val open = context.packageManager.getLaunchIntentForPackage(context.packageName)?.let {
            PendingIntent.getActivity(context, 0, it, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        }
        val silence = PendingIntent.getBroadcast(
            context,
            1,
            Intent(context, GlassesSilenceReceiver::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle("Smart glasses nearby")
            .setContentText(GlassesSignatures.alertText(sighting))
            .setCategory(Notification.CATEGORY_ALARM)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setAutoCancel(true)
            .setDeleteIntent(silence)
            .apply { open?.let(::setContentIntent) }
            .addAction(Notification.Action.Builder(null, "Stop alarm", silence).build())
            .build()
    }
}

class GlassesSilenceReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        GlassesAlarm.get(context).stop()
        context.getSystemService(NotificationManager::class.java)?.cancel(GlassesAlarm.NOTIFICATION_ID)
    }
}
