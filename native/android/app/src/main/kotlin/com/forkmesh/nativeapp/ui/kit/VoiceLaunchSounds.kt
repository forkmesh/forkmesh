package com.forkmesh.nativeapp.ui.kit

import android.media.AudioAttributes
import android.media.SoundPool
import android.os.Handler
import android.os.Looper
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.agentwatch.VoiceLaunchChime
import com.forkmesh.nativeapp.ui.SoundCueSynth
import java.io.File

object VoiceLaunchSounds {
    private val main = Handler(Looper.getMainLooper())
    private val ids = HashMap<VoiceLaunchChime, Int>()
    private val ready = HashSet<Int>()
    private var pool: SoundPool? = null
    private var loading = false

    fun play(chime: VoiceLaunchChime) {
        if (!FmSoundCues.enabled) return
        val sounds = pool
        if (sounds == null) {
            load()
            return
        }
        val id = ids[chime] ?: return
        if (id in ready) sounds.play(id, VOLUME, VOLUME, 1, 0, 1f)
    }

    fun warmUp() {
        if (FmSoundCues.enabled) load()
    }

    private fun load() {
        if (pool != null || loading) return
        val context = runCatching { App.instance.applicationContext }.getOrNull() ?: return
        loading = true
        Thread({
            val dir = File(context.cacheDir, "sound-cues")
            runCatching { dir.mkdirs() }
            val files = runCatching {
                VoiceLaunchChime.entries.associateWith { chime ->
                    val wav = SoundCueSynth.wav(chime.render())
                    File(dir, "voice-launch-${chime.slug}.wav").also { file ->
                        if (!file.exists() || !file.readBytes().contentEquals(wav)) file.writeBytes(wav)
                    }
                }
            }.getOrNull()
            main.post { install(files) }
        }, "VoiceLaunchRender").apply {
            isDaemon = true
            start()
        }
    }

    private fun install(files: Map<VoiceLaunchChime, File>?) {
        loading = false
        if (files == null) return
        val sounds = SoundPool.Builder()
            .setMaxStreams(2)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build(),
            )
            .build()
        sounds.setOnLoadCompleteListener { _, id, status ->
            if (status == 0) main.post { ready += id }
        }
        files.forEach { (chime, file) -> ids[chime] = sounds.load(file.path, 1) }
        pool = sounds
    }

    private const val VOLUME = 0.85f
}
